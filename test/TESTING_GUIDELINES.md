# UMDK Testing Guidelines
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

## 1. Test Naming Convention

### 1.1 Test Class Naming
- Use descriptive names: `<Module>Test`, `<Feature>Test`
- Example: `BoundaryTest`, `EdgeCaseTest`, `ConcurrentTest`

### 1.2 Test Case Naming
Follow pattern: `<function>_<scenario>_<expected_result>`
- Example: `test_bitmap_boundary_min_size`
- Example: `test_umq_log_config_null_pointer_should_return_error`

### 1.3 Naming Categories
- `boundary`: Boundary value tests
- `edge_case`: Edge case and error handling tests
- `concurrent`: Concurrency and thread safety tests
- `performance`: Performance benchmark tests
- `integration`: Integration tests

## 2. Test Coverage Requirements

### 2.1 Thresholds
| Metric | Threshold |
|--------|-----------|
| Line Coverage | >= 70% |
| Branch Coverage | >= 60% |
| Function Coverage | >= 80% |

### 2.2 Module-specific Thresholds
| Module | Line Coverage |
|--------|---------------|
| URPC | >= 70% |
| URMA | >= 60% |
| DLock | >= 65% |
| UMS | >= 60% |
| CAM | >= 50% |

## 3. Test Categories

### 3.1 Unit Tests
- Location: `test/<module>/`
- Framework: GTest + MockCpp
- Run: `./test/<module>/umdk_<module>_ut.sh`

### 3.2 Integration Tests
- Location: `test/intergration_test/test_suites/`
- Framework: pytest + fabric
- Run: `pytest test/intergration_test/test_suites/<suite>/test.py`

### 3.3 Boundary Tests
- Test minimum/maximum values
- Test zero and negative values
- Test overflow conditions

### 3.4 Edge Case Tests
- Test null pointers
- Test empty inputs
- Test invalid parameters

### 3.5 Concurrent Tests
- Test thread safety
- Test race conditions
- Test resource contention

## 4. Assertion Guidelines

### 4.1 Use Appropriate Assertions
- `ASSERT_*`: Fatal assertions (stop on failure)
- `EXPECT_*`: Non-fatal assertions (continue on failure)

### 4.2 Common Assertions
```cpp
ASSERT_EQ(expected, actual);    // Equality
ASSERT_NE(val1, val2);          // Inequality
ASSERT_TRUE(condition);         // Boolean true
ASSERT_FALSE(condition);        // Boolean false
ASSERT_NULL(ptr);               // Null pointer
ASSERT_NOT_NULL(ptr);           // Non-null pointer
ASSERT_LT(val1, val2);          // Less than
ASSERT_LE(val1, val2);          // Less or equal
ASSERT_GT(val1, val2);          // Greater than
ASSERT_GE(val1, val2);          // Greater or equal
ASSERT_STREQ(str1, str2);       // String equality
```

## 5. Test Fixture Usage

### 5.1 Setup/TearDown
```cpp
class MyTest : public ::testing::Test {
public:
    void SetUp() override {
        // Per-test setup
    }
    
    void TearDown() override {
        // Per-test cleanup
        GlobalMockObject::verify();
    }
    
    static void SetUpTestCase() {
        // One-time setup for all tests
    }
    
    static void TearDownTestCase() {
        // One-time cleanup for all tests
    }
};
```

## 6. Mock Usage

### 6.1 Basic Mocking
```cpp
MOCKER(function_name).stubs().will(returnValue(value));
MOCKER(function_name).expects(once()).will(returnValue(value));
```

### 6.2 Mock Cleanup
Always call `GlobalMockObject::verify()` in TearDown

## 7. Running Tests

### 7.1 Run All Tests
```bash
cd test
./run_all_tests.sh
```

### 7.2 Run Specific Module
```bash
cd test/urpc
./umdk_urpc_ut.sh
```

### 7.3 Run with Coverage
```bash
cd test/urpc
./umdk_urpc_ut.sh --coverage
```

### 7.4 Check Coverage Threshold
```bash
cd test
./check_coverage.sh
```

### 7.5 Check Environment
```bash
cd test
./check_env.sh
```

## 8. CI/CD Integration

Tests are automatically run via GitHub Actions:
- On push to main/master/develop branches
- On pull requests
- Coverage reports are generated and uploaded

## 9. Best Practices

1. **Test Independence**: Each test should be independent
2. **Clear Intent**: Test name should describe what is being tested
3. **Complete Coverage**: Cover normal, boundary, and error cases
4. **Resource Cleanup**: Always cleanup resources in TearDown
5. **Thread Safety**: Use proper synchronization in concurrent tests
6. **Documentation**: Add comments explaining test purpose