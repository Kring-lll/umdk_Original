/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * CAM shim for ops-transformer `#include "err/ops_err.h"`.
 * Maps OP_* logging macros onto CAM ops_error.h / ops_log.h.
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

#ifndef OP_CHECK_IF
#define OP_CHECK_IF(condition, log, return_expr)                                                                       \
    do {                                                                                                               \
        if (condition) {                                                                                               \
            log;                                                                                                       \
            return_expr;                                                                                               \
        }                                                                                                              \
    } while (0)
#endif

#ifndef OP_CHECK_NULL_WITH_CONTEXT
#define OP_CHECK_NULL_WITH_CONTEXT(context, ptr)                                                                       \
    do {                                                                                                               \
        if ((ptr) == nullptr) {                                                                                        \
            const char *name =                                                                                         \
                (((context) == nullptr) || (context)->GetNodeName() == nullptr) ? "nil" : (context)->GetNodeName();    \
            OP_LOGE(name, "%s is nullptr!", #ptr);                                                                     \
            return ge::GRAPH_FAILED;                                                                                   \
        }                                                                                                              \
    } while (0)
#endif

#ifndef OP_LOGE
#define OP_LOGE(opName, ...) OPS_LOG_E(opName, __VA_ARGS__)
#endif

#ifndef OP_LOGI
#define OP_LOGI(opName, ...) OPS_LOG_I(opName, __VA_ARGS__)
#endif

#ifndef OP_LOGW
#define OP_LOGW(opName, ...) OPS_LOG_W(opName, __VA_ARGS__)
#endif

#ifndef OP_LOGE_FOR_INVALID_SHAPE
#define OP_LOGE_FOR_INVALID_SHAPE(opName, paramName, incorrectShape, correctShape)                                     \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _param(paramName);                                                                                 \
        std::string _got(incorrectShape);                                                                              \
        std::string _expect(correctShape);                                                                             \
        OPS_LOG_E(_op.c_str(), "Parameter %s has incorrect shape %s. It should be %s.", _param.c_str(), _got.c_str(),  \
                  _expect.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_SHAPE_WITH_REASON
#define OP_LOGE_FOR_INVALID_SHAPE_WITH_REASON(opName, paramName, incorrectShape, reason)                               \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _param(paramName);                                                                                 \
        std::string _got(incorrectShape);                                                                              \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameter %s has incorrect shape %s. Reason: %s.", _param.c_str(), _got.c_str(),       \
                  _reason.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_SHAPES_WITH_REASON
#define OP_LOGE_FOR_INVALID_SHAPES_WITH_REASON(opName, paramNames, incorrectShapes, reason)                             \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _params(paramNames);                                                                               \
        std::string _got(incorrectShapes);                                                                             \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameters %s have incorrect shapes %s. Reason: %s.", _params.c_str(), _got.c_str(),   \
                  _reason.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_SHAPEDIM_WITH_REASON
#define OP_LOGE_FOR_INVALID_SHAPEDIM_WITH_REASON(opName, paramName, incorrectDim, reason)                               \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _param(paramName);                                                                                 \
        std::string _got(incorrectDim);                                                                                \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameter %s has incorrect dim %s. Reason: %s.", _param.c_str(), _got.c_str(),         \
                  _reason.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_SHAPESIZE_WITH_REASON
#define OP_LOGE_FOR_INVALID_SHAPESIZE_WITH_REASON(opName, paramName, incorrectSize, reason)                             \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _param(paramName);                                                                                 \
        std::string _got(incorrectSize);                                                                               \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameter %s has incorrect shape size %s. Reason: %s.", _param.c_str(), _got.c_str(),  \
                  _reason.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_DTYPE_WITH_REASON
#define OP_LOGE_FOR_INVALID_DTYPE_WITH_REASON(opName, paramName, incorrectDtype, reason)                                \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _param(paramName);                                                                                 \
        std::string _got(incorrectDtype);                                                                              \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameter %s has incorrect dtype %s. Reason: %s.", _param.c_str(), _got.c_str(),       \
                  _reason.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_DTYPES_WITH_REASON
#define OP_LOGE_FOR_INVALID_DTYPES_WITH_REASON(opName, paramNames, incorrectDtypes, reason)                             \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _params(paramNames);                                                                               \
        std::string _got(incorrectDtypes);                                                                             \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameters %s have incorrect dtypes %s. Reason: %s.", _params.c_str(), _got.c_str(),   \
                  _reason.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_VALUE
#define OP_LOGE_FOR_INVALID_VALUE(opName, paramName, incorrectValue, correctValue)                                      \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _param(paramName);                                                                                 \
        std::string _got(incorrectValue);                                                                              \
        std::string _expect(correctValue);                                                                             \
        OPS_LOG_E(_op.c_str(), "Parameter %s has incorrect value %s. It should be %s.", _param.c_str(), _got.c_str(),  \
                  _expect.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_VALUE_WITH_REASON
#define OP_LOGE_FOR_INVALID_VALUE_WITH_REASON(opName, paramName, incorrectValue, reason)                                \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _param(paramName);                                                                                 \
        std::string _got(incorrectValue);                                                                              \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameter %s has incorrect value %s. Reason: %s.", _param.c_str(), _got.c_str(),       \
                  _reason.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_VALUES_WITH_REASON
#define OP_LOGE_FOR_INVALID_VALUES_WITH_REASON(opName, paramNames, incorrectValues, reason)                             \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _params(paramNames);                                                                               \
        std::string _got(incorrectValues);                                                                             \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameters %s have incorrect values %s. Reason: %s.", _params.c_str(), _got.c_str(),   \
                  _reason.c_str());                                                                                    \
    } while (0)
#endif

#ifndef OP_LOGE_FOR_INVALID_ARGUMENT_WITH_REASON
#define OP_LOGE_FOR_INVALID_ARGUMENT_WITH_REASON(opName, paramName, reason)                                            \
    do {                                                                                                               \
        std::string _op(opName);                                                                                       \
        std::string _param(paramName);                                                                                 \
        std::string _reason(reason);                                                                                   \
        OPS_LOG_E(_op.c_str(), "Parameter %s is invalid. Reason: %s.", _param.c_str(), _reason.c_str());               \
    } while (0)
#endif
