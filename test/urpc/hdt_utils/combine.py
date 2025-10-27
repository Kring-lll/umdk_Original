"""
SPDX-License-Identifier: MIT
Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
Description: combine all coverage report from test projects
"""

import hdt_func


def main():
    build_dir = hdt_func.get_build_dir()
    projects = hdt_func.discover()
    opt = hdt_func.normopt()
    [hdt_func.system(f"hdt report {x} {opt}") for x in projects]

    lcov_dir = hdt_func.get_comp_path('lcov', projects[0])
    print(lcov_dir)

    if lcov_dir != '' :
        lcov_dir = lcov_dir + '/bin/'
    lcov = lcov_dir + "lcov"
    genhtml = lcov_dir + "genhtml"

    merged = ""
    for x in projects :
        merged = merged + " -a " + x + "/build/lcov_report/lcov_report.info"
    print("{} {} -o {}/lcov_report.info".format(lcov, merged, build_dir))
    hdt_func.system(f"{lcov} {merged} -o {build_dir}/lcov_report.info -q --rc lcov_branch_coverage=1")
    print("{} -o {}/lcov_report {}/lcov_report.info -q --rc lcov_branch_coverage=1".
        format(genhtml, build_dir, build_dir))
    hdt_func.system(f"{genhtml} -o {build_dir}/lcov_report {build_dir}/lcov_report.info -q --rc lcov_branch_coverage=1")


if __name__ == "__main__":
    main()
