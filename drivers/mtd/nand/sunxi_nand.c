/*
 * sunxi_nand.c
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

#define NAND_READ_BUFFER_SIZE (16384 + 2048)
static int read_offset = 0, write_offset = 0;
static int buffer_size = NAND_READ_BUFFER_SIZE;
static char write_buffer[NAND_READ_BUFFER_SIZE] __attribute__((aligned(4)));
static char read_buffer[NAND_READ_BUFFER_SIZE] __attribute__((aligned(4)));
static struct nand_ecclayout sunxi_ecclayout;
static int program_column = -1, program_page = -1;

static void nfc_select_chip(struct mtd_info *mtd, int chip)
{
	uint32_t ctl;
	// A10 has 8 CE pin to support 8 flash chips
	ctl = readl(NFC_REG_CTL);
	ctl &= ~NFC_CE_SEL;
	ctl |= ((chip & 7) << 24);
	writel(ctl, NFC_REG_CTL);
}

static void nfc_cmdfunc(struct mtd_info *mtd, unsigned command, int column,
						int page_addr)
{
	int i;
	uint32_t cfg = command;
	int read_size, write_size, do_enable_ecc = 0, do_enable_random = 0;
	int addr_cycle, wait_rb_flag, byte_count, sector_count;

	addr_cycle = wait_rb_flag = byte_count = sector_count = 0;

	wait_cmdfifo_free();

	// switch to AHB
	writel(readl(NFC_REG_CTL) & ~NFC_RAM_METHOD, NFC_REG_CTL);

	switch (command) {
	case NAND_CMD_RESET:
	case NAND_CMD_ERASE2:
		break;
	case NAND_CMD_READID:
		addr_cycle = 1;
		// read 8 byte ID
		byte_count = 8;
		break;
	case NAND_CMD_PARAM:
		addr_cycle = 1;
		byte_count = 1024;
		wait_rb_flag = 1;
		break;
	case NAND_CMD_READ0: /* Implied randomised read, see
			      * nand_base.c:do_nand_read_ops() */
		do_enable_ecc = 1;
		do_enable_random = 1;
		/* otherwise the same as a regular read */
	case NAND_CMD_READOOB:
	case NAND_CMD_READ1: /* Non-randomised read: this interpretation is
			      * specific to this driver. */
		if (command != NAND_CMD_READOOB) {
			sector_count = mtd->writesize / 1024;
			read_size = mtd->writesize;
		}
		else {
			// sector num to read
			sector_count = 1;  /* FIXME: get rid of it */
			read_size = 1024;  /* FIXME: try mtd->oobsize */
			// OOB offset
			column += mtd->writesize;
		}
		cfg = NAND_CMD_READ0; /* same underlying command for randomised
				       * page, non-randomised page and OOB
				       * reads. */

		//access NFC internal RAM by DMA bus
		writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
		// if the size is smaller than NFC_REG_SECTOR_NUM, read command won't finish
		// does that means the data read out (by DMA through random data output) hasn't finish?
		_dma_config_start(0, NFC_REG_IO_DATA, (__u32)read_buffer, read_size);

		// NFC_SEND_CMD1 for the command 1nd cycle enable
		// NFC_SEND_CMD2 for the command 2nd cycle enable
		// NFC_SEND_CMD3 & NFC_SEND_CMD4 for NFC_READ_CMD0 & NFC_READ_CMD1
		cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD;
		// 3 - ?
		// 2 - page command
		// 1 - spare command?
		// 0 - normal command
		cfg |= 2 << 30;

		addr_cycle = 5;
		// RAM0 is 1K size
		byte_count =1024;
		wait_rb_flag = 1;

		if (do_enable_random)
			// 0x30 for 2nd cycle of read page
			// 0x05+0xe0 is the random data output command
			writel(0x00e00530, NFC_REG_RCMD_SET);
		else
			writel(0x00000030, NFC_REG_RCMD_SET);

		break;
	case NAND_CMD_ERASE1:
		addr_cycle = 3;
		//debug("cmdfunc earse block %d\n", page_addr);
		break;
	case NAND_CMD_SEQIN:
		program_column = column;
		program_page = page_addr;
		write_offset = 0;
		return;
	case NAND_CMD_PAGEPROG:
		cfg = NAND_CMD_SEQIN;
		addr_cycle = 5;
		column = program_column;
		page_addr = program_page;
		debug("cmdfunc pageprog: %d %d\n", column, page_addr);

		// for write OOB
		if (column == mtd->writesize) {
			sector_count = 1024 /1024;
			write_size = 1024;
		}
		else if (column == 0) {
			sector_count = mtd->writesize / 1024;
			do_enable_ecc = 1;
			write_size = mtd->writesize;
			for (i = 0; i < sector_count; i++)
				writel(*((unsigned int *)(write_buffer + mtd->writesize) + i), NFC_REG_USER_DATA(i));
		}
		else {
			printf("program unsupported column %d %d\n", column, page_addr);
			return;
		}
		do_enable_random = 1;

		//access NFC internal RAM by DMA bus
		writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
		_dma_config_start(1, (__u32)write_buffer, NFC_REG_IO_DATA, write_size);
		// RAM0 is 1K size
		byte_count =1024;
		writel(0x00008510, NFC_REG_WCMD_SET);
		cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD | NFC_ACCESS_DIR;
		cfg |= 2 << 30;
		if (column != 0) {
			debug("cmdfunc program %d %d with %x %x %x\n", column, page_addr,
					 write_buffer[0], write_buffer[1], write_buffer[2]);
		}
		break;
	case NAND_CMD_STATUS:
		byte_count = 1;
		break;
	default:
		printf("unknown command\n");
		return;
	}

	// address cycle
	if (addr_cycle) {
		uint32_t low = 0;
		uint32_t high = 0;
		switch (addr_cycle) {
		case 5:
			high = (page_addr >> 16) & 0xff;
		case 1:
			low = column & 0xff;
			break;
		case 2:
			low = column & 0xffff;
			break;
		case 3:
			low = page_addr & 0xffffff;
			break;
		case 4:
			low = (column & 0xffff) | (page_addr << 16);
			break;
		default:
			error("wrong address cycle count");
			break;
		}
		writel(low, NFC_REG_ADDR_LOW);
		writel(high, NFC_REG_ADDR_HIGH);
		cfg |= NFC_SEND_ADR;
		cfg |= ((addr_cycle - 1) << 16);
	}

	// command will wait until the RB ready to mark finish?
	if (wait_rb_flag)
		cfg |= NFC_WAIT_FLAG;

	// will fetch data
	if (byte_count) {
		cfg |= NFC_DATA_TRANS;
		writel(byte_count, NFC_REG_CNT);
	}

	// set sectors
	if (sector_count)
		writel(sector_count, NFC_REG_SECTOR_NUM);

	if (do_enable_random)
		enable_random(page_addr);

	// enable ecc
	if (do_enable_ecc)
		enable_ecc(1);

	// send command
	cfg |= NFC_SEND_CMD1;
	writel(cfg, NFC_REG_CMD);

	switch (command) {
	case NAND_CMD_READ0:
	case NAND_CMD_READOOB:
	case NAND_CMD_PAGEPROG:
		_wait_dma_end();
		break;
	default:
		break;
	}

	// wait command send complete
	wait_cmdfifo_free();
	wait_cmd_finish();

	// reset will wait for RB ready
	switch (command) {
	case NAND_CMD_RESET:
		// wait rb0 ready
		select_rb(0);
		while (!check_rb_ready(0));
		// wait rb1 ready
//		select_rb(1);
//		while (!check_rb_ready(1));
		// select rb 0 back
//		select_rb(0);
		break;
	case NAND_CMD_READ0:
	case NAND_CMD_READ1: {
		uint32_t* oob_start = (uint32_t*) read_buffer + mtd->writesize;
		for (i = 0; i < sector_count; i++) {
			uint32_t userdata = readl(NFC_REG_USER_DATA(i));
			*(oob_start + i * 4) = userdata;
		}
		break;
	}
	default:
		break;
	}

	if (do_enable_ecc)
		disable_ecc();

	if (do_enable_random)
		disable_random();

	read_offset = 0;
}

static uint8_t nfc_read_byte(struct mtd_info *mtd)
{
	return readb(NFC_RAM0_BASE + read_offset++);
}

static int nfc_dev_ready(struct mtd_info *mtd)
{
	return check_rb_ready(0);
}

static void nfc_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	if (write_offset + len > buffer_size) {
		printf("write buffer overfill offs=%d len=%d size=%d\n",
		       write_offset, len, buffer_size);
		return;
	}
	memcpy(write_buffer + write_offset, buf, len);
	write_offset += len;
}

static void nfc_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	if (read_offset + len > buffer_size) {
		printf("read buffer overfill offs=%d len=%d size=%d\n",
		       read_offset, len, buffer_size);
		return;
	}
	memcpy(buf, read_buffer + read_offset, len);
	read_offset += len;
}

static int get_chip_status(struct mtd_info *mtd)
{
	struct nand_chip *nand = mtd->priv;
	nand->cmdfunc(mtd, NAND_CMD_STATUS, -1, -1);
	return nand->read_byte(mtd);
}

// For erase and program command to wait for chip ready
static int nfc_wait(struct mtd_info *mtd, struct nand_chip *chip)
{
	while (!check_rb_ready(0));
	return get_chip_status(mtd);
}

static void nfc_ecc_hwctl(struct mtd_info *mtd, int mode)
{

}

static int nfc_ecc_calculate(struct mtd_info *mtd, const uint8_t *dat, uint8_t *ecc_code)
{
	return 0;
}

static int nfc_ecc_correct(struct mtd_info *mtd, uint8_t *dat, uint8_t *read_ecc, uint8_t *calc_ecc)
{
	return check_ecc(mtd->writesize / 1024);
}

//////////////////////////////////////////////////////////////////////////////////
// 1K mode for SPL read/write

struct save_1k_mode {
	uint32_t ctl;
	uint32_t ecc_ctl;
	uint32_t spare_area;
};

static void enter_1k_mode(struct save_1k_mode *save)
{
	uint32_t ctl;

	ctl = readl(NFC_REG_CTL);
	save->ctl = ctl;
	ctl &= ~NFC_PAGE_SIZE;
	writel(ctl, NFC_REG_CTL);

	ctl = readl(NFC_REG_ECC_CTL);
	save->ecc_ctl = ctl;
	set_ecc_mode(8);

	ctl = readl(NFC_REG_SPARE_AREA);
	save->spare_area = ctl;
	writel(1024, NFC_REG_SPARE_AREA);
}

static void exit_1k_mode(struct save_1k_mode *save)
{
	writel(save->ctl, NFC_REG_CTL);
	writel(save->ecc_ctl, NFC_REG_ECC_CTL);
	writel(save->spare_area, NFC_REG_SPARE_AREA);
}

void nfc_read_page1k(uint32_t page_addr, void *buff)
{
	struct save_1k_mode save;
	uint32_t cfg = NAND_CMD_READ0 | NFC_SEQ | NFC_SEND_CMD1 | NFC_DATA_TRANS | NFC_SEND_ADR |
		NFC_SEND_CMD2 | ((5 - 1) << 16) | NFC_WAIT_FLAG | NFC_DATA_SWAP_METHOD | (2 << 30);

	nfc_select_chip(NULL, 0);

	wait_cmdfifo_free();

	enter_1k_mode(&save);

	writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
	_dma_config_start(0, NFC_REG_IO_DATA, (uint32_t)buff, 1024);

	writel(page_addr << 16, NFC_REG_ADDR_LOW);
	writel(page_addr >> 16, NFC_REG_ADDR_HIGH);
	writel(1024, NFC_REG_CNT);
	writel(0x00e00530, NFC_REG_RCMD_SET);
	writel(1, NFC_REG_SECTOR_NUM);

	enable_random_preset();
	enable_ecc(1);

	writel(cfg, NFC_REG_CMD);

	_wait_dma_end();
	wait_cmdfifo_free();
	wait_cmd_finish();

	disable_ecc();
	check_ecc(1);
	disable_random();

	exit_1k_mode(&save);

	nfc_select_chip(NULL, -1);
}

void nfc_write_page1k(uint32_t page_addr, void *buff)
{
	struct save_1k_mode save;
	uint32_t cfg = NAND_CMD_SEQIN | NFC_SEQ | NFC_SEND_CMD1 | NFC_DATA_TRANS | NFC_SEND_ADR |
		NFC_SEND_CMD2 | ((5 - 1) << 16) | NFC_WAIT_FLAG | NFC_DATA_SWAP_METHOD | NFC_ACCESS_DIR |
		(2 << 30);

	nfc_select_chip(NULL, 0);

	wait_cmdfifo_free();

	enter_1k_mode(&save);

	writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
	_dma_config_start(1, (uint32_t)buff, NFC_REG_IO_DATA, 1024);

	writel(page_addr << 16, NFC_REG_ADDR_LOW);
	writel(page_addr >> 16, NFC_REG_ADDR_HIGH);
	writel(1024, NFC_REG_CNT);
	writel(0x00008510, NFC_REG_WCMD_SET);
	writel(1, NFC_REG_SECTOR_NUM);

	enable_random_preset();

	enable_ecc(1);

	writel(cfg, NFC_REG_CMD);

	_wait_dma_end();
	wait_cmdfifo_free();
	wait_cmd_finish();

	disable_ecc();

	disable_random();

	exit_1k_mode(&save);

	nfc_select_chip(NULL, -1);
}

/*
 * Page read with hardware ECC and randomisation that takes into account empty
 * pages and does not issue an ECC error when trying to read a correct empty
 * page.
 */
static int nfc_read_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip,
			       uint8_t *buf, int oob_required, int page)
{
	int eccstatus = 0;

	eccstatus = check_ecc(mtd->writesize / 1024);

	if (eccstatus >= 0) {
		/* Collect ECC statistics: corrected bitflips. */
		mtd->ecc_stats.corrected += eccstatus;

		/* Copy the output to the buffer of the main NAND driver. */
		nfc_read_buf(mtd, buf, mtd->writesize);
		if (oob_required)
			/* copy the OOB area */
			nfc_read_buf(mtd, chip->oob_poi, mtd->oobsize);
	}
	else {
		/* The ECC check has failed. Check if the page is empty
		 * and update the read buffer if so. Otherwise report
		 * ECC failure. */

		/* Re-read the page without the randomiser or ECC. */
		nfc_cmdfunc(mtd, NAND_CMD_READ1, 0, page);

		/* Copy the output to the main driver area. */
		nfc_read_buf(mtd, buf, mtd->writesize);
		if (oob_required)
			/* copy the OOB area */
			nfc_read_buf(mtd, chip->oob_poi, mtd->oobsize);

		if (nand_page_is_empty(mtd, buf, NULL)) {
			memset(buf, 0xff, mtd->writesize);
			if (oob_required)
				memset(chip->oob_poi, 0xff, mtd->oobsize);
			/* success */
			eccstatus = 0;
		}
		else {
			/* ECC error. The number of bitflips is inessential */
			mtd->ecc_stats.failed++;
			/* keep the same negative value for eccstatus */
		}
	}

	return eccstatus;
}

int board_nand_init(struct nand_chip *nand)
{
	struct mtd_info* mtd;
	u32 ctl;
	int i, j;
	uint8_t id[8];
	struct nand_chip_param *nand_chip_param, *chip_param = NULL;

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
	ctl = (1 << 8);
	writel(ctl, NFC_REG_TIMING_CTL);

	// reset nand chip
	nfc_cmdfunc(NULL, NAND_CMD_RESET, -1, -1);
	// read nand chip id
	nfc_cmdfunc(NULL, NAND_CMD_READID, 0, -1);
	for (i = 0; i < 8; i++)
		id[i] = nfc_read_byte(NULL);

	// find chip
	nand_chip_param = sunxi_get_nand_chip_param(id[0]);
	for (i = 0; nand_chip_param[i].id_len; i++) {
		int find = 1;
		for (j = 0; j < nand_chip_param[i].id_len; j++) {
			if (id[j] != nand_chip_param[i].id[j]) {
				find = 0;
				break;
			}
		}
		if (find) {
			chip_param = &nand_chip_param[i];
			for (j = 0; j < nand_chip_param[i].id_len; j++) {
				printf("%x", nand_chip_param[i].id[j]);
			}
			printf(" - ");
			break;
		}
	}

	// not find
	if (chip_param == NULL) {
		printf("chip database lookup failed\n");
		return -ENODEV;
	}

	// set final NFC clock freq
	if (chip_param->clock_freq > 30)
		chip_param->clock_freq = 30;
	sunxi_nand_set_clock((int)chip_param->clock_freq * 1000000);

	// disable interrupt
	writel(0, NFC_REG_INT);
	// clear interrupt
	writel(readl(NFC_REG_ST), NFC_REG_ST);

	// set ECC mode
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_ECC_MODE;
	ctl |= (unsigned int)chip_param->ecc_mode << NFC_ECC_MODE_SHIFT;
	writel(ctl, NFC_REG_ECC_CTL);

	// enable NFC
	ctl = NFC_EN;

	// Page size
	if (chip_param->page_shift > 14 || chip_param->page_shift < 10) {
		printf("Page shift %d out of range\n", (int)chip_param->page_shift);
		return -EINVAL;
	}
	// 0 for 1K
	ctl |= (((int)chip_param->page_shift - 10) & 0xf) << 8;
	writel(ctl, NFC_REG_CTL);

	writel(0xff, NFC_REG_TIMING_CFG);
	writel((1U << chip_param->page_shift) + BB_MARK_SIZE,
	       NFC_REG_SPARE_AREA);

	// disable random
	disable_random();

	// setup ECC layout
	sunxi_ecclayout.eccbytes = 0;
	sunxi_ecclayout.oobavail =
		(1U << chip_param->page_shift) / 1024 * 4 - BB_MARK_SIZE;
	sunxi_ecclayout.oobfree->offset = BB_MARK_SIZE;
	sunxi_ecclayout.oobfree->length =
		(1U << chip_param->page_shift) / 1024 * 4 - BB_MARK_SIZE;
	nand->ecc.layout = &sunxi_ecclayout;
//	nand->ecc.size = 1U << chip_param->page_shift;
	nand->ecc.bytes = 0;

	// Temporary. Derived from the ID in nand_base.c:parse_hynix_sizes().
	nand->ecc.strength = 40;
	nand->ecc.size = 1024;

	// set buffer size: page size + max oob size
	buffer_size = (1U << chip_param->page_shift) + 2048;

	// setup DMA
	dma_hdle = DMA_Request(DMAC_DMATYPE_DEDICATED);
	if (dma_hdle == 0) {
		printf("DMA request failed\n");
		return -ENODEV;
	}
	print_nand_dma(dma_hdle);

	nand->ecc.mode = NAND_ECC_HW;
	nand->ecc.hwctl = nfc_ecc_hwctl;
	nand->ecc.calculate = nfc_ecc_calculate;
	nand->ecc.correct = nfc_ecc_correct;
	nand->ecc.read_page = nfc_read_page_hwecc;
	nand->select_chip = nfc_select_chip;
	nand->dev_ready = nfc_dev_ready;
	nand->cmdfunc = nfc_cmdfunc;
	nand->read_byte = nfc_read_byte;
	nand->read_buf = nfc_read_buf;
	nand->write_buf = nfc_write_buf;
	nand->waitfunc = nfc_wait;
	nand->bbt_options = NAND_BBT_USE_FLASH;
	nand->options = 0;

	mtd = &nand_info[0];
	mtd->priv = nand;

	return 0;
}
