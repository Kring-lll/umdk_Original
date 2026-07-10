package com.huawei.umdk.snc.dto;

import java.util.Map;

import lombok.AllArgsConstructor;
import lombok.EqualsAndHashCode;
import lombok.Getter;
import lombok.NoArgsConstructor;
import lombok.Setter;
import lombok.ToString;

@Getter
@Setter
@NoArgsConstructor
@AllArgsConstructor
@EqualsAndHashCode
@ToString
public class PathPlanRequest {
    private String superNodeName;
    private String srcPort;
    private String destPort;
    private String srcDevice;
    private String destDevice;
    private Map<String, String> interDevices;
}
