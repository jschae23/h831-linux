// SPDX-License-Identifier: GPL-2.0
/*
 * MaxLinear MxL601 silicon tuner driver — H831 bring-up
 *
 * Copyright (C) 2026 AVerMedia H831 bring-up project
 *
 * Implementation derived from the publicly available MxLWare601
 * reference SDK (the vendor SDK itself is not redistributed here),
 * register map from its TunerCfg / TunerApi headers and register
 * tables from TunerCfg.c:
 *   - read protocol: write {0xFB, RegAddr} then separate 1-byte read
 *   - init chain   : SoftReset / OverwriteDefault / Xtal / IfOut /
 *                    Agc / ApplicationMode (DVB-T/T2 or DVB-C)
 *   - tune chain   : TUNE_MODE(0x0D), BW(0x0F), RF FCW(0x10/11),
 *                    PowerUp(0x0B), StartTune(0x12/0x6E/0x14)
 *
 * Open parameters (until verified with a vendor USB capture):
 *   - tuner crystal (16 or 24 MHz) comes from mxl601_config.xtal_hz
 *   - IF output frequency comes from mxl601_config.if_freq_hz and is
 *     programmed via the manual-IF-FCW path (register 0x04 bit5)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/dvb/frontend.h>
#include <media/dvb_frontend.h>
#include "mxl601.h"

/* ---- register map (MxL601_TunerCfg.h) ---- */
#define AIC_RESET_REG          0xFF /* write 0x00 = soft reset */
#define PAGE_CHANGE_REG        0x00 /* page 0/1 = bank switch */
#define XTAL_CAP_CTRL_REG      0x01 /* bit5 xtal sel, bit4:0 cap */
#define XTAL_ENABLE_DIV_REG    0x02 /* bit0 div4 */
#define XTAL_CALI_SET_REG      0x03 /* xtal calibration trigger */
#define IF_FREQ_SEL_REG        0x04 /* bit5 = manual IF FCW bypass */
#define IF_PATH_GAIN_REG       0x05 /* bit7:6 invert, 5:4 path, 3:0 gain */
#define IF_FCW_LOW_REG         0x06
#define IF_FCW_HIGH_REG        0x07
#define AGC_CONFIG_REG         0x08 /* bit3:2 type, bit0 sel */
#define AGC_SET_POINT_REG      0x09 /* bit6:0 set point */
#define AGC_FLIP_REG           0x5E /* bit4 polarity */
#define TUNER_ENABLE_REG       0x0B /* bit0 power up */
#define TUNE_MODE_REG          0x0D /* bit0: view/scan */
#define MAIN_REG_AMP           0x0E
#define CHAN_TUNE_BW_REG       0x0F
#define CHAN_TUNE_LOW_REG      0x10
#define CHAN_TUNE_HI_REG       0x11
#define START_TUNE_REG         0x12 /* bit0: start calibrations */
#define CHIP_ID_REQ_REG        0x18
#define CHIP_VERSION_REQ_REG   0x1A
#define RF_REF_STATUS_REG      0x2B /* bit0 ref lock, bit1 rf lock */
#define DIG_ANA_IF_CFG_0       0x5A
#define DIG_ANA_IF_CFG_1       0x5B
#define DIG_ANA_IF_PWR         0x5C
#define XTAL_EXT_BIAS_REG      0x6D
#define DFE_CSF_SS_SEL         0xEA
#define DFE_DACIF_GAIN         0xDC

/* SDK register aliases (identical addresses) */
#define DFE_RFLUT_SWP1_REG     0x49
#define DFE_RFLUT_DIV_MOD_REG  0xDB
#define DFE_REFLUT_BYP_REG     0xEA
#define DFE_REFSX_INT_MOD_REG  0xEB

/* bandwidth codes for digital terrestrial (written to 0x0F) */
#define DIG_TERR_BW_6MHZ       0x20
#define DIG_TERR_BW_7MHZ       0x21
#define DIG_TERR_BW_8MHZ       0x22

#define XTAL_16MHZ             0
#define XTAL_24MHZ             1

/* ---- register tables (MxL601_TunerCfg.c) ---- */

/* MxL_OverwriteDefaults (all masks 0xFF -> plain writes) */
static const u8 mxl601_overwrite_defaults[][2] = {
	{0x00, 0x01}, {0x96, 0x00}, {0x00, 0x00},
	{0x14, 0x13}, {0x6D, 0x8A}, {0x6D, 0x0A},
	{0xDF, 0x19}, {0x45, 0x1B}, {0xA9, 0x59}, {0xAA, 0x6A},
	{0xBE, 0x4C}, {0xCF, 0x25}, {0xD0, 0x34}, {0x77, 0xE7},
	{0x78, 0xE3}, {0x6F, 0x51}, {0x7B, 0x84}, {0x7C, 0x9F},
	{0x56, 0x41}, {0xCD, 0x64}, {0xC3, 0x2C}, {0x9D, 0x61},
	{0xF7, 0x52}, {0x58, 0x81}, {0xB1, 0x30},
};

/* DigitalDvbt (DVB-T/T2/DTMB application mode) */
static const u8 mxl601_dvbt_regs[][2] = {
	{0x13, 0x04}, {0x0C, 0x00}, {0x53, 0xFE}, {0x57, 0x91},
	{0x62, 0xC2}, {0x6E, 0x01}, {0x6F, 0x51}, {0x87, 0x77},
	{0x88, 0x55}, {0x93, 0x22}, {0x97, 0x02}, {0xBA, 0x30},
	{0x98, 0xAF}, {0x9B, 0x20}, {0x9C, 0x1E}, {0xA0, 0x18},
	{0xA5, 0x09}, {0xC2, 0xA9}, {0xC5, 0x7C}, {0xD5, 0x03},
	{0xD9, 0x04},
};

/* DigitalDvbc (DVB-C application mode) */
static const u8 mxl601_dvbc_regs[][2] = {
	{0x0C, 0x00}, {0x13, 0x04}, {0x53, 0x7E}, {0x57, 0x91},
	{0x5C, 0xB1}, {0x62, 0xF2}, {0x6E, 0x03}, {0x6F, 0xD1},
	{0x87, 0x77}, {0x88, 0x55}, {0x93, 0x33}, {0x97, 0x03},
	{0xBA, 0x40}, {0x98, 0xAF}, {0x9B, 0x20}, {0x9C, 0x1E},
	{0xA0, 0x18}, {0xA5, 0x09}, {0xC2, 0xA9}, {0xCD, 0x64},
	{0xCE, 0x7C}, {0xC5, 0x7C}, {0xD5, 0x05}, {0xD9, 0x00},
	{0xEA, 0x00}, {0xDC, 0x1C},
};

/*
 * NOTE on the RF LUT registers (0x49/0xDB/0xEA/0xEB, "spur table"):
 * the SDK only programs them inside MxL601_ConfigTunerChanTune's
 * analog branch (SignalMode <= ANA_SECAM_L, i.e. enum value 5, while
 * DIG_DVB_T_DTMB = 8) — the DIG_TERR_FREQ_LUT_* tables are dead code
 * in the reference. In digital modes the SDK therefore leaves 0xEA as
 * DFE_CSF_SS_SEL (0x0D for a 16 MHz XTAL, 0x0E for 24 MHz) as set by
 * the application-mode block, and never touches 0xEB. Writing a LUT
 * row here would blank DFE_CSF_SS_SEL and keep the reference PLL from
 * ever reporting lock.
 */

static bool ref_lut = true;
module_param(ref_lut, bool, 0644);
MODULE_PARM_DESC(ref_lut, "write 0xEA/0xEB (0x00/0xD8) at tune time like the LibreELEC mxl60x reference (default 1)");

static bool start_seq = false;
module_param(start_seq, bool, 0644);
MODULE_PARM_DESC(start_seq, "bracket the START_TUNE pulse with the MxL601 SDK 0x6E/0x14 xtal-clock-bypass dance (default 0 = mxl60x-style bare pulse)");

#define ARRAY_LEN(a)	(sizeof(a) / sizeof((a)[0]))

struct mxl601_priv {
	struct i2c_adapter *i2c;
	u8  i2c_addr;
	u8  xtal_sel;		/* XTAL_16MHZ / XTAL_24MHZ */
	u8  xtal_cap;		/* bit4:0 */
	u8  agc_type;		/* 0 = AGC_SELF, 1 = AGC_EXTERNAL */
	u8  agc_sel;		/* 0 = AGC2, 1 = AGC1 */
	u8  if_path;		/* 0/1, bits5:4 of IF_PATH_GAIN_REG */
	u8  if_gain;		/* 0..15, bits3:0 of IF_PATH_GAIN_REG */
	bool single_supply;	/* 3.3 V single supply board */
	bool xtal_sharing;	/* slave mode: clock from master chip */
	bool clk_out;		/* XTAL clock-out enable (0x01 bit7) */
	u32 if_req_hz;		/* requested IF output — immutable */
	u32 if_freq_hz;		/* IF output actually programmed */
	bool invert_if;
	bool dvb_c_mode;	/* last programmed application mode */
	u32 frequency;
	u32 bandwidth_hz;
};

/* ---- low level ---- */

static int mxl601_write_reg(struct mxl601_priv *priv, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = priv->i2c_addr, .flags = 0,
			       .len = 2, .buf = buf };
	int rc = i2c_transfer(priv->i2c, &msg, 1);

	if (rc != 1) {
		dev_warn(&priv->i2c->dev,
			 "mxl601: write reg 0x%02x failed (rc=%d)\n", reg, rc);
		return rc < 0 ? rc : -EIO;
	}
	return 0;
}

static int mxl601_read_reg(struct mxl601_priv *priv, u8 reg, u8 *val)
{
	/*
	 * MxL601 8-bit register-read protocol (MxL601_OEM_Drv.c):
	 *   write {0xFB, RegAddr} (0xFB = switch chip command mode to read)
	 *   then a separate read of 1 data byte
	 * The SDK diagrams a STOP between write and read, so issue them
	 * as two distinct transactions rather than a repeated start.
	 */
	u8 cmd[2] = { 0xFB, reg };
	struct i2c_msg msg[2] = {
		{ .addr = priv->i2c_addr, .flags = 0,
		  .len = 2, .buf = cmd },
		{ .addr = priv->i2c_addr, .flags = I2C_M_RD,
		  .len = 1, .buf = val },
	};
	int rc;

	rc = i2c_transfer(priv->i2c, &msg[0], 1);
	if (rc != 1) {
		dev_warn(&priv->i2c->dev,
			 "mxl601: read cmd 0x%02x failed (rc=%d)\n", reg, rc);
		return rc < 0 ? rc : -EIO;
	}
	rc = i2c_transfer(priv->i2c, &msg[1], 1);
	if (rc != 1) {
		dev_warn(&priv->i2c->dev,
			 "mxl601: read data 0x%02x failed (rc=%d)\n", reg, rc);
		return rc < 0 ? rc : -EIO;
	}
	return 0;
}

static int mxl601_write_table(struct mxl601_priv *priv,
			      const u8 table[][2], size_t count)
{
	int i, ret;

	for (i = 0; i < count; i++) {
		ret = mxl601_write_reg(priv, table[i][0], table[i][1]);
		if (ret)
			return ret;
	}
	return 0;
}

/* read-AND-value | value, matching the SDK's read/modify/write idiom */
static int mxl601_andor(struct mxl601_priv *priv, u8 reg, u8 and_mask, u8 or_val)
{
	u8 d = 0;
	int ret = mxl601_read_reg(priv, reg, &d);

	if (ret)
		return ret;
	d = (u8)((d & and_mask) | or_val);
	return mxl601_write_reg(priv, reg, d);
}

/* ---- init chain (MxLWare601_API_ConfigDevice / _ConfigTuner) ---- */

static int mxl601_config_if_out(struct mxl601_priv *priv)
{
	/* Manual IF frequency set: bit5 of IF_FREQ_SEL_REG */
	int ret;
	u16 if_fcw;
	u32 if_khz = priv->if_req_hz / 1000;

	ret = mxl601_andor(priv, IF_FREQ_SEL_REG, 0xFF, 0x20);
	if (ret)
		return ret;

	/* FCW = IF[kHz] * 8192 / 216000 (MxL601_ConfigDevIfOutSet).
	 * always recomputed from the immutable if_req_hz — a stored
	 * quantized value would drift on succeeding re-configs */
	if_fcw = (u16)(if_khz * 8192 / 216000);
	ret = mxl601_write_reg(priv, IF_FCW_LOW_REG, if_fcw & 0xFF);
	if (ret)
		return ret;
	ret = mxl601_write_reg(priv, IF_FCW_HIGH_REG, (if_fcw >> 8) & 0x0F);
	if (ret)
		return ret;

	/* store the quantized value; the demod programs its own IF
	 * registers from get_if_frequency() */
	priv->if_freq_hz = DIV_ROUND_CLOSEST_ULL((u64)if_fcw * 216000000ULL,
						 8192ULL);

	dev_dbg(&priv->i2c->dev,
		"mxl601: IF requested=%u Hz -> fcw=%u -> actual=%u Hz\n",
		priv->if_req_hz, if_fcw, priv->if_freq_hz);

	/* spectrum invert bits7:6, path bits5:4, gain bits3:0 */
	return mxl601_write_reg(priv, IF_PATH_GAIN_REG,
		(priv->invert_if ? 0xC0 : 0x00) |
		((priv->if_path & 0x03) << 4) |
		(priv->if_gain & 0x0F));
}

static int mxl601_config_agc(struct mxl601_priv *priv)
{
	int ret;

	/* type bits3:2 (0 = AGC_SELF internal loop, 1 = AGC_EXTERNAL
	 * closed loop via AGC pin), sel bit0 (0 = AGC2, 1 = AGC1) */
	ret = mxl601_andor(priv, AGC_CONFIG_REG, 0xF2,
			   ((priv->agc_type & 0x03) << 2) |
			   (priv->agc_sel & 0x01));
	if (ret)
		return ret;
	/* AGC set point = 66 (SDK integration default) */
	ret = mxl601_andor(priv, AGC_SET_POINT_REG, 0x80, 66);
	if (ret)
		return ret;
	/* AGC polarity bit4 */
	return mxl601_andor(priv, AGC_FLIP_REG, 0xEF, 0x00);
}

/* application mode: DigitalDvbt (T/T2/DTMB) or DigitalDvbc */
static int mxl601_config_mode(struct mxl601_priv *priv, bool dvb_c)
{
	const u8 (*table)[2];
	size_t count;
	int ret;

	if (dvb_c) {
		table = mxl601_dvbc_regs;
		count = ARRAY_LEN(mxl601_dvbc_regs);
	} else {
		table = mxl601_dvbt_regs;
		count = ARRAY_LEN(mxl601_dvbt_regs);
	}

	ret = mxl601_write_table(priv, table, count);
	if (ret)
		return ret;

	/* our manual IF is always far below the 35.25 MHz high-IF
	 * threshold -> low-IF dependent settings */
	if (dvb_c) {
		ret = mxl601_write_reg(priv, DIG_ANA_IF_CFG_0, 0xFE);
		if (ret)
			return ret;
		ret = mxl601_write_reg(priv, DIG_ANA_IF_CFG_1, 0x10);
		if (ret)
			return ret;
	} else {
		ret = mxl601_write_reg(priv, DIG_ANA_IF_CFG_0, 0xFE);
		if (ret)
			return ret;
		ret = mxl601_write_reg(priv, DIG_ANA_IF_CFG_1, 0x18);
		if (ret)
			return ret;
		ret = mxl601_write_reg(priv, DIG_ANA_IF_PWR, 0xF1);
		if (ret)
			return ret;
	}

	/* DFE_CSF_SS_SEL: xtal dependent */
	ret = mxl601_write_reg(priv, DFE_CSF_SS_SEL,
			       priv->xtal_sel == XTAL_16MHZ ? 0x0D : 0x0E);
	if (ret)
		return ret;

	/* DFE_DACIF_GAIN per IF gain level (default 0 for Dvbt, 0x1C
	 * preset already in the Dvbc table) */
	if (!dvb_c) {
		ret = mxl601_write_reg(priv, DFE_DACIF_GAIN, 0x00);
		if (ret)
			return ret;
	}

	return 0;
}

/* full configuration chain — run once at tuner init */
static int mxl601_configure(struct mxl601_priv *priv)
{
	int ret;

	/* soft reset */
	ret = mxl601_write_reg(priv, AIC_RESET_REG, 0x00);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	/* overwrite defaults (page-1 writes embedded) */
	ret = mxl601_write_table(priv, mxl601_overwrite_defaults,
				 ARRAY_LEN(mxl601_overwrite_defaults));
	if (ret)
		return ret;
	/* page-1 0x31 bias update (d &= 0x2F; d |= 0xD0): the SDK performs
	 * this RMW with page 1 explicitly selected — the overwrite table
	 * above has already flipped the page back to 0, so select page 1
	 * ourselves, then restore */
	ret = mxl601_write_reg(priv, PAGE_CHANGE_REG, 0x01);
	if (ret)
		return ret;
	ret = mxl601_andor(priv, 0x31, 0x2F, 0xD0);
	if (ret)
		return ret;
	ret = mxl601_write_reg(priv, PAGE_CHANGE_REG, 0x00);
	if (ret)
		return ret;

	/* single 3.3 V supply board: regulator re-program (SDK) */
	if (priv->single_supply) {
		ret = mxl601_write_reg(priv, MAIN_REG_AMP, 0x04);
		if (ret)
			return ret;
	}

	/* xtal set: freq sel bit5, cap bit4:0, clk-out enable bit7
	 * (MxL601_ConfigDevXtalSet) */
	ret = mxl601_write_reg(priv, XTAL_CAP_CTRL_REG,
			       (priv->clk_out ? 0x80 : 0x00) |
			       (priv->xtal_sel << 5) | (priv->xtal_cap & 0x1F));
	if (ret)
		return ret;
	/* clock div / sharing: SDK XtalSet — sharing slave mode sets
	 * 0x02 bit6 and 0x6D = 0x80; own-crystal mode 0x02 bit0 and
	 * 0x6D = 0x0A */
	if (priv->xtal_sharing) {
		ret = mxl601_write_reg(priv, XTAL_ENABLE_DIV_REG, 0x40);
		if (ret)
			return ret;
		ret = mxl601_write_reg(priv, XTAL_EXT_BIAS_REG, 0x80);
	} else {
		ret = mxl601_write_reg(priv, XTAL_ENABLE_DIV_REG, 0x00);
		if (ret)
			return ret;
		ret = mxl601_write_reg(priv, XTAL_EXT_BIAS_REG, 0x0A);
	}
	if (ret)
		return ret;
	/* single-supply: MAIN_REG_AMP = 0x14 (SDK XtalSet) */
	if (priv->single_supply) {
		ret = mxl601_write_reg(priv, MAIN_REG_AMP, 0x14);
		if (ret)
			return ret;
	}

	/* power mode ACTIVE */
	ret = mxl601_write_reg(priv, TUNER_ENABLE_REG, 0x01);
	if (ret)
		return ret;
	ret = mxl601_write_reg(priv, START_TUNE_REG, 0x01);
	if (ret)
		return ret;

	ret = mxl601_config_if_out(priv);
	if (ret)
		return ret;

	ret = mxl601_config_agc(priv);
	if (ret)
		return ret;

	ret = mxl601_config_mode(priv, false);
	if (ret)
		return ret;
	priv->dvb_c_mode = false;

	/* xtal calibration trigger */
	ret = mxl601_write_reg(priv, XTAL_CALI_SET_REG, 0x00);
	if (ret)
		return ret;
	ret = mxl601_write_reg(priv, XTAL_CALI_SET_REG, 0x01);

	/* 50 ms sleep after XTAL calibration (SDK requirement) */
	msleep(50);
	if (ret)
		return ret;

	/*
	 * Archived findings from the earlier (broken) spur-LUT port:
	 * once the 0x49/0xDB/0xEA/0xEB LUT row was programmed here and
	 * per tune, the REF PLL never locked (0x2B stuck at 0x04). See
	 * the NOTE above the mode tables: in digital modes the SDK
	 * never writes these registers, DFE_CSF_SS_SEL stays 0x0D/0x0E.
	 */

	return 0;
}

/* ---- dvb_frontend tuner ops ---- */

static int mxl601_init(struct dvb_frontend *fe)
{
	struct mxl601_priv *priv = fe->tuner_priv;
	int ret;

	dev_dbg(&priv->i2c->dev, "mxl601: init\n");

	ret = mxl601_configure(priv);
	if (!ret)
		priv->frequency = 0;

	return ret;
}

static int mxl601_sleep(struct dvb_frontend *fe)
{
	struct mxl601_priv *priv = fe->tuner_priv;
	int ret;

	dev_dbg(&priv->i2c->dev, "mxl601: sleep\n");

	/* enter standby (SDK ConfigDevPowerMode STANDBY) */
	ret = mxl601_write_reg(priv, START_TUNE_REG, 0x00);
	if (ret)
		return ret;
	return mxl601_write_reg(priv, TUNER_ENABLE_REG, 0x00);
}

/*
 * Tune sequence follows the LibreELEC mxl60x driver (mxl60x_set_freq),
 * which is derived from the vendor flow, rather than the
 * generic SDK ChanTune (whose VCO-band and 0xEA/0xEB writes are gated
 * to analog modes but are applied for digital modes by shipping drivers).
 */
static int mxl601_set_params(struct dvb_frontend *fe)
{
	struct mxl601_priv *priv = fe->tuner_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	bool dvb_c = c->delivery_system == SYS_DVBC_ANNEX_A;
	u8 bw_byte, d = 0, d1 = 0, d2 = 0, d3 = 0, vco31, ea, eb;
	u32 fcw;
	int ret;

	dev_dbg(&priv->i2c->dev,
		"mxl601: set_params sys=%u freq=%u bw_hz=%u\n",
		c->delivery_system, c->frequency, c->bandwidth_hz);

	switch (c->bandwidth_hz) {
	case 6000000: bw_byte = DIG_TERR_BW_6MHZ; break;
	case 7000000: bw_byte = DIG_TERR_BW_7MHZ; break;
	case 8000000: bw_byte = DIG_TERR_BW_8MHZ; break;
	default:
		return -EINVAL;
	}

	if (dvb_c != priv->dvb_c_mode) {
		ret = mxl601_config_mode(priv, dvb_c);
		if (ret)
			return ret;
		priv->dvb_c_mode = dvb_c;
	}

	/* abort any running calibration */
	ret = mxl601_write_reg(priv, START_TUNE_REG, 0x00);
	if (ret)
		return ret;

	/* VCO band: 0x7C and page-1 0x31 depend on RF < / >= 700 MHz */
	if (c->frequency < 700000000) {
		ret = mxl601_write_reg(priv, 0x7C, 0x1F);
		vco31 = dvb_c ? 0xC1 : 0x81;
	} else {
		ret = mxl601_write_reg(priv, 0x7C, 0x9F);
		vco31 = dvb_c ? 0xD1 : 0x91;
	}
	if (ret)
		return ret;
	if ((ret = mxl601_write_reg(priv, PAGE_CHANGE_REG, 0x01)) ||
	    (ret = mxl601_write_reg(priv, 0x31, vco31)) ||
	    (ret = mxl601_write_reg(priv, PAGE_CHANGE_REG, 0x00)))
		return ret;

	/* REF LUT default row for digital modes (0xEA/0xEB) */
	if (ref_lut) {
		ea = 0x00;
		eb = 0xD8;
		if (dvb_c && abs((int)c->frequency - 695000000) <= 500000) {
			ea = 0x20;
			eb = 0xD7;
		}
		if ((ret = mxl601_write_reg(priv, DFE_CSF_SS_SEL, ea)) ||
		    (ret = mxl601_write_reg(priv, DFE_REFSX_INT_MOD_REG, eb)))
			return ret;
	}

	ret = mxl601_andor(priv, TUNE_MODE_REG, 0xFE, 0x00);
	if (ret)
		return ret;
	ret = mxl601_write_reg(priv, CHAN_TUNE_BW_REG, bw_byte);
	if (ret)
		return ret;

	/* RF channel = round(freq_kHz * 64 / 1000) */
	fcw = DIV_ROUND_CLOSEST(c->frequency / 1000 * 64, 1000);
	if ((ret = mxl601_write_reg(priv, CHAN_TUNE_LOW_REG, fcw & 0xFF)) ||
	    (ret = mxl601_write_reg(priv, CHAN_TUNE_HI_REG, (fcw >> 8) & 0xFF)) ||
	    (ret = mxl601_write_reg(priv, TUNER_ENABLE_REG, 0x01)))
		return ret;

	priv->frequency = c->frequency;
	priv->bandwidth_hz = c->bandwidth_hz;

	/* front-end (LNA / loop-through) setup, as in the reference */
	if ((ret = mxl601_write_reg(priv, PAGE_CHANGE_REG, 0x01)) ||
	    (ret = mxl601_read_reg(priv, 0x96, &d)) ||
	    (ret = mxl601_write_reg(priv, PAGE_CHANGE_REG, 0x00)) ||
	    (ret = mxl601_read_reg(priv, 0xB6, &d1)) ||
	    (ret = mxl601_write_reg(priv, PAGE_CHANGE_REG, 0x01)) ||
	    (ret = mxl601_read_reg(priv, 0x60, &d2)) ||
	    (ret = mxl601_read_reg(priv, 0x5F, &d3)))
		return ret;

	if (d & 0x10) {
		d2 = (d2 & 0xC0) | 0x0E;
		d3 = (d3 & 0xC0) | 0x0E;
	} else {
		d2 = (d2 & 0xC0) | 0x37;
		d3 = (d3 & 0xC0) | 0x37;
	}
	if ((ret = mxl601_write_reg(priv, 0x60, d2)) ||
	    (ret = mxl601_write_reg(priv, 0x5F, d3)) ||
	    (ret = mxl601_write_reg(priv, PAGE_CHANGE_REG, 0x00)) ||
	    (ret = mxl601_write_reg(priv, 0xB6, d)))
		return ret;

	/*
	 * Start the calibrations. MxL601's SDK brackets the START_TUNE
	 * pulse (0x12<0> = 1) with 0x6E<7> DIG_ANA_XTAL_CLK_EN_BYPS = 1
	 * and 0x14<5> = 0, resumed after 10 ms (MxL601_ConfigTunerSequenceSet);
	 * the mxl60x reference (MxL608) pulses 0x12 bare. Swept live via
	 * the start_seq module param (no replug needed).
	 */
	if (start_seq) {
		if ((ret = mxl601_andor(priv, 0x6E, 0xFF, 0x80)) ||
		    (ret = mxl601_andor(priv, 0x14, 0xDF, 0x00)) ||
		    (ret = mxl601_write_reg(priv, START_TUNE_REG, 0x01)))
			return ret;
		msleep(10);
		if ((ret = mxl601_andor(priv, 0x6E, 0x7F, 0x00)))
			return ret;
	} else {
		if ((ret = mxl601_write_reg(priv, START_TUNE_REG, 0x01)))
			return ret;
	}

	msleep(20);
	if ((ret = mxl601_write_reg(priv, 0xB6, d | 0x40)))
		return ret;
	msleep(20);

	/* diagnostic: poll synth lock (0x2B == 0x07 when both locked) */
	{
		u32 waited = 0;
		u8 st = 0;
		int rr;

		do {
			rr = mxl601_read_reg(priv, RF_REF_STATUS_REG, &st);
			if (!rr && (st & 0x03) == 0x03)
				break;
			msleep(20);
			waited += 20;
		} while (waited < 500);

		if (rr)
			dev_warn(&priv->i2c->dev,
				 "mxl601: lock status read failed (%d)\n", rr);
		else
			dev_info(&priv->i2c->dev,
				 "mxl601: 0x2B=0x%02x after %u ms (ref=%u rf=%u)\n",
				 st, waited, !!(st & 1), !!(st & 2));
	}

	return 0;
}

static int mxl601_get_frequency(struct dvb_frontend *fe, u32 *freq)
{
	struct mxl601_priv *priv = fe->tuner_priv;

	*freq = priv->frequency;
	return 0;
}

static int mxl601_get_bandwidth(struct dvb_frontend *fe, u32 *bw)
{
	struct mxl601_priv *priv = fe->tuner_priv;

	*bw = priv->bandwidth_hz;
	return 0;
}

/* required by mn88473: programs its own internal IF mixer from this */
static int mxl601_get_if_frequency(struct dvb_frontend *fe, u32 *freq)
{
	struct mxl601_priv *priv = fe->tuner_priv;

	*freq = priv->if_freq_hz;
	return 0;
}

static int mxl601_get_status(struct dvb_frontend *fe, u32 *status)
{
	struct mxl601_priv *priv = fe->tuner_priv;
	u8 d = 0;
	int ret;

	*status = 0;
	ret = mxl601_read_reg(priv, RF_REF_STATUS_REG, &d);
	if (ret)
		return ret;

	if ((d & 0x01) && (d & 0x02))
		*status |= TUNER_STATUS_LOCKED;

	return 0;
}

static void mxl601_release(struct dvb_frontend *fe)
{
	struct mxl601_priv *priv = fe->tuner_priv;

	kfree(priv);
	fe->tuner_priv = NULL;
}

static const struct dvb_tuner_ops mxl601_tuner_ops = {
	.info = {
		.name             = "MaxLinear MxL601",
		.frequency_min_hz =  42 * 1000 * 1000,
		.frequency_max_hz = 900 * 1000 * 1000,
		.frequency_step_hz = 62500,
	},
	.release           = mxl601_release,
	.init              = mxl601_init,
	.sleep             = mxl601_sleep,
	.set_params        = mxl601_set_params,
	.get_frequency     = mxl601_get_frequency,
	.get_bandwidth     = mxl601_get_bandwidth,
	.get_if_frequency  = mxl601_get_if_frequency,
	.get_status        = mxl601_get_status,
};

struct dvb_frontend *mxl601_attach(struct dvb_frontend *fe,
				  struct i2c_adapter *i2c,
				  const struct mxl601_config *cfg)
{
	struct mxl601_priv *priv;
	u8 chip_id = 0, chip_ver = 0;
	int rc;

	if (!cfg->fe || !*cfg->fe) {
		dev_dbg(&i2c->dev, "mxl601: no frontend\n");
		return NULL;
	}

	/* probe: read chip id/version registers */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;
	priv->i2c = i2c;
	priv->i2c_addr = cfg->i2c_addr;

	rc = mxl601_read_reg(priv, CHIP_ID_REQ_REG, &chip_id);
	dev_info(&i2c->dev,
		"mxl601: attach at 0x%02x: read CHIP_ID rc=%d chip_id=0x%02x\n",
		cfg->i2c_addr, rc, chip_id);
	if (rc || chip_id == 0x00 || chip_id == 0xff) {
		dev_info(&i2c->dev,
			"mxl601: no response at 0x%02x (chip_id=0x%02x, rc=%d)\n",
			cfg->i2c_addr, chip_id, rc);
		kfree(priv);
		return NULL;
	}
	rc = mxl601_read_reg(priv, CHIP_VERSION_REQ_REG, &chip_ver);

	dev_info(&i2c->dev,
		 "mxl601: MxL601 detected at 0x%02x: chip id 0x%02x version 0x%02x\n",
		 cfg->i2c_addr, chip_id, chip_ver);

	/* xtal frequency: only 16 or 24 MHz supported by the chip */
	switch (cfg->xtal_hz) {
	case 16000000:
		priv->xtal_sel = XTAL_16MHZ;
		break;
	case 24000000:
		priv->xtal_sel = XTAL_24MHZ;
		break;
	default:
		dev_warn(&i2c->dev,
			"mxl601: unknown xtal_hz=%u, using 24 MHz\n",
			cfg->xtal_hz);
		priv->xtal_sel = XTAL_24MHZ;
	}
	priv->xtal_cap = cfg->xtal_cap ? cfg->xtal_cap & 0x1F : 12; /* SDK integration default */
	priv->if_req_hz = cfg->if_freq_hz;
	priv->if_freq_hz = cfg->if_freq_hz; /* initial value; config_if_out
					     * quantizes from if_req_hz */
	priv->invert_if = cfg->invert_if;
	priv->agc_type = cfg->agc_mode & 0x03;  /* 0 self, 1 external */
	priv->agc_sel  = cfg->agc_sel & 0x01;   /* 0 AGC2, 1 AGC1 */
	priv->if_path  = cfg->if_path & 0x03;
	if (!priv->if_path)
		dev_warn(&i2c->dev,
			 "mxl601: if_path 0 disables both IF outputs (valid: 1, 2)\n");
	priv->if_gain  = cfg->if_gain & 0x0F;
	priv->single_supply = cfg->single_supply;
	priv->xtal_sharing = cfg->xtal_sharing;
	priv->clk_out = cfg->clk_out;

	dev_info(&i2c->dev,
		 "mxl601: cfg: xtal=%u cap=%u if=%u inv=%d agc(mode=%d sel=%d) path=%d gain=%d supply3v3=%d sharing=%d clk_out=%d\n",
		 cfg->xtal_hz, priv->xtal_cap, cfg->if_freq_hz,
		 priv->invert_if ? 1 : 0, priv->agc_type, priv->agc_sel,
		 priv->if_path, priv->if_gain,
		 priv->single_supply ? 1 : 0, priv->xtal_sharing ? 1 : 0,
		 priv->clk_out ? 1 : 0);

	fe->tuner_priv = priv;
	fe->ops.tuner_ops = mxl601_tuner_ops;

	return fe;
}
EXPORT_SYMBOL_GPL(mxl601_attach);

MODULE_DESCRIPTION("MaxLinear MxL601 tuner driver (AVerMedia H831)");
MODULE_AUTHOR("AVerMedia H831 bring-up project");
MODULE_LICENSE("GPL");
