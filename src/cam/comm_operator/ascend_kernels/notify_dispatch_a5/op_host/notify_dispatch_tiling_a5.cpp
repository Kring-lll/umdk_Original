/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: notify dispatch A5 host part (multi-mesh layered support)
 * Create: 2026-05-26
 * Note:
 * History: 2026-05-26 create notify dispatch A5 host part
 */

#include <queue>
#include <vector>
#include <dlfcn.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <sys/types.h>
#include <unistd.h>
#include <cmath>
#include <cstdint>
#include <string>

#include "ops_log.h"
#include "ops_error.h"
#include "graph/utils/type_utils.h"
#include "register/op_def_registry.h"
#include "tiling/platform/platform_ascendc.h"
#include "tiling/hccl/hccl_tiling.h"
#include "mc2_tiling_utils.h"
#include "../op_kernel/notify_dispatch_tiling_a5.h"

#ifdef USE_CANN83_PATH
#include "platform/platform_infos_def.h"
#elif defined(USE_CANN82_PATH)
#include "experiment/platform/platform/platform_infos_def.h"
#else
#error "CANN version not supported or platform_infos_def.h not found. Check CANN_VERSION_MACRO definition."
#endif

using namespace ge;
using namespace Cam;
using namespace Util;

namespace {
constexpr const char *OPS_UTILS_LOG_SUB_MOD_NAME = "NOTIFY_DISPATCH_A5";
constexpr const char *OPS_UTILS_LOG_PACKAGE_TYPE = "CAM_OPS";
constexpr uint32_t OP_TYPE_ALL_TO_ALL = 8U;  // numeric representation of AlltoAll

constexpr uint32_t INPUT_SEND_DATA_INDEX = 0;
constexpr uint32_t INPUT_TOKEN_PER_EXPERT_INDEX = 1;
constexpr uint32_t INPUT_TMP_DATA_INDEX = 2;
constexpr uint32_t NUM_2 = 2;
constexpr uint32_t NUM_4 = 4;

constexpr uint32_t OUTPUT_SEND_DATA_OFFSET_INDEX = 0;
constexpr uint32_t OUTPUT_RECV_DATA_INDEX = 1;
constexpr uint32_t OUTPUT_TOKEN_MESH_IDX_INDEX = 2;
constexpr uint32_t OUTPUT_TOKEN_UNIQUE_PER_MESH_INDEX = 3;
constexpr uint32_t OUTPUT_EP_RANK_TOKEN_CNT_INDEX = 4;
constexpr uint32_t OUTPUT_LOCAL_EP_TOKEN_CNT_INDEX = 5;
constexpr uint32_t OUTPUT_SRC_OFFSET_RANK_TOKEN_INDEX = 6;
constexpr uint32_t OUTPUT_DST_OFFSET_RANK_TOKEN_INDEX = 7;
constexpr uint32_t OUTPUT_OFFSET_INNER_INDEX = 8;
constexpr uint32_t OUTPUT_COUNT_OUTER_INDEX = 9;
constexpr uint32_t OUTPUT_EXPAND_IDX_INDEX = 10;
constexpr uint32_t OUTPUT_TOTAL_RECV_TOKENS_INDEX = 11;

constexpr uint32_t ATTR_SEND_COUNT_INDEX = 0;
constexpr uint32_t ATTR_NUM_TOKENS_INDEX = 1;
constexpr uint32_t ATTR_TOPK_NUM_INDEX = 2;
constexpr uint32_t ATTR_NUM_EXPERTS_INDEX = 3;
constexpr uint32_t ATTR_COMM_GROUP_INDEX = 4;
constexpr uint32_t ATTR_RANK_SIZE_INDEX = 5;
constexpr uint32_t ATTR_RANK_ID_INDEX = 6;
constexpr uint32_t ATTR_LOCAL_RANK_SIZE_INDEX = 7;
constexpr uint32_t ATTR_LOCAL_RANK_ID_INDEX = 8;

constexpr size_t MAX_GROUP_NAME_LENGTH = 128UL;
constexpr int64_t MAX_COMM_WORLD_SIZE = 384;
constexpr int64_t MAX_A5_LOCAL_MESH_SIZE = 4;
constexpr uint32_t MIN_K_VALUE_A5 = 2;
constexpr uint32_t MAX_K_VALUE_A5 = 8;

constexpr uint32_t SYSTEM_NEED_WORKSPACE = 16 * 1024 * 1024;
constexpr uint32_t KERNEL_USE_WORKSPACE = 1 * 1024 * 1024;
constexpr uint32_t KERNEL_A5_ARG_SIZE = 16 * 1024 * 1024;
constexpr int32_t HCCL_BUFFER_SIZE_DEFAULT = 200 * 1024 * 1024;  // Bytes
constexpr uint64_t MB_SIZE = 1024UL * 1024UL;

constexpr static int TILING_KEY_INT = 23;
constexpr static int TILING_KEY_A5_TYPE = 200;
}  // namespace

namespace optiling {
static void PrintTilingDataInfo(const char *nodeName, const NotifyDispatchA5TilingData &tilingData)
{
    OPS_LOG_D(nodeName, "rankSize is %u.", tilingData.notifyDispatchInfoA5.rankSize);
    OPS_LOG_D(nodeName, "rankId is %u.", tilingData.notifyDispatchInfoA5.rankId);
    OPS_LOG_D(nodeName, "localRankSize is %u.", tilingData.notifyDispatchInfoA5.localRankSize);
    OPS_LOG_D(nodeName, "localRankId is %u.", tilingData.notifyDispatchInfoA5.localRankId);
    OPS_LOG_D(nodeName, "sendCount is %u.", tilingData.notifyDispatchInfoA5.sendCount);
    OPS_LOG_D(nodeName, "numTokens is %u.", tilingData.notifyDispatchInfoA5.numTokens);
    OPS_LOG_D(nodeName, "topkNum is %u.", tilingData.notifyDispatchInfoA5.topkNum);
    OPS_LOG_D(nodeName, "numExperts is %u.", tilingData.notifyDispatchInfoA5.numExperts);
    OPS_LOG_D(nodeName, "aivNum is %u.", tilingData.notifyDispatchInfoA5.aivNum);
    OPS_LOG_D(nodeName, "totalUbSize is %lu.", tilingData.notifyDispatchInfoA5.totalUbSize);
}

static bool CheckIfA5MultiMachine(const gert::TilingContext &context, const char *nodeName,
                                  const NotifyDispatchA5TilingData &tilingData)
{
    fe::PlatFormInfos *platformInfoPtr = context.GetPlatformInfo();
    OPS_ERR_IF(platformInfoPtr == nullptr, OPS_LOG_E(nodeName, "platformInfoPtr is nullptr."), return false);
    fe::PlatFormInfos &platformInfo = *platformInfoPtr;

    std::string socVersion;
    (void)platformInfo.GetPlatformResWithLock("version", "Short_SoC_version", socVersion);

    uint32_t numRanks = tilingData.notifyDispatchInfoA5.rankSize;
    uint32_t localRankSize = tilingData.notifyDispatchInfoA5.localRankSize;

    if (socVersion == "Ascend950" && numRanks > localRankSize) {
        return true;
    }
    return false;
}

static ge::graphStatus GetAttrAndSetTilingData(const gert::TilingContext &context, const char *nodeName,
                                               NotifyDispatchA5TilingData &tilingData, std::string &commGroup)
{
    auto attrs = context.GetAttrs();
    OPS_ERR_IF(attrs == nullptr, OPS_LOG_E(nodeName, "attrs is nullptr."), return ge::GRAPH_FAILED);

    auto sendCountPtr = attrs->GetAttrPointer<int64_t>(ATTR_SEND_COUNT_INDEX);
    auto numTokenPtr = attrs->GetAttrPointer<int64_t>(ATTR_NUM_TOKENS_INDEX);
    auto topkNumPtr = attrs->GetAttrPointer<int64_t>(ATTR_TOPK_NUM_INDEX);
    auto numExpertsPtr = attrs->GetAttrPointer<int64_t>(ATTR_NUM_EXPERTS_INDEX);
    auto commGroupPtr = attrs->GetAttrPointer<char>(static_cast<int>(ATTR_COMM_GROUP_INDEX));
    auto rankSizePtr = attrs->GetAttrPointer<int64_t>(ATTR_RANK_SIZE_INDEX);
    auto rankIdPtr = attrs->GetAttrPointer<int64_t>(ATTR_RANK_ID_INDEX);
    auto localRankSizePtr = attrs->GetAttrPointer<int64_t>(ATTR_LOCAL_RANK_SIZE_INDEX);
    auto localRankIdPtr = attrs->GetAttrPointer<int64_t>(ATTR_LOCAL_RANK_ID_INDEX);

    OPS_ERR_IF((commGroupPtr == nullptr) || (strnlen(commGroupPtr, MAX_GROUP_NAME_LENGTH) == 0) ||
                        (strnlen(commGroupPtr, MAX_GROUP_NAME_LENGTH) == MAX_GROUP_NAME_LENGTH),
                    OPS_LOG_E(nodeName, "commGroupPtr is null."), return ge::GRAPH_FAILED);
    OPS_ERR_IF(sendCountPtr == nullptr, OPS_LOG_E(nodeName, "sendCountPtr is null."), return ge::GRAPH_FAILED);
    OPS_ERR_IF(numTokenPtr == nullptr, OPS_LOG_E(nodeName, "numTokenPtr is null."), return ge::GRAPH_FAILED);
    OPS_ERR_IF(topkNumPtr == nullptr, OPS_LOG_E(nodeName, "topkNumPtr is null."), return ge::GRAPH_FAILED);
    OPS_ERR_IF((*topkNumPtr < MIN_K_VALUE_A5) || (*topkNumPtr > MAX_K_VALUE_A5),
                    OPS_LOG_E(nodeName, "topkNum is invalid, only support [%u, %u], but got topkNum=%ld.",
                            MIN_K_VALUE_A5, MAX_K_VALUE_A5, *topkNumPtr),
                    return ge::GRAPH_FAILED);
    OPS_ERR_IF(numExpertsPtr == nullptr, OPS_LOG_E(nodeName, "numExpertsPtr is null."), return ge::GRAPH_FAILED);
    OPS_ERR_IF(rankSizePtr == nullptr, OPS_LOG_E(nodeName, "rankSizePtr is null."), return ge::GRAPH_FAILED);
    OPS_ERR_IF(rankIdPtr == nullptr, OPS_LOG_E(nodeName, "rankIdPtr is null."), return ge::GRAPH_FAILED);
    OPS_ERR_IF(localRankSizePtr == nullptr, OPS_LOG_E(nodeName, "localRankSizePtr is null."),
                    return ge::GRAPH_FAILED);
    OPS_ERR_IF(localRankIdPtr == nullptr, OPS_LOG_E(nodeName, "localRankIdPtr is null."), return ge::GRAPH_FAILED);

    OPS_ERR_IF((*rankSizePtr <= 0) || (*rankSizePtr > MAX_COMM_WORLD_SIZE),
                    OPS_LOG_E(nodeName, "rankSize is invalid, only support (0, %ld], but got rankSize=%ld.",
                            MAX_COMM_WORLD_SIZE, *rankSizePtr),
                    return ge::GRAPH_FAILED);
    OPS_ERR_IF(
        (*rankIdPtr < 0) || (*rankIdPtr >= *rankSizePtr),
        OPS_LOG_E(nodeName, "rankId is invalid, only support [0, %ld), but got rankId=%ld.", *rankSizePtr, *rankIdPtr),
        return ge::GRAPH_FAILED);
    OPS_ERR_IF((*localRankSizePtr <= 0) || (*localRankSizePtr > MAX_A5_LOCAL_MESH_SIZE),
                    OPS_LOG_E(nodeName,
                            "localRankSize is invalid, A5 only support (0, %ld], but got localRankSize=%ld.",
                            MAX_A5_LOCAL_MESH_SIZE, *localRankSizePtr),
                    return ge::GRAPH_FAILED);
    OPS_ERR_IF((*localRankIdPtr < 0) || (*localRankIdPtr >= *localRankSizePtr),
                    OPS_LOG_E(nodeName, "localRankId is invalid, only support [0, %ld), but got localRankId=%ld.",
                            *localRankSizePtr, *localRankIdPtr),
                    return ge::GRAPH_FAILED);
    OPS_ERR_IF((*rankSizePtr > *localRankSizePtr) && (*rankSizePtr % *localRankSizePtr != 0),
                    OPS_LOG_E(nodeName,
                            "rankSize (%ld) must be divisible by localRankSize (%ld) in multi-mesh scenario.",
                            *rankSizePtr, *localRankSizePtr),
                    return ge::GRAPH_FAILED);

    OPS_ERR_IF((*sendCountPtr <= 0),
                    OPS_LOG_E(nodeName,
                        "sendCount is invalid, only support > 0, but got sendCount=%ld.", *sendCountPtr),
                    return ge::GRAPH_FAILED);
    OPS_ERR_IF(
        (*numTokenPtr <= 0),
        OPS_LOG_E(nodeName, "numTokenPtr is invalid, only support > 0, but got numTokenPtr=%ld.", *numTokenPtr),
        return ge::GRAPH_FAILED);

    commGroup = std::string(commGroupPtr);
    tilingData.notifyDispatchInfoA5.rankSize = static_cast<uint32_t>(*rankSizePtr);
    tilingData.notifyDispatchInfoA5.rankId = static_cast<uint32_t>(*rankIdPtr);
    tilingData.notifyDispatchInfoA5.localRankSize = static_cast<uint32_t>(*localRankSizePtr);
    tilingData.notifyDispatchInfoA5.localRankId = static_cast<uint32_t>(*localRankIdPtr);
    tilingData.notifyDispatchInfoA5.sendCount = static_cast<uint32_t>(*sendCountPtr);
    tilingData.notifyDispatchInfoA5.numTokens = static_cast<uint32_t>(*numTokenPtr);
    tilingData.notifyDispatchInfoA5.topkNum = static_cast<uint32_t>(*topkNumPtr);
    tilingData.notifyDispatchInfoA5.numExperts = static_cast<uint32_t>(*numExpertsPtr);

    return ge::GRAPH_SUCCESS;
}

static void SetHcommCfg(const gert::TilingContext &context, NotifyDispatchA5TilingData &tiling,
                        const std::string commGroup)
{
    const char *nodeName = context.GetNodeName();
    OPS_LOG_D(nodeName, "NotifyDispatchA5 commGroup = %s", commGroup.c_str());
    uint32_t opType1 = OP_TYPE_ALL_TO_ALL;
    std::string algConfigAllToAllStr = "BatchWrite=level0:fullmesh"; // BatchWriteBySdma

    AscendC::Mc2CcTilingConfig mc2CcTilingConfig(commGroup, opType1, algConfigAllToAllStr);
    mc2CcTilingConfig.GetTiling(tiling.mc2InitTiling);
    mc2CcTilingConfig.GetTiling(tiling.mc2CcTiling1);
}

static ge::graphStatus SetWorkSpace(gert::TilingContext &context, const char *nodeName)
{
    size_t *workSpaces = context.GetWorkspaceSizes(1);
    OPS_ERR_IF(workSpaces == nullptr, OPS_LOG_E(nodeName, "workSpaces is nullptr."), return ge::GRAPH_FAILED);
    workSpaces[0] = SYSTEM_NEED_WORKSPACE + KERNEL_USE_WORKSPACE + KERNEL_A5_ARG_SIZE;
    return ge::GRAPH_SUCCESS;
}

static bool CheckTensorDataType(gert::TilingContext &context, const char *nodeName)
{
    OPS_LOG_D(nodeName, "========CheckTensorDataType============");
    auto sendData = context.GetInputDesc(INPUT_SEND_DATA_INDEX);
    OPS_ERR_IF(sendData == nullptr, OPS_LOG_E(nodeName, "sendData is null."), return false);
    OPS_ERR_IF(
        (sendData->GetDataType() != ge::DT_BF16) && (sendData->GetDataType() != ge::DT_FLOAT16) &&
            (sendData->GetDataType() != ge::DT_FLOAT) && (sendData->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "sendData datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(sendData->GetDataType())),
        return false);
    uint64_t dataSize;
    if ((sendData->GetDataType() == ge::DT_BF16) || (sendData->GetDataType() == ge::DT_FLOAT16)) {
        dataSize = NUM_2;
    } else {
        dataSize = NUM_4;
    }
    auto tokenPerExpertData = context.GetInputDesc(INPUT_TOKEN_PER_EXPERT_INDEX);
    OPS_ERR_IF(tokenPerExpertData == nullptr, OPS_LOG_E(nodeName, "tokenPerExpertData is null."), return false);
    OPS_ERR_IF(
        (tokenPerExpertData->GetDataType() != ge::DT_BF16) && (tokenPerExpertData->GetDataType() != ge::DT_FLOAT16) &&
            (tokenPerExpertData->GetDataType() != ge::DT_FLOAT) && (tokenPerExpertData->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(
            nodeName,
            "tokenPerExpertData datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
            static_cast<ge::DataType>(tokenPerExpertData->GetDataType())),
        return false);
    // for saving intermediate vars in kernel, same size as recvData
    auto tmpData = context.GetInputDesc(INPUT_TMP_DATA_INDEX);
    OPS_ERR_IF(tmpData == nullptr, OPS_LOG_E(nodeName, "tmpData is null."), return false);
    OPS_ERR_IF(
        (tmpData->GetDataType() != ge::DT_BF16) && (tmpData->GetDataType() != ge::DT_FLOAT16) &&
            (tmpData->GetDataType() != ge::DT_FLOAT) && (tmpData->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "tmpData datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(tmpData->GetDataType())),
        return false);

    auto sendDataOffset = context.GetOutputDesc(OUTPUT_SEND_DATA_OFFSET_INDEX);
    OPS_ERR_IF(sendDataOffset == nullptr, OPS_LOG_E(nodeName, "sendDataOffset is null."), return false);
    OPS_ERR_IF(
        (sendDataOffset->GetDataType() != ge::DT_BF16) && (sendDataOffset->GetDataType() != ge::DT_FLOAT16) &&
            (sendDataOffset->GetDataType() != ge::DT_FLOAT) && (sendDataOffset->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "sendDataOffset datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(sendDataOffset->GetDataType())),
        return false);

    auto recvData = context.GetOutputDesc(OUTPUT_RECV_DATA_INDEX);
    OPS_ERR_IF(recvData == nullptr, OPS_LOG_E(nodeName, "recvData is null."), return false);
    OPS_ERR_IF(
        (recvData->GetDataType() != ge::DT_BF16) && (recvData->GetDataType() != ge::DT_FLOAT16) &&
            (recvData->GetDataType() != ge::DT_FLOAT) && (recvData->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "recvData datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(recvData->GetDataType())),
        return false);

    auto tokenMeshIdx = context.GetOutputDesc(OUTPUT_TOKEN_MESH_IDX_INDEX);
    OPS_ERR_IF(tokenMeshIdx == nullptr, OPS_LOG_E(nodeName, "tokenMeshIdx is null."), return false);
    OPS_ERR_IF(
        (tokenMeshIdx->GetDataType() != ge::DT_BF16) && (tokenMeshIdx->GetDataType() != ge::DT_FLOAT16) &&
            (tokenMeshIdx->GetDataType() != ge::DT_FLOAT) && (tokenMeshIdx->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "tokenMeshIdx datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(tokenMeshIdx->GetDataType())),
        return false);

    auto tokenUniquePerMesh = context.GetOutputDesc(OUTPUT_TOKEN_UNIQUE_PER_MESH_INDEX);
    OPS_ERR_IF(tokenUniquePerMesh == nullptr, OPS_LOG_E(nodeName, "tokenUniquePerMesh is null."), return false);
    OPS_ERR_IF(
        (tokenUniquePerMesh->GetDataType() != ge::DT_BF16) &&
            (tokenUniquePerMesh->GetDataType() != ge::DT_FLOAT16) &&
            (tokenUniquePerMesh->GetDataType() != ge::DT_FLOAT) &&
            (tokenUniquePerMesh->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(
            nodeName,
            "tokenUniquePerMesh datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
            static_cast<ge::DataType>(tokenUniquePerMesh->GetDataType())),
        return false);

    auto epRankTokenCnt = context.GetOutputDesc(OUTPUT_EP_RANK_TOKEN_CNT_INDEX);
    OPS_ERR_IF(epRankTokenCnt == nullptr, OPS_LOG_E(nodeName, "epRankTokenCnt is null."), return false);
    OPS_ERR_IF(
        (epRankTokenCnt->GetDataType() != ge::DT_BF16) && (epRankTokenCnt->GetDataType() != ge::DT_FLOAT16) &&
            (epRankTokenCnt->GetDataType() != ge::DT_FLOAT) && (epRankTokenCnt->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "epRankTokenCnt datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(epRankTokenCnt->GetDataType())),
        return false);

    auto localEpTokenCnt = context.GetOutputDesc(OUTPUT_LOCAL_EP_TOKEN_CNT_INDEX);
    OPS_ERR_IF(localEpTokenCnt == nullptr, OPS_LOG_E(nodeName, "localEpTokenCnt is null."), return false);
    OPS_ERR_IF((localEpTokenCnt->GetDataType() != ge::DT_INT64),
                    OPS_LOG_E(nodeName, "localEpTokenCnt datatype is invalid, datatype should be int64, but is %d.",
                            static_cast<ge::DataType>(localEpTokenCnt->GetDataType())),
                    return false);

    auto srcOffsetRankTokenIdx = context.GetOutputDesc(OUTPUT_SRC_OFFSET_RANK_TOKEN_INDEX);
    OPS_ERR_IF(srcOffsetRankTokenIdx == nullptr, OPS_LOG_E(nodeName, "srcOffsetRankTokenIdx is null."),
                    return false);
    OPS_ERR_IF(
        (srcOffsetRankTokenIdx->GetDataType() != ge::DT_BF16) &&
            (srcOffsetRankTokenIdx->GetDataType() != ge::DT_FLOAT16) &&
            (srcOffsetRankTokenIdx->GetDataType() != ge::DT_FLOAT) &&
            (srcOffsetRankTokenIdx->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(
            nodeName,
            "srcOffsetRankTokenIdx datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
            static_cast<ge::DataType>(srcOffsetRankTokenIdx->GetDataType())),
        return false);

    auto dstOffsetRankTokenIdx = context.GetOutputDesc(OUTPUT_DST_OFFSET_RANK_TOKEN_INDEX);
    OPS_ERR_IF(dstOffsetRankTokenIdx == nullptr, OPS_LOG_E(nodeName, "dstOffsetRankTokenIdx is null."),
                    return false);
    OPS_ERR_IF(
        (dstOffsetRankTokenIdx->GetDataType() != ge::DT_BF16) &&
            (dstOffsetRankTokenIdx->GetDataType() != ge::DT_FLOAT16) &&
            (dstOffsetRankTokenIdx->GetDataType() != ge::DT_FLOAT) &&
            (dstOffsetRankTokenIdx->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(
            nodeName,
            "dstOffsetRankTokenIdx datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
            static_cast<ge::DataType>(dstOffsetRankTokenIdx->GetDataType())),
        return false);

    auto offsetInner = context.GetOutputDesc(OUTPUT_OFFSET_INNER_INDEX);
    OPS_ERR_IF(offsetInner == nullptr, OPS_LOG_E(nodeName, "offsetInner is null."), return false);
    OPS_ERR_IF(
        (offsetInner->GetDataType() != ge::DT_BF16) && (offsetInner->GetDataType() != ge::DT_FLOAT16) &&
            (offsetInner->GetDataType() != ge::DT_FLOAT) && (offsetInner->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "offsetInner datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(offsetInner->GetDataType())),
        return false);

    auto countOuter = context.GetOutputDesc(OUTPUT_COUNT_OUTER_INDEX);
    OPS_ERR_IF(countOuter == nullptr, OPS_LOG_E(nodeName, "countOuter is null."), return false);
    OPS_ERR_IF(
        (countOuter->GetDataType() != ge::DT_BF16) && (countOuter->GetDataType() != ge::DT_FLOAT16) &&
            (countOuter->GetDataType() != ge::DT_FLOAT) && (countOuter->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "countOuter datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(countOuter->GetDataType())),
        return false);

    auto expandIdx = context.GetOutputDesc(OUTPUT_EXPAND_IDX_INDEX);
    OPS_ERR_IF(expandIdx == nullptr, OPS_LOG_E(nodeName, "expandIdx is null."), return false);
    OPS_ERR_IF(
        (expandIdx->GetDataType() != ge::DT_BF16) && (expandIdx->GetDataType() != ge::DT_FLOAT16) &&
            (expandIdx->GetDataType() != ge::DT_FLOAT) && (expandIdx->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "expandIdx datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(expandIdx->GetDataType())),
        return false);

    auto totalRecvTokens = context.GetOutputDesc(OUTPUT_TOTAL_RECV_TOKENS_INDEX);
    OPS_ERR_IF(totalRecvTokens == nullptr, OPS_LOG_E(nodeName, "totalRecvTokens is null."), return false);
    OPS_ERR_IF(
        (totalRecvTokens->GetDataType() != ge::DT_BF16) && (totalRecvTokens->GetDataType() != ge::DT_FLOAT16) &&
            (totalRecvTokens->GetDataType() != ge::DT_FLOAT) && (totalRecvTokens->GetDataType() != ge::DT_INT32),
        OPS_LOG_E(nodeName,
                "totalRecvTokens datatype is invalid, datatype should be bf16 or float16 or float or int, but is %d.",
                static_cast<ge::DataType>(totalRecvTokens->GetDataType())),
        return false);

    // Verify the size of the win area
    NotifyDispatchA5TilingData *tilingData = context.GetTilingData<NotifyDispatchA5TilingData>();
    OPS_ERR_IF(tilingData == nullptr, OPS_LOG_E(nodeName, "tilingData is nullptr."), return false);
    uint64_t maxWindowSize = Mc2TilingUtils::GetMaxWindowSize();
    uint64_t actualSize = 2 * dataSize * tilingData->notifyDispatchInfoA5.sendCount + 2 * 1024 * 1024;  // 2MB flag位
    if (actualSize > maxWindowSize) {
        OPS_LOG_E(nodeName, "HCCL_BUFFSIZE is too SMALL, should larger than %luMB", actualSize / MB_SIZE);
        return false;
    }
    return true;
}

static ge::graphStatus TilingCheckTensor(gert::TilingContext &context, const char *nodeName)
{
    OPS_ERR_IF(!CheckTensorDataType(context, nodeName), OPS_LOG_E(nodeName, "params dataType is invalid."),
                    return ge::GRAPH_FAILED);

    return ge::GRAPH_SUCCESS;
}

static ge::graphStatus NotifyDispatchA5TilingFuncImpl(gert::TilingContext &context)
{
    const char *nodeName = context.GetNodeName();
    OPS_ERR_IF(nodeName == nullptr, OPS_LOG_E("unKnownNodeName", "nodeName is nullptr."), return ge::GRAPH_FAILED);
    OPS_LOG_D(nodeName, "Enter NotifyDispatchA5TilingFuncImpl.");
    NotifyDispatchA5TilingData *tilingData = context.GetTilingData<NotifyDispatchA5TilingData>();
    OPS_ERR_IF(tilingData == nullptr, OPS_LOG_E(nodeName, "tilingData is nullptr."), return ge::GRAPH_FAILED);
    std::string commGroup = "";
    OPS_LOG_I(nodeName, "Enter NotifyDispatchA5 tiling check func.");

    OPS_ERR_IF(GetAttrAndSetTilingData(context, nodeName, *tilingData, commGroup) != ge::GRAPH_SUCCESS,
                    OPS_LOG_E(nodeName, "Get attr and set tiling data failed."), return ge::GRAPH_FAILED);

    OPS_ERR_IF(TilingCheckTensor(context, nodeName) != ge::GRAPH_SUCCESS,
                    OPS_LOG_E(nodeName, "Tiling check param failed."), return ge::GRAPH_FAILED);

    OPS_ERR_IF(SetWorkSpace(context, nodeName) != ge::GRAPH_SUCCESS,
                    OPS_LOG_E(nodeName, "Tiling set workspace failed."), return ge::GRAPH_FAILED);
    SetHcommCfg(context, *tilingData, commGroup);

    std::string hcclIntraPcieEnableStr;
    std::string hcclIntraRoceEnableStr;
    const char *hcclIntraPcieEnable = getenv("HCCL_INTRA_PCIE_ENABLE");
    if (hcclIntraPcieEnable != nullptr) {
        hcclIntraPcieEnableStr = hcclIntraPcieEnable;
    }
    const char *hcclIntraRoceEnable = getenv("HCCL_INTRA_ROCE_ENABLE");
    if (hcclIntraRoceEnable != nullptr) {
        hcclIntraRoceEnableStr = hcclIntraRoceEnable;
    }

    OPS_ERR_IF(hcclIntraPcieEnableStr.empty() || hcclIntraRoceEnableStr.empty(),
                    OPS_LOG_E(nodeName, "Please set ENV HCCL_INTRA_PCIE_ENABLE = 1 and HCCL_INTRA_ROCE_ENABLE = 0"),
                    return ge::GRAPH_FAILED);
    OPS_ERR_IF(hcclIntraPcieEnableStr != "1" || hcclIntraRoceEnableStr != "0",
                    OPS_LOG_E(nodeName, "Need set ENV HCCL_INTRA_PCIE_ENABLE = 1 and HCCL_INTRA_ROCE_ENABLE = 0"),
                    return ge::GRAPH_FAILED);

    int tilingKey = TILING_KEY_INT;
    OPS_ERR_IF(context.GetInputDesc(0) == nullptr, OPS_LOG_E(nodeName, "input 0 is nullptr."),
        return ge::GRAPH_FAILED);

    if (CheckIfA5MultiMachine(context, nodeName, *tilingData)) {
        tilingKey = tilingKey + TILING_KEY_A5_TYPE;
    }
    context.SetTilingKey(tilingKey);

    auto ascendcPlatform = platform_ascendc::PlatformAscendC(context.GetPlatformInfo());
    uint32_t blockDim;
    uint32_t aivNum = ascendcPlatform.GetCoreNumAiv();
    uint64_t ubSize = 0UL;
    ascendcPlatform.GetCoreMemSize(platform_ascendc::CoreMemType::UB, ubSize);

    blockDim = aivNum;
    context.SetBlockDim(blockDim);
    tilingData->notifyDispatchInfoA5.totalUbSize = ubSize;
    tilingData->notifyDispatchInfoA5.aivNum = aivNum;
    OPS_LOG_D(nodeName, "blockDim=%u, aivNum=%u, ubSize=%lu", blockDim, aivNum, ubSize);
    PrintTilingDataInfo(nodeName, *tilingData);
    return ge::GRAPH_SUCCESS;
}

static ge::graphStatus NotifyDispatchA5TilingFunc(gert::TilingContext *context)
{
    ge::graphStatus ret = NotifyDispatchA5TilingFuncImpl(*context);
    return ret;
}

struct NotifyDispatchA5CompileInfo {};
ge::graphStatus TilingParseForNotifyDispatchA5(gert::TilingParseContext *context)
{
    (void)context;
    return ge::GRAPH_SUCCESS;
}

IMPL_OP_OPTILING(NotifyDispatchA5)
    .Tiling(NotifyDispatchA5TilingFunc)
    .TilingParse<NotifyDispatchA5CompileInfo>(TilingParseForNotifyDispatchA5);
}  // namespace optiling
