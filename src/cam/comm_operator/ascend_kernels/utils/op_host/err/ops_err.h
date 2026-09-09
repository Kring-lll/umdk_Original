/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * CAM shim for ops-transformer `#include "err/ops_err.h"`.
 * Maps OP_* logging helpers onto CAM ops_error.h / ops_log.h.
 */
#pragma once

#ifndef OPS_UTILS_LOG_SUB_MOD_NAME
#define OPS_UTILS_LOG_SUB_MOD_NAME "GATHER_SELECTION_SPARSE_FLASH_ATTENTION"
#endif
#ifndef OPS_UTILS_LOG_PACKAGE_TYPE
#define OPS_UTILS_LOG_PACKAGE_TYPE "[CAM]"
#endif

#include <string>
// Quoted includes search this header's directory first (op_host/err/).
// CAM utils headers live in op_host/, so use a parent-relative path.
#include "../ops_error.h"
#include "../ops_log.h"

// Reuse CAM control-flow helpers (avoid introducing new function-like macros).
#ifndef OP_CHECK_IF
#define OP_CHECK_IF OP_CHECK
#endif

#ifndef OP_LOGE
#define OP_LOGE OPS_LOG_E
#endif

#ifndef OP_LOGI
#define OP_LOGI OPS_LOG_I
#endif

#ifndef OP_LOGW
#define OP_LOGW OPS_LOG_W
#endif

inline void OpLogeForInvalidShape(const char *opName, const char *paramName, const char *incorrectShape,
                                  const char *correctShape)
{
    OPS_LOG_E(opName, "Parameter %s has incorrect shape %s. It should be %s.", paramName, incorrectShape,
              correctShape);
}

inline void OpLogeForInvalidShapeWithReason(const char *opName, const char *paramName, const char *incorrectShape,
                                            const char *reason)
{
    OPS_LOG_E(opName, "Parameter %s has incorrect shape %s. Reason: %s.", paramName, incorrectShape, reason);
}

inline void OpLogeForInvalidShapesWithReason(const char *opName, const char *paramNames, const char *incorrectShapes,
                                             const char *reason)
{
    OPS_LOG_E(opName, "Parameters %s have incorrect shapes %s. Reason: %s.", paramNames, incorrectShapes, reason);
}

inline void OpLogeForInvalidShapedimWithReason(const char *opName, const char *paramName, const char *incorrectDim,
                                               const char *reason)
{
    OPS_LOG_E(opName, "Parameter %s has incorrect dim %s. Reason: %s.", paramName, incorrectDim, reason);
}

inline void OpLogeForInvalidShapesizeWithReason(const char *opName, const char *paramName, const char *incorrectSize,
                                                const char *reason)
{
    OPS_LOG_E(opName, "Parameter %s has incorrect shape size %s. Reason: %s.", paramName, incorrectSize, reason);
}

inline void OpLogeForInvalidDtypeWithReason(const char *opName, const char *paramName, const char *incorrectDtype,
                                            const char *reason)
{
    OPS_LOG_E(opName, "Parameter %s has incorrect dtype %s. Reason: %s.", paramName, incorrectDtype, reason);
}

inline void OpLogeForInvalidDtypesWithReason(const char *opName, const char *paramNames, const char *incorrectDtypes,
                                             const char *reason)
{
    OPS_LOG_E(opName, "Parameters %s have incorrect dtypes %s. Reason: %s.", paramNames, incorrectDtypes, reason);
}

inline void OpLogeForInvalidValue(const char *opName, const char *paramName, const char *incorrectValue,
                                  const char *correctValue)
{
    OPS_LOG_E(opName, "Parameter %s has incorrect value %s. It should be %s.", paramName, incorrectValue,
              correctValue);
}

inline void OpLogeForInvalidValueWithReason(const char *opName, const char *paramName, const char *incorrectValue,
                                            const char *reason)
{
    OPS_LOG_E(opName, "Parameter %s has incorrect value %s. Reason: %s.", paramName, incorrectValue, reason);
}

inline void OpLogeForInvalidValuesWithReason(const char *opName, const char *paramNames, const char *incorrectValues,
                                             const char *reason)
{
    OPS_LOG_E(opName, "Parameters %s have incorrect values %s. Reason: %s.", paramNames, incorrectValues, reason);
}

inline void OpLogeForInvalidArgumentWithReason(const char *opName, const char *paramName, const char *reason)
{
    OPS_LOG_E(opName, "Parameter %s is invalid. Reason: %s.", paramName, reason);
}
