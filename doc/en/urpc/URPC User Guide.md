# 1. Introduction
Provides a high-performance RPC communication library with extremely low latency, ultra-high IOPS, and massive bandwidth.

# 2. Software Build

## URPC

### Build and install URPC RPM packages
```bash
rm -rf .git*
mkdir -p /root/rpmbuild/SOURCES/
tar -cvf /root/rpmbuild/SOURCES/umdk-26.06.0.tar.gz --exclude=.git $(ls -A)
rpmbuild -bb umdk.spec --with urpc
```

### Compile URPC with cmake
```bash
cd src
mkdir build; cd build
cmake .. -DBUILD_ALL=disable -DBUILD_URPC=enable
make -j16
make install # optional, install URPC if needed
```
## UMQ

### Compile UMQ with bazel
Run the following command to build the UMQ static library:
```bash
cd src/urpc/
bazel build --noenable_bzlmod --copt=-DUMQ_QBUF_DEBUG //:umq # output: bazel-bin/libumq.a
```
#### Note

* The current `//:umq` target depends on the `@boringssl` and `@urma` repositories declared in `src/urpc/WORKSPACE`; build it with WORKSPACE dependency resolution by specifying `--noenable_bzlmod` (and `--enable_workspace` on Bazel versions where WORKSPACE is disabled by default).
* `src/urpc/MODULE.bazel` declares and overrides an OpenSSL module, but it does not declare the `@boringssl` or `@urma` repositories required by `//:umq`, so the current build is not compatible with Bzlmod-only dependency resolution.

#### Build Modes
* By default, `.bazelrc` sets `--compilation_mode=opt` and `--strip=always`.
* `--config=release` additionally sets `--linkopt=-Wl,-s` for linking; this does not strip the `libumq.a` static archive.
* `--config=debug` sets `--compilation_mode=dbg` and `--strip=never`.

The effective optimization flags depend on the selected C/C++ toolchain.

#### BoringSSL Dependency
* The current `//:umq` target depends on BoringSSL (`@boringssl//:ssl` and `@boringssl//:crypto`), whose revision is pinned in `src/urpc/WORKSPACE`. This target does not provide an `openssl_mode` switch.

# 3. Usage Guide
- rpm -ivh /root/rpmbuild/RPMS/aarch64/umdk-urpc-*.rpm
- urpc_framework_example -i SERVER_IP -d DEV_NAME -T 1 -e 0 --server --eid EID -L --assign_mode 2 -R
- urpc_framework_example -i SERVER_IP -d DEV_NAME -T 1 -e 0 --client --eid EID -L --assign_mode 2 -R

example:
```bash
urpc_framework_example -i 192.168.100.100 -d udma0 -T 1 -e 0 --server --eid 4245:4944:0000:0000:0000:0000:0100:0000 -L --assign_mode 2 -R
urpc_framework_example -i 192.168.100.100 -d udma0 -T 1 -e 0 --client --eid 4245:4944:0000:0000:0000:0000:0200:0000 -L --assign_mode 2 -R
```