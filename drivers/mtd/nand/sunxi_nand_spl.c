/*
 * sunxi_nand_spl.c
 *
 * Copyright (C) 2013 Qiang Yu <yuq825@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sunxi_nand.h"

int sunxi_nand_spl_page_size;
int sunxi_nand_spl_block_size;

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

struct spl_read_retry {
	uint8_t  retries; // maximum number of possible retries
	uint8_t  regnum;  // number of registers to set on each RR step
	uint8_t* regs;    // array of register addresses
	uint8_t* values;  // RR values to be written into the RR registers
        int      (*setup)(int retry);  // setup function
};

struct spl_read_retry read_retry;
int hynix_setup_read_retry(int retry);

static void wait_cmdfifo_free(void)
{
	int timeout = 0xffff;
	while ((timeout--) && (readl(NFC_REG_ST) & NFC_CMD_FIFO_STATUS));
	if (timeout <= 0) {
		printf("wait_cmdfifo_free timeout\n");
	}
}

static void wait_cmd_finish(void)
{
	int timeout = 0xffff;
	while((timeout--) && !(readl(NFC_REG_ST) & NFC_CMD_INT_FLAG));
	if (timeout <= 0) {
		printf("wait_cmd_finish timeout\n");
		return;
	}
	writel(NFC_CMD_INT_FLAG, NFC_REG_ST);
}

// 1 for ready, 0 for not ready
static int check_rb_ready(int rb)
{
	return (readl(NFC_REG_ST) & (NFC_RB_STATE0 << (rb & 0x3))) ? 1 : 0;
}

static int nfc_isbad(uint32_t offs)
{
	uint32_t page_addr;
	uint8_t marker;
	uint32_t cfg = NAND_CMD_READ0 | NFC_SEQ | NFC_SEND_CMD1 | NFC_DATA_TRANS | NFC_SEND_ADR |
		NFC_SEND_CMD2 | ((5 - 1) << 16) | NFC_WAIT_FLAG | (0 << 30);

	offs &= ~(sunxi_nand_spl_block_size - 1);
	page_addr = offs / sunxi_nand_spl_page_size;

	wait_cmdfifo_free();
	writel(readl(NFC_REG_CTL) & ~NFC_RAM_METHOD, NFC_REG_CTL);
	writel(sunxi_nand_spl_page_size | (page_addr << 16), NFC_REG_ADDR_LOW);
	writel(page_addr >> 16, NFC_REG_ADDR_HIGH);
	writel(2, NFC_REG_CNT);
	writel(1, NFC_REG_SECTOR_NUM);
	writel(NAND_CMD_READSTART, NFC_REG_RCMD_SET);
	writel(cfg, NFC_REG_CMD);
	wait_cmdfifo_free();
	wait_cmd_finish();
	if ((marker = readb(NFC_RAM0_BASE)) != 0xff)
		return 1;
	return 0;
}

static int nfc_read_page(uint32_t offs, void *buff, bool raw)
{
	uint32_t page_addr;
	uint32_t cfg = NAND_CMD_READ0 | NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD | NFC_SEND_CMD1 |
		NFC_SEND_ADR | ((5 - 1) << 16) | NFC_WAIT_FLAG | NFC_DATA_TRANS | (2 << 30);
	int status;

	page_addr = offs / sunxi_nand_spl_page_size;

//	printf("p@%x/%x->%p\n", offs, page_addr, buff);

	wait_cmdfifo_free();
	writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
	_dma_config_start(0, NFC_REG_IO_DATA, (__u32)buff, sunxi_nand_spl_page_size);
	writel(page_addr << 16, NFC_REG_ADDR_LOW);
	writel(page_addr >> 16, NFC_REG_ADDR_HIGH);
	writel(0x00e00530, NFC_REG_RCMD_SET);
	writel(1024, NFC_REG_CNT);
	writel(sunxi_nand_spl_page_size / 1024, NFC_REG_SECTOR_NUM);
	if (!raw) {
		enable_random(page_addr);
		enable_ecc(1);
	}
	writel(cfg, NFC_REG_CMD);
	_wait_dma_end();
	wait_cmdfifo_free();
	wait_cmd_finish();
	if (!raw) {
		disable_ecc();
		disable_random();
		status = check_ecc(sunxi_nand_spl_page_size / 1024);
	}
	else
		status = 0;
	return status;
}

static void nfc_reset(void)
{
	u32 cfg;

	wait_cmdfifo_free();
	cfg = NAND_CMD_RESET | NFC_SEND_CMD1;
	writel(cfg, NFC_REG_CMD);
	wait_cmdfifo_free();
	wait_cmd_finish();
	// wait rb0 ready
	select_rb(0);
	while (!check_rb_ready(0));
	// wait rb1 ready
//	select_rb(1);
//	while (!check_rb_ready(1));
	// select rb 0 back
//	select_rb(0);
}

static void nfc_readid(uint8_t *id)
{
	u32 cfg;
	int i;

	wait_cmdfifo_free();
	writel(0, NFC_REG_ADDR_LOW);
	writel(0, NFC_REG_ADDR_HIGH);
	cfg = NAND_CMD_READID | NFC_SEND_ADR | NFC_DATA_TRANS | NFC_SEND_CMD1;
	writel(8, NFC_REG_CNT);
	writel(cfg, NFC_REG_CMD);
	wait_cmdfifo_free();
	wait_cmd_finish();

	for (i = 0; i < 8; i++)
		id[i] = readb(NFC_RAM0_BASE + i);
}

static void nfc_select_chip(int chip)
{
	uint32_t ctl;
	// A10 has 8 CE pin to support 8 flash chips
	ctl = readl(NFC_REG_CTL);
	ctl &= ~NFC_CE_SEL;
	ctl |= ((chip & 7) << 24);
	writel(ctl, NFC_REG_CTL);
}

static int nfc_init(void)
{
	u32 ctl;
	int i, j;
	uint8_t id[8];
	struct nand_chip_param *chip_cur, *chip = NULL;

	// set init clock
	sunxi_nand_set_clock(NAND_MAX_CLOCK);

	// set gpio
	sunxi_nand_set_gpio();

	// reset NFC
	ctl = readl(NFC_REG_CTL);
	ctl |= NFC_RESET;
	writel(ctl, NFC_REG_CTL);
	while(readl(NFC_REG_CTL) & NFC_RESET);

	// enable NFC
	ctl = NFC_EN;
	writel(ctl, NFC_REG_CTL);

	// serial_access_mode = 1
	ctl = 1 << 8;
	writel(ctl, NFC_REG_TIMING_CTL);

	// reset nand chip
	nfc_reset();

	// read nand chip id
	nfc_readid(id);

//	printf("NAND:");
	/* Get parameters of the chip in the database of the driver. */
	chip_cur = sunxi_get_nand_chip_param(id[0]);
	for (i = 0; !chip && chip_cur[i].id_len; i++)
		for (j = 0; j < chip_cur[i].id_len; j++) {
			if (id[j] != chip_cur[i].id[j])
				/* ID mismatch */
				break;
			else if (j == chip_cur[i].id_len - 1)
				/* all bytes of the ID matched */
				chip = &chip_cur[i];
		}

	if (!chip) {
		printf(" unknown chip");
		return -ENODEV;
	}
	else
		for (j = 0; j < chip->id_len; j++)
			printf(" %x", chip->id[j]);
	printf("\n");

	/* TODO: load read retry tables from OOB */
	read_retry.retries = 8;
	read_retry.regnum  = 4;
	read_retry.regs    = h27ucg8t2e_read_retry_regs;
	read_retry.values  = h27ucg8t2e_read_retry_values;
	read_retry.setup   = hynix_setup_read_retry;

	// TODO: remove this upper bound
	if (chip->clock_freq > 30)
		chip->clock_freq = 30;
	sunxi_nand_set_clock((int)chip->clock_freq * 1000000);

	// disable interrupt
	writel(0, NFC_REG_INT);
	// clear interrupt
	writel(readl(NFC_REG_ST), NFC_REG_ST);

	// set ECC mode
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_ECC_MODE;
	ctl |= (unsigned int)chip->ecc_mode << NFC_ECC_MODE_SHIFT;
	writel(ctl, NFC_REG_ECC_CTL);

	// enable NFC
	ctl = NFC_EN;

	// Page size
	if (chip->page_shift > 14 || chip->page_shift < 10) {
//		printf("Page shift out of range\n");
		return -EINVAL;
	}
	// 0 for 1K
	ctl |= ((chip->page_shift - 10) & 0xf) << 8;
	writel(ctl, NFC_REG_CTL);

	writel(0xff, NFC_REG_TIMING_CFG);
	writel((1U << chip->page_shift) + BB_MARK_SIZE,
	       NFC_REG_SPARE_AREA);

	disable_random();

	// record size
	sunxi_nand_spl_page_size =
		1U << chip->page_shift;
	sunxi_nand_spl_block_size =
		1U << (chip->page_per_block_shift + chip->page_shift);

	// setup DMA
	dma_hdle = DMA_Request(DMAC_DMATYPE_DEDICATED);
	if (dma_hdle == 0) {
//		printf("DMA request failed\n");
		return -ENODEV;
	}

	return 0;
}

int nand_spl_isbad(uint32_t offs)
{
	return nfc_isbad(offs);
}

static bool nand_spl_page_is_empty(void *data)
{
	uint8_t *buf = data;
	uint32_t pattern = 0xffffffff;
	int bitflips = 0, cnt;
	uint32_t length = sunxi_nand_spl_page_size;
	/* hard-coded total error correction bit limit for 40-bit/1KiB ECC */
	int max_bitflips = (sunxi_nand_spl_page_size / 1024) * 40;

	while (length) {
		cnt = length < sizeof(pattern) ? length : sizeof(pattern);
		if (memcmp(&pattern, buf, cnt)) {
			int i;
			for (i = 0; i < cnt * 8; i++) {
				if (!(buf[i / 8] &
				      (1 << (i % 8)))) {
					bitflips++;
					if (bitflips > max_bitflips)
						return false;
				}
			}
		}

		buf += sizeof(pattern);
		length -= sizeof(pattern);
	}

	return true;
}

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

int hynix_setup_read_retry(int retry)
{
	uint32_t cfg;
	int i;
	int offset = retry * read_retry.regnum;

	printf("RR %d\n", retry);

	if (retry >= read_retry.retries)
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

void nand_spl_read(uint32_t offs, int size, void *dst)
{
//	printf("i@%x(%x)->%p\n", offs, size, dst);

	// offs must be page aligned
	while (size > 0) {
		int retry, status;

		retry = 0;
		status = 1;

		while (status && retry < read_retry.retries) {
			status = nfc_read_page(offs, dst, false);
			if (!status)
				/* page read successful */
				break;

			/* ECC check has failed. Read the page raw and
			 * check it for emptiness. */
			nfc_read_page(offs, dst, true);
			if (nand_spl_page_is_empty(dst)) {
				/* emptiness check passed */
				memset(dst, 0xff, sunxi_nand_spl_page_size);
				/* set termination condition */
				status = 0;
			}
			else
				printf("ECC error @ %x\n", offs);

			if (status && retry + 1 < read_retry.retries) {
				retry++;
				if (read_retry.setup(retry)) {
//					printf("RR setup error\n");
					status = 0;
				}
			}
		}

		if (retry)
			read_retry.setup(0);

		offs += sunxi_nand_spl_page_size;
		dst += sunxi_nand_spl_page_size;
		size -= sunxi_nand_spl_page_size;
	}
}

int nand_spl_load_image(uint32_t offs, unsigned int image_size, void *dst)
{
	int size = image_size;
	uint32_t to, len, bound;

	while (size > 0) {
		puts(">");
		if (nand_spl_isbad(offs)) {
			debug("Bad NAND block %x\n", offs);
			offs += sunxi_nand_spl_block_size;
			continue;
		}

		to = roundup(offs, sunxi_nand_spl_block_size);
		bound = (to == offs) ? sunxi_nand_spl_block_size : (to - offs);
		len = bound > size ? size : bound;
		nand_spl_read(offs, len, dst);
		offs += len;
		dst += len;
		size -= len;
	}

	return 0;
}

/* nand_init() - initialize data to make nand usable by SPL */
void nand_init(void)
{
	if (nfc_init())
		return;

	nfc_select_chip(0);
}

/* Unselect after operation */
void nand_deselect(void)
{

}
