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
#ifndef ONEFLOW_CAMBRICON_EP_PRIMITIVE_TYPE_SEQ_H_
#define ONEFLOW_CAMBRICON_EP_PRIMITIVE_TYPE_SEQ_H_

#include <half.hpp>
#include "oneflow/core/common/preprocessor.h"
#include "oneflow/core/common/data_type.h"

#define MLU_PRIMITIVE_BOOL_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(bool, DataType::kBool)
#define MLU_PRIMITIVE_CHAR_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(char, DataType::kChar)
#define MLU_PRIMITIVE_INT8_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(int8_t, DataType::kInt8)
#define MLU_PRIMITIVE_UINT8_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(uint8_t, DataType::kUInt8)
#define MLU_PRIMITIVE_INT32_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(int32_t, DataType::kInt32)
#define MLU_PRIMITIVE_UINT32_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(uint32_t, DataType::kUInt32)
#define MLU_PRIMITIVE_INT64_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(int64_t, DataType::kInt64)
#define MLU_PRIMITIVE_UINT64_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(uint64_t, DataType::kUInt64)
#define MLU_PRIMITIVE_FLOAT_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(float, DataType::kFloat)
#define MLU_PRIMITIVE_DOUBLE_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(double, DataType::kDouble)
#define MLU_PRIMITIVE_FLOAT16_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(float16, DataType::kFloat16)
#define MLU_PRIMITIVE_BFLOAT16_TYPE_SEQ OF_PP_MAKE_TUPLE_SEQ(bfloat16, DataType::kBFloat16)

#define MLU_PRIMITIVE_ALL_TYPE_SEQ \
  MLU_PRIMITIVE_BOOL_TYPE_SEQ      \
  MLU_PRIMITIVE_INT8_TYPE_SEQ      \
  MLU_PRIMITIVE_UINT8_TYPE_SEQ     \
  MLU_PRIMITIVE_INT32_TYPE_SEQ     \
  MLU_PRIMITIVE_UINT32_TYPE_SEQ    \
  MLU_PRIMITIVE_INT64_TYPE_SEQ     \
  MLU_PRIMITIVE_UINT64_TYPE_SEQ    \
  MLU_PRIMITIVE_FLOAT_TYPE_SEQ     \
  MLU_PRIMITIVE_FLOAT16_TYPE_SEQ

#define MLU_PRIMITIVE_ACTIVATION_BACKWARD_GRAD_TYPE_SEQ \
  MLU_PRIMITIVE_FLOAT_TYPE_SEQ                          \
  MLU_PRIMITIVE_FLOAT16_TYPE_SEQ

#endif  // ONEFLOW_CAMBRICON_EP_PRIMITIVE_TYPE_SEQ_H_
