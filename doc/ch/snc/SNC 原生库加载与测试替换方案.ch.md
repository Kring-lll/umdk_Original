# SNC 原生库测试替换方案

> 适用范围：`src/snc` 模块。用于测试人员拿到 jar 包后，用外部原生库（`.so` / `.dll`）替换打包版本运行测试。

---

## 1. 场景与目标

开发提交代码后，CI / 测试人员拿到 `snc-1.0.0.jar`，需要用**新版本原生库**（`libubswitch*.so` / `libubswitch.dll`）运行测试，而非重新解包改 jar。

**目标**：测试人员只需把新 `.so` 放到一个外部目录，运行测试时通过一个参数指向该目录，即可让测试加载外部 `.so`，绕过 jar 内 / resources 内的打包版本。

---

## 2. 原生库加载机制

### 2.1 加载链路

```
HashUtils 静态初始化块
   └─> DllLoader.load(NATIVE_LIBRARY_NAME, UbSwitchLibrary.class)
         └─> findDllPath(name)   // 按搜索目录顺序找文件，命中即用绝对路径加载
               └─> buildSearchPaths()  // 返回候选目录列表
```

- 入口：`src/snc/src/main/java/com/huawei/umdk/snc/util/HashUtils.java:26`
- 加载器：`src/snc/src/main/java/com/huawei/umdk/snc/util/DllLoader.java:71`

### 2.2 平台选择

`HashUtils.detectNativeLibraryName()`（`HashUtils.java:55`）按 `os.name` / `os.arch` 选择：

| 平台 | 文件名 |
|------|--------|
| Windows | `libubswitch.dll` |
| Linux x86_64 | `libubswitch-x86_64.so` |
| Linux AArch64 | `libubswitch-aarch64.so` |

### 2.3 搜索目录顺序（本次实现后）

`DllLoader.buildSearchPaths()`（`DllLoader.java:154`）返回的候选目录，按优先级从高到低：

1. **`jna.library.path` 系统属性指向的目录** ← 外部替换入口
2. `src/main/resources`
3. `user.dir`（当前工作目录）
4. 应用目录（jar / classes 所在目录）
5. `target`
6. `target/classes`

命中任一目录即用绝对路径调 `Native.load(absolutePath, ...)` 加载；全部未命中则从 classpath（含 jar 内）提取到 temp 目录加载；最后兜底用裸库名让 JNA 自解析。

### 2.4 Maven 属性桥接

`src/snc/pom.xml` 中：

```xml
<properties>
    <native.lib.dir>${project.basedir}/src/main/resources</native.lib.dir>
</properties>

<plugin>
    <artifactId>maven-surefire-plugin</artifactId>
    <configuration>
        <systemPropertyVariables>
            <jna.library.path>${native.lib.dir}</jna.library.path>
        </systemPropertyVariables>
    </configuration>
</plugin>
```

`-Dnative.lib.dir=...` 是 Maven 属性，surefire 把它桥接为 JVM 系统属性 `jna.library.path`。`DllLoader` 读 `jna.library.path` 即可同时覆盖 Maven 与非 Maven 两种运行方式。

---

## 3. 实现

### 3.1 代码改动

**文件**：`src/snc/src/main/java/com/huawei/umdk/snc/util/DllLoader.java`

**方法**：`buildSearchPaths()`

**改动**：在候选目录列表最前插入 `jna.library.path` 系统属性的读取，使其成为最高优先级的外部覆盖入口。

```java
private static List<String> buildSearchPaths() {
    List<String> paths = new ArrayList<>();
    String jnaPath = System.getProperty("jna.library.path");
    if (jnaPath != null && !jnaPath.isEmpty()) {
        paths.add(jnaPath);
    }
    paths.add("src/main/resources");
    paths.add(System.getProperty("user.dir"));
    paths.add(getApplicationDir());
    paths.add("target");
    paths.add("target/classes");
    return paths;
}
```

同步更新了 `DllLoader` 类 Javadoc 的搜索顺序说明。

### 3.2 pom.xml

无需改动。已有的 `native.lib.dir` 属性与 surefire `systemPropertyVariables` 桥接正好支撑本方案。

### 3.3 改动前后对比

| 场景 | 改动前 | 改动后 |
|------|--------|--------|
| `mvn test -Dnative.lib.dir=/external/libs` | `jna.library.path` 被设为外部目录，但 `DllLoader` 不读它，仍命中 `src/main/resources` 或 `target/classes` 的旧 `.so` | `DllLoader` 优先搜索 `jna.library.path`，命中外部目录的新 `.so` |
| 无外部覆盖（默认 `mvn test`） | 行为不变 | 行为不变（`jna.library.path` 默认指向 `src/main/resources`，与原首条路径一致） |

---

## 4. 构建与测试操作步骤

### 4.1 构建 jar（CI / 开发人员）

流水线分两步：先测试（验证 Java 代码与基线原生库集成），再打包（跳过测试避免重复执行）。

```bash
cd ${WORKSPACE}/umdk/src/snc
mvn test                    # 用基线 .so 跑测试（构建机架构自动匹配）
mvn package -DskipTests     # 打包，跳过测试避免二次执行
```

产物：`target/snc-1.0.0.jar`，内含基线版本原生库。

> **为何第二步要 `-DskipTests`**：`mvn package` 默认会再次执行 test 阶段，导致测试跑两遍。第一步 `mvn test` 已完成验证，第二步加 `-DskipTests` 直接复用已编译产物打包，避免重复测试与 jacoco 重复计算。
>
> **为何基线测试在任意架构构建机都能跑**：`src/main/resources` 同时打包了 `libubswitch.dll`、`libubswitch-x86_64.so`、`libubswitch-aarch64.so` 三种架构版本，`HashUtils.detectNativeLibraryName()` 按构建机 `os.arch` 自动选择匹配版本，不会出现 `dlopen` 架构不匹配。
>
> **若流水线不需要构建期测试**：可省去第一步，直接 `mvn package -DskipTests`。
>
> **替换 .so 的专项测试**：见 4.3，由测试人员单独执行，与流水线解耦。

### 4.2 准备外部原生库目录

测试人员拿到 jar 后，在任意目录放入待测原生库，文件名必须与平台匹配（见 2.2）：

```
/home/tester/native-libs/
└── libubswitch-x86_64.so      # Linux x86_64
```

或 Windows：

```
D:\tmp\native-libs\
└── libubswitch.dll
```

### 4.3 运行测试（替换 .so）

在 snc 源码目录下执行（需要源码与 test 目录，jar 仅作为依赖载体）：

```bash
cd ${WORKSPACE}/umdk/src/snc

# Linux
mvn test -Dnative.lib.dir=/home/tester/native-libs

# Windows
mvn test -Dnative.lib.dir=D:/tmp/native-libs
```

surefire 会把 `native.lib.dir` 桥接为 `jna.library.path` 系统属性，`DllLoader` 优先从该目录加载 `.so`。

### 4.4 只跑触发原生库的测试

```bash
mvn test -Dnative.lib.dir=/home/tester/native-libs -Dtest=HashCouplingTest
```

### 4.5 验证加载来源

`HashUtils` 静态块加载失败会打印 `[HashUtils] Failed to load native library ...` 到 stderr。加载成功无输出。若需确认加载的具体路径，可在 `HashUtils.java:36` 临时加一行：

```java
System.err.println("[HashUtils] jna.library.path=" + System.getProperty("jna.library.path"));
```

该值应指向 4.2 的外部目录。

---

## 5. 注意事项

1. **架构匹配**：外部 `.so` 必须与测试机 `os.arch` 一致，否则 `dlopen` 失败，`HashUtils.LIB` 为 `null`，`nativeHash` 抛 `IllegalStateException`。
2. **JNA 库缓存**：同一 JVM 内同库名只加载一次。`-Dnative.lib.dir` 必须在 JVM 启动期（surefire 注入）就设好，运行时再改对已加载库无效。
3. **文件名严格匹配**：外部目录里的文件名必须与 `detectNativeLibraryName()` 返回值完全一致（如 `libubswitch-x86_64.so`，不是 `libubswitch.so`）。
4. **不改 jar**：本方案不需要解包 / 重新打包 jar。jar 内 `.so` 保持基线版本，外部 `.so` 仅用于本次测试运行。
5. **默认行为不变**：不传 `-Dnative.lib.dir` 时，`jna.library.path` 默认指向 `src/main/resources`，与改动前首条搜索路径一致，向后兼容。

---

## 6. 变更清单

| 文件 | 变更 |
|------|------|
| `src/snc/src/main/java/com/huawei/umdk/snc/util/DllLoader.java` | `buildSearchPaths()` 开头读取 `jna.library.path` 并插入候选目录最前；同步更新类 Javadoc 搜索顺序 |
| `src/snc/pom.xml` | 无需改动 |
| `src/snc/src/main/resources/` | 无需改动 |
