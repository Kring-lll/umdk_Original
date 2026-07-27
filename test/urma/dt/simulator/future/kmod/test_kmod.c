/*
 * SPDX-License-Identifier: GPL-2.0
 * kmod 单独验证程序（不接 sim，纯测 kmod 的 ioctl handler）。
 * open /dev/uburma/udma_sim0 + 发 QUERY_DEV_ATTR ioctl + 打印回填的 max_jfc 等。
 *
 * 在容器内用 build_noasan 编（用户态，aarch64）：
 *   gcc -I/umdk/src/urma/lib/urma/core/include -I/umdk/src/urma/lib/urma/core \
 *       -I/umdk/src/urma/common/include test_kmod.c -o /tmp/test_kmod
 * 在 VM 内（insmod urma_kmod.ko 后）跑：
 *   /tmp/test_kmod
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "urma_cmd.h"
#include "urma_cmd_tlv.h"

#define DEV_PATH "/dev/uburma/udma_sim0"

int main(void)
{
    int fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s fail: %s\n", DEV_PATH, strerror(errno));
        return 1;
    }
    printf("opened %s fd=%d\n", DEV_PATH, fd);

    /* 构造 QUERY_DEV_ATTR 的 attr 数组：要读 max_jfc(OUT type=130) max_jfs(131) max_jfr(132)
     * max_jetty(133) 的值。每个 OUT attr 给个 field_size=4 的 u32 缓冲。 */
    uint32_t v_max_jfc = 0, v_max_jfs = 0, v_max_jfr = 0, v_max_jetty = 0;
    urma_cmd_attr_t attrs[4];
    memset(attrs, 0, sizeof(attrs));
    /* QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC=130, MAX_JFS=131, MAX_JFR=132, MAX_JETTY=133 */
    attrs[0].type = 130; attrs[0].field_size = 4; attrs[0].data = (uint64_t)(uintptr_t)&v_max_jfc;
    attrs[1].type = 131; attrs[1].field_size = 4; attrs[1].data = (uint64_t)(uintptr_t)&v_max_jfs;
    attrs[2].type = 132; attrs[2].field_size = 4; attrs[2].data = (uint64_t)(uintptr_t)&v_max_jfr;
    attrs[3].type = 133; attrs[3].field_size = 4; attrs[3].data = (uint64_t)(uintptr_t)&v_max_jetty;

    urma_cmd_hdr_t hdr = {
        .command = 40,  /* URMA_CMD_QUERY_DEV_ATTR */
        .args_len = sizeof(attrs),
        .args_addr = (uint64_t)(uintptr_t)attrs,
    };

    int ret = ioctl(fd, URMA_CMD, &hdr);
    printf("ioctl QUERY_DEV_ATTR ret=%d errno=%d(%s)\n", ret, errno,
           ret ? strerror(errno) : "ok");
    if (ret == 0) {
        printf("  max_jfc=%u (kmod 内置 999)\n", v_max_jfc);
        printf("  max_jfs=%u (kmod 内置 1024)\n", v_max_jfs);
        printf("  max_jfr=%u (kmod 内置 1024)\n", v_max_jfr);
        printf("  max_jetty=%u (kmod 内置 4096)\n", v_max_jetty);
        /* 验证 kmod 回填对了 */
        if (v_max_jfc == 999 && v_max_jfs == 1024 && v_max_jfr == 1024 && v_max_jetty == 4096) {
            printf("KMOD TEST PASS\n");
        } else {
            printf("KMOD TEST FAIL (回填值不对)\n");
        }
    } else {
        printf("KMOD TEST FAIL (ioctl 失败)\n");
    }
    close(fd);
    return 0;
}
