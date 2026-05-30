package com.huawei.umdk.snc.engine;

import com.huawei.umdk.snc.entity.*;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Nested;
import org.junit.jupiter.api.Test;

import java.util.*;

import static org.junit.jupiter.api.Assertions.*;

/**
 * RouteLookupEngine 单元测试 - 100% 分支覆盖
 */
@DisplayName("RouteLookupEngine 单元测试")
class RouteLookupEngineTest {

    private RouteLookupEngine engine;
    private RouterTable routerTable;

    @BeforeEach
    void setUp() {
        engine = new RouteLookupEngine();
        routerTable = new RouterTable();
    }

    // ==================== lookup ====================

    @Nested
    @DisplayName("lookup 方法")
    class LookupTest {

        @Test
        @DisplayName("routerTable 为 null 返回 null")
        void testLookupNullRouterTable() {
            assertNull(engine.lookup(null, "0xAAAABBBBCCCCDDDDEEEEFFFF00000001"));
        }

        @Test
        @DisplayName("dstPrefix 为 null 返回 null")
        void testLookupNullDstPrefix() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            RouteEntry entry = new RouteEntry();
            entry.setPrefix("0xAAAA");
            routes.put("0xAAAA", entry);
            routerTable.setRoutes(routes);

            assertNull(engine.lookup(routerTable, null));
        }

        @Test
        @DisplayName("routes 为 null 返回 null")
        void testLookupNullRoutes() {
            routerTable.setRoutes(null);
            assertNull(engine.lookup(routerTable, "0xAAAABBBBCCCCDDDDEEEEFFFF00000001"));
        }

        @Test
        @DisplayName("routes 为空返回 null")
        void testLookupEmptyRoutes() {
            routerTable.setRoutes(new LinkedHashMap<>());
            assertNull(engine.lookup(routerTable, "0xAAAABBBBCCCCDDDDEEEEFFFF00000001"));
        }

        @Test
        @DisplayName("精确前缀匹配返回对应条目（CNA 前缀）")
        void testLookupExactPrefixMatch() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            RouteEntry entry = new RouteEntry();
            entry.setPrefix("0xAAAAAA11");
            entry.setOutPortInfos(Collections.singletonList(
                    createOutPortInfo("400GE 0/0/1", "rack1#la0")
            ));
            routes.put("0xAAAAAA11", entry);
            routerTable.setRoutes(routes);

            RouteEntry result = engine.lookup(routerTable, "0xAAAAAA11000000000000000000000001");
            assertNotNull(result);
            assertEquals("0xAAAAAA11", result.getPrefix());
        }

        @Test
        @DisplayName("最长前缀匹配 - 多个条目选最长（CNA 前缀）")
        void testLookupLongestPrefixMatch() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            
            RouteEntry shortEntry = new RouteEntry();
            shortEntry.setPrefix("0xAAAA");
            shortEntry.setOutPortInfos(Collections.singletonList(
                    createOutPortInfo("400GE 0/0/2", "rack1#la1")
            ));
            routes.put("0xAAAA", shortEntry);
            
            RouteEntry longEntry = new RouteEntry();
            longEntry.setPrefix("0xAAAAAA11");
            longEntry.setOutPortInfos(Collections.singletonList(
                    createOutPortInfo("400GE 0/0/1", "rack1#la0")
            ));
            routes.put("0xAAAAAA11", longEntry);
            
            routerTable.setRoutes(routes);

            RouteEntry result = engine.lookup(routerTable, "0xAAAAAA11000000000000000000000001");
            assertNotNull(result);
            assertEquals("0xAAAAAA11", result.getPrefix());
        }

        @Test
        @DisplayName("无匹配前缀返回 null")
        void testLookupNoMatch() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            RouteEntry entry = new RouteEntry();
            entry.setPrefix("0xBBBBBB11");
            routes.put("0xBBBBBB11", entry);
            routerTable.setRoutes(routes);

            assertNull(engine.lookup(routerTable, "0xAAAAAA11000000000000000000000001"));
        }

        @Test
        @DisplayName("条目 prefix 为 null 时跳过")
        void testLookupNullPrefix() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            RouteEntry entry = new RouteEntry();
            entry.setPrefix(null);
            routes.put("nullPrefix", entry);
            
            RouteEntry validEntry = new RouteEntry();
            validEntry.setPrefix("0xAAAA");
            routes.put("0xAAAA", validEntry);
            
            routerTable.setRoutes(routes);

            RouteEntry result = engine.lookup(routerTable, "0xAAAA0000000000000000000000000001");
            assertNotNull(result);
            assertEquals("0xAAAA", result.getPrefix());
        }

        @Test
        @DisplayName("条目为 null 时跳过")
        void testLookupNullEntry() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            routes.put("nullEntry", null);
            
            RouteEntry validEntry = new RouteEntry();
            validEntry.setPrefix("0xAAAA");
            routes.put("0xAAAA", validEntry);
            
            routerTable.setRoutes(routes);

            RouteEntry result = engine.lookup(routerTable, "0xAAAA0000000000000000000000000001");
            assertNotNull(result);
        }

        @Test
        @DisplayName("小写前缀也能匹配（normalizePrefix）")
        void testLookupLowercasePrefix() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            RouteEntry entry = new RouteEntry();
            entry.setPrefix("0xAAAAAA11");
            routes.put("0xAAAAAA11", entry);
            routerTable.setRoutes(routes);

            RouteEntry result = engine.lookup(routerTable, "0xaaaaaa11000000000000000000000001");
            assertNotNull(result);
            assertEquals("0xAAAAAA11", result.getPrefix());
        }

        @Test
        @DisplayName("同长度多前缀 - 第一个匹配生效（按遍历顺序）")
        void testLookupSameLengthPrefix() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            
            RouteEntry entry1 = new RouteEntry();
            entry1.setPrefix("0xAAAAAA11");
            routes.put("0xAAAAAA11", entry1);
            
            RouteEntry entry2 = new RouteEntry();
            entry2.setPrefix("0xAAAAAA22");
            routes.put("0xAAAAAA22", entry2);
            
            routerTable.setRoutes(routes);

            // "0xAAAAAA11" should match "0xAAAAAA11" prefix (not "0xAAAAAA22")
            RouteEntry result = engine.lookup(routerTable, "0xAAAAAA11000000000000000000000001");
            assertNotNull(result);
            assertEquals("0xAAAAAA11", result.getPrefix());
        }
    }

    // ==================== lookupDefault ====================

    @Nested
    @DisplayName("lookupDefault 方法")
    class LookupDefaultTest {

        @Test
        @DisplayName("routerTable 为 null 返回 null")
        void testLookupDefaultNullRouterTable() {
            assertNull(engine.lookupDefault(null));
        }

        @Test
        @DisplayName("routes 为 null 返回 null")
        void testLookupDefaultNullRoutes() {
            routerTable.setRoutes(null);
            assertNull(engine.lookupDefault(routerTable));
        }

        @Test
        @DisplayName("routes 为空返回 null")
        void testLookupDefaultEmptyRoutes() {
            routerTable.setRoutes(new LinkedHashMap<>());
            assertNull(engine.lookupDefault(routerTable));
        }

        @Test
        @DisplayName("存在空 prefix 条目 - 返回默认路由")
        void testLookupDefaultEmptyPrefix() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            RouteEntry defaultRoute = new RouteEntry();
            defaultRoute.setPrefix("");
            routes.put("default", defaultRoute);
            routerTable.setRoutes(routes);

            RouteEntry result = engine.lookupDefault(routerTable);
            assertNotNull(result);
            assertEquals("", result.getPrefix());
        }

        @Test
        @DisplayName("存在 null prefix 条目 - 返回该条目")
        void testLookupDefaultNullPrefix() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            RouteEntry defaultRoute = new RouteEntry();
            defaultRoute.setPrefix(null);
            routes.put("default", defaultRoute);
            routerTable.setRoutes(routes);

            RouteEntry result = engine.lookupDefault(routerTable);
            assertNotNull(result);
            assertNull(result.getPrefix());
        }

        @Test
        @DisplayName("所有条目都有 prefix - 返回 null")
        void testLookupDefaultNoDefault() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            RouteEntry entry = new RouteEntry();
            entry.setPrefix("0xAAAA");
            routes.put("0xAAAA", entry);
            routerTable.setRoutes(routes);

            assertNull(engine.lookupDefault(routerTable));
        }

        @Test
        @DisplayName("条目为 null 时跳过")
        void testLookupDefaultNullEntry() {
            Map<String, RouteEntry> routes = new LinkedHashMap<>();
            routes.put("nullEntry", null);
            
            RouteEntry defaultRoute = new RouteEntry();
            defaultRoute.setPrefix("");
            routes.put("default", defaultRoute);
            
            routerTable.setRoutes(routes);

            RouteEntry result = engine.lookupDefault(routerTable);
            assertNotNull(result);
            assertEquals("", result.getPrefix());
        }
    }

    /** 辅助创建 OutPortInfo */
    private OutPortInfo createOutPortInfo(String outInterface, String nextHop) {
        OutPortInfo info = new OutPortInfo();
        info.setOutInterface(outInterface);
        info.setNextHop(nextHop);
        return info;
    }
}