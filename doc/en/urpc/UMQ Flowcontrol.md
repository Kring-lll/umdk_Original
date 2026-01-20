# UMQ Flowcontrol

## Overview
UMQ supports enabling flow control in UB mode, which determines whether the local UMQ can send packets based on the real-time rx-depth of the peer UMQ.

UMQ flow control primarily controls data transmission by having the receiving end use its own receive buffer space as "credits" to issue to the sending end. The queue's private counters `queue_be_allocated` and `queue_acquired` represent the receive credits allocated to the peer and the send credits acquired from the peer, respectively.

When sending I/O, if it needs to consume the peer UMQ's receive queue buffer, such as UMQ_OPC_SEND/UMQ_OPC_SEND_IMM/UMQ_OPC_WRITE_IMM, UMQ will internally attempt to allocate a queue_acquired based on the number of WRs sent. If the queue_acquired is insufficient, it will return -UMQ_ERR_EAGAIN and set bad_qbuf to point to the qbuf where the transmission failed.

**Instructions for Use**:

Configurable Parameter Description
* initial_credit：The number of available credits allocated to each queue during initialization (this refers to the initialization based on the actual available cache if the number of available caches in the local main queue is insufficient).
* credits_per_request：number of credits allocated per request for each umq
* use_atomic_window: Whether flow control related counters are maintained using atomic variables should be set to true in scenarios where there is concurrency in sending and receiving packets within the same UMQ

Statistical Information Query
The main queue credit pool statistics structure umq_credit_pool_stats contains the following key information:
* pool_idle: The number of credits currently available in the credit pool, referring to the number of available cached credits in the main queue's receive cache queue.
* pool_be_allocated: The number of credits currently allocated and taken from the credit pool, referring to the number of cached credits currently allocated from the main queue's receive cache queue.
* total_pool_idle: The total number of available credits in the current credit pool.
* total_pool_be_allocated：The total number of credits currently allocated and taken from the credit pool.
* total_pool_post_rx_err：The total number of invalid credits (which will cause `pool_idle` to overflow)

the queue-private credit statistics structure umq_credit_private_stats_t mainly contains the following key information:
* queue_idle: Queue-level credits to be allocated to the peer (no practical use, always 0, reserved field)
* queue_be_allocated: Queue-level credits already allocated to the peer, used for local RX direction receive IO
* queue_acquired: Queue-level credits acquired from the peer, used for local TX direction send IO
* total_queue_idle: Queue-level accumulated credits to be allocated to the peer (no practical use, always 0, reserved field)
* total_queue_acquired: Queue-level accumulated transmit credits obtained from the peer, used for TX-direction I/O transmissions.
* total_queue_be_allocated: Queue-level accumulated receive credits already allocated to the peer, used for RX-direction receive I/O transmissions.
* total_queue_post_tx_success: Queue-level accumulated number of successfully transmitted IOs in the TX direction.
* total_queue_post_tx_err: Queue-level accumulated number of transmitted IOs that failed due to insufficient credits in the TX direction.
* total_queue_acquired_err: Queue-level accumulated transmit credits obtained from the peer that were invalid (causing statistical overflow).

The following is an example of using the statistical query interface：
```
umq_flow_control_stats_t s = {0};
umq_user_ctl_in_t in = {.opcode = UMQ_OPCODE_FLOW_CONTROL_STATS_QUERY};
umq_user_ctl_out_t out = {.addr = &s, .len = sizeof(s)};
int ret = umq_user_ctl(umqh, &in, &out);
...
```
