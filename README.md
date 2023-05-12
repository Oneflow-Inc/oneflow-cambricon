# oneflow-mlu

OneFlow-MLU is an OneFlow extension that enables oneflow to run on the Cambrian MLU chips. Currently it only supports the MLU 370 series.

## Installation

### pip

TODO

### Building From Source

#### Prerequisites

- install cmake
- build oneflow with cpu only from source and install it

#### Get the OneFlow-MLU Source

```shell
git clone https://github.com/Oneflow-Inc/oneflow-mlu
```

#### Building

Inside OneFlow-MLU source directory, then run the following command to install `oneflow_mlu`,

```shell
python3 setup.py install
```

## Run A Toy Program

```python
# python3

>>> import oneflow_mlu
>>> import oneflow as flow
>>>
>>> m = flow.nn.Linear(3, 4).to("mlu")
>>> x = flow.randn(4, 3, device="mlu")
>>> y = m(x)
>>> print(y)
tensor([[ 0.4239, -0.4689, -0.1660,  0.0718],
        [ 0.5413,  1.9006,  2.0763,  0.8693],
        [ 0.4226, -0.0207,  0.1006,  0.2234],
        [ 0.4054, -0.2816, -0.4405,  0.1099]], device='mlu:0', dtype=oneflow.float32, grad_fn=<broadcast_addBackward>)
```
