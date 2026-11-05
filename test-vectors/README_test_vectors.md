# Test vectors: known-good DVB-T2 mux at 690 MHz

These files are derived from a working vendor channel list for the same
AVerMedia H831 tuner. They give a Linux port a concrete, already-proven
lock target instead of "scan and hope".

The list contains **one multiplex, 11 services, all on 690 MHz**
(UHF channel 48, NetworkID `8468`). If a Linux driver can lock it and
play the free-to-air services, the demod + tuner + I2C path is working
end to end.

---

## Files

| File | What it is | Usable on Linux? |
|---|---|---|
| `690MHz_channels.csv` | Parsed table of the 11 services (SIDs, PIDs, language, flags) | Yes - human/reference |
| `690MHz_dvbv5_channels.conf` | `dvb-tools` / `dvbv5-zap` channel file, one section per service | **Yes** - `dvbv5-zap -c` |
| `690MHz_dvbv5_scan.conf` | `dvbv5-scan` initial file for the single mux | **Yes** - `dvbv5-scan` |
| `README_test_vectors.md` | This file | - |

The original vendor list is not included; these files contain only the
derived tuning parameters and service metadata.

---

## The mux, at a glance

| Field | Value |
|---|---|
| Frequency | 690,000 kHz = **690 MHz** (UHF ch 48) |
| Bandwidth | **8 MHz** (UHF raster; not stored in the vendor list, assumed) |
| Network ID | `8468` |
| Vendor `StreamID` | `16481` |
| Services | 11 |
| Service IDs | 16960-16968, 16970, 16971 (note: **16969 is absent**) |
| Delivery system | DVB-T2 (`DVBT2`) |

### Services

| SID | Name | Video | Audio | PMT | PCR | Teletext | Lang | Flag |
|---|---|---|---|---|---|---|---|---|
| 16960 | Disney Channel HD | 1361 | 1362 | 1360 | 1361 | 1366 | deu | 25 |
| 16961 | WELT HD | 1377 | 1378 | 1376 | 1377 | 0 | deu | 25 |
| 16962 | DMAX HD | 1393 | 1394 | 1392 | 1393 | 1398 | deu | 25 |
| 16963 | EUROSPORT 1 HD | 1409 | 1410 | 1408 | 1409 | 0 | deu | 25 |
| 16964 | NICK HD | 1425 | 1426 | 1424 | 1425 | 0 | deu | 25 |
| 16965 | Handystar TV HD | 1441 | 1442 | 1440 | 1441 | 0 | - | 24 |
| 16966 | QVC HD | 1457 | 1458 | 1456 | 1457 | 0 | deu | 24 |
| 16967 | HSE24 HD | 1473 | 1474 | 1472 | 1473 | 1478 | deu | 24 |
| 16968 | Bibel TV HD | 1489 | 1490 | 1488 | 1489 | 1494 | deu | 24 |
| 16970 | QVC 2 HD | 1521 | 1522 | 1520 | 1521 | 0 | deu | 24 |
| 16971 | VOXup HD | 1537 | 1538 | 1536 | 1537 | 1542 | deu | 25 |

`Flag` is the vendor `Encrypted` field. `24` marks channels that look
free-to-air (QVC, HSE24, Bibel TV, QVC 2, Handystar) and `25` marks the
subscription/Freenet ones. Treat this as a **hint only** - verify with the
actual stream (`ffprobe`, or no decodable video on a CA channel).

---

## How to use on Linux

```bash
sudo apt install dvb-tools v4l-utils ffmpeg
ls /dev/dvb/adapter0/            # frontend0, demux0, dvr0 must exist

# 1) Discover the real broadcast parameters (PLP, modulation, code rates)
#    Starts from the one frequency we know is live.
dvbv5-scan -a 0 690MHz_dvbv5_scan.conf -o scanned.conf
cat scanned.conf

# 2) Confirm the frontend is locked and healthy
dvb-fe-tool -a 0 -m            # live monitor: SIGNAL, SNR, BER, FE_HAS_LOCK

# 3) Zap one free-to-air service for 10 s and record it
dvbv5-zap -a 0 -c 690MHz_dvbv5_channels.conf 'QVC HD' -r -o qvc.ts -t 10
ffprobe qvc.ts
```

`dvbv5-zap` matches the section name exactly, so quote names containing
spaces.

### Expected results

- `dvbv5-scan` finds **11 services** on 690 MHz with the SIDs above and
  writes them in dvbv5 format to `scanned.conf`.
- A working driver reports `FE_HAS_LOCK` and a sane SNR; the five
  flag-24 services decode video/audio.
- The six flag-25 services may show a valid PAT/PMT and even video
  packets but no decodable picture without a CA solution. That is
  expected, not a driver bug.

### Kaffeine

Kaffeine cannot import these files. Use it only as the end-user check
after the driver works: **Television -> Configure Television ->
Device 0 -> Scan**, choose your regional DVB-T2 source, and scan. If
690 MHz is found by Kaffeine's own scan, the port is usable.

---

## Caveats (read before trusting a failed lock)

1. **`STREAM_ID = 0` is an assumption.** The vendor `StreamID` value
   `16481` is *not* a DVB-T2 PLP id (PLP ids are 0-255). Single-PLP muxes
   commonly use PLP 0, but if the frontend never locks, try `1`, `2`, ...
   or let `dvbv5-scan` report the real value.
2. **`BANDWIDTH_HZ = 8000000` is an assumption** based on the UHF raster;
   the vendor list does not store bandwidth.
3. **Modulation/guard/FEC are `AUTO`.** The vendor list does not store
   them. AUTO is correct for a first lock; use the values `dvbv5-scan`
   reports afterwards.
4. **`Encrypted` 24 vs 25 is inferred**, not documented. Do not use it as
   an authoritative CA flag.
5. **The vendor list uses vendor field names** (`StreamID`, `TunerType=2`,
   `OrbitalPos=5000`, `Polarity=2`) with no meaning outside AVerMedia's
   software. They were deliberately not copied into the dvbv5 files.
6. This is **one mux**, not a full channel list for a region. It is a
   lock/regression target, not a substitute for a real scan.

---

## Why this matters for the H831 port

Use it as the acceptance test described in the top-level `README.md`:

- **Locks + scans 11 services** -> CX23102 firmware upload, MN88473 demod
  attach and MxL601 tuner tuning are all correct.
- **Locks but zero services** -> tuning/IF is right, but the TS/PLP path
  or demod configuration is wrong.
- **No lock** -> tuner PLL/IF or demod I2C init is still wrong (the hard
  part of the port). Compare a `usbmon`/USBPcap trace against the vendor
  init sequence.

Record the observed `dvb-fe-tool -m` output alongside any test result;
SNR/BER numbers make regressions obvious.
