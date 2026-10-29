/* SPDX-License-Identifier: BSD-2-Clause
 *
 * libslogic: Sipeed DFU loader client (configuration SPI Flash access without our gateware: stock units,
 * recovery). Enter the loader with the MODE button (LED blinking red).
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef SLOGIC_DFU_H
#define SLOGIC_DFU_H

#include "slogic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLOGIC_DFU_VID 0x359f
#define SLOGIC_DFU_PID 0x30f1

struct slogic_dfu;

int  slogic_dfu_open(struct slogic_dfu **dfu, uint8_t jedec[3]);
void slogic_dfu_close(struct slogic_dfu *dfu);
int  slogic_dfu_read(struct slogic_dfu *dfu, uint32_t addr, uint8_t *data, size_t size);
/* Application update at 0x000000 (erase, program, verify), size <= SLOGIC_FLASH_APP_SIZE. */
int  slogic_dfu_update(struct slogic_dfu *dfu, const uint8_t *image, size_t size,
                       slogic_progress_cb progress, void *user);

#ifdef __cplusplus
}
#endif

#endif /* SLOGIC_DFU_H */
