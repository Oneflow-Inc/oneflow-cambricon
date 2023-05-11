"""
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
"""

import unittest
from collections import OrderedDict

import numpy as np
from oneflow.test_utils.test_util import GenArgList

import oneflow as flow
import oneflow.unittest
import oneflow_mlu


def _test_rsqrt_forward(test_case, shape, device, dtype):
    arr = np.random.rand(*shape) * 10
    x = flow.tensor(arr, device=flow.device(device), dtype=dtype)
    x_cpu = x.cpu()

    of_out = flow._C.rsqrt(x_cpu)
    mlu_out = flow._C.rsqrt(x)
    test_case.assertTrue(np.allclose(mlu_out.numpy(), of_out.numpy(), 0.001, 0.001))


@flow.unittest.skip_unless_1n1d()
class TestReluCambriconModule(flow.unittest.TestCase):
    def test_relu(test_case):
        arg_dict = OrderedDict()
        arg_dict["test_fun"] = [
            _test_rsqrt_forward,
        ]
        arg_dict["shape"] = [(2,), (2, 3), (2, 3, 4), (2, 3, 4, 5)]
        arg_dict["device"] = ["mlu"]
        arg_dict["dtype"] = [flow.float32]
        for arg in GenArgList(arg_dict):
            arg[0](test_case, *arg[1:])


if __name__ == "__main__":
    unittest.main()
