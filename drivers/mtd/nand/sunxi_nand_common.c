/*
 * sunxi_nand_common.c
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

int dma_hdle;

static const uint16_t random_seed[128] = {
	0x2b75, 0x0bd0, 0x5ca3, 0x62d1, 0x1c93, 0x07e9, 0x2162, 0x3a72,
	0x0d67, 0x67f9, 0x1be7, 0x077d, 0x032f, 0x0dac, 0x2716, 0x2436,
	0x7922, 0x1510, 0x3860, 0x5287, 0x480f, 0x4252, 0x1789, 0x5a2d,
	0x2a49, 0x5e10, 0x437f, 0x4b4e, 0x2f45, 0x216e, 0x5cb7, 0x7130,
	0x2a3f, 0x60e4, 0x4dc9, 0x0ef0, 0x0f52, 0x1bb9, 0x6211, 0x7a56,
	0x226d, 0x4ea7, 0x6f36, 0x3692, 0x38bf, 0x0c62, 0x05eb, 0x4c55,
	0x60f4, 0x728c, 0x3b6f, 0x2037, 0x7f69, 0x0936, 0x651a, 0x4ceb,
	0x6218, 0x79f3, 0x383f, 0x18d9, 0x4f05, 0x5c82, 0x2912, 0x6f17,
	0x6856, 0x5938, 0x1007, 0x61ab, 0x3e7f, 0x57c2, 0x542f, 0x4f62,
	0x7454, 0x2eac, 0x7739, 0x42d4, 0x2f90, 0x435a, 0x2e52, 0x2064,
	0x637c, 0x66ad, 0x2c90, 0x0bad, 0x759c, 0x0029, 0x0986, 0x7126,
	0x1ca7, 0x1605, 0x386a, 0x27f5, 0x1380, 0x6d75, 0x24c3, 0x0f8e,
	0x2b7a, 0x1418, 0x1fd1, 0x7dc1, 0x2d8e, 0x43af, 0x2267, 0x7da3,
	0x4e3d, 0x1338, 0x50db, 0x454d, 0x764d, 0x40a3, 0x42e6, 0x262b,
	0x2d2e, 0x1aea, 0x2e17, 0x173d, 0x3a6e, 0x71bf, 0x25f9, 0x0a5d,
	0x7c57, 0x0fbe, 0x46ce, 0x4939, 0x6b17, 0x37bb, 0x3e91, 0x76db
};

void sunxi_nand_set_clock(int hz)
{
	struct sunxi_ccm_reg *const ccm =
		(struct sunxi_ccm_reg *)SUNXI_CCM_BASE;
	int clock = clock_get_pll5();
	int edo_clk = hz * 2;
	int div_n = 0, div_m;
	int nand_clk_divid_ratio = clock / edo_clk;

	if (clock % edo_clk)
		nand_clk_divid_ratio++;
	for (div_m = nand_clk_divid_ratio; div_m > 16 && div_n < 3; div_n++) {
		if (div_m % 2)
			div_m++;
		div_m >>= 1;
	}
	div_m--;
	if (div_m > 15)
		div_m = 15;	/* Overflow */

	/* nand clock source is PLL5 */
	/* TODO: define proper clock sources for NAND reg */
	clrsetbits_le32(&ccm->nand_sclk_cfg, 3 << 24, 2 << 24); /* 2 = PLL5 */
	clrsetbits_le32(&ccm->nand_sclk_cfg, 3 << 16, div_n << 16);
	clrsetbits_le32(&ccm->nand_sclk_cfg, 0xf << 0, div_m << 0);
	setbits_le32(&ccm->nand_sclk_cfg, (1 << 31));
	/* open clock for nand and DMA*/
	setbits_le32(&ccm->ahb_gate0, (1 << AHB_GATE_OFFSET_NAND) | (1 << AHB_GATE_OFFSET_DMA));
	debug("NAND Clock: PLL5=%dHz, divid_ratio=%d, n=%d m=%d, clock=%dHz (target %dHz\n",
		  clock, nand_clk_divid_ratio, div_n, div_m, (clock>>div_n)/(div_m+1), hz);
}

void sunxi_nand_set_gpio(void)
{
	struct sunxi_gpio *pio =
		&((struct sunxi_gpio_reg *)SUNXI_PIO_BASE)->gpio_bank[2];
	int i;

	for (i = 0; i < 3; i++)
		writel(0x22222222, &pio->cfg[i]);
}

void select_rb(int rb)
{
	uint32_t ctl;
	// A10 has 2 RB pin
	ctl = readl(NFC_REG_CTL);
	ctl &= ~NFC_RB_SEL;
	ctl |= ((rb & 0x1) << 3);
	writel(ctl, NFC_REG_CTL);
}

void enable_random_preset(void)
{
	uint32_t ctl;
	ctl = readl(NFC_REG_ECC_CTL);
	ctl |= NFC_RANDOM_EN;
	ctl &= ~NFC_RANDOM_DIRECTION;
	ctl &= ~NFC_RANDOM_SEED;
	ctl |= 0x4a80 << 16;
	writel(ctl, NFC_REG_ECC_CTL);
}

void enable_random(uint32_t page)
{
	uint32_t ctl;

	ctl = readl(NFC_REG_ECC_CTL);
	ctl |= NFC_RANDOM_EN;
	ctl &= ~NFC_RANDOM_DIRECTION;
	ctl &= ~NFC_RANDOM_SEED;
	ctl |= (uint32_t) random_seed[page % 128] << 16;
	writel(ctl, NFC_REG_ECC_CTL);
}

void disable_random(void)
{
	uint32_t ctl;
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_RANDOM_EN;
	writel(ctl, NFC_REG_ECC_CTL);
}

void enable_ecc(int pipeline)
{
	uint32_t cfg = readl(NFC_REG_ECC_CTL);
	if (pipeline)
		cfg |= NFC_ECC_PIPELINE;
	else
		cfg &= (~NFC_ECC_PIPELINE) & 0xffffffff;

	// if random open, disable exception
	if(cfg & (1 << 9))
	    cfg &= ~(0x1 << 4);
	else
	    cfg |= 1 << 4;

	//cfg |= (1 << 1); 16 bit ecc

	cfg |= NFC_ECC_EN;
	writel(cfg, NFC_REG_ECC_CTL);
}

int check_ecc(int eblock_cnt)
{
	int i;
	int ecc_mode;
	int max_ecc_bit_cnt = 16;
	int cfg, corrected = 0;

	ecc_mode = (readl(NFC_REG_ECC_CTL) & NFC_ECC_MODE) >> NFC_ECC_MODE_SHIFT;

	switch (ecc_mode) {
	case 4:
		max_ecc_bit_cnt = 40;
		break;
	case 5:
		max_ecc_bit_cnt = 48;
		break;
	case 6:
		max_ecc_bit_cnt = 56;
		break;
	case 7:
		max_ecc_bit_cnt = 60;
		break;
	case 8:
		max_ecc_bit_cnt = 64;
		break;
	case 3:
		max_ecc_bit_cnt = 32;
		break;
	case 2:
		max_ecc_bit_cnt = 28;
		break;
	case 1:
		max_ecc_bit_cnt = 24;
		break;
	case 0:
	default:
		max_ecc_bit_cnt = 16;
		break;
	}

	//check ecc error
	cfg = readl(NFC_REG_ECC_ST) & 0xffff;
	for (i = 0; i < eblock_cnt; i++) {
		if (cfg & (1<<i)) {
			/* ECC error in sector i */
			return -1;
		}
	}

	//check ecc limit
	for (i = 0; i < eblock_cnt; i += 4) {
		int j, n = (eblock_cnt - i) < 4 ? (eblock_cnt - i) : 4;
		cfg = readl(NFC_REG_ECC_CNT0 + i);
		for (j = 0; j < n; j++, cfg >>= 8) {
			int bits = cfg & 0xff;
			if (bits >= max_ecc_bit_cnt - 4) {
				debug("ECC limit %d/%d\n", bits, max_ecc_bit_cnt);
				corrected++;
			}
		}
	}

	return corrected;
}

void set_ecc_mode(int mode)
{
	uint32_t ctl;
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_ECC_MODE;
	ctl |= mode << NFC_ECC_MODE_SHIFT;
	writel(ctl, NFC_REG_ECC_CTL);
}

void disable_ecc(void)
{
	uint32_t cfg = readl(NFC_REG_ECC_CTL);
	cfg &= (~NFC_ECC_EN) & 0xffffffff;
	writel(cfg, NFC_REG_ECC_CTL);
}

void _dma_config_start(__u32 rw, __u32 src_addr, __u32 dst_addr, __u32 len)
{
	__dma_setting_t   dma_param;

	if(rw)  //write
	{
		if(src_addr & 0xC0000000) //DRAM
			dma_param.cfg.src_drq_type = DMAC_CFG_SRC_TYPE_D_SDRAM;
		else  //SRAM
			dma_param.cfg.src_drq_type = DMAC_CFG_SRC_TYPE_D_SRAM;
		dma_param.cfg.src_addr_type = DMAC_CFG_SRC_ADDR_TYPE_LINEAR_MODE;  //linemode
		dma_param.cfg.src_burst_length = DMAC_CFG_SRC_4_BURST;  //burst mode
		dma_param.cfg.src_data_width = DMAC_CFG_SRC_DATA_WIDTH_32BIT;  //32bit

		dma_param.cfg.dst_drq_type = DMAC_CFG_DEST_TYPE_NFC;
		dma_param.cfg.dst_addr_type = DMAC_CFG_DEST_ADDR_TYPE_IO_MODE; //IO mode
		dma_param.cfg.dst_burst_length = DMAC_CFG_DEST_4_BURST; // burst mode
		dma_param.cfg.dst_data_width = DMAC_CFG_DEST_DATA_WIDTH_32BIT; //32 bit

		dma_param.cfg.wait_state = DMAC_CFG_WAIT_1_DMA_CLOCK; // invalid value
		dma_param.cfg.continuous_mode = DMAC_CFG_CONTINUOUS_DISABLE; //no continous

		dma_param.cmt_blk_cnt =  0x7f077f07; //commit register

	}
	else //read
	{
		dma_param.cfg.src_drq_type = DMAC_CFG_SRC_TYPE_NFC;
		dma_param.cfg.src_addr_type = DMAC_CFG_SRC_ADDR_TYPE_IO_MODE;  //IO mode
		dma_param.cfg.src_burst_length = DMAC_CFG_SRC_4_BURST;  //burst mode
		dma_param.cfg.src_data_width = DMAC_CFG_SRC_DATA_WIDTH_32BIT;  //32bit

		if(dst_addr & 0xC0000000) //DRAM
			dma_param.cfg.dst_drq_type = DMAC_CFG_DEST_TYPE_D_SDRAM;
		else  //SRAM
			dma_param.cfg.dst_drq_type = DMAC_CFG_DEST_TYPE_D_SRAM;
		dma_param.cfg.dst_addr_type = DMAC_CFG_DEST_ADDR_TYPE_LINEAR_MODE; //line mode
		dma_param.cfg.dst_burst_length = DMAC_CFG_DEST_4_BURST; // burst mode
		dma_param.cfg.dst_data_width = DMAC_CFG_DEST_DATA_WIDTH_32BIT; //32 bit

		dma_param.cfg.wait_state = DMAC_CFG_WAIT_1_DMA_CLOCK; // invalid value
		dma_param.cfg.continuous_mode = DMAC_CFG_CONTINUOUS_DISABLE; //no continous

		dma_param.cmt_blk_cnt =  0x7f077f07; //commit register
	}

	DMA_Setting(dma_hdle, (void *)&dma_param);

	if (rw) {
		debug("write flush src\n");
		flush_cache(src_addr, len);
	}
	else {
		debug("read flush dst\n");
		flush_cache(dst_addr, len);
	}

	DMA_Start(dma_hdle, src_addr, dst_addr, len);
}


__s32 _wait_dma_end(void)
{
	__s32 timeout = 0xffff;

	while ((timeout--) && DMA_QueryStatus(dma_hdle));
	if (timeout <= 0)
		return -1;

	return 0;
}
