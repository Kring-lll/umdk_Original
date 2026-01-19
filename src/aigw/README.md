# AIGW - AI Gateway

[![License](https://img.shields.io/badge/License-Mulan%20PSL%20v2-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-Go-blue.svg)](https://golang.org/)

## Overview

**AIGW** (AI Gateway) is an intelligent and high-performance gateway designed for large-scale inference services within the UMDK (Unified Multi-Domain Toolkit) project. It provides a unified entry point for AI inference workloads, offering advanced features such as request routing, load balancing, authentication, and performance optimization.

This gateway is built to handle high-throughput AI inference scenarios, supporting multiple inference backends and providing enterprise-grade features for production deployments.

## Features

### Core Capabilities

- **High-Performance Gateway**: Optimized for low-latency and high-throughput AI inference workloads
- **Intelligent Routing**: Dynamic request routing based on model availability and load
- **Load Balancing**: Distribute inference requests across multiple backend instances
- **Authentication & Authorization**: Secure access control for inference services
- **Request/Response Transformation**: Format conversion and data preprocessing
- **Rate Limiting**: Protect backend services from overload
- **Monitoring & Metrics**: Built-in observability for performance analysis

### Technical Features

- **Protocol Support**: HTTP/HTTPS, gRPC, and WebSocket protocols
- **Model Management**: Dynamic model loading and versioning
- **Caching Layer**: Intelligent caching for frequently requested results
- **Batching**: Automatic request batching for improved throughput
- **Circuit Breaker**: Fault tolerance and resilience patterns
- **Plugin System**: Extensible architecture for custom functionality

## Architecture

### Project Structure

```
src/aigw/
├── build/                 # Build scripts and configurations
├── cmd/
│   └── aigw/
│       └── main.go       # Application entry point
├── configs/              # Configuration files and templates
├── internal/             # Internal packages (non-exported)
├── pkg/                  # Public packages and libraries
│   ├── crypto/          # Cryptographic utilities
│   ├── hg_tokenizers/   # HuggingFace tokenizers integration
│   ├── lightgbm/        # LightGBM model support
│   ├── log/             # Logging framework
│   └── utils/           # General utilities
├── build.sh             # Build automation script
├── go.mod               # Go module definition
└── README.md           # This file
```

### Component Overview

#### Core Modules (`pkg/`)

- **crypto**: Provides cryptographic functions for secure communication, token generation, and data encryption
- **hg_tokenizers**: Integration with HuggingFace tokenizers for NLP preprocessing and tokenization
- **lightgbm**: Support for LightGBM gradient boosting framework, enabling efficient model inference
- **log**: Structured logging framework with configurable levels and outputs
- **utils**: Common utilities including data structures, error handling, and helper functions

#### Application Entry (`cmd/aigw/`)

The main application entry point that initializes and starts the AIGW service. Handles configuration loading, component initialization, and graceful shutdown.

#### Configuration Management (`configs/`)

Contains configuration templates and default settings for various deployment scenarios. Supports environment-specific configurations and dynamic reloading.

## Quick Start

### Prerequisites

- **Go**: Version 1.21 or higher
- **Operating System**: Linux (recommended), macOS, or Windows
- **Dependencies**: See `go.mod` for full dependency list

### Building from Source

1. **Clone the repository**:
   ```bash
   git clone https://atomgit.com/openeuler/umdk.git
   cd umdk/src/aigw
   ```

2. **Build the project**:
   ```bash
   # Using the provided build script
   chmod +x build.sh
   ./build.sh
   
   # Or using go directly
   go mod download
   go build -o bin/aigw ./cmd/aigw
   ```

3. **Verify the build**:
   ```bash
   ./bin/aigw --version
   ```

### Configuration

1. **Create configuration directory**:
   ```bash
   mkdir -p /etc/aigw
   ```

2. **Copy default configurations**:
   ```bash
   cp configs/* /etc/aigw/
   ```

3. **Edit configuration files** according to your environment

### Running AIGW

```bash
# Start the gateway
./bin/aigw --config /etc/aigw/config.yaml

# Or using environment variables
export AIGW_CONFIG_PATH=/etc/aigw/config.yaml
./bin/aigw
```

### Docker Deployment

```bash
# Build Docker image
docker build -t aigw:latest .

# Run container
docker run -d \
  --name aigw \
  -p 8080:8080 \
  -v /etc/aigw:/etc/aigw \
  aigw:latest
```

## Configuration

AIGW uses YAML-based configuration files. Key configuration sections include:

### Server Configuration

```yaml
server:
  host: "0.0.0.0"
  port: 8080
  tls:
    enabled: false
    cert_file: ""
    key_file: ""
```

### Backend Services

```yaml
backends:
  - name: "inference-service-1"
    url: "http://localhost:9000"
    weight: 1
    health_check:
      enabled: true
      interval: 30s
```

### Authentication

```yaml
auth:
  enabled: true
  providers:
    - name: "jwt"
      secret: "your-secret-key"
      expiry: 3600
```

### Logging

```yaml
logging:
  level: "info"
  format: "json"
  output: "stdout"
```

## API Documentation

### Health Check

```bash
GET /health
```

### Inference Endpoint

```bash
POST /api/v1/inference
Content-Type: application/json

{
  "model": "model-name",
  "input": "input data",
  "parameters": {}
}
```

### Metrics

```bash
GET /metrics
```

## Development

### Project Layout

The project follows standard Go project layout conventions:

- `cmd/`: Application entry points
- `pkg/`: Reusable libraries
- `internal/`: Private application code
- `configs/`: Configuration files
- `build/`: Build scripts and CI configurations

### Adding New Features

1. **New Package**: Place in appropriate directory under `pkg/`
2. **New Command**: Add to `cmd/` directory
3. **Configuration**: Update schema in `configs/`
4. **Tests**: Follow `_test.go` naming convention

### Testing

```bash
# Run unit tests
go test ./...

# Run with coverage
go test -cover ./...

# Run specific test
go test ./pkg/utils
```

### Code Style

- Follow standard Go formatting (`gofmt`)
- Use meaningful variable and function names
- Add comments for exported functions and types
- Include unit tests for new functionality

## Performance Tuning

### Optimization Strategies

1. **Connection Pooling**: Configure appropriate connection pool sizes
2. **Batching**: Enable request batching for throughput optimization
3. **Caching**: Configure cache sizes and TTL values
4. **Load Balancing**: Adjust load balancing algorithms based on traffic patterns

### Monitoring

- **Metrics**: Prometheus-compatible metrics endpoint
- **Tracing**: Distributed tracing support (Jaeger/OpenTelemetry)
- **Logging**: Structured logging with configurable verbosity

## Contributing

We welcome contributions to AIGW! Please follow these guidelines:

1. **Fork the repository** on AtomGit
2. **Create a feature branch**: `git checkout -b feature/your-feature`
3. **Make your changes** and add tests
4. **Commit with meaningful messages**: `git commit -m "Add feature: description"`
5. **Push to your fork**: `git push origin feature/your-feature`
6. **Create a Pull Request** on the main repository

### Code Review Process

- All contributions must be made via Pull Requests
- At least one reviewer must approve changes
- All tests must pass
- Documentation should be updated for user-facing changes

## License

This project is licensed under the Mulan PSL v2 License. See the [LICENSE](LICENSE) file for details.

## Support

### Community

- **Issues**: Report bugs and feature requests via the project issue tracker
- **Discussions**: Join community discussions on the project forum
- **Documentation**: Refer to the project wiki for detailed documentation

### Commercial Support

For commercial support and enterprise features, please contact the openEuler community.

## Roadmap

### Upcoming Features

- [ ] Multi-tenancy support
- [ ] Advanced rate limiting policies
- [ ] GraphQL API gateway capabilities
- [ ] Integration with popular ML platforms
- [ ] Enhanced security features
- [ ] Performance optimization improvements

### Long-term Vision

- Cloud-native architecture with Kubernetes integration
- Support for emerging AI/ML frameworks
- Advanced analytics and observability
- Multi-cloud deployment strategies

---

**AIGW** is part of the [openEuler UMDK project](https://atomgit.com/openeuler/umdk), providing a unified toolkit for multi-domain development.

For more information, visit the [project homepage](https://atomgit.com/openeuler/umdk/tree/br_aigw_feature/src/aigw).
