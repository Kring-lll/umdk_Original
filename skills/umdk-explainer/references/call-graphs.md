# UMDK 典型调用链（框架特异，由 umdk-skill-updater 维护）

> 本文件沉淀**跨 ≥3 文件**的调用链，省去每次现编的成本。
> 未沉淀的链路由 explainer 现编，并提示"本次现编，建议沉淀"——沉淀后追加到此文件。
> 每条链路标 `file:line`。链路失效（API 变更）时由 updater 重走。

## 链路 1：URMA 单边 read（数据面）

```
urma_read()                                   # 用户态便捷封装
  └─ urma_post_jfs_wr() / urma_post_jetty_send_wr()   # 提交 WR 到 JFS
       └─ 经 ioctl 提交 CMD                             # umdk 内可见的最后一层
  └─ urma_poll_jfc()                            # 轮询完成队列取 CQE
```

> `urma_read` / `urma_post_*` / `urma_poll_jfc` 定义在 `umdk/src/urma/lib/urma/core/` 下，
> 精确 file:line 待 updater 补全。ioctl 之后的下层处理不在 `umdk/` 内，链路自然止步。
> 文档参考：`doc/ch/urma/URMA User Guide.ch.md` §5.3.1 单边操作流程。

## 链路 2：URMA 单边 write（数据面）

```
urma_write()
  └─ post WR → JFS → ioctl 提交 CMD → 对端写指定内存
  └─ 对端回 TAACK → 本端 JFC 产生 CQE → urma_poll_jfc
```

> 文档参考：§5.3.1 write 流程（用户组装报文→目标存储→TAACK→用户端）。

## 链路 3：URMA 双边 send/recv（消息语义）

```
发送端: urma_send() / urma_post_jfs_wr() → JFS(SQ) → ... → 对端 JFR(RQ)
接收端: urma_recv() / urma_post_jfr_wr() → 预下 RQE → 报文到达填入 RQE → JFC CQE → urma_poll_jfc
```

> 注意：SEND 必须在接收端已下发 RECV 之后才能成功。
> IB 传输层需先 `urma_advise_jfr` 建立 JFS→JFR 通道；UB 天然一对多可省。

## 链路 4：RC 模式建链（控制面）

```
1. 带外交换 rjetty: urma_get_rjetty() → 序列化传对端 → 对端 urma_put_rjetty()
2. 导入对端: urma_import_jetty() [不感知tp] 或 urma_import_jetty_ex() [感知tp]
3. 绑定: urma_bind_jetty() / urma_bind_jetty_ex()   # 构造传输通道
   └─ 经 ioctl 触发建链状态机 (REQ/REP/ACK)
拆链: urma_unbind_jetty() → urma_unimport_jetty()
```

> 文档参考：§5.1 管理面、§5.1.1.1 可靠建链协议（CONN_REQ/REP/ACK）。

## 链路 5：Segment 注册与远端导入（内存语义前置）

```
Target 侧: urma_register_seg(ctx, seg_cfg) → 返回 urma_target_seg_t（含 token_id、ubva）
Initiator 侧: urma_import_seg(ctx, seg, token, addr, ...) → 拿到 target_seg.mva
              → 之后单边 read/write 用 mva 访问对端内存
释放: urma_unimport_seg() / urma_unregister_seg()
```

> 文档参考：§5.2.3 Segment 管理。

## 链路 6：异步异常事件处理

```
用户态 urma_get_async_event(ctx, &event) 取事件
  → 按 event 类型分类处理（打印/重试/flush）
  → urma_ack_async_event(&event) 确认（删除对象前必须 ack 其未确认事件）
```

> 事件类型见 [data-structures.md](data-structures.md) 异常事件枚举。
> 文档参考：§5.2.4 异常事件、§5.2.4.1 flush jetty。

## 链路 7：ioctl 边界（用户态侧）

```
liburma.so: 调 ioctl(fd, UBRMA_CMD_*, arg)   # umdk/ 侧的调用点
  └─ 之后的 CMD 分发与硬件交互不在 umdk/ 内，链路止步
```

> 用户态侧的 ioctl 调用点可在 `umdk/src/urma/lib/urma/core/` 实现里定位（精确 file:line 待 updater 补全）。
> 追踪任意用户态 API 时，若它在 `umdk/` 内最终经 ioctl 提交，链路止于该 ioctl 调用点。

## 沉淀记录

| 日期 | 链路 | 状态 |
|------|------|------|
| 2026-07-07 | 链路 1~7 骨架 | 初创，行号待 updater 精化 |

## 待沉淀（现编过的候选）

> 当 explainer 现编过一条跨多文件的链路且具复用价值时，在此登记，updater 下次运行时补全行号并上移到正式链路。

- （空）
