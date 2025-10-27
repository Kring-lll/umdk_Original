"""
SPDX-License-Identifier: MIT
Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
Description: run all test projects
"""

import hdt_func


def main():
    opt = hdt_func.normopt()
    projects = hdt_func.discover()
    raise_exception = 0
    for modules in projects:
        ret = hdt_func.system_with_return(f"hdt run {modules} {opt}")
        if ret != 0:
            raise_exception = ret

    if raise_exception != 0:
        raise SystemExit("Fail to execute command, return = {}".format(raise_exception))


if __name__ == "__main__":
    main()
