# UMQ Flowcontrol

## Overview
UMQ supports enabling flow control in UB mode, which determines whether the local UMQ can send packets based on the real-time rx-depth of the peer UMQ.

The flow control jetty and the IO jetty are independent. umq_bind will internally supplement the flow control jetty's receive depth, and the user does not need to supplement the flow control jetty's rx.
When flow control is enabled, UMQ uses IO immediate data ​​to exchange the available rx-depth of the UMQ, and occupies 1 bit (LSB) in the IO immediate data to distinguish whether the immediate data is used by application IO or by an internal UMQ process. Therefore, in this scenario, applications need to be aware that they can only use a maximum of 63 bits of immediate data space.

UMQ flow control maintains two counts: queue_be_allocated and queue_acquired. These counters represent the number of receive credits allocated to the peer at the queue level,and the number of send credits acquired from the peer at the queue level, respectively.

During the UMQ initialization phase, such as the umq_bind phase, when the receive buffer count filled with flow control reaches a depth of 2, UMQ internally sets the RX depth of flow control in the memory supplied to the peer UMQ for reading. If a user reads that the peer's flow control receive buffer depth is 2 via poll rx, then the user sets the local queue state to ready and internally initiates a request to actively acquire credits, with the requested credit count being initial_credit.Users need to perform poll rx updates to their local queue_acquired. After receiving a credit request, the peer UMQ allocates initial_credit from the credit pool, records it in queue_be_allocated, and initiates an immediate send request. The local end receives the umq_buf_t with its status set to UMQ_BUF_FLOW_CONTROL_UPDATE, and can use this status to determine when to continue sending packets.

After the peer UMQ receives the notification, it increments the remote_rx_window counter and sets the status in umq_buf_t to UMQ_BUF_FLOW_CONTROL_UPDATE. The application can determine when to continue sending packets based on this status.

When sending I/O, if it needs to consume the peer UMQ's receive queue buffer, such as UMQ_OPC_SEND/UMQ_OPC_SEND_IMM/UMQ_OPC_WRITE_IMM, UMQ will internally attempt to allocate a queue_acquired based on the number of WRs sent. If the queue_acquired is insufficient, it will return -UMQ_ERR_EAGAIN and set bad_qbuf to point to the qbuf where the transmission failed.

**Instructions for Use**:

Configurable Parameter Description
* initial_credit：initial available credit for each umq
* credits_per_request：number of credits allocated per request for each umq
* use_atomic_window: Whether flow control related counters are maintained using atomic variables should be set to true in scenarios where there is concurrency in sending and receiving packets within the same UMQ

Statistical Information Query
The credit pool contains the following key information:
* pool_idle: The number of credits currently available in the credit pool.
* pool_be_allocated: The number of credits currently allocated and taken from the credit pool.
* total_pool_idle: The total number of available credits in the current credit pool.
* total_pool_be_allocated：The total number of credits currently allocated and taken from the credit pool.
* total_pool_post_rx_err：The total number of invalid credits (which will cause `pool_idle` to overflow)

The key traffic information for a queue includes:
* queue_idle: Queue granularity of credits to be allocated to the peer for reception
* queue_be_allocated: Queue granularity of reception credits already allocated to the peer for reception
* queue_acquired: Queue granularity of transmission credits acquired from the peer for transmission in the local TX direction.
* total_queue_idle: Queue-level accumulated receive credits to be allocated to the peer.
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
