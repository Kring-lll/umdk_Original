# CAM API Guide

## Introduction

**CAM**  is short for  **C**ommunication  **A**cceleration for  **M**atrix on Ascend NPU. CAM provides EP (Expert Parallelism) communication kernels, high performance KVCache transfer for PD disaggregation and KVC pooling, AFD communication kernels, RL weights transfer and so on. CAM is easily to be run in single kernel mode or integrated into vllm or SGLang framework.

## CAM Structure

（To be done）

## CAM API

### 1. EP Communication Kernels

UMDK provides high-performance Python interfaces for communication and fused computation-communication via "umdk\_cam\_op\_lib". Users can use this lib in popular Ascend inference framework, such as vllm-ascend, sglang-kernel-npu, etc.

#### 1.1 Interfaces of Dispatch & Combine

#### 1.1.1 moe\_dispatch\_shmem ▶

##### 1.1.1.1 Prototype

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

##### 1.1.1.2 Interface Description

Dispatch interface based on SHMEM, which is used for token dispatch to different experts in EP communication phase. This interface should be used in conjunction with "moe\_combine\_shmem".

##### 1.1.1.3 Input Parameters

| **📌Parameter**           | **🔧Type** | **✅Required/Optional** | **📋Value Range**                                                                                                           | **📝Details**                                             |
| ------------------------- | ---------- | ---------------------- | --------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------- |
| x                         | Tensor     | Required               | Shape: (batch\_size, hidden\_size)                                                                                          | Input token                                               |
| expert\_ids               | Tensor     | Required               | Shape: (batch\_size, top\_k)                                                                                                | Destination expert ID                                     |
| scales                    | Tensor     | Optional               | Float type，Shape: (m+1,h) when shared expert exists, (m, h) when there is no shared expert, m is the shared expert number.  | Quant parameters                                          |
| x\_active\_mask           | Tensor     | Optional               | Not supported，set to None                                                                                                   | --                                                        |
| ep\_world\_size           | int        | Required               | Supported values：\[8, 16, 32, 64, 128, 144, 256, 288]                                                                       | Rank size in EP Communicator                              |
| ep\_rank\_id              | int        | Required               | \[0, ep\_world\_size-1]                                                                                                     | Rank ID in EP communicator                                |
| moe\_expert\_num          | int        | Required               | \[1, 512]                                                                                                                   | MoE expert number                                         |
| tp\_world\_size           | int        | Required               | Not supported，set to 0                                                                                                      | --                                                        |
| tp\_rank\_id              | int        | Required               | Not supported，set to 0                                                                                                      | --                                                        |
| expert\_shard\_type       | int        | Required               | Not supported，set to 0                                                                                                      | --                                                        |
| shared\_expert\_num       | int        | Required               | Only support 1, set to 1                                                                                                    | Shared expert number                                      |
| shared\_expert\_rank\_num | int        | Required               | \[0, ep\_world\_size-1]                                                                                                     | Shared expert rank number                                 |
| quant\_mode               | int        | Required               | set to 0 when no quant，set to 2 when quant                                                                                  | Quant mode                                                |
| global\_bs                | int        | Required               | Value constrains by the total Memory buffer size.                                                                           | Global BS value upper bound in the EP communicator        |
| expert\_token\_nums\_type | int        | Required               | 0: Output is the token processing number of each expert；1：Output is the prefix sum of each expert's token processing number | expert\_token\_nums\_out data format indicator            |
| ext\_info                 | int        | Required               | --                                                                                                                          | Basic address pointer return value after SHMEM initiation |

##### 1.1.1.4 Return Values

Output is a List of Tensor, which stores the following value sequencially: expand\_x, dynamic\_scales, expand\_idx, expert\_token\_nums, ep\_send\_count, tp\_send\_count and expand\_scales.

| **📌Parameter**     | **🔧Type** | **📋Value Range**                                                                                                                                                                                                                                    | **📝Details**                                                              |
| ------------------- | ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| expand\_x           | Tensor     | Shape: (rank\_size \* batch\_size / shared\_expert\_num, hidden\_size) when the current rank is shared expert; Shape：(expert\_num\_per\_rank \* rank\_size \* batch\_size, hidden\_size) when the current rank is MoE expert                         | All expert tokens in the rank                                              |
| dynamic\_scales     | Tensor     | Shape is the same as the first dimension of expand\_x, which is: (rank\_size \* batch\_size / shared\_expert\_num) when the current rank is shared expert; (expert\_num\_per\_rank \* rank\_size \* batch\_size) when the current rank is MoE expert | Quant parameters                                                           |
| expand\_idx         | Tensor     | Shape：(batch\_size, top\_k)                                                                                                                                                                                                                          | In target expert, the send sequence ID when only consider the current rank |
| expert\_token\_nums | Tensor     | (expert\_num\_on\_rank)                                                                                                                                                                                                                              | Token receive number of each expert in this rank                           |
| ep\_send\_count     | Tensor     | Shape：(expert\_num\_per\_rank \* ep\_world\_size)                                                                                                                                                                                                    | Token receive number of each expert from each rank                         |
| tp\_send\_count     | Tensor     | --                                                                                                                                                                                                                                                   | Not support                                                                |
| expand\_scales      | Tensor     | --                                                                                                                                                                                                                                                   | Not support                                                                |

##### 1.1.1.5 Constraints and Precautions⚠️

1. Input Shape should satisfy the shape definition above.
2. expand\_x data type is int8 when quan mode is on; expand\_x data type is bfloat16 when quan mode is off.
3. Current interface do not support A2.
4. Current interface do not support concurrent usage.
5. Do not support dynamic graph when in GE mode; Do not support fullgraph=true.
6. Data type of x do not support bfloat16.
7. Current version does not support shared expert function.
8. Other Constraits need to be satisfy:

- top\_k supports 8 only.
- Required: (moe\_expert\_num + shared\_expert\_rank\_num) ≤ CAM\_MAX\_EXPERT\_NUM, where CAM\_MAX\_EXPERT\_NUM is 512 currently.
- Required: moe\_expert\_num % (ep\_world\_size - shared\_expert\_rank\_num) == 0
- Required：moe\_expert\_num / (ep\_world\_size - shared\_expert\_rank\_num) ≤ MAX\_EXPERT\_PER\_RANK, where MAX\_EXPERT\_PER\_RANK is 32 currently.
- Required： if shared\_expert\_rank\_num is not 0，ep\_world\_size % shared\_expert\_rank\_num == 0，and ep\_world\_size ≠ shared\_expert\_rank\_num.
- Required：(batch\_size \* hidden\_size \* ep\_world\_size \* expert\_num\_per\_rank \* 2) ≤ the space allocated by SHMEM, which is pointed by ext\_info.

#### 1.1.2 moe\_combine\_shmem ▶

##### 1.1.2.1 Prototype

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

##### 1.1.2.2 Inrterface Description

Combine interface based on SHMEM, which is used for token combine from different experts in EP communication phase. This interface should be used in conjunction with "moe\_dispatch\_shmem".

##### 1.1.2.3 Input Parameters

| **📌Parameter**           | **🔧Type** | **✅Required/Optional** | **📋Value Range**                                    | **📝Details**                                               |
| ------------------------- | ---------- | ---------------------- | ---------------------------------------------------- | ----------------------------------------------------------- |
| expand\_x                 | Tensor     | Required               | Shape: same as expand\_x from dispatch output        | Token to each expert from dispatch                          |
| expert\_ids               | Tensor     | Required               | Shape:(batch\_size, top\_k)                          | Target expert ID                                            |
| expand\_idx               | Tensor     | Required               | Shape:(batch\_size, top\_k)                          | In target expert, token sequential ID based on current rank |
| ep\_send\_counts          | Tensor     | Required               | Shape: (expert\_num\_per\_rank \* ep\_world\_size)   | Token number from each expert in each rank                  |
| expert\_scales            | Tensor     | Required               | Shape：（batch\_size, top\_k）                          | Weights when combine token from each expert                 |
| tp\_send\_count           | Tensor     | Optional               | Not support，set to tensor\[0] as int8 type           | --                                                          |
| x\_active\_mask           | Tensor     | Optional               | Not support，set to None                              | --                                                          |
| activation\_scale         | Tensor     | Optional               | Not support，set to None                              | --                                                          |
| weight\_scale             | Tensor     | Optional               | Not support，set to None                              | --                                                          |
| group\_list               | Tensor     | Optional               | Not support，set to None                              | --                                                          |
| expand\_scales            | Tensor     | Optional               | Not support，set to None                              | --                                                          |
| ep\_world\_size           | int        | Required               | Supported Value：\[8, 16, 32, 64, 128, 144, 256, 288] | Rank number in EP communicator                              |
| ep\_rank\_id              | int        | Required               | \[0, ep\_world\_size-1]                              | rank ID in EP communicator                                  |
| moe\_expert\_num          | int        | Required               | \[1, 512]                                            | MoE expert number                                           |
| tp\_world\_size           | int        | Required               | Not support，set to 1                                 | --                                                          |
| tp\_rank\_id              | int        | Required               | Not support，set to 0                                 | --                                                          |
| expert\_shard\_type       | int        | Required               | Not support，set to 0                                 | --                                                          |
| shared\_expert\_num       | int        | Required               | Set to 1                                             | Shared expert number                                        |
| shared\_expert\_rank\_num | int        | Required               | \[0, ep\_world\_size-1]                              | Shared expert rank ID                                       |
| global\_bs                | int        | Required               | Value constrains by the total Memory buffer size.    | Global BS value upper bound in the EP communicator          |
| out\_dtype                | int        | Required               | Not support，set to 0                                 | --                                                          |
| comm\_quant\_mode         | int        | Required               | Set to 0 when no quant, set to 2 when quant          | Quant mode                                                  |
| group\_list\_type         | int        | Required               | Not support，set to 0                                 | --                                                          |
| ext\_info                 | int        | Required               | --                                                   | Basic address pointer return value after SHMEM initiation   |

##### 1.1.2.4 Return Values

Output is a tensor，which stores expand\_x。

| **📌Parameter** | **🔧Type** | **📋Value Range**                 | **📝Details**                         |
| --------------- | ---------- | --------------------------------- | ------------------------------------- |
| expand\_x       | Tensor     | Shape:(batch\_size, hidden\_size) | token combined from different experts |

##### 1.1.2.5 Constraints and Precautions⚠️

1. Input Shape should satisfy the shape definition above.
2. Current interface do not support A2.
3. Current interface do not support concurrent usage.
4. Do not support dynamic graph when in GE mode; Do not support fullgraph=true.
5. Current version does not support shared expert function.
6. Other Constraits need to be satisfy:

- top\_k supports 8 only.
- Required: (moe\_expert\_num + shared\_expert\_rank\_num) ≤ CAM\_MAX\_EXPERT\_NUM, where CAM\_MAX\_EXPERT\_NUM is 512 currently.
- Required: moe\_expert\_num % (ep\_world\_size - shared\_expert\_rank\_num) == 0
- Required：moe\_expert\_num / (ep\_world\_size - shared\_expert\_rank\_num) ≤ MAX\_EXPERT\_PER\_RANK, where MAX\_EXPERT\_PER\_RANK is 32 currently.
- Required： if shared\_expert\_rank\_num is not 0，ep\_world\_size % shared\_expert\_rank\_num == 0，and ep\_world\_size ≠ shared\_expert\_rank\_num.
- Required：(batch\_size \* hidden\_size \* ep\_world\_size \* expert\_num\_per\_rank \* 2) ≤ the space allocated by SHMEM, which is pointed by ext\_info.

#### 1.1.3 get\_dispatch\_layout ▶

##### 1.1.3.1 Prototype

```python
get_dispatch_layout(
    Tensor topk_idx, 
    int num_experts, 
    int num_ranks)
-> output: tuple(Tensor, Tensor)
```

##### 1.1.3.2 Interface Description

!\[get\_dispatch\_layout diagram]\(figures/get\_dispatch\_layout\_a3.png null)
Interface used before dispatch in prefill phase for A3, which copies the current tokens in this rank TopK times and rearranges these tokens in experts' granularity. This interface should used with moe\_dispatch\_prefill and moe\_combine\_prefill.

##### 1.1.3.3 Input Parameters

| **📌Parameter** | **🔧Type** | **✅Required/Optional** | **📋Value Range**                     | **📝Details**                         |
| --------------- | ---------- | ---------------------- | ------------------------------------- | ------------------------------------- |
| topk\_idx       | Tensor     | Required               | Shape:(batch\_size, topk)， int64 type | ID info for target experts            |
| num\_experts    | int        | Required               | Range：(0, 512]                        | MOE experts number                    |
| num\_ranks      | int        | Required               | Range：\[1, 384]                       | rank number in EP communication group |

##### 1.1.3.4 Return Value

Return value of this interface is a tuple made of 2 tensor, which stores number\_tokens\_per\_expert and send\_token\_idx respectively.

| **📌Parameter**             | **🔧Type** | **📋Value Range**           | **📝Details**                                                                  |
| --------------------------- | ---------- | --------------------------- | ------------------------------------------------------------------------------ |
| number\_tokens\_per\_expert | Tensor     | Shape：（num\_experts）        | token value sent to each expert in this rank                                   |
| send\_token\_idx            | Tensor     | Shape：(batch\_size, top\_k) | The position offset of each token after re-arrangement in experts' perspective |

##### 1.1.3.5 Constraints and Precautions ⚠️

1. Input Shape should satisfy the shape definition above.
2. Current interface supports A3 only.
3. Current interface do not support concurrent usage.
4. Do not support dynamic graph when in GE mode; Do not support fullgraph=true.
5. Other Constraits need to be satisfy:

- top\_k value range: (0, 16].
- Required: num\_experts % num\_ranks == 0

#### 1.1.4 moe\_dispatch\_prefill ▶

##### 1.1.4.1 Prototype

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

##### 1.1.4.2 Interface Description

!\[moe\_dispatch\_prefill diagram]\(figures/moe\_dispatch\_prefill\_a3.png null)
Dispatch interface in prefill phase for A3, which sends the tokens to the target experts in the rules of topk\_idx.

##### 1.1.4.3 Input Parameters

| **📌Parameter**             | **🔧Type** | **✅Required/Optional** | **📋Value Range**                                           | **📝Details**                                                                  |
| --------------------------- | ---------- | ---------------------- | ----------------------------------------------------------- | ------------------------------------------------------------------------------ |
| x                           | Tensor     | Required               | Shape:(batch\_size, hidden\_size), support bf16 and float16 | token sent from current rank                                                   |
| topk\_idx                   | Tensor     | Required               | Shape:(batch\_size, topk)， int64 type                       | target ID of each token                                                        |
| topk\_weights               | Tensor     | Required               | Shape:(batch\_size, topk)， float32 type                     | weights of target experts for each token                                       |
| number\_tokens\_per\_expert | Tensor     | Required               | Shape：（num\_experts），int type                               | token number sent to each expert in current rank                               |
| send\_token\_idx\_small     | Tensor     | Required               | Shape：(batch\_size, top\_k), int type                       | The position offset of each token after re-arrangement in experts' perspective |
| group\_ep                   | str        | Required               | --                                                          | name of HCCL communication group                                               |
| rank                        | int        | Required               | \[0, num\_ranks)                                            | rank ID in communication group                                                 |
| num\_ranks                  | int        | Required               | \[2, 384]                                                   | rank number of EP group                                                        |
| use\_quant                  | bool       | Required               | True: use quant； False: do not use quant                    | Dispatch quant indicator                                                       |

##### 1.1.4.4 Return Value

Return value is a tuple made of 5 tensors，which stores：recv\_x, dynamic\_scales\_out, expand\_idx\_out, recv\_count, recv\_token\_per\_expert.

| **📌Parameter**           | **🔧Type** | **📋Value Range**                                                                                                                                                                                              | **📝Details**                                                                                                                                                                                                                      |
| ------------------------- | ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| recv\_x                   | Tensor     | Shape：(recv\_token\_num, hidden\_size), recv\_token\_num is the token number received by this rank. When use\_quant is true, data type is int8, and data type is the same as input x when use\_quant is false. | token received in current rank                                                                                                                                                                                                     |
| dynamic\_scales\_out      | Tensor     | Shape：(recv\_token\_num), float type. This value has no meanings when use\_quant is false.                                                                                                                     | dynamic quant scale infos for received tokens in current rank                                                                                                                                                                      |
| expand\_idx\_out          | Tensor     | Shape：(recv\_token\_num \* 3), int type                                                                                                                                                                        | info triplet of token received by this rank, the three numbers of each triplet is: source rank, index of token in source rank(from BS's perspective), token offset after the re-arrangement in source rank in experts' perspective |
| recv\_count               | Tensor     | Shape：(num\_experts), int type                                                                                                                                                                                 | prefix-sum number of token received in this rank from each other ranks                                                                                                                                                             |
| recv\_tokens\_per\_expert | Tensor     | Shape：(local\_expert\_num), int64 type                                                                                                                                                                         | token received by each expert in this rank                                                                                                                                                                                         |

##### 1.1.4.5 Constraints and Precautions ⚠️

1. Input Shape should satisfy the shape definition above.
2. Current interface supports A3 only.
3. Current interface do not support concurrent usage.
4. Do not support dynamic graph when in GE mode; Do not support fullgraph=true.
5. Other Constraits need to be satisfy:

- top\_k value range: (0, 16].
- BS value range: \[1, 8K]
- num\_ranks range: \[2, 384]
- num\_experts range: (0, 512]
- required: (num\_experts % num\_ranks) == 0
- required: set HCCL\_BUFFERSIZE = 4096

#### 1.1.5 moe\_combine\_prefill ▶

##### 1.1.5.1 Prototype

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

##### 1.1.5.2 Interface Description

!\[moe\_combine\_prefill diagram]\(figures/moe\_combine\_prefill\_a3.png null)
combine interface in prefill phase for A3, which combines the token sent to each expert in topk\_idx rules with weights given by topk\_weights.

##### 1.1.5.3 Input Parameters

| **📌Parameter** | **🔧Type** | **✅Required/Optional**                  | **📋Value Range**                                                                                                                                                                                                                                                                                             | **📝Details**                                                                                                                                |
| --------------- | ---------- | --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| x               | Tensor     | Required                                | Shape:(recv\_token\_num, hidden\_size), support bf16 and float16 types                                                                                                                                                                                                                                        | token received of this rank in dispatch phase                                                                                                |
| topk\_idx       | Tensor     | Required                                | Shape:(batch\_size, topk)， int64 phase                                                                                                                                                                                                                                                                        | target experts info for each token                                                                                                           |
| topk\_weights   | Tensor     | Required                                | Shape:(batch\_size, topk)， float32 type                                                                                                                                                                                                                                                                       | weights of topk experts for each token                                                                                                       |
| src\_idx        | Tensor     | Shape：(recv\_token\_num \* 3), int type | info triplet of token received by this rank, the three numbers of each triplet is: source rank, index of token in source rank(from BS's perspective), token offset after the re-arrangement in source rank in experts' perspective. Corresponds to return value "expand\_idx\_out" of moe\_dispatch\_prefill. | <br />                                                                                                                                       |
| <br />          | send\_head | Tensor                                  | Shape：(num\_experts), int type                                                                                                                                                                                                                                                                                | prefix-sum number of token received in this rank from each other ranks. Corresponds to return value "recv\_count" of moe\_dispatch\_prefill. |
| group\_ep       | str        | Required                                | --                                                                                                                                                                                                                                                                                                            | name of HCCL communication group                                                                                                             |
| rank            | int        | Required                                | \[0, num\_ranks)                                                                                                                                                                                                                                                                                              | rank ID of current rank in EP group                                                                                                          |
| num\_ranks      | int        | Required                                | \[2, 384]                                                                                                                                                                                                                                                                                                     | rank number of EP group                                                                                                                      |

##### 1.1.5.4 Return Value

Return value is a tensor，which stores combine\_x。

| **📌Parameter** | **🔧Type** | **📋Value Range**                                             | **📝Details**                  |
| --------------- | ---------- | ------------------------------------------------------------- | ------------------------------ |
| combine\_x      | Tensor     | Shape：(batch\_size, hidden\_size), data type is the same as x | token received in current rank |

##### 1.1.5.5 Constraints and Precautions ⚠️

1. Input Shape should satisfy the shape definition above.
2. Current interface supports A3 only.
3. Current interface do not support concurrent usage.
4. Do not support dynamic graph when in GE mode; Do not support fullgraph=true.
5. Other Constraits need to be satisfy:

- top\_k value range: (0, 16].
- BS value range: \[1, 8K]
- num\_ranks range: \[2, 384]
- num\_experts range: (0, 512]
- required: (num\_experts % num\_ranks) == 0
- required: set HCCL\_BUFFERSIZE = 4096

#### 1.1.6 get\_dispatch\_layout\_a2 ▶

##### 1.1.6.1 Prototype

```python
get_dispatch_layout_a2(
    Tensor topk_idx, 
    int num_experts, 
    int num_ranks)
-> output: tuple(Tensor, Tensor)
```

##### 1.1.6.2 Interface Description

!\[get\_dispatch\_layout\_a2 diagram]\(figures/get\_dispatch\_layout\_a2.png null)
Interface used befor dispatch in prefill phase for A2, which copies the current tokens in this rank TopK times((TopK + 1) times if shared experts exist) and rearranges these tokens in different granularity. This interface should used with moe\_dispatch\_prefill\_a2 and moe\_combine\_prefill\_a2.

##### 1.1.6.3 Input Parameters

| **📌Parameter** | **🔧Type** | **✅Required/Optional** | **📋Value Range**                                              | **📝Details**                          |
| --------------- | ---------- | ---------------------- | -------------------------------------------------------------- | -------------------------------------- |
| topk\_idx       | Tensor     | Required               | Shape:(batch\_size, topk)， int64 type，Range：\[0, num\_experts) | ID for target experts                  |
| num\_experts    | int        | Required               | Range：(0, 256]                                                 | MOE expert numbers                     |
| num\_ranks      | int        | Required               | Support 16 only                                                | rank number for EP communication group |

##### 1.1.6.4 Return Value

Return value is a tuple made of 2 tensors, which stores number\_tokens\_per\_expert and notify\_send\_data.

| **📌Parameter**             | **🔧Type** | **📋Value Range**                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | **📝Details**                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| --------------------------- | ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| number\_tokens\_per\_expert | Tensor     | Shape：(num\_experts), int type                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | token number to each expert from current rank                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| notify\_send\_data          | Tensor     | Shape：(num\_experts \* EXPERT\_DATA\_SIZE + server\_num + max\_bs \* (1 + 2\* server\_num + num\_experts)), int type. Set EXPERT\_DATA\_SIZE=4097，max\_bs=4096。Shape info for seven parts： 1. num\_tokens\_per\_expert, Shape：（num\_experts）； 2. num\_token\_per\_server\_uniq, Shape：（num\_experts）； 3. num\_each\_token\_to\_server, Shape：（max\_bs \* num\_server）; 4. each\_token\_to\_num\_server, Shape：（max\_bs）; 5. each\_token\_offset\_to\_server, Shape：（max\_bs \* num\_server）； 6. send\_token\_idx, Shape：（max\_bs \* num\_experts）； 7. expert\_rank\_token\_idx, Shape：（num\_experts， max\_bs）； | tensor made of 7 different parts: 1. token number each expert receives from current rank； 2. token number each server receives from current rank(deduplication)； 3. number each token sent from this rank to each server； 4. server number each token sent from this rank； 5. token offset each token sent to each server in this rank 6. token offset each token sent to each expert in this rank 7. the "each\_token\_offset\_to\_server" value of each token received from each expert |

##### 1.1.6.5 Constraints and Precautions ⚠️

1. Input Shape should satisfy the shape definition above.
2. Current interface supports A2 only.
3. Current interface do not support concurrent usage.
4. Do not support dynamic graph when in GE mode; Do not support fullgraph=true.
5. Do not support shared experts.
6. Other Constraits need to be satisfy:

- top\_k range：(2， 16]
- Required: num\_experts % num\_ranks == 0
- Required: num\_ranks % 8 == 0
- Required：export HCCL\_INTRA\_PCIE\_ENABLE = 1, export HCCL\_INTRA\_ROCE\_ENABLE = 0

#### 1.1.7 moe\_dispatch\_prefill\_a2 ▶

##### 1.1.7.1 Prototype

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

##### 1.1.7.2 Interface Description

!\[moe\_dispatch\_prefill\_a2 diagram]\(figures/moe\_dispatch\_prefill\_a2.png null)
Dispatch interface in prefill phase for A2, which send token to target experts in topk\_idx rules.

##### 1.1.7.3 Input Parameters

| **📌Parameter**             | **🔧Type** | **✅Required/Optional** | **📋Value Range**                                                                                                    | **📝Details**                                                       |
| --------------------------- | ---------- | ---------------------- | -------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| x                           | Tensor     | Required               | Shape:(batch\_size, hidden\_size), support bf16 and float16 type                                                     | token sent from this rank                                           |
| topk\_idx                   | Tensor     | Required               | Shape:(batch\_size, topk)， int64 type，range: \[0, num\_experts)                                                      | target expert IDs for each token                                    |
| topk\_weights               | Tensor     | Required               | Shape:(batch\_size, topk)， float32 type                                                                              | topk weights of target experts for each token                       |
| number\_tokens\_per\_expert | Tensor     | Required               | Shape：（num\_experts），int type                                                                                        | token numbers to each expert in current rank                        |
| notify\_send\_data          | Tensor     | Required               | Shape：(num\_experts \* EXPERT\_DATA\_SIZE + server\_num + max\_bs \* (1 + 2\* server\_num + num\_experts)), int type | output of get\_dispatch\_layout\_a2，refer to the descriptions above |
| group\_ep                   | str        | Required               | --                                                                                                                   | name of HCCL communication group                                    |
| rank                        | int        | Required               | \[0, num\_ranks)                                                                                                     | rank ID of this rank in EP group                                    |
| num\_ranks                  | int        | Required               | support 16 only                                                                                                      | rank number in EP group                                             |
| use\_quant                  | bool       | Required               | False: no quant, quant is not supported in current version                                                           | Dispatch quant indicator                                            |

##### 1.1.7.4 Return Value

Return value is a list made of 8 tensors, which stores: recv\_x, dynamic\_scales\_out, expand\_idx\_out, ep\_rank\_token\_cnt, offset\_inner, offset\_outer, count\_outer, expand\_scales.

| **📌Parameter**      | **🔧Type** | **📋Value Range**                                                                                                                                                                                    | **📝Details**                                                                |
| -------------------- | ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| recv\_x              | Tensor     | Shape：(recv\_token\_num, hidden\_size), where recv\_token\_num is the number received by this rank. Data type is int8 when use\_quant is true; type is the same as input x when use\_quant is false. | token received in this rank                                                  |
| dynamic\_scales\_out | Tensor     | Shape：(recv\_token\_num), float type. This value has no meanings when use\_quant is false.                                                                                                           | dynamic quant scales of the token received in this rank                      |
| expand\_idx\_out     | Tensor     | Shape：(maxbs, num\_experts), int type                                                                                                                                                                | token offset after the re-arrangement in source rank in experts' perspective |
| ep\_rank\_token\_cnt | Tensor     | Shape：(num\_experts, num\_ranks), int type                                                                                                                                                           | token number received of each expert from different ranks                    |
| offset\_inner        | Tensor     | Shape：(2, max\_bs, num\_experts), int type                                                                                                                                                           | token offset of this server from global experts' perspective                 |
| offset\_outer        | Tensor     | Shape：(max\_bs, num\_experts), int type                                                                                                                                                              | token offset to the target server                                            |
| count\_outer         | Tensor     | Shape：(max\_bs), int type                                                                                                                                                                            | token number to the target server                                            |
| expand\_scales       | Tensor     | Shape：(num\_recv\_tokens), float type                                                                                                                                                                | weights corresponding to topk\_weights when receives token                   |

##### 1.1.7.5 Constraints and Precautions ⚠️

1. Input Shape should satisfy the shape definition above.
2. Current interface supports A2 only.
3. Current interface do not support concurrent usage.
4. Do not support dynamic graph when in GE mode; Do not support fullgraph=true.
5. Do not support shared experts.
6. Other Constraits need to be satisfy:

- top\_k range：(2， 16]
- BS range：\[1，4k]
- num\_experts range：(0， 256]
- Required: num\_experts % num\_ranks == 0
- Required: num\_ranks % 8 == 0
- Required: hidden\_size range: (0, 7168] and (hidden\_size % 32) == 0
- Required：export HCCL\_BUFFERSIZE=4096
- Required：export HCCL\_INTRA\_PCIE\_ENABLE = 1, export HCCL\_INTRA\_ROCE\_ENABLE = 0

#### 1.1.8 moe\_combine\_prefill\_a2 ▶

##### 1.1.8.1 Prototype

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

##### 1.1.8.2 Interface Description

!\[moe\_combine\_prefill\_a2 diagram]\(figures/moe\_combine\_prefill\_a2.png null)
combine interface in prefill phase for A2, which combines the token from the topk experts in weights of topk\_weights.

##### 1.1.8.3 Input Parameters

| **📌Parameter** | **🔧Type** | **✅Required/Optional**    | **📋Value Range**                                                          | **📝Details**                                                                      |
| --------------- | ---------- | ------------------------- | -------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| x               | Tensor     | Required                  | Shape:(recv\_token\_num, hidden\_size), support bf16 and float16 type      | token received in dispatch phase of this rank                                      |
| topk\_idx       | Tensor     | Required                  | Shape:(batch\_size, topk)， int64 type, range: \[0, num\_experts)           | target expert ID of each token                                                     |
| topk\_weights   | Tensor     | Required                  | Shape:(batch\_size, topk)， float32 type                                    | weights of topK target expert for each token                                       |
| src\_idx        | Tensor     | Required                  | Shape：(max\_bs, num\_experts), int type                                    | corresponding to the output "expand\_idx\_out" of "moe\_dispatch\_prefill\_a2"     |
| send\_head      | Tensor     | Required                  | Shape：(num\_experts), int type                                             | corresponding to the output "ep\_rank\_token\_cnt" of "moe\_dispatch\_prefill\_a2" |
| expand\_scales  | Tensor     | Required                  | Shape：(num\_recv\_tokens), float type                                      | corresponding to the output "expand\_scales" of "moe\_dispatch\_prefill\_a2"       |
| offset\_inner   | Tensor     | Required                  | Shape：(2, max\_bs, num\_experts), int type                                 | corresponding to the output "offset\_inner" of "moe\_dispatch\_prefill\_a2"        |
| offset\_outer   | Tensor     | Required                  | Shape：(max\_bs, num\_experts), int type                                    | corresponding to the output "offset\_outer" of "moe\_dispatch\_prefill\_a2"        |
| count\_outer    | Tensor     | Shape：(max\_bs), int type | corresponding to the output "count\_outer" of "moe\_dispatch\_prefill\_a2" | <br />                                                                             |
| group\_ep       | str        | Required                  | --                                                                         | HCCL communication group name                                                      |
| rank            | int        | Required                  | \[0, num\_ranks)                                                           | rank ID in EP group                                                                |
| num\_ranks      | int        | Required                  | support 16 only                                                            | rank number in EP group                                                            |

##### 1.1.8.4 Return Value

Return value is a tensor，which stores combine\_x.

| **📌Parameter** | **🔧Type** | **📋Value Range**                                           | **📝Details**                  |
| --------------- | ---------- | ----------------------------------------------------------- | ------------------------------ |
| combine\_x      | Tensor     | Shape：(batch\_size, hidden\_size), the same type as input x | token received in current rank |

##### 1.1.8.5 Constraints and Precautions ⚠️

1. Input Shape should satisfy the shape definition above.
2. Current interface supports A2 only.
3. Current interface do not support concurrent usage.
4. Do not support dynamic graph when in GE mode; Do not support fullgraph=true.
5. Do not support shared experts.
6. Other Constraits need to be satisfy:

- top\_k range：(2， 16]
- BS range：\[1，4k]
- num\_experts range：(0， 256]
- Required: num\_experts % num\_ranks == 0
- Required: num\_ranks % 8 == 0
- Required: hidden\_size range: (0, 7168] and (hidden\_size % 32) == 0
- Required：export HCCL\_BUFFERSIZE=4096
- Required：export HCCL\_INTRA\_PCIE\_ENABLE = 1, export HCCL\_INTRA\_ROCE\_ENABLE = 0

#### 1.1.9 fused\_deep\_moe ▶

##### 1.1.9.1 Prototype

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

##### 1.1.9.2 Interface Description

Fused computation-communication operator in MoE Decode phase for A3, which merges \[Dispatch + FFN(GMM1 + Swiglu + GMM2) + Combine] into an operator for better inference and training performance.

##### 1.1.9.3 Input Parameters

| **📌Parameter**           | **🔧Type** | **✅Required/Optional** | **📋Value Range**                                                                                                                                                                                                         | **📝Details**                                                      |
| ------------------------- | ---------- | ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ |
| x                         | Tensor     | Required               | Shape:(batch\_size, token\_length), support bf16 and float16 type                                                                                                                                                         | token to be sent of this rank in dispatch phase                    |
| expert\_ids               | Tensor     | Required               | Shape:(batch\_size, topk)，int32 type, range: \[-1, num\_experts)，where -1 is used as a placeholder. A token cannot sent to an expert beyond one time.                                                                     | target expert IDs of each token                                    |
| gmm1\_weight              | Tensor\[]  | Required               | In coupling mode，there is one tensor, Shape:(localExpertNum, token\_length, gmm1\_hidden\_size); In seperated mode，there are localExpertNum tensors, TensorShape：（token\_length, gmm1\_hidden\_size; int8 type            | GMM1 weight matrix，supports coupling mode and seperated mode       |
| gmm1\_weight\_scale       | Tensor\[]  | Required               | In coupling mode，there is one tensor, Shape:(localExpertNum, gmm1\_hidden\_size); In seperated mode，there are localExpertNum tensors, each tensor Shape：（gmm1\_hidden\_size）; float32 type                                | GMM1 weight scale matrix，supports coupling mode and seperated mode |
| gmm2\_weight              | Tensor\[]  | Required               | In coupling mode，there is one tensor, Shape:(localExpertNum, gmm1\_hidden\_size/2, token\_length); In seperated mode，there are localExpertNum tensors, each tensor Shape：（gmm1\_hidden\_size/2, token\_length）; int8 type | GMM2 weight matrix，supports coupling mode and seperated mode       |
| gmm2\_weight\_scale       | Tensor\[]  | Required               | In coupling mode，there is one tensor, Shape:(localExpertNum, token\_length); In seperated mode，there are localExpertNum tensors, each tensor Shape：（token\_length）; float32 type                                          | GMM2 weight scale matrix，supports coupling mode and seperated mode |
| expert\_scales            | Tensor     | Required               | Shape：(batch\_size, topk), float32 type                                                                                                                                                                                   | weights of each expert，used in combine phase                       |
| expert\_smooth\_scales    | Tensor     | Optional               | --                                                                                                                                                                                                                        | Reserved parameter                                                 |
| x\_active\_mask           | Tensor     | Optional               | Shape: (batch\_size), bool type, value in\[true, false], the true value must come before the false value                                                                                                                  | Reserved parameter，set to None                                     |
| group\_ep                 | str        | Required               | Length of str：(0, 128), make sure it is valid                                                                                                                                                                             | HCCL communication group name                                      |
| ep\_rank\_size            | int        | Required               | Required：(ep\_rank\_size \* MoeExpertNumPerRank) ≤ 512, and ep\_rank\_size > 0                                                                                                                                            | EP group size                                                      |
| ep\_rank\_id              | int        | Required               | range: \[0, ep\_rank\_size)                                                                                                                                                                                               | rank ID in EP group                                                |
| moe\_expert\_num          | int        | Required               | Required：(ep\_rank\_size - shared\_expert\_rank\_num) % moe\_expert\_num == 0                                                                                                                                             | MOE expert number                                                  |
| shared\_expert\_num       | int        | Required               | support 1 only                                                                                                                                                                                                            | shared expert number                                               |
| shared\_expert\_rank\_num | int        | Required               | Required：(ep\_rank\_size - shared\_expert\_rank\_num) % moe\_expert\_num == 0, and shared\_expert\_rank\_num < ep\_rank\_size                                                                                             | rank number of shared experts                                      |
| quant\_mode               | int        | Required               | Reserved parameter, set to 0                                                                                                                                                                                              | quant mode                                                         |
| global\_bs                | int        | Required               | set to 0 or (batch\_size \* ep\_rank\_size) when token is the same in different ranks; set to (max\_batch\_size \* ep\_rank\_size) otherwise.                                                                             | max token number among all ranks                                   |

##### 1.1.9.4 Return Value

Return value is a list of tensors，which stores combine\_x and expert\_token\_nums.

| **📌Parameter**     | **🔧type** | **📋Value Range**                                            | **📝Details**                                           |
| ------------------- | ---------- | ------------------------------------------------------------ | ------------------------------------------------------- |
| combine\_x          | Tensor     | Shape：(batch\_size, token\_length), the same type as input x | token after combination from experts in different ranks |
| expert\_token\_nums | Tensor     | Shape：(local\_expert\_num), int64 type                       | token number received by each expert in current rank    |

##### 1.1.9.5 Constraints and Precautions ⚠️

1. Input Shape should satisfy the shape definition above.
2. Current interface supports A3 only.
3. Current interface do not support concurrent usage.
4. Support aclgraph only when graph in on.
5. Do not support shared experts.
6. The performance may decline when batch\_size is lower than 16, as it is not the target scenario.
7. Other Constraits need to be satisfy:

- top\_k range：\[0， 12] and it should be lower than expert number.
- BS range：\[0，256]
- num\_experts range：(0， 512]
- Required: local\_expert\_num ≤ (aivnum / 2), where aivnum is the vector core number
- Required: token length range: \[1024, 7168] and (hidden\_size % 256) == 0
- Required: gmm1\_hiden\_size range: \[1024, 6144] and (gmm1\_hiden\_size % 256) == 0
- Required：HCCL\_BUFFERSIZE should be greater than \[(ep\_rank\_size \* max\_batch\_size \* moe\_expert\_num\_per\_rank \* total\_length \* sizeof(x) \* 2) / 1024 / 1024], which should be round up to the nearest integer.
- Required：global\_bs ≥ 0 and（global\_bs % ep\_rank\_size） == 0
- Required: gmm1\_weight, gmm1\_weight\_scale, gmm2\_weight, gmm2\_weight\_scale should be in the same mode

