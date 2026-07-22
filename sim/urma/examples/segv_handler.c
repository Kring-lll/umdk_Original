/* tiny SIGSEGV handler preload: prints backtrace via backtrace() + dli_addr → function name.
 * 用：LD_PRELOAD=this.so ./test_case ... （在 sim 的 LD_PRELOAD 之外再加，或直接编译进 test_case）
 * 写成独立 .so 避免改 test 框架代码。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <string.h>

static void segv_handler(int sig, siginfo_t *info, void *uc)
{
    (void)uc;
    void *frames[64];
    int n = backtrace(frames, 64);
    fprintf(stderr, "\n=== SIGSEGV (%d) at addr=%p, si_code=%d ===\n",
            sig, info ? info->si_addr : NULL, info ? info->si_code : -1);
    fprintf(stderr, "backtrace (%d frames):\n", n);
    for (int i = 0; i < n; i++) {
        Dl_info di;
        const char *sym = "?";
        const char *fname = "?";
        if (dladdr(frames[i], &di)) {
            if (di.dli_sname) sym = di.dli_sname;
            if (di.dli_fname) fname = di.dli_fname;
        }
        fprintf(stderr, "  [%2d] %p  %s + %tx  (%s)\n", i, frames[i], sym,
                di.dli_saddr ? (char *)frames[i] - (char *)di.dli_saddr : 0, fname);
    }
    /* dump maps 帮助 addr2line 定位 lib 里未导出符号的偏移 */
    FILE *mf = fopen("/proc/self/maps", "r");
    if (mf) {
        char line[512];
        fprintf(stderr, "=== /proc/self/maps (lib lines) ===\n");
        while (fgets(line, sizeof(line), mf)) {
            if (strstr(line, "urma") || strstr(line, "liburma") || strstr(line, "udma") || strstr(line, "test_case"))
                fprintf(stderr, "%s", line);
        }
        fclose(mf);
    }
    _exit(139);
}

__attribute__((constructor))
static void install_segv(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    fprintf(stderr, "[segv_handler installed]\n");
}
