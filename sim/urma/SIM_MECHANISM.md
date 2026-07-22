# urma_sim 实现机制：从 open 到跨进程

这份文档讲 sim **怎么实现**的——拦截机制底层、各部件工作原理。不讲用法（用见 [README.md](README.md)），不讲设计依据（见 [PLAN.md](PLAN.md)），只讲"代码里怎么做到的"。

按一次 post 的调用链顺序：从最前的 `open` 拦截开始，沿建链 → mmap → doorbell → WQE → 轮询 → CQE → 跨进程 IPC 一路往下。

## 0. 拦截机制总览：LD_PRELOAD 怎么盖住 libc

sim 编成 `liburma_sim.so`，用 `LD_PRELOAD=/path/liburma_sim.so` 加载。动态链接器（ld-linux）解析未定义符号时按顺序找：程序自己 → **LD_PRELOAD 库（sim 在此）** → LD_LIBRARY_PATH/默认路径（libc.so.6）。所以 sim 的 `open`/`ioctl`/`mmap` 先被找到，libc 同名函数被遮蔽。

**版本符号是关键**：glibc 符号带版本（`open@GLIBC_2.17`），liburma 引用的也是带版本的。ld-linux 只匹配同版本符号定义，所以 sim 必须按精确版本导出——见 [urma_sim.version](urma_sim.version)：

```
GLIBC_2.17 { global: open; __open_2; close; opendir; readdir; realpath;
             read; ioctl; access; mmap; munmap; };
GLIBC_2.33 { global: stat; };    # stat 在新 glibc 是 2.33（不是 2.17）
```

两个易漏：`stat` 在新 glibc 是 `@GLIBC_2.33`，`__open_2` 是 GCC FORTIFY 把不带 `O_CREAT` 的 `open` 重写成的变体。漏任一会导致 `urma_admin show` 设备空。

**RTLD_NEXT 拿真函数**：[urma_sim.c:56](urma_sim.c#L56) 的 `init_real_funcs` 用 `dlsym(RTLD_NEXT, "open")` 取 libc 本尊的 `open` 存进 `real_open`，供非 sim 域时透传。`RTLD_NEXT` = "当前库之后下一个"（即 libc）。

**constructor 提前接管**：[urma_sim.c:212](urma_sim.c#L212) 的 `__attribute__((constructor))` 在 main 之前跑——加载 config、造临时设备目录、`dlsym` 拿真函数指针。

## 1. 怎么模拟 open：两条 sim 路径 + 透传

`open` 拦截在 [urma_sim.c:470](urma_sim.c#L470)（外壳处理变参 mode）+ [urma_sim.c:411 `urma_sim_open_impl`](urma_sim.c#L411)（真逻辑）+ [urma_sim.c:486 `__open_2`](urma_sim.c#L486)（FORTIFY 变体）。三个都标 `@GLIBC_2.17` 导出。

为什么有两条 sim 路径——因为真 URMA 设备暴露两类内核接口，sim 各拦一种：

### 路径1：sysfs → 假 SYSFS fd（读设备属性）
```
路径以 /sys/class/ubcore 开头（urma_sim_is_sysfs_path）
  → 解析 dev_name + file_name
  → urma_sim_fd_alloc(URMA_SIM_FD_SYSFS, idx, file_name)  # 假 fd ≥100000
```
liburma 的 `urma_read_sysfs_file` open `/sys/class/ubcore/<dev>/<file>` 读 driver_name/max_jfc/eids/eid0 等。sim 给假 SYSFS fd，记设备下标 + 文件名，后续 `read` 时从 config 查值返回。

### 路径2：cdev → 假 CDEV fd（建链 + 数据面）
```
路径以 /dev/uburma 开头（urma_sim_is_cdev_path）
  → 取 dev_name
  → urma_sim_fd_alloc(URMA_SIM_FD_CDEV, idx, NULL)
```
liburma open `/dev/uburma/<dev>` 拿 fd 去发 `ioctl(URMA_CMD)` 建链 + `mmap` 映射 queue/doorbell/CQ。假 CDEV fd 供后续 ioctl/mmap 识别。

### 路径3：透传
```
非 sim 域（/etc/hostname 之类）→ real_open 透传真内核
```

**假 fd 表 ≥100000 是"该不该拦"的判据**：[urma_sim.c:167 `urma_sim_fd_alloc`](urma_sim.c#L167) 给的 fd 都在 `100000+i` 段。后续 `ioctl`/`mmap`/`read` 拦到调用，用 `urma_sim_fd_get(fd)`（[urma_sim_hw.c:107](urma_sim_hw.c#L107)）查表，只认 ≥100000——真 fd（小整数）查不到就透传。

## 2. 怎么让 urma_admin 发现设备：造目录 + 重定向 opendir

`urma_admin show` 走 liburma 的 `urma_discover_devices`，第一步 `opendir("/sys/class/ubcore")`。容器没这目录（无内核模块、sysfs 只读）。

**constructor 造假目录**（[urma_sim.c:226](urma_sim.c#L226)）：
```c
snprintf(g_sim_dir_root, ..., "/tmp/urma_sim_%d", getpid());
mkdir(g_sim_dir_root, 0700);
for (每个设备 i) mkdir("/tmp/urma_sim_<pid>/<设备名>", 0700);   // 设备名作子目录
```

**opendir 拦截重定向**（[urma_sim.c:250](urma_sim.c#L250)）：
```c
if (strcmp(name, "/sys/class/ubcore") == 0 || strcmp(name, "/dev/uburma") == 0) {
    return real_opendir(g_sim_dir_root);   // 不打开 /sys/class/ubcore，打开 /tmp/urma_sim_<pid>
}
```
返回的 DIR* 指向真目录，后续 `readdir`（走 libc 原样，sim 不拦）正常吐 `udma_sim0`/`udma_sim1` 设备名。liburma 只认"目录有子条目"这一件事，不验证是不是真 sysfs，就当发现了设备。

`show -d <name>` 校验设备名同样：`opendir("/dev/uburma")` 重定向到同一假目录，readdir 吐设备名校验通过。

## 3. 怎么模拟 ioctl：拦入口 + 用户态回填

`ioctl` 拦截在 [urma_sim.c:527](urma_sim.c#L527)（入口），核心逻辑在 [urma_sim_dev.c:492 `urma_sim_handle_ioctl`](urma_sim_dev.c#L492)。

### 入口：拦 URMA_CMD
```c
int ioctl(int fd, unsigned long request, ...) {
    urma_sim_fd_t *e = urma_sim_fd_get(fd);   // 查假 fd 表
    if (e != NULL && e->type == CDEV && request == URMA_CMD && argp != NULL) {
        urma_cmd_hdr_t *hdr = (urma_cmd_hdr_t *)argp;
        int ret = urma_sim_handle_ioctl(fd, hdr->command, hdr->args_len, hdr->args_addr);
        return ret != 0 ? (errno=EIO, -1) : 0;   // 用户态回填，不进内核
    }
    return real_ioctl(fd, request, argp);   // 真 fd 或非 URMA_CMD 透传
}
```

### 协议：urma_cmd_hdr_t + urma_cmd_attr_t
```
ioctl(fd, URMA_CMD, &hdr)
  hdr->command  = 命令号（URMA_CMD_CREATE_CTX 等）
  hdr->args_addr = urma_cmd_attr_t 数组首址（用户态指针）
  hdr->args_len  = 数组总字节 = attrNum * sizeof(attr)
每个 attr: { type, flag, field_size, attr_data, data }
  data 是用户态指针，指向实际值缓冲
  OUT attr type ≥ 0x80（URMA_CMD_OUT_TYPE_INIT），IN attr type < 0x80
```

### 命令分发 + 回填
`urma_sim_handle_ioctl` 按 command 分发到各 handler（都在 [urma_sim_dev.c](urma_sim_dev.c)）：
- `handle_query_dev_attr`（[行 215](urma_sim_dev.c#L215)）：按 `g_query_dev_attr_map`（attr type→sysfs 文件名）用 config/sysfs 值回填设备能力
- `handle_create_ctx`（[行 306](urma_sim_dev.c#L306)）：回填 async_fd + 设 udata.resp.cqe_size=64
- `handle_link_cmd`（[行 377](urma_sim_dev.c#L377)）：CREATE_JFC/JFS/JFR/JETTY/IMPORT_JETTY/BIND_JETTY/ALLOC_TOKEN_ID/REGISTER_SEG，回填 OUT id/handle/token/depth/max_sge，并登记数据面关联（queue_assoc/cq_assoc/jfr_bind）
- `GET_EID_LIST`（[行 573](urma_sim_dev.c#L573)）：返回 -1 让 urma 走 sysfs fallback（读 eids/eidN）
- `CREATE_JFCE`（[行 556](urma_sim_dev.c#L556)）：创建真 eventfd 给它（wait_jfc 阻塞读）

回填靠 [`write_attr_value`](urma_sim_dev.c#L33)：按 `attr->field_size`（u8/u16/u32/u64）往 `attr->data`（用户态指针）写值。因为 sim 在用户态，直接 `*(uint8_t*)attr->data = v` 就行（内核态要 copy_to_user，那是 [future/kmod/](future/kmod/DESIGN.md) 的事）。

**ioctl 仿真的本质**：不进内核，在用户态按 URMA_CMD 协议把内核 handler 该回填的值填进用户态 attr 缓冲，顶替内核 handler 的位置。

## 4. 怎么模拟 mmap：给假内存 + 登记 region

`mmap` 拦截在 [urma_sim.c:343](urma_sim.c#L343)，内存管理在 [urma_sim_hw.c](urma_sim_hw.c)。

### 入口：只拦假 cdev fd 的 mmap
```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (urma_sim_fd_is_cdev(fd)) {
        uint32_t cmd, idx;
        if (urma_sim_mmap_decode_offset(offset, 4096, &cmd, &idx) == 0) {  // 反解 offset
            void *p = urma_sim_mmap_alloc(fd, length, cmd, idx);   // 给匿名内存
            if (cmd == URMA_SIM_MMAP_JETTY_DSQE)   // doorbell mmap
                urma_sim_queue_assoc_set_db_by_idx(idx, (uint64_t)(uintptr_t)p);  // 关联到 queue
            return p;
        }
    }
    return real_mmap(...);   // 透传
}
```

### offset 反解 + 假内存分配
- [`urma_sim_mmap_decode_offset`](urma_sim_hw.c#L48)：真 udma 把 `db.id` 编进 mmap offset（`offset = (idx<<4)|cmd`），用 `MAP_COMMAND_MASK`/`MAP_INDEX_SHIFT` 反解出 `cmd`（映射类型：JETTY_DSQE/HUGEPAGE/JFC_PAGE/...）和 `idx`（=db.id 或 queue id）。
- [`urma_sim_mmap_alloc`](urma_sim_hw.c#L70)：从 region 池分配一段匿名内存，登记 (size/cmd/idx)。返回的指针就是后续真 udma 当"queue/doorbell/CQ 内存"读写的那块。
- [`urma_sim_mmap_find`](urma_sim_hw.c#L90)：按地址反查是不是 sim 假内存，供 `munmap` no-op（[urma_sim.c:374](urma_sim.c#L374)）。

**mmap 仿真的本质**：不真映射设备页，给一段假匿名内存 + 登记"这块假内存代表哪个 queue/db/cq"，供数据面轮询线程用。真 udma 的 mmap 拿到的是假内存，但它读写这块假内存时以为在操作真设备内存。

## 5. WQE：数据面的"任务卡"

post 时用户态 API 的 `urma_jfs_wr_t`（wr）被真 udma 转成 WQE 写进 queue 内存。WQE 是 48 字节位域结构（`_Static_assert(sizeof==48)`），sim 用镜像结构 `struct urma_sim_sqe_ctl`（[urma_sim_hw.c:513](urma_sim_hw.c#L513)）解：

```
偏移0:  sqe_bb_idx(16) flag(7) ... owner(1)
偏移5:  opcode(8)              ← 操作类型 READ(6)/WRITE(3)/SEND(0)
偏移12: rmt_jetty_or_seg_id(20) ← 对端 jetty id(SEND) 或 seg tid(READ/WRITE)
偏移16: rmt_eid[16]           ← 对端 EID（定位对端进程）
偏移40: rmt_addr_l            ← 远端地址低32位（READ/WRITE 用）
偏移44: rmt_addr_h            ← 远端地址高32位
```
WQE 头 48 字节后紧跟 SGE（Scatter/Gather Element，`{va, len}`）——本端数据缓冲地址+长度，`sge_num` 个。WQE 在 queue 里按 pi 定长排布：`queue 起址 + pi*64` 是第 pi 个 WQE。

三者在闭环里流转：wr（用户态结构）→ WQE（queue 内存位域）→ CQE（CQ 内存完成条目）。wr 给人用，WQE 给硬件/sim 读，CQE 告诉应用执行完了。

## 6. 怎么模拟 doorbell：假内存当门铃 + 轮询盯 pi

真硬件 doorbell 是个寄存器：post 时往它写 pi，硬件看到 pi 变就去 queue 取新 WQE。sim 的轮询线程充当"看到门铃响就干活的硬件"。

**第1步 假内存当门铃**：真 udma `udma_u_alloc_db` 时 mmap JETTY_DSQE → sim 给假页（见第4节），登记 `queue_assoc.db_addr = 假页地址`（按 idx=db.id 关联回 create_jfs 时登记的 queue）。

**第2步 post 写门铃**：真 udma `udma_u_post_jfs_wr` 用 `st64b`（`asm volatile .inst 0xf83f9140`，aarch64 原子存储）把 pi 写到 `db_addr+UDMA_DOORBELL_OFFSET(0x80)`。这不是 syscall，sim 不拦——真 udma 直接写假内存。

**第3步 轮询盯 pi**：[`process_queue`](urma_sim_hw.c#L786) 每 100µs 扫：
```c
volatile uint32_t *pi_ptr = (volatile uint32_t*)(qa->db_addr + 0x80);
uint32_t pi = *pi_ptr;                       // 读假 doorbell 的 pi
if (pi == qa->pi_seen) return;               // 没变 = 没新 WQE
while (qa->pi_seen != pi) {
    uint8_t *wqe = (uint8_t*)(qa->buf_addr + idx*64);  // 从假 queue 取 WQE
    process_wqe(qa, wqe, idx);               // 解 WQE + memcpy/IPC + 造 CQE
    qa->pi_seen++;
}
```

**doorbell 仿真的本质**：mmap 给假内存当门铃 + 后台线程轮询读假内存偏移 0x80 的 pi + pi 变了就从假 queue 取 WQE 处理造 CQE。真 udma 的 st64b 写门铃、读 CQE 全是用户态内存操作（不经 syscall），sim 给假内存，轮询线程充当硬件。

JFR 的 recv 队列用 sw_db（record db），不是 mmap 来的——是 create_jfr 时 udata 直接带的 `cmd->db_addr`（[urma_sim_hw.c:151](urma_sim_hw.c#L151)）。轮询线程对 q_type=1（rq）走 `jfr_refill_pending`（[行 433](urma_sim_hw.c#L433)），同样盯 pi 变化，但取 recv WQE 索引存进 pending 队列供 SEND 投递。

## 7. 轮询线程主体：解 WQE + 搬数据 + 造 CQE

[`urma_sim_hw_thread`](urma_sim_hw.c#L810) 每 100µs 扫所有 queue_assoc：
- q_type=0（sq）→ `process_queue` 扫 doorbell pi，解 WQE 执行 READ/WRITE/SEND
- q_type=1（rq）→ `jfr_refill_pending` 扫 sw_db pi，吸新 post 的 recv WQE 进 pending

[`process_wqe`](urma_sim_hw.c#L679) 按 opcode 分支：
- **READ(6)**：`memcpy(本端va=SGE.va, 远端va=rmt_addr, len)`。单进程远端 va 在 rmt_addr（import_seg 复用真实 va）；跨进程走 IPC。
- **WRITE(3)**：反向 `memcpy(远端va, 本端src va, len)`。
- **SEND(0)**：按 `rmt_jetty_id` 找对端 JFR recv 槽（本进程查或跨进程 IPC），`urma_sim_jfr_deliver_send` memcpy 数据进槽 + `produce_recv_cqe` 造对端 recv CQE + `produce_cqe` 造发送方 send CQE。
- **IMM/NOTIFY 变体**：SEND_WITH_IMM(1)/SEND_WITH_INVALID(2) 复用 SEND 路径、WRITE_WITH_IMM(4)/WRITE_WITH_NOTIFY(5) 复用 WRITE 路径。WQE 头长度随 opcode 变（get_ctl_len：WRITE=48/WRITE_IMM=64/WRITE_NOTIFY=80），process_wqe 按 opcode 算 ctl_len 从正确偏移取首 SGE。**SEND_WITH_IMM imm 闭环完整验证**：imm_data 在 WQE 偏移 40（=SQE_SEND_IMM_FIELD，复用 rmt_addr_l/h），process_wqe 取出 → `urma_sim_ipc_send` 协议头扩展传 imm → 对端 handle_conn 收 → `urma_sim_jfr_deliver_send` 传 → `produce_recv_cqe` 填 CQE data_l/h + opcode=SEND_WITH_IMM（test_ipc_send_imm PASS）。**WRITE_WITH_IMM 数据搬运验证**：ctl_len=64 取首 SGE@64 + imm@48，复用 WRITE 路径写 seg + send CQE（test_hw_write_imm PASS，验 seg_buf 写对 + send CQE）。WRITE_WITH_IMM 单进程 imm 通知 CQE 投递已实现（`urma_sim_jfr_deliver_imm` 给本进程 JFR 产 recv CQE 带 imm），但**完整投递 TODO**——WRITE 的 WQE 只带 seg tid+rmt_eid 不带 jetty id，单进程无法定位"对端 JFR"路由 imm 通知（跨进程同），故 test_hw_write_imm 只验数据搬运+send CQE，imm 通知 CQE 投递未实测。
- **NOP(0x11)**：占位产 CQE 不搬数据。
- **原子类 CAS(7)/FAA(0xb) 单进程已实现**：原子数据在 WQE 偏移 64（SQE_ATOMIC_DATA_FIELD，首 SGE 在 48，CAS/FAA WQE 占 2 WQEBB）。CAS：读远端 rmt_addr 的旧值回本端 src buf，若旧值==cmp_data（wqe+64+len）则换 swap_data（wqe+64）；FAA：读旧值回本端，远端 += operand（wqe+64），按 4/8 字节无符号加。单进程用 rmt_addr 直接读-改-写（test_hw_atomic PASS：CAS 匹配/不匹配 + FAA 三场景）。**process_queue 按 opcode 算 pi_seen step**（CAS/FAA 占 2 WQEBB → step=2，其他=1），否则多块 WQE 被当多个空 WQE 处理。跨进程原子 TODO（需 IPC OP_ATOMIC）。WRITE_WITH_ATOMICSTORE_ADD(0x19) TODO（复合，字段布局复杂）。

[`produce_cqe`](urma_sim_hw.c#L592) 往 `cq_addr + (cq_ci&mask)*64` 写 `urma_sim_jfc_cqe`（`_Static_assert(sizeof==64)`）：
- `owner = valid_owner^1`（让真 udma `get_next_cqe` 的 `owner^valid_owner!=0` 成立，CQE 被认为有效）
- `entry_idx = wqe_pi`（让 `parse_cqe_for_send` 更新 queue->ci）
- `user_data_l/h = jetty_addr`（让 parse 还原 queue 指针）
- `local_num_l/h = jetty_id`（send CQE 的 local_id）
- `status=0`、`s_r=0`（send CQE）/ `s_r=1`（recv CQE，`produce_recv_cqe`）

真 udma `poll_jfc` 读这块假 CQ 内存就拿到"完成"。

## 8. 怎么跨进程通信：eid 寻址 + unix socket + 零共享存储

跨进程难点：A 解到 WQE 的远端地址是 B 进程的指针，A 直接 memcpy 会 SIGSEGV。sim 解法：**A 不碰 B 的指针，按 WQE 自带的 rmt_eid 找到 B 进程，发请求让 B 自己读自己的内存回传**。无共享内存/文件/注册表，纯 socket 请求-响应。

全部在 [urma_sim_ipc.c](urma_sim_ipc.c)。

### 寻址依据：WQE 的 rmt_eid
真 udma post 填 WQE 时把对端 EID 写进 `wqe->rmt_eid`（16B，从 import_jetty 链路天然带）。sim 白拿它当定位对端进程的钥匙。每进程 create_context 时拿自己设备 eid 存成 `g_local_eid`（[行 43](urma_sim_ipc.c#L43)）。

### 定位：eid → socket 路径
每进程 `create_context` 后自动调 [`urma_sim_ipc_start(eid)`](urma_sim_ipc.c#L199)，起后台线程 listen 自己的 socket：
```c
eid_to_sockpath(g_local_eid, path, ...);   // /tmp/urma_sim_ipc_<eid前8字节hex(swap后)>
int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
bind/listen(lfd, 16);
pthread_create(..., urma_sim_ipc_thread, lfd);   // 后台 accept+handle
```
用 eid 前 8 字节（含 IPv4 段，有区分度）做路径名。不同设备 guid → 不同 eid → 不同 socket，不撞。A 解 WQE 拿 `rmt_eid` → 推算 `/tmp/urma_sim_ipc_<rmt_eid前8字节hex>` → connect。

### 分流：is_local
[`urma_sim_ipc_is_local(rmt_eid)`](urma_sim_ipc.c#L70)：rmt_eid == g_local_eid → 本进程走 memcpy（test_hw_post 自引用）；不等 → 跨进程走 IPC。g_local_eid 全 0（IPC 没起）→ 一律本进程（单进程兼容）。

### 协议
```
请求: magic(4) op(4) tid(4) len(4)   [WRITE/SEND 后跟 len 字节数据]
响应: magic(4) status(4) len(4)       [READ 后跟 len 字节数据]
op: READ(1)/WRITE(2)/SEND(3)  tid 字段在 SEND 复用为 jetty_id
```

### 三种操作链路
- **READ**（A 读 B 的 seg）：A 发 `{op=READ, tid=B_tid, len}` → B 后台线程用 tid 查本进程 `tid_seg` 表拿 B 的 buf 指针 → 读 buf → 发回 A → A memcpy 进 A 的 buf + 造 CQE
- **WRITE**（A 写 B 的 seg）：A 发 `{op=WRITE, tid=B_tid, len}` + 数据 → B 用 tid 查 buf 指针 → memcpy 进 buf
- **SEND**（A SEND 给 B 的 JFR）：A 发 `{op=SEND, jetty_id=B_jetty, len}` + 数据 → B 用 jetty_id 查本进程 jetty_id→JFR 映射 → JFR recv 槽 → memcpy 进槽 + 造 recv CQE

### 零共享存储的精妙：tid/jetty_id 只在本进程查
每个进程 preload 同一 liburma_sim.so，但 .so data 段是每进程独立副本（COW），`g_sim_tid_seg`/`g_sim_queue_assoc` 各自独立。这恰好对路：A 要访问 B 的内存，A 不知道 B 的指针，但 A 有 B 的 **eid**（定位进程）+ B 的 **tid**（B 内部内存索引）。A 发 (tid, len) 给 B，B 用 tid 查 B 自己的表拿 B 的指针，B 在 B 进程内访问（指针有效）。A 从不需要 B 的指针，B 的指针只在 B 进程内用——A 给名字，B 查自己的本子找到东西。这是零共享存储跨进程寻址的本质。

## 9. ummu 拦截：tid→seg 登记

[urma_sim_ummu.c](urma_sim_ummu.c) 拦 ummu 库 4 函数（`ummu_allocate_tid`/`grant`/`ungrant`/`free_tid`，用 `URMA_SIM_UMMU` 版本块导出，不是 libc 函数）。真 udma `register_seg` 调 `ummu_grant(tid, va, len, ...)` 把 tid→va 写进硬件（硬件知道 tid 对应哪段内存的唯一渠道）。sim 拦下在用户态记 tid→va/len 表（[`urma_sim_tid_seg_register`](urma_sim_hw.c#L267)）。

跨进程 READ/WRITE 时，对端进程后台线程收到 (tid, len) 请求，就用 tid 查**本进程**这张表拿 buf 指针——这正是上面 IPC "B 用 tid 查 B 自己的表"的表。单进程时 sim 轮询线程解到 WQE 的 rmt_tid 也能查这张表（作兜底，但单进程远端地址通常直接在 rmt_addr）。

## 整体链路（一次跨进程 READ 全程）

```
A 进程:
  open(/dev/uburma/udma_sim0) → sim 给假 CDEV fd
  ioctl(CREATE_CTX...) → sim 用户态回填 async_fd/cqe_size + urma_sim_ipc_start(A的eid) 起后台 socket
  ioctl(CREATE_JFC/JFS...) → sim 回填 id/handle + 登记 queue_assoc/cq_assoc
  register_seg → ummu_grant 拦截登记 A 的 tid→bufA 表
  mmap(queue/db/cq) → sim 给假内存 + db_addr 关联回 queue
  post READ(wr) → 真 udma 转 WQE 写进假 queue + st64b 写 pi 进假 doorbell
sim 轮询线程(A):
  读 db_addr+0x80 的 pi → 变了 → 取 WQE(READ, rmt_eid=B, rmt_tid=B_tid, rmt_addr=B_va)
  is_local(rmt_eid)? 否 → 走 IPC
  rmt_eid → B 的 socket 路径 → connect B
  发 {op=READ, tid=B_tid, len}
B 进程后台线程:
  tid → 查 B 自己的 tid_seg 表 → B 的 buf 指针 → 读 buf → 发回 A
A:
  memcpy 进 A 的 buf → produce_cqe 往假 CQ 写 CQE
A 的 poll: 真 udma 读假 CQ 内存 → 拿 sim 造的 CQE(status=SUCCESS, user_ctx 回填)
```

## 参考文件

| 部件 | 文件 |
|---|---|
| 拦截层（open/opendir/mmap/ioctl 等）| [urma_sim.c](urma_sim.c) |
| ioctl 命令分发 + 回填 | [urma_sim_dev.c](urma_sim_dev.c) |
| 仿真硬件数据面（mmap/doorbell/轮询/CQE/WQE）| [urma_sim_hw.c](urma_sim_hw.c) |
| 跨进程 IPC | [urma_sim_ipc.c](urma_sim_ipc.c) |
| ummu 拦截 + tid→seg | [urma_sim_ummu.c](urma_sim_ummu.c) |
| constructor 启动 | [urma_sim_provider.c](urma_sim_provider.c) |
| config 加载 | [urma_sim_config.c](urma_sim_config.c) |
| 符号版本脚本 | [urma_sim.version](urma_sim.version) |
| 内部接口/数据结构 | [urma_sim.h](urma_sim.h) / [urma_sim_hw.h](urma_sim_hw.h) |
