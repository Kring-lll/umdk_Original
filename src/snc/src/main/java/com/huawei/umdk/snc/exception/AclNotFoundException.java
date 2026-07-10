package com.huawei.umdk.snc.exception;

public class AclNotFoundException extends SNCException {

    public AclNotFoundException() {
        super();
    }

    public AclNotFoundException(String message) {
        super(message);
    }

    public AclNotFoundException(String message, Throwable cause) {
        super(message, cause);
    }
}
