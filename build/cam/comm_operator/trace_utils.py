import os
import re

def get_define_value_from_file(filepath, macro_name):
    """
    从单个头文件中提取 #define 宏的值，并尝试用 eval 计算表达式
    :param filepath: 头文件的完整路径
    :param macro_name: 宏名称，如 "PROF_SIZE_PER_CORE"
    :return: 计算后的数值（int/float），未找到则返回 None
    """
    if not os.path.isfile(filepath):
        print(f"文件不存在: {filepath}")
        return None

    # 匹配 #define 宏定义的正则（忽略注释）
    pattern = re.compile(r'^\s*#\s*define\s+' + re.escape(macro_name) + r'\s+([^\\/].*?)(?:\s*//.*|$)', re.IGNORECASE)

    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            for line_num, line in enumerate(f, 1):
                line = line.strip()
                # 跳过纯注释行
                if line.startswith('//') or line.startswith('/*'):
                    continue

                match = pattern.match(line)
                if match:
                    value_expr = match.group(1).strip()
                    # 去除可能的括号，如 (1024) 或 (1024 * 2)
                    value_expr = value_expr.strip()
                    if value_expr.startswith('(') and value_expr.endswith(')'):
                        value_expr = value_expr[1:-1].strip()

                    print(f"找到宏定义: {macro_name} = {value_expr} (在 {filepath} 第 {line_num} 行)")

                    # 使用 eval 安全计算表达式
                    try:
                        # 构建安全的命名空间（只允许基本数学常量和操作）
                        safe_namespace = {
                            '__builtins__': {},
                            'True': True,
                            'False': False,
                        }
                        result = eval(value_expr, safe_namespace)
                        print(f"计算结果: {result}")
                        return result
                    except Exception as e:
                        print(f"eval 计算失败: {e}")
                        return None
    except Exception as e:
        print(f"读取文件出错: {e}")
        return None

    return None  # 未找到

def get_define_value(relative_dir, filename, macro_name):
    """
    从相对路径目录中读取宏定义
    :param relative_dir: 相对路径目录，如 "include", ".", "src/inc" 等
    :param filename: 头文件名，如 "config.h"
    :param macro_name: 宏名称
    :return: 宏的值（计算后）
    """
    # 将相对路径转为基于当前脚本所在目录的绝对路径（可选）
    script_dir = os.path.dirname(__file__)  # 脚本所在目录
    if not script_dir:  # 如果脚本在当前目录运行
        script_dir = os.getcwd()

    filepath = os.path.join(script_dir, relative_dir, filename)
    return get_define_value_from_file(filepath, macro_name)

def get_prof_size_per_core():
    relative_dir = "../../../src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel"          # 相对路径目录（相对于脚本所在位置）
    filename = "fused_deep_moe_base.h"             # 头文件名
    macro_name = "PROF_SIZE_PER_CORE"
    prof_size_per_core = get_define_value(relative_dir, filename, macro_name)
    if prof_size_per_core is not None:
        print(f"🎉 最终结果: {macro_name} = {prof_size_per_core}")
    else:
        print(f"❌ 未找到宏 {macro_name}，请检查路径和文件内容。")
    return prof_size_per_core

def get_enable_moe_profiling():
    relative_dir = "../../../src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel"          # 相对路径目录（相对于脚本所在位置）
    filename = "fused_deep_moe_base.h"             # 头文件名
    macro_name = "ENABLE_MOE_PROFILING"
    enable_moe_profiling = get_define_value(relative_dir, filename, macro_name)
    if enable_moe_profiling is not None:
        print(f"🎉 最终结果: {macro_name} = {enable_moe_profiling}")
    else:
        print(f"❌ 未找到宏 {macro_name}，请检查路径和文件内容。")
    return enable_moe_profiling != 0

def get_core_num_list():
    return [24, 48]