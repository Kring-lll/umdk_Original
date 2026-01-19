# AIGW - AI 网关

[![License](https://img.shields.io/badge/License-Mulan%20PSL%20v2-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-Go-blue.svg)](https://golang.org/)

## 项目概述

**AIGW** (AI Gateway) 是一个智能高性能网关，专为 UMDK (统一多领域工具包) 项目中的大规模推理服务而设计。它为 AI 推理工作负载提供统一的入口点，提供高级功能，如请求路由、负载均衡、身份认证和性能优化。

该网关专为处理高吞吐量 AI 推理场景而构建，支持多个推理后端，并为生产部署提供企业级功能。

## 核心功能

### 主要能力

- **高性能网关**：针对低延迟和高吞吐量 AI 推理工作负载进行优化
- **智能路由**：基于模型可用性和负载的动态请求路由
- **负载均衡**：在多个后端实例之间分发推理请求
- **认证与授权**：为推理服务提供安全的访问控制
- **请求/响应转换**：格式转换和数据预处理
- **速率限制**：保护后端服务免受过载
- **监控与指标**：内置可观测性用于性能分析

### 技术特性

- **协议支持**：HTTP/HTTPS、gRPC 和 WebSocket 协议
- **模型管理**：动态模型加载和版本控制
- **缓存层**：为频繁请求的结果提供智能缓存
- **批处理**：自动请求批处理以提高吞吐量
- **熔断器**：容错和弹性模式
- **插件系统**：可扩展架构支持自定义功能

## 架构设计

### 项目结构

```
src/aigw/
├── build/                 # 构建脚本和配置
├── cmd/
│   └── aigw/
│       └── main.go       # 应用程序入口点
├── configs/              # 配置文件和模板
├── internal/             # 内部包（非导出）
├── pkg/                  # 公共包和库
│   ├── crypto/          # 加密工具
│   ├── hg_tokenizers/   # HuggingFace 分词器集成
│   ├── lightgbm/        # LightGBM 模型支持
│   ├── log/             # 日志框架
│   └── utils/           # 通用工具
├── build.sh             # 构建自动化脚本
├── go.mod               # Go 模块定义
└── README.md           # 本文档
```

### 组件概览

#### 核心模块 (`pkg/`)

- **crypto**：为安全通信、令牌生成和数据加密提供加密功能
- **hg_tokenizers**：与 HuggingFace 分词器集成，用于 NLP 预处理和分词
- **lightgbm**：支持 LightGBM 梯度提升框架，实现高效的模型推理
- **log**：具有可配置级别和输出的结构化日志框架
- **utils**：包括数据结构、错误处理和辅助函数的常用工具

#### 应用入口 (`cmd/aigw/`)

主应用程序入口点，初始化和启动 AIGW 服务。处理配置加载、组件初始化和优雅关闭。

#### 配置管理 (`configs/`)

包含各种部署场景的配置模板和默认设置。支持环境特定配置和动态重新加载。

## 快速入门

### 环境要求

- **Go**: 1.21 或更高版本
- **操作系统**: Linux（推荐）、macOS 或 Windows
- **依赖项**: 查看 `go.mod` 获取完整依赖列表

### 从源码构建

1. **克隆代码仓库**:
   ```bash
   git clone https://atomgit.com/openeuler/umdk.git
   cd umdk/src/aigw
   ```

2. **构建项目**:
   ```bash
   # 使用提供的构建脚本
   chmod +x build.sh
   ./build.sh
   
   # 或直接使用 go
   go mod download
   go build -o bin/aigw ./cmd/aigw
   ```

3. **验证构建**:
   ```bash
   ./bin/aigw --version
   ```

### 配置设置

1. **创建配置目录**:
   ```bash
   mkdir -p /etc/aigw
   ```

2. **复制默认配置**:
   ```bash
   cp configs/* /etc/aigw/
   ```

3. **根据您的环境编辑配置文件**

### 运行 AIGW

```bash
# 启动网关
./bin/aigw --config /etc/aigw/config.yaml

# 或使用环境变量
export AIGW_CONFIG_PATH=/etc/aigw/config.yaml
./bin/aigw
```

### Docker 部署

```bash
# 构建 Docker 镜像
docker build -t aigw:latest .

# 运行容器
docker run -d \
  --name aigw \
  -p 8080:8080 \
  -v /etc/aigw:/etc/aigw \
  aigw:latest
```

## 配置说明

AIGW 使用基于 YAML 的配置文件。主要配置部分包括：

### 服务器配置

```yaml
server:
  host: "0.0.0.0"
  port: 8080
  tls:
    enabled: false
    cert_file: ""
    key_file: ""
```

### 后端服务

```yaml
backends:
  - name: "inference-service-1"
    url: "http://localhost:9000"
    weight: 1
    health_check:
      enabled: true
      interval: 30s
```

### 认证配置

```yaml
auth:
  enabled: true
  providers:
    - name: "jwt"
      secret: "your-secret-key"
      expiry: 3600
```

### 日志配置

```yaml
logging:
  level: "info"
  format: "json"
  output: "stdout"
```

## API 文档

### 健康检查

```bash
GET /health
```

### 推理端点

```bash
POST /api/v1/inference
Content-Type: application/json

{
  "model": "模型名称",
  "input": "输入数据",
  "parameters": {}
}
```

### 指标监控

```bash
GET /metrics
```

## 开发指南

### 项目布局

项目遵循标准的 Go 项目布局约定：

- `cmd/`: 应用程序入口点
- `pkg/`: 可重用的库
- `internal/`: 私有应用程序代码
- `configs/`: 配置文件
- `build/`: 构建脚本和 CI 配置

### 添加新功能

1. **新包**: 放置在 `pkg/` 下的适当目录
2. **新命令**: 添加到 `cmd/` 目录
3. **配置**: 更新 `configs/` 中的模式
4. **测试**: 遵循 `_test.go` 命名约定

### 测试

```bash
# 运行单元测试
go test ./...

# 带覆盖率运行
go test -cover ./...

# 运行特定测试
go test ./pkg/utils
```

### 代码规范

- 遵循标准 Go 格式 (`gofmt`)
- 使用有意义的变量和函数名称
- 为导出的函数和类型添加注释
- 为新功能包含单元测试

## 性能调优

### 优化策略

1. **连接池**: 配置适当的连接池大小
2. **批处理**: 启用请求批处理以优化吞吐量
3. **缓存**: 配置缓存大小和 TTL 值
4. **负载均衡**: 根据流量模式调整负载均衡算法

### 监控

- **指标**: Prometheus 兼容的指标端点
- **追踪**: 分布式追踪支持 (Jaeger/OpenTelemetry)
- **日志**: 具有可配置详细程度的结构化日志

## 贡献指南

我们欢迎对 AIGW 的贡献！请遵循以下指南：

1. **在 AtomGit 上 Fork 仓库**
2. **创建功能分支**: `git checkout -b feature/your-feature`
3. **进行更改** 并添加测试
4. **提交有意义的提交信息**: `git commit -m "添加功能: 描述"`
5. **推送到您的 Fork**: `git push origin feature/your-feature`
6. **在主仓库上创建 Pull Request**

### 代码审查流程

- 所有贡献必须通过 Pull Request 进行
- 至少一名审查者必须批准更改
- 所有测试必须通过
- 应更新面向用户的更改的文档

## 开源协议

本项目采用 Mulan PSL v2 协议授权。详情请参见 [LICENSE](LICENSE) 文件。

## 技术支持

### 社区支持

- **问题**: 通过项目问题跟踪器报告错误和功能请求
- **讨论**: 加入项目论坛上的社区讨论
- **文档**: 参考项目 wiki 获取详细文档

### 商业支持

有关商业支持和企业功能，请联系 openEuler 社区。

## 发展路线图

### 即将推出的功能

- [ ] 多租户支持
- [ ] 高级速率限制策略
- [ ] GraphQL API 网关功能
- [ ] 与流行 ML 平台的集成
- [ ] 增强的安全功能
- [ ] 性能优化改进

### 长期愿景

- 具有 Kubernetes 集成的云原生架构
- 对新兴 AI/ML 框架的支持
- 高级分析和可观测性
- 多云部署策略

---

**AIGW** 是 [openEuler UMDK 项目](https://atomgit.com/openeuler/umdk) 的一部分，为多个领域开发提供统一的工具包。

有关更多信息，请访问[项目主页](https://atomgit.com/openeuler/umdk/tree/br_aigw_feature/src/aigw)。
