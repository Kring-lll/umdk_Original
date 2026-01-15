#include <string.h>
#include "graph/types.h"
#include "aclnn_notify_dispatch_a2.h"

enum NnopbaseHcclServerType {
    NNOPBASE_HCCL_SERVER_TYPE_AICPU = 0,
    NNOPBASE_HCCL_SERVER_TYPE_MTE,
    NNOPBASE_HCCL_SERVER_TYPE_END
};
extern "C" void __attribute__((weak)) NnopbaseSetHcclServerType(void *executor, NnopbaseHcclServerType sType);

#ifdef __cplusplus
extern "C" {
#endif

extern aclnnStatus aclnnInnerNotifyDispatchA2GetWorkspaceSize(
    const aclTensor *sendData,
    const aclTensor *tokenPerExpertData,
    const aclTensor *tmpData,
    int64_t sendCount,
    int64_t numTokens,
    int64_t topkNum,
    int64_t numExperts,
    char *commGroup,
    int64_t rankSize,
    int64_t rankId,
    int64_t localRankSize,
    int64_t localRankId,
    const aclTensor *sendDataOffsetOut,
    const aclTensor *recvDataOut,
    const aclTensor *tokenServerIdxOut,
    const aclTensor *tokenUniquePerServerOut,
    const aclTensor *epRankTokenCntOut,
    const aclTensor *localEpTokenCntOut,
    const aclTensor *srcOffsetRankTokenIdxOut,
    const aclTensor *dstOffsetRankTokenIdxOut,
    const aclTensor *offsetInnerOut,
    const aclTensor *countOuterOut,
    const aclTensor *expandIdxOut,
    const aclTensor *totalRecvTokensOut,
    uint64_t *workspaceSize,
    aclOpExecutor **executor);

extern aclnnStatus aclnnInnerNotifyDispatchA2(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream);


aclnnStatus aclnnNotifyDispatchA2GetWorkspaceSize(
    const aclTensor *sendData,
    const aclTensor *tokenPerExpertData,
    const aclTensor *tmpData,
    int64_t sendCount,
    int64_t numTokens,
    int64_t topkNum,
    int64_t numExperts,
    char *commGroup,
    int64_t rankSize,
    int64_t rankId,
    int64_t localRankSize,
    int64_t localRankId,
    const aclTensor *sendDataOffsetOut,
    const aclTensor *recvDataOut,
    const aclTensor *tokenServerIdxOut,
    const aclTensor *tokenUniquePerServerOut,
    const aclTensor *epRankTokenCntOut,
    const aclTensor *localEpTokenCntOut,
    const aclTensor *srcOffsetRankTokenIdxOut,
    const aclTensor *dstOffsetRankTokenIdxOut,
    const aclTensor *offsetInnerOut,
    const aclTensor *countOuterOut,
    const aclTensor *expandIdxOut,
    const aclTensor *totalRecvTokensOut,
    uint64_t *workspaceSize,
    aclOpExecutor **executor)
{
    return aclnnInnerNotifyDispatchA2GetWorkspaceSize(
        sendData,
        tokenPerExpertData,
        tmpData,
        sendCount,
        numTokens,
        topkNum,
        numExperts,
        commGroup,
        rankSize,
        rankId,
        localRankSize,
        localRankId,
        sendDataOffsetOut,
        recvDataOut,
        tokenServerIdxOut,
        tokenUniquePerServerOut,
        epRankTokenCntOut,
        localEpTokenCntOut,
        srcOffsetRankTokenIdxOut,
        dstOffsetRankTokenIdxOut,
        offsetInnerOut,
        countOuterOut,
        expandIdxOut,
        totalRecvTokensOut,
        workspaceSize,
        executor);
}

aclnnStatus aclnnNotifyDispatchA2(
    void *workspace,
    uint64_t workspaceSize,
    aclOpExecutor *executor,
    aclrtStream stream)
{
    return aclnnInnerNotifyDispatchA2(
        workspace,
        workspaceSize,
        executor,
        stream);
}


#ifdef __cplusplus
}
#endif
