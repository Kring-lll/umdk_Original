package com.huawei.umdk.snc.exception;

public class SNCException extends RuntimeException {

    public SNCException() {
        super();
    }

    public SNCException(String message) {
        super(message);
    }

    public SNCException(String message, Throwable cause) {
        super(message, cause);
    }
}
