package com.huawei.umdk.snc.dto;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;

@DisplayName("CoverageRequirement 枚举")
class CoverageRequirementTest {

    @Test
    @DisplayName("枚举值")
    void values() {
        assertEquals(2, CoverageRequirement.values().length);
        assertEquals(CoverageRequirement.MIN_COVERAGE,
            CoverageRequirement.valueOf("MIN_COVERAGE"));
        assertEquals(CoverageRequirement.REDUNDANT,
            CoverageRequirement.valueOf("REDUNDANT"));
    }
}
