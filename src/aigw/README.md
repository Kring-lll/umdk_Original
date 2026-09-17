# AIGW - AI推理网关

<div align="center">

![Go Version](https://img.shields.io/badge/Go-1.24+-00ADD8?style=flat&logo=go)
![License](https://img.shields.io/badge/License-Proprietary-red)
![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen)

**高性能 · 智能调度 · 企业级**

</div>

---

## 简介

AIGW (AI Gateway) 是一个企业级的大语言模型推理网关，专为大规模LLM推理场景设计。它提供智能调度、负载均衡、缓存管理等核心能力，为AI推理服务提供高性能、高可用的支撑。

### 核心价值

- **极致性能**：通过智能调度和缓存优化，显著降低推理延迟
- **灵活部署**：支持独立服务和共享库两种部署模式，适应不同场景
- **高可用性**：多实例部署、自动故障转移，保障服务稳定运行
- **易于集成**：提供C API和RESTful接口，轻松集成到现有系统

---

## 功能特性

### 🚀 核心能力

| 功能 | 描述 |
|------|------|
| **智能调度** | 支持Token、时间感知、容量、ERR等多种负载均衡策略 |
| **智能路由** | 区分prefill和decode阶段，实现上下文感知的请求分发 |
| **缓存管理** | Redis和本地缓存双重缓存机制，有效降低重复请求开销 |
| **延迟预测** | 基于LightGBM和EMA算法的机器学习预测，优化调度决策 |
| **服务发现** | 集成ZooKeeper实现实例注册与发现 |
| **监控告警** | 内置统计、日志和告警系统，实时掌握服务状态 |
| **多模型支持** | 支持同时管理多个大语言模型，灵活切换 |
| **分词器集成** | 集成HuggingFace Tokenizers实现高效文本分词 |

### 🛡️ 安全与可靠性

- **安全通信**：支持HMAC、AES-GCM、TLS等多种安全机制
- **健康检查**：自动检测实例健康状态，及时剔除故障节点
- **限流保护**：支持请求限流，防止服务过载

---

## 技术架构

### 技术栈

```
┌─────────────────────────────────────────────────────────────┐
│                      应用层                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────┐ │
│  │  RESTful API │  │   C API      │  │  监控面板 │ │
│  └──────────────┘  └──────────────┘  └──────────┘ │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│                      网关层 (Go)                         │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐ │
│  │ 调度器   │ │ 负载均衡 │ │ 缓存中心 │ │ 监控   │ │
│  └──────────┘ └──────────┘ └──────────┘ └────────┘ │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│                      基础设施层                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐ │
│  │ ZooKeeper │ │  Redis   │ │ LightGBM │ │ 日志   │ │
│  └──────────┘ └──────────┘ └──────────┘ └────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 多语言支持

| 语言 | 用途 |
|------|------|
| **Go** | 核心业务逻辑、HTTP服务、调度算法 |
| **Rust** | 高性能分词器（通过FFI集成） |
| **C/C++** | CGO共享库、缓存驱动接口 |
| **Python** | LightGBM模型训练、HuggingFace Tokenizers |

---

## 项目结构

```
AIGW/
├── cmd/                        # 命令行入口
│   └── aigw/
│       └── main.go           # 主程序入口
├── internal/                   # 内部包
│   ├── core/                 # 核心管理器
│   ├── gs/                   # 全局调度器
│   ├── cachecenter/          # 缓存中心
│   ├── server/               # HTTP服务器
│   ├── zk/                   # ZooKeeper集成
│   ├── tokenizers/           # 分词器
│   ├── vectorizer/           # 特征向量化
│   ├── alarmmonitor/          # 告警监控
│   ├── modelmonitor/         # 模型监控
│   └── stats/               # 统计模块
├── pkg/                        # 公共包
│   ├── lightgbm/             # LightGBM集成
│   ├── latencyprediction/    # 延迟预测
│   ├── log/                  # 日志模块
│   ├── crypto/               # 加密模块
│   ├── sock/                 # Socket通信
│   └── utils/                # 工具函数
├── src/                        # CGO源码
│   └── libaigw.go          # C API实现
├── include/                    # C API头文件
│   └── aigw.h             # C API接口定义
├── example/                    # 示例代码
│   ├── cgo/                 # C语言示例
│   └── CMakeLists.txt       # 示例构建配置
├── configs/                    # 配置文件
│   └── aigw.json           # 主配置文件
├── build/                      # 构建脚本
├── open_source/                # 第三方依赖
└── output/                     # 构建输出
```

---

## 快速开始

### 环境要求

| 组件 | 版本要求 | 说明 |
|------|----------|------|
| Go | 1.21+ | 核心开发语言 |
| Rust | 1.70+ | 分词器编译 |
| CMake | 3.10+ | 构建工具 |
| GCC/Clang | - | C/C++编译器 |
| Python | 3.8+ | LightGBM模型训练（可选） |
| ZooKeeper | 3.5+ | 服务发现（必需） |
| Redis | 5.0+ | 缓存服务（可选） |

### 构建项目

```bash
# 基础构建
./build.sh

# 带单元测试构建
./build.sh --ut

# 调试模式构建
./build.sh --debug

# 清理构建产物
./build.sh --clean
```

### 构建选项

| 选项 | 说明 |
|------|------|
| `--coverage` | 启用代码覆盖率 |
| `--debug` | 启用调试编译 |
| `--fuzz` | 启用模糊测试 |
| `--ut` | 启用单元测试 |
| `--at` | 启用集成测试 |
| `--rpm` | 构建RPM包 |
| `--version` | 指定版本号 |
| `--release` | 指定发布号 |
| `--notest` | 移除所有测试文件 |
| `--tsan` | 启用线程安全检查 |
| `--clean` | 清理构建产物 |

### 运行服务

```bash
# 作为独立服务运行
./output/aigw --config=/etc/aigw/conf/aigw.json
```

---

## 配置说明

配置文件采用JSON格式，主要包含以下部分：

### 全局配置

```json
{
  "global": {
    "host": "",                    // 监听地址
    "port": "",                    // 监听端口
    "logPath": "/var/log/aigw",  // 日志路径
    "logLevel": "info",           // 日志级别: debug/info/warn/error
    "reqTimeout": 600             // 请求超时时间（秒）
  }
}
```

### ZooKeeper配置

```json
{
  "zookeeper": {
    "address": "",                          // ZooKeeper地址
    "inferenceInstancePath": "/mep/aigw/instances",
    "scheduleServicePath": "/mep/aigw/global_schedulers",
    "enableTls": false,                   // 是否启用TLS
    "tlsCaFile": "",                      // CA证书路径
    "tlsCrtFile": "",                    // 客户端证书路径
    "tlsKeyFile": ""                      // 客户端私钥路径
  }
}
```

### 全局调度器配置

```json
{
  "globalSchedulers": [
    {
      "model": "DeepSeek-R1-Distill-Qwen-7B",
      "blockSize": 128,
      "deployPolicy": "separated",         // 部署策略: mixed/separated
      "maxTimeToFirstToken": 200,        // 首token最大延迟（ms）
      "maxTimeBetweenTokens": 50,         // token间最大延迟（ms）
      "tokenizeModelName": "DeepSeek-R1",
      "loadBalancer": {
        "prefill": "prefillTimeAware",   // prefill负载均衡策略
        "decode": "token",               // decode负载均衡策略
        "batchSize": 32,
        "reservedBlockNumber": 50
      }
    }
  ]
}
```

### 负载均衡策略

| 策略 | 适用场景 | 说明 |
|------|----------|------|
| `roundRobin` | 通用场景 | 轮询分配请求 |
| `leastConn` | 长连接场景 | 选择连接数最少的节点 |
| `capacity` | 资源敏感场景 | 基于节点容量分配 |
| `token` | Token敏感场景 | 基于Token数量分配 |
| `prefillTimeAware` | Prefill阶段 | 预填充时间感知调度 |
| `decode` | Decode阶段 | 解码阶段专用策略 |

---

## ERR 调度器（MEJ/ERR/CatERR）

ERR（Expected Remaining Round）调度器在 instance 模式下为多轮 agent 会话提供优先级队列调度。通过跟踪每个 session 的剩余步数分布（E[rem|k]），优先调度即将完成的会话（SJF），减少整体完成时间。

### 工作原理

```
请求到达 → ERR OnRequest(k++) → 计算 priority = -E[rem|k]
         → capacity 有空 → 直接 dispatch（RR 选实例）
         → capacity 满 → 入队等待
         → 实例完成请求 → ErrOnResponse → OnCompleted → drain 队列
                         → Pick 最高 priority（最低 E[rem]）dispatch
```

### 四种算法

| 算法 | 说明 | 分类 | 适用 |
|------|------|------|------|
| `fifo` | FIFO（First In First Out）| 无 | 按入队时间排序，不依赖 session 追踪，作为 baseline 对比 |
| `mej` | MEJ（Minimum Expected Job）| 无 | 所有会话混合，根据某次会话中已完成的轮数k，选择k最大的优先调度 |
| `err` | ERR（Expected Remaining）| 无 | 所有会话混合，根据会话历史数据和k，推算剩余的轮数期望，选择剩余轮数最少的优先调度 |
| `caterr` | CatERR（Categorized ERR）| TF-IDF 分类 | 根据prompt将会话分类为code/office场景，再做ERR调度 |

CatERR 使用 TF-IDF + K-means 对 prompt 分类，每个类别维护独立的 E[rem] 分布。office 会话（步数少）一般优先于 code 会话（步数多）被调度。

### 配置示例

```jsonc
{
  "globalSchedulers": [{
    "model": "DeepSeek-R1",
    "mode": "instance",           // ERR 仅在 instance 模式生效
    "loadBalancer": { "batchSize": 8, "mixed": "roundRobin" },
    "errScheduler": {
      "errSchedulerType": "caterr",   // "fifo" | "mej" | "err" | "caterr" | "none"
      "queueSize": 100,
      "queueBoostSec": 240,              // 0 = 默认 240，负数 = 关闭
      "warmupSessions": 100,         // 预热完成至少需要的 session 数量
      "sessionTimeoutSec": 660,      // agent 工作中长时间无变化的兜底清理时间（秒）
      "taskIdleTimeoutSec": 660,      // stop 后静默清理（秒）；默认与 sessionTimeoutSec 对齐（压缩会阻塞下一请求）
      "trainDataPath": "data/err_train.json",
      "tfidfVocabPath": "data/caterr/tfidf_vocab.json",      // CatERR 必填
      "tfidfCentroidsPath": "data/caterr/tfidf_centroids.json"  // CatERR 必填
    }
  }]
}
```

### 部分字段说明

| 字段 | 说明 |
|------|------|
| `warmupSessions` | 预热完成至少需要的 session 数量。预热阶段使用 MEJ 算法，达到阈值后切换到所选算法 |
| `trainDataPath` | 预训练步数分布文件。加载后直接计入 warmup 计数，可跳过预热阶段 |
| `queueBoostSec` | 饥饿保底：排队超过该秒数后强制提升到最高优先级（默认 240，0=默认，负数=关闭）。计时含跨尝试累计：排队超时/被驱逐的 session 把已等待时长记为债务，重试入队时折算进饥饿时钟——超时重试入队即享保底，不再从零重计 |
#### 流式转发超时（两个预算 × 四类死法）

流式转发链路为 `引擎 → aigw → 调用方`，两侧各可能死两种方式，共用两个预算参数处理。**两个值必须跟随 jiuwenswarm 的 LLM 配置**（`resources/config.yaml` 的 `model_client_config` 段）——aigw 的每个超时应等于调用方自己的耐心，对端放弃后再等没有意义：

| aigw 参数 | 跟随的 jiuwen 参数（openjiuwen SDK 默认值） | 默认 | 覆盖的死法 |
|---|---|---|---|
| `proxy.firstChunkTimeoutSec` | `stream_first_chunk_timeout`（**首 token 预算**：等引擎响应流首字节的最长时间；0=默认，负=关闭。首字节到达后自动切到 chunk 间隔档） | 300 | ① 引擎首 token 吐不出（崩溃/挂死但 TCP 未断；引擎合法排队+预填充可达分钟级） |
| `proxy.chunkIntervalTimeoutSec` | `stream_idle_timeout`（**token 间预算**：相邻 token 的最大间隔；0=默认，负=关闭。**一参两用**——读方向引擎静默超此值判半流死亡，写方向调用方写阻塞超此值判客户端死亡） | 60 | ② 引擎吐到一半挂掉 ③④ 调用方停止读取但未关连接（TCP 窗口塞满后写永久阻塞） |

> 默认值跟 openjiuwen SDK 源码默认（300/60）。若部署侧覆盖了客户端预算（如 jiuwenswarm 产品配置 config.yaml 将 idle 调到 120），**aigw 必须同步调高对应键**，否则 aigw 会在客户端还活着时提前掐流。

两档按活动重置（读按字节、写按次），慢引擎上跑几十分钟的**活跃**长流不受影响；不设则四类死法都会把在飞槽位挂死到 3600s 僵尸扫描（校准轮实测：引擎轮中崩溃后 12 个槽位全挂死 17 分钟+，调用方风暴放弃后 7 个槽位占满 21 分钟）
| `sessionTimeoutSec` | agent 工作中长时间无变化的兜底清理超时。超时后删除 session 并打 Warn 日志 |
| `taskIdleTimeoutSec` | 判断 session 已结束的超时时间。session 完成后等待此时间再静默清理 |

### Session 跟踪

ERR 通过请求 body 的 `agent_hint.session_id` 跟踪多轮会话。客户端（如 jiuwenswarm）在开启 AscendAffinity 模式时自动注入该字段。

匿名请求（无 session_id）仍可正常调度，但不记步数分布（priority 固定 -1）。

---

## C API 使用指南

### 初始化

```c
#include "aigw.h"

// 配置AIGW
aigw_config_t cfg = {
    .log_level = "info",
    .log_path = "/tmp",
    .max_instances_per_model = 128,
    .max_supported_models = 128,
    .max_prompt_length = 20480,
    .request_ttl_seconds = 600
};

// 初始化
aigw_error_t err = aigw_init(&cfg);
if (err != AIGW_SUCCESS) {
    // 处理错误
}
```

### 注册模型

```c
aigw_model_config_t model_cfg = {
    .model = "qwen-72b",
    .deploy_policy = AIGW_DEPLOY_SEPARATED,
    .p_lb_type = AIGW_LB_PREFILL_TIME_AWARE,
    .d_lb_type = AIGW_LB_TOKEN_AWARE,
    .pretrain_ttft_path = "/path/to/ttft_model.txt",
    .tokenization_ratio = 0.35
};

err = aigw_register_model(&model_cfg);
```

### 节点选择

```c
// 定义候选节点
aigw_node_info_t nodes[] = {
    {.role = AIGW_INFER_PREFILL, .node_addr = "192.168.1.10:8080"},
    {.role = AIGW_INFER_DECODE, .node_addr = "192.168.1.11:8080"}
};

// 构建请求
aigw_openai_message_t msg = {.role = "user", .content = "Hello"};
aigw_request_t req = {
    .uuid = "req-001",
    .model = "qwen-72b",
    .messages = &msg,
    .message_num = 1
};

// 选择节点
aigw_select_context_t ctx = {
    .node_num = 2,
    .node_list = nodes
};

aigw_select_result_t result = {0};
err = aigw_select_nodes(&req, &ctx, &result);

if (err == AIGW_SUCCESS) {
    printf("Prefill: %s, Decode: %s\n",
           result.prefill_node_addr, result.decode_node_addr);
}
```

### 事件通知

```c
aigw_event_info_t event = {
    .model = "qwen-72b",
    .request_id = "req-001",
    .event_name = "REQUEST_IS_FINISHED"
};

err = aigw_notify_event(AIGW_EVENT_REQUEST, &event);
```

### 清理资源

```c
aigw_unregister_model("qwen-72b");
aigw_uninit();
```

---

## 部署模式

### 独立服务模式

作为HTTP服务器运行，提供RESTful API接口：

```bash
./aigw --config=/etc/aigw/conf/aigw.json
```

**适用场景**：
- 需要独立部署推理网关服务
- 多个应用共享推理网关
- 需要远程访问推理服务

### 共享库模式

通过C API嵌入其他应用：

```c
#include "aigw.h"

// 在应用中初始化并使用AIGW
aigw_init(&cfg);
// ... 使用AIGW功能
aigw_uninit();
```

**适用场景**：
- 需要将推理网关集成到现有应用
- 对性能有极致要求
- 需要更紧密的集成

---

## 示例代码

完整示例代码位于 `example/cgo/` 目录，演示了：

- AIGW初始化和配置
- 缓存驱动注册
- 模型注册和管理
- 并发请求处理
- 节点选择和负载均衡
- 事件通知

### 构建示例

```bash
cd example
mkdir build && cd build
cmake ..
make
./aigw_test_demo
```

---

## 性能特性

| 指标 | 说明 |
|------|------|
| **高并发** | 支持数千并发推理请求 |
| **低延迟** | 智能调度优化首token延迟 |
| **高可用** | 多实例部署，自动故障转移 |
| **可扩展** | 水平扩展支持，动态实例注册 |

---

## 常见问题

### Q: AIGW支持哪些大语言模型？

A: AIGW支持所有兼容OpenAI API格式的大语言模型，包括但不限于：
- DeepSeek系列
- Qwen系列
- LLaMA系列
- ChatGLM系列

### Q: 如何配置多个模型？

A: 在配置文件的 `globalSchedulers` 数组中添加多个模型配置即可。

### Q: 缓存是必须的吗？

A: 不是必须的。Redis缓存是可选的，不配置时系统仍可正常运行，只是无法享受缓存带来的性能提升。

### Q: 如何监控AIGW的运行状态？

A: AIGW提供内置的监控接口，可以通过HTTP API查询服务状态、实例信息、请求统计等数据。

---

## 开源依赖

本项目集成了以下开源组件：

| 组件 | 许可证 | 用途 |
|------|--------|------|
| [LightGBM](https://github.com/microsoft/LightGBM) | MIT | 梯度提升决策树框架 |
| [Eigen](https://eigen.tuxfamily.org/) | MPL2.0 | C++模板库，用于线性代数运算 |
| [compute](https://github.com/kth-competitive-programming/kactl) | MIT | 计算几何库 |
| [fast_double_parser](https://github.com/lemire/fast_double_parser) | BSD-3 | 快速浮点数解析 |
| [fmt](https://github.com/fmtlib/fmt) | MIT | C++格式化库 |

---

## 许可证

Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.

---

## 贡献

欢迎提交Issue和Pull Request来帮助改进项目。在提交代码前，请确保：

1. 代码通过所有单元测试
2. 代码符合项目编码规范
3. 添加必要的注释和文档

---

## 联系方式

如有问题或建议，请通过以下方式联系：

- 提交Issue：[GitHub Issues](https://github.com/your-repo/aigw/issues)
- 发送邮件：[your-email@example.com]

---

<div align="center">

**Made with ❤️ by AIGW Team**

</div>
