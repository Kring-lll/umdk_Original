# UMQ Flowcontrol

## 功能简介
UMQ支持在UB模式下启用流量控制功能，根据对端UMQ的接收队列实时可用深度确定本端UMQ是否可执行发包。

流控的Jetty和IO的jetty是独立的，umq_bind内部会补充流控jetty接收深度，用户不涉及流控jetty的rx的补充。
启用流量控制时，UMQ会使用IO立即数交换接收队列的可用深度，并且在IO立即数中占用1bit位（LSB）用于区分该立即数是应用IO，还是UMQ内部流程使用的。因此，该场景下应用需关注，最多只能使用63bit立即数空间。

UMQ流量控制维护了queue_be_allocted和queue_acquired两个计数，分别表示以queue粒度已分配给对端的接收信用和以queue粒度从对端获取到的发送信用

在UMQ初始化阶段，比如umq_bind阶段，当填充流控的接收缓存数到深度2时，UMQ内部会将流控的RX深度设置到可供对端UMQ读取的内存中。用户通过poll rx，读取到对端的流控接收缓存深度为2， 则将本端queue状态置ready状态，且内部发起一个主动申请获取credit的请求，请求的信用数为initial_credit。用户需要做poll rx更新本端的queue_acquired。对端UMQ接收到申请credit请求以后，从credit池子中分配出initial_credit，记录到queue_be_allocted，发起一个发送的立即数请求。本端接收到umq_buf_t中设置status为UMQ_BUF_FLOW_CONTROL_UPDATE，可根据该状态确定继续发包的时机。

当发送IO时，如果需要消耗对端的接收队列缓存，如UMQ_OPC_SEND/UMQ_OPC_SEND_IMM/UMQ_OPC_WRITE_IMM，UMQ内部会尝试根据发送wr的数量申请queue_acquired，如果queue_acquired不足，则返回-UMQ_ERR_EAGAIN，并且将bad_qbuf指向未成功发送的qbuf处。

## 使用说明
可配置参数说明：
* initial_credit：每个umq 初始化分配的可用信用数（当信用池可用不足，则依当前信用池里实际可用的信用数分配）
* credits_per_request：发起申请信用请求时，填充的信用数
* use_atomic_window: 流量控制相关计数是否采用原子变量维护，在同一个UMQ收发包存在并发的场景需设置为true

统计信息查询：
信用池有以下关键信息：
* pool_idle: 当前信用池可用的信用数
* pool_be_allocated: 当前信用池被分配取走的信用数
* total_pool_idle: 当前信用池可用信用累计总数
* total_pool_be_allocated：当前信用池累计被分配取走的总数
* total_pool_post_rx_err：不合法（会导致pool_idle统计溢出）信用总数

具体queue的流量信息主要有以下关键信息：
* queue_idle: queue粒度待分配给对端接收的信用
* queue_be_allocted：queue粒度已分配给对端的接收信用
* queue_acquired：queue粒度从对端获取到的发送信用，用于本端TX方向发送IO
* total_queue_idle：queue粒度待分配给对端接收信用累计值
* total_queue_acquired：queue粒度从对端获取到的发送信用累计值，用于本端TX方向发送IO
* total_queue_be_allocated：queue粒度已分配给对端的接收信用累计值，用于本端RX方向接收IO
* total_queue_post_tx_success：queue粒度本端TX方向发送IO成功的wr数量累计值
* total_queue_post_tx_err：queue粒度本端TX方向由于信用不足导致发送IO失败的wr数量累计值
* total_queue_acquired_err：queue粒度从对端获取到不合法（导致统计溢出）的发送信用累计值

统计查询接口使用示例如下：
```
umq_flow_control_stats_t s = {0};
umq_user_ctl_in_t in = {.opcode = UMQ_OPCODE_FLOW_CONTROL_STATS_QUERY};
umq_user_ctl_out_t out = {.addr = &s, .len = sizeof(s)};
int ret = umq_user_ctl(umqh, &in, &out);
...
```
