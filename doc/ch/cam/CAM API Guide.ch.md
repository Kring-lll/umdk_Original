# CAM API指南

## CAM简介

CAM是Huawei昇腾NPU超节点通信加速器(Communication Accelerator for Maxtrix)的简称，提供EP（Expert Parallelism）通信加速库、PD（Prefill & Decode）分离场景高性能KVCache传输和KVC池化、AFD（Attention-FFN Disaggregation）通信加速库、RL(Reinforcement Learning)权重传输等特性。

## CAM架构

（To be done）

## CAM API

### 1. 高性能EP通信库

CAM在umdk\_cam\_op\_lib库中提供高性能Python通信和通算融合接口供用户使用，用户可以方便的在主流昇腾推理框架（如vllm-ascend, sglang-kernel-npu等）导入该通信库并调用接口使用。

#### 1.1 Dispatch & Combine类接口

#### 1.1.1 moe\_dispatch\_shmem ▶

##### 1.1.1.1 接口原型

```python
moe_dispatch_shmem(
    Tensor x, 
    Tensor expert_ids, 
    Tensor scales, 
    Tensor x_active_mask, 
    int ep_world_size, 
    int ep_rank_id, 
    int moe_expert_num, 
    int tp_world_size, 
    int tp_rank_id, 
    int expert_shard_type, 
    int shared_expert_num, 
    int shared_expert_rank_num, 
    int quant_mode, 
    int global_bs, 
    int expert_token_nums_type, 
    int ext_info)
-> output: List[Tensor]
```

##### 1.1.1.2 接口描述

基于SHMEM类内存的Dispatch接口，用以在EP通信阶段将token分发至不同的专家以供后续的操作。该接口需配合moe\_combine\_shmem配套使用。

##### 1.1.1.3 入参

| **📌参数**                  | **🔧类型** | **✅是否必选** | **📋取值说明**                                                | **📝描述**                        |
| ------------------------- | -------- | --------- | --------------------------------------------------------- | ------------------------------- |
| x                         | Tensor   | 必选        | 形状:(batch\_size, hidden\_size)                            | 输入Token                         |
| expert\_ids               | Tensor   | 必选        | 形状:(batch\_size, top\_k)                                  | 目的专家ID信息                        |
| scales                    | Tensor   | 可选        | 非空时为float类型，存在共享专家时形状:(m+1,h), 不存在共享专家时形状(m, h),其中m为共享专家数 | 量化参数                            |
| x\_active\_mask           | Tensor   | 可选        | 暂不支持，传入None                                               | --                              |
| ep\_world\_size           | int      | 必选        | 只支持如下取值：\[8, 16, 32, 64, 128, 144, 256, 288]              | EP通信域内的rank数                    |
| ep\_rank\_id              | int      | 必选        | \[0, ep\_world\_size-1]                                   | EP通信域内rank ID号                  |
| moe\_expert\_num          | int      | 必选        | \[1, 512]                                                 | MoE专家数                          |
| tp\_world\_size           | int      | 必选        | 暂不支持，传入0                                                  | --                              |
| tp\_rank\_id              | int      | 必选        | 暂不支持，传入0                                                  | --                              |
| expert\_shard\_type       | int      | 必选        | 暂不支持，传入0                                                  | --                              |
| shared\_expert\_num       | int      | 必选        | 不支持非1的值，传入1                                               | 共享专家数量                          |
| shared\_expert\_rank\_num | int      | 必选        | \[0, ep\_world\_size-1]                                   | 共享专家卡号                          |
| quant\_mode               | int      | 必选        | 非量化传0，量化传2                                                | 量化模式                            |
| global\_bs                | int      | 必选        | 根据实际情况传入，由实际内存大小约束                                        | EP通信域全局BS大小                     |
| expert\_token\_nums\_type | int      | 必选        | 传入0：输出每个专家处理的token数量；传入1：输出每个专家处理的token前缀和。               | 输出expert\_token\_nums\_out的数据格式 |
| ext\_info                 | int      | 必选        | --                                                        | SHMEM初始化后返回的基地址指针               |

##### 1.1.1.4 返回值

函数返回值是一个由Tensor构成的List，依次存放：expand\_x, dynamic\_scales, expand\_idx, expert\_token\_nums, ep\_send\_count, tp\_send\_count和expand\_scales.

| **📌参数**            | **🔧类型** | **📋取值说明**                                                                                                                                                          | **📝描述**                                      |
| ------------------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------- |
| expand\_x           | Tensor   | 当前rank是共享专家时，形状:(rank\_size \* batch\_size / shared\_expert\_num, hidden\_size);当前rank是路由专家时，形状：(expert\_num\_per\_rank \* rank\_size \* batch\_size, hidden\_size) | 每个rank上所有专家的token                             |
| dynamic\_scales     | Tensor   | 形状同expand\_x的第一维，即:当前rank是共享专家时，形状:(rank\_size \* batch\_size / shared\_expert\_num);当前rank是路由专家时，形状：(expert\_num\_per\_rank \* rank\_size \* batch\_size)          | 量化参数信息                                        |
| expand\_idx         | Tensor   | 形状：(batch\_size, top\_k)                                                                                                                                            | 在目标专家内，仅排序当前rank的token时，当前rank发出的token各自的排序ID |
| expert\_token\_nums | Tensor   | (expert\_num\_on\_rank)                                                                                                                                             | 当前rank上每个专家收到的token数                          |
| ep\_send\_count     | Tensor   | 形状：(expert\_num\_per\_rank \* ep\_world\_size)                                                                                                                      | 每个专家从每个rank收到的token数                          |
| tp\_send\_count     | Tensor   | --                                                                                                                                                                  | 暂不支持，无意义                                      |
| expand\_scales      | Tensor   | --                                                                                                                                                                  | 暂不支持，无意义                                      |

##### 1.1.1.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当量化模式开启时，expand\_x的数据类型为int8类型，而不开启量化时其数据类型为bfloat16类型。
3. 当前接口不支持A2环境调用。
4. 当前接口不支持并发调用。
5. 当前接口在GE图模式下不支持动态图， 不支持fullgraph=true的选项。
6. 当前x入参的输入格式不支持bfloat16类型。
7. 当前不支持共享专家功能。
8. 除满足上述形状约束外，其他参数取值要求：

- top\_k当前仅支持8
- 需要满足：(moe\_expert\_num + shared\_expert\_rank\_num) ≤ CAM\_MAX\_EXPERT\_NUM, 当前最大专家数为512
- 需要满足: moe\_expert\_num % (ep\_world\_size - shared\_expert\_rank\_num) == 0
- 需要满足：moe\_expert\_num / (ep\_world\_size - shared\_expert\_rank\_num) ≤ MAX\_EXPERT\_PER\_RANK, 当前该值设置为32
- 需要满足： (ep\_world\_size + block\_num -1) / block\_num ≤ MULTI\_RANK\_SIZE
- 需要满足： 如果shared\_expert\_rank\_num不为0，则ep\_world\_size需要为其整数倍，切ep\_world\_size ≠ shared\_expert\_rank\_num
- 需要满足：(batch\_size \* hidden\_size \* ep\_world\_size \* expert\_num\_per\_rank \* 2)小于ext\_info指向的地址空间大小

#### 1.1.2 moe\_combine\_shmem ▶

##### 1.1.2.1 接口原型

```python
moe_combine_shmem(
    Tensor expand_x, 
    Tensor expert_ids, 
    Tensor expand_idx, 
    Tensor ep_send_counts, 
    Tensor expert_scales, 
    Tensor tp_send_counts, 
    Tensor x_active_mask, 
    Tensor activation_scale, 
    Tensor weight_scale, 
    Tensor group_list, 
    Tensor expand_scales, 
    int ep_world_size, 
    int ep_rank_id, 
    int moe_expert_num, 
    int tp_world_size, 
    int tp_rank_id, 
    int expert_shard_type, 
    int shared_expert_num, 
    int shared_expert_rank_num, 
    int global_bs, 
    int comm_quant_mode, 
    int ext_info, 
    int out_dtype, 
    int group_list_type)
-> output: Tensor
```

##### 1.1.2.2 接口描述

基于SHMEM类内存的Combine接口，用以在EP通信阶段将分发至不同的专家的token回合以供后续的操作。该接口需配合moe\_dispatch\_shmem配套使用。

##### 1.1.2.3 入参

| **📌参数**                  | **🔧类型** | **✅是否必选** | **📋取值说明**                                     | **📝描述**                                      |
| ------------------------- | -------- | --------- | ---------------------------------------------- | --------------------------------------------- |
| expand\_x                 | Tensor   | 必选        | 形状同dispatch的出参expand\_x                        | dispatch分发至各专家上的token                         |
| expert\_ids               | Tensor   | 必选        | 形状:(batch\_size, top\_k)                       | 目的专家ID信息                                      |
| expand\_idx               | Tensor   | 必选        | 形状:(batch\_size, top\_k)                       | 在目标专家内，仅排序当前rank的token时，按照rank发出的token各自的排序ID |
| ep\_send\_counts          | Tensor   | 必选        | 形状:(expert\_num\_per\_rank \* ep\_world\_size) | 每个专家从每个rank收到的token数                          |
| expert\_scales            | Tensor   | 必选        | 形状：（batch\_size, top\_k）                       | 合并token时需要的权重                                 |
| tp\_send\_count           | Tensor   | 可选        | 暂不支持，传入int32类型的tensor\[0]即可                    | --                                            |
| x\_active\_mask           | Tensor   | 可选        | 暂不支持，传入None                                    | --                                            |
| activation\_scale         | Tensor   | 可选        | 暂不支持，传入None                                    | --                                            |
| weight\_scale             | Tensor   | 可选        | 暂不支持，传入None                                    | --                                            |
| group\_list               | Tensor   | 可选        | 暂不支持，传入None                                    | --                                            |
| expand\_scales            | Tensor   | 可选        | 暂不支持，传入None                                    | --                                            |
| ep\_world\_size           | int      | 必选        | 只支持如下取值：\[8, 16, 32, 64, 128, 144, 256, 288]   | EP通信域内的rank数                                  |
| ep\_rank\_id              | int      | 必选        | \[0, ep\_world\_size-1]                        | EP通信域内rank ID号                                |
| moe\_expert\_num          | int      | 必选        | \[1, 512]                                      | MoE专家数                                        |
| tp\_world\_size           | int      | 必选        | 暂不支持，传入1                                       | --                                            |
| tp\_rank\_id              | int      | 必选        | 暂不支持，传入0                                       | --                                            |
| expert\_shard\_type       | int      | 必选        | 暂不支持，传入0                                       | --                                            |
| shared\_expert\_num       | int      | 必选        | 不支持非1的值，传入1                                    | 共享专家数量                                        |
| shared\_expert\_rank\_num | int      | 必选        | \[0, ep\_world\_size-1]                        | 共享专家卡号                                        |
| global\_bs                | int      | 必选        | 根据实际情况传入，由实际内存大小约束                             | EP通信域全局BS大小                                   |
| out\_dtype                | int      | 必选        | 暂不支持，传入0                                       | --                                            |
| comm\_quant\_mode         | int      | 必选        | 非量化传0，量化传2                                     | 量化模式                                          |
| group\_list\_type         | int      | 必选        | 暂不支持，传入0                                       | --                                            |
| ext\_info                 | int      | 必选        | --                                             | SHMEM初始化后返回的基地址指针                             |

##### 1.1.2.4 返回值

函数返回值是一个Tensor，存放expand\_x信息。

| **📌参数**  | **🔧类型** | **📋取值说明**                     | **📝描述**    |
| --------- | -------- | ------------------------------ | ----------- |
| expand\_x | Tensor   | 形状:(batch\_size, hidden\_size) | 合并后的token信息 |

##### 1.1.2.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当前接口不支持A2环境调用。
3. 当前接口不支持并发调用。
4. 当前接口在GE图模式下不支持动态图， 不支持fullgraph=true的选项。
5. 当前不支持共享专家功能。
6. 除满足上述形状约束外，其他参数取值要求：

- top\_k当前仅支持8
- 需要满足：(moe\_expert\_num + shared\_expert\_rank\_num) ≤ CAM\_MAX\_EXPERT\_NUM, 当前最大专家数为512
- 需要满足: moe\_expert\_num % (ep\_world\_size - shared\_expert\_rank\_num) == 0
- 需要满足：moe\_expert\_num / (ep\_world\_size - shared\_expert\_rank\_num) ≤ MAX\_EXPERT\_PER\_RANK, 当前该值设置为32
- 需要满足： (ep\_world\_size + block\_num -1) / block\_num ≤ MULTI\_RANK\_SIZE,
- 需要满足： 如果shared\_expert\_rank\_num不为0，则ep\_world\_size需要为其整数倍，切ep\_world\_size ≠ shared\_expert\_rank\_num
- 需要满足：(batch\_size \* hidden\_size \* ep\_world\_size \* expert\_num\_per\_rank \* 2)小于ext\_info指向的地址空间大小

#### 1.1.3 get\_dispatch\_layout ▶

##### 1.1.3.1 接口原型

```python
get_dispatch_layout(
    Tensor topk_idx, 
    int num_experts, 
    int num_ranks)
-> output: tuple(Tensor, Tensor)
```

##### 1.1.3.2 接口描述

!\[get\_dispatch\_layout示意图]\(figures/get\_dispatch\_layout\_a3.png null)
A3代际Prefill阶段Dispatch使用的前置接口，用以在当前rank将Token拓展TopK份之后按照专家粒度重排，方便后续的分发操作。该接口需配合moe\_dispatch\_prefill和moe\_combine\_prefill使用。

##### 1.1.3.3 入参

| **📌参数**     | **🔧类型** | **✅是否必选** | **📋取值说明**                      | **📝描述**   |
| ------------ | -------- | --------- | ------------------------------- | ---------- |
| topk\_idx    | Tensor   | 必选        | 形状:(batch\_size, topk)， int64类型 | 目标专家的ID信息  |
| num\_experts | int      | 必选        | 取值范围：(0, 512]                   | MOE专家数     |
| num\_ranks   | int      | 必选        | 取值范围：\[1, 384]                  | EP通信域rank数 |

##### 1.1.3.4 返回值

函数返回值是一个2个Tensor构成的Tuple，分别存放：number\_tokens\_per\_expert和send\_token\_idx.

| **📌参数**                    | **🔧类型** | **📋取值说明**               | **📝描述**                                  |
| --------------------------- | -------- | ------------------------ | ----------------------------------------- |
| number\_tokens\_per\_expert | Tensor   | 形状：（num\_experts）        | 当前rank上发送给每个专家的token个数                    |
| send\_token\_idx            | Tensor   | 形状：(batch\_size, top\_k) | 当前rank上，发送给每个专家的token在以专家重排分桶后，其在桶里的第几个位置 |

##### 1.1.3.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当前接口只支持A3环境调用。
3. 当前接口不支持并发调用。
4. 当前接口不支持入图使用。
5. 除满足上述形状约束外，其他参数取值要求：

- top\_k取值范围：(0， 16]
- 需要满足: num\_experts % num\_ranks == 0

#### 1.1.4 moe\_dispatch\_prefill ▶

##### 1.1.4.1 接口原型

```python
moe_dispatch_prefill(
    Tensor x, 
    Tensor topk_idx, 
    Tensor topk_weights, 
    Tensor num_tokens_per_expert, 
    Tensor send_token_idx_small, 
    str group_ep, 
    int rank, 
    int num_ranks, 
    bool use_quant) 
-> output: tuple(Tensor, Tensor, Tensor, Tensor, Tensor)
```

##### 1.1.4.2 接口描述

!\[moe\_dispatch\_prefill示意图]\(figures/moe\_dispatch\_prefill\_a3.png null)
A3代际Prefill阶段Dispatch接口，将Token按照topk\_idx的规则发送给对应专家。

##### 1.1.4.3 入参

| **📌参数**                    | **🔧类型** | **✅是否必选** | **📋取值说明**                                        | **📝描述**                                  |
| --------------------------- | -------- | --------- | ------------------------------------------------- | ----------------------------------------- |
| x                           | Tensor   | 必选        | 形状:(batch\_size, hidden\_size), 支持bf16, float16类型 | 本卡发送的token                                |
| topk\_idx                   | Tensor   | 必选        | 形状:(batch\_size, topk)， 数据类型为int64                | 每个token的目标专家ID信息                          |
| topk\_weights               | Tensor   | 必选        | 形状:(batch\_size, topk)， 数据类型为float32              | 每个token的topk个目标专家的权重信息                    |
| number\_tokens\_per\_expert | Tensor   | 必选        | 形状：（num\_experts），数据类型为int                        | 当前rank上发送给每个专家的token个数                    |
| send\_token\_idx\_small     | Tensor   | 必选        | 形状：(batch\_size, top\_k), 数据类型为int                | 当前rank上，发送给每个专家的token在以专家重排分桶后，其在桶里的第几个位置 |
| group\_ep                   | str      | 必选        | --                                                | HCCL通信域名称                                 |
| rank                        | int      | 必选        | \[0, num\_ranks)                                  | 本卡在通信域中的rankID                            |
| num\_ranks                  | int      | 必选        | \[2, 384]                                         | EP通信域rank数                                |
| use\_quant                  | bool     | 必选        | True: 开启量化； False: 关闭量化                           | Dispatch量化指示符                             |

##### 1.1.4.4 返回值

函数返回值是一个5个Tensor构成的Tuple，分别存放：recv\_x, dynamic\_scales\_out, expand\_idx\_out, recv\_count, recv\_token\_per\_expert.

| **📌参数**                  | **🔧类型** | **📋取值说明**                                                                                                         | **📝描述**                                                                                       |
| ------------------------- | -------- | ------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------- |
| recv\_x                   | Tensor   | 形状：(recv\_token\_num, hidden\_size), 其中recv\_token\_num为本卡收到的token个数。当use\_quant为true时，数据类型为int8, false时数据类型与入参x一致 | 当前rank上收到的token信息                                                                              |
| dynamic\_scales\_out      | Tensor   | 形状：(recv\_token\_num), 数据类型为float.当use\_quant为false时该值没有意义。                                                        | 当前rank上收到token的动态量化scale信息                                                                     |
| expand\_idx\_out          | Tensor   | 形状：(recv\_token\_num \* 3), 数据类型为int                                                                               | 本卡收到的token信息三元组，每组三个数的含义依次为：token的源rank, token在源rank的序号（BS视角），token在源rank时topk专家扩展重排后的序号（专家视角） |
| recv\_count               | Tensor   | 形状：(num\_experts), 数据类型为int                                                                                        | 当前rank上每个专家从每个rank收到的收到token数，为前缀和                                                             |
| recv\_tokens\_per\_expert | Tensor   | 形状：(local\_expert\_num), 数据类型为int64                                                                                | 当前rank上每个专家收到的token信息                                                                          |

##### 1.1.4.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当前接口只支持A3环境调用。
3. 当前接口不支持并发调用。
4. 当前接口不支持入图使用。
5. 除满足上述形状约束外，其他参数取值要求：

- 需要满足：BS取值范围\[1, 8K]
- 需要满足: num\_ranks取值范围\[2, 384]
- 需要满足: num\_experts取值范围(0, 512]
- 需要满足: topk取值范围(0, 16]
- 需要满足: (num\_experts % num\_ranks) == 0
- 需要满足: 配置全局宏HCCL\_BUFFERSIZE=4096

#### 1.1.5 moe\_combine\_prefill ▶

##### 1.1.5.1 接口原型

```python
moe_combine_prefill(
    Tensor x, 
    Tensor topk_idx, 
    Tensor topk_weights, 
    Tensor src_idx, 
    Tensor send_head,
    str group_ep, 
    int rank, 
    int num_ranks) 
-> output: Tensor
```

##### 1.1.5.2 接口描述

!\[moe\_combine\_prefill示意图]\(figures/moe\_combine\_prefill\_a3.png null)
A3代际Prefill阶段Combine接口，将按照topk\_idx的规则发送给对应专家的token，按照topk\_weights指定的权重收回。

##### 1.1.5.3 入参

| **📌参数**      | **🔧类型**   | **✅是否必选**                            | **📋取值说明**                                                                                                                                 | **📝描述**                                                                                  |
| ------------- | ---------- | ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------- |
| x             | Tensor     | 必选                                   | 形状:(recv\_token\_num, hidden\_size), 支持bf16, float16类型                                                                                     | 本卡dispatch阶段收集到的token                                                                     |
| topk\_idx     | Tensor     | 必选                                   | 形状:(batch\_size, topk)， 数据类型为int64                                                                                                         | 每个token的目标专家ID信息                                                                          |
| topk\_weights | Tensor     | 必选                                   | 形状:(batch\_size, topk)， 数据类型为float32                                                                                                       | 每个token的topk个目标专家的权重信息                                                                    |
| src\_idx      | Tensor     | 形状：(recv\_token\_num \* 3), 数据类型为int | 本卡收到的token信息三元组，每组三个数的含义依次为：token的源rank, token在源rank的序号（BS视角），token在源rank时topk专家扩展重排后的序号（专家视角）。对应moe\_dispatch\_prefill的出参expand\_idx\_out | <br />                                                                                    |
| <br />        | send\_head | Tensor                               | 形状：(num\_experts), 数据类型为int                                                                                                                | 当前rank上每个专家从每个rank收到的收到token的动态量化scale信息，该信息按照一维排开。对应moe\_dispatch\_prefill的出参recv\_count |
| group\_ep     | str        | 必选                                   | --                                                                                                                                         | HCCL通信域名称                                                                                 |
| rank          | int        | 必选                                   | \[0, num\_ranks)                                                                                                                           | 本卡在通信域中的rankID                                                                            |
| num\_ranks    | int        | 必选                                   | \[2, 384]                                                                                                                                  | EP通信域rank数                                                                                |

##### 1.1.5.4 返回值

函数返回值是一个Tensor，存放combine\_x信息。

| **📌参数**   | **🔧类型** | **📋取值说明**                              | **📝描述**          |
| ---------- | -------- | --------------------------------------- | ----------------- |
| combine\_x | Tensor   | 形状：(batch\_size, hidden\_size)。数据类型与x一致 | 当前rank上收到的token信息 |

##### 1.1.5.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当前接口只支持A3环境调用。
3. 当前接口不支持并发调用。
4. 当前接口不支持入图使用。
5. 除满足上述形状约束外，其他参数取值要求：

- 需要满足：BS取值范围\[1, 8K]
- 需要满足: num\_ranks取值范围\[2, 384]
- 需要满足: num\_experts取值范围(0, 512]
- 需要满足: topk取值范围(0, 16]
- 需要满足: (num\_experts % num\_ranks) == 0
- 需要满足: 配置全局宏HCCL\_BUFFERSIZE=4096

#### 1.1.6 get\_dispatch\_layout\_a2 ▶

##### 1.1.6.1 接口原型

```python
get_dispatch_layout_a2(
    Tensor topk_idx, 
    int num_experts, 
    int num_ranks)
-> output: tuple(Tensor, Tensor)
```

##### 1.1.6.2 接口描述

!\[get\_dispatch\_layout\_a2示意图]\(figures/get\_dispatch\_layout\_a2.png null)
A2代际Prefill阶段Dispatch使用的前置接口，用以在当前rank将Token拓展TopK份(存在共享专家时，此处为(topK+1)份)之后按照专家粒度重排，方便后续的分发操作。该接口需配合moe\_dispatch\_prefill\_a2和moe\_combine\_prefill\_a2使用。

##### 1.1.6.3 入参

| **📌参数**     | **🔧类型** | **✅是否必选** | **📋取值说明**                                              | **📝描述**   |
| ------------ | -------- | --------- | ------------------------------------------------------- | ---------- |
| topk\_idx    | Tensor   | 必选        | 形状:(batch\_size, topk)， int64类型，取值范围：\[0, num\_experts) | 目标专家的ID信息  |
| num\_experts | int      | 必选        | 取值范围：(0, 256]                                           | MOE专家数     |
| num\_ranks   | int      | 必选        | 当前仅支持16                                                 | EP通信域rank数 |

##### 1.1.6.4 返回值

函数返回值是一个2个Tensor构成的Tuple，分别存放：number\_tokens\_per\_expert和notify\_send\_data.

| **📌参数**                    | **🔧类型** | **📋取值说明**                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | **📝描述**                                                                                                                                                                                                                                                  |
| --------------------------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| number\_tokens\_per\_expert | Tensor   | 形状：（num\_experts）,int类型                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | 当前rank上发送给每个专家的token个数                                                                                                                                                                                                                                    |
| notify\_send\_data          | Tensor   | 形状：(num\_experts \* EXPERT\_DATA\_SIZE + server\_num + max\_bs \* (1 + 2\* server\_num + num\_experts)), 数据类型为int。当前EXPERT\_DATA\_SIZE=4097，max\_bs=4096。七个部分的形状信息： 1. num\_tokens\_per\_expert, 形状：（num\_experts）； 2. num\_token\_per\_server\_uniq, 形状：（num\_experts）； 3. num\_each\_token\_to\_server, 形状：（max\_bs \* num\_server）; 4. each\_token\_to\_num\_server, 形状：（max\_bs）; 5. each\_token\_offset\_to\_server, 形状：（max\_bs \* num\_server）； 6. send\_token\_idx, 形状：（max\_bs \* num\_experts）； 7. expert\_rank\_token\_idx, 形状：（num\_experts， max\_bs）； | 由七个部分组成的tensor,分别表示 1. 每个expert从本卡收到的token数目； 2. 每个server从本卡接收到的token数目（去重）； 3. 本卡每个token发往每个server的个数； 4. 本卡每个token发往的server个数； 5. 本卡每个token发往每个server,token的顺序偏移。 6. 本卡每个token按照专家维度分桶，在桶中的序号偏移。 7. 每个专家收到的每个token，其对应的each\_token\_offset\_to\_server值 |

##### 1.1.6.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当前接口只支持A2环境调用。
3. 当前接口不支持并发调用。
4. 当前接口不支持入图使用。
5. 当前接口不支持共享专家。
6. 除满足上述形状约束外，其他参数取值要求：

- top\_k取值范围：(2， 16]
- 需要满足: num\_experts % num\_ranks == 0
- 需要满足: num\_ranks % 8 == 0
- 需要配置：export HCCL\_INTRA\_PCIE\_ENABLE = 1, export HCCL\_INTRA\_ROCE\_ENABLE = 0

#### 1.1.7 moe\_dispatch\_prefill\_a2 ▶

##### 1.1.7.1 接口原型

```python
moe_dispatch_prefill_a2(
    Tensor x, 
    Tensor topk_idx, 
    Tensor topk_weights, 
    Tensor num_tokens_per_expert,
 	Tensor notify_send_data, 
    str group_ep, 
    int rank, 
    int num_ranks, 
    bool use_quant) 
-> output: Tensor[]
```

##### 1.1.7.2 接口描述

!\[moe\_dispatch\_prefill\_a2示意图]\(figures/moe\_dispatch\_prefill\_a2.png null)
A2代际Prefill阶段Dispatch接口，将Token按照topk\_idx的规则发送给对应专家。

##### 1.1.7.3 入参

| **📌参数**                    | **🔧类型** | **✅是否必选** | **📋取值说明**                                                                                                        | **📝描述**                                |
| --------------------------- | -------- | --------- | ----------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| x                           | Tensor   | 必选        | 形状:(batch\_size, hidden\_size), 支持bf16, float16类型                                                                 | 本卡发送的token                              |
| topk\_idx                   | Tensor   | 必选        | 形状:(batch\_size, topk)， 数据类型为int64，取值范围\[0, num\_experts)                                                         | 每个token的目标专家ID信息                        |
| topk\_weights               | Tensor   | 必选        | 形状:(batch\_size, topk)， 数据类型为float32                                                                              | 每个token的topk个目标专家的权重信息                  |
| number\_tokens\_per\_expert | Tensor   | 必选        | 形状：（num\_experts），数据类型为int                                                                                        | 当前rank上发送给每个专家的token个数                  |
| notify\_send\_data          | Tensor   | 必选        | 形状：(num\_experts \* EXPERT\_DATA\_SIZE + server\_num + max\_bs \* (1 + 2\* server\_num + num\_experts)), 数据类型为int | get\_dispatch\_layout\_a2的输出，含义参考该部分的描述 |
| group\_ep                   | str      | 必选        | --                                                                                                                | HCCL通信域名称                               |
| rank                        | int      | 必选        | \[0, num\_ranks)                                                                                                  | 本卡在通信域中的rankID                          |
| num\_ranks                  | int      | 必选        | 当前只支持16                                                                                                           | EP通信域rank数                              |
| use\_quant                  | bool     | 必选        | False: 不开启量化, 当前版本暂不支持量化                                                                                          | Dispatch量化指示符                           |

##### 1.1.7.4 返回值

函数返回值是一个8个Tensor构成的List，分别存放：recv\_x, dynamic\_scales\_out, expand\_idx\_out, ep\_rank\_token\_cnt, offset\_inner, offset\_outer, count\_outer, expand\_scales.

| **📌参数**             | **🔧类型** | **📋取值说明**                                                                                                          | **📝描述**                           |
| -------------------- | -------- | ------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| recv\_x              | Tensor   | 形状：(recv\_token\_num, hidden\_size), 其中recv\_token\_num为本卡收到的token个数。当use\_quant为true时，数据类型为int8, false时数据类型与入参x一致。 | 当前rank上收到的token信息                  |
| dynamic\_scales\_out | Tensor   | 形状：(recv\_token\_num), 数据类型为float.当use\_quant为false时该值没有意义。                                                         | 当前rank上收到token的动态量化scale信息         |
| expand\_idx\_out     | Tensor   | 形状：(maxbs, num\_experts), 数据类型为int                                                                                  | 本卡发出的token在同一专家内的序号                |
| ep\_rank\_token\_cnt | Tensor   | 形状：(num\_experts, num\_ranks), 数据类型为int                                                                             | 每个专家从不同rank接收的token数量              |
| offset\_inner        | Tensor   | 形状：(2, max\_bs, num\_experts), 数据类型为int                                                                             | token给对应专家的偏移，仅存放当前卡对端server的同号卡信息 |
| offset\_outer        | Tensor   | 形状：(max\_bs, num\_experts), 数据类型为int                                                                                | token发送给对应server的token序号           |
| count\_outer         | Tensor   | 形状：(max\_bs), 数据类型为int                                                                                              | token发送到server的数量                  |
| expand\_scales       | Tensor   | 形状：(num\_recv\_tokens), 数据类型为float                                                                                  | 接收token时对应到topk\_weights中的权重       |

##### 1.1.7.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当前接口只支持A2环境调用。
3. 当前接口不支持并发调用。
4. 当前接口不支持入图使用。
5. 当前接口不支持共享专家。
6. 除满足上述形状约束外，其他参数取值要求：

- 需要满足：BS取值范围\[1, 4K]
- 需要满足: num\_experts取值范围(0, 256]
- 需要满足: topk取值范围(2, 16]
- 需要满足: (num\_experts % num\_ranks) == 0
- 需要满足: (num\_ranks % 8) == 0
- 需要满足: hidden\_size取值范围(0， 7168]且(hidden\_size % 32) == 0
- 需要满足: 配置全局宏HCCL\_BUFFERSIZE=4096
- 需要配置：export HCCL\_INTRA\_PCIE\_ENABLE = 1, export HCCL\_INTRA\_ROCE\_ENABLE = 0

#### 1.1.8 moe\_combine\_prefill\_a2 ▶

##### 1.1.8.1 接口原型

```python
moe_combine_prefill_a2(
    Tensor x, 
    Tensor topk_idx, 
    Tensor topk_weights, 
    Tensor src_idx, 
    Tensor send_head, 
    Tensor expand_scales, 
    Tensor offset_inner, 
    Tensor offset_outer, 
    Tensor count_outer, 
    str group_ep, 
    int rank, 
    int num_ranks)
-> output: Tensor
```

##### 1.1.8.2 接口描述

!\[moe\_combine\_prefill\_a2示意图]\(figures/moe\_combine\_prefill\_a2.png null)
A2代际Prefill阶段Combine接口，将按照topk\_idx的规则发送给对应专家的token，按照topk\_weights指定的权重收回。

##### 1.1.8.3 入参

| **📌参数**       | **🔧类型** | **✅是否必选**              | **📋取值说明**                                                 | **📝描述**                                            |
| -------------- | -------- | ---------------------- | ---------------------------------------------------------- | --------------------------------------------------- |
| x              | Tensor   | 必选                     | 形状:(recv\_token\_num, hidden\_size), 支持bf16, float16类型     | 本卡dispatch阶段收集到的token                               |
| topk\_idx      | Tensor   | 必选                     | 形状:(batch\_size, topk)， 数据类型为int64, 取值范围\[0, num\_experts) | 每个token的目标专家ID信息                                    |
| topk\_weights  | Tensor   | 必选                     | 形状:(batch\_size, topk)， 数据类型为float32                       | 每个token的topk个目标专家的权重信息                              |
| src\_idx       | Tensor   | 必选                     | 形状：(max\_bs, num\_experts), 数据类型为int                       | 对应moe\_dispatch\_prefill\_a2的出参expand\_idx\_out     |
| send\_head     | Tensor   | 必选                     | 形状：(num\_experts), 数据类型为int                                | 对应moe\_dispatch\_prefill\_a2的出参ep\_rank\_token\_cnt |
| expand\_scales | Tensor   | 必选                     | 形状：(num\_recv\_tokens), 数据类型为float                         | 对应moe\_dispatch\_prefill\_a2的出参expand\_scales       |
| offset\_inner  | Tensor   | 必选                     | 形状：(2, max\_bs, num\_experts), 数据类型为int                    | 对应moe\_dispatch\_prefill\_a2的出参offset\_inner        |
| offset\_outer  | Tensor   | 必选                     | 形状：(max\_bs, num\_experts), 数据类型为int                       | 对应moe\_dispatch\_prefill\_a2的出参offset\_outer        |
| count\_outer   | Tensor   | 形状：(max\_bs), 数据类型为int | 对应moe\_dispatch\_prefill\_a2的出参count\_outer                | <br />                                              |
| group\_ep      | str      | 必选                     | --                                                         | HCCL通信域名称                                           |
| rank           | int      | 必选                     | \[0, num\_ranks)                                           | 本卡在通信域中的rankID                                      |
| num\_ranks     | int      | 必选                     | 当前只支持16                                                    | EP通信域rank数                                          |

##### 1.1.8.4 返回值

函数返回值是一个Tensor，存放combine\_x信息。

| **📌参数**   | **🔧类型** | **📋取值说明**                              | **📝描述**           |
| ---------- | -------- | --------------------------------------- | ------------------ |
| combine\_x | Tensor   | 形状：(batch\_size, hidden\_size)。数据类型与x一致 | 当前rank上收到的token信息。 |

##### 1.1.8.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当前接口只支持A2环境调用。
3. 当前接口不支持并发调用。
4. 当前接口不支持入图使用。
5. 当前接口不支持共享专家。
6. 除满足上述形状约束外，其他参数取值要求：

- 需要满足：BS取值范围\[1, 4K]
- 需要满足: num\_experts取值范围(0, 256]
- 需要满足: topk取值范围\[2, 16]
- 需要满足: (num\_experts % num\_ranks) == 0
- 需要满足: hidden\_size取值范围(0， 7168]且(hidden\_size % 32) == 0
- 需要满足: 配置全局宏HCCL\_BUFFERSIZE=4096
- 需要配置：export HCCL\_INTRA\_PCIE\_ENABLE = 1, export HCCL\_INTRA\_ROCE\_ENABLE = 0

#### 1.1.9 fused\_deep\_moe ▶

##### 1.1.9.1 接口原型

```python
fused_deep_moe(
    Tensor x, 
    Tensor expert_ids, 
    Tensor[] gmm1_weight, 
    Tensor[] gmm1_weight_scale, 
    Tensor[] gmm2_weight, 
    Tensor[] gmm2_weight_scale, 
    Tensor expert_scales, 
    Tensor? expert_smooth_scales, \
    Tensor? x_active_mask, 
    str group_ep, 
    int ep_rank_size, 
    int ep_rank_id, 
    int moe_expert_num, 
    int shared_expert_num, 
    int shared_expert_rank_num, 
    int quant_mode, 
    int global_bs) 
-> output: Tensor[]
```

##### 1.1.9.2 接口描述

用于MoE Decode阶段的通算大融合算子，通过融合\[Dispatch + FFN(GMM1 + Swiglu + GMM2) + Combine]实现高效的模型推理和专家选择，适用于分布式训练和推理场景。

##### 1.1.9.3 入参

| **📌参数**                  | **🔧类型**  | **✅是否必选** | **📋取值说明**                                                                                                                                                            | **📝描述**                          |
| ------------------------- | --------- | --------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------- |
| x                         | Tensor    | 必选        | 形状:(batch\_size, token\_length), 支持bf16, float16类型                                                                                                                    | 本卡dispatch阶段待处理的token             |
| expert\_ids               | Tensor    | 必选        | 形状:(batch\_size, topk)， 数据类型为int32, 取值范围\[-1, num\_experts)，-1用于占位使用，一个token不允许重复发给同一个专家                                                                              | 每个token的目标专家ID信息                  |
| gmm1\_weight              | Tensor\[] | 必选        | 耦合模式下，只有一个Tensor, 形状:(localExpertNum, token\_length, gmm1\_hidden\_size); 分离模式下，包含localExpertNum个Tensor, 每个Tensor形状：（token\_length, gmm1\_hidden\_size），数据类型为int8     | GMM1的权重矩阵列表，支持耦合模式和分离模式           |
| gmm1\_weight\_scale       | Tensor\[] | 必选        | 耦合模式下，只有一个Tensor, 形状:(localExpertNum, gmm1\_hidden\_size); 分离模式下，包含localExpertNum个Tensor, 每个Tensor形状：（gmm1\_hidden\_size），数据类型为float32                                | GMM1的权重矩阵量化时使用的缩放系数列表，支持耦合模式和分离模式 |
| gmm2\_weight              | Tensor\[] | 必选        | 耦合模式下，只有一个Tensor, 形状:(localExpertNum, gmm1\_hidden\_size/2, token\_length); 分离模式下，包含localExpertNum个Tensor, 每个Tensor形状：（gmm1\_hidden\_size/2, token\_length），数据类型为int8 | GMM2的权重矩阵列表，支持耦合模式和分离模式           |
| gmm2\_weight\_scale       | Tensor\[] | 必选        | 耦合模式下，只有一个Tensor, 形状:(localExpertNum, token\_length); 分离模式下，包含localExpertNum个Tensor, 每个Tensor形状：（token\_length），数据类型为float32                                          | GMM2的权重矩阵量化时使用的缩放系数列表，支持耦合模式和分离模式 |
| expert\_scales            | Tensor    | 必选        | 形状：(batch\_size, topk), 数据类型为float32                                                                                                                                  | 每个专家的权重，combine阶段使用               |
| expert\_smooth\_scales    | Tensor    | 可选        | --                                                                                                                                                                    | 预留参数，暂不参与计算                       |
| x\_active\_mask           | Tensor    | 可选        | 形状： (batch\_size)，数据类型bool，取值范围\[true, false]，true值一定要在false之前                                                                                                        | 预留参数，传入None                       |
| group\_ep                 | str       | 必选        | 字符串长度范围：(0, 128), 且需要保证是有效的通信域名称                                                                                                                                      | HCCL通信域名称                         |
| ep\_rank\_size            | int       | 必选        | 需要满足：(ep\_rank\_size \* MoeExpertNumPerRank) ≤ 512且ep\_rank\_size > 0                                                                                                 | EP通信域大小                           |
| ep\_rank\_id              | int       | 必选        | \[0, ep\_rank\_size)                                                                                                                                                  | 本卡在通信域中的rankID                    |
| moe\_expert\_num          | int       | 必选        | 需要满足：(ep\_rank\_size - shared\_expert\_rank\_num) % moe\_expert\_num == 0                                                                                             | MOE专家数量                           |
| shared\_expert\_num       | int       | 必选        | 当前仅支持传1                                                                                                                                                               | 共享专家数量                            |
| shared\_expert\_rank\_num | int       | 必选        | 需要满足：(ep\_rank\_size - shared\_expert\_rank\_num) % moe\_expert\_num == 0且shared\_expert\_rank\_num < ep\_rank\_size                                                  | 共享专家卡数量                           |
| quant\_mode               | int       | 必选        | 预留入参，当前只支持传0                                                                                                                                                          | 量化模式                              |
| global\_bs                | int       | 必选        | 若所有卡的token数量一致，可以传入0或者batch\_size \* ep\_rank\_size; 若所有卡的token数量不一致，需要传入max\_batch\_size \* ep\_rank\_size                                                           | 所有卡的最大token总数                     |

##### 1.1.9.4 返回值

函数返回值是一个Tensor列表，存放combine\_x和expert\_token\_nums信息。

| **📌参数**            | **🔧类型** | **📋取值说明**                               | **📝描述**                  |
| ------------------- | -------- | ---------------------------------------- | ------------------------- |
| combine\_x          | Tensor   | 形状：(batch\_size, token\_length)。数据类型与x一致 | 当前rank上token经各个专家处理后汇聚的结果 |
| expert\_token\_nums | Tensor   | 形状：(local\_expert\_num)。数据类型为int64       | 本卡各个专家收到的token数量          |

##### 1.1.9.5 约束和注意事项 ⚠️

1. 入参形状需严格满足上述入参描述中的形状定义。
2. 当前接口只支持A3环境调用。
3. 当前接口不支持并发调用。
4. 当前接口图模式只支持AclGraph模式。
5. 当前接口不支持共享专家。
6. Batch\_size小于16时非目标场景，其性能相对于小算子拼接可能劣化，建议性能对比后决策使用。
7. 除满足上述形状约束外，其他参数取值要求：

- 需要满足：BS取值范围\[0, 256]
- 需要满足: token\_length取值范围\[1024， 7168]且(token\_length % 256) == 0
- 需要满足: gmm1\_hiden\_size取值范围\[1024， 6144]且(gmm1\_hiden\_size % 256) == 0
- 需要满足: topk取值范围\[0, 12]且应保证小于等于专家数
- 需要满足：global\_bs ≥ 0 且保证（global\_bs % ep\_rank\_size） == 0
- 需要满足: 路由专家卡需满足local\_expert\_num ≤ (aivnum / 2)，其中aivnum为硬件aiv核心数
- 需要满足: gmm1\_weight, gmm1\_weight\_scale, gmm2\_weight, gmm2\_weight\_scale四个入参的模式必须统一，不能一部分耦合模式一部分分离模式
- 需要满足: HCCL\_BUFFERSIZE环境变量配置应不小于\[(ep\_rank\_size \* max\_batch\_size \* moe\_expert\_num\_per\_rank \* total\_length \* sizeof(x) \* 2) / 1024 / 1024]向上取整

