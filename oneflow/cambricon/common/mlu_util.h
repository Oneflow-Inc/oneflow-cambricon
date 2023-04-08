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
#ifndef ONEFLOW_CAMBRICON_COMMON_MLU_UTIL_H_
#define ONEFLOW_CAMBRICON_COMMON_MLU_UTIL_H_

#include "oneflow/core/common/util.h"
#include "cnrt.h"
#include "cnnl.h"
#include "cnpapi.h"

#define OF_MLU_CHECK(condition)                                                        \
  for (cnrtRet_t _cnrt_check_status = (condition); _cnrt_check_status != cnrtSuccess;) \
  THROW(RuntimeError) << "CNRT check failed: " #condition " : "                        \
                      << " (" << _cnrt_check_status << ") "

#define OF_CNNL_CHECK(condition)                                                                  \
  for (cnnlStatus_t _cnnl_check_status = (condition); _cnnl_check_status != CNNL_STATUS_SUCCESS;) \
  THROW(RuntimeError) << "CNNL check failed: " #condition " : "                                   \
                      << " (error code:" << _cnnl_check_status                                    \
                      << " " + std::string(cnnlGetErrorString(_cnnl_check_status)) + ") "

#define OF_CNPAPI_CHECK(condition)                                    \
  do {                                                                \
    cnpapiResult _cnpapi_check_status = (condition);                  \
    if (_cnpapi_check_status != CNPAPI_SUCCESS) {                     \
      const char* errstr;                                             \
      cnpapiGetResultString(_cnpapi_check_status, &errstr);           \
      THROW(RuntimeError) << "CNPAPI check failed: " #condition " : " \
                          << " (error code:" << _cnpapi_check_status  \
                          << " " + std::string(errstr) + ") ";        \
    }                                                                 \
  } while (0)

#endif  // ONEFLOW_CAMBRICON_COMMON_MLU_UTIL_H_
