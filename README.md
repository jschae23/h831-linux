# AVerMedia H831 (07ca:1831) native Linux driver

Status: **working** - DVB-T2 receive verified on a 690 MHz multiplex.

This repository contains an out-of-tree Linux driver set for the
AVerMedia H831 USB Hybrid DVB-T/T2 stick. It adds the missing board
support to `cx231xx` and provides a driver for the MxL601 silicon tuner,
which has no in-kernel driver.

## Device

* AVerMedia H831 USB Hybrid DVB-T/T2 Lite, VID:PID `07ca:1831`
* silicon (verified on hardware):
  * Conexant **CX23102** (USB 2.0 bridge, in-kernel `cx231xx` driver)
  * Panasonic **MN88473** (DVB-T/T2 demodulator, in-kernel `mn88473`;
    bank-2 register `0xff` reads `0x03`, not the MN88472 some sources
    claim)
  * MaxLinear **MxL601** (RF tuner, no in-kernel driver)

## Repository layout

| Path | Purpose |
|---|---|
| `cx231xx/` | patched `cx231xx` bridge driver, builds as `cx231xx-h831.ko` |
| `mn88473/` | stock MN88473 demodulator driver, builds as `mn88473.ko` |
| `mxl601/` | new MxL601 tuner driver, builds as `mxl601.ko` |
| `dtv-inc/` | kernel media/DVB headers needed by the out-of-tree build |
| `tools/h831-fe-test.c` | small direct-frontend tune/status probe |
| `test-vectors/` | verified 690 MHz DVB-T2 mux and usage notes |
| `h831-test.sh` | parameter-sweep helper for bring-up |

## What the patches do

1. **`cx231xx`** (from the v7.2 series) is vendored and patched:
   * new board `CX231XX_BOARD_AVERMEDIA_H831`
   * new USB ID `07ca:1831`
   * DVB wiring: `mn88473` demod + `mxl601` tuner
   * the USB driver is renamed to `cx231xx-h831` so it does not collide
     with the stock `cx231xx` module
2. **`mn88473`** is a stock driver copy; no changes are needed.
3. **`mxl601`** is a new tuner module:
   * I2C register map, probe (chip-id/version read), soft reset and
     power up/down
   * full tune/register-init sequence derived from the publicly
     available MxLWare601 reference, with PLL lock diagnostics
   * defaults: **16 MHz crystal**, **4.063 MHz IF**, `ref_lut=1`
4. The build uses only the distribution `linux-headers` tree; no kernel
   source tree is required.

## Verified configuration

* bridge, MN88473 demod (address `0x18`) and MxL601 tuner (address
  `0x60`) all probe successfully
* the tuner crystal is **16 MHz** (24 MHz never locks: `0x2B=0x04`)
* IF output is **4.063 MHz**, matching the vendor driver default
* the driver locks the 690 MHz DVB-T2 mux, scans 11 services and records
  a valid transport stream with `dvbv5-zap`
* the `mxl601` parameters can be changed at runtime through
  `/sys/module/cx231xx_h831/parameters/` for debugging; the compiled
  defaults are the verified values

## Building

The build directory path must not contain spaces: kbuild canonicalises
paths internally and rejects such targets. Clone or unpack the tree into
a space-free location.

```bash
sudo apt install -y linux-headers-$(uname -r) build-essential
make modules
sudo make load     # copies the modules to /lib/modules/$(uname -r)/updates and runs depmod
```

Run `make modules` as your normal user, not as root. Only `make load`
needs `sudo`.

To rebuild and reload without rebooting, unplug the stick first and run:

```bash
make modules
sudo make reload   # swaps modules, unloads the old set, loads the new one
```

Then replug the stick. `sudo make unload` removes the modules.

## Loading and checking

The modules are normally loaded automatically once the stick is plugged
in and `depmod` has run. To load them explicitly:

```bash
sudo modprobe mn88473
sudo modprobe mxl601
sudo insmod cx231xx/cx231xx-h831.ko
```

Watch the kernel log:

```bash
sudo dmesg -w | grep -E 'cx231|mn88473|mxl601|h831'
```

A healthy attach looks like this (bus numbers vary by host):

```
cx231xx-h831: New device ... AVerMedia H831 USB Hybrid DVB-T/T2 @ 480 Mbps (07ca:1831)
cx231xx-h831: Identified as AVerMedia H831 USB Hybrid DVB-T/T2 (card=28)
mn88473: Panasonic MN88473 successfully identified
mxl601: MxL601 detected at 0x60: chip id 0x03 version 0x02
mxl601: cfg: xtal=16000000 cap=12 if=4063000 ...
mxl601: 0x2B=0x07 after 0 ms (ref=1 rf=1)
```

`/dev/dvb/adapter0/{frontend0,demux0,dvr0}` appearing together with
`mxl601: 0x2B=0x07` means the demod path works and the tuner PLL is
locked.

## Using the tuner

List the adapters and monitor the frontend:

```bash
dvb-fe-tool -a 0 -m
```

Run the supplied acceptance test (requires `dvb-tools`):

```bash
cd test-vectors
dvbv5-scan -a 0 690MHz_dvbv5_scan.conf -o scanned.conf
dvbv5-zap -a 0 -c 690MHz_dvbv5_channels.conf 'QVC HD' -r -o qvc.ts -t 10
```

See `test-vectors/README_test_vectors.md` for the full service list,
expected results and caveats.

### Kaffeine

Kaffeine detects the stick as **Panasonic MN88473**. Scan for channels
with **Television -> Configure Television -> Device 0 -> Scan**, select
your regional DVB-T2 source, and start the scan. Kaffeine's own scan is
the supported path; the files in `test-vectors/` cannot be imported into
Kaffeine's channel database directly.

## Bring-up helper

`h831-test.sh` sweeps tuner parameters, asks you to replug the stick,
runs a `w_scan2` scan and prints the relevant PLL and service lines:

```bash
H831_COUNTRY=XX sudo ./h831-test.sh            # verified defaults + full scan
sudo ./h831-test.sh 4000000 0 1                # IF, inversion, path
sudo ./h831-test.sh list                       # previous runs
```

Replace `XX` with your ISO country code; it is used only by `w_scan2`.

`tools/h831-fe-test.c` is a minimal direct-frontend probe:

```bash
cc -O2 -o h831-fe-test tools/h831-fe-test.c
./h831-fe-test 690000000 8 2
```

It bypasses adapter auto-detection, reports every ioctl failure and
polls `FE_READ_STATUS` for a lock.

## Known limitations

* The verified defaults target a 690 MHz DVB-T2 mux. Other muxes have
  not been swept; a broad scan is the next validation step.
* Received C/N on the test mux was around 9-12 dB with the available
  antenna. HD playback may be marginal depending on signal quality.
* The driver is an out-of-tree bring-up. Upstreaming would require
  splitting the changes into reviewable patches for the linux-media
  list.

## License

The new driver code is licensed under **GPL-2.0**; see `LICENSE`. The
vendored `cx231xx` and `mn88473` sources retain their original kernel
licenses (`GPL-2.0-or-later` where marked) and copyright notices. The
vendor MxL601 SDK is not included in this repository.
