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
#include "oneflow/cambricon/common/mlu_util.h"
#include "oneflow/cambricon/cnnl/cnnl_op_descriptor.h"
#include "oneflow/cambricon/cnnl/cnnl_tensor_descriptor.h"
#include "oneflow/cambricon/ep/mlu_stream.h"
#include "oneflow/core/common/data_type.h"
#include "oneflow/core/common/util.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/kernel/new_kernel_util.h"

namespace oneflow {

class MluActivationKernel final : public user_op::OpKernel {
 public:
  MluActivationKernel() = default;
  ~MluActivationKernel() = default;

  using ComputeImpl = std::function<void(user_op::KernelComputeContext* ctx)>;
  explicit MluActivationKernel(ComputeImpl compute_impl) : compute_impl_(compute_impl) {}

 private:
  using user_op::OpKernel::Compute;

  void Compute(user_op::KernelComputeContext* ctx) const override { compute_impl_(ctx); }

  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }

 private:
  ComputeImpl compute_impl_;
};

void CnnlActivationForward(user_op::KernelComputeContext* ctx, const user_op::Tensor* in,
                           user_op::Tensor* out, const CnnlActivationDescriptor& activation_desc) {
  CnnlTensorDescriptor input_desc, output_desc;
  input_desc.set(in);
  output_desc.set(out);
  OF_CNNL_CHECK(cnnlActivationForward(ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
                                      activation_desc.desc(), nullptr, input_desc.desc(),
                                      in->dptr(), nullptr, output_desc.desc(), out->mut_dptr()));
}

inline auto BaseActivationIsMatched(const std::string& input_name) {
  return (user_op::HobDeviceType() == DeviceType::kMLU)
         && ((user_op::HobDataType(input_name, 0) == kFloat)
             || (user_op::HobDataType(input_name, 0) == kFloat16));
}

REGISTER_USER_KERNEL("relu")
    .SetCreateFn([]() {
      return user_op::NewOpKernel<MluActivationKernel>([](user_op::KernelComputeContext* ctx) {
        const user_op::Tensor* in = ctx->Tensor4ArgNameAndIndex("x", 0);
        user_op::Tensor* out = ctx->Tensor4ArgNameAndIndex("y", 0);
        CnnlActivationDescriptor activation_desc;
        activation_desc.set(CNNL_ACTIVATION_RELU, /*prefer=*/CNNL_ACTIVATION_HIGH_PRECISION,
                            /*nanProp=*/CNNL_NOT_PROPAGATE_NAN, /*ceof=*/1.0);
        CnnlActivationForward(ctx, in, out, activation_desc);
      });
    })
    .SetIsMatchedHob(BaseActivationIsMatched("x"));

REGISTER_USER_KERNEL("gelu")
    .SetCreateFn([]() {
      return user_op::NewOpKernel<MluActivationKernel>([](user_op::KernelComputeContext* ctx) {
        const user_op::Tensor* in = ctx->Tensor4ArgNameAndIndex("in", 0);
        user_op::Tensor* out = ctx->Tensor4ArgNameAndIndex("out", 0);
        CnnlActivationDescriptor activation_desc;
        activation_desc.set(CNNL_ACTIVATION_GELU, /*prefer=*/CNNL_ACTIVATION_HIGH_PRECISION,
                            /*nanProp=*/CNNL_NOT_PROPAGATE_NAN, /*ceof=*/1.0);
        CnnlActivationForward(ctx, in, out, activation_desc);
      });
    })
    .SetIsMatchedHob(BaseActivationIsMatched("in"));

// TODO: x/y 适配不同的 mode
static void LaunchActivationGradKernelMlu(cnnlHandle_t handle, cnnlActivationMode_t activation_mode,
                                          const user_op::Tensor* input_a_tensor,
                                          const user_op::Tensor* input_b_tensor,
                                          user_op::Tensor* output_tensor) {
  CnnlTensorDescriptor diff_y_desc;
  diff_y_desc.set(input_a_tensor);
  CnnlTensorDescriptor x_desc;
  x_desc.set(input_b_tensor);
  CnnlTensorDescriptor diff_x_desc;
  diff_x_desc.set(output_tensor);
  CnnlActivationDescriptor activation_desc;
  activation_desc.set(activation_mode, CNNL_ACTIVATION_HIGH_PRECISION, CNNL_NOT_PROPAGATE_NAN,
                      /*coef*/ 0.0);
  OF_CNNL_CHECK(
      cnnlActivationBackward(handle, activation_desc.desc(),
                             /*alpha*/ nullptr,
                             /*y_desc*/ nullptr,
                             /*y*/ nullptr, diff_y_desc.desc(),
                             /*diff_y*/ input_a_tensor->dptr(), x_desc.desc(),
                             /*x. when op=relu_grad, replace x with y*/ input_b_tensor->dptr(),
                             /*beta*/ nullptr, diff_x_desc.desc(),
                             /*diff_x*/ output_tensor->mut_dptr()));
}

template<typename T>
class ActivationGradKernelMlu final : public user_op::OpKernel {
 public:
  OF_DISALLOW_COPY_AND_MOVE(ActivationGradKernelMlu);
  ActivationGradKernelMlu() = default;
  ~ActivationGradKernelMlu() = default;
  ActivationGradKernelMlu(const std::string& output_name, const std::string& input_a_name,
                          const std::string& input_b_name, cnnlActivationMode_t activation_mode)
      : output_name_(output_name),
        input_a_name_(input_a_name),
        input_b_name_(input_b_name),
        activation_mode_(activation_mode) {}

 private:
  using user_op::OpKernel::Compute;
  void Compute(user_op::KernelComputeContext* ctx) const override {
    // The meanings of input_a_name and input_b_name are determined by kernel registration, in the
    // order of output_name, input_a_name, input_b_name.
    const user_op::Tensor* input_a_tensor = ctx->Tensor4ArgNameAndIndex(input_a_name_, 0);
    const user_op::Tensor* input_b_tensor = ctx->Tensor4ArgNameAndIndex(input_b_name_, 0);
    user_op::Tensor* output_tensor = ctx->Tensor4ArgNameAndIndex(output_name_, 0);
    const ShapeView& input_a_shape = input_a_tensor->shape_view();
    const ShapeView& input_b_shape = input_b_tensor->shape_view();
    const ShapeView& output_shape = output_tensor->shape_view();
    CHECK_EQ(input_a_shape, input_b_shape) << "InputA shape should be equal to InputB shape.";
    CHECK_EQ(input_a_shape, output_shape) << "Input shape should be equal to Output shape.";
    if (input_a_shape.elem_cnt() == 0) { return; }
    LaunchActivationGradKernelMlu(ctx->stream()->As<ep::MluStream>()->cnnl_handle(),
                                  activation_mode_, input_a_tensor, input_b_tensor, output_tensor);
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }

  std::string output_name_;
  std::string input_a_name_;
  std::string input_b_name_;
  cnnlActivationMode_t activation_mode_;
};

inline auto ActivationGradIsMatched(const std::string& name1, const std::string& name2,
                                    const std::string& name3, DataType dtype) {
  return (user_op::HobDeviceType() == DeviceType::kMLU) && (user_op::HobDataType(name1, 0) == dtype)
         && (user_op::HobDataType(name2, 0) == dtype) && (user_op::HobDataType(name3, 0) == dtype);
}

#define REGISTER_RELU_GRAD_USER_KERNEL(data_type)                                               \
  REGISTER_USER_KERNEL("relu_grad")                                                             \
      .SetCreateFn([]() {                                                                       \
        return user_op::NewOpKernel<ActivationGradKernelMlu<data_type>>("dx", "dy", "y",        \
                                                                        CNNL_ACTIVATION_RELU);  \
      })                                                                                        \
      .SetIsMatchedHob(ActivationGradIsMatched("dx", "dy", "y", GetDataType<data_type>::value)) \
      .SetInplaceProposalFn(                                                                    \
          [](const user_op::InferContext&,                                                      \
             const user_op::AddInplaceArgPair& AddInplaceArgPairFn) -> Maybe<void> {            \
            OF_RETURN_IF_ERROR(AddInplaceArgPairFn("dx", 0, "dy", 0, true));                    \
            return Maybe<void>::Ok();                                                           \
          });
REGISTER_RELU_GRAD_USER_KERNEL(float)
REGISTER_RELU_GRAD_USER_KERNEL(float16)

#define REGISTER_GELU_GRAD_USER_KERNEL(data_type)                                              \
  REGISTER_USER_KERNEL("gelu_grad")                                                            \
      .SetCreateFn([]() {                                                                      \
        return user_op::NewOpKernel<ActivationGradKernelMlu<data_type>>("dx", "dy", "x",       \
                                                                        CNNL_ACTIVATION_GELU); \
      })                                                                                       \
      .SetIsMatchedHob(ActivationGradIsMatched("dx", "dy", "x", GetDataType<data_type>::value));
REGISTER_GELU_GRAD_USER_KERNEL(float)
REGISTER_GELU_GRAD_USER_KERNEL(float16)

}  // namespace oneflow
