"""
SPDX-License-Identifier: MIT
Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
Description: incept all test projects, download all necessary dependencies, such as asan/lcov/mockcpp
"""

import hdt_func


def normopt():
    return "-i"


def main():
    opt = normopt()
    projects = hdt_func.discover()
    [hdt_func.system(f"hdt comp {x} {opt}") for x in projects]


if __name__ == "__main__":
    main()
