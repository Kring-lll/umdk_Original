# urma_sim：URMA 设备用户态仿真

`liburma_sim.so` 经 `LD_PRELOAD` 拦截 libc 系统调用，在用户态虚拟出 `/sys/class/ubcore/<dev>` 设备树与 `/dev/uburma/<dev>` 字符设备，并模拟硬件数据面行为。无需内核 `ubcore` 模块、无需真实 URMA 硬件，即可在容器里跑 `urma_admin` 设备枚举与 `liburma` 应用程序的建链/post/poll 全链路。

> 本文讲功能与用法。**实现机制**（拦截底层、open/mmap/ioctl/doorbell/WQE/IPC 怎么做到的）见 [SIM_MECHANISM.md](SIM_MECHANISM.md)；**设计依据**见 [PLAN.md](PLAN.md)。

## 仿真什么

URMA 的设备发现与数据面依赖两类内核接口：

- **sysfs**：`/sys/class/ubcore/<dev>/` 下的属性文件（驱动名、能力上限、eid、端口等），由 `urma_read_sysfs_device` / `urma_parse_device_attr` 读取。
- **字符设备 ioctl + mmap**：`/dev/uburma/<dev>` 的 `ioctl(URMA_CMD, ...)` 提交建链/查询命令，`mmap(dev_fd, offset)` 映射 doorbell / queue / CQ 内存。

容器里 sysfs 只读、无 `ubcore` 内核模块，这两者都不存在。`liburma_sim.so` 在用户态把整套链路接管：sysfs 路径拦截返回内存里的虚拟值，ioctl 命令按配置/规则回填，mmap 返回匿名内存并由后台轮询线程模拟硬件产出 CQE。

## 数据来源

[urma_sim_config.json](urma_sim_config.json) 定义虚拟设备，由 [urma_sim_config.c](urma_sim_config.c) 内置的手写 JSON 解析器加载（`parse_config` → `urma_sim_init`），存进全局 `g_urma_sim.devices[]`（`urma_sim_state_t`，最多 16 台）。每台设备含：

- `sysfs[128]`：文件相对路径 → 字符串值（`urma_sim_kv_t`）。
- `ioctl_rules[32]`：命令的"输入→输出"表单规则（`urma_sim_ioctl_rule_t`）。

环境变量 `URMA_SIM_CONFIG` 指向自定义 json 即可换设备/换属性，无需重编。

## 运行模式

由环境变量 `URMA_SIM_HW` 控制，在 [urma_sim_provider.c](urma_sim_provider.c) 的 constructor 里读取：

- **`URMA_SIM_HW=1`（默认运行模式）**：sim 不注册 provider，真 udma provider 接管设备；sim 起后台轮询线程 `urma_sim_hw_start()`，在 mmap/ioctl/ummu/系统调用层拦截 + 解 WQE + 搬数据 + 造 CQE。post 数据面可用。
- **不设 `URMA_SIM_HW`**：sim 同样不注册 provider，真 udma 接管但无真实硬件 → `create_context` 会失败。此时仅设备枚举（`urma_admin show`，走 sysfs 拦截）可用，建链/post 不可用。

> 早期还有一个"sim 自身注册为 provider"的实现，现已停止维护，源码移至 [deprecated/](deprecated/)，不参与编译。
>
> 现行 sim 是**纯用户态**：ioctl/mmap/ummu 都在用户态拦截 + 回填/造假页，内核态那层由 sim 用户态冒名顶替（详见"架构"）。"真仿内核态"（kmod 在内核注册 cdev + ioctl handler，建链命令真进内核）是**未来实现**，源码已编通并验证核心前提（VM 内核 init_module 是真 syscall），但因当前 TCG 环境跑不动完整 VM 而暂未上线，见 [future/kmod/](future/kmod/DESIGN.md)。

## 构成

### 拦截层（[urma_sim.c](urma_sim.c)）

LD_PRELOAD 拦截下列 libc 符号，全部按带版本号导出（见 [urma_sim.version](urma_sim.version)）。`stat` 在新 glibc 是 `@GLIBC_2.33`，`__open_2` 是 FORTIFY 变体，这两个版本必须精确，否则 `show` 设备列表会空。

| 调用 | 导出版本 | 仿真行为 |
|------|----------|----------|
| opendir | `@GLIBC_2.17` | `/sys/class/ubcore`、`/dev/uburma` → 真 opendir 临时目录（constructor 已造好，含设备名条目） |
| readdir | `@GLIBC_2.17` | 走 libc 原样（临时目录里就是设备名） |
| stat | `@GLIBC_2.33` | sim 设备路径 → S_ISDIR/S_IFREG + 固定 mtime |
| realpath | `@GLIBC_2.17` | sim 路径 → 返回原路径 |
| open / `__open_2` | `@GLIBC_2.17` | sysfs 文件 + `/dev/uburma/<dev>` → 假 fd（≥100000，进 `urma_sim_fd` 表） |
| read | `@GLIBC_2.17` | 假 fd → 查 fd 表返回对应 sysfs 值 |
| ioctl | `@GLIBC_2.17` | 假 cdev fd + `URMA_CMD` → 交 `urma_sim_handle_ioctl` 分发 |
| access | `@GLIBC_2.17` | build 路径下 `*/urma/*.so` 重定向到 `/usr/lib64/urma`，让 `urma_open_drivers` 能 dlopen 真 udma provider |
| mmap / munmap | `@GLIBC_2.17` | cdev fd 的 mmap → 由 `urma_sim_mmap_alloc` 给匿名内存并登记 region |

constructor `urma_sim_ctor` 还做两件事：`urma_sim_init()` 加载 config + 在 `/tmp` 造临时设备目录（供 opendir/readdir）；若 `URMA_SIM_STDERR_LOG=1` 则注册 `urma_sim_stderr_log` 把 liburma 日志引到 stderr（urma 默认走 syslog，容器里看不到）。

### ioctl 命令分发（[urma_sim_dev.c](urma_sim_dev.c)）

`urma_sim_handle_ioctl(fd, command, args_len, args_addr)` 解析 `urma_cmd_hdr_t`，按 command 分发：

- **QUERY_DEV_ATTR（40）**：`handle_query_dev_attr` 按 `g_query_dev_attr_map`（attr type → sysfs 文件名）用 sysfs 值回填设备能力 attr；其中 `priority_info` 按结构体数组回填。
- **CREATE_CTX（1）**：`handle_create_ctx` 回填 async_fd、`resp.cqe_size=64`、关所有可选特性；并读本设备 `eids/eid0` → `urma_str_to_eid` → `urma_sim_ipc_start` 自动起本进程 IPC（见下）。
- **建链命令（CREATE_JFC=16 / CREATE_JFS / CREATE_JETTY=22 / CREATE_JFR / IMPORT_JETTY=26 / REGISTER_SEG 等）**：`handle_link_cmd` 从 IN attr 取 jfc_id / depth / max_sge / tp_type 等回填到 OUT，并把 queue/CQ/JFR 关联登记到 [urma_sim_hw.c](urma_sim_hw.c) 的关联表。
- **表单规则**：`find_ioctl_rule` + `apply_rule_out` 按 config 的 `ioctl_rules` 匹配 IN attr、回填 OUT attr，优先级高于 sysfs fallback。

### 仿真硬件数据面（[urma_sim_hw.c](urma_sim_hw.c) + [urma_sim_ummu.c](urma_sim_ummu.c) + [urma_sim_ipc.c](urma_sim_ipc.c)）

`URMA_SIM_HW=1` 时，真 udma provider 的 WQE/doorbell/poll 代码原样执行，sim 充当"硬件"。真 udma 与硬件的全部交互只经两个渠道：`ioctl(/dev/uburma)` + `mmap(dev_fd, offset)`，sim 拦这两个 + ummu 库函数即可让真 udma 在无硬件容器里跑通。

**关联网**：真 udma 在 create 命令时把 queue/doorbell/CQ 的地址和关联经 ioctl 的 udata 传给内核，sim 拦 ioctl 从 udata 全部拿到，登记在下列表（`urma_sim_hw.c`）：

- `urma_sim_queue_assoc_register`：CREATE_JFS/JFR 时从 IN_UDATA 读 `jetty_addr` / `buf_addr`，建 queue 关联（区分 sq `q_type=0` 与 rq `q_type=1`）。
- `urma_sim_queue_assoc_set_db_by_idx`：拦 mmap JETTY_DSQE 时用 offset 反解的 `idx`(=db.id) 反查 queue 关联，回填 `db_addr`。
- `urma_sim_cq_assoc_register` / `urma_sim_cq_assoc_find`：CREATE_JFC 时从 udata 读 `buf_addr`/`buf_len`，建 jfc_id → cq_addr + depth（`depth=buf_len/64`）。
- `urma_sim_jfr_bind_jetty` / `urma_sim_jfr_find_by_jetty`：CREATE_JETTY 的 IN_JFR_ID 建 jetty_id → jfr_id 映射，SEND 时按 jetty_id 找对端 JFR 的 rq。
- `urma_sim_tid_seg_register`：拦 `ummu_grant`（[urma_sim_ummu.c](urma_sim_ummu.c)）登记 tid → va/len。

**轮询线程** `urma_sim_hw_thread`（`urma_sim_hw_start` 起）：

1. `process_queue` 扫各 queue_assoc，读 `db_addr + UDMA_DOORBELL_OFFSET(0x80)` 的 pi 与 `pi_seen` 比较发现新 WQE。
2. `process_wqe` 用镜像位域结构 `struct urma_sim_sqe_ctl`（`_Static_assert` 校 sizeof=48、opcode@5、rmt_jetty@12、rmt_addr_l@40）读 opcode / rmt_addr / 首 sge(va,len)，sge 在 `sqe_ctl + 48`。
3. 按 opcode 分支：
   - READ(6)：`memcpy(本端va, 远端va, len)`。
   - WRITE(3)：反向 memcpy。
   - SEND(0)：按 `rmt_jetty_id` 经 IPC（跨进程）或本进程查表找到对端 JFR recv 槽 → `urma_sim_jfr_deliver_send` 把数据 memcpy 进 recv buf 的 va → `produce_recv_cqe` 造对端 recv CQE（`s_r=1`、`is_jetty=1`、`entry_idx=rqe_idx`、`opcode=HW_CQE_OPC_SEND`），再 `produce_cqe` 造发送方 send CQE。
   - 其他 opcode：走 default 只造成功 CQE 不搬数据（见"未实现"）。
4. `produce_cqe` 往 `cq_addr + (cq_ci & baseblk_mask)*64` 写 `struct urma_sim_jfc_cqe`（`_Static_assert` 校 sizeof=64）：`owner = valid_owner^1`（让真 udma `get_next_cqe` 的 `owner^valid_owner!=0` 成立）、`entry_idx=wqe_pi`、`user_data_l/h=jetty_addr`、`local_num_l/h=jetty_id`、`status=0`、`s_r=0`。CQ 参数：`baseblk_shift=6`（cqe_size=64），`cq_shift=align_power2(baseblk_cnt)`，`baseblk_cnt=depth 向上 2 幂`。

**JFR recv 槽跟踪**：对 `q_type=1` 的 assoc，`jfr_refill_pending` 扫 sw_db（record db，pi 变化），读索引队列 `idx_addr + head*4` 拿 `wqe_idx`，读 recv WQE `buf_addr + (wqe_idx<<wqe_shift)` 拿首 sge.va 存进 pending 队列。`urma_sim_jfr_set_recv_geom` 从 IN attrs 按 `field_size` 读 max_sge/depth 算 `wqe_shift`/`wqe_cnt`。

**跨进程 IPC**（[urma_sim_ipc.c](urma_sim_ipc.c)）：单进程仿真里远端地址直接在 WQE 的 `rmt_addr`（import_seg 复用真实 va）。真实双进程时 A 解引用 B 的指针会 SIGSEGV，故用 unix socket 实时传数据，零共享存储：

- `urma_sim_ipc_start(eid)`：create_context 后自动调用（`handle_create_ctx` 里），起后台线程 listen `/tmp/urma_sim_ipc_<eid前8字节hex(swap后)>>`，维护本进程私有 tid→buf 表。
- 寻址依据 WQE 自带的对端信息：`rmt_eid`（对端 EID，import_jetty 链路天然携带）→ 推算对端 socket 路径定位**对端进程**；`rmt_tid` → 在对端进程内查对端自己的 tid→buf 表定位**对端内存**；`rmt_addr`（对端指针）跨进程无意义，忽略。
- `urma_sim_ipc_is_local(rmt_eid)`：与本进程 `g_local_eid`（存 swap 后的 le_eid）比较；未起 IPC 时全 0 → 一律返回 1（走 memcpy，兼容单进程）。
- `urma_sim_ipc_read` / `_write` / `_send`：发请求到对端 socket，对端后台线程查本进程表拿 buf 指针后回传数据。

## 构建

urma_sim 的 `open` 拦截会被 libasan（ASan 二进制必须 preload libasan）抢先，因此被测程序须用非 ASan 版本。

```bash
# 1. 编非 ASan 的 urma_admin / liburma
cd /umdk/src && rm -rf build_noasan && mkdir build_noasan && cd build_noasan
cmake .. -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable \
         -DGTEST=disable -DASAN=disable -DCODE_COVERAGE=disable
cmake --build . -j
# 产物：/umdk/src/build_noasan/urma/...  下的 urma_admin、liburma.so、liburma_common.so

# 2. 编 liburma_sim.so
cd /umdk/sim/urma && mkdir -p build && cd build
cmake .. && cmake --build . -j
# 产物：/umdk/sim/urma/build/liburma_sim.so

# 或一键脚本
bash /umdk/sim/urma/urma_sim_setup.sh
```

### 3. 单元测试（UT，可选）

仿 [umdk/test/urma/ut](../../test/urma/ut/) 风格，测 sim 纯函数（链接 liburma_sim.so 调真导出函数）。默认 OFF，不编 UT 不影响 sim 正常构建。

```bash
cd /umdk/sim/urma/build
cmake -DBUILD_SIM_UT=ON ..
cmake --build .          # 产出 test/urma_sim_mmap_decode_ut（链接 liburma_sim.so）
ctest                    # 跑所有 UT（1 个用例，46 个断言）
# 或直接跑：./test/urma_sim_mmap_decode_ut
```

当前 UT：[test/test_mmap_decode_ut.c](test/test_mmap_decode_ut.c) 测 `urma_sim_mmap_decode_offset`（mmap offset 反解 cmd/idx 的纯函数，7 个 case 覆盖往返/边界/截断/零值）。

## 运行

```bash
# 一键示例：urma_admin show 设备枚举
bash /umdk/sim/urma/examples/run_urma_admin.sh

# 手动
export LD_LIBRARY_PATH=/umdk/src/build_noasan/urma/lib/urma/core:/umdk/src/build_noasan/urma/common:$LD_LIBRARY_PATH
export LD_PRELOAD=/umdk/sim/urma/build/liburma_sim.so
export URMA_SIM_CONFIG=/umdk/sim/urma/urma_sim_config.json
export URMA_SIM_HW=1   # 跑 post 必须

urma_admin show -d udma_sim0   # 设备名校验
urma_admin show                # 设备列表
```

## 配置 config.json

[urma_sim_config.json](urma_sim_config.json) 结构：

```json
{
  "devices": [
    {
      "name": "udma_sim0",
      "sysfs": {
        "driver_name": "udma",     // 必须 "udma"，匹配 udma provider 的 ops->name，否则设备被丢弃
        "transport_type": "0",
        "max_jfc": "64",
        "max_jfs_sge": "8",
        "eids/eid0": "192.168.10.1",
        "port0/state": "1"
      },
      "ioctl": [
        { "command": 40, "match": [], "out": [ { "key": "130", "value": "999" } ] }
      ]
    }
  ]
}
```

**sysfs**：`urma_read_sysfs_device` + `urma_parse_device_attr` 读的所有文件。完整字段见 json 与 [urma_sim_config.c](urma_sim_config.c) 的 `urma_parse_device_attr`。必填项：

| 字段 | 值 | 说明 |
|------|-----|------|
| `driver_name` | `"udma"` | 匹配 udma provider 的 `ops->name`（[udma_u_ops.c:371](../../src/urma/hw/udma/udma_u_ops.c#L371)），否则 `urma_match_device` 不匹配，设备被丢弃 |
| `ubdev` | 设备名 | 一般与 `name` 一致 |
| `transport_type` | `"0"` | 0 = UB 传输 |
| `max_eid_cnt` | 如 `"4"` | 决定读多少个 `eids/eidN` |

设备能力字段（`max_jfc`/`max_jfs`/`max_jfr`/`max_jetty*`/`max_*_depth`/`max_*_sge`/`max_msg_size`/`max_read_size`/`max_cas_size`/`trans_mode`/`ceq_cnt`/`port_count`/`page_size_cap` 等）、端口字段（`port0/max_mtu`/`port0/state`/`port0/active_*`）、网络字段（`net_dev`/`eids/eidN`/`priority/priorityN/*`）全部走 sysfs 拦截，按 json key 填即可。

**ioctl 表单**：`ioctl_rules` 定义命令的输入→输出规则。

```json
"ioctl": [
  {
    "command": 40,                        // 命令号（urma_cmd 枚举值）
    "match": [ { "key": "<IN attr type 十进制>", "value": "<期望值 十进制>" } ],  // 全满足才命中，空=无条件
    "out":   [ { "key": "<OUT attr type 十进制>", "value": "<回填值 十进制>" } ]   // 命中后回填
  }
]
```

- `command`：常用 `40`=QUERY_DEV_ATTR、`26`=IMPORT_JETTY、`16`=CREATE_JFC、`22`=CREATE_JETTY、`1`=CREATE_CTX。
- `match`：读 IN attr（type < 0x80）与 `value` 比较，全满足才命中；空 match = 无条件命中。多条规则按顺序取第一条命中的。
- `out`：命中的 OUT attr（type ≥ 0x80）按 `value` 回填，按 attr 的 `field_size` 写 u8/u16/u32/u64。
- 表单优先于 sysfs fallback：未在 `out` 列出的 attr 仍走 `g_query_dev_attr_map` 用 sysfs 值回填。

attr type = `URMA_CMD_OUT_TYPE_INIT`(0x80=128) + 枚举偏移。用 `gcc -I<urma头> -E` 或小程序 `printf("%d", ENUM)` 取值最准。示例：`printf("%d", QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC)` → 130。

表单作用于走 URMA_CMD ioctl 的命令（`urma_query_device` / `urma_create_jfc` / `urma_import_jetty` 等 API 经 ioctl 提交）。`urma_admin show` 走 sysfs 不走 ioctl，改 sysfs 值即可。

## 能力边界

### 已工作 ✅（`URMA_SIM_HW=1`）

设备发现层（sysfs 拦截）：
- 设备枚举：`opendir("/sys/class/ubcore")` 返回虚拟设备名。
- 设备名校验：`urma_admin show -d <name>` 不报 invalid。
- sysfs 属性读：`driver_name` / `max_jfc` / `eids/eid0` / `priority/...` 全套可读。
- `URMA_CMD_QUERY_DEV_ATTR` ioctl：按 config/sysfs 回填设备能力（含 priority_info 结构体数组）。
- `urma_admin show` / `show -a` / `show -d <dev>` 列出仿真设备。

数据面（真 udma 跑 + sim 拦截/轮询）：
- 建链全通：`create_context` / `create_jfc` / `create_jfr` / `create_jetty` / `register_seg` / `alloc_token_id` / `import_jetty(_ex)` / `import_seg`。
- READ/WRITE/SEND 端到端（单进程 + 跨进程经 unix-socket IPC）：
  - [test_hw_post.c](examples/test_hw_post.c) 单进程 READ：TEST PASS。
  - [test_ipc_2proc.c](examples/test_ipc_2proc.c) 跨进程 READ：TEST PASS。
  - [test_ipc_send.c](examples/test_ipc_send.c) 跨进程 SEND：TEST PASS（A 收 recv CQE `s_r=1 opcode=0`，B 收 send CQE `s_r=0`）。
  - [test_ipc_send_imm.c](examples/test_ipc_send_imm.c) 跨进程 SEND_WITH_IMM：TEST PASS（A poll recv CQE `opcode=1 imm=0xdeadbeef`——验证 imm 从 WQE→IPC→对端 recv CQE data_l/h 完整回填）。
  - [test_hw_write_imm.c](examples/test_hw_write_imm.c) 单进程 WRITE_WITH_IMM：TEST PASS（验 ctl_len=64 变长、SGE@64、imm@48、数据搬运写对 seg + send CQE；imm 通知 CQE 投递 TODO）。
  - [test_hw_atomic.c](examples/test_hw_atomic.c) 单进程 CAS/FAA 原子：TEST PASS（CAS 匹配 swap + 原值读回、CAS 不匹配不变 + 原值读回、FAA +=operand + 原值读回；验 WQE 占 2 WQEBB、原子数据@64、读-改-写远端值）。
  - [test_ipc_multi_send.c](examples/test_ipc_multi_send.c) 多进程 SEND（1 server + 3 client 并发）：验证 IPC 按 eid 任意互联（不写死双边），3 client 各持不同设备 eid 并发 SEND，server 收齐 3 个 recv CQE 且按 user_ctx 区分来源。容器实跑待验证。
  - intergration_test 框架的 [send_cr](../../test/intergration_test/test_suites/URMA/test_urma_post_jetty_wr_send_cr)（UM 3×SEND+recv）与 [write_cr](../../test/intergration_test/test_suites/URMA/test_urma_post_jetty_wr_write_cr)（UM 3×WRITE）双进程跑通。
- CREATE_CTX 自动起本进程 IPC（读设备 `eids/eid0` → `urma_sim_ipc_start`），跨进程 `urma_sim_ipc_is_local` 判定正确。

### 未实现 ⚠️

- **opcode**：已实现 SEND(0)/WRITE(3)/READ(6) 的数据搬运。**A 类 IMM 变体**：SEND_WITH_IMM(1) imm 闭环完整验证通过（imm 从 WQE 偏移40 取 → 跨进程 IPC 协议头扩展传 → 回填对端 recv CQE data_l/h + opcode，test_ipc_send_imm PASS）。WRITE_WITH_IMM(4)/WRITE_WITH_NOTIFY(5)：WQE 头长度随 opcode 变（get_ctl_len：WRITE=48/IMM=64/NOTIFY=80），process_wqe 按 opcode 算 ctl_len 取首 SGE + imm/notify 偏移；数据搬运复用 WRITE 路径（test_hw_write_imm PASS：验 seg_buf 写对 + send CQE + ctl_len 变长逻辑）；imm 通知 CQE 投递 TODO（WRITE WQE 只带 seg tid+rmt_eid 不带 jetty id，无法路由对端 JFR 投 imm 通知）。SEND_WITH_INVALID(2) case 实现（token 走 data_l）。NOP(0x11) 显式 case。**B 类原子 CAS(7)/FAA(0xb) 单进程已实现并验证**（test_hw_atomic PASS：CAS 匹配 swap 生效 + 原值读回、CAS 不匹配不变 + 原值读回、FAA +=operand + 原值读回）。原子数据在 WQE 偏移64（SQE_ATOMIC_DATA_FIELD，CAS: swap+cmp 各 len 字节、FAA: operand len 字节），单进程用 rmt_addr 读-改-写远端值；**跨进程原子 TODO**（需 IPC OP_ATOMIC 读远端值+条件写+回原值）。CAS/FAA WQE 占 2 WQEBB（UDMA_ATOMIC_WQE_BB_NUM=2），process_queue 按 opcode 算 step（2 vs 1）步进 pi_seen。WRITE_WITH_ATOMICSTORE_ADD(0x19) TODO（复合操作，字段布局复杂）。
- **多 CQE 同步**：sim 的 `cq_ci` 独立递增，未与真 udma 的 `cq.ci` 同步；浅 depth（1-3）不暴露，深队列可能 CQE 覆盖。~~`process_queue` 的 `pi` 也无 mask 回绕，post 数超 depth 会越界~~（已修复：process_queue 用 `pi & (wqe_cnt-1)` 环形回绕，register 时 sq 从 buf_len 算 wqe_cnt）。
- **现成测试仍卡**：perftest `read_lat` 卡 post 前 segfault（疑 TP/import_jetty_ex 交互）；urma_sample 卡 1GB `memset`（qemu-user 固有慢，非 sim 问题）。

### 环境限制

- ASan 二进制不兼容：libasan 会抢走 `open` 拦截，必须用非 ASan 程序。
- netlink 管理命令（`urma_admin dev set/expose`、`eid add/del`）走 generic netlink 未拦（容器无 ubcore 内核 family）。`show` 不受影响（走 sysfs）。

## 异常注入（错误 CQE 测试）

sim 作为可控假依赖，支持注入 error CQE 验真 udma 错误转换链。由环境变量 `URMA_SIM_INJECT_STATUS` 控制（constructor 读，produce_cqe 命中造 status≠0 的 CQE）：

| URMA_SIM_INJECT_STATUS | CQE status（udma_abi.h）| cr.status（urma_opcode.h）| 语义 |
|---|---|---|---|
| 0（默认）| SUCCESS=0 | URMA_CR_SUCCESS | 正常，不注入 |
| 1 | LOCAL_OP_ERR（substatus=0）| URMA_CR_UNSUPPORTED_OPCODE_ERR | 不支持操作码 |
| 4 | TRANSACTION_RETRY_COUNTER_ERR | URMA_CR_RNR_RETRY_CNT_EXC_ERR | RNR 重试超限 |
| 5 | TRANSACTION_ACK_TIMEOUT_ERR | URMA_CR_ACK_TIMEOUT_ERR | ACK 超时 |

用法：`URMA_SIM_INJECT_STATUS=5 /tmp/test_inject_cqe_err` → post READ，sim 造 error CQE，验 poll 拿到对应 cr 错误码（[test_inject_cqe_err.c](examples/test_inject_cqe_err.c)）。

注：sim process_wqe 先 memcpy 再造 CQE，故 error CQE 也会搬数据（与真硬件 error 时数据可能已部分传输一致）。注入只影响 send 侧 CQE（READ/WRITE/SEND 完成），recv 侧不注入。

## 测试

一键构建 + post 端到端验证：

```bash
bash /umdk/sim/urma/build_and_test_hw.sh   # hw_post + ipc_send
```

或手动编译单进程测试：

```bash
gcc -I/umdk/src/urma/lib/urma/core/include -I/umdk/src/urma/common/include \
    -L/umdk/src/build_noasan/urma/lib/urma/core -lurma \
    -L/umdk/src/build_noasan/urma/common -lurma_common \
    -Wl,-rpath,/umdk/src/build_noasan/urma/lib/urma/core \
    -Wl,-rpath,/umdk/src/build_noasan/urma/common \
    /umdk/sim/urma/examples/test_hw_post.c -o /tmp/test_hw_post

URMA_SIM_HW=1 LD_PRELOAD=/umdk/sim/urma/build/liburma_sim.so \
  URMA_SIM_CONFIG=/umdk/sim/urma/urma_sim_config.json \
  LD_LIBRARY_PATH=/umdk/src/build_noasan/urma/lib/urma/core:/umdk/src/build_noasan/urma/common \
  /tmp/test_hw_post
```

链路（[test_hw_post.c](examples/test_hw_post.c)，单进程 READ）：

```
urma_init → urma_get_device_list → urma_create_context
→ urma_create_jfce → urma_create_jfc → urma_create_jfs(UM)
→ urma_alloc_token_id → urma_import_jetty（伪造 rjetty 自引用 eid）
→ urma_register_seg(src_buf)（ummu_grant 拦截登记 tid→src_va）
→ 构 wr(URMA_OPC_READ, dst=本地 dst_buf, src=远端 src_buf+src_tseg)
→ urma_post_jfs_wr：真 udma 写 WQE 到 sim 假 queue + 写 doorbell pi
→ sim 轮询线程：pi 变化 → 解 WQE → memcpy(dst_buf, src_buf) → 造 CQE
→ urma_poll_jfc：真 udma 读 sim 造的 CQE → cr(status=SUCCESS, user_ctx 回填)
验证：dst_buf 内容 'Z'→'A'，poll 拿到 SUCCESS CQE → TEST PASS
```

调试：segfault 定位用 [examples/segv_handler.c](examples/segv_handler.c)（**链接进被测程序，不能 LD_PRELOAD**，否则与 sim 符号冲突），constructor 装 SIGSEGV handler 打 `si_addr`/`si_code`/backtrace/`/proc/self/maps`，`addr2line -e liburma_sim.so -f <偏移>` 定位行（库须 `-g` 编译，已在 CMakeLists 加 `-g`；-O2 内联致行号可能偏 1-2 行）。

## 文件结构

| 文件 | 作用 |
|------|------|
| [urma_sim.c](urma_sim.c) | LD_PRELOAD 拦截层（opendir/stat/realpath/open/read/ioctl/access/mmap/munmap + constructor 造临时目录 + stderr 日志） |
| [urma_sim_provider.c](urma_sim_provider.c) | 启动入口：constructor（`urma_sim_init` + 按 `URMA_SIM_HW` 起 `urma_sim_hw_start`） |
| [urma_sim_dev.c](urma_sim_dev.c) | ioctl 命令分发：QUERY_DEV_ATTR（含 priority_info 回填）、CREATE_CTX（auto IPC + cqe_size=64）、建链命令回填（jfc_id/depth/max_sge/tp_type）+ queue/CQ/JFR 关联登记 + 表单规则匹配 |
| [urma_sim_hw.c](urma_sim_hw.c) | 仿真硬件核心：mmap 拦截+offset 反解+假内存、queue/CQ/tid/JFR 关联表、轮询线程（解 WQE+memcpy/IPC+造 send/recv CQE）、SEND 投递对端 JFR recv 槽 |
| [urma_sim_hw.h](urma_sim_hw.h) | 仿真硬件内部接口：mmap 关联表、queue_assoc（sq/rq）、cq_assoc、tid_seg、jfr bind/find/deliver |
| [urma_sim_ipc.c](urma_sim_ipc.c) | 跨进程 IPC：后台 unix-socket 线程 + read/write/send 请求-响应 + eid 寻址 |
| [urma_sim_ummu.c](urma_sim_ummu.c) | 拦 ummu 4 函数（grant/ungrant/allocate_tid/free_tid）+ tid→seg 登记 |
| [urma_sim_config.c](urma_sim_config.c) | 加载 config.json（手写 JSON 解析器 + 内置默认配置） |
| [urma_sim.h](urma_sim.h) | 内部接口 + 数据结构（`urma_sim_dev_t`/`urma_sim_state_t`/`urma_sim_fd_t`） |
| [urma_sim.version](urma_sim.version) | 链接器版本脚本（符号按 @@GLIBC_2.17/2.33 + URMA_SIM_UMMU/API 块导出） |
| [urma_sim_config.json](urma_sim_config.json) | 默认设备配置（udma_sim0 + udma_sim1） |
| [CMakeLists.txt](CMakeLists.txt) | 构建（仅编译上述 7 个 .c，deprecated/ 不含） |
| [urma_sim_setup.sh](urma_sim_setup.sh) | 构建 + 运行环境准备 |
| [build_and_test.sh](build_and_test.sh) | 一键构建 + 设备枚举验证 |
| [build_and_test_hw.sh](build_and_test_hw.sh) | 一键构建 + post 端到端验证（hw_post + ipc_send） |
| [examples/run_urma_admin.sh](examples/run_urma_admin.sh) | 运行示例：urma_admin show 枚举设备 |
| [examples/test_hw_post.c](examples/test_hw_post.c) | 单进程 post 端到端测试（READ） |
| [examples/test_ipc_2proc.c](examples/test_ipc_2proc.c) | 跨进程 READ 测试（A/B 各持一设备，经 IPC） |
| [examples/test_ipc_send.c](examples/test_ipc_send.c) | 跨进程 SEND 测试（B SEND→A 的 JFR recv） |
| [examples/test_ipc_send_imm.c](examples/test_ipc_send_imm.c) | 跨进程 SEND_WITH_IMM 测试（验 imm 回填对端 recv CQE） |
| [examples/test_hw_write_imm.c](examples/test_hw_write_imm.c) | 单进程 WRITE_WITH_IMM 测试（验 ctl_len 变长 + 数据搬运） |
| [examples/test_hw_atomic.c](examples/test_hw_atomic.c) | 单进程 CAS/FAA 原子测试（验 WQE 占 2 WQEBB、读-改-写远端值） |
| [examples/test_inject_cqe_err.c](examples/test_inject_cqe_err.c) | 异常测试：注入 error CQE，验真 udma cr 错误转换链 |
| [examples/test_hw_atomic.c](examples/test_hw_atomic.c) | 单进程 CAS/FAA 原子操作测试（验读-改-写 + 原值读回） |
| [examples/test_ipc_multi_send.c](examples/test_ipc_multi_send.c) | 多进程 SEND 测试（1 server + 3 client 并发，验证 eid 任意互联） |
| [examples/run_ipc_multi_send.sh](examples/run_ipc_multi_send.sh) | 多进程 SEND 一键运行（起 4 进程） |
| [examples/segv_handler.c](examples/segv_handler.c) | 调试工具：SIGSEGV handler 打 backtrace + /proc/self/maps |
| [test/](test/) | 单元测试（UT，仿 umdk/test/urma/ut 风格）：测 sim 纯函数。`cmake -DBUILD_SIM_UT=ON` 开启，默认 OFF |
| [deprecated/](deprecated/) | 已停止维护的旧实现（sim 自身注册为 provider），不参与编译 |
| [future/](future/) | 已设计/编通但当前不接入的"未来实现"（内核态 kmod 仿真：方案 A，建链进内核），不参与编译 |

## 参考

- ioctl 命令/attr 协议：[urma_cmd.h](../../src/urma/lib/urma/core/include/urma_cmd.h)、[urma_cmd_tlv.h](../../src/urma/lib/urma/core/urma_cmd_tlv.h)
- 设备发现源码：[urma_device.c](../../src/urma/lib/urma/core/urma_device.c)
- provider 名：[udma_u_ops.c:371](../../src/urma/hw/udma/udma_u_ops.c#L371)
- 设计依据：[PLAN.md](PLAN.md)
