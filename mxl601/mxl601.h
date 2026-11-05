/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MaxLinear MxL601 silicon tuner driver (first-cut / bring-up version)
 *
 * Derived from publicly available MxLWare601 reference material
 * (AVL6862 SDK, tuner/MxL601) and the LibreELEC mxl60x kernel patch.
 * Ported to Linux DVB dev framework in clean form for the
 * AVerMedia H831 (07ca:1831) bring-up.
 */
#ifndef MXL601_H
#define MXL601_H

#include <media/dvb_frontend.h>

#if IS_ENABLED(CONFIG_MEDIA_CONTROLLER_DVB)
#include <media/media-device.h>
#endif

/**
 * struct mxl601_config - mxl601 tuner configuration
 * @fe : frontend pointer to attach the tuner to
 * @i2c_addr : 7-bit I2C address of the tuner
 * @xtal_hz : crystal frequency in Hz
 * @if_freq_hz : IF output frequency in Hz (what demodulator expects)
 * @invert_if : true if IF is inverted
 * @clkout_smd : clock output setup for crystalless operation
 * @clk_out :enable the MxL601 XTAL clock-out (reg 0x01 bit7; SDK
 *           reference integration defaults to enable)
 */
struct mxl601_config {
	u8 i2c_addr;          /* 7-bit I2C slave address (0x60 for H831) */
	unsigned int xtal_hz;
	unsigned int xtal_cap;    /* bit4:0, default 12 (SDK integration) */
	unsigned int if_freq_hz;
	bool invert_if;
	bool clk_out;         /* XTAL clock-out enable, reg 0x01 bit7 */
	u8  agc_mode;         /* 0 = AGC_SELF (internal loop), 1 = AGC_EXTERNAL */
	u8  agc_sel;          /* 0 = AGC2, 1 = AGC1 */
	u8  if_path;          /* 0 or 1: IF output path bits5:4 of 0x05 */
	u8  if_gain;          /* 0..15: IF out gain bits3:0 of 0x05 */
	bool single_supply;   /* single 3.3 V supply board config */
	bool xtal_sharing;    /* slave mode: clock from master chip (0x02
			       * bit6 + 0x6D = 0x80), no own crystal */
	struct dvb_frontend **fe;
};

struct dvb_frontend *mxl601_attach(struct dvb_frontend *fe,
				  struct i2c_adapter *i2c,
				  const struct mxl601_config *cfg);

#endif /* MXL601_H */
