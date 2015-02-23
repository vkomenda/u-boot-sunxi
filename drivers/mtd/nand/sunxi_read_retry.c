/*
 * Copyright (C) 2015 Vladimir Komendantskiy
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "sunxi_nand.h"

static uint8_t h27ucg8t2e_read_retry_regs[] = {
	0x38, 0x39, 0x3a, 0x3b
};

static uint8_t h27ucg8t2e_read_retry_values[] = {
	0x00, 0x00, 0x00, 0x00,
	0x02, 0x02, 0xfe, 0xfd,
	0x03, 0x03, 0xff, 0xf5,
	0xf1, 0xfd, 0xf8, 0xf7,
	0xed, 0xfc, 0xfb, 0xf5,
	0xe7, 0xfb, 0xf1, 0xf0,
	0xdd, 0xf8, 0xf7, 0xf4,
	0xd3, 0xe4, 0xeb, 0xeb
};

struct read_retry_setting read_retry;

static void hynix_send_rrt_prefix(uint8_t addr, uint8_t data)
{
	uint32_t cfg;

	writel(1, NFC_REG_CNT);
	writeb(data, NFC_RAM0_BASE);
	writel(addr, NFC_REG_ADDR_LOW);
	writel(0, NFC_REG_ADDR_HIGH);
	cfg = 0x36 | NFC_WAIT_FLAG | NFC_SEND_CMD1 | NFC_DATA_TRANS |
		NFC_ACCESS_DIR | NFC_SEND_ADR;
	wait_cmdfifo_free();
	writel(cfg, NFC_REG_CMD);
	wait_cmdfifo_free();
	wait_cmd_finish();
}

static int hynix_setup_read_retry(int retry)
{
	uint32_t cfg;
	int i;
	int offset = retry * read_retry.regnum;

	printf("RR %d\n", retry);

	if (retry >= read_retry.tries)
		return -EINVAL;

	for (i = 0; i < read_retry.regnum; i++) {
		hynix_send_rrt_prefix(read_retry.regs[i],
				      read_retry.values[offset + i]);
	}
	cfg = 0x16;
	cfg |= NFC_SEND_CMD1;
	writel(cfg, NFC_REG_CMD);

	wait_cmdfifo_free();
	wait_cmd_finish();
	/* TODO: check NFC status */

	return 0;
}

static void h27ucg8t2e_init(void)
{
	/* TODO: consider loading read retry tables from OOB */
	read_retry.tries  = 8;
	read_retry.regnum = 4;
	read_retry.regs   = h27ucg8t2e_read_retry_regs;
	read_retry.values = h27ucg8t2e_read_retry_values;
	read_retry.setup  = hynix_setup_read_retry;
}

struct rr_chip_init_assoc {
	uint8_t id[6];
	void (*init)(void);
};

struct rr_chip_init_assoc rr_chip_init[] = {
//	{
//		.id = {NAND_MFR_HYNIX, 0xd7, 0x94, 0xda, 0x74, 0xc3},
//		.init = TODO,
//	},
	{
		.id = {NAND_MFR_HYNIX, 0xde, 0x14, 0xa7, 0x42, 0x4a},
		.init = h27ucg8t2e_init,
	},
};

int read_retry_init(const uint8_t *id)
{
	int i, ret = -ENODEV;

	/* default value for chips not supported by the RR procedures */
	read_retry.tries = 1;

	for (i = 0; i < ARRAY_SIZE(rr_chip_init); i++) {
		struct rr_chip_init_assoc *init = &rr_chip_init[i];
		if (!memcmp(id, init->id, sizeof(init->id))) {
			init->init();
			ret = 0;
			break;
		}
	}

	return ret;
}
