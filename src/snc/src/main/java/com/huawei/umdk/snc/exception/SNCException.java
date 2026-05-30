package com.huawei.umdk.snc.exception;

import com.huawei.umdk.snc.entity.PlanStatus;

/**
 * SNC 业务异常
 */
public class SNCException extends RuntimeException {

    private final PlanStatus status;
    private final int errorCode;

    public SNCException(PlanStatus status) {
        super(status.getMessage());
        this.status = status;
        this.errorCode = status.getCode();
    }

    public SNCException(PlanStatus status, String detail) {
        super(detail != null ? detail : status.getMessage());
        this.status = status;
        this.errorCode = status.getCode();
    }

    public SNCException(PlanStatus status, String detail, Throwable cause) {
        super(detail != null ? detail : status.getMessage(), cause);
        this.status = status;
        this.errorCode = status.getCode();
    }

    public PlanStatus getStatus() {
        return status;
    }

    public int getErrorCode() {
        return errorCode;
    }

    @Override
    public String toString() {
        return "SNCException{" +
               "status=" + status +
               ", errorCode=" + errorCode +
               ", message='" + getMessage() + '\'' +
               '}';
    }
}