"""
SPDX-License-Identifier: MIT
Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
Description: clean all test projects
"""

import hdt_func


def main():
    opt = hdt_func.normopt()
    projects = hdt_func.discover()
    [hdt_func.system(f"hdt clean {x} {opt}") for x in projects]


if __name__ == "__main__":
    main()
