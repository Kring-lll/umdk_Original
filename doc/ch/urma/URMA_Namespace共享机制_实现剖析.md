# URMA Namespace 共享机制实现剖析

> 主题：通过 `urma_admin` 配置 `g_dev_ns_shared` / `g_eid_ns_shared` 后，内核具体如何实现 device 与 EID 跨 namespace 共享
> 范围：`01_kernel/drivers/ub/urma/ubcore/` + `00_umdk/umdk/src/urma/tools/urma_admin/`
> 所有结论带 `file:line` 可回溯

---

## 0. 核心论断

跨 ns 共享**不是**"把 device/EID 复制一份给每个 ns"，而是**三个机制叠加**：

| 机制 | 作用 | 触发 |
|---|---|---|
| ① 全局布尔开关（运行期校验短路） | 让访问校验对所有 ns 放行 | `set_dev_ns_mode`/`set_eid_ns_mode` |
| ② sysfs 逻辑设备副本 | 让每个 ns 的 `/sys/class/ubcore` 看得到 device | 切 shared 时 + pernet init + device 注册时 |
| ③ pernet 钩子 | netns 增删时自动维护副本与 EID 归属 | 内核 netns 子系统 |

device 对象与 EID 表**始终全局唯一**（挂 `g_device_list`）。"共享"= 可见性共享 + 访问放行，所有 ns 最终操作同一份底层资源。

---

## 1. 命令下发链路（用户态 → 内核）

### 1.1 device 共享开关

```
urma_admin system set dev_sharing on
  └─ admin_cmd_system.c: cmd_system_set_dev_sharing
       └─ admin_nl_alloc_msg(URMA_CORE_SET_DEV_NS_MODE, 0, UBCORE_GENL)
          + admin_nl_put_u8(msg, UBCORE_ATTR_DEV_NS_MODE, 1)
          + admin_nl_send_recv_msg_default(msg, UBCORE_GENL)
            └─ genl → 内核分发
                 └─ ubcore_genl.c:107  ops[.cmd=UBCORE_CMD_SET_DEV_NS_MODE,
                                       .doit=ubcore_set_dev_ns_mode_ops,
                                       .flags=GENL_ADMIN_PERM]
                      └─ ubcore_genl_admin.c:394  ubcore_set_dev_ns_mode_ops
                           └─ nla_get_u8(UBCORE_ATTR_DEV_NS_MODE) = 1
                                └─ ubcore_device.c:2175  ubcore_set_dev_ns_mode(true)
```

### 1.2 EID 共享开关（完全对称）

```
urma_admin system set eid_sharing on
  └─ URMA_CORE_SET_EID_NS_MODE + UBCORE_ATTR_EID_NS_MODE(u8=1)
       └─ ubcore_genl.c:200  ops[.cmd=UBCORE_CMD_SET_EID_NS_MODE,
                              .doit=ubcore_set_eid_ns_mode_ops, .flags=GENL_ADMIN_PERM]
            └─ ubcore_genl_admin.c:405  ubcore_set_eid_ns_mode_ops
                 └─ ubcore_device.c:2237  ubcore_set_eid_ns_mode(true)
```

### 1.3 状态查询

```
urma_admin system show
  └─ ubcore_genl.c:223  ops[.cmd=UBCORE_CMD_SHOW_SYSTEM, .doit=ubcore_show_system_ops]
       └─ ubcore_genl_admin.c:416  ubcore_show_system_ops
            └─ nla_put_u8(UBCORE_ATTR_DEV_NS_MODE, ubcore_dev_ns_shared()?1:0)
               nla_put_u8(UBCORE_ATTR_EID_NS_MODE, ubcore_eid_ns_shared()?1:0)
               genlmsg_reply  →  用户态打印 DEV_NS_SHARED / EID_NS_SHARED
```

### 1.4 关键常量与绑定

| 项 | 值 | 出处 |
|---|---|---|
| family 名 | `UBCORE_GENL`（`UBCORE_GENL_FAMILY_NAME`） | [ubcore_genl.c:235](01_kernel/drivers/ub/urma/ubcore/ubcore_genl.c#L235) |
| family `.netnsok` | `true`（跨 ns 可见） | [ubcore_genl.c:242](01_kernel/drivers/ub/urma/ubcore/ubcore_genl.c#L242) |
| `UBCORE_CMD_SET_DEV_NS_MODE` | enum 值 40 | [ubcore_cmd.h:40](01_kernel/drivers/ub/urma/ubcore/ubcore_cmd.h#L40) |
| `UBCORE_CMD_SET_EID_NS_MODE` | enum 值 78 | [ubcore_cmd.h:78](01_kernel/drivers/ub/urma/ubcore/ubcore_cmd.h#L78) |
| `UBCORE_CMD_SHOW_SYSTEM` | enum 值 81 | [ubcore_cmd.h:81](01_kernel/drivers/ub/urma/ubcore/ubcore_cmd.h#L81) |
| `UBCORE_ATTR_DEV_NS_MODE` / `EID_NS_MODE` | attr id | [ubcore_genl_define.h:24,40](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_define.h#L24) |
| ops 权限 | `GENL_ADMIN_PERM`（需 CAP_NET_ADMIN） | [ubcore_genl.c:103,203,226](01_kernel/drivers/ub/urma/ubcore/ubcore_genl.c#L103) |

> 两个开关**独立**——可只开 device 共享、保持 EID 隔离，反之亦然。

---

## 2. 机制①：全局布尔开关（运行期校验短路）

### 2.1 开关定义

[ubcore_device.c:79-92](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L79)：

```c
static bool g_dev_ns_shared;   // device 是否跨 ns 共享（默认 false = exclusive）
static bool g_eid_ns_shared;   // EID 是否跨 ns 共享（默认 false = exclusive）

bool ubcore_dev_ns_shared(void) { return READ_ONCE(g_dev_ns_shared); }
bool ubcore_eid_ns_shared(void) { return READ_ONCE(g_eid_ns_shared); }
```

`READ_ONCE` + `g_ubcore_net_rwsem` 写锁保证切换期间的一致性。

### 2.2 `ubcore_set_dev_ns_mode(true)` 核心动作

[ubcore_device.c:2175-2235](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2175)：

```c
down_write(&g_ubcore_net_rwsem);
if (g_dev_ns_shared == shared) { up; return 0; }   // 幂等

// shared→exclusive 分支（此处 shared=true 不进入）
if (g_dev_ns_shared && !list_empty(&g_ubcore_net_list)) { 拒绝 -EPERM; }

// exclusive→shared 分支（!g_dev_ns_shared 进入）：
down_read(&g_device_rwsem);
// (a) 拒绝：若有 device 已有逻辑设备副本（exclusive 下已 expose 过）
list_for_each_entry(dev, &g_device_list, list_node) {
    if (dev->transport_type != UBCORE_TRANSPORT_UB) continue;
    if (!list_empty(&dev->ldev_list)) { 拒绝 -EPERM; }
}
// (b) ★ 为每个 device 在所有已存在 ns 建逻辑设备副本
list_for_each_entry(dev, &g_device_list, list_node) {
    if (dev->transport_type != UBCORE_TRANSPORT_UB) continue;
    ubcore_copy_logic_devices(dev);   // 见 §3
}
// 失败则回滚（ubcore_remove_logic_devices）
up_read(&g_device_rwsem);

g_dev_ns_shared = shared;   // ★ 最后才置位
```

**顺序要点**：先建副本（§3），再置位开关——不能反。置位后新 ns 的 pernet init（§4）会并发建副本，靠 `g_ubcore_net_rwsem` 写锁串行化。`ubcore_set_eid_ns_mode`（[:2237](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2237)）逻辑相同但不建副本（EID 共享只影响校验，无需 sysfs 副本）。

### 2.3 访问放行（机制①落地）

共享开关在**用户态访问入口**短路校验：

**device 放行** `ubcore_dev_accessible`（[ubcore_device.c:1686-1702](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1686)）：

```c
bool ubcore_dev_accessible(struct ubcore_device *dev, struct net *net) {
    struct ubcore_logic_device *ldev;
    if (ubcore_dev_ns_shared() || net_eq(net, read_pnet(&dev->ldev.net)))
        return true;                              // ★ shared 直接放行
    mutex_lock(&dev->ldev_mutex);
    list_for_each_entry(ldev, &dev->ldev_list, node) {   // exclusive: 查 expose 副本
        if (net_eq(read_pnet(&ldev->net), net)) return true;
    }
    mutex_unlock(&dev->ldev_mutex);
    return false;
}
```

**EID 放行** `ubcore_eid_accessible`（[ubcore_device.c:1572-1599](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1572)）：

```c
bool ubcore_eid_accessible(struct ubcore_device *dev, uint32_t eid_index) {
    // 越界/无效检查...
    net = dev->eid_table.eid_entries[eid_index].net;   // EID 自带的 ns 归属
    if (ubcore_eid_ns_shared())
        return true;                          // ★ shared 直接放行，不看 EID 的 net
    else
        return net_eq(net, current->nsproxy->net_ns);  // exclusive: 必须归属 == 调用者 ns
}
```

**EID 列表可见性** 两处过滤点同样短路：
- sysfs `eid_show_cb`（[ubcore_cdev_file.c:833](01_kernel/drivers/ub/urma/ubcore/ubcore_cdev_file.c#L833)）
- ioctl `uburma_fill_eid_list`（[uburma_cmd.c:4083-4084](01_kernel/drivers/ub/urma/uburma/uburma_cmd.c#L4083)）

```c
if (!e->valid || (!ubcore_eid_ns_shared() && !net_eq(e->net, current->nsproxy->net_ns)))
    // 显示空 / 不返回（隐藏）
else
    // 显示真实 EID
```

**访问网关** `ubcore_alloc_ucontext`（[ubcore_device.c:1725-1730](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1725)）：

```c
if (!ubcore_dev_accessible(dev, current->nsproxy->net_ns) ||
    !ubcore_eid_accessible(dev, eid_index))
    return ERR_PTR(-EPERM);   // 拒绝建 context
```

> **重要**：内核数据面（报文收发）路径**不查 ns**——`current->nsproxy` 在内核线程/软中断上下文不稳定（[ubcore_device.c:1548-1567](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1548) 注释明说）。ns 校验只在 `alloc_ucontext` 做一次，建完 context 后数据面畅通。

---

## 3. 机制②：sysfs 逻辑设备副本（让 device 在每个 ns "看得见"）

device 对象全局唯一，但 `/sys/class/ubcore/<name>` 是 **netns-aware** 的 class（`g_ubcore_class` 有 `.ns_type`/`.namespace` 回调，[ubcore_device.c:178-207](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L178)），其 `.namespace` 返回 `read_pnet(&ldev->net)`——决定该 sysfs 节点出现在哪个 ns。

要让同一台 device 在**多个** ns 的 sysfs 都出现，就建多个 `ubcore_logic_device` 副本，每个副本的 `net` 指向一个 ns。

### 3.1 副本创建 `ubcore_add_one_logic_device`

[ubcore_device.c:1159-1193](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1159)：

```c
static int ubcore_add_one_logic_device(struct ubcore_device *dev, struct net *net) {
    mutex_lock(&dev->ldev_mutex);
    // 幂等：已有该 ns 的副本则直接返回
    list_for_each_entry(ldev, &dev->ldev_list, node)
        if (net_eq(read_pnet(&ldev->net), net)) { mutex_unlock; return 0; }

    ldev = kzalloc(sizeof(*ldev), GFP_KERNEL);
    ret = ubcore_create_logic_device(ldev, dev, net);   // 见下
    list_add_tail(&ldev->node, &dev->ldev_list);        // 挂到 device 的副本链表
    mutex_unlock(&dev->ldev_mutex);
    return 0;
}
```

### 3.2 `ubcore_create_logic_device`（副本的"灵魂"）

[ubcore_device.c:1091-1116](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1091)：

```c
static int ubcore_create_logic_device(struct ubcore_logic_device *ldev,
                                      struct ubcore_device *dev, struct net *net) {
    write_pnet(&ldev->net, net);           // ★ 副本归属该 ns
    ldev->ub_dev = dev;                    // ★ 指向同一个全局 device
    // 在该 ns 的 sysfs 建节点（device_create + class 的 .namespace 回调决定归属）
    ldev->dev = device_create(&g_ubcore_class, dev->dev.parent, MKDEV(0,0),
                              ldev, "%s", dev->dev_name);
    ubcore_fill_logic_device_attr(ldev, dev);   // 填 eid/port 等属性
    return 0;
}
```

副本是独立 sysfs kobject，但 `drv_data` 都指向**同一个** `ubcore_device`。**"共享"=可见性共享，不是对象复制。**

### 3.3 批量复制 `ubcore_copy_logic_devices`

[ubcore_device.c:1195-1218](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1195)：

```c
static int ubcore_copy_logic_devices(struct ubcore_device *dev) {
    if (dev->transport_type != UBCORE_TRANSPORT_UB) return 0;
    list_for_each_entry(unet, &g_ubcore_net_list, node) {  // 遍历所有已注册 ns
        if (net_eq(unet->net, dev->ldev.net)) continue;    // 跳过主 ns（主 ldev 已存在）
        ubcore_add_one_logic_device(dev, unet->net);       // 每个 ns 建一个副本
    }
    return ret;
}
```

副本都挂 `dev->ldev_list`（[ubcore_types.h:3484](01_kernel/include/ub/urma/ubcore_types.h#L3484)）。

### 3.4 device 后注册时的补副本

切到 shared 模式后，若之后才 `ubcore_register_device`，在 [ubcore_device.c:1321-1339](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1321)：

```c
down_read(&g_ubcore_net_rwsem);
down_write(&g_device_rwsem);
ubcore_clients_add(dev);
list_add_tail(&dev->list_node, &g_device_list);
if (g_dev_ns_shared)                              // ★ shared 下补副本
    ubcore_copy_logic_devices(dev);
up_write(&g_device_rwsem);
up_read(&g_ubcore_net_rwsem);
```

---

## 4. 机制③：pernet 钩子（netns 增删时自动维护）

切到 shared 后，新容器（新 netns）创建时如何自动获得副本？靠 `g_ubcore_net_ops` 注册的 pernet 子系统（[ubcore_device.c:2564-2569](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2564)）。

### 4.1 netns 创建 `ubcore_net_init`

[ubcore_device.c:2522-2562](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2522)：

```c
static int ubcore_net_init(struct net *net) {
    write_pnet(&unet->net, net);
    if (net_eq(net, &init_net)) { INIT_LIST_HEAD(&unet->node); return 0; }  // init_net 不登记
    down_write(&g_ubcore_net_rwsem);
    list_add_tail(&unet->node, &g_ubcore_net_list);   // ★ 登记新 ns
    if (!g_dev_ns_shared) { up; return 0; }           // exclusive：啥也不做
    // shared：为每个 UB device 在这个新 ns 建逻辑设备副本
    down_read(&g_device_rwsem);
    list_for_each_entry(dev, &g_device_list, list_node) {
        if (dev->transport_type != UBCORE_TRANSPORT_UB) continue;
        ret = ubcore_add_one_logic_device(dev, net);   // ★ 自动建副本
        if (ret) break;
    }
    up_read(&g_device_rwsem);
    up_write(&g_ubcore_net_rwsem);
    if (ret) ubcore_net_exit(net);
    return 0;   // ★ 注释："return ret will cause error starting a container"
                //   即便建副本失败也返回 0，不阻断容器启动
}
```

### 4.2 netns 销毁 `ubcore_net_exit`

[ubcore_device.c:2480-2520](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2480) 按当前模式清理：

```c
void ubcore_net_exit(struct net *net) {
    down_write(&g_ubcore_net_rwsem);
    list_del_init(&unet->node);   // 从 ns 链表摘除

    if (!g_dev_ns_shared) {
        // exclusive：删副本 + EID 重置 + device 迁回 init_net
        list_for_each_entry(dev, &g_device_list, list_node) {
            ubcore_remove_one_logic_device(dev, net);
            ubcore_reset_eid_ns(dev, net);
            ubcore_modify_dev_ns(dev, &init_net, true);
        }
    } else {
        // shared：删副本；EID 按 eid_ns_shared 决定 reset 还是 invalidate
        list_for_each_entry(dev, &g_device_list, list_node) {
            ubcore_remove_one_logic_device(dev, net);
            if (g_eid_ns_shared)
                ubcore_reset_eid_ns(dev, net);        // 可恢复
            else
                ubcore_invalidate_eid_ns(dev, net);   // 标记 invalid + net=&init_net
        }
    }
    up_write(&g_ubcore_net_rwsem);
}
```

`ubcore_invalidate_eid_ns`（[ubcore_device.c:2071](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2071)）把对应 ns 的 EID `valid=false`、`net=&init_net`。

### 4.3 pernet 子系统注册

[ubcore_device.c:2564-2578](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2564)：

```c
static struct pernet_operations g_ubcore_net_ops = {
    .init = ubcore_net_init,
    .exit = ubcore_net_exit,
    .id = &g_ubcore_net_id,
    .size = sizeof(struct ubcore_net)
};
int ubcore_register_pnet_ops(void) { return register_pernet_device(&g_ubcore_net_ops); }
```

`struct ubcore_net`（[:208-211](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L208)）含 `possible_net_t net` + `list_head node`，`g_ubcore_net_list` 记录所有已注册 ns。

---

## 5. 端到端时序（shared 模式下新容器跑 URMA）

```
① 宿主: urma_admin system set dev_sharing on
   → ubcore_set_dev_ns_mode(true)
   → 遍历 g_device_list，为每个 device 在所有已存在 ns 建 ldev 副本（机制②）
   → g_dev_ns_shared = true（机制①置位）

② 容器启动 (unshare CLONE_NEWNET / docker run --network=container:...)
   → 内核 ubcore_net_init(new_net)
   → 登记 g_ubcore_net_list
   → g_dev_ns_shared=true → 为每个 UB device 在 new_net 建 ldev 副本（机制③）
   → /sys/class/ubcore/<dev> 自动出现在容器 sysfs

③ 容器内进程
   → liburma 扫 /sys/class/ubcore 拿到 device（可见性已满足）
   → open /dev/uburma/<dev>   ← ★ 需 bind-mount（uburma class 非 netns-aware，见 §7）
   → ioctl(URMA_CMD, CREATE_CTX, eid_index)
   → ubcore_alloc_ucontext
      → ubcore_dev_accessible: g_dev_ns_shared=true → 放行（机制①）
      → ubcore_eid_accessible: 若 eid_sharing 也 on → 放行
      → ubcore_cgroup_try_charge（资源配额）
      → dev->ops->alloc_ucontext → context 建成功
   → 数据面（send/recv）打通，此后不再查 ns
```

---

## 6. 为什么两个开关独立 + 切换有约束

### 6.1 独立性

- **device 共享** 解决 sysfs/cdev 可见性 + `dev_accessible` 放行。
- **EID 共享** 解决 EID 列表可见性（`eid_accessible`/`fill_eid_list`/`eid_show`）。
- 可只开 device 共享：device 全 ns 可见，但每个 ns 只能用归属自己的 EID（`eid_accessible` 仍按 `e->net == 当前 ns` 校验）。

### 6.2 切换约束

| 切换方向 | 拒绝条件 | 出处 |
|---|---|---|
| shared→exclusive | 有自定义 ns 存在（`g_ubcore_net_list` 非空） | [ubcore_device.c:2187](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2187) |
| exclusive→shared | 有 device 已 `ldev_list` 非空（exclusive 下已 `dev expose` 过） | [ubcore_device.c:2196-2208](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2196) |
| shared 下 `dev expose` | 直接拒绝（shared 已全可见，expose 无意义） | [ubcore_device.c:2265](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2265) |

约束原因：切换会破坏既有 ns 归属/副本，故要求"干净状态"。

---

## 7. 关键澄清（易误解点）

1. **"共享"≠"复制对象"**：`ubcore_device` 与 EID 表始终全局唯一（挂 `g_device_list`）。共享只是①建 sysfs 副本（可见性）+ ②访问校验短路（放行）。所有 ns 操作同一份底层资源。

2. **`/dev/uburma/*` 与 `/dev/ubagg` 不自动跨 ns**：uburma/ubagg 的 class **不是** netns-aware（无 `.ns_type`/`.namespace` 回调，[uburma_main.c:81-84](01_kernel/drivers/ub/urma/uburma/uburma_main.c#L81) / [ubagg_main.c:87-90](01_kernel/drivers/ub/urma/ubagg/ubagg_main.c#L87)），即便 `dev_sharing=on`，这些 `/dev` 节点也只在 init_net 的 devtmpfs。**容器内必须 bind-mount**：
   ```bash
   docker run --device /dev/uburma/ubdma0:/dev/uburma/ubdma0 \
              --device /dev/ubagg:/dev/ubagg ...
   # 或
   mount --bind /dev/uburma/ubdma0  /path/to/ctr/dev/uburma/ubdma0
   mount --bind /dev/ubagg          /path/to/ctr/dev/ubagg
   ```
   `/sys/class/ubcore/*` 因 ubcore class 是 netns-aware，shared 模式下自动出现在容器 sysfs，无需手动处理。

3. **bonding EID 不参与隔离**：[ubagg_ioctl.c:1304](01_kernel/drivers/ub/urma/ubagg/ubagg_ioctl.c#L1304) 强制 `eid_entries[0].net = &init_net`，聚合口 EID 始终全局可见。受 `eid_sharing` 控制的是底层 primary/port EID。

4. **数据面不查 ns**：ns 校验只在 `alloc_ucontext` 做一次；建链后收发不再校验，避免内核线程 `current->nsproxy` 不稳定（[ubcore_device.c:1548-1567](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1548)）。

5. **genl `.netnsok=true` 的含义**：family 注册到 init_net 全局 genl 表，所有 netns 用户都能发命令（控制面通道跨 ns）；但这只让"命令能发出去"，设备隔离仍由 §2.3 的 accessible 校验决定。

---

## 8. 附录：精细化暴露（exclusive 模式下的替代方案）

若不切全局 shared，可用 `urma_admin` 把单个 device/EID 暴露到指定 ns（走 `ns_fd`，不 setns）：

| 命令 | 内核 ops | 实现 |
|---|---|---|
| `urma_admin dev expose <dev> <netns>` | `ubcore_expose_dev_ns_ops` [:464](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L464) | `ubcore_expose_dev_ns` [:2258](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2258)：`get_net_ns_by_fd` → 建单个 ldev 副本 |
| `urma_admin dev unexpose <dev> <netns>` | `ubcore_unexpose_dev_ns_ops` [:475](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L475) | 撤销副本 |
| `urma_admin eid set <dev> <idx> ns <netns>` | `ubcore_set_dev_eid_ns_ops` [:486](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L486) | `ubcore_set_dev_eid_ns` [:2409](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2409)：改单个 EID 的 `net` 归属 |
| `urma_admin dev set <dev> ns <netns>` | `ubcore_set_dev_ns_ops` [:453](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L453) | `ubcore_set_dev_ns` [:2128](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2128)：device 整体迁 ns |

`<netns>` 格式 = `/proc/$pid/ns/net`，用户态 `admin_get_ns_fd`（[admin_parameters.c:406](00_umdk/umdk/src/urma/tools/urma_admin/admin_parameters.c#L406)）解析拿 fd，工具自身不 setns，只把 ns_fd 经 netlink 传内核。

精细化暴露与全局 shared **互斥**（§6.2）。

---

## 9. Shared 模式切换演练脚本

完整演示从 exclusive 切到 shared、验证、再切回的端到端流程。需 root 权限在宿主机执行。

### 9.1 前置检查脚本 `urma_ns_preflight.sh`

```bash
#!/bin/bash
# urma_ns_preflight.sh — shared 模式切换前置检查
set -euo pipefail

echo "=== [1] 内核模块 ==="
for m in ubcore ubagg uburma udma ubfi ummu_core ummu ubus ubase unic cdma; do
    if lsmod | grep -qw "$m"; then echo "  [OK]   $m"; else echo "  [MISS] $m"; fi
done

echo "=== [2] genl family 注册 ==="
for f in ubcore ubagg; do
    if cat /proc/net/genl 2>/dev/null | grep -qi "$f"; then echo "  [OK]   genl: $f"; else echo "  [MISS] genl: $f"; fi
done

echo "=== [3] urma_admin 工具 ==="
if command -v urma_admin >/dev/null 2>&1; then echo "  [OK]   $(command -v urma_admin)"; else echo "  [MISS] urma_admin"; fi

echo "=== [4] 当前共享模式 ==="
urma_admin system show 2>/dev/null || echo "  (查询失败，检查 urma_admin/权限)"

echo "=== [5] device 列表 ==="
ls /sys/class/ubcore/ 2>/dev/null || echo "  /sys/class/ubcore 不存在"

echo "=== [6] 字符设备节点 ==="
ls -l /dev/uburma/* /dev/ubagg 2>/dev/null || echo "  /dev/uburma/* 或 /dev/ubagg 不存在"

echo "=== [7] 已存在的自定义 netns ==="
ip netns list 2>/dev/null || true

echo "=== [8] 权限 ==="
ls -ld /usr/lib64/urma /dev/ummu/tid 2>/dev/null || true
```

### 9.2 切换主脚本 `urma_ns_switch_shared.sh`

```bash
#!/bin/bash
# urma_ns_switch_shared.sh <on|off>  — 切换 device + EID 全局共享
# 约束：切 on 前不能有 device 已 expose（ldev_list 非空）；
#       切 off 前不能有自定义 netns 存在（否则内核返回 -EPERM）
set -euo pipefail
MODE="${1:?usage: $0 <on|off>}"

case "$MODE" in
  on)  WANT=on  ;;
  off) WANT=off ;;
  *)   echo "arg must be on|off"; exit 1 ;;
esac

echo ">>> 切换前状态:"
urma_admin system show

if [ "$WANT" = on ]; then
    echo ">>> 检查约束: 切 on 前不应有 device 已 expose"
    # 若之前在 exclusive 下做过 dev expose，需先 unexpose（按实际设备名）
    # urma_admin dev unexpose <dev> /proc/<pid>/ns/net

    echo ">>> 开启 device 共享"
    urma_admin system set dev_sharing on
    echo ">>> 开启 EID 共享"
    urma_admin system set eid_sharing on
else
    echo ">>> 检查约束: 切 off 前不能有自定义 netns"
    if [ -n "$(ip netns list 2>/dev/null)" ]; then
        echo "  [WARN] 存在自定义 netns，切 off 会被内核拒绝。请先删除:"
        ip netns list
        exit 2
    fi
    echo ">>> 关闭 device 共享"
    urma_admin system set dev_sharing off
    echo ">>> 关闭 EID 共享"
    urma_admin system set eid_sharing off
fi

echo ">>> 切换后状态:"
urma_admin system show
```

### 9.3 完整演练流程

```bash
chmod +x urma_ns_preflight.sh urma_ns_switch_shared.sh

# ① 前置检查
./urma_ns_preflight.sh

# ② 切到 shared（device + EID 全开）
./urma_ns_switch_shared.sh on
# 期望输出：
#   DEV_NS_SHARED: on
#   EID_NS_SHARED: on
# dmesg | grep "ns mode"  →  "Dev ns mode changed to shared" / "Eid ns mode changed to shared"

# ③ 创建一个测试 netns（shared 模式下会自动为每个 device 建 sysfs 副本）
ip netns add ns1
# 验证：ns1 内能看到 ubcore 设备（sysfs 自动可见）
ip netns exec ns1 ls /sys/class/ubcore/

# ④ 在 ns1 内跑 URMA（需先 bind-mount /dev 节点，见 §10）
ip netns exec ns1 bash -c '
  mount --bind /dev/uburma/ubdma0 /dev/uburma/ubdma0 2>/dev/null  # 仅示例，实际见 §10
  urma_ping <dst_eid> -c 3
'

# ⑤ 切回 exclusive（必须先删自定义 netns）
ip netns del ns1
./urma_ns_switch_shared.sh off
```

### 9.4 精细化暴露演练（exclusive 模式，不全局共享）

```bash
# 确保 exclusive 模式
./urma_ns_switch_shared.sh off

# 创建容器 netns（docker 方式，见 §10.2；或 ip netns）
ip netns add ctr1

# 把 device dev0 暴露到 ctr1 的 netns
# <netns> 支持两种格式：/var/run/netns/<name> 或 /proc/<pid>/ns/net
urma_admin dev expose ubdma0 /var/run/netns/ctr1
# 或用容器 pid:
# CTR_PID=$(docker inspect -f '{{.State.Pid}}' myctr)
# urma_admin dev expose ubdma0 /proc/$CTR_PID/ns/net

# 把 dev0 的 EID idx=1 绑定到 ctr1 的 netns
urma_admin eid set ubdma0 1 ns /var/run/netns/ctr1

# 暴露聚合设备及关联 EID
urma_admin agg expose 192.168.1.100 /var/run/netns/ctr1

# 验证：ctr1 内能看到 device 与对应 EID
ip netns exec ctr1 ls /sys/class/ubcore/
ip netns exec ctr1 cat /sys/class/ubcore/ubdma0/eids/eid1 2>/dev/null

# 撤销
urma_admin dev unexpose ubdma0 /var/run/netns/ctr1
ip netns del ctr1
```

> `<netns>` 格式由用户态 `admin_get_ns_fd`（[admin_parameters.c:406](00_umdk/umdk/src/urma/tools/urma_admin/admin_parameters.c#L406)）解析：接受 `/var/run/netns/<name>` 和 `/proc/<pid>/ns/net` 两种，纯数字会被当作 pid（[:326-400](00_umdk/umdk/src/urma/tools/urma_admin/admin_parameters.c#L326)）。工具自身不 setns，只 open ns 文件拿 fd 传给内核。

### 9.5 切换约束速查

| 操作 | 前置条件 | 失败原因 |
|---|---|---|
| `dev_sharing on` | 无 device 已 `dev expose`（`ldev_list` 空） | 有则 `-EPERM "has logic devices"` |
| `dev_sharing off` | 无自定义 netns 存在 | 有则 `-EPERM "existing ns"` |
| `eid_sharing off` | 无自定义 netns 存在 | 同上 |
| `dev expose`（shared 模式下） | 必须在 exclusive 模式 | shared 下直接 `-EPERM "Can not expose under shared ns mode"` |

---

## 10. Docker / K8s 部署 Bind-mount 示例

**核心问题**：ubcore class 是 netns-aware（`/sys/class/ubcore/*` 在 shared 模式下自动出现在容器 sysfs），但 **uburma/ubagg class 非 netns-aware**（[uburma_main.c:81-84](01_kernel/drivers/ub/urma/uburma/uburma_main.c#L81)、[ubagg_main.c:87-90](01_kernel/drivers/ub/urma/ubagg/ubagg_main.c#L87)）——`/dev/uburma/*` 和 `/dev/ubagg` 只在 init_net 的 devtmpfs，容器内**默认看不到**，必须 bind-mount。

### 10.1 先查设备节点与设备号

```bash
# 宿主机执行，查所有 URMA 字符设备节点 + 主从设备号
ls -l /dev/uburma/* /dev/ubagg
# 示例输出：
#   crw------- 1 root root 243, 0  /dev/uburma/ubdma0
#   crw------- 1 root root 243, 1  /dev/uburma/ubdma1
#   crw------- 1 root root 242, 0  /dev/ubagg
# 记下 major:minor，后续 mknod 方式要用

ls /sys/class/ubcore/   # 列出所有 device 名，与 /dev/uburma/* 对应
```

### 10.2 Docker 部署（shared 模式）

**方式 A — `--device` 挂载（推荐，最简单）**：

```bash
# 前提：宿主已 urma_admin system set dev_sharing on + eid_sharing on

docker run -it --rm \
  --device /dev/uburma/ubdma0 \
  --device /dev/ubagg \
  --cap-add=IPC_LOCK \
  --cap-add=SYS_PTRACE \
  -v /usr/lib64/liburma.so*:/usr/lib64/ \
  -v /usr/lib64/urma:/usr/lib64/urma:ro \
  your-urma-app-image \
  sh -c 'urma_admin system show && urma_ping <dst_eid> -c 3'
```

> `--device` 会自动在容器内 mknod 同主从设备号节点。`IPC_LOCK` 通常用于锁内存（URMA 可能需要），按实际调整。库文件也可打进镜像。

**方式 B — bind-mount `/dev` 子树**（挂多个设备时方便）：

```bash
# 先在宿主建挂载点目录树
mkdir -p /tmp/urma-ctr-dev/uburma
mount --bind /dev/uburma /tmp/urma-ctr-dev/uburma
mount --bind /dev/ubagg /tmp/urma-ctr-dev/ubagg 2>/dev/null || true

docker run -it --rm \
  -v /tmp/urma-ctr-dev/uburma:/dev/uburma:ro \
  -v /tmp/urma-ctr-dev/ubagg:/dev/ubagg:ro \
  your-urma-app-image \
  urma_ping <dst_eid> -c 3
```

**方式 C — 容器内 mknod（无 `--device` 时，需 `--cap-add=MKNOD` + 已知设备号）**：

```bash
docker run -it --rm \
  --cap-add=MKNOD \
  --privileged=false \
  your-urma-app-image sh -c '
    # 设备号从宿主 ls -l 查得（见 §10.1）
    mknod /dev/uburma/ubdma0 c 243 0 && chmod 600 /dev/uburma/ubdma0
    mkdir -p /dev/ubagg && mknod /dev/ubagg c 242 0 && chmod 600 /dev/ubagg
    urma_ping <dst_eid> -c 3
  '
```

**方式 D — 容器共享宿主 network namespace（不建独立 netns）**：

```bash
# --network=host 时容器用宿主 netns，无需考虑 ns 可见性
# 但 dev_sharing/eid_sharing 仍需 on 才能让容器进程通过 alloc_ucontext 校验
docker run -it --rm --network=host \
  --device /dev/uburma/ubdma0 --device /dev/ubagg \
  your-urma-app-image urma_ping <dst_eid> -c 3
```

### 10.3 Docker 部署（exclusive 模式 + 精细化暴露）

exclusive 模式下，先把 device/EID 暴露到容器 netns，再启动容器：

```bash
# ① 先起一个占位容器拿到其 netns（或用 ip netns add）
ip netns add ctr1-netns

# ② 暴露 device + EID 到该 netns
urma_admin dev expose ubdma0 /var/run/netns/ctr1-netns
urma_admin eid set ubdma0 1 ns /var/run/netns/ctr1-netns

# ③ 容器复用该 netns + bind-mount /dev 节点
docker run -it --rm \
  --network=none \
  --device /dev/uburma/ubdma0 \
  --device /dev/ubagg \
  your-urma-app-image \
  urma_ping <dst_eid> -c 3

# docker 的 netns 与 ip netns 命名 ns 是同一套；--network=none 后用 nsenter 注入也可：
# PID=$(docker inspect -f '{{.State.Pid}}' <ctr>)
# urma_admin dev expose ubdma0 /proc/$PID/ns/net
```

### 10.4 K8s 部署

**方式 A — Device Plugin + hostPath 挂载字符设备**（生产推荐）：

K8s 不直接支持 `--device`，需通过 device plugin 暴露 URMA 设备资源，或用 hostPath + initContainer mknod。

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: urma-app
spec:
  replicas: 2
  selector:
    matchLabels: { app: urma-app }
  template:
    metadata:
      labels: { app: urma-app }
    spec:
      hostNetwork: true          # 用宿主 netns（shared 模式下最省事）
      containers:
      - name: app
        image: your-urma-app-image:latest
        command: ["urma_ping", "<dst_eid>", "-c", "3"]
        securityContext:
          capabilities:
            add: ["IPC_LOCK", "SYS_PTRACE"]
        volumeMounts:
        - name: uburma-dev
          mountPath: /dev/uburma
        - name: ubagg-dev
          mountPath: /dev/ubagg
        - name: urma-lib
          mountPath: /usr/lib64/urma
          readOnly: true
      volumes:
      - name: uburma-dev
        hostPath: { path: /dev/uburma, type: Directory }
      - name: ubagg-dev
        hostPath: { path: /dev/ubagg, type: File }   # 或 CharDevice
      - name: urma-lib
        hostPath: { path: /usr/lib64/urma, type: Directory }
```

> `hostNetwork: true` 让 Pod 用宿主 netns。若要独立 netns（exclusive + expose），见方式 B。

**方式 B — 独立 Pod netns + initContainer 暴露**（exclusive 精细化）：

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: urma-ctr
spec:
  shareProcessNamespace: true   # 让 init 与主容器同 ns 便于 urma_admin 操作
  initContainers:
  - name: expose
    image: your-urma-admin-image:latest   # 含 urma_admin
    securityContext: { privileged: true } # 需能访问宿主 /proc 和 netlink
    command:
    - sh
    - -c
    - |
      # Pod 的 netns 由 pause 容器建立；这里拿 pause pid 暴露 device
      # 注：initContainer 与主容器共享 netns（shareProcessNamespace）
      PEER_PID=1   # pause 进程 pid（同 ns）
      urma_admin dev expose ubdma0 /proc/$PEER_PID/ns/net
      urma_admin eid set ubdma0 1 ns /proc/$PEER_PID/ns/net
  containers:
  - name: app
    image: your-urma-app-image:latest
    command: ["urma_ping", "<dst_eid>", "-c", "3"]
    volumeMounts:
    - { name: uburma-dev, mountPath: /dev/uburma }
    - { name: ubagg-dev,  mountPath: /dev/ubagg }
  volumes:
  - { name: uburma-dev, hostPath: { path: /dev/uburma, type: Directory } }
  - { name: ubagg-dev,  hostPath: { path: /dev/ubagg,  type: File } }
```

> 精细化暴露需要 `urma_admin` 在 Pod netns 建好后执行，时序敏感，生产环境建议优先用 shared 模式 + hostNetwork（方式 A）。

### 10.5 容器内验证清单

容器启动后，在容器内执行：

```bash
# ① sysfs 能看到 device（shared 自动可见 / exclusive 需 expose）
ls /sys/class/ubcore/
cat /sys/class/ubcore/ubdma0/eids/eid* 2>/dev/null   # EID 不为空（eid_sharing on 或已 eid set）

# ② /dev 节点存在（bind-mount 成功）
ls -l /dev/uburma/* /dev/ubagg

# ③ 库可用
ldconfig -p | grep liburma

# ④ 端到端
urma_admin system show    # 查模式（shared 下应 on）
urma_ping <dst_eid> -c 3  # 数据面打通
```

### 10.6 部署排障

| 容器内现象 | 原因 | 处置 |
|---|---|---|
| `ls /sys/class/ubcore` 为空 | shared 未开 或 exclusive 未 expose | 宿主 `urma_admin system show`；开 shared 或 `dev expose` |
| `open /dev/uburma/x` → ENOENT | `/dev` 节点未 bind-mount | `--device` 或 hostPath 挂载 |
| `alloc_ucontext` → EPERM | `dev_accessible`/`eid_accessible` 未通过 | 开 `dev_sharing`+`eid_sharing`，或 `eid set ... ns` |
| `urma_admin` 在容器内报权限错 | genl 需 `CAP_NET_ADMIN` | 容器加 `--cap-add=NET_ADMIN` 或 `privileged` |
| K8s hostPath CharDevice 报错 | `/dev/ubagg` 是字符文件不是目录 | hostPath type 用 `File`/`CharDevice`，`/dev/uburma` 用 `Directory` |
| 多 Pod 抢同一 device 资源超限 | cgroup rdma 配额 | 调 `HCA_HANDLE`/`HCA_OBJECT` 限额 |

---

## 11. 源码索引速查

| 主题 | 位置 |
|---|---|
| 开关定义 | [ubcore_device.c:79-92](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L79) |
| `ubcore_set_dev_ns_mode` | [ubcore_device.c:2175](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2175) |
| `ubcore_set_eid_ns_mode` | [ubcore_device.c:2237](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2237) |
| `ubcore_dev_accessible`（device 放行） | [ubcore_device.c:1686](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1686) |
| `ubcore_eid_accessible`（EID 放行） | [ubcore_device.c:1572](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1572) |
| `ubcore_alloc_ucontext`（访问网关） | [ubcore_device.c:1725](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1725) |
| `ubcore_create_logic_device`（建副本） | [ubcore_device.c:1091](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1091) |
| `ubcore_add_one_logic_device` | [ubcore_device.c:1159](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1159) |
| `ubcore_copy_logic_devices` | [ubcore_device.c:1195](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1195) |
| `ubcore_net_init` / `ubcore_net_exit` | [ubcore_device.c:2522](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2522) / [:2480](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2480) |
| `g_ubcore_net_ops` 注册 | [ubcore_device.c:2564](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L2564) |
| device 注册时补副本 | [ubcore_device.c:1325](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L1325) |
| netns-aware class 回调 | [ubcore_device.c:178-207](01_kernel/drivers/ub/urma/ubcore/ubcore_device.c#L178) |
| genl family 定义（`.netnsok`） | [ubcore_genl.c:233-246](01_kernel/drivers/ub/urma/ubcore/ubcore_genl.c#L233) |
| `set_dev_ns_mode_ops` / `set_eid_ns_mode_ops` | [ubcore_genl_admin.c:394](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L394) / [:405](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L405) |
| `show_system_ops` | [ubcore_genl_admin.c:416](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L416) |
| `expose_dev_ns_ops` / `set_dev_ns_ops` / `set_dev_eid_ns_ops` | [ubcore_genl_admin.c:464](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L464) / [:453](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L453) / [:486](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_admin.c#L486) |
| ops 绑定位置 | [ubcore_genl.c:107,200,223](01_kernel/drivers/ub/urma/ubcore/ubcore_genl.c#L107) |
| 命令枚举 | [ubcore_cmd.h:40-44,78,81](01_kernel/drivers/ub/urma/ubcore/ubcore_cmd.h#L40) |
| attr 枚举 | [ubcore_genl_define.h:24-40](01_kernel/drivers/ub/urma/ubcore/ubcore_genl_define.h#L24) |
| sysfs EID 过滤 `eid_show_cb` | [ubcore_cdev_file.c:833](01_kernel/drivers/ub/urma/ubcore/ubcore_cdev_file.c#L833) |
| ioctl EID 过滤 `uburma_fill_eid_list` | [uburma_cmd.c:4066](01_kernel/drivers/ub/urma/uburma/uburma_cmd.c#L4066)（过滤 :4083） |
| bonding EID 固定 init_net | [ubagg_ioctl.c:1304](01_kernel/drivers/ub/urma/ubagg/ubagg_ioctl.c#L1304) |
| 用户态 `admin_nl_set_eid_sharing` | [admin_cmd_system.c:177](00_umdk/umdk/src/urma/tools/urma_admin/admin_cmd_system.c#L177) |
| 用户态 `admin_get_ns_fd` | [admin_parameters.c:406](00_umdk/umdk/src/urma/tools/urma_admin/admin_parameters.c#L406) |

---

*本剖析基于源码走读生成，结论可回溯至上述 file:line。核心论断：共享 = 全局布尔校验短路 + sysfs 逻辑设备副本 + pernet 钩子自动维护，三者叠加；device/EID 对象始终全局唯一。源码演进时请以实际代码为准。*
