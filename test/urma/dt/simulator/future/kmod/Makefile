# SPDX-License-Identifier: GPL-2.0
# urma stub kmod 编译。在 oe-arm 容器内（6.6 kernel-devel 头树）。
# KDIR 硬编码 6.6（容器 uname -r 是 5.15 WSL2，但我们要编针对 VM 的 6.6 内核）。

KDIR ?= /lib/modules/6.6.0-159.4.3.154.oe2403sp4.aarch64/build
ARCH ?= arm64

obj-m := urma_kmod.o

# urma_cmd.h / urma_cmd_tlv.h 在 umdk src 核心头目录；udma_abi.h 在 hw/udma/kernel_headers
URMA_INC := $(M)/../../../src/urma/lib/urma/core/include
URMA_CORE := $(M)/../../../src/urma/lib/urma/core
UDMA_ABI := $(M)/../../../src/urma/hw/udma/kernel_headers

ccflags-y := -I$(URMA_INC) -I$(UDMA_ABI) -I$(M) -Wno-error -w

all:
	make -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) modules

clean:
	make -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) clean
