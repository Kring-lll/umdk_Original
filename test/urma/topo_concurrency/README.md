# main_ue_eid 测试工具说明

这组测试代码用于验证 main_ue_eid 查询表配置链路，主要包括 urma_admin
命令行测试、直接接口测试、大 topo 并发一致性测试，以及 ftrace 函数耗时
测试。

测试目录可以放在任意路径。配置文件默认用自身所在目录作为测试代码目录，
不依赖固定的 `JD_ROOT`。如果 kernel 或 UMDK 仓库不在测试目录旁边，需要
显式指定路径。

## 通用配置

推荐使用 `main_primary_eid_debug.wyh.conf`。运行前按实际环境设置：

```bash
export KERNEL_REPO=/root/lwj/kernel
export UMDK_REPO=/root/umdk1/umdk
export LIBTPSA_DIR=/usr/lib64
```

如果要直接使用 UMDK 编译目录中的 `libtpsa.so`，把 `LIBTPSA_DIR` 指向
包含 `libtpsa.so` 的目录即可。

可选准备环境：

```bash
sudo bash ./prepare_main_primary_eid_debug_env.sh \
    --config ./main_primary_eid_debug.wyh.conf
```

该脚本可按配置构建 kernel/UMDK、加载 ko，并写出运行时环境文件。若现场
已经手工替换 ko 和 so，可以不运行它。

## 脚本说明

### 1. urma_admin 命令行测试

脚本：

```text
run_urma_admin_main_ue_eid_cli_test.sh
```

目的：

验证 `urma_admin main_ue_eid` 的完整命令行链路，包括 flush、lookup 未命中、
单条 insert、重复 insert 更新、delete、insert_batch，以及批量条目的抽样
lookup。

典型命令：

```bash
sudo bash ./run_urma_admin_main_ue_eid_cli_test.sh
```

如果 `urma_admin` 不在 `PATH` 中，可以指定二进制路径：

```bash
sudo bash ./run_urma_admin_main_ue_eid_cli_test.sh \
    --admin /root/umdk1/umdk/src/build/urma/tools/urma_admin/urma_admin
```

覆盖一个 `main_ue_eid` 下辖总量超过 128 个 EID 的场景：

```bash
sudo bash ./run_urma_admin_main_ue_eid_cli_test.sh \
    --batch-num 256 \
    --batch-chunk 16
```

说明：

- `--batch-num` 是总插入 EID 数。
- `--batch-chunk` 是每次 `insert_batch` 命令携带的 EID 数。当前脚本默认
  限制为不超过 16，避免 `urma_admin` 命令行长度限制影响测试。
- 单次 `insert_batch` 超过 128 个 EID 是非法参数场景；当前用户态和内核态
  都会按最大 128 个 EID 进行拦截。

### 2. 直接接口测试

脚本：

```text
run_main_primary_eid_debug_test.sh
```

目的：

验证新增 UVS 接口的基本语义，包括 insert、lookup、update、delete、
flush，以及批量插入相关路径。

典型命令：

```bash
sudo bash ./run_main_primary_eid_debug_test.sh \
    --config ./main_primary_eid_debug.wyh.conf \
    --flush-first \
    --full-api
```

说明：

- `--flush-first`：测试前先清空 main_ue_eid 表。
- `--full-api`：额外测试 delete、flush 等会清空全表的接口。

### 3. 大 topo 并发一致性测试

脚本：

```text
run_main_primary_eid_concurrency_test.sh
```

目的：

构造大 synthetic topo，通过 `uvs_set_topo_info()` 下发 topo 和
`eid -> main_ue_eid` 查询表，然后用旧 topo 计算语义生成 expected 结果，
再并发调用 `uvs_lookup_main_ue_eid()` 校验查询结果完全一致。

典型命令：

```bash
sudo bash ./run_main_primary_eid_concurrency_test.sh \
    --config ./main_primary_eid_debug.wyh.conf \
    --large-synthetic-topo \
    --primary-eid 0000:0000:0000:0040:0010:0000:dfdf:004b \
    --flush-first \
    --node-id-base 1000000 \
    --lookup-threads 16 \
    --lookup-loop 20 \
    --api-threads 0 \
    --shuffle \
    --require-eids 280000 \
    --warn-eids 280000
```

说明：

- `--primary-eid` 必须填写当前机器真实存在的 primary eid，current node
  在 set_topo 时会被内核校验。
- `--large-synthetic-topo` 会构造大规模 fake non-current nodes。
- `--node-id-base` 用于避免 fake node id 和已有 topo 冲突。
- `--api-threads 0` 表示只测 set_topo 下发表和 lookup 一致性，不跑直接
  API 压测。
- `--require-eids` 是测试有效性门槛，防止样本规模不足时误判通过。

依赖：

```text
run_main_primary_eid_set_topo.sh
main_primary_eid_set_topo.c
```

这两个文件用于构造并下发 synthetic topo。

### 4. ftrace 耗时测试

脚本：

```text
run_main_primary_eid_ftrace_stress.sh
```

目的：

配置 function_graph ftrace，循环触发 main_ue_eid 查询路径，用于观察内核
函数耗时。

推荐命令：

```bash
sudo bash ./run_main_primary_eid_ftrace_stress.sh \
    --config ./main_primary_eid_debug.wyh.conf \
    --trace \
    --trace-func ubcore_lookup_main_ue_eid \
    --trace-out /root/lwj/trace_exp_lookup_loop20.log \
    --mode lookup \
    --loop 20 \
    --print-every 1
```

说明：

- `--mode lookup` 会读取当前 topo，先刷新查询表，然后循环调用
  `uvs_lookup_main_ue_eid()`，比反复 set_topo 更直接。
- `--trace-func` 可以指定要追踪的内核函数。
- 如果指定函数不可被 ftrace 追踪，可先查看可用函数：

```bash
sudo grep -E 'ubcore_.*(main.*ue|main.*primary|eid.*trie.*lookup|lookup.*eid)' \
    /sys/kernel/debug/tracing/available_filter_functions
```

然后把 `--trace-func` 换成可用函数，例如：

```text
ubcore_cmd_lookup_main_ue_eid
ubcore_eid_trie_lookup
```

### 5. 一键 smoke 测试

脚本：

```text
run_main_ue_eid_smoke_suite.sh
```

目的：

串行执行 urma_admin 命令行测试、直接接口测试和大 topo 并发一致性测试。

典型命令：

```bash
sudo bash ./run_main_ue_eid_smoke_suite.sh \
    --config ./main_primary_eid_debug.wyh.conf \
    --primary-eid 0000:0000:0000:0040:0010:0000:dfdf:004b
```

可按需跳过部分测试：

```bash
sudo bash ./run_main_ue_eid_smoke_suite.sh --skip-admin-test
sudo bash ./run_main_ue_eid_smoke_suite.sh --skip-debug-test
sudo bash ./run_main_ue_eid_smoke_suite.sh --skip-concurrency
```

也可以在一键测试中指定 admin 批量规模：

```bash
sudo bash ./run_main_ue_eid_smoke_suite.sh \
    --admin-batch-num 256 \
    --admin-batch-chunk 16 \
    --primary-eid 0000:0000:0000:0040:0010:0000:dfdf:004b
```
