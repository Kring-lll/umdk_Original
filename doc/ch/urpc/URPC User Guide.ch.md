# 1. 介绍
统一远程过程调用，支持灵衢原生高性能主机间和设备间RPC通信，以及RPC加速。

# 2. 软件编译

## URPC

### 使用RPM包编译构建URPC
```bash
rm -rf .git*
mkdir -p /root/rpmbuild/SOURCES/
tar -cvf /root/rpmbuild/SOURCES/umdk-26.06.0.tar.gz --exclude=.git $(ls -A)
rpmbuild -bb umdk.spec --with urpc
```

### 使用cmake编译URPC
```bash
cd src
mkdir build; cd build
cmake .. -DBUILD_ALL=disable -DBUILD_URPC=enable
make -j16
make install # 可选项，如果需要安装URPC则执行此命令
```

## UMQ

### 使用bazel编译UMQ
当前 Bazel 配置提供 UMQ 静态库目标；如需动态库，请使用上文的 CMake 构建方式。
```bash
cd src/urpc/
bazel build --noenable_bzlmod --copt=-DUMQ_QBUF_DEBUG //:umq # 使用支持 WORKSPACE 的 Bazel 版本，构建静态库目标
```
#### 注意
* 当前 `MODULE.bazel` 未声明 `//:umq` 所需的 `boringssl` 和 `urma` 仓库，不能据此保证默认 Bzlmod 模式可构建该目标。
* 使用支持 WORKSPACE 的 Bazel 版本，并显式指定 `--noenable_bzlmod`；依赖定义见 `src/urpc/WORKSPACE`，其中 URMA 使用 `/usr` 下的本地安装。当前源码还需要显式传入 `--copt=-DUMQ_QBUF_DEBUG`，以提供 qbuf 调试记录函数的定义。

#### 编译模式
* 默认配置为 `--compilation_mode=opt --strip=always`；具体优化参数由所用 C/C++ 工具链决定。
* `--config=release` 额外设置链接参数 `-Wl,-s`；该参数不表示生成 `//:umq` 静态归档时会剥离其符号。
* `--config=debug` 设置 `--compilation_mode=dbg --strip=never`；具体编译参数由所用 C/C++ 工具链决定。

#### openssl依赖
* 当前 Bazel 的 `//:umq` 目标依赖 BoringSSL，其版本由 `src/urpc/WORKSPACE` 中的提交号固定。当前构建文件未定义 `//umq:openssl_mode` 配置目标，因此不支持通过 `--//umq:openssl_mode=bazel` 或 `--//umq:openssl_mode=system` 切换依赖。

# 3. 安装使用
- rpm -ivh /root/rpmbuild/RPMS/aarch64/umdk-urpc-*.rpm
- urpc_framework_example -i SERVER_IP -d DEV_NAME -T 1 -e 0 --server --eid EID -L --assign_mode 2 -R
- urpc_framework_example -i SERVER_IP -d DEV_NAME -T 1 -e 0 --client --eid EID -L --assign_mode 2 -R

**指令样例**:

```bash
urpc_framework_example -i 192.168.100.100 -d udma0 -T 1 -e 0 --server --eid 4245:4944:0000:0000:0000:0000:0100:0000 -L --assign_mode 2 -R
urpc_framework_example -i 192.168.100.100 -d udma0 -T 1 -e 0 --client --eid 4245:4944:0000:0000:0000:0000:0200:0000 -L --assign_mode 2 -R
```