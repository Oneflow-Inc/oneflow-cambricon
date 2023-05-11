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
#ifndef ONEFLOW_CAMBRICON_EP_MLU_EVENT_H_
#define ONEFLOW_CAMBRICON_EP_MLU_EVENT_H_

#include "oneflow_mlu/common/mlu_util.h"
#include "oneflow/core/ep/include/event.h"

namespace oneflow {
namespace ep {

class MluEvent : public Event {
 public:
  OF_DISALLOW_COPY_AND_MOVE(MluEvent);
  explicit MluEvent(unsigned int flags);
  ~MluEvent() override;

  Maybe<bool> QueryDone() override;
  Maybe<void> Sync() override;

  cnrtNotifier_t mlu_event();

 private:
  cnrtNotifier_t mlu_event_;
};

}  // namespace ep
}  // namespace oneflow

#endif  // ONEFLOW_CAMBRICON_EP_MLU_EVENT_H_
