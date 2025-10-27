"""
SPDX-License-Identifier: MIT
Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
Description: common functions
"""

import os
import sys
#import shlex
import pathlib
import yaml


def system(cmd):
    print(f"Executing command: {cmd}")
    ret = os.system(cmd)
    if ret != 0:
        raise SystemExit("Fail to execute command, return = {}".format(ret))


def system_with_return(cmd):
    print(f"Executing command: {cmd}")
    return os.system(cmd)


def normopt():
    new_arg_list = []
    for arg in sys.argv[1:]:
        if arg == "'":
            new_arg_list.append('" ')
        elif arg == "-a ":
            new_arg_list.append('--args= ')
        else:
            new_arg_list.append(arg + " ")
    return "".join(new_arg_list)
    # for python3.8 and above, return shlex.join(sys.argv[1:]).replace("'", '"').replace("-a ", "--args=")


def discover():
    test_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    project = yaml.load(open("project.yaml"), Loader=yaml.FullLoader)
    projects = project['project']
    print("Discovering {} test projects: {} ...".format(len(projects), projects))
    return [os.path.join(test_dir, x) for x in projects]


def get_build_dir():
    project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build_dir = os.path.join(project_dir, "build")
    return build_dir


def get_comp_path(cmd, project):
    r = os.popen(f"hdt comp {project}")
    lines = r.readlines()
    for line in lines :
        tmp = ' '.join(line.split()) # remove one more space
        comp = tmp.split(' ')[-1]
        if comp.split('/')[-1] == cmd :
            return comp
    return ''