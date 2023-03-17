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
#include "oneflow/cambricon/ep/mlu_stream.h"
#include "oneflow/cambricon/common/mlu_util.h"
#include "oneflow/core/common/data_type.h"
#include "oneflow/core/common/util.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/kernel/new_kernel_util.h"
#include "oneflow/cambricon/cnnl/cnnl_tensor_descriptor.h"
#include "oneflow/cambricon/cnnl/cnnl_workspace.h"
#include "oneflow/core/ep/include/primitive/permute.h"

namespace oneflow {

template<typename Context>
std::unique_ptr<ep::primitive::Permute> NewPermutePrimitive(Context* ctx, const int& num_dims) {
  return ep::primitive::NewPrimitive<ep::primitive::PermuteFactory>(ctx->device_type(), num_dims);
}

template<typename T>
class MluNormalizationKernel final : public user_op::OpKernel {
 public:
  MluNormalizationKernel() = default;
  ~MluNormalizationKernel() = default;

 private:
  using user_op::OpKernel::Compute;

  void Compute(user_op::KernelComputeContext* ctx) const override {
    const bool training = ctx->Attr<bool>("training");
    CHECK(!training);
    const user_op::Tensor* x = ctx->Tensor4ArgNameAndIndex("x", 0);
    user_op::Tensor* y = ctx->Tensor4ArgNameAndIndex("y", 0);
    const auto* gamma = ctx->Tensor4ArgNameAndIndex("gamma", 0);
    const auto* beta = ctx->Tensor4ArgNameAndIndex("beta", 0);
    auto* moving_mean = ctx->Tensor4ArgNameAndIndex("moving_mean", 0);
    auto* moving_variance = ctx->Tensor4ArgNameAndIndex("moving_variance", 0);
    // make sure input tensor's format NCHW, so channel axis must be 1
    const auto axis = ctx->Attr<int32_t>("axis");
    CHECK_EQ(axis, 1);
    const auto epsilon = ctx->Attr<float>("epsilon");

    int n = 0, c = 0, h = 0, w = 0;
    if (x->shape_view().NumAxes() == 2) {
      n = x->shape_view().At(0);
      h = 1;
      w = 1;
      c = x->shape_view().At(1);
    } else {
      n = x->shape_view().At(0);
      c = x->shape_view().At(1);
      h = x->shape_view().At(2);
      w = x->shape_view().At(3);
    }

    size_t tmp_in_size = x->shape_view().elem_cnt() * sizeof(x->data_type());
    size_t tmp_out_size = y->shape_view().elem_cnt() * sizeof(y->data_type());
    ;
    CnnlWorkspace tmp_in_workspace(ctx->stream()->As<ep::MluStream>(), tmp_in_size);
    CnnlWorkspace tmp_out_workspace(ctx->stream()->As<ep::MluStream>(), tmp_out_size);
    void* tmp_in_dptr = tmp_in_workspace.dptr();
    void* tmp_out_dptr = tmp_out_workspace.dptr();

    std::vector<int64_t> in_shapevec({n, h, w, c});
    std::vector<int64_t> out_shapevec({n, c, h, w});
    auto transpose = NewPermutePrimitive(ctx, x->shape_view().NumAxes());
    CHECK(transpose);
    // transpose input NCHW -> NHWC
    transpose->Launch(ctx->stream(), x->data_type(), x->shape_view().NumAxes(), in_shapevec.data(),
                      x->dptr<T>(), std::vector<int>({0, 3, 1, 2}).data(), tmp_in_dptr);

    int dims[4];
    dims[0] = n;
    dims[1] = h;
    dims[2] = w;
    dims[3] = c;
    cnnlTensorDescriptor_t input_desc, output_desc, weight_bias_mean_var_desc;
    cnnlTensorLayout_t layout = CNNL_LAYOUT_NHWC;
    auto dtype = ConvertToCnnlDataType(x->data_type());
    OF_CNNL_CHECK(cnnlCreateTensorDescriptor(&input_desc));
    OF_CNNL_CHECK(cnnlCreateTensorDescriptor(&output_desc));
    OF_CNNL_CHECK(cnnlCreateTensorDescriptor(&weight_bias_mean_var_desc));
    OF_CNNL_CHECK(cnnlSetTensorDescriptor(input_desc, layout, dtype, 4, dims));
    OF_CNNL_CHECK(cnnlSetTensorDescriptor(output_desc, layout, dtype, 4, dims));
    int dim[1];
    dim[0] = c;
    OF_CNNL_CHECK(cnnlSetTensorDescriptor(weight_bias_mean_var_desc, layout, dtype, 1, dim));
    // inference
    OF_CNNL_CHECK(cnnlBatchNormForwardInference(
        ctx->stream()->As<ep::MluStream>()->cnnl_handle(), nullptr, nullptr, input_desc, x->dptr(),
        weight_bias_mean_var_desc, gamma->dptr(), beta->dptr(), moving_mean->dptr(),
        moving_variance->dptr(), epsilon, output_desc, y->mut_dptr()));
    // transpose output NHWC -> NCHW
    transpose->Launch(ctx->stream(), y->data_type(), y->shape_view().NumAxes(), out_shapevec.data(),
                      y->dptr<T>(), std::vector<int>({0, 2, 3, 1}).data(), tmp_out_dptr);
    cnnlDestroyTensorDescriptor(input_desc);
    cnnlDestroyTensorDescriptor(output_desc);
    cnnlDestroyTensorDescriptor(weight_bias_mean_var_desc);
  }

  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

#define REGISTER_BN_IINFERENCE_MLU_KERNEL(dtype)                      \
  REGISTER_USER_KERNEL("normalization")                               \
      .SetCreateFn<MluNormalizationKernel<dtype>>()                   \
      .SetIsMatchedHob((user_op::HobDeviceType() == DeviceType::kMLU) \
                       && (user_op::HobDataType("x", 0) == GetDataType<dtype>::value));

REGISTER_BN_IINFERENCE_MLU_KERNEL(float)
REGISTER_BN_IINFERENCE_MLU_KERNEL(float16)

#undef REGISTER_BN_IINFERENCE_MLU_KERNEL

}  // namespace oneflow
