package com.huawei.umdk.snc.exception;

import com.huawei.umdk.snc.dto.PathPlanResult.PlanStatus;

public class PathPlanException extends SNCException {

    private final PlanStatus status;

    public PathPlanException(PlanStatus status, String message) {
        super(message);
        this.status = status;
    }

    public PlanStatus getStatus() {
        return status;
    }
}
