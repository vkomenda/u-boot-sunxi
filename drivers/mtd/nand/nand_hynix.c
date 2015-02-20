/*
 * Copyright (C) 2014 Boris BREZILLON <b.brezillon.dev@gmail.com>
 *               2015 Vladimir Komendantskiy
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

#include <common.h>
#include <malloc.h>
#include <linux/compat.h>
#include <linux/mtd/nand.h>

static u8 h27ucg8t2a_read_retry_regs[] = {
	0xcc, 0xbf, 0xaa, 0xab, 0xcd, 0xad, 0xae, 0xaf
};

static u8 h27ucg8t2e_read_retry_regs[] = {
	0x38, 0x39, 0x3a, 0x3b
};

struct hynix_read_retry {
	u8 regnum;      // number of registers to set on each RR step
	u8 *regs;       // array of register addresses
	u8 values[64];  // RR values to be written into the RR registers
};

struct hynix_nand {
	struct hynix_read_retry read_retry;
};

int nand_setup_read_retry_hynix(struct mtd_info *mtd, int retry_count)
{
	struct nand_chip *chip = mtd->priv;
	struct hynix_nand *hynix = chip->manuf_priv;
	int offset = retry_count * hynix->read_retry.regnum;
	int status;
	int i;

	chip->cmdfunc(mtd, 0x36, -1, -1);
	for (i = 0; i < hynix->read_retry.regnum; i++) {
		int column = hynix->read_retry.regs[i];
		column |= column << 8;
		chip->cmdfunc(mtd, NAND_CMD_NONE, column, -1);
		chip->write_byte(mtd, hynix->read_retry.values[offset + i]);
	}
	chip->cmdfunc(mtd, 0x16, 0 /* leave the OTP mode */, -1);

	status = chip->waitfunc(mtd, chip);
	if (status & NAND_STATUS_FAIL)
		return -EIO;

	return 0;
}

static void h27ucg8t2a_cleanup(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	kfree(chip->manuf_priv);
}

static int h27ucg8t2a_init(struct mtd_info *mtd, const uint8_t *id)
{
	struct nand_chip *chip = mtd->priv;
	struct hynix_nand *hynix;
	u8* buf = NULL;  // max(RR count register count, RR set register count)
	int rrtReg, rrtSet, i;
	int ret;

	buf = kzalloc(1024, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	chip->select_chip(mtd, 0);
	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
	chip->cmdfunc(mtd, 0x36, 0xff, -1);
	chip->write_byte(mtd, 0x40);
	chip->cmdfunc(mtd, NAND_CMD_NONE, 0xcc, -1);
	chip->write_byte(mtd, 0x4d);
	chip->cmdfunc(mtd, 0x16, -1, -1);
	chip->cmdfunc(mtd, 0x17, -1, -1);
	chip->cmdfunc(mtd, 0x04, -1, -1);
	chip->cmdfunc(mtd, 0x19, -1, -1);
	// FIXME: this will only read 1024 bytes at a time if flash chip RAM is
	// used as a buffer. Therefore the last 16 bytes of the last RRT copy
	// will be lost. If the last RRT copy is not used, the 0<=j<8 loop below
	// should be restricted to 0<=j<7.
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0x0, 0x200);

	// Read total RR count - 1 byte x 8 times - and
	// RR register count - also 1 byte x 8 times.
	chip->read_buf(mtd, buf, 16);
	printf("RR count (8 copies), RR reg. count (8 copies):\n");
	for (i = 0; i < 16; i++) {
		printf(" %.2x", buf[i]);
	}
	printf("\n");

	if ((buf[0] != 8 || buf[1] != 8) &&
	    (buf[8] != 8 || buf[9] != 8)) {
		error("wrong total RR count or RR register count\n");
		ret = -EINVAL;
		goto succeed_fail_a;
	}
	// Read 8 RR register sets, each consisting of a 64-byte original and
	// its 64-byte inverse copy. Every set stores 8 lists of values for the
	// 8 RR registers on the flash chip.
	chip->read_buf(mtd, buf, 1024);

	/* FIXME: common function - majority check, not "all correct" */
	ret = 0;
	printf("RRT sets in OTP...\n");
	for (rrtSet = 0; rrtSet < 8; rrtSet++) {
		u8 *cur = buf + (128 * rrtSet);
		printf("%d.", rrtSet);
		for (rrtReg = 0; rrtReg < 64; rrtReg++) {
			uint8_t original = cur[rrtReg];
			uint8_t inverse  = cur[rrtReg + 64];
			printf(" %.2x", original);
			if ((original | inverse) != 0xff) {
				error("original read retry level doesn't match its inverse\n");
				ret = -EINVAL;
				break;
			}
		}
		printf("\n");

		if (ret)
			// read the next set
			continue;
		else
			// current set is correct
			break;
	}

	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
	chip->cmdfunc(mtd, 0x38, -1, -1);
	chip->select_chip(mtd, -1);

succeed_fail_a:
	if (!ret) {
		hynix = kzalloc(sizeof(*hynix), GFP_KERNEL);
		if (!hynix) {
			ret = -ENOMEM;
			goto buf_dealloc_a;
		}

		hynix->read_retry.regs = h27ucg8t2a_read_retry_regs;
		hynix->read_retry.regnum = 8;
		memcpy(hynix->read_retry.values, &buf[rrtSet * 128], 64);
		chip->manuf_priv = hynix;
		chip->setup_read_retry = nand_setup_read_retry_hynix;
		chip->read_retries = 8;
		chip->manuf_cleanup = h27ucg8t2a_cleanup;
	}
	else
		error("Read retry initialisation failed.\n");

buf_dealloc_a:
	if (buf)
		kfree(buf);

	return ret;
}

static void h27ucg8t2e_cleanup(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;
	kfree(chip->manuf_priv);
}

#define UCG8T2E_RRT_OTP_SIZE (16 + 8 * 64)

static int h27ucg8t2e_init(struct mtd_info *mtd, const uint8_t *id)
{
	struct nand_chip *chip = mtd->priv;
	struct hynix_nand *hynix;
	u8* buf = NULL;
	int rrtReg, rrtSet, i;
	int ret;

	buf = kzalloc(UCG8T2E_RRT_OTP_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	printf("RR: select\n");
	chip->select_chip(mtd, 0);
	printf("RR: reset\n");
	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);

	/* copy RRT from OTP to buf */
	printf("RR: 0x36\n");
	chip->cmdfunc(mtd, 0x36, 0x38, -1);
	printf("RR: write 0x52\n");
	chip->write_byte(mtd, 0x52);
	printf("RR: 0x16\n");
	chip->cmdfunc(mtd, 0x16, -1, -1);
	printf("RR: 0x17\n");
	chip->cmdfunc(mtd, 0x17, -1, -1);
	printf("RR: 0x04\n");
	chip->cmdfunc(mtd, 0x04, -1, -1);
	printf("RR: 0x19\n");
	chip->cmdfunc(mtd, 0x19, -1, -1);
	printf("RR: read @ 0x200\n");
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0, 0x200);

	/*
	 *  1. Read total RR count - 1 byte x 8 times - and RR register count -
	 *  also 1 byte x 8 times.
	 *
	 *  2. Read 8 RR register sets, each consisting of a 32-byte original
	 *  and its 32-byte inverse copy. Every set stores 8 lists of values for
	 *  the 4 RR registers on the flash chip.
	 */
	chip->read_buf(mtd, buf, UCG8T2E_RRT_OTP_SIZE);
	printf("RR count (8 copies), RR reg. count (8 copies):\n");
	for (i = 0; i < 16; i++) {
		printf(" %.2x", buf[i]);
	}
	printf("\n");

	if ((buf[0] != 8 || buf[1] != 8) &&
	    (buf[8] != 4 || buf[9] != 4)) {
		error("wrong total RR count or RR register count\n");
		ret = -EINVAL;
		goto succeed_fail_e;
	}

	/* copy RRT from OTP, command suffix */
	printf("RR: reset\n");
	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
	printf("RR: 0x36\n");
	chip->cmdfunc(mtd, 0x36, 0x38, -1);
	printf("RR: write 0\n");
	chip->write_byte(mtd, 0);

	/* dummy read from any address */
	printf("RR: read\n");
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0, 0);

	printf("RR: select\n");
	chip->select_chip(mtd, -1);

	/* FIXME: common function - majority check, not "all correct" */
	ret = 0;
	printf("RRT sets in OTP...\n");
	for (rrtSet = 0; rrtSet < 8; rrtSet++) {
		u8 *cur = buf + 16 + (64 * rrtSet);
		printf("%d.", rrtSet);
		for (rrtReg = 0; rrtReg < 32; rrtReg++) {
			uint8_t original = cur[rrtReg];
			uint8_t inverse  = cur[rrtReg + 32];
			printf(" %.2x", original);
			if ((original | inverse) != 0xff) {
				error("original RR level doesn't match its inverse\n");
				ret = -EINVAL;
				break;
			}
		}
		printf("\n");

		if (ret)
			// read the next set
			continue;
		else
			// current set is correct
			break;
	}

succeed_fail_e:
	if (!ret) {
		hynix = kzalloc(sizeof(*hynix), GFP_KERNEL);
		if (!hynix) {
			ret = -ENOMEM;
			goto buf_dealloc_e;
		}

		hynix->read_retry.regs = h27ucg8t2e_read_retry_regs;
		hynix->read_retry.regnum = 4;

		// copy the first correct RRT set (the original half)
		memcpy(hynix->read_retry.values, &buf[16 + rrtSet * 64], 32);

		chip->manuf_priv = hynix;
		chip->setup_read_retry = nand_setup_read_retry_hynix;
		chip->read_retries = 8;
		chip->manuf_cleanup = h27ucg8t2e_cleanup;
	}
	else
		error("Read retry initialisation failed.\n");

buf_dealloc_e:
	if (buf)
		kfree(buf);

	return ret;
}

struct hynix_nand_initializer {
	u8 id[6];
	int (*init)(struct mtd_info *mtd, const uint8_t *id);
};

struct hynix_nand_initializer initializers[] = {
	{
		.id = {NAND_MFR_HYNIX, 0xde, 0x94, 0xda, 0x74, 0xc4},
		.init = h27ucg8t2a_init,
	},
//	{       // same RR procedure for H27UBG8T2B
//		.id = {NAND_MFR_HYNIX, 0xd7, 0x94, 0xda, 0x74, 0xc3},
//		.init = h27ucg8t2a_init,
//	},
	{
		.id = {NAND_MFR_HYNIX, 0xde, 0x14, 0xa7, 0x42, 0x4a},
		.init = h27ucg8t2e_init,
	},
};

int hynix_nand_init(struct mtd_info *mtd, const uint8_t *id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(initializers); i++) {
		struct hynix_nand_initializer *initializer = &initializers[i];
		if (memcmp(id, initializer->id, sizeof(initializer->id)))
			continue;

		return initializer->init(mtd, id);
	}

	return 0;
}
