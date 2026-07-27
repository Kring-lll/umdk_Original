# URMA 设备仿真实现计划（全 LD_PRELOAD 方案）

## 目标
在 `umdk/sim/urma/` 新建仿真模块，无硬件环境下 `urma_admin` 能枚举出虚拟设备 + 查询设备属性。
为后续"按表单输入产出输出"打基础。

## 架构：全 LD_PRELOAD 用户态仿真

### 为什么是这个方案
- 容器 `/sys` 是 **只读 sysfs**（实测 `ro,nosuid,nodev,noexec,relatime`），`/sys/class/ubcore` 不存在且
  无法 mkdir。→ 不能造真 sysfs 假树。
- 容器共享宿主内核（5.15 WSL2），无法 modprobe 注册真内核 misc driver。→ 不能造真字符设备。
- 故：所有假数据放 `liburma_sim.so` 内存里，用 LD_PRELOAD 拦截全套系统调用，纯用户态虚拟。

## 已确认的关键事实

### 1. 设备发现链路（urma_main.c / urma_device.c）
```
urma_init
  → dlopen /usr/lib64/urma/liburma-udma.so  (provider, ops->name="udma")
  → urma_discover_devices:
      opendir("/sys/class/ubcore")          [拦 opendir]
      readdir 每个条目                       [拦 readdir]
      stat(条目) 必须 S_ISDIR                [拦 stat]
      urma_read_sysfs_device:
        realpath(sysfs_path)                [拦 realpath]
        urma_read_sysfs_dev_attrs:
          fopen-equivalent: realpath+open+read 读每个属性文件
                                            [拦 open/read,返回假值]
          driver_name 必须 == "udma"        [urma_match_device strcmp]
        urma_query_device_attr:
          urma_open_cdev("/dev/uburma/<dev>"): realpath+open(O_RDWR)
                                            [拦 realpath/open,返回假fd]
          ioctl(fd, URMA_CMD, &hdr)         [拦 ioctl,回填 QUERY_DEV_ATTR]
```

### 2. 要拦的系统调用（全集，6 类）
| 调用 | 用途 | 仿真行为 |
|------|------|----------|
| opendir | 枚举 /sys/class/ubcore | 返回一个仿真 DIR 句柄 |
| readdir | 读目录项 | 逐个吐出 config 里的设备名 |
| stat | 确认是目录 | 返回 st_mode=S_IFDIR |
| realpath | 路径标准化 | 对 sim 路径直接返回原路径 |
| open | 打开 sysfs 文件 + /dev/uburma | 返回仿真 fd（自维护 fd 表） |
| read | 读 sysfs 文件内容 | 按 fd→文件名返回 config 里的值 |
| ioctl | URMA_CMD 查询 | 解析 hdr,按 command 回填 attr |

非 sim 路径的调用 → dlsym(RTLD_NEXT) 转真实现（不影响其他程序）。

### 3. ioctl 协议（urma_cmd.h / urma_cmd_tlv.h）
- `URMA_CMD = _IOWR('U', 1, urma_cmd_hdr_t)`
- `urma_cmd_hdr_t { uint32_t command; uint32_t args_len; uint64_t args_addr; }`
- `urma_cmd_attr_t { uint8_t type; uint8_t flag; uint16_t field_size; union{struct{el_num,el_size,reserved}bs;uint32_t value;} attr_data; uint64_t data; }`
- `hdr->args_addr` → `urma_cmd_attr_t[]` 数组，每项 data 是指向值缓冲的指针
- 初步支持 `URMA_CMD_QUERY_DEV_ATTR`：遍历 attrs，按 type 匹配 QUERY_DEVICE_OUT_*，
  往 *(uintptr_t)attr->data 写值（参考 core_fixture.h WriteCoreAttrValue 按 field_size 写）

### 4. sysfs 文件全集（urma_parse_device_attr，~35 个）
ubdev, driver_name(="udma\n"), transport_type, guid, feature,
max_jfc, max_jfs, max_jfr, max_jetty, max_jetty_grp, max_jetty_in_jetty_grp,
max_jfc_depth, max_jfs_depth, max_jfr_depth, max_jfs_inline_size, max_jfs_sge, max_jfs_rsge, max_jfr_sge,
max_msg_size, max_read_size, max_write_size, max_cas_size, max_swap_size,
max_fetch_and_add_size, max_fetch_and_sub_size, max_fetch_and_and_size, max_fetch_and_or_size, max_fetch_and_xor_size,
atomic_feat, trans_mode, congestion_ctrl_alg, ceq_cnt, max_tp_in_tpg, port_count, max_eid_cnt,
page_size_cap, max_oor_cnt, mn, max_netaddr_cnt,
device/{vendor,device}, port0/{max_mtu,state,active_width,active_speed,active_mtu}

## 实现结构

```
umdk/sim/urma/
├── CMakeLists.txt          # 构建 liburma_sim.so
├── urma_sim.c              # LD_PRELOAD 拦截层(opendir/readdir/stat/realpath/open/read/ioctl)
├── urma_sim.h              # 内部接口 + sim 设备/文件/fd 数据结构
├── urma_sim_dev.c          # 虚拟设备表 + ioctl 命令分发(QUERY_DEV_ATTR 等)
├── urma_sim_config.c       # 加载 config.json 初始化设备表
├── urma_sim_config.json    # 设备表单:每个设备的 sysfs 值 + 各命令输出
├── urma_sim_setup.sh       # 编译 + 设 LD_PRELOAD 环境验证
├── README.md               # 使用文档
└── examples/run_urma_admin.sh  # 示例:preload + urma_admin show dev
```

## 实现要点

### 拦截层 urma_sim.c
- 构造函数 `__attribute__((constructor))` 启动时加载 config.json
- 每个被拦截的 libc 函数：先判断是否针对 sim 路径（/sys/class/ubcore/ 或 /dev/uburma/）
  - 是 → 走仿真逻辑
  - 否 → `dlsym(RTLD_NEXT, "func")` 转真实现
- 自维护 fd 表：open 返回的假 fd 映射到 {设备名, 文件名}，read 时查表返回值
- opendir 返回仿真 DIR（用一个小 struct 伪装），readdir 吐设备名

### 命令分发 urma_sim_dev.c
- `int urma_sim_ioctl(urma_cmd_hdr_t *hdr)`: switch(hdr->command)
- case URMA_CMD_QUERY_DEV_ATTR: 遍历 attr 数组，按 type 回填设备属性
- 用 WriteCoreAttrValue 同款逻辑（按 field_size 写 u8/u16/u32/u64）
- 后续扩展：IMPORT_JETTY 等按 config 的输入→输出表

### config.json 格式
```json
{
  "devices": [
    {
      "name": "udma_sim0",
      "sysfs": { "driver_name": "udma", "transport_type": "0", "max_jfc": "16", ... },
      "ioctl": { "QUERY_DEV_ATTR": { "max_jfc": 16, "max_jfs": 32, ... } }
    }
  ]
}
```

## 构建
- CMakeLists.txt 产 `liburma_sim.so`（-shared -fPIC -ldl）
- include：`src/urma/lib/urma/core/include`（urma_cmd.h, urma_cmd_tlv.h, urma_types.h）
- 不链接 liburma.so，只复用头文件协议定义
- 构建产物放 `sim/urma/build/liburma_sim.so`

## 验证标准
1. `liburma_sim.so` 编译通过
2. `LD_PRELOAD=... urma_admin show dev` 枚举出 `udma_sim0`
3. `LD_PRELOAD=... urma_admin show dev -d udma_sim0` 查询出属性
4. 不加 LD_PRELOAD 时 urma_admin 行为不变（无副作用）

## 文档
README.md 含：架构说明、构建步骤、配置 config.json、运行 urma_admin、已知限制、扩展指南。

## 已知限制
- 仅用户态拦截，urma_admin 的 netlink 操作（dev set/expose/eid）暂不仿真（需额外拦 libnl）
- 仅仿真设备枚举+属性查询，真实 RDMA 通信（post_wr 等）不仿真
- ASan 二进制与 LD_PRELOAD 顺序冲突（urma_admin 非asan二进制，无此问题）

---

## 跨进程 IPC 验证设计（方式1 扩展：双进程实时数据传递）

### 背景
方式1（URMA_SIM_HW=1）单进程自引用已跑通 test_hw_post（READ/WRITE，TEST PASS）。
但单进程够不着"对端进程的内存"——A 进程解引用 B 进程的指针 = SIGSEGV。
要突破单进程边界，用**进程间通信（unix socket）实时传数据**，不依赖文件/shm/注册表。

### 核心依据：WQE 自带对端定位信息
真 udma post 时填 WQE（udma_u_jfs.c:969）：
```
memcpy(wqe_ctl->rmt_eid, &udma_tjetty->le_eid.raw, URMA_EID_SIZE);  // 对端 EID
ctrl->rmt_jetty_or_seg_id = udma_seg->tid;                          // 对端 seg 的 tid
ctrl->rmt_addr_l/h = sgl[0].addr;                                   // 对端 va（跨进程无意义,sim不用）
```
- **rmt_eid**（对端 EID，16B）：test 链路天然带的（seg/import 时传过来），sim 白拿 → 用来**定位对端进程**
- **rmt_tid**（对端 tid）：用来在对端进程内查自己的 buf 指针
- rmt_addr（对端指针）：跨进程无意义，sim 忽略

eid 来源：urma_create_context → ctx->eid = device eid_list[0]。sim 拦 QUERY_DEV_ATTR/GET_EID_LIST
回填 eid（从 config guid 派生）。config 已有 udma_sim0(guid 192.168.10.1)/udma_sim1(guid 192.168.20.1)
两个设备 → **A 用 sim0、B 用 sim1，eid 天然不同**，socket 路径不冲突。

### 架构（零共享存储，纯 IPC）
```
每进程 sim 在 urma_init 时：
  起后台线程，unix socket listen /tmp/urma_sim_ipc_<eid低64位hex>
  （eid 在 create_context 后确定，socket 在第一次 post 前起即可）

每进程私有表（ummu_grant 时存，进程内即可）：
  tid → {buf指针, len}   // 本进程自己的 seg

A 解 WQE（READ，rmt_eid=B_eid，rmt_tid=B_tid）：
  1. rmt_eid → 推算 B 的 socket 路径 /tmp/urma_sim_ipc_<B_eid_hex>
  2. 连 B 的 socket，发请求 {tid=B_tid, op=READ, len}
  3. B 后台线程收到：tid→查本进程表→B 的 buf 指针→read buf→发回 A
  4. A 收到数据→memcpy 进 A 的 buf→造 CQE

A 解 WQE（WRITE，rmt_eid=B_eid，rmt_tid=B_tid）：
  1. A 先把要写的数据从 A 的 buf 读出（A 知道自己 src sge.addr 指针）
  2. 连 B，发请求 {tid=B_tid, op=WRITE, len, data...}
  3. B 收到：tid→查表→B 的 buf 指针→memcpy 进 B 的 buf→回复 OK
  4. A 收到 OK→造 CQE
```

### 寻址（无需注册表）
- 对端进程定位：rmt_eid → socket 路径（约定推算，不查表）
- 对端内存定位：rmt_tid → 对端进程私有 tid 表（对端自己查自己指针）
- 数据传递：socket 实时请求-响应（不落盘、不缓存）

### 验证目标
test_hw_post 拆双进程：
- proc_A：用 udma_sim0，register_seg(buf_A)，把 seg（含 A 的 eid）通过 socket 发给 B
- proc_B：用 udma_sim1，register_seg(buf_B)，收 A 的 seg，import_seg，post READ(A 的 seg)
- 期望：B 的 buf 被 A 的数据填满（A WRITE→B），或 B READ A 拿到 A 的数据
- 成功标志：跨进程数据搬运成立 + poll 拿到 SUCCESS CQE

**✅ 已跑通（oe-arm aarch64 容器，TEST PASS）**：
[examples/test_ipc_2proc.c](examples/test_ipc_2proc.c) 双进程，B READ A 的 seg，buf 'Z'→'A'。
实跑暴露并修复：rmt_eid 是 swap 后 le_eid（sim 全程用反转版）；ALLOC_TOKEN_ID 回填须=ummu_tid<<8；
REGISTER_SEG 须回填 OUT_TOKEN_ID=IN_TOKEN_ID；未启 IPC 时 is_local 默认走 memcpy 兼容单进程。

### 改动范围（仅 sim，test 自写）
1. sim 起后台 socket 线程（urma_init 触发，路径含 eid）
2. ummu_grant：tid→buf 指针存进本进程私有表（已有 g_sim_tid_seg，改用）
3. process_wqe：rmt_eid 判断本端/对端
   - 本端（eid==自己）：现有 memcpy 路径（单进程兼容）
   - 对端：socket 请求 B，B 响应数据
4. 新增 examples/test_ipc_2proc.c：双进程，A/B 各持一设备互传

### 兼容性
- 单进程 test_hw_post：rmt_eid==自己 eid → 走现有 memcpy 路径，不受影响
- 双进程：rmt_eid!=自己 → 走 IPC，突破单进程边界

## SEND 实现设计（✅ 已跑通）

**✅ 已跑通（oe-arm aarch64 容器，TEST PASS）**：
[examples/test_ipc_send.c](examples/test_ipc_send.c) 双进程，B 发 SEND 给 A 的 JFR recv 槽，A 收 recv CQE（`s_r=1 opcode=0 user_ctx=0xA1`），recv buf='Hello from B'，B 拿 send CQE（`s_r=0 user_ctx=0xB1`）。
实跑暴露并修复：CREATE_JETTY/CREATE_JFR 没给 sq/rq 设 jfc_id（send/recv CQE 丢）；max_sge 是 uint8 按 `*(uint32_t)` 读带垃圾（改用 read_in_attr 按 field_size 读）。

### 本质区别：SEND vs READ/WRITE

| | READ/WRITE（已通） | SEND（待做） |
|--|--|--|
| WQE 带的对端标识 | seg tid + rmt_addr（对端内存指针） | jetty id（`rmt_jetty_or_seg_id = tjetty->id.id`，udma_u_jfs.c:533），**无 rmt_addr** |
| 对端角色 | 被动存储（sim/硬件按 tid→va 直接搬） | **主动方**：必须先 `post recv` WQE 到 JFR，recv WQE 含对端自己的接收 buf sge |
| 对端预先 post | 否 | **是**（无 recv 槽则 RNR） |
| 造 CQE | 1 个 send CQE | 2 个：对端 recv CQE(s_r=1) + 发送方 send CQE(s_r=0) |

### 实现要做的 4 件事

#### 1. 按 jetty_id 找对端 JFR 的 rq 队列内存（核心缺失）
当前 `urma_sim_queue_assoc_t`（urma_sim_hw.h）存 jetty_addr/buf_addr/db_addr，**没存 jetty_id**，且 CREATE_JETTY 的 udata 只给 jetty_addr（指向 sq，不是 rq）。JFR 的 rq.qbuf/buf_addr 拿不到 → 无法读对端 post 的 recv WQE。
- queue_assoc 加 `uint32_t jetty_id` 字段（CREATE_JETTY 时 OUT_ID 填，已回填到 .id，确认与 jetty_id 关系）。
- 区分 sq/rq：queue_assoc 加 `is_rq`/`q_type`。CREATE_JFR 登记 rq（jetty_addr=&jfr->rq，udma_u_jfr.c:571），CREATE_JETTY 登记 sq（jetty_addr=&jetty->sq）。需查 CREATE_JETTY udata 是否含 rq_addr；若不含，sim 从 jetty 对象指针反推 rq（查 `struct udma_u_jetty`（udma_u_jetty.h）的 sq/rq 布局，确认 jetty_addr 能否推 rq）。

#### 2. 跟踪对端 JFR 的 recv WQE 索引
真 udma JFR 用 bitmap 分配 recv WQE idx（post_recv_one udma_u_jfr.c:463 `udma_bitmap_use_idx`），recv CQE 的 `entry_idx` = 选中的 rqe_idx，对端 poll 时 `jfr->rq.wrid[rqe_idx]` 还原 user_ctx（udma_u_jfc.c:675）。
sim 镜像：轮询线程扫 JFR 的 rq sw_db（jfr->sw_db，record db，pi 变化，udma_u_jfr.c:498）读新 post 的 recv WQE（含 sge.va/sge.length + wrid[rqe_idx]=user_ctx），建"待用 recv 槽"队列（rqe_idx→{va,len,user_ctx}）。这是和 sq 轮询对称的 rq 轮询（sim 现在 only 轮 sq 的 db_addr+0x80 pi）。

#### 3. process_wqe 加 SEND case
SEND(opcode=0)：
- 取首 sge = 发送方 src（va/len，本进程，直接读）。
- `rmt_jetty_or_seg_id`→jetty_id→经 IPC（跨进程）或本进程查→找对端 JFR 的 rq + 一个待用 recv 槽 {va,len,user_ctx}。
- **跨进程**：经 IPC（urma_sim_ipc.c）发"SEND 请求{jetty_id, data...}"，对端 sim 收到查自己 JFR recv 槽→memcpy 数据进槽 va→造 recv CQE。需给 IPC 加 SEND op（当前只有 READ/WRITE op 按 tid 寻址；SEND 按 jetty_id 寻址，IPC 协议加 jetty_id 路由）。
- **本进程**：直接 memcpy(对端 recv 槽 va, 发送 src va, len)→造 recv CQE。
- 造两个 CQE：① 对端 recv CQE（s_r=1）写到对端 CQ；② 发送方 send CQE（s_r=0，现有 produce_cqe 路径）写到发送方 CQ。

#### 4. recv CQE 字段（与 send CQE 不同，对齐 udma_u_jfc.c:749 parse_cqe_for_jfc）
- `s_r=1`（CQE_FOR_RECEIVE，路由到 parse_cqe_for_recv udma_u_jfc.c:692）
- `is_jetty=1`（recv 属 jetty 的 jfr；shared jetty 走 jetty_table 查 jfr）
- `local_num_l/h`→`cr->local_id`=jetty_id（查 jfr_table/jetty_table，udma_u_jfc.c:642-644）
- `entry_idx`=rqe_idx（`cr->user_ctx=queue->wrid[entry_idx & mask]`，udma_u_jfc.c:675）—— 关键，对端靠它找 recv buf user_ctx
- `byte_cnt`=completion_len
- `opcode`=HW_CQE_OPC_SEND(0)（cr->opcode=URMA_CR_OPC_SEND，udma_u_jfc.c:598）
- `owner`=valid_owner^1（同 send，get_next_cqe udma_u_jfc.c:779）
- `user_data_l/h`=对端 **jfr/jetty 对象指针**（parse_cqe_for_recv 里 `cr->user_data=&jfr->base`/`&jetty->base`，udma_u_jfc.c:653/666）

### 难点
- 拿对端 JFR rq 内存：CREATE_JETTY udata 是否含 rq_addr？若不含需从 jetty_addr（=jq 指针）反推 rq 偏移。
- IPC 协议加 jetty_id 路由：当前 IPC read/write 按 tid 寻址（对端 tid→buf 表）。SEND 按 jetty_id 寻址对端 JFR recv 槽，需对端 sim 起"按 jetty_id 查 JFR recv 槽"响应。
- recv 槽生命周期：对端 post recv 异步，SEND 到达时若没 recv 槽→真 udma RNR(retry)，sim 简化可返回失败 CQE 或等。

### 验证目标
urma_sample `client_send`（urma_sample.c:914）走完整 SEND+recv：client post recv→post SEND→server 收 recv CQE→server `server_reply_to_client` 再 SEND response→client 收 recv CQE。但 urma_sample 1GB memset 在 qemu-user 跑不动（见 README "urma_sample 初始化修复"），**得自写小 buffer 的 SEND 双进程 test** 验证（仿 test_ipc_2proc，加 jfr + post recv + post SEND）。
