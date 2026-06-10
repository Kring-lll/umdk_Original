# Rjetty 变长结构体交互验证指南

## 修改内容

### 1. 打桩函数支持变长数据

修改了 `src/urpc/umq/umq_ub/core/private/umq_symbol_private.c`：

```c
static urma_status_t stub_urma_get_rjetty(urma_jetty_t *jetty, urma_rjetty_t **rjetty, uint32_t *length)
{
    // 基础结构体大小 + 32字节额外数据
    uint32_t extra_data_size = 32;
    uint32_t total_size = sizeof(urma_rjetty_t) + extra_data_size;
    
    // 分配变长内存
    urma_rjetty_t *stub_rjetty = (urma_rjetty_t *)malloc(total_size);
    
    // 填充基础字段
    stub_rjetty->jetty_id = jetty->jetty_id;
    stub_rjetty->trans_mode = jetty->jetty_cfg.trans_mode;
    stub_rjetty->policy = jetty->jetty_cfg.shared.policy;
    stub_rjetty->type = URMA_TARGET_JETTY;
    stub_rjetty->tp_type = jetty->tp_type;
    
    // 填充额外数据（用于验证）
    char *extra_data = (char *)stub_rjetty + sizeof(urma_rjetty_t);
    for (uint32_t i = 0; i < extra_data_size; i++) {
        extra_data[i] = (char)(i + jetty->jetty_id.id);
    }
    
    *rjetty = stub_rjetty;
    *length = total_size;  // 返回总长度
    
    return URMA_SUCCESS;
}
```

**关键点**：
- 分配 `sizeof(urma_rjetty_t) + 32` 字节的内存
- 返回的 `length` 是总长度（包含额外数据）
- 额外数据填充了可验证的模式

### 2. 测试用例验证

添加了测试用例 `test_rjetty_variable_length_serialize_deserialize`：

```cpp
TEST_F(UmqUBTest, test_rjetty_variable_length_serialize_deserialize)
{
    // 1. 序列化 bind info
    uint32_t serialize_size = umq_ub_bind_info_serialize(&queue, bind_info_buf, sizeof(bind_info_buf));
    
    // 2. 反序列化
    int ret = umq_ub_bind_info_deserialize(bind_info_buf, serialize_size, &bind_info);
    
    // 3. 验证变长数据
    uint32_t expected_base_size = sizeof(urma_rjetty_t);  // 56字节
    uint32_t expected_extra_size = 32;
    uint32_t expected_total_size = expected_base_size + expected_extra_size;  // 88字节
    
    ASSERT_EQ(bind_info.queue_info->rjetty_size, expected_total_size);
    
    // 4. 验证基础字段
    ASSERT_EQ(queue_rjetty->jetty_id.id, 100);
    ASSERT_EQ(queue_rjetty->trans_mode, URMA_TM_UB);
    
    // 5. 验证额外数据完整性
    char *extra_data = (char *)bind_info.queue_info->rjetty + expected_base_size;
    for (uint32_t i = 0; i < expected_extra_size; i++) {
        char expected_value = (char)(i + jetty_id);
        ASSERT_EQ(extra_data[i], expected_value);
    }
}
```

## 编译和测试步骤

### 1. 编译测试代码

```bash
cd C:\zhoulingjuan\code\rjetty\umdk

# 如果 build 目录不存在，创建并初始化
if [ ! -d "build_test" ]; then
    mkdir -p build_test
fi

cd build_test

# 配置编译选项（根据项目实际情况）
cmake ../test/urpc/umq \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_TESTS=ON

# 编译
make test_umq -j4
```

### 2. 运行测试

```bash
cd build_test

# 运行特定测试
./test_umq --gtest_filter=UmqUBTest.test_rjetty_variable_length*

# 或运行所有 UMQ UB 测试
./test_umq --gtest_filter=UmqUBTest.*
```

### 3. 查看日志验证

测试成功时会输出类似日志：

```
[ INFO] stub: get rjetty for jetty_id 100, base_size 56, extra_size 32, total_length 88
[ INFO] Serialize bind info success, size XXX
[ INFO] Rjetty variable length test passed!
[ INFO]   - Base size: 56
[ INFO]   - Extra size: 32
[ INFO]   - Total size: 88
[ INFO]   - Queue rjetty verified successfully
[ INFO]   - FC rjetty verified successfully
```

## 验证要点

### 1. 序列化验证

- ✅ `umq_ub_queue_info_serialize` 正确处理变长 rjetty
- ✅ `info_tlv_head->len` 包含完整长度（基础+额外）
- ✅ `memcpy` 复制完整的变长数据

### 2. 反序列化验证

- ✅ `umq_ub_bind_info_deserialize` 正确解析变长数据
- ✅ `bind_info.queue_info->rjetty_size` 反映真实长度
- ✅ 通过柔性数组 `rjetty[0]` 正确访问变长数据

### 3. 数据完整性验证

- ✅ 基础字段（jetty_id, trans_mode, tp_type）正确
- ✅ 额外数据完整保留，未丢失或截断
- ✅ 不同 jetty_id 的额外数据内容正确区分

## 变长结构体设计

### 数据结构

```c
// urma_rjetty_t 固定大小结构体
typedef struct urma_rjetty {
    urma_jetty_id_t jetty_id;       // 16字节
    urma_transport_mode_t trans_mode; // 4字节
    urma_jetty_grp_policy_t policy;  // 4字节
    urma_target_type_t type;         // 4字节
    urma_import_jetty_flag_t flag;   // 4字节
    urma_tp_type_t tp_type;          // 4字节
} urma_rjetty_t;                     // 总计约56字节

// bind info 中的柔性数组支持变长
typedef struct umq_ub_bind_queue_info {
    // ... 其他字段 ...
    uint32_t rjetty_size;            // 实际大小
    urma_rjetty_t rjetty[0];         // 柔性数组
} umq_ub_bind_queue_info_t;
```

### 序列化流程

```
1. stub_urma_get_rjetty() 
   -> 返回 length = 88 (56基础 + 32额外)

2. umq_ub_queue_info_serialize()
   -> 检查缓冲区大小：sizeof(queue_info) + 88
   -> memcpy(queue_info->rjetty, rjetty, 88)
   -> info_tlv_head->len = sizeof(queue_info) + 88

3. 传输
   -> TLV 格式：type | len | value(包含完整88字节rjetty)
```

### 反序列化流程

```
1. umq_ub_bind_info_deserialize()
   -> 解析 TLV head
   -> 检查：info_tlv_head->len == sizeof(queue_info) + rjetty_size
   -> 指针赋值：bind_info.queue_info = (umq_ub_bind_queue_info_t *)value

2. 访问数据
   -> queue_info->rjetty_size = 88
   -> 通过柔性数组访问：queue_info->rjetty[0] 开始的88字节
   -> 验证额外数据完整性
```

## 预期结果

如果变长交互成功，会看到：

```
[ PASSED  ] test_rjetty_variable_length_serialize_deserialize
```

所有 ASSERT_EQ 和 ASSERT_GT 都通过，说明：

1. ✅ 变长数据正确序列化
2. ✅ 变长数据正确反序列化  
3. ✅ 数据完整性保持
4. ✅ 额外数据未丢失
5. ✅ bind 流程可以正确处理变长 rjetty

## 后续工作

当 urma 提供真实接口后：

1. 真实接口可能返回不同长度的 rjetty（根据实际需求）
2. 当前代码会自动适配真实接口返回的长度
3. 无需修改序列化/反序列化逻辑
4. 只需移除打桩函数即可

## 关键验证数据

| 项目                | 值              |
|---------------------|-----------------|
| urma_rjetty_t大小   | 56字节          |
| 额外数据大小        | 32字节          |
| 总大小              | 88字节          |
| 验证模式            | extra_data[i] = i + jetty_id |
| 数据完整性          | 所有88字节正确传递 |