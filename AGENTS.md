# UMDK (UMDK) Codebase Guidelines for Agentic Coding agents

This document provides essential information for building, linting, testing, and code style guidelines in working with the repository.

## Build Commands

### RPM Build (Full build)
```bash
mkdir -p /root/rpmbuild/SOURCES/
tar -czf /root/rpmbuild/SOURCES/umdk-25.12.0.tar.gz --exclude=.git `ls -A`
rpmbuild -ba umdk.spec
```

**Build with Address Sanitizer:**
```bash
rpmbuild -ba umdk.spec --with test
rpmbuild -ba umdk.spec --with urma
rpmbuild -ba umdk.spec --with urpc
rpmbuild -ba umdk.spec --with dlock
rpmbuild -ba umdk.spec --with ums
```

### CMake Build (Individual modules)
```bash
mkdir build
cd build
cmake -DBUILD_ALL=disable -DB BUILD_URMA=enable ..
make -j
make install -j
```

**Run a single test:**
```bash
cd test/urma/bond
mkdir build
cd build
cmake ..
make -j
./urma_gtest
```

**Run all URMA unit tests:**
```bash
cd test/urpc
mkdir build
cd build
cmake ..
make -j
./umdk_urpc_ut
```

**Run a single URPC test:**
```bash
cd test/urpc/core
mkdir build
cd build
cmake ..
./test_core
```

**Run a single UMQ test:**
```bash
cd test/urpc/umq
mkdir build
cd build
cmake ..
./test_umq
```

**Run a single URPC test:**
```bash
cd test/urpc/util
mkdir build
cd build
cmake ..
./test_util
```

**Run all URPC tests:**
```bash
cd test/urpc
ctest --verbose
```

**Run single test with verbose output:**
```bash
ctest --verbose
test_urpc_single_test() {
    local test_name=$1
    local test_binary="test_${test_name}"
    
    echo "Test binary: $test_binary"
    
    echo "Running test: $test_binary..."
    ./$test_binary
done
```
cd test/urpc
ctest --verbose
``


### CMake Build Options
```bash
cmake -DBUILD_ALL=disable -DBUILD_URPC=enable -DASAN=enable -DCODE_COVERAGE=enable ..
make -j
make install -j
```

cmake -DBUILD_ALL=disable -DBUILD_URPC=enable ..
make -j
make install -j
cd test/urpc/build
ctest --verbose
    echo "Verbose mode for detailed output"
    fi)

fi()
```

## Code Style Guidelines

- **Language**: C (primary), C++ (tests use C++)
- **Naming Convention**:
  - Functions: `snake_case` (e.g., `urma_init`)
  - Types: `urma_status_t`, `urma_device_t`,  - Structs: `urma_*_t`, `urma_*_cfg_t` (e.g., `urma_jfc_cfg_t`)
  - Constants: `UB_*`, `URMA_*`, `MAX_len` (e.g., `URMA_MAX_JETTY_DEPTH`)
  - Macros: `UB_*`, `UB_BITMAP_*`, `UB_LIST_*`
- **Formatting**:
  - Use `.clang-format` (based on LLVM style)
  - Indent with 4 spaces
  - Function braces on separate line after function body (no comments inside)
  - Column limit: 0 (no limit)
- **Imports**:
  - System headers first, then project headers (alphabetical order)
  - Use angle brackets for pointer alignment with URMA types.h
  - Header files: use `#pragma once` pattern
  - Source files: include project headers first
  - Use `extern "C"` for C++ projects
  - Use include guards for header inclusion

  - Example: `#include "urma_api.h"`
- **Error Handling**:
  - Return values: functions return 0 on success (URMA_STATUS_SUCCESS)
  - Return NULL on error/failure
  - Use `urma_*_t` type strings for errors (e.g., "URMA_EINVAL")
  - Check return values: `urma_status_t` is `urma_errno` enum values
  - Log errors via `urma_log()` macros before returning
  - Use `errno` for system errors
- **Memory Management**:
  - Use `urma_register_seg()` for memory registration
  - Always pair `urma_unregister_seg()` when done
  - Use `malloc()` for dynamic allocation; check return values
  - Use free() for URMA-allocated resources
  - Use `mmap()` for shared memory; always check alignment and permissions
  - Clean up resources in `urma_delete_*()` and similar functions
  - Use `close()` for file descriptors, `urma_fclose()` for config files
- **Thread Safety**:
  - URMA is thread-safe; use atomic operations for reference counting
  - Use mutexes (`pthread_mutex_t`, `pthread_spinlock_t`) for synchronization
  - URPC framework uses worker threads for task execution
  - Use `pthread_*` threading primitives for thread management
- **Resource Management**:
  - URMA contexts, jettys (JFC, JFS, JFR), created via `urma_create_*()` functions
  - Transport providers (TP) created via `urma_import_jetty()` / `urma_bind_jetty()`
  - Segments registered via `urma_register_seg()`
  - Target segments imported via `urma_import_seg()`
  - Cleanup: call `urma_delete_*()`, `urma_unimport_*()`, `urma_unregister_seg()`, `urma_unbind_*()` when done
- **Logging**:
  - Use `urma_log()` macros for logging (default: rsyslog)
  - Log levels: URMA_VLOG_DEBUG, URMA_VLOG_INFO, URMA_VLOG_WARN, URMA_VLOG_ERROR
  - Example: `urma_log(URMA_VLOG_ERROR, "Error: %s", ctx-> buf, len);`
  - URPC uses `urpc_vlog()` with similar log levels
- **Build Configuration**:
  - Config files located in `/etc/`qbuf.conf`, `urpc_framework.conf`, `umq.conf`
  - Use `--with-*` cmake options for conditional builds
- **Dependencies**:
  - Build: cmake, gcc, gcc-c++, kernel-devel, glib2-devel, openssl-devel, libnl3-devel
  - Runtime: kernel drivers (ubcore, uburma, ummu, etc.)
  - Test: gtest (C++ testing framework)

  - Python tests: pytest (for integration tests)
  - Python linting: ruff or pylint

- **Single Test Execution**:
  - URMA tests:
    ```bash
    cd test/urma/bond && mkdir build
    cd build
    cmake -DBUILD_ALL=disable -DBUILD_URMA=enable -DURMA_TEST=enable -DRELEASE_ENABLE=disable
    make -j
    ./urma_gtest
    ```
  - URPC tests:
    ```bash
    cd test/urpc && mkdir build
            cd build
            cmake -DBUILD_ALL=disable -DBUILD_URPC=enable -DASAN=enable -DCODE_COVERAGE=enable ..
            make -j
            ./umdk_urpc_ut
            cd $SCRIPT_PATH
            export LD_LIBRARY_PATH=$DEPENDENCIES_PATH
            export GCOV_PREFIX="$REPORTS_PATH"
            cd $REPORTS_PATH
            # Run all tests
            ./core/test_core
            ./lib/test_lib
            ./protocol/test_protocol
            ./util/test_util
            ./umq/test_umq
            cd $REPORTS_PATH
            lcov --capture --directory . --rc lcov_branch_coverage=1 coverage.info
            genhtml coverage.info --output-directory lcov_report
## URMA 1-Hour Presentation Structure

> For team sharing sessions focused on user-side URMA code

### Part 1: Concepts & Architecture (15 min)

1. **What is URMA** (3 min)
   - Unified Remote Memory Access abstraction layer
   - Comparable to: RDMA verbs / Libfabric

2. **Core Object Model** (7 min)
   ```
   Device → Context → Jetty(JFC+JFS+JFR)
   
   Segment(memory) ← Token(permission)
   ```
   - Context: Communication context on device
   - JFC: Completion Queue
   - JFS: Sender
   - JFR: Receiver
   - Jetty: JFS+JFR combination

3. **Two Communication Modes** (5 min)
   - One-sided operations: Read/Write
   - Two-sided operations: Send/Recv

### Part 2: API Overview (15 min)

4. **Device & Context** (5 min)
   - `urma_get_device_list()`
   - `urma_create_context()`
   - `urma_query_device()`

5. **Resource Creation** (5 min)
   - `urma_create_jfc/jfs/jfr/jetty()`
   - `urma_alloc_token_id()`
   - `urma_register_seg()`

6. **Data Operations** (5 min)
   - `urma_write/read()`
   - `urma_send/recv()`
   - `urma_poll_jfc()`

### Part 3: Code Walkthrough (20 min)

7. **Header Files** (5 min)
   - `urma_api.h`: API declarations
   - `urma_types.h`: Type definitions
   - `urma_opcode.h`: Operation codes

8. **Example Code Flow** (10 min)
   - `src/urma/examples/urma_sample.c`
   
   | Step | Function | Purpose |
   |------|----------|---------|
   | 1 | `urma_init()` | Initialize library |
   | 2 | `urma_get_device_list()` + `urma_create_context()` | Get device, create context |
   | 3 | `urma_create_jfc/jetty()` | Create resources |
   | 4 | `urma_register_seg()` | Register memory |
   | 5 | `urma_import_jetty()` + `urma_bind_jetty()` | Connect to remote |
   | 6 | `urma_write/read()` + `urma_poll_jfc()` | Transfer data |
   | 7 | Cleanup functions | Release resources |

9. **Key Data Structures** (5 min)
   - `urma_jfs_wr_t`: Work Request
   - `urma_cr_t`: Completion Record
   - `urma_seg_t`: Memory segment descriptor

### Part 4: Summary & Extensions (10 min)

10. **Key Takeaways** (5 min)
    - Resource lifecycle management
    - Memory registration & permission control
    - Polling vs event-driven

11. **Extended Topics** (3 min)
    - How UDMA driver implements these APIs
    - How URPC builds on URMA

12. **Q&A** (2 min)

---
*Reference: For PlantUML diagrams, see `docs/presentations/urma/`*
