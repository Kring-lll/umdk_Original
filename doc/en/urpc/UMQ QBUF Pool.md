# UMQ QBUF Pool

## Functional Introduction
The UMQ QBUF Pool is the core memory management module of the UMQ component, providing high performance memory allocation and deallocation functions. Through a two-layer architecture of **Thread-Local Storage (TLS) and a global memory pool**, this module achieves low-latency and high-concurrency memory management capabilities.
### Core Configuration Parameters

The QBUF Pool provides seven key local storage configuration parameters to optimize memory management performance:

- **total_size**：Total memory size of the memory pool
- **data_size**：Size of each block
- **headroom_size**：Size of the user header
- **mode**：Mode of the UMQ Buffer, supporting two types: UMQ_BUF_SPLIT and UMQ_BUF_COMBINE
- **tls_buf_capacity**：The total size maintained by the local thread storage
- **tls_alloc_batch_size**：The number of buffers fetched in batches from the global memory pool

## Two-layer Memory Pool Architecture

The core design of the UMQ QBUF Pool adopts a two-layer memory management architecture, aiming to improve the efficiency of memory allocation and deallocation:：

### 1. Thread-Local Storage
Each thread has an independent memory storage, avoiding lock contention between threads. Its core features are:
- Private to the thread, no need for lock protection
- The preferred source for memory allocation and deallocation
- Responsible for caching memory resources of this thread

### 2. Global Memory Pool
A memory pool shared by all threads, serving as the total repository and scheduling center of memory resources. Its core features are:
- Shared by all threads, requiring global lock protection
- Serves as a supplement and backup for the local storage
- Responsible for the global balance of memory resources

## Local Storage Threshold Control
- The maximum capacity of the local storage is controlled by the `tls_buf_capacity` parameter
- When the local storage exceeds `tls_buf_capacity` + `tls_alloc_batch_size`, a return is triggered, and buffers exceeding `tls_buf_capacity` are returned.
- Ensures that each thread does not occupy excessive memory resources
- When the local storage is insufficient, buffers are obtained from the global pool, and `tls_alloc_batch_size` buffers are obtained each time

## Memory Allocation and Deallocation Process

### Memory Allocation Process

Memory allocation is the core function of the QBUF Pool, and its process reflects the interaction mechanism of the two-layer memory pool:

1. **Memory Pool Check**: Verify whether the memory pool has been initialized
2. **Local Storage Acquisition**: Obtain the local storage of the current thread through TLS
3. **Demand Calculation**: Calculate the actual number of buffers required according to the request parameters
4. **Local Allocation Attempt**:
   - Check if there are enough target type buffers in the local storage
   - If sufficient, allocate directly from the local linked list
5. **Global Pool Supplement**:
   - If the local storage is insufficient, obtain `tls_alloc_batch_size` buffers in batches from the global pool
   - Update the buffer count of the local storage
6. **Reallocate locally**: Allocate the required buffers from the supplemented local storage
7. **Buffer Initialization**: Set buffer metadata and mark it as the allocated state
8. **Result Return**: Return the allocated buffer linked list

### Memory Deallocation Process

The memory deallocation process corresponds to the allocation process and also reflects the interaction of the two-layer memory pool:

1. **Memory Pool Check**: Verify whether the memory pool has been initialized
2. **Local Storage Acquisition**: Obtain the local storage of the current thread through TLS
3. **Buffer Type Judgment**: Judge whether it is a data containing buffer or an empty buffer according to the memory address
4. **Local Storage Deallocation**:
   - Add the buffer to the corresponding linked list of the local storage
   - Update the buffer count of the local storage
5. **Threshold Check**:
   - Check if the number of buffers in the local storage exceeds `tls_buf_capacity` + `tls_alloc_batch_size`
   - If it exceeds, return the buffers exceeding `tls_buf_capacity` to the global pool
   - Update the counts of the local storage and the global pool
