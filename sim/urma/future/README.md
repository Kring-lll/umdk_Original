# future/

本目录存放 **已设计/编通但当前不接入 sim 的"未来实现"**。这些代码不参与 sim 的编译与运行，sim 仍保持纯用户态（见上层 [README.md](../README.md)）。留档供将来环境具备条件时恢复。

与 [../deprecated/](../deprecated/) 的区别：deprecated 是已废弃、被取代的旧实现；future 是方向正确但因当前环境限制（如 TCG 仿真跑不动）暂未上线的实现。

## kmod/

**内核态仿真（方案 A）**：在内核注册真 cdev + ioctl handler，使 urma 建链命令真进内核被处理（不再被 sim 用户态冒名顶替），数据面留用户态 sim。

- `urma_kmod.c` / `Makefile` —— 内核模块源码，已在容器用 6.6 kernel-devel 编通过 `urma_kmod.ko`（vermagic 6.6 aarch64）
- `enum_check.c` —— 辅助小程序，打印 urma_cmd 各枚举的实际十进制值（kmod 自包含定义协议常量，不能 include urma 用户态头，需用此确认值）
- `test_kmod.c` —— kmod 单独验证程序（open cdev + QUERY_DEV_ATTR ioctl + 验回填值）
- `DESIGN.md` —— 完整设计/分工/状态/恢复方法

**当前状态**：核心前提已验证——openEuler aarch64 VM 内核的 `init_module` 是真 syscall、能加载模块（容器 qemu-user 返回 ENOSYS 的对照）。但当前 x86+TCG 环境下完整 VM 被 openEuler 自带 `ummu_pmu` 驱动的 PMU 寄存器访问 panic 堵死，到不了 insmod 阶段。

**将来恢复**：拿到真 aarch64 硬件、或能避开 PMU panic 的 VM 环境（禁 PMU 启动参数 / 极简内核+busybox / 真 ARM 机器）时，按 [kmod/DESIGN.md](kmod/DESIGN.md) 的"将来恢复"小节编译加载并改造 sim 拦截层透传。
