/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 门禁式回归测试生成器/比对器（C）。
 *
 * 三模式（同一程序，看 argv）：
 *   gen      <run_output.txt> <expected.txt>          生成基准（从跑测试的 BASELINE 输出）
 *   check    <new_run_output.txt> <expected.txt>      回归比对（文本，不重编，轻量验门禁）
 *   gen_test <test_src.c> <expected.txt> <out.cpp>    生成 gtest 风格 C++ 测试用例（宏→EXPECT，值用基准）
 *
 * 数据格式：
 *   跑测试输出：BASELINE|<file>|<line>|<kind>|<name>|<value>（测试源用宏标注打出）
 *   基准文件：  <kind>|<name>|<value>（NONEMPTY 行 value 省略——只验非空不比值）
 *
 * kind 分语义：
 *   VAL/HEX/STR  逐次必须等于基准（设备无关的协议契约）
 *   NONEMPTY    只验非空不比值（设备相关值：地址/handle，ASLR/递增也不怕）
 *
 * 设计：逐行字符匹配，不解析 AST、不懂语义。生成器极简。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 1024
#define MAX_ENTRIES 256

typedef struct {
    char kind[16];     /* VAL/HEX/STR/NONEMPTY */
    char name[64];
    char value[256];   /* 录制的值（NONEMPTY 时为空） */
} entry_t;

/* 解析一行 BASELINE|file|line|kind|name|value，填 entry。成功 0，非 BASELINE 行 -1。 */
static int parse_line(const char *line, entry_t *e)
{
    if (strncmp(line, "BASELINE|", 9) != 0) return -1;
    const char *p = line + 9;
    /* 跳过 file| */
    p = strchr(p, '|'); if (!p) return -1; p++;
    /* 跳过 line| */
    p = strchr(p, '|'); if (!p) return -1; p++;
    /* kind */
    const char *k = p; p = strchr(p, '|'); if (!p) return -1;
    size_t kn = p - k; if (kn >= sizeof(e->kind)) kn = sizeof(e->kind)-1;
    memcpy(e->kind, k, kn); e->kind[kn] = '\0'; p++;
    /* name */
    const char *n = p; p = strchr(p, '|'); if (!p) return -1;
    size_t nn = p - n; if (nn >= sizeof(e->name)) nn = sizeof(e->name)-1;
    memcpy(e->name, n, nn); e->name[nn] = '\0'; p++;
    /* value（到行尾，去换行） */
    strncpy(e->value, p, sizeof(e->value)-1);
    e->value[sizeof(e->value)-1] = '\0';
    char *nl = strchr(e->value, '\n'); if (nl) *nl = '\0';
    return 0;
}

/* 读 BASELINE 输出文件，解析成 entries 数组。返回条目数，-1 错。 */
static int load_output(const char *path, entry_t *ents, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "open %s fail\n", path); return -1; }
    char line[MAX_LINE]; int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (n >= max) break;
        if (parse_line(line, &ents[n]) == 0) n++;
    }
    fclose(f);
    return n;
}

/* 加载 expected.txt（kind|name|value 格式，无 BASELINE 前缀）。返回条目数，-1 错。 */
static int load_expected(const char *path, entry_t *ents, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "open %s fail\n", path); return -1; }
    char line[MAX_LINE]; int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        char *k = line, *p = strchr(k, '|'); if (!p) continue; *p = '\0';
        char *name = p+1; p = strchr(name, '|'); if (!p) continue; *p = '\0';
        char *val = p+1; char *nl = strchr(val, '\n'); if (nl) *nl = '\0';
        strncpy(ents[n].kind, k, sizeof(ents[n].kind)-1);
        strncpy(ents[n].name, name, sizeof(ents[n].name)-1);
        strncpy(ents[n].value, val, sizeof(ents[n].value)-1);
        n++;
    }
    fclose(f);
    return n;
}

/* 生成基准：写 expected.txt。VAL/HEX/STR 存 name=value，NONEMPTY 只存 name。 */
static int do_gen(const char *out_path, const char *exp_path)
{
    entry_t ents[MAX_ENTRIES];
    int n = load_output(out_path, ents, MAX_ENTRIES);
    if (n < 0) return 1;
    FILE *f = fopen(exp_path, "w");
    if (!f) { fprintf(stderr, "write %s fail\n", exp_path); return 1; }
    for (int i = 0; i < n; i++) {
        if (strcmp(ents[i].kind, "NONEMPTY") == 0) {
            fprintf(f, "NONEMPTY|%s|\n", ents[i].name);
        } else {
            fprintf(f, "%s|%s|%s\n", ents[i].kind, ents[i].name, ents[i].value);
        }
    }
    fclose(f);
    printf("gen: %d entries → %s\n", n, exp_path);
    return 0;
}

/* 比对：读新输出 + expected.txt，逐条比。 */
static int do_check(const char *out_path, const char *exp_path)
{
    entry_t actual[MAX_ENTRIES];
    int na = load_output(out_path, actual, MAX_ENTRIES);
    if (na < 0) return 1;
    entry_t expected[MAX_ENTRIES];
    int ne = load_expected(exp_path, expected, MAX_ENTRIES);
    if (ne < 0) return 1;
    int pass = 0, fail = 0;
    /* expected 和 actual 按名字配对比（顺序无关，按 name 找） */
    for (int i = 0; i < ne; i++) {
        int found = -1;
        for (int j = 0; j < na; j++) {
            if (strcmp(expected[i].name, actual[j].name) == 0) { found = j; break; }
        }
        if (found < 0) {
            printf("FAIL [%s] %s: 缺失（基准要求此条，实际未输出）\n", expected[i].kind, expected[i].name);
            fail++; continue;
        }
        if (strcmp(expected[i].kind, "NONEMPTY") == 0) {
            /* 只验非空：actual 的 value 不是空指针表示（"0x0"/"(nil)"/"" 算空） */
            const char *v = actual[found].value;
            if (v[0] == '\0' || strcmp(v, "(nil)") == 0 || strcmp(v, "0x0") == 0 ||
                strcmp(v, "(null)") == 0) {
                printf("FAIL [NONEMPTY] %s: 实际为空 (%s)\n", expected[i].name, v);
                fail++;
            } else {
                printf("PASS [NONEMPTY] %s=%s\n", expected[i].name, v);
                pass++;
            }
        } else {
            /* VAL/HEX/STR：值必须相等 */
            if (strcmp(expected[i].value, actual[found].value) != 0) {
                printf("FAIL [%s] %s: 期望=%s 实际=%s\n", expected[i].kind,
                       expected[i].name, expected[i].value, actual[found].value);
                fail++;
            } else {
                printf("PASS [%s] %s=%s\n", expected[i].kind, expected[i].name, actual[found].value);
                pass++;
            }
        }
    }
    /* actual 里多出来的（基准没要求的）只警告不 FAIL */
    for (int j = 0; j < na; j++) {
        int found = 0;
        for (int i = 0; i < ne; i++) {
            if (strcmp(expected[i].name, actual[j].name) == 0) { found = 1; break; }
        }
        if (!found) printf("WARN [%s] %s: 基准无此条（新输出多出，建议重生成基准）\n",
                           actual[j].kind, actual[j].name);
    }
    printf("\n%d passed, %d failed\n", pass, fail);
    printf("BASELINE %s\n", fail == 0 ? "PASS" : "FAIL");
    return fail == 0 ? 0 : 1;
}

/* 从宏调用行抽 name（第一个 "..."）和 var（逗号后到 ')' 或 ';'）。
 * 如 WRITE_VAL("post_ret", ret) → name="post_ret", var="ret"。
 * 成功 0，失败 -1。保守实现：用 strchr 逐段，避免越界。 */
static int extract_macro_args(const char *line, const char *macro,
                              char *name, int name_sz, char *var, int var_sz)
{
    if (!line || !macro || !name || !var || name_sz <= 0 || var_sz <= 0) return -1;
    name[0] = '\0'; var[0] = '\0';
    const char *p = strstr(line, macro);
    if (!p) return -1;
    p += strlen(macro);
    /* 跳空白，找 ( */
    p += strspn(p, " \t");
    if (*p != '(') return -1;
    p++;
    /* 找第一个 " */
    p += strspn(p, " \t");
    if (*p != '"') return -1;
    p++;
    const char *nstart = p;
    const char *nend = strchr(p, '"');
    if (!nend) return -1;
    int nlen = nend - nstart;
    if (nlen >= name_sz) nlen = name_sz - 1;
    memcpy(name, nstart, nlen); name[nlen] = '\0';
    p = nend + 1;
    /* 找逗号 */
    p += strspn(p, " \t");
    if (*p != ',') return -1;
    p++;
    p += strspn(p, " \t");
    /* var：配对括号深度找宏调用的闭 )（var 内可能有类型转换的嵌套括号） */
    const char *vstart = p;
    int paren_depth = 1;  /* 已过了宏的 ( ，从 1 开始（name 的引号在前面已消耗）*/
    /* 但 var 在逗号后，宏的 ( 还没闭。所以从 0 开始，遇到 ( ++, 遇到 ) -- 到 -1 是宏的闭 */
    paren_depth = 0;
    const char *vend = NULL;
    for (const char *q = p; *q && *q != '\n'; q++) {
        if (*q == '(') paren_depth++;
        else if (*q == ')') {
            paren_depth--;
            if (paren_depth < 0) { vend = q; break; }  /* 宏的闭 ) */
        }
    }
    if (!vend) vend = p + strlen(p);  /* 无 ) 到行尾 */
    int vlen = vend - vstart;
    if (vlen < 0) vlen = 0;
    if (vlen >= var_sz) vlen = var_sz - 1;
    while (vlen > 0 && (vstart[vlen-1] == ' ' || vstart[vlen-1] == '\t')) vlen--;
    memcpy(var, vstart, vlen); var[vlen] = '\0';
    return 0;
}

/* 按 name 在 expected 里查条目，返回指针，未找到 NULL。 */
static entry_t *find_entry(entry_t *ents, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(ents[i].name, name) == 0) return &ents[i];
    }
    return NULL;
}

/* 生成 gtest 风格 C++ 测试用例（直接用 gtest 库，不自实现宏）。
 * 读 expected.txt（基准值）+ 测试源（提取 main 体，宏行替换成 EXPECT_*），
 * 输出 .cc：#include <gtest/gtest.h> + TEST(BaselineDemo, RecordedRun) { <main体> }。
 * 不含 main（用 gtest_main 库的）。参照 umdk/test/urma/ut/common 的 gtest 风格。 */
static int do_gen_test(const char *src_path, const char *exp_path, const char *out_path)
{
    entry_t ents[MAX_ENTRIES];
    int n = load_expected(exp_path, ents, MAX_ENTRIES);
    if (n < 0) return 1;
    FILE *src = fopen(src_path, "r");
    if (!src) { fprintf(stderr, "open src %s fail\n", src_path); return 1; }
    FILE *out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "write %s fail\n", out_path); fclose(src); return 1; }

    /* 头部：gtest + fixture 头（生成的 .cpp 需要 include 才能编译）*/
    fprintf(out, "/* SPDX-License-Identifier: MIT */\n");
    fprintf(out, "/* 自动生成：gen_baseline gen_test <src.cpp> <expected.txt> <out.cpp>\n");
    fprintf(out, " * 由 baseline 生成器从测试源 + 录制基准合成。改源改基准后重生成，勿手编。 */\n\n");
    fprintf(out, "#include <gtest/gtest.h>\n");
    fprintf(out, "#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n#include <cstdint>\n");
    fprintf(out, "#include \"bl_fixture.hpp\"\n#include \"baseline_macros.h\"\n\n");

    /* 提取测试体：模板用 TEST(...) 或 int main() 均可。
     * 如果是 TEST：保留壳 + 体，宏换 EXPECT。
     * 如果是 int main：包进 TEST(Generated, RecordedRun) 壳，去掉 main/return。
     * 不生成 main（gtest_main 提供）、不加 ok/fprintf。 */
    char line[MAX_LINE];
    int in_test = 0;       /* 0=未进入，1=在体内，-1=找到了待 { */
    int depth = 0;
    int is_main = 0;      /* 模板用 int main 而非 TEST */
    while (fgets(line, sizeof(line), src)) {
        /* 检测 TEST( 或 int main( 开始 */
        if (!in_test && (strstr(line, "TEST(") != NULL || strstr(line, "int main(") != NULL)) {
            is_main = (strstr(line, "int main(") != NULL);
            if (is_main) {
                fprintf(out, "TEST(Generated, RecordedRun)\n{\n");
            }
            int opens = 0, closes = 0;
            for (const char *p = line; *p && *p != '\n'; p++) {
                if (*p == '{') opens++;
                else if (*p == '}') closes++;
            }
            if (opens > 0) {
                in_test = 1;
                depth = opens - closes;  /* main 的 { 算一个 depth，但 is_main 已输出了 { */
                if (is_main) depth = 1;  /* is_main：跳过模板的 {，depth=1 表示已在 TEST 体内 */
                else fputs(line, out);
                continue;
            }
            in_test = -1;
            if (!is_main) fputs(line, out);
            continue;
        }
        if (in_test == -1) {
            int opens = 0, closes = 0;
            for (const char *p = line; *p && *p != '\n'; p++) {
                if (*p == '{') opens++;
                else if (*p == '}') closes++;
            }
            if (opens > 0) {
                in_test = 1;
                depth = opens - closes;
                if (!is_main) fputs(line, out);  /* 保留模板的开 { */
                else depth = 1;  /* is_main：depth=1 已在 TEST 体内 */
                continue;
            }
            if (!is_main) fputs(line, out);
            continue;
        }
        if (in_test == 0) {
            /* TEST 前的 #define 复制进生成测试 */
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "#define ", 8) == 0) fputs(line, out);
            continue;
        }
        /* 在 TEST 体内：数 { } */
        int opens = 0, closes = 0;
        for (const char *p = line; *p && *p != '\n'; p++) {
            if (*p == '{') opens++;
            else if (*p == '}') closes++;
        }
        int newdepth = depth + opens - closes;
        if (newdepth <= 0) {
            /* 闭 } 行：is_main 不输出模板的 }（TEST 的 } 已在 is_main 头输出了对应的 {，
             * 这里补 }），非 is_main 原样输出 */
            if (is_main) fprintf(out, "}\n");
            else fputs(line, out);
            break;
        }
        depth = newdepth;
        /* return 处理：int main 的 return → 删（gtest TEST 是 void）*/
        {
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "return ", 7) == 0 && strchr(p, ';') != NULL) continue;
        }
        /* 宏行替换成 gtest 断言（用录制的基准值） */
        char name[64], var[256];
        if (extract_macro_args(line, "WRITE_VAL", name, sizeof(name), var, sizeof(var)) == 0) {
            entry_t *e = find_entry(ents, n, name);
            if (e) fprintf(out, "    EXPECT_EQ(%s, %s);\n", e->value, var);
            else fprintf(out, "    /* WRITE_VAL(%s) 无基准 */\n", name);
            continue;
        }
        if (extract_macro_args(line, "WRITE_HEX", name, sizeof(name), var, sizeof(var)) == 0) {
            entry_t *e = find_entry(ents, n, name);
            if (e) fprintf(out, "    EXPECT_EQ(%s, %s);\n", e->value, var);
            else fprintf(out, "    /* WRITE_HEX(%s) 无基准 */\n", name);
            continue;
        }
        if (extract_macro_args(line, "ASSERT_NONEMPTY", name, sizeof(name), var, sizeof(var)) == 0) {
            fprintf(out, "    ASSERT_NE(nullptr, %s);\n", var);
            continue;
        }
        if (extract_macro_args(line, "WRITE_STR", name, sizeof(name), var, sizeof(var)) == 0) {
            entry_t *e = find_entry(ents, n, name);
            if (e) fprintf(out, "    EXPECT_EQ(0, strncmp(\"%s\", %s, strlen(\"%s\")));\n", e->value, var, e->value);
            else fprintf(out, "    /* WRITE_STR(%s) 无基准 */\n", name);
            continue;
        }
        /* 普通行原样输出 */
        fputs(line, out);
    }
    fclose(src);
    fclose(out);
    printf("gen_test: %s + %s → %s\n", src_path, exp_path, out_path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage:\n"
                "  %s gen      <run_output.txt> <expected.txt>          生成基准\n"
                "  %s check    <new_output.txt> <expected.txt>          回归比对\n"
                "  %s gen_test <test_src.c> <expected.txt> <out.cpp>    生成 gtest 风格 C 测试用例\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "gen") == 0) return do_gen(argv[2], argv[3]);
    if (strcmp(argv[1], "check") == 0) return do_check(argv[2], argv[3]);
    if (strcmp(argv[1], "gen_test") == 0) {
        if (argc < 5) { fprintf(stderr, "gen_test 需要 <src.c> <expected.txt> <out.cpp>\n"); return 2; }
        return do_gen_test(argv[2], argv[3], argv[4]);
    }
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
