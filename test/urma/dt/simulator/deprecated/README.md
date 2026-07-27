# deprecated/

本目录存放已停止维护的 urma_sim 旧实现，**不参与编译**（[CMakeLists.txt](../CMakeLists.txt) 未含本目录）。仅保留供历史参考。

## urma_sim_provider_mode3.c

这是一份**"sim 自身注册为 URMA provider"**的完整实现。它把仿真 .so 的 constructor 里调 `urma_register_provider_ops(&g_sim_provider_ops)`（`.name = "udma"`，与真 udma provider 同名），利用 `urma_match_device` 取第一个匹配的特性——sim 先注册在前，所以设备由 sim provider 接管，真 udma provider 代码不执行。

### 代码结构

文件定义了三个对象：

- **`g_sim_ops`**（`urma_ops_t`）：约 60 个 ops 的完整函数指针表。其中只有 4 个有真实实现，其余由 `SIM_TODO_OK` / `SIM_TODO_NULL` 宏生成桩（返回 `URMA_SUCCESS` 或 `NULL`，注释 `TODO`）。
- **`g_sim_provider_ops`**（`urma_provider_ops_t`）：provider 注册表，`init`/`uninit`/`query_device`/`create_context`/`delete_context` 指向本文件实现。
- **`urma_sim_provider_ctor`**（`__attribute__((constructor))`）：`urma_sim_init()` 后读 `URMA_SIM_HW`，为 `'1'` 时调 `urma_sim_hw_start()`（即现行仿真硬件路径，不注册 provider）；否则调 `urma_register_provider_ops(&g_sim_provider_ops)` 走本文件的 provider 路径。

### 真实实现的部分

provider 生命周期：

- `sim_provider_init`：为每台 sim 设备 mmap 一块 `URMA_SIM_MEM_SIZE`(16MB) 匿名内存，bump 分配。
- `sim_query_device`：硬编码一份设备能力（max_jfc=64/max_jfs=1024/max_jfr=1024/max_jetty=4096/各 depth=16384/trans_mode=7/port_cnt=1）兜底，不读 config。
- `sim_create_context`：calloc 一个 `urma_sim_ctx_t`（首字段为 `urma_context_t`，可被库直接当 `urma_context_t*` 用），`ctx->ops = &g_sim_ops`，`dev_idx=0`（注释 TODO：按 dev->name 选下标，实际不选）。
- `sim_delete_context`：free sim ctx。

内存与段（Segment）：

- `sim_register_seg`：在 `g_sim_mem[dev_idx]` 里 bump 分配，返回 `urma_sim_tseg_t`，`ubva.va`/`mva` 直接用用户态指针。
- `sim_import_seg`：单进程仿真，import 直接复用对端 seg 的地址（同一进程内存）。
- token：`sim_alloc_token_id` / `sim_free_token_id`：桩，token_id 写死 `0xabc`。

数据面 `sim_post_jfs_wr`：不经 doorbell / WQE / CQE，直接按 `wr->opcode` 分支：

- `URMA_OPC_READ` / `URMA_OPC_WRITE`：从 `wr->rw.src` / `wr->rw.dst` 取首 sge 的 addr/len，直接 `memcpy`。
- `URMA_OPC_SEND`：返回 SUCCESS 但不搬数据（注释 TODO）。
- 其他：返回 `URMA_FAIL`，`*bad_wr = wr`。

### 为什么停止维护

这份实现能跑通的只有"设备枚举 + register_seg + 单边 READ/WRITE memcpy"，且只在单进程内有意义。它没有 URMA 的队列/完成/doorbell 语义：

- 建链 ops（`sim_create_jfc` / `sim_create_jfs` / `sim_create_jfr` / `sim_create_jetty` / `sim_import_jetty` / `sim_import_jetty_ex` / `sim_create_jfce` / `sim_create_jetty_grp`）全是 `SIM_TODO_NULL` 返回 NULL，`urma_create_*` 会失败，无法完成完整建链。
- `sim_poll_jfc` / `sim_wait_jfc` / `sim_rearm_jfc` 是 `SIM_TODO_OK` 返回 0，拿不到任何 CQE。
- `sim_post_jfr_wr` / `sim_post_jetty_send_wr` / `sim_post_jetty_recv_wr` 是桩，双边 send/recv 跑不通。

端到端建链/post/poll 全链路跑不通，因此停止维护。现行仿真改走"保留真 udma provider 代码照跑、sim 只在 mmap/ioctl/ummu 层拦截 + 后台轮询造 CQE"的路径（[urma_sim_hw.c](../urma_sim_hw.c) + [urma_sim_dev.c](../urma_sim_dev.c) + [urma_sim_ipc.c](../urma_sim_ipc.c) + [urma_sim_ummu.c](../urma_sim_ummu.c)），有完整队列/完成/doorbell 语义，READ/WRITE/SEND 单进程与跨进程均跑通。详见上层 [README.md](../README.md)。

### 如何重启（如需）

1. 把 `urma_sim_provider_mode3.c` 复制回上层目录、改名覆盖 `urma_sim_provider.c`（现 [urma_sim_provider.c](../urma_sim_provider.c) 已精简为只剩 constructor，本文件含完整 provider ops）。
2. 当前 constructor 在 `URMA_SIM_HW` 未设时不再注册 provider；要重启需让 else 分支调 `urma_register_provider_ops(&g_sim_provider_ops)`。
3. 本文件依赖 `g_urma_sim` / `urma_sim_init`（[urma_sim.h](../urma_sim.h) / [urma_sim_config.c](../urma_sim_config.c)）、`urma_sim_hw_start`（[urma_sim_hw.c](../urma_sim_hw.c)），头与库都已存在。
4. 建链桩仍是 TODO，端到端跑不通，需补齐 ops 才能用。

### 相关符号对照

| 文件内符号 | 类型 | 状态 |
|------------|------|------|
| `urma_sim_ctx_t` / `urma_sim_mem_t` / `urma_sim_tseg_t` | 内部数据结构 | 旧实现专用 |
| `sim_provider_init` / `uninit` / `query_device` / `create_context` / `delete_context` | provider 生命周期 | 有实现 |
| `sim_register_seg` / `unregister_seg` / `import_seg` / `unimport_seg` | Segment 管理 | 有实现 |
| `sim_alloc_token_id` / `free_token_id` | token | 桩（写死 0xabc） |
| `sim_post_jfs_wr` | 数据面 | 有实现（直接 memcpy，无 WQE/CQE） |
| `sim_create_jfc`/`_jfs`/`_jfr`/`_jetty`/`_jfce`/`_jetty_grp`、`sim_import_jetty`/`_ex`、`sim_poll_jfc` 等 | 建链/完成/双边 | `SIM_TODO_NULL`/`SIM_TODO_OK` 桩 |
| `g_sim_ops` | `urma_ops_t` | 完整指针表 |
| `g_sim_provider_ops` | `urma_provider_ops_t` | 注册表 |
