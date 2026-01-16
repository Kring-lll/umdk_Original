/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: notify dispatch kernel A2 part
 * Create: 2026-01-15
 * Note:
 * History: 2026-01-15 create notify dispatch A2 kernel part
 */

#ifndef NOTIFY_DISPATCH_A2_H
#define NOTIFY_DISPATCH_A2_H

#include <climits>
#include "kernel_operator.h"

#include "comm_args.h"
#include "data_copy.h"
#include "moe_distribute_base.h"
#include "notify_dispatch_tiling_a2.h"

using namespace AscendC;
using namespace Moe;

template <AscendC::HardEvent event>
__aicore__ inline void SyncFunc()
{
    int32_t eventID = static_cast<int32_t>(GetTPipePtr()->FetchEventID(event));
    AscendC::SetFlag<event>(eventID);
    AscendC::WaitFlag<event>(eventID);
}

template <typename T>
class NotifyDispatchA2 {
    constexpr static int32_t MAX_CORE_NUM = 20;
    constexpr static int64_t MULTI_RANK_SIZE = 4;  // 每个core最多往4个rank发送数据，64卡场景
    constexpr static int64_t MAX_RANK_SIZE = 64;   // 910B设备本算子最大支持的rank数，64卡场景
    constexpr static int32_t INVALID_RANK = -1;
    constexpr static uint32_t TEMP_BUF_LEN = 128 * 1024;  // tBuf注册长度为128K，剩余部分注册为其他buffer

    constexpr static uint32_t BW_ITEM_SIZE = 32;                               // = sizeof(BatchWriteItem)
    constexpr static uint32_t U64_PER_ITEM = BW_ITEM_SIZE / sizeof(uint64_t);  // 每个BatchWriteItem占多少个unit64
    constexpr static uint32_t U32_PER_ITEM = BW_ITEM_SIZE / sizeof(uint32_t);  // 每个BatchWriteItem占多少个unit32
    constexpr static uint32_t BW_MEB_OFFSET64_LOCAL_GM = 0;  // BatchWriteItem成员变量offset，按照sizeof(unit64)计算
    constexpr static uint32_t BW_MEB_OFFSET64_REMOTE_GM = 1;  // BatchWriteItem成员变量offset，按照sizeof(unit64)计算
    constexpr static uint32_t BW_MEB_OFFSET64_DATA_SIZE = 2;  // BatchWriteItem成员变量offset，按照sizeof(unit64)计算
    constexpr static uint32_t BW_MEB_OFFSET32_DATA_TYPE = 6;  // BatchWriteItem成员变量offset，按照sizeof(unit32)计算
    constexpr static uint32_t BW_MEB_OFFSET32_TARGET_RANK = 7;  // BatchWriteItem成员变量offset，按照sizeof(unit32)计算

    constexpr static int32_t FLAG_VALUE = 0xFFFFFFFF;
    constexpr static uint32_t STATUS_ENTRY_SIZE = 32;  // 每个status entry占用的空间大小, bytes
    constexpr static uint32_t U32_STATUS_ENTRY = STATUS_ENTRY_SIZE / sizeof(int32_t);
    constexpr static uint32_t FLAG_OFFSET = 8;          // status_flag 在 statusTensor中的offset, bytes
    constexpr static uint32_t SOURCE_RANK_OFFSET = 16;  // sourceRankId 在 statusTensor中的offset, bytes
    constexpr static uint32_t DEST_RANK_OFFSET = 20;    // destRankId 在 statusTensor中的offset, bytes
    constexpr static uint32_t DATALEN_OFFSET = 24;      // dataLen 在 statusTensor中的offset, bytes
    constexpr static uint32_t UB_ALIGN = 32;            // UB按32字节对齐
    constexpr static uint64_t EXP_TOKEN_COUNT_FLAG_CNT = UB_ALIGN / sizeof(uint64_t);  // 4
    constexpr static uint32_t GM_ALIGN = 64;                                           // GM按64字节对齐

    constexpr static uint32_t MAX_BS = 4096;  // 每卡支持的最大bs
    // Synchronization flag occupies length
    constexpr static int64_t FLAG_UNIT_INT_NUM = 4;
    constexpr static int64_t MAGIC_MASK = ~((1LL << 32) - 1);
    constexpr static int64_t NUM_2 = 2;
    constexpr static int64_t NUM_3 = 3;
    constexpr static int64_t NUM_4 = 4;
    constexpr static int64_t NUM_1000 = 1000;

public:
    __aicore__ inline NotifyDispatchA2(int rank, int rankSize, uint32_t extraFlag)
        : rank(rank), rankSize(rankSize), extraFlag(extraFlag)
    {}

    __aicore__ inline void Init(GM_ADDR sendDataInput, GM_ADDR tokenPerExpertDataInput, GM_ADDR tmpDataInput,
        GM_ADDR sendDataOffsetOutput,
        GM_ADDR recvDataOutput, int64_t len, int64_t numTokens, int64_t topkNum, int64_t numExperts, int op, int root,
        int cycleCount, GM_ADDR scale, int64_t scaleCount, GM_ADDR offset, int localRank, int localRankSize,
        GM_ADDR tokenServerIdxOutput, GM_ADDR tokensUniquePerServerOutput, GM_ADDR epRankTokenCntOutput,
        GM_ADDR localEpTokenCntOutput, GM_ADDR srcOffsetRankTokenIdxOutput, GM_ADDR dstOffsetRankTokenIdxOutput,
        GM_ADDR offsetInnerOutput, GM_ADDR countOuterOutput, GM_ADDR expandIdxOutput, GM_ADDR totalRecvTokensOutput,
        GM_ADDR workspace, GM_ADDR tiling)
    {
        return;
    }

    __aicore__ inline void Process()
    {
        return;
    }

private:
    __aicore__ inline void InitAll2AllLayeredRdma(GM_ADDR sendDataInput, GM_ADDR tokenPerExpertDataInput,
        GM_ADDR tmpDataInput, GM_ADDR sendDataOffsetOutput,
        GM_ADDR recvDataOutput, int64_t len, int64_t numTokens, int64_t topkNum, int64_t numExperts, int op, int root,
        int cycleCount, GM_ADDR scale, int64_t scaleCount, GM_ADDR offset, int localRank, int localRankSize,
        GM_ADDR tokenServerIdxOutput, GM_ADDR tokensUniquePerServerOutput, GM_ADDR epRankTokenCntOutput,
        GM_ADDR localEpTokenCntOutput, GM_ADDR srcOffsetRankTokenIdxOutput, GM_ADDR dstOffsetRankTokenIdxOutput,
        GM_ADDR offsetInnerOutput, GM_ADDR countOuterOutput, GM_ADDR expandIdxOutput, GM_ADDR totalRecvTokensOutput,
        GM_ADDR workspace, GM_ADDR tiling)
    {
        this->root = 0;
        this->len = len;
        this->numExperts = numExperts;
        this->numTokens = numTokens;
        this->topkNum = topkNum;
        this->scale = nullptr;
        this->magic = 0;
        this->localRank = localRank;
        this->localRankSize = localRankSize;
        this->xRankSize = localRankSize;
        this->yRankSize = rankSize / localRankSize;
        this->xRankIdx = rank % localRankSize;
        this->yRankIdx = rank / localRankSize;
        this->blockIdx = GetBlockIdx();
        this->blockNum = GetBlockNum();
        uint8_t ctxIdx;

        ctxIdx = COMM_EP_IDX;

        // 初始化RDMA相关变量
        auto tilingData = (__gm__ NotifyDispatchA2TilingData *)tiling;
        __gm__ void *mc2InitTiling = (__gm__ void *)(&(tilingData->mc2InitTiling));
        __gm__ void *mc2CcTiling = (__gm__ void *)(&(tilingData->mc2CcTiling1));

        auto contextGM0 = AscendC::GetHcclContext<HCCL_GROUP_ID_0>();

        hccl_.Init(contextGM0, mc2InitTiling);
        hccl_.SetCcTiling(mc2CcTiling);
        this->winContext_[COMM_EP_IDX] = (__gm__ HcclOpResParam *)contextGM0;
        notifyMemoryOffset = winContext_[COMM_EP_IDX]->winSize - IPC_BUFF_MAX_SIZE * NUM_2;
        // 设置并自增magic
        magicTensor_.SetGlobalBuffer((__gm__ uint64_t *)(hccl_.GetWindowsInAddr(rank) + IPC_DATA_OFFSET -
                                                         blockNum * sizeof(uint64_t) * EXP_TOKEN_COUNT_FLAG_CNT +
                                                         notifyMemoryOffset));

        pipe.InitBuffer(this->tBuf, TEMP_BUF_LEN);
        LocalTensor<uint64_t> tempLocal = tBuf.Get<uint64_t>();
        tempLocal(0) = 1;
        // 使用atomic方式实现+1
        AscendC::SetAtomicAdd<uint64_t>();
        AscendC::SetFlag<HardEvent::S_MTE3>(EVENT_ID0);
        AscendC::WaitFlag<HardEvent::S_MTE3>(EVENT_ID0);  // 等待SetValue完成
        DataCopy(magicTensor_[blockIdx * EXP_TOKEN_COUNT_FLAG_CNT], tempLocal, EXP_TOKEN_COUNT_FLAG_CNT);
        AscendC::SetAtomicNone();
        AscendC::SetFlag<HardEvent::MTE3_S>(EVENT_ID0);
        AscendC::WaitFlag<HardEvent::MTE3_S>(EVENT_ID0);  // 等待DataCopy完成
        magic = magicTensor_.GetValue(blockIdx * EXP_TOKEN_COUNT_FLAG_CNT);
        PipeBarrier<PIPE_ALL>();
        // 初始化目标rank的shareAddrs
        for (int i = 0; i < rankSize; i++) {
            this->shareAddrs[i] =
                hccl_.GetWindowsInAddr(i) + notifyMemoryOffset + (magic % PING_PONG_SIZE) * IPC_BUFF_MAX_SIZE;
        }
    }

    template <typename K, typename U = K>
    __aicore__ inline void CpGM2GMPingPong(int64_t dataSizeRemain, const GlobalTensor<U> &sendDataInputGt,
                                           const GlobalTensor<K> &recvDataOutputGT, int op);
    template <typename F>
    __aicore__ inline void SetAtomic(int op);
    __aicore__ inline void UnsetAtomic(int op);
    template <HardEvent eventType>
    __aicore__ inline void SetWaitEvent(event_t eventId);

    GlobalTensor<T> sendDataInputGt;
    GlobalTensor<int> tokenPerExpertDataInputGt;
    GlobalTensor<int> tmpDataInputGt;
    GlobalTensor<T> sendDataOffsetOutputGt;
    GlobalTensor<T> recvDataOutputGt;
    GlobalTensor<T> readGt;
    GlobalTensor<T> writeGt;
    GlobalTensor<T> remoteGt;

    __gm__ T *sendDataInput;
    __gm__ int *tokenPerExpertDataInput;
    __gm__ int *tmpDataInput;
    __gm__ T *sendDataOffsetOutput;
    __gm__ T *recvDataOutput;

    int64_t queLen;
    int64_t queSize;
    int64_t queElemLen;  // 共享内存队列里每个元素大小（以sizeof(T)计）

    int64_t coreNumBetween;  // 分层通信第一阶段，Server间通信使用的核数
    int64_t coreNumWithin;   // 分层通信第二阶段，Server内通信使用的核数
    int32_t rankNumPerCore;  // 每个核负责的rank数

    // RDMA相关变量
    int32_t serverNum;                    // Server数量
    int32_t serverId;                     // 本卡所属的server ID
    int32_t targetRank[MULTI_RANK_SIZE];  // 当前核心跨Server发送数据的目标rank Id，即数据最终的目标rank
    int32_t targetRankNum;  // 当前核心跨Server发送数据的目标rank Id的数量，小于等于MULTI_RANK_SIZE
    int64_t perRankDataNum;

    int rank;
    int rankSize;
    int localRank = 0;
    int localRankSize = 0;  // 在910A5中，表示一块板子上使用的卡数，在910B上表示单机内卡数。
    int xRankSize = 0;
    int yRankSize = 0;
    int xRankIdx = 0;
    int yRankIdx = 0;
    uint32_t extraFlag;
    int root;
    int sendPerGroup = 3;
    int topkNum;
    int64_t numExperts;
    int64_t numTokens;
    int64_t len;
    uint64_t magic;
    int64_t blockIdx;  // 当前aicore序号
    int64_t blockNum;  // 当前rank的总aicore数
    int64_t timeout;
    GM_ADDR scale;
    GM_ADDR shareAddrs[CAM_MAX_RANK_SIZE];  // 共享内存地址列表
    __gm__ HcclOpResParam *winContext_[COMM_NUM]{nullptr, nullptr};
    TPipe pipe;  // pipe工具类
    TBuf<QuePosition::VECCALC> tBuf;

    Hccl<HCCL_SERVER_TYPE_AICPU> hccl_;
    GM_ADDR windowInGM_;
    GM_ADDR windowOutGM_;
    GlobalTensor<uint64_t> magicTensor_;  // 用于存放magic，位于windowInstatusTensor_之前
    GlobalTensor<uint32_t> batchWriteInfoTensor_;
    GlobalTensor<int32_t> windowInstatusTensor_;  // 用于rank间状态同步
    GlobalTensor<T> windowInTensor_;
    GlobalTensor<int32_t> windowOutstatusTensor_;  // 用于rank间状态同步
    GlobalTensor<T> windowOutTensor_;
    TBuf<> batchWriteInfoBuf_;  // 临时存放 batch write info
    TBuf<> tempBuf_;
    TBuf<> statusBuf_;
    LocalTensor<int32_t> statusTensor_;  // 临时存放statusFlag
    TBuf<> tokenPerExpertDataBuf;
    TBuf<> sendDataOffsetBuf;
    TBuf<> sendDataBuf;
    TBuf<> tempBuf2_;
    TBuf<> tempBuf3_;
    TBuf<> tempBuf4_;
    TBuf<> tempBuf5_;
    TBuf<> tempBuf6_;
    TBuf<> tempBuf7_;
    TBuf<> tempBuf8_;
    TBuf<> tempBuf9_;
    TBuf<> tempBuf10_;
    TBuf<> tempBuf11_;

    uint32_t sendDataAlignLen{0};
    uint32_t tokenPerExpertDataAlignLen{0};
    uint32_t sendDataOffsetAlignLen{0};

    uint32_t numTokensPerExpertAlignLen{0};   // 每个expert从本卡接收的token个数，对应一个rank的数据
    uint32_t gNumTokensPerExpertAlignLen{0};  // 全局，包含所有rank的
    uint32_t numTokensUniquePerServerAlignLen{0};  // 每个server从本卡接收的token个数(去重)，对应一个rank的
    uint32_t gNumTokensUniquePerServerAlignLen{0};  // 全局，包含所有rank的
    uint32_t numTokensPerServerAlignLen{0};  // 本卡每个token发到每个server的个数(不去重), 对应一个rank的
    uint32_t gNumTokensPerServerAlignLen{0};  // 全局，包含所有rank的
    uint32_t tokenServerCntAlignLen{0};       // 本卡每个token发给多少个server, 对应一个rank的
    uint32_t gTokenServerCntAlignLen{0};      // 全局，包含所有rank的
    uint32_t tokenServerIdxAlignLen{0};       // 本卡每个token发送给各个server的顺序, 对应一个rank的
    uint32_t gTokenServerIdxAlignLen{0};      // 全局，包含所有rank的
    uint32_t tokenExpertIdxAlignLen{0};       // 每个token发到expert的顺序, 对应一个rank的
    uint32_t gTokenExpertIdxAlignLen{0};      // 全局，包含所有rank的
    uint32_t expertMaxBsSrcOffsetAlignLen{0};  // 每个expert从本卡接收的token的server内offset, 对应一个rank的
    uint32_t gExpertMaxBsSrcOffsetAlignLen{0};  // 全局，包含所有rank的
    uint32_t expertMaxBsOriOffsetAlignLen{0};  // 每个expert从本卡接收的token在原卡上的origin_offset, 对应一个rank的
    uint32_t gExpertMaxBsOriOffsetAlignLen{0};  // 全局，包含所有rank的
    uint32_t notifyMemoryOffset{0};

    GlobalTensor<int32_t> gRankEpTokenCntGT_;  // 临时数据
    GlobalTensor<int32_t> gExpertMaxBsSrcGT_;  // 临时数据

    GlobalTensor<int32_t>
        tokenServerIdxOutputGT_;  // token发送给对应server的token序号，-1表示没有，0-N表示序号 [bs, serverNum]
    GlobalTensor<int32_t>
        tokensUniquePerServerOutputGT_;  // 当前rank发送给对应server的token个数 [serverNum] -> value:count数量
    GlobalTensor<int32_t>
        epRankTokenCntOutputGT_;  // 每个专家、从rank接收的token数量 [expert_num, rank_num] -> value:token_cnt
    GlobalTensor<int64_t> localEpTokenCntOutputGT_;  // 本卡每个专家接收的token数量 [local_expert_num]
    GlobalTensor<int32_t> srcOffsetRankTokenIdxOutputGT_;  // 每个专家、从rank接收的token源端偏移 [expert_num, rank_num,
                                                           // token_idx] -> value:src_offset
    GlobalTensor<int32_t> dstOffsetRankTokenIdxOutputGT_;  // 每个专家、从rank接收的token目的端偏移 [expert_num,
                                                           // rank_num, token_idx] -> value:dst_offset
    GlobalTensor<int32_t> countInnerOutputGT_;   // token给各个server发送个数    弃用
    GlobalTensor<int32_t> offsetInnerOutputGT_;  // 存放全局的expandIdx, [globalBs, expertNum]
    GlobalTensor<int32_t> countOuterOutputGT_;   // 每个token发送到的server数量 [bs] -> value:server数量
    GlobalTensor<int32_t> offsetOuterOutputGT_;  // 每个token在server上的位次    同tokenServerIdxOutputGT_
    GlobalTensor<int32_t>
        expandIdxOutputGT_;  // 给同一专家的token个数 [bs * numExperts], topk_idx的同专家前缀和扩维到所有专家维度
    GlobalTensor<int32_t> totalRecvTokensOutputGT_;  // 本卡收到的token总数, [1] -> value:count
};

template <typename T>
template <typename F>
__aicore__ inline void NotifyDispatchA2<T>::SetAtomic(int op)
{
    PipeBarrier<PIPE_ALL>();
    if (op != -1) {
#ifdef __DAV_C220_VEC__
        SetAtomicOpType<F>(op);
#endif
    }
    PipeBarrier<PIPE_ALL>();
}

template <typename T>
template <HardEvent eventType>
__aicore__ inline void NotifyDispatchA2<T>::SetWaitEvent(event_t eventId)
{
    AscendC::SetFlag<eventType>(eventId);
    AscendC::WaitFlag<eventType>(eventId);
}

template <typename T>
__aicore__ inline void NotifyDispatchA2<T>::UnsetAtomic(int op)
{
    if (op != -1) {
        AscendC::SetAtomicNone();
    }
    PipeBarrier<PIPE_ALL>();
}

template <typename T>
template <typename K, typename U>
__aicore__ inline void NotifyDispatchA2<T>::CpGM2GMPingPong(int64_t dataSizeRemain,
                                                            const GlobalTensor<U> &sendDataInputGt,
                                                            const GlobalTensor<K> &recvDataOutputGT, int op)
{
    // General case (U = K), input/output are the same, share one UB
    // Only when conversion is needed (U->K), UB will be divided into two parts according to the ratio of
    // sizeof(U):sizeof(K) and aligned to 32 bytes
    constexpr int32_t ubBlockSize = UB_SINGLE_PING_PONG_ADD_SIZE_MAX;
    constexpr int32_t ubAlignNum = ubBlockSize / (sizeof(K) + sizeof(U)) / UB_ALIGN_SIZE * UB_ALIGN_SIZE;
    constexpr int32_t inputUbBlockSize = std::is_same_v<K, U> ? ubBlockSize : ubAlignNum * sizeof(U);
    constexpr int32_t outputUbBlockSize = std::is_same_v<K, U> ? ubBlockSize : ubAlignNum * sizeof(K);

    __gm__ U *input = const_cast<__gm__ U *>(sendDataInputGt.GetPhyAddr());
    __gm__ K *output = const_cast<__gm__ K *>(recvDataOutputGT.GetPhyAddr());
    __ubuf__ U *inputUB[2] = {(__ubuf__ U *)(UB_HEAD_OFFSET), (__ubuf__ U *)(UB_MID_OFFSET)};
    __ubuf__ K *outputUB[2] = {(__ubuf__ K *)inputUB[0], (__ubuf__ K *)inputUB[1]};
    if constexpr (!std::is_same_v<K, U>) {
        outputUB[0] = (__ubuf__ K *)(inputUB[0] + inputUbBlockSize / sizeof(U));
        outputUB[1] = (__ubuf__ K *)(inputUB[1] + inputUbBlockSize / sizeof(U));
    }
    int inputOffsetNum = 0;
    int outputOffsetNum = 0;
    if (dataSizeRemain <= 0) {
        return;
    }

    SetAtomic<K>(op);

    AscendC::SetFlag<HardEvent::MTE3_MTE2>(EVENT_ID0);  // MTE2 waits for MTE3
    AscendC::SetFlag<HardEvent::MTE3_MTE2>(EVENT_ID1);  // MTE2 waits for MTE3
    for (int64_t i = 0; dataSizeRemain > 0; i++) {
        // size and dataSizeRemain both refer to the output size
        uint32_t size = dataSizeRemain > outputUbBlockSize ? outputUbBlockSize : dataSizeRemain;
        event_t eventId = (i & 1) ? EVENT_ID0 : EVENT_ID1;
        AscendC::WaitFlag<HardEvent::MTE3_MTE2>(eventId);
        CpGM2UB((i & 1) ? inputUB[0] : inputUB[1], input + inputOffsetNum, size / sizeof(K) * sizeof(U));
        if constexpr (!std::is_same_v<K, U>) {
            SetWaitEvent<HardEvent::MTE2_V>(eventId);
            CastImpl((i & 1) ? outputUB[0] : outputUB[1], (i & 1) ? inputUB[0] : inputUB[1], RoundMode::CAST_NONE,
                     size / sizeof(K));
            SetWaitEvent<HardEvent::V_MTE3>(eventId);
        }
        AscendC::SetFlag<HardEvent::MTE2_MTE3>(eventId);
        AscendC::WaitFlag<HardEvent::MTE2_MTE3>(eventId);
        CpUB2GM(output + outputOffsetNum, (i & 1) ? outputUB[0] : outputUB[1], size);
        AscendC::SetFlag<HardEvent::MTE3_MTE2>(eventId);

        dataSizeRemain -= size;
        inputOffsetNum += (size / sizeof(K));
        outputOffsetNum += (size / sizeof(K));
    }
    AscendC::WaitFlag<HardEvent::MTE3_MTE2>(EVENT_ID0);  // MTE2 waits for MTE3
    AscendC::WaitFlag<HardEvent::MTE3_MTE2>(EVENT_ID1);  // MTE2 waits for MTE3

    AscendC::SetFlag<HardEvent::MTE3_S>(EVENT_ID3);  // Scalar waits for MTE3
    AscendC::WaitFlag<HardEvent::MTE3_S>(EVENT_ID3);

    UnsetAtomic(op);
    return;
}

#endif /* NOTIFY_DISPATCH_A2_H */
