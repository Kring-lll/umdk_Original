import os
import pytest
import torch
import torch_npu
import torch.distributed as dist
from .util import tool
# 用以保证各用例执行同步
@pytest.fixture(scope="function", autouse=True)
def test_cases_sync():
    if tool.is_torchrun_process():
        dist.barrier()
    yield
    if tool.is_torchrun_process():
        dist.barrier()


@pytest.fixture(scope="session", autouse=True)
def mpt_global_fixture():
    if tool.is_torchrun_process():
        device = torch.device("npu", tool.get_local_rank())
        torch_npu.npu.set_device(device)
        dist.init_process_group(backend="hccl", rank=tool.get_rank(), world_size=tool.get_world_size())
    yield
