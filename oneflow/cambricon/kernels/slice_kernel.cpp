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
#include "oneflow/cambricon/kernels/slice_util.h"
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/framework/nd_sbp.h"
#include "oneflow/core/job/nd_sbp_util.h"
#include "oneflow/core/kernel/kernel_util.h"
#include "oneflow/user/kernels/op_kernel_wrapper.h"

namespace oneflow {
namespace mlu {

namespace {

const int SPLIT_AXIS_FOR_NON_SPLIT = -1;

// [start, end)
int64_t GetSizeInSlice(const int64_t start, const int64_t end, const int64_t step) {
  if (end <= start) { return 0; }
  return (end - start - 1) / step + 1;
}

class SliceContext final {
 public:
  struct SplitInfo {
    // These fields shows how the logical tensor is split.
    // The logical tensor is split on the axis `split_axis`
    // The physical tensor on current device is in the range [lower, upper)
    // The length of the logical tensor on `split_axis` is `logical_length`
    // Example:
    // Variable shape = (8, 7, 6, 5), sbp = S(0), on 4 devices, then on the first card:
    // split_axis = 0
    // lower = 0
    // upper = 2
    // logical_length = 8
    const int64_t split_axis;
    const int64_t lower;
    const int64_t upper;
    const int64_t logical_length;
  };

  SliceContext() : axis_bitset_(0) {}

  Maybe<void> PushSplitInfo(int64_t split_axis, int64_t lower, int64_t upper,
                            int64_t logical_length) {
    if (split_axis != SPLIT_AXIS_FOR_NON_SPLIT) {
      // split_axis can only be push once
      CHECK_OR_RETURN(!IsAxisPushed(split_axis))
          << "split_axis " << split_axis << " has been pushed to SliceContext";
      CHECK_GE_OR_RETURN(split_axis, 0) << "split_axis should be greater than or equal to 0";

      axis_bitset_ |= ((uint32_t)1 << split_axis);  // NOLINT
    }
    split_info_vec_.emplace_back(SplitInfo{split_axis, lower, upper, logical_length});
    return Maybe<void>::Ok();
  }

  const std::vector<SplitInfo>& GetSplitInfo() const { return split_info_vec_; }

  bool IsAxisPushed(int64_t split_axis) const {
    if (split_axis == SPLIT_AXIS_FOR_NON_SPLIT) { return false; }
    CHECK_GE(split_axis, 0) << "split_axis should be greater than or equal to 0";
    return (axis_bitset_ & ((uint32_t)1 << split_axis)) != 0;  // NOLINT
  }

 private:
  std::vector<SplitInfo> split_info_vec_;
  uint32_t axis_bitset_;
};

void ConstructSliceParamsLarge(const SliceContext& ctx, const std::vector<int64_t>& start_vec,
                               const std::vector<int64_t>& stop_vec,
                               const std::vector<int64_t>& step_vec, const ShapeView& shape,
                               SliceParams* slice_param) {
  const int64_t ndim = shape.NumAxes();
  CHECK_LE(ndim, kSliceMaxDims);
  CHECK_EQ(start_vec.size(), ndim);
  CHECK_EQ(stop_vec.size(), ndim);
  CHECK_EQ(step_vec.size(), ndim);

  slice_param->ndim = ndim;
  FOR_RANGE(int, i, 0, slice_param->ndim) {
    const int64_t dim_size = shape.At(i);
    const int64_t start_in_full_large = start_vec.at(i);
    const int64_t stop_in_full_large = stop_vec.at(i);
    const int64_t step = step_vec.at(i);
    CHECK_GT(step, 0);
    int64_t start_in_splitted_large = start_in_full_large;
    int64_t stop_in_splitted_large = stop_in_full_large;
    // large tensor has split sbp attribute
    for (const auto& split_info : ctx.GetSplitInfo()) {
      if (split_info.split_axis == i) {
        if (start_in_splitted_large < split_info.lower) {
          start_in_splitted_large =
              split_info.lower
              + (step - (split_info.lower - start_in_splitted_large) % step) % step;
        }
        start_in_splitted_large =
            std::min(std::max(start_in_splitted_large, split_info.lower), split_info.upper);
        stop_in_splitted_large =
            std::min(std::max(stop_in_splitted_large, split_info.lower), split_info.upper);
        start_in_splitted_large -= split_info.lower;
        stop_in_splitted_large -= split_info.lower;
      }
    }
    const int64_t slice_size =
        GetSizeInSlice(start_in_splitted_large, stop_in_splitted_large, step);
    slice_param->dims[i] = dim_size;
    slice_param->start[i] = start_in_splitted_large;
    slice_param->step[i] = step;
    slice_param->size[i] = slice_size;
  }
}

void ConstructSliceParamsSmall(const SliceContext& ctx, const std::vector<int64_t>& start_vec,
                               const std::vector<int64_t>& stop_vec,
                               const std::vector<int64_t>& step_vec, const ShapeView& shape,
                               SliceParams* slice_param) {
  const int64_t ndim = shape.NumAxes();
  CHECK_LE(ndim, kSliceMaxDims);
  CHECK_EQ(start_vec.size(), ndim);
  CHECK_EQ(stop_vec.size(), ndim);
  CHECK_EQ(step_vec.size(), ndim);

  slice_param->ndim = ndim;
  FOR_RANGE(int, i, 0, slice_param->ndim) {
    const int64_t start_in_full_large = start_vec.at(i);
    const int64_t step = step_vec.at(i);
    CHECK_GT(step, 0);
    // small tensor has broadcast/partialsum sbp attribute
    const int64_t dim_size = shape.At(i);
    int64_t start_in_full_small = 0;
    int64_t stop_in_full_small = dim_size;
    for (const auto& split_info : ctx.GetSplitInfo()) {
      if (split_info.split_axis == i) {
        start_in_full_small = GetSizeInSlice(start_in_full_large, split_info.lower, step);
        stop_in_full_small = GetSizeInSlice(start_in_full_large, split_info.upper, step);
        start_in_full_small = std::min(std::max<int64_t>(start_in_full_small, 0), dim_size);
        stop_in_full_small = std::min(std::max<int64_t>(stop_in_full_small, 0), dim_size);
      }
    }
    const int64_t slice_size = stop_in_full_small - start_in_full_small;
    slice_param->dims[i] = dim_size;
    slice_param->start[i] = start_in_full_small;
    slice_param->step[i] = 1;
    slice_param->size[i] = slice_size;
  }
}

void WriteSlice(user_op::KernelComputeContext* ctx, const user_op::Tensor* src,
                user_op::Tensor* dst, const SliceContext& slice_ctx,
                const bool from_large_to_small) {
  const user_op::Tensor* large = from_large_to_small ? src : dst;
  const user_op::Tensor* small = from_large_to_small ? dst : src;
  // Check physical tensor's shape
  for (const auto& split_info : slice_ctx.GetSplitInfo()) {
    if (split_info.split_axis != SPLIT_AXIS_FOR_NON_SPLIT) {
      CHECK_EQ(large->shape_view().At(split_info.split_axis), split_info.upper - split_info.lower)
          << "split_info shape mismatch physical tensor shape";
    }
  }

  const std::vector<int64_t> start_attr = ctx->Attr<std::vector<int64_t>>("start");
  const std::vector<int64_t> stop_attr = ctx->Attr<std::vector<int64_t>>("stop");
  const std::vector<int64_t> step_attr = ctx->Attr<std::vector<int64_t>>("step");
  const int64_t ndim = start_attr.size();
  std::vector<int64_t> positive_start_vec(ndim);
  std::vector<int64_t> positive_stop_vec(ndim);

  // regulate axis number
  std::vector<int64_t> logical_dims(ndim);
  {
    for (int i = 0; i < ndim; i++) {
      if (!slice_ctx.IsAxisPushed(i)) {
        // axis is not split, logical shape is same as physical shape
        logical_dims[i] = large->shape_view().At(i);
      }
    }
    for (const auto& split_info : slice_ctx.GetSplitInfo()) {
      if (split_info.split_axis != SPLIT_AXIS_FOR_NON_SPLIT) {
        logical_dims[split_info.split_axis] = split_info.logical_length;
      }
    }
  }
  for (int i = 0; i < ndim; i++) {
    positive_start_vec[i] = RegulateSliceStart(start_attr[i], logical_dims[i]);
    positive_stop_vec[i] = RegulateSliceStop(stop_attr[i], logical_dims[i]);
  }

  SliceParams large_slice_param;
  std::copy(large->stride().begin(), large->stride().end(), large_slice_param.stride);
  SliceParams small_slice_param;
  std::copy(small->stride().begin(), small->stride().end(), small_slice_param.stride);
  ConstructSliceParamsLarge(slice_ctx, positive_start_vec, positive_stop_vec, step_attr,
                            large->shape_view(), &large_slice_param);
  ConstructSliceParamsSmall(slice_ctx, positive_start_vec, positive_stop_vec, step_attr,
                            small->shape_view(), &small_slice_param);
  CHECK_EQ(large_slice_param.elem_cnt(), small_slice_param.elem_cnt());

  if (large_slice_param.ndim == 0 && small_slice_param.ndim == 0) {
    // Copy data directly for scalar tensor
    AutoMemcpy(ctx->stream(), dst->mut_dptr(), src->dptr(), GetSizeOfDataType(dst->data_type()),
               src->mem_case(), dst->mem_case());
    return;
  }
  if (from_large_to_small) {
    if (small_slice_param.elem_cnt() == small->shape_view().elem_cnt()) {
      SliceKernelUtil::Forward(ctx->stream(), large_slice_param, dst->data_type(), src->dptr(),
                               dst->mut_dptr());
    } else {
      AutoMemset(ctx->stream(), dst->mut_dptr(), 0,
                 dst->shape_view().elem_cnt() * GetSizeOfDataType(dst->data_type()),
                 dst->mem_case());
      SliceKernelUtil::Forward(ctx->stream(), large_slice_param, small_slice_param,
                               dst->data_type(), src->dptr(), dst->mut_dptr());
    }
  } else {
    SliceKernelUtil::Forward(ctx->stream(), small_slice_param, large_slice_param, dst->data_type(),
                             src->dptr(), dst->mut_dptr());
  }
}

}  // namespace

class MluSliceKernel final : public user_op::OpKernel {
 public:
  MluSliceKernel() = default;
  ~MluSliceKernel() = default;

  std::shared_ptr<user_op::OpKernelCache> InitOpKernelCache(
      user_op::KernelCacheContext* ctx) const override {
    SliceContext slice_ctx;
    if (ctx->parallel_ctx().parallel_num() == 1) {
      // split_axis == SPLIT_AXIS_FOR_NON_SPLIT means the sbp attribute is not 'split'
      CHECK_JUST(slice_ctx.PushSplitInfo(SPLIT_AXIS_FOR_NON_SPLIT, 0, 0, 0));
    } else {
      const Shape& parallel_hierarchy = *ctx->parallel_desc().hierarchy();
      NdSbp in_nd_sbp = ctx->NdSbp4ArgNameAndIndex("x", 0);
      {
        const NdSbp& y_nd_sbp = ctx->NdSbp4ArgNameAndIndex("y", 0);
        // If x and y both split in the same axis(must be full slice),
        // we can consider the physical tensor is broadcast in this axis.
        FOR_RANGE(int32_t, i, 0, parallel_hierarchy.NumAxes()) {
          const SbpParallel& x_sbp = in_nd_sbp.sbp_parallel(i);
          const SbpParallel& y_sbp = y_nd_sbp.sbp_parallel(i);
          if (x_sbp.has_split_parallel() && y_sbp.has_split_parallel()) {
            CHECK_EQ(x_sbp.split_parallel().axis(), y_sbp.split_parallel().axis());
            in_nd_sbp.mutable_sbp_parallel(i)->clear_split_parallel();
            in_nd_sbp.mutable_sbp_parallel(i)->mutable_broadcast_parallel();
          }
        }
      }
      const Shape& logical_shape = ctx->LogicalTensorDesc4ArgNameAndIndex("x", 0)->shape();
      const int64_t parallel_id = ctx->parallel_ctx().parallel_id();
      const TensorSliceView& slice_view =
          GetTensorSliceView4ParallelId(parallel_hierarchy, in_nd_sbp, logical_shape, parallel_id);
      for (int i = 0; i < logical_shape.NumAxes(); ++i) {
        const Range& range = slice_view.At(i);
        if (range.begin() != 0 || range.end() != logical_shape.At(i)) {
          CHECK_JUST(slice_ctx.PushSplitInfo(i, range.begin(), range.end(), logical_shape.At(i)));
        }
      }
    }
    return std::make_shared<OpKernelCacheWrapper<SliceContext>>(slice_ctx);
  }

  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState*,
               const user_op::OpKernelCache* cache) const override {
    user_op::Tensor* y_tensor = ctx->Tensor4ArgNameAndIndex("y", 0);
    if (y_tensor->shape_view().elem_cnt() == 0) { return; }
    const user_op::Tensor* x_tensor = ctx->Tensor4ArgNameAndIndex("x", 0);
    const SliceContext& slice_ctx =
        dynamic_cast<const OpKernelCacheWrapper<SliceContext>*>(cache)->Get();
    WriteSlice(ctx, x_tensor, y_tensor, slice_ctx, /*from_large_to_small=*/true);
  }

  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

class MluSliceUpdateKernel final : public user_op::OpKernel {
 public:
  MluSliceUpdateKernel() = default;
  ~MluSliceUpdateKernel() = default;

  std::shared_ptr<user_op::OpKernelCache> InitOpKernelCache(
      user_op::KernelCacheContext* ctx) const override {
    SliceContext slice_ctx;
    if (ctx->parallel_ctx().parallel_num() == 1) {
      // split_axis == SPLIT_AXIS_FOR_NON_SPLIT means the sbp attribute is not 'split'
      CHECK_JUST(slice_ctx.PushSplitInfo(SPLIT_AXIS_FOR_NON_SPLIT, 0, 0, 0));
    } else {
      const Shape& parallel_hierarchy = *ctx->parallel_desc().hierarchy();
      NdSbp ref_nd_sbp = ctx->NdSbp4ArgNameAndIndex("ref", 0);
      {
        const NdSbp& value_nd_sbp = ctx->NdSbp4ArgNameAndIndex("value", 0);
        // If ref and value both split in the same axis(full slice),
        // we can consider the physical tensor is broadcast in this axis.
        for (int i = 0; i < parallel_hierarchy.NumAxes(); ++i) {
          const SbpParallel& ref_sbp = ref_nd_sbp.sbp_parallel(i);
          const SbpParallel& value_sbp = value_nd_sbp.sbp_parallel(i);
          if (ref_sbp.has_split_parallel() && value_sbp.has_split_parallel()) {
            CHECK_EQ(ref_sbp.split_parallel().axis(), value_sbp.split_parallel().axis());
            ref_nd_sbp.mutable_sbp_parallel(i)->clear_split_parallel();
            ref_nd_sbp.mutable_sbp_parallel(i)->mutable_broadcast_parallel();
          }
        }
      }
      const Shape& logical_shape = ctx->LogicalTensorDesc4ArgNameAndIndex("ref", 0)->shape();
      const int64_t parallel_id = ctx->parallel_ctx().parallel_id();
      const TensorSliceView& slice_view =
          GetTensorSliceView4ParallelId(parallel_hierarchy, ref_nd_sbp, logical_shape, parallel_id);
      for (int i = 0; i < logical_shape.NumAxes(); ++i) {
        const Range& range = slice_view.At(i);
        if (range.begin() != 0 || range.end() != logical_shape.At(i)) {
          CHECK_JUST(slice_ctx.PushSplitInfo(i, range.begin(), range.end(), logical_shape.At(i)));
        }
      }
    }
    return std::make_shared<OpKernelCacheWrapper<SliceContext>>(slice_ctx);
  }

  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState*,
               const user_op::OpKernelCache* cache) const override {
    const user_op::Tensor* value_tensor = ctx->Tensor4ArgNameAndIndex("value", 0);
    user_op::Tensor* ref_tensor = ctx->Tensor4ArgNameAndIndex("ref", 0);
    user_op::Tensor* y_tensor = ctx->Tensor4ArgNameAndIndex("y", 0);
    if (y_tensor->shape_view().elem_cnt() == 0) { return; }
    // When eager executing, y_tensor shared the same memory with ref_tensor
    if (ref_tensor->dptr() != y_tensor->dptr()) {
      // lazy run
      AutoMemcpy(ctx->stream(), y_tensor->mut_dptr(), ref_tensor->dptr(),
                 y_tensor->shape_view().elem_cnt() * GetSizeOfDataType(y_tensor->data_type()),
                 ref_tensor->mem_case(), y_tensor->mem_case());
    }
    const SliceContext& slice_ctx =
        dynamic_cast<const OpKernelCacheWrapper<SliceContext>*>(cache)->Get();
    WriteSlice(ctx, value_tensor, y_tensor, slice_ctx, /*from_large_to_small=*/false);
  }

  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

REGISTER_USER_KERNEL("slice").SetCreateFn<MluSliceKernel>().SetIsMatchedHob(
    (user_op::HobDeviceType() == DeviceType::kMLU));
REGISTER_USER_KERNEL("slice_update")
    .SetCreateFn<MluSliceUpdateKernel>()
    .SetIsMatchedHob((user_op::HobDeviceType() == DeviceType::kMLU));

}  // namespace mlu
}  // namespace oneflow
