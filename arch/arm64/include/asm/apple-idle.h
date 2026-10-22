/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_APPLE_IDLE_H
#define __ASM_APPLE_IDLE_H

/* WFI that preserves x18-x30 on affected Apple cores. */
void apple_cpu_wfi(void);
void apple_cpu_wfe(void);

#endif
