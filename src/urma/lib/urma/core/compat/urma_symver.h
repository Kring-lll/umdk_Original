/* SPDX-License-Identifier: MIT */
#ifndef URMA_SYMVER_H
#define URMA_SYMVER_H

/*
 * Private ELF helpers, used at file scope in the implementation's TU.
 * Use urma_compat_ implementation names (hidden by liburma.map).
 * Do not also define an ordinary C function with the public alias name.
 */
#define URMA_SYMVER_COMPAT(impl, api, version) \
    __asm__(".symver " #impl ", " #api "@" version)
#define URMA_SYMVER_DEFAULT(impl, api, version) \
    __asm__(".symver " #impl ", " #api "@@" version)

#endif /* URMA_SYMVER_H */
