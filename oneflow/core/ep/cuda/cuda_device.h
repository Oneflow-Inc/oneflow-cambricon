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
#ifndef ONEFLOW_CORE_EP_CUDA_CUDA_DEVICE_H_
#define ONEFLOW_CORE_EP_CUDA_CUDA_DEVICE_H_

#include "oneflow/core/ep/include/device.h"

#ifdef WITH_CUDA

namespace oneflow {

namespace ep {

class CudaDevice : public Device {
 public:
  OF_DISALLOW_COPY_AND_MOVE(CudaDevice);
  explicit CudaDevice(int device_index);
  virtual ~CudaDevice();

  void SetAsActiveDevice() override;

  Stream* CreateStream() override;
  void DestroyStream(Stream* stream) override;

  void CreateEvents(Event** events, size_t count) override;
  void DestroyEvents(Event** events, size_t count) override;

 private:
  int device_index_;
  std::mutex events_mutex_;
  std::vector<Event*> events_;
  unsigned int event_flags_;
};

}  // namespace ep

}  // namespace oneflow

#endif  // WITH_CUDA

#endif  // ONEFLOW_CORE_EP_CUDA_CUDA_DEVICE_H_