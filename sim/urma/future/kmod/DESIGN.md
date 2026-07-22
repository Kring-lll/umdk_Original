# urma stub kmod 设计（方案 A：建链进内核，数据面留用户态）

> **状态：未来实现（留档，当前不接入 sim）**
>
> 本目录是"内核态仿真"方案的产物，**当前不参与 sim 编译/运行**，sim 仍保持纯用户态（见上层 README）。
> 代码已设计完成、在容器内用 6.6 kernel-devel 头树编通过 `urma_kmod.ko`（vermagic 6.6 aarch64），
> 并在 openEuler aarch64 VM 里验证了核心前提——**VM 内核的 init_module 是真 syscall、能加载模块**
> （内核 dmesg 跑到 `__se_sys_init_module → load_module → do_init_module`，对比容器 qemu-user 返回 ENOSYS）。
>
> 但当前环境（x86 WSL2 + qemu-system TCG 仿真 aarch64）下，完整 openEuler VM 在 systemd 阶段自动加载
> openEuler 自带的 `ummu_pmu`（PMU 驱动）时，其访问 ARM PMU 系统寄存器触发 synchronous external abort →
> 内核 panic → VM 起不到 login，无法 insmod 自己的 kmod 做端到端验证。这是 TCG 仿真的局限，非 kmod 本身问题。
>
> **将来恢复**：拿到真 aarch64 硬件、或能避开 PMU panic 的 VM 环境时，按下方设计编译加载：
> - 容器内编：`cd kmod && make`（KDIR 硬编码 6.6，ARCH=arm64；需 kernel-devel 头树 + gcc）
> - VM 内：`insmod urma_kmod.ko`，再改 sim 拦截层（见"sim 侧改造"）透传 ioctl 进内核，最后跑 test_kmod/test_hw_post 验证。
>
> 详见记忆 urma-sim-kmod-progress。

## 目标

让"用户态 → 内核态 → 设备"三层中，**内核态那层不再被 sim 用户态冒名顶替**。
kmod 在内核注册真 cdev + sysfs + 实现 ioctl handler，使 urma 的建链命令（create_ctx/jfc/jfs/jetty/register_seg 等）真进内核被 kmod 处理。
数据面（post WQE → 写 doorbell → poll CQE）仍由 sim 用户态轮询线程负责（不动），因为 URMA 数据面本就该绕开内核、直访设备内存。

## 分工（方案 A，修正后）

| 职责 | 现状 sim 用户态 | 方案 A 后 |
|---|---|---|
| /dev/uburma/<dev> cdev | sim open 造假 fd(≥100000) | **kmod 注册真 cdev**，sim 不再拦此 open |
| /sys/class/ubcore/<dev> sysfs | sim 造 /tmp 假目录 | **暂留 sim 假目录**（kmod 不做 sysfs，urma_admin show 仍靠 sim 拦 opendir/read） |
| ioctl CREATE_CTX/建链/QUERY_DEV_ATTR 回填 | sim 用户态回填 | **kmod 内核 handler 回填**（copy_to_user） |
| ioctl 建链命令关联登记 | sim 在拦 ioctl 时做 | **sim 仍拦 ioctl，先登记关联，再透传给 kmod 回填** |
| ioctl GET_EID_LIST | sim 返回 -1 走 sysfs fallback | **仍 sim 处理**（返回 -1 → urma 走 sim 的 sysfs 假目录读 eids/eidN） |
| ioctl CREATE_JFCE | sim 创建真 eventfd 返回 | **仍 sim 处理**（eventfd 是用户态 wait 用的，sim 拦截层自己创，不透传 kmod） |
| mmap(queue/db/cq 页) | sim 给匿名内存 | **仍 sim 给匿名页**（kmod 不实现 mmap fault） |
| 数据面轮询造 CQE | sim 用户态线程 | **留 sim 用户态不动** |

关键修正：sim 仍拦 ioctl（不是完全透传）。sim 的 ioctl 拦截改为：
1. GET_EID_LIST / CREATE_JFCE → sim 自己处理（前者返回 -1 走 sysfs fallback，后者创建 eventfd），不透传
2. CREATE_CTX/建链命令 → sim 先登记数据面关联（cq_assoc/queue_assoc/jfr_bind/jfr_set_recv_geom），再调 real_ioctl 透传给 kmod 回填 OUT
3. QUERY_DEV_ATTR → sim 透传给 kmod（kmod 内置能力表回填）

kmod 只回填 OUT（CREATE_CTX/建链/QUERY_DEV_ATTR），不碰关联、不碰 GET_EID_LIST/CREATE_JFCE、不碰 mmap、不碰 sysfs。

## 链路（方案 A 后）

```
应用 → liburma → urma_ioctl_*(dev_fd, URMA_CMD)
  dev_fd 是真 cdev fd（open /dev/uburma/<dev> 真进内核，sim 不拦 open）
  ioctl 不被 sim 拦（fd 不在 sim 假 fd 表）→ 真进内核
  → kmod 的 .unlocked_ioctl handler 处理 URMA_CMD：
      解 urma_cmd_hdr_t → 按 command 回填 OUT attr（逻辑同 sim 的 urma_sim_handle_ioctl）
  → 返回用户态

post/poll 数据面：
  mmap(dev_fd, offset) → sim 仍拦（识别真 cdev fd）→ 给匿名页 → sim 轮询线程扫这些页造 CQE
  （完全同现状，不动）
```

## kmod 实现清单

### 1. 模块 init/exit
- init：注册 cdev（/dev/uburma/udma_sim0 等，按 config 设备数）+ sysfs kobject（/sys/class/ubcore/<dev>）
- exit：注销

### 2. cdev + file_operations
- `.owner = THIS_MODULE`
- `.unlocked_ioctl = urma_kmod_ioctl` —— 处理 URMA_CMD
- `.mmap` —— **不实现**（让 sim 拦，或返回 -ENOSYS 让 sim 的 mmap 拦截层接管；需验证 mmap 对真 cdev fd 时 sim 能否拦到）
- `.open`/`.release` —— 简单计数

### 3. ioctl handler（复用 sim 的 urma_sim_handle_ioctl 逻辑）
处理的命令集（与 sim 现状一致，post 端到端所需）：
- QUERY_DEV_ATTR(40) —— 按设备能力表回填 OUT attr
- CREATE_CTX(1) —— 回填 async_fd/cqe_size 等
- CREATE_JFC(16)/CREATE_JFS(8)/CREATE_JFR/CREATE_JETTY(22) —— 回填 handle + 从 udata 读关联
- IMPORT_JETTY(26)/IMPORT_JETTY_EX(47)/BIND_JETTY —— 回填 handle
- ALLOC_TOKEN_ID —— 回填 token_id
- REGISTER_SEG —— 回填 token_id（数据面 tid 关联仍由 sim ummu 拦截做，kmod 只回填）
- CREATE_JFCE —— 回填 eventfd 相关
- GET_EID_LIST —— 回填或 -1 让走 sysfs fallback

**注意**：kmod 回填逻辑要把 sim 的 `urma_sim_handle_ioctl` 整个搬过来。但 sim 的 handler 还登记 queue/cq 关联（urma_sim_queue_assoc_register 等）供数据面用——这部分**仍要在用户态做**（数据面在用户态）。
所以 kmod 的 ioctl handler 只负责回填 OUT attr，关联登记的活怎么传回用户态 sim？

  → 解法：kmod 不登记关联。关联信息（jetty_addr/buf_addr/db_id 等）本就在 ioctl 的 IN udata 里，
     sim 在拦 mmap 时（给匿名页时）从 mmap 的 offset 反解 idx + 从后续 mmap 关联即可拿到，
     不依赖 kmod 传。即关联登记仍由 sim 做（sim 拦 mmap 时 set_db_by_idx 等），kmod 不参与。

  → 所以 kmod 的 ioctl handler = 纯回填 OUT attr，不碰关联。sim 的关联登记逻辑不动。

### 4. sysfs
- /sys/class/ubcore/udma_sim0/ 下属性文件：driver_name/max_jfc/eids/eid0 等
- 读时返回 config 值（kmod 内置设备配置表，或从模块参数读）
- urma 库的 urma_read_sysfs_file 读这些 → 走真 sysfs（不再 sim /tmp 假目录）

## sim 侧改造（方案 A）

### urma_sim.c
- **open 拦截**：`/dev/uburma/<dev>` 不再造假 fd，调 `real_open` 打开真 cdev fd。但要在 sim fd 表登记"这个真 fd 是 cdev"（type=CDEV，但用真 fd 号，不占 ≥100000 段），供 mmap 拦截识别。
- **mmap 拦截**：`urma_sim_fd_is_cdev(fd)` 改为既能查 sim 假 fd 表、也能查"登记的真 cdev fd"。命中则给匿名页（同现状）。
- **ioctl 拦截**：URMA_CMD 不再拦（因为 fd 是真 cdev，透传进内核）——删掉 ioctl 里的 sim 处理分支，或保留但真 cdev fd 不命中假 fd 表自然透传。
- **sysfs 拦截**：如果 kmod 提供真 sysfs，sim 的 /tmp 假目录 + opendir/stat/realpath/read 拦截可逐步移除（或保留兼容）。
- **ummu 拦截**：不动（数据面 tid→seg 仍由 sim 用户态管）。

### urma_sim_dev.c
- `urma_sim_handle_ioctl` 的逻辑搬到 kmod。sim 这边 ioctl 拦截分支删除（透传）。
- 但关联登记（queue_assoc/cq_assoc）相关代码**保留**，因为 mmap 拦截时还要用（set_db_by_idx 等）。

## 验证策略（少跑 VM，TCG 慢）

1. **代码静态走查**：kmod 写完先在容器用 6.6 头树编过（ARCH=arm64 硬编码 KDIR），modinfo 看 vermagic 对。
2. **VM 验证（尽量少次）**：
   - 起一次 VM，insmod kmod，看 dmesg 无 panic、/dev/uburma/<dev> + /sys/class/ubcore/<dev> 出现。
   - 在 VM 里跑 urma_admin show（验 sysfs）+ 跑 test_hw_post（验 ioctl 进内核 + 数据面）。
   - 一次 VM 跑多个检查，减少启动次数。
3. 内核 panic 调试：dmesg + 内核日志，TCG 下慢但要能看。

## 风险

- **mmap 对真 cdev fd 的拦截**：sim 拦 mmap 靠 fd 类型。真 cdev fd 经 open 拿到，sim 要能在 fd 表登记它。若 mmap 对真 cdev fd 时 sim 拦不到（fd 不在表），数据面页就没有，post 跑不了。→ 需先验证 sim 能拦真 cdev fd 的 mmap。
- **kmod 内核态调试**：内核 bug 要 insmod 后看 panic，TCG 慢迭代慢。→ 静态走查尽量写对。
- **cdev/sysfs 权限**：容器内 insmod 要 CAP_SYS_MODULE——但 VM 内是 root，没问题（VM 内 init_module 不是 ENOSYS）。

## 文件

- `kmod/urma_kmod.c` —— kmod 主文件（cdev+sysfs+ioctl handler）
- `kmod/Makefile` —— KDIR 硬编码 6.6，ARCH=arm64
- sim 侧改 urma_sim.c / urma_sim_dev.c（见上）
