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
#include "oneflow/cambricon/ep/primitive/cast.h"
#include "oneflow/cambricon/ep/mlu_stream.h"
#include "oneflow/core/common/data_type.h"
#include "oneflow/core/common/data_type.pb.h"
#include "oneflow/core/common/util.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/framework/user_op_hob.h"
#include "oneflow/core/framework/user_op_tensor.h"
#include "oneflow/core/kernel/new_kernel_util.h"
#include "oneflow/core/kernel/kernel_util.h"
#include "oneflow/cambricon/cnnl/cnnl_tensor_descriptor.h"
#include "oneflow/cambricon/cnnl/cnnl_workspace.h"

namespace oneflow {

template<typename T, typename K>
class MluNLLKernel final : public user_op::OpKernel {
 public:
  MluNLLKernel() = default;
  ~MluNLLKernel() = default;

 private:
  using user_op::OpKernel::Compute;

  // api `cnnlNlllossForward` doesn't accept w_desc==NULL and filter==NULL,
  // which doesn't match the doc, so if weight is None, the impl is tricky,
  // use workspace to save weight filled by 1.0 and set tensor desc manually
  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* input = ctx->Tensor4ArgNameAndIndex("input", 0);
    user_op::Tensor* target = ctx->Tensor4ArgNameAndIndex("target", 0);
    user_op::Tensor* output = ctx->Tensor4ArgNameAndIndex("output", 0);
    user_op::Tensor* out_weight = ctx->Tensor4ArgNameAndIndex("out_weight", 0);
    const int64_t C = input->shape_view().At(input->shape_view().NumAxes() - 1);
    const K ignore_index = static_cast<K>(ctx->Attr<int64_t>("ignore_index"));

    CnnlTensorDescriptor input_desc(input), target_desc(target), output_desc(output);
    CnnlTensorDescriptor weight_desc, out_weight_desc;

    CnnlTensorDescriptor tmp_target_desc;
    size_t tmp_target_workspace_size = target->shape_view().elem_cnt() * GetSizeOfDataType(kInt32);
    CnnlWorkspace tmp_target_cnnl_workspace(ctx->stream()->As<ep::MluStream>(),
                                            tmp_target_workspace_size);
    void* tmp_target_ptr = tmp_target_cnnl_workspace.dptr();
    if (target->data_type() == kInt32) {
      tmp_target_desc.set(target);
      OF_MLU_CHECK(cnrtMemcpyAsync(tmp_target_ptr, target->mut_dptr(),
                                   target->shape_view().elem_cnt() * GetSizeOfDataType(kInt32),
                                   ctx->stream()->As<ep::MluStream>()->mlu_stream(),
                                   cnrtMemcpyDevToDev));
    } else {
      cnnlCastDataType_t type = ep::primitive::GetCnnlCastType(kInt64, kInt32);
      tmp_target_desc.set(target, ConvertToCnnlDataType(kInt32));
      OF_CNNL_CHECK(cnnlCastDataType(ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
                                     target_desc.desc(), target->dptr(), type,
                                     tmp_target_desc.desc(), tmp_target_ptr));
    }

    int64_t out_weight_size[1] = {1};
    out_weight_desc.set(1, out_weight_size, ConvertToCnnlDataType(out_weight->data_type()));

    const void* weight_dptr = nullptr;
    CnnlWorkspace cnnl_workspace_for_weight(ctx->stream()->As<ep::MluStream>(), 0);
    if (ctx->has_input("weight", 0)) {
      const user_op::Tensor* weight = ctx->Tensor4ArgNameAndIndex("weight", 0);
      weight_desc.set(weight);
      weight_dptr = weight->dptr();
    } else {
      // for input without weight cases
      size_t workspace_size_for_weight = sizeof(T) * C;
      cnnl_workspace_for_weight.resize(workspace_size_for_weight);
      const int dim_size[] = {static_cast<int>(C)};
      const int stride_size[] = {1};
      weight_desc.set(1, dim_size, stride_size, ConvertToCnnlDataType(out_weight->data_type()));

      T value = static_cast<T>(1.0f);
      OF_CNNL_CHECK(cnnlFill_v3(
          /* handle       */ ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
          /* pointer_mode */ CNNL_POINTER_MODE_HOST,
          /* value        */ &value,
          /* output_desc  */ weight_desc.desc(),
          /* output       */ cnnl_workspace_for_weight.dptr()));
      weight_dptr = cnnl_workspace_for_weight.dptr();
    }

    size_t workspace_size = 0;
    OF_CNNL_CHECK(cnnlGetNlllossWorkspaceSize(ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
                                              input_desc.desc(), &workspace_size));
    CnnlWorkspace cnnl_workspace(ctx->stream()->As<ep::MluStream>(),
                                 workspace_size + sizeof(int64_t));
    void* workspace = cnnl_workspace.dptr();

    OF_CNNL_CHECK(cnnlNlllossForward(
        /* handle         */ ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
        /* algorithm      */ CNNL_REDUCTION_NONE,
        /* workspace      */ workspace,
        /* workspace_size */ workspace_size,
        /* x_desc         */ input_desc.desc(),
        /* x              */ input->dptr(),
        /* t_desc         */ tmp_target_desc.desc(),
        /* target         */ tmp_target_ptr,
        /* ignore_index   */ ignore_index,
        /* w_desc         */ weight_desc.desc(),
        /* filter         */ weight_dptr,
        /* tf_desc        */ out_weight_desc.desc(),
        /* total_filter   */ out_weight->mut_dptr(),
        /* y_desc         */ output_desc.desc(),
        /* y              */ output->mut_dptr()));

    // execute cnnlNlllossForward again to compute out_weight.
    {
      void* mean_y = static_cast<char*>(workspace) + workspace_size;

      OF_CNNL_CHECK(cnnlNlllossForward(
          /* handle         */ ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
          /* algorithm      */ CNNL_REDUCTION_MEAN,
          /* workspace      */ workspace,
          /* workspace_size */ workspace_size,
          /* x_desc         */ input_desc.desc(),
          /* x              */ input->dptr(),
          /* t_desc         */ tmp_target_desc.desc(),
          /* target         */ tmp_target_ptr,
          /* ignore_index   */ ignore_index,
          /* w_desc         */ weight_desc.desc(),
          /* filter         */ weight_dptr,
          /* tf_desc        */ out_weight_desc.desc(),
          /* total_filter   */ out_weight->mut_dptr(),
          /* y_desc         */ out_weight_desc.desc(),
          /* y              */ mean_y));

      // out_weight[i] = none_loss[i] / mean_loss
      OF_CNNL_CHECK(cnnlGetDivWorkspaceSize(ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
                                            output_desc.desc(), out_weight_desc.desc(),
                                            output_desc.desc(), &workspace_size));
      CnnlWorkspace div_workspace(ctx->stream()->As<ep::MluStream>(), workspace_size);
      OF_CNNL_CHECK(cnnlDiv_v2(ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
                               CNNL_COMPUTATION_HIGH_PRECISION, output_desc.desc(), output->dptr(),
                               out_weight_desc.desc(), mean_y, div_workspace.dptr(), workspace_size,
                               output_desc.desc(), out_weight->mut_dptr()));
    }
  }

  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

#define REGISTER_NLL_MLU_KERNEL(input_dtype, target_dtype)                         \
  REGISTER_USER_KERNEL("nll")                                                      \
      .SetCreateFn<MluNLLKernel<input_dtype, target_dtype>>()                      \
      .SetIsMatchedHob(                                                            \
          (user_op::HobDeviceType() == DeviceType::kMLU)                           \
          && (user_op::HobDataType("input", 0) == GetDataType<input_dtype>::value) \
          && (user_op::HobDataType("target", 0) == GetDataType<target_dtype>::value));

// target only supports int32
REGISTER_NLL_MLU_KERNEL(float, int32_t)
REGISTER_NLL_MLU_KERNEL(float16, int32_t)
REGISTER_NLL_MLU_KERNEL(float, int64_t)
REGISTER_NLL_MLU_KERNEL(float16, int64_t)

template<typename T, typename K>
class MluNLLGradKernel final : public user_op::OpKernel {
 public:
  MluNLLGradKernel() = default;
  ~MluNLLGradKernel() = default;

 private:
  using user_op::OpKernel::Compute;

  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* input = ctx->Tensor4ArgNameAndIndex("input", 0);
    user_op::Tensor* target = ctx->Tensor4ArgNameAndIndex("target", 0);
    const user_op::Tensor* out_grad = ctx->Tensor4ArgNameAndIndex("out_grad", 0);
    user_op::Tensor* in_grad = ctx->Tensor4ArgNameAndIndex("in_grad", 0);
    const K ignore_index = static_cast<K>(ctx->Attr<int64_t>("ignore_index"));

    CnnlTensorDescriptor input_desc(input), target_desc(target), out_grad_desc(out_grad),
        in_grad_desc(in_grad);
    CnnlTensorDescriptor weight_desc;

    CnnlTensorDescriptor tmp_target_desc;
    size_t tmp_target_workspace_size = target->shape_view().elem_cnt() * GetSizeOfDataType(kInt32);
    CnnlWorkspace tmp_target_cnnl_workspace(ctx->stream()->As<ep::MluStream>(),
                                            tmp_target_workspace_size);
    void* tmp_target_ptr = tmp_target_cnnl_workspace.dptr();
    if (target->data_type() == kInt32) {
      tmp_target_desc.set(target);
      OF_MLU_CHECK(cnrtMemcpyAsync(tmp_target_ptr, target->mut_dptr(),
                                   target->shape_view().elem_cnt() * GetSizeOfDataType(kInt32),
                                   ctx->stream()->As<ep::MluStream>()->mlu_stream(),
                                   cnrtMemcpyDevToDev));
    } else {
      cnnlCastDataType_t type = ep::primitive::GetCnnlCastType(kInt64, kInt32);
      tmp_target_desc.set(target, ConvertToCnnlDataType(kInt32));
      OF_CNNL_CHECK(cnnlCastDataType(ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
                                     target_desc.desc(), target->dptr(), type,
                                     tmp_target_desc.desc(), tmp_target_ptr));
    }

    const void* weight_dptr = nullptr;
    if (ctx->has_input("weight", 0)) {
      const user_op::Tensor* weight = ctx->Tensor4ArgNameAndIndex("weight", 0);
      weight_desc.set(weight);
      weight_dptr = weight->dptr();
    }

    OF_CNNL_CHECK(cnnlNlllossBackward(
        /* handle       */ ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
        /* algorithm    */ CNNL_REDUCTION_NONE,
        /* diff_y_desc  */ out_grad_desc.desc(),
        /* diff_y       */ out_grad->dptr(),
        /* t_desc       */ tmp_target_desc.desc(),
        /* target       */ tmp_target_ptr,
        /* ignore_index */ ignore_index,
        /* w_desc       */ weight_desc.desc(),
        /* filter       */ weight_dptr,
        /* tw_desc      */ nullptr,
        /* total_filter */ nullptr,
        /* diff_x_desc  */ in_grad_desc.desc(),
        /* diff_x       */ in_grad->mut_dptr()));
  }

  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

#define REGISTER_NLL_GRAD_MLU_KERNEL(input_dtype, target_dtype)                    \
  REGISTER_USER_KERNEL("nll_grad")                                                 \
      .SetCreateFn<MluNLLGradKernel<input_dtype, target_dtype>>()                  \
      .SetIsMatchedHob(                                                            \
          (user_op::HobDeviceType() == DeviceType::kMLU)                           \
          && (user_op::HobDataType("input", 0) == GetDataType<input_dtype>::value) \
          && (user_op::HobDataType("target", 0) == GetDataType<target_dtype>::value));

// target only supports int32
REGISTER_NLL_GRAD_MLU_KERNEL(float, int32_t)
REGISTER_NLL_GRAD_MLU_KERNEL(float16, int32_t)
REGISTER_NLL_GRAD_MLU_KERNEL(float, int64_t)
REGISTER_NLL_GRAD_MLU_KERNEL(float16, int64_t)

}  // namespace oneflow
