/* SPDX-License-Identifier: BSD-2-Clause
 *
 * SLogic16U3 gateware registers (fixed CSR map, slogic_16u3.py csr_map, checked against the generated
 * csr.csv by test/test_regs.py).
 *
 * This file is part of SLogic16U3.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef SLOGIC_REGS_H
#define SLOGIC_REGS_H

/* Bus */
/*-----*/

#define SLOGIC_CSR_BASE              0x00000000
#define SLOGIC_FLASH_BUFFER_BASE     0x20000000
#define SLOGIC_FLASH_BUFFER_SIZE     512

/* CSR banks (64-bit registers: two 32-bit words, most significant first). */
#define SLOGIC_CTRL_BASE             0x0000
#define SLOGIC_ANALYZER_BASE         0x0800
#define SLOGIC_FLASH_BASE            0x1000
#define SLOGIC_USB3_PHY_BASE         0x1800
#define SLOGIC_IDENTIFIER_BASE       0x2000
#define SLOGIC_IDENTIFIER_SIZE       256

/* Ctrl */
/*------*/

#define SLOGIC_CTRL_RESET            (SLOGIC_CTRL_BASE + 0x00)
#define SLOGIC_CTRL_SCRATCH          (SLOGIC_CTRL_BASE + 0x04)
#define SLOGIC_CTRL_BUS_ERRORS       (SLOGIC_CTRL_BASE + 0x08)

/* Analyzer */
/*----------*/

#define SLOGIC_ANALYZER_MAGIC        (SLOGIC_ANALYZER_BASE + 0x00)
#define SLOGIC_ANALYZER_VERSION      (SLOGIC_ANALYZER_BASE + 0x04)
#define SLOGIC_ANALYZER_CONFIG       (SLOGIC_ANALYZER_BASE + 0x08)
#define SLOGIC_ANALYZER_BASE_RATE    (SLOGIC_ANALYZER_BASE + 0x0c)
#define SLOGIC_ANALYZER_CONTROL      (SLOGIC_ANALYZER_BASE + 0x10)
#define SLOGIC_ANALYZER_DIVIDER      (SLOGIC_ANALYZER_BASE + 0x14)
#define SLOGIC_ANALYZER_LIMIT        (SLOGIC_ANALYZER_BASE + 0x18) /* 64-bit. */
#define SLOGIC_ANALYZER_STATUS       (SLOGIC_ANALYZER_BASE + 0x20)
#define SLOGIC_ANALYZER_COUNT        (SLOGIC_ANALYZER_BASE + 0x24) /* 64-bit. */
#define SLOGIC_ANALYZER_CLK_COUNT    (SLOGIC_ANALYZER_BASE + 0x2c)

#define SLOGIC_MAGIC                 0x534c4f47 /* "SLOG". */

#define SLOGIC_CONFIG_NCHANNELS(c)   (((c) >>  0) & 0xff)
#define SLOGIC_CONFIG_RATIO(c)       (((c) >>  8) & 0xff)
#define SLOGIC_CONFIG_FIFO_DEPTH(c)  ((((c) >> 16) & 0xffff)*64) /* 64-bit words. */

#define SLOGIC_CONTROL_ENABLE        (1 << 0)
#define SLOGIC_CONTROL_MODE(m)       (((m) & 0x3) << 4)
#define SLOGIC_CONTROL_PATTERN(p)    (((p) & 0x3) << 8)

#define SLOGIC_MODE_4CH              0
#define SLOGIC_MODE_8CH              1
#define SLOGIC_MODE_16CH             2

#define SLOGIC_PATTERN_INPUTS        0
#define SLOGIC_PATTERN_COUNTER       1

#define SLOGIC_STATUS_RUNNING        (1 << 0)
#define SLOGIC_STATUS_DONE           (1 << 1)
#define SLOGIC_STATUS_OVERFLOW       (1 << 2)
#define SLOGIC_STATUS_ERROR          (1 << 3)

/* Flash */
/*-------*/

#define SLOGIC_FLASH_CONTROL         (SLOGIC_FLASH_BASE + 0x00)
#define SLOGIC_FLASH_DIVIDER         (SLOGIC_FLASH_BASE + 0x04)
#define SLOGIC_FLASH_XFER            (SLOGIC_FLASH_BASE + 0x08)
#define SLOGIC_FLASH_CTRL            (SLOGIC_FLASH_BASE + 0x0c)
#define SLOGIC_FLASH_STATUS          (SLOGIC_FLASH_BASE + 0x10)

#define SLOGIC_FLASH_CONTROL_ENABLE  (1 << 0)
#define SLOGIC_FLASH_CONTROL_PINS(clk, cs_n, mosi, miso) \
    ((((clk) & 7) << 4) | (((cs_n) & 7) << 8) | (((mosi) & 7) << 12) | (((miso) & 7) << 16))
#define SLOGIC_FLASH_CONTROL_HIGH(m) (((m) & 0xff) << 20)
#define SLOGIC_FLASH_XFER_HOLD       (1 << 16)
#define SLOGIC_FLASH_CTRL_START      (1 << 0)
#define SLOGIC_FLASH_CTRL_RELEASE    (1 << 1)
#define SLOGIC_FLASH_STATUS_BUSY     (1 << 0)
#define SLOGIC_FLASH_NPADS           6

/* USB3 PHY */
/*----------*/

#define SLOGIC_USB3_PHY_STATUS       (SLOGIC_USB3_PHY_BASE + 0x00)
#define SLOGIC_USB3_PHY_LTSSM(s)     ((s) & 0xff)
#define SLOGIC_USB3_PHY_U0           (1 << 8)
#define SLOGIC_USB3_PHY_READY        (1 << 9)

#endif /* SLOGIC_REGS_H */
