大模型训练包含以下几个关键阶段
1.分词器训练tokenizer
目标：将自然语言字符串转换成模型理解的tokenId
做法：使用BPE（Bype pair encoding）构建词表
产物：tokenizer.json
2.模型预训练pretraining
目的：模型学习知识
任务：让模型在数亿token上做token预测
产物：基座模型
3.指令微调supervised fine-tuning
目的：让模型学会听话，对齐格式。将基座模型转变为对话模型
任务：模型对人工编写的“指令-回答”训练
产物：对齐后模型权重
4。强化学习与人类反馈RLHF
目的：让模型价值观对齐，逻辑进化
任务：奖励模型训练reward modeling，学习人类好恶。策略优化PPO DPO，通过奖励信号微调模型
产物：最终成品模型

prefill: 一次性将整个prompt并行计算，生成KV cache并吐出第一个token，小bs下就能够用满计算资源。瓶颈在于计算
decode：自回归逐个token生成。当前token的K和V会被缓存下来，供之后token的生成使用，计算过程中需要读取KV cache。瓶颈在于对KV cache的访存使用。

chunked prefill
在PD混部时，每个layer做pp的场景下，诞生了chunked prefill用于解决pp之间的空泡。原先是先计算一个batch的prefill后再decode，而chunked prefill将prompt拆分成多个长短一致的小块，允许在pp的空泡中捎带decode的请求。而捎带的decode请求可以复用prefill时fetch的模型参数，减轻memory bound。chunked prefill略微增加了prefill的开销，因为prefill拆分成小块后，后续chunk计算时需要不停地去gpu mem里面读取之前chunk的KV。

continuous batching
传统的静态批处理中，GPU必须等待当前batch中最长的请求生成结束才会开始下一批请求，导致算力空置
而连续批处理不再以请求为单位进行调度，而是以token生成迭代为单位调度
当batch中某个生成请求结束时，会把空位腾出来，将等待队列中的新请求插入。新请求的prefill可以和旧请求的decode在同一个iteration中并行。
但因为prefill对算力占用量大，因此需要配合chunked prefill对prefill进行切分。当前一个token的decode和一个512token的chunked prefill搭配能够较好的互补算力和带宽
同是因为连续批处理会大量申请释放kv cache，如果采用连续物理地址的方案会导致内存有大量碎片。因此常常搭配pagedAttention使用。

pagedAttention
通过block来存储kv cache。通过一个块表，将逻辑上连续的token映射到物理上不连续的块中。解决了内存碎片的问题

radixAttention
对prompt的token进行公共前缀匹配，匹配上的token只存一份kv cache。解决多轮对话的重复计算重复存储。

页表管理
pd分离
因为prefill和decode的瓶颈不同，也产生了PD分离。P节点专门做prefill，D节点专门做decode，中间加一个kv cache模块。kv cache模块既需要管理cache指针，又需要负载均衡，将kv cache发给最空闲的d节点。pd分离的挑战是kv cache传输。核心离不开rdma协议。kv的良好以及每层kv cache算完后立刻发给d节点，实现流水并行

残差链接
attention和ffn计算后会加上残差

layernorm或rmsnorm
1.在attention前，保护注意力权重计算，防止softmax饱和
2.ffn前，对齐残差相加后的特征分布
3.gating前有。常见，防止token发往同一个专家

attention
这里取self-attention。
1.原始输入向量x，分别通过Q,K,V的线性变换得到Q,K,V。
2.之后对Q和K做rope的线性变换。
3.之后做Q @ K^T计算出分数并对分数用dHead^0.5做scale（防止点击过大，softmax后落在梯度极小的饱和区）。再对分数加上上三角为负无穷的mask。之后算出softmax。
4.再用softmax @ V并通过output的线性层得到输出

rope
位置编码，作用于QK。如果dHead为128，则会有64个旋转平面，旋转平面序号为i，token位置为m，则每个token的逆时针旋转角度为m * 10000^(-2i/d)

gating
1.attention的输出通过线性层得到对所有专家的原始分数
2.保留前k个最大值，其他的设置为负无穷
3.对选中的专家分数进行softmax处理，获得权重

dispatch
按照token的topk专家选择，将token发送给抓对应ep的专家。实际上是一个count数量的alltoall和一次token发送的alltoallv。通过采用动态量化，降低通信量

ffn
ffn是专家对token的处理过程。为线性层升维，激活，再线性层降维。
以下场景取激活函数为swiglu，入参token为x，线性层矩阵W1,W2，门控矩阵为Wg1，Wg2。swiglu = (x @ Wg1) * swish(x @ Wg2)。计算时常将Wg1 Wg2拼接成一个W1与x做乘法，之后再列切计算。
w8a8场景，x和W1,W2都是int8类型，x_scale, W1_scale和W2_scale为float类型
1. 计算获取 x @ W1，结果为float。
2. 乘以W1_scale和x_scale，结果为float
3. 将结果列切带入swiglu计算得到激活值。结果为float
4. 将结果量化为int8，同时输出quant_scale。
5. 计算 x @ W2，结果为float
6. 乘以W2_scale和quant_scale，结果为float。最终转换为bfloat16输出

w16a16场景，x和W1,W2都是bfloat16类型
1. 计算获取x @ W1，结果为float。
2. 将结果列切带入swiglu计算得到激活值。结果为float
3. 转换为bfloat16类型，与W2做乘法，结果为float。最终转换为bfloat16输出

combine
将经过topk专家处理后的结果收集并加权求和得到结果。实际上是一个token发送的alltoallv和一次weighted sum过程

NV
单机跨卡访问gpu内存通过NVLINK。UVA确保每个gpu的显存在虚拟地址中的唯一性。这样不同进场可以直接访问其他gpu显存
跨机场景涉及到了rdma通信，通过ibgda技术，通过PCIE总线的P2P技术将GPU物理显存映射到了网卡的DMA空间，这样网卡能够直接访问gpu显存。doorbell地址被映射到了gpu上，gpu可以绕过cpu去提交rdma任务

NVshmem
初始化时会在所有gpu上分批额一段大小相同、对齐方式相同的显存空间，NVshmem将显存映射到相同的虚拟空间
对称虚拟地址管理+UCX/Libfabric传输调度+gpu直驱rdma就是nvshmem的跨机实现
通过nvshmem_put，nvshmem_get

昇腾
没有UVA支持，npu内存地址空间是隔离的。