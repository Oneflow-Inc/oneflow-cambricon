/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/cambricon/cnnl/cnnl_op_descriptor.h"
#include "oneflow/cambricon/cnnl/cnnl_tensor_descriptor.h"
#include "oneflow/cambricon/cnnl/cnnl_workspace.h"
#include "oneflow/cambricon/common/mlu_util.h"
#include "oneflow/cambricon/ep/mlu_stream.h"
#include "oneflow/core/common/data_type.h"
#include "oneflow/core/common/util.h"
#include "oneflow/core/ep/include/primitive/permute.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/framework/user_op_tensor.h"
#include "oneflow/core/kernel/new_kernel_util.h"
#include "oneflow/user/kernels/avg_pool_kernel_util.h"

namespace oneflow {

namespace {

template<typename Context>
std::unique_ptr<ep::primitive::Permute> NewPermutePrimitive(Context* ctx, const int& num_dims) {
  return ep::primitive::NewPrimitive<ep::primitive::PermuteFactory>(ctx->device_type(), num_dims);
}

std::vector<int32_t> ComputePermutation(int32_t ndim, cnnlTensorLayout_t layout) {
  CHECK_GT(ndim, 2);
  CHECK(layout == CNNL_LAYOUT_NHWC || layout == CNNL_LAYOUT_NCHW);
  std::vector<int32_t> permute(ndim);
  if (layout == CNNL_LAYOUT_NHWC) {
    // NCHW -> NHWC
    permute[0] = 0;
    permute[ndim - 1] = 1;
    for (int i = 0; i < ndim - 2; ++i) { permute[i + 1] = i + 2; }
  } else {
    // NHWC -> NCHW
    permute[0] = 0;
    permute[1] = ndim - 1;
    for (int i = 0; i < ndim - 2; ++i) { permute[i + 2] = i + 1; }
  }
  return permute;
}

std::vector<int64_t> ComputePermuteShape(const ShapeView& shape,
                                         const std::vector<int32_t>& permute) {
  CHECK_EQ(shape.NumAxes(), permute.size());
  std::vector<int64_t> permute_shape(shape.NumAxes());
  for (int i = 0; i < permute.size(); ++i) { permute_shape[i] = shape[permute[i]]; }
  return permute_shape;
}

std::vector<int32_t> GetCnnlPadding(int32_t ndim, const std::vector<int32_t>& padding) {
  int32_t offset = padding.size() - ndim;
  std::vector<int32_t> cnnl_padding(ndim * 2);
  for (int i = 0; i < ndim; ++i) {
    cnnl_padding[2 * i] = padding[i + offset];
    cnnl_padding[2 * i + 1] = padding[i + offset];
  }
  return cnnl_padding;
}

struct AvgPoolOpKernelCache final : public user_op::OpKernelCache {
  AvgPoolParams3D params_3d;
  explicit AvgPoolOpKernelCache(const AvgPoolParams3D& params_3d) : params_3d(params_3d) {}
  const AvgPoolParams3D& GetParams3D() const { return params_3d; }
};

std::shared_ptr<AvgPoolOpKernelCache> CreateAvgOpKernelCache(user_op::KernelCacheContext* ctx,
                                                             const int32_t& dim) {
  const Shape& x_shape = ctx->TensorDesc4ArgNameAndIndex("x", 0)->shape();
  const std::string& data_format = ctx->Attr<std::string>("data_format");
  const std::vector<int32_t>& padding = ctx->Attr<std::vector<int32_t>>("padding");
  const std::vector<int32_t>& kernel_size = ctx->Attr<std::vector<int32_t>>("kernel_size");
  const std::vector<int32_t>& stride = ctx->Attr<std::vector<int32_t>>("stride");
  const bool ceil_mode = ctx->Attr<bool>("ceil_mode");
  const bool count_include_pad = ctx->Attr<bool>("count_include_pad");
  const int32_t divisor_override = ctx->Attr<int32_t>("divisor_override");

  AvgPoolParams3D params_3d =
      AvgPoolParams3D(dim, x_shape, data_format, padding, kernel_size, stride, ceil_mode,
                      count_include_pad, divisor_override);
  std::shared_ptr<AvgPoolOpKernelCache> cache(new AvgPoolOpKernelCache(params_3d));
  return cache;
}

template<int Nd, typename T>
class MluAvgPoolKernel final : public user_op::OpKernel {
 public:
  MluAvgPoolKernel() = default;
  ~MluAvgPoolKernel() = default;

  std::shared_ptr<user_op::OpKernelCache> InitOpKernelCache(
      user_op::KernelCacheContext* ctx) const override {
    return CreateAvgOpKernelCache(ctx, Nd);
  }

 private:
  using user_op::OpKernel::Compute;

  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState*,
               const user_op::OpKernelCache* cache) const override {
    const user_op::Tensor* x = ctx->Tensor4ArgNameAndIndex("x", 0);
    user_op::Tensor* y = ctx->Tensor4ArgNameAndIndex("y", 0);

    const auto* pool_cache = dynamic_cast<const AvgPoolOpKernelCache*>(cache);
    const AvgPoolParams3D& params_3d = pool_cache->GetParams3D();
    CHECK_OR_THROW(params_3d.divisor_override() == 0)
        << "cambricon cnnl avg pool does not support divisor_override.";

    const std::vector<int32_t>& padding = GetCnnlPadding(Nd, params_3d.padding());
    const std::vector<int32_t>& kernel_size = params_3d.pool_size_3d();
    const std::vector<int32_t>& stride = params_3d.stride_3d();
    const bool ceil_mode = params_3d.ceil_mode();
    const std::string& data_format = params_3d.data_format();

    CHECK_OR_THROW(padding.size() == Nd * 2) << "padding size should be " << Nd;
    CHECK_OR_THROW(kernel_size.size() >= Nd) << "kernel_size size should be " << Nd;
    CHECK_OR_THROW(stride.size() >= Nd) << "stride size should be " << Nd;
    CHECK_OR_THROW(params_3d.divisor_override() == 0)
        << "cambricon cnnl avg pool does not support divisor_override.";

    std::vector<int32_t> dilation(Nd, 1);
    cnnlTensorLayout_t layout =
        (data_format == "channels_last") ? CNNL_LAYOUT_NHWC : CNNL_LAYOUT_NCHW;
    cnnlPoolingMode_t mode = params_3d.count_include_pad()
                                 ? CNNL_POOLING_AVERAGE_COUNT_INCLUDE_PADDING
                                 : CNNL_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING;
    CnnlPoolingDescriptor pooling_desc;
    CnnlTensorDescriptor x_desc, y_desc;
    x_desc.set(x, layout);
    y_desc.set(y, layout);

    CHECK_EQ_OR_THROW(Nd, 2) << "cnnlGetPoolingWorkspaceSize only support 2D.";
    if (Nd == 2) {
      pooling_desc.set(mode, kernel_size[1], kernel_size[2], stride[1], stride[2], padding[0],
                       padding[1], padding[2], padding[3], ceil_mode);
    } else if (Nd == 3) {
      pooling_desc.set(mode, Nd, kernel_size.data(), stride.data(), padding.data(), dilation.data(),
                       ceil_mode);
    } else {
      // TODO()
    }
    int h_axis = (data_format == "channels_last") ? 1 : 2;
    int w_axis = h_axis + 1;
    int64_t output_h = y->shape_view()[h_axis];
    int64_t output_w = y->shape_view()[w_axis];

    auto handle = ctx->stream()->As<ep::MluStream>()->cnnl_handle();
    size_t pooling_workspace_size = 0;
    OF_CNNL_CHECK(cnnlGetPoolingWorkspaceSize(
        /* handle         */ handle,
        /* mode           */ mode,
        /* output_w       */ output_w,
        /* output_h       */ output_h,
        /* workspace_size */ &pooling_workspace_size));
    CnnlWorkspace pooling_workspace(ctx->stream()->As<ep::MluStream>(), pooling_workspace_size);

    const void* extra_device_input_dptr = nullptr;
    CnnlHostWorkspace extra_input_workspace(ctx->stream()->As<ep::MluStream>());
    CnnlWorkspace extra_device_input_workspace(ctx->stream()->As<ep::MluStream>());
    size_t extra_input_size = 0;
    OF_CNNL_CHECK(
        cnnlGetPoolingExtraInputSize(handle, mode, output_w, output_h, &extra_input_size));
    if (extra_input_size > 0) {
      extra_input_workspace.resize(extra_input_size);
      OF_CNNL_CHECK(cnnlInitPoolingExtraInput(handle, pooling_desc.desc(), x_desc.desc(),
                                              y_desc.desc(), extra_input_workspace.dptr()));
      extra_device_input_workspace.resize(extra_input_size);
      OF_MLU_CHECK(cnrtMemcpyAsync(
          extra_device_input_workspace.dptr(), extra_input_workspace.dptr(), extra_input_size,
          ctx->stream()->As<ep::MluStream>()->mlu_stream(), cnrtMemcpyHostToDev));
      extra_device_input_dptr = extra_device_input_workspace.dptr();
    }

    OF_CNNL_CHECK(cnnlPoolingForward_v2(
        /* handle         */ handle,
        /* pooling_desc   */ pooling_desc.desc(),
        /* alpha          */ nullptr,
        /* x_desc         */ x_desc.desc(),
        /* x              */ x->dptr(),
        /* beta           */ nullptr,
        /* extra_input    */ extra_device_input_dptr,
        /* y_desc         */ y_desc.desc(),
        /* y              */ y->mut_dptr(),
        /* workspace      */ pooling_workspace.dptr(),
        /* workspace_size */ pooling_workspace_size));
  }

  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

#define REGISTER_MLU_AVG_POOL_KERNEL(dtype)                           \
  REGISTER_USER_KERNEL("avg_pool_2d")                                 \
      .SetCreateFn<MluAvgPoolKernel<2, dtype>>()                      \
      .SetIsMatchedHob((user_op::HobDeviceType() == DeviceType::kMLU) \
                       && (user_op::HobDataType("x", 0) == GetDataType<dtype>::value));

REGISTER_MLU_AVG_POOL_KERNEL(float)
REGISTER_MLU_AVG_POOL_KERNEL(float16)

#undef REGISTER_MLU_AVG_POOL_KERNEL

template<int Nd, typename T>
class MluAvgPoolGradKernel final : public user_op::OpKernel {
 public:
  MluAvgPoolGradKernel() = default;
  ~MluAvgPoolGradKernel() = default;

  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
  std::shared_ptr<user_op::OpKernelCache> InitOpKernelCache(
      user_op::KernelCacheContext* ctx) const override {
    return CreateAvgOpKernelCache(ctx, Nd);
  }

 private:
  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState*,
               const user_op::OpKernelCache* cache) const override {
    const user_op::Tensor* x = ctx->Tensor4ArgNameAndIndex("x", 0);
    const user_op::Tensor* dy = ctx->Tensor4ArgNameAndIndex("dy", 0);
    user_op::Tensor* dx = ctx->Tensor4ArgNameAndIndex("dx", 0);

    const auto* pool_cache = dynamic_cast<const AvgPoolOpKernelCache*>(cache);
    const AvgPoolParams3D& params_3d = pool_cache->GetParams3D();

    const std::vector<int32_t>& padding = GetCnnlPadding(Nd, params_3d.padding());
    const std::vector<int32_t>& kernel_size = params_3d.pool_size_3d();
    const std::vector<int32_t>& stride = params_3d.stride_3d();
    const bool ceil_mode = params_3d.ceil_mode();

    CHECK_OR_THROW(padding.size() == Nd * 2) << "padding size should be " << Nd;
    CHECK_OR_THROW(kernel_size.size() >= Nd) << "kernel_size size should be " << Nd;
    CHECK_OR_THROW(stride.size() >= Nd) << "stride size should be " << Nd;
    CHECK_OR_THROW(params_3d.divisor_override() == 0)
        << "cambricon cnnl avg pool does not support divisor_override.";

    std::vector<int32_t> dilation(Nd, 1);
    cnnlPoolingMode_t mode = params_3d.count_include_pad()
                                 ? CNNL_POOLING_AVERAGE_COUNT_INCLUDE_PADDING
                                 : CNNL_POOLING_AVERAGE_COUNT_EXCLUDE_PADDING;
    auto cnnl_data_type = ConvertToCnnlDataType(dx->data_type());
    cnnlTensorLayout_t layout = CNNL_LAYOUT_NHWC;
    CnnlTensorDescriptor x_desc, dy_desc, dx_desc;
    CnnlPoolingDescriptor pooling_desc;
    if (Nd == 2) {
      pooling_desc.set(mode, kernel_size[1], kernel_size[2], stride[1], stride[2], padding[0],
                       padding[1], padding[2], padding[3], ceil_mode);
    } else if (Nd == 3) {
      pooling_desc.set(mode, Nd, kernel_size.data(), stride.data(), padding.data(), dilation.data(),
                       ceil_mode);
    } else {
      // TODO()
    }
    size_t element_size = GetSizeOfDataType(dx->data_type());
    const auto& x_shape = x->shape_view();
    const auto& dy_shape = dy->shape_view();
    const auto& dx_shape = dx->shape_view();
    const void* x_ptr = x->dptr();
    const void* dy_ptr = dy->dptr();
    void* dx_ptr = dx->mut_dptr();

    CnnlWorkspace temp_x(ctx->stream()->As<ep::MluStream>(), 0);
    CnnlWorkspace temp_dy(ctx->stream()->As<ep::MluStream>(), 0);
    CnnlWorkspace temp_dx(ctx->stream()->As<ep::MluStream>(), 0);
    std::vector<int64_t> temp_dx_shape;

    if (params_3d.data_format() != "channels_last") {
      temp_x.resize(x_shape.elem_cnt() * element_size);
      temp_dy.resize(dy_shape.elem_cnt() * element_size);
      temp_dx.resize(dx_shape.elem_cnt() * element_size);
      x_ptr = temp_x.dptr();
      dy_ptr = temp_dy.dptr();
      dx_ptr = temp_dx.dptr();

      // transpose input from NCHW to NHWC
      auto permute = ComputePermutation(x_shape.NumAxes(), layout);
      {
        auto transpose = NewPermutePrimitive(ctx, x_shape.NumAxes());
        CHECK(transpose);
        transpose->Launch(ctx->stream(), x->data_type(), x_shape.NumAxes(), x_shape.data(),
                          x->dptr(), permute.data(), temp_x.dptr());
      }
      {
        auto transpose = NewPermutePrimitive(ctx, dy_shape.NumAxes());
        CHECK(transpose);
        auto permute = ComputePermutation(dy_shape.NumAxes(), layout);
        transpose->Launch(ctx->stream(), dy->data_type(), dy_shape.NumAxes(), dy_shape.data(),
                          dy->dptr(), permute.data(), temp_dy.dptr());
      }
      const auto& temp_x_shape = ComputePermuteShape(x_shape, permute);
      x_desc.set(x_shape.NumAxes(), temp_x_shape.data(), cnnl_data_type, layout);
      const auto& temp_dy_shape = ComputePermuteShape(dy_shape, permute);
      dy_desc.set(dy_shape.NumAxes(), temp_dy_shape.data(), cnnl_data_type, layout);

      temp_dx_shape = ComputePermuteShape(dx_shape, permute);
      dx_desc.set(dx_shape.NumAxes(), temp_dx_shape.data(), cnnl_data_type, layout);
    }

    auto handle = ctx->stream()->As<ep::MluStream>()->cnnl_handle();
    OF_CNNL_CHECK(cnnlPoolingBackward(handle, pooling_desc.desc(), nullptr, /*y_desc*/ nullptr,
                                      /*y*/ nullptr, dy_desc.desc(), dy_ptr, x_desc.desc(), x_ptr,
                                      nullptr, dx_desc.desc(), dx_ptr));

    if (params_3d.data_format() != "channels_last") {
      // transpose output from NHWC to NCHW
      auto permute = ComputePermutation(dx_shape.NumAxes(), CNNL_LAYOUT_NCHW);
      auto transpose = NewPermutePrimitive(ctx, dx_shape.NumAxes());
      CHECK(transpose);
      transpose->Launch(ctx->stream(), dx->data_type(), dx_shape.NumAxes(), temp_dx_shape.data(),
                        dx_ptr, permute.data(), dx->mut_dptr());
    }
  }
};

#define REGISTER_MLU_AVG_POOL_GRAD_KERNEL(dtype)                      \
  REGISTER_USER_KERNEL("avg_pool_2d_grad")                            \
      .SetCreateFn<MluAvgPoolGradKernel<2, dtype>>()                  \
      .SetIsMatchedHob((user_op::HobDeviceType() == DeviceType::kMLU) \
                       && (user_op::HobDataType("x", 0) == GetDataType<dtype>::value));

REGISTER_MLU_AVG_POOL_GRAD_KERNEL(float)
REGISTER_MLU_AVG_POOL_GRAD_KERNEL(float16)

#undef REGISTER_MLU_AVG_POOL_GRAD_KERNEL

}  // namespace

}  // namespace oneflow
