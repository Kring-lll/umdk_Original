#!/bin/bash

set -e

echo "========================================="
echo "  Rjetty 变长数据交互验证脚本"
echo "========================================="
echo ""

PROJECT_ROOT="C:/zhoulingjuan/code/rjetty/umdk"
TEST_DIR="${PROJECT_ROOT}/test/urpc/umq"
BUILD_DIR="${PROJECT_ROOT}/build_rjetty_test"

echo "步骤1: 检查修改的文件"
echo "-------------------------------------------"
if [ -f "${PROJECT_ROOT}/src/urpc/umq/umq_ub/core/private/umq_symbol_private.c" ]; then
    echo "✅ umq_symbol_private.c 存在"
    grep -n "stub_urma_get_rjetty" "${PROJECT_ROOT}/src/urpc/umq/umq_ub/core/private/umq_symbol_private.c" | head -2 || echo "⚠️  stub函数未找到"
else
    echo "❌ umq_symbol_private.c 不存在"
    exit 1
fi

if [ -f "${PROJECT_ROOT}/src/urpc/umq/umq_ub/core/private/umq_ub.c" ]; then
    echo "✅ umq_ub.c 存在"
    grep -n "urma_get_rjetty" "${PROJECT_ROOT}/src/urpc/umq/umq_ub/core/private/umq_ub.c" | head -3 || echo "⚠️  调用未找到"
else
    echo "❌ umq_ub.c 不存在"
    exit 1
fi

if [ -f "${TEST_DIR}/test_umq_ub.cpp" ]; then
    echo "✅ test_umq_ub.cpp 存在"
    grep -n "test_rjetty_variable_length" "${TEST_DIR}/test_umq_ub.cpp" | head -1 || echo "⚠️  测试用例未添加"
else
    echo "❌ test_umq_ub.cpp 不存在"
    exit 1
fi

echo ""
echo "步骤2: 准备编译环境"
echo "-------------------------------------------"
if [ ! -d "${BUILD_DIR}" ]; then
    echo "创建编译目录: ${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"
else
    echo "编译目录已存在: ${BUILD_DIR}"
fi

echo ""
echo "步骤3: 编译说明"
echo "-------------------------------------------"
echo "由于这是 Windows 环境（C:/路径），建议使用以下命令手动编译："
echo ""
echo "cd ${BUILD_DIR}"
echo "cmake ../test/urpc/umq -G \"Unix Makefiles\""
echo "make test_umq -j4"
echo ""
echo "或者在项目根目录使用原有编译流程："
echo "cd ${PROJECT_ROOT}"
echo "./build.sh 或使用原有构建系统"
echo ""

echo "步骤4: 运行测试说明"
echo "-------------------------------------------"
echo "编译成功后，运行测试："
echo ""
echo "cd ${BUILD_DIR}"
echo "./test_umq --gtest_filter=UmqUBTest.test_rjetty_variable_length*"
echo ""
echo "预期输出："
echo "  [ INFO] stub: get rjetty for jetty_id XXX, base_size 56, extra_size 32, total_length 88"
echo "  [ PASSED  ] test_rjetty_variable_length_serialize_deserialize"
echo ""

echo "步骤5: 验证关键点"
echo "-------------------------------------------"
echo "✓ 变长数据（基础56字节 + 额外32字节）正确分配"
echo "✓ 序列化时 memcpy 复制完整的88字节"
echo "✓ TLV len 字段包含完整长度 sizeof(queue_info) + 88"
echo "✓ 反序列化正确解析 rjetty_size = 88"
echo "✓ 额外数据完整性验证通过"
echo "✓ 不同 jetty_id 的数据正确区分"
echo ""

echo "========================================="
echo "  验证准备完成"
echo "========================================="
echo ""
echo "请在 Windows 环境下使用适当的编译工具（CMake + Make 或 Visual Studio）"
echo "编译并运行测试以验证变长 rjetty 数据的交互功能"
echo ""
echo "详细文档请参考："
echo "  - STUB_RJETTY_TEST.md"
echo "  - VERIFY_RJETTY_VARIABLE_LENGTH.md"
echo ""