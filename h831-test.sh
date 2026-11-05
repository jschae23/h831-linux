#!/usr/bin/env bash
# H831 bring-up sweep runner.
#
# usage:
#   sudo ./h831-test.sh                     # replay of the winning defaults
#       (xtal=16MHz, sharing=0, if=4063000, inv=0, path=1, full scan)
#   sudo ./h831-test.sh IF INVERT PATH [TAG]
#       e.g. sudo ./h831-test.sh 4000000 0 1
#   sudo ./h831-test.sh list                # list previous runs + tricks
#
# The script sets the module params, asks you to unplug+replug the stick
# (actually it waits for you to do it), then runs a scan and prints the
# banner, PLL lines, strength values and found services.

tagg=${4:-"r"}
IF=${1:-4063000}
INV=${2:-0}
PATHP=${3:-1}
XTAL=${H831_XTAL:-16000000}
SHARING=${H831_SHARING:-0}
LOG=/tmp/h831-${IF}-${INV}-${PATHP}-x${XTAL}-sh${SHARING}-s${H831_SUPPLY:-1}-${tagg}.log

if [ "$1" = "list" ]; then
  for f in /tmp/h831-*.log; do
    [ -f "$f" ] || continue
    n=$(grep -a ';' "$f" | wc -l)
    printf "%-45s services=%s lines=%s\n" "$f" "$n" "$(wc -l < "$f")"
  done
  exit 0
fi

sys=/sys/module/cx231xx_h831/parameters

echo "$IF"    | sudo tee "$sys/h831_if_freq_hz" >/dev/null
echo "$INV"   | sudo tee "$sys/h831_if_invert"   >/dev/null
echo "$PATHP" | sudo tee "$sys/h831_if_path"     >/dev/null
echo "$XTAL"  | sudo tee "$sys/h831_tuner_xtal_hz" >/dev/null
echo "$SHARING" | sudo tee "$sys/h831_xtal_sharing" >/dev/null
# mxl601 tune-time 0xEA/0xEB LUT writes (LibreELEC-style): default 1,
# toggle live via env REF_LUT=0/1 — read per set_params, no replug needed
if [ -n "$REF_LUT" ]; then
  echo "$REF_LUT" | sudo tee /sys/module/mxl601/parameters/ref_lut >/dev/null
fi
# SDK START_TUNE bracket (0x6E<7>/0x14<5> xtal-clock bypass dance):
# toggle live via env START_SEQ=0/1 — no replug needed
if [ -n "$START_SEQ" ]; then
  echo "$START_SEQ" | sudo tee /sys/module/mxl601/parameters/start_seq >/dev/null
fi
# SDK-aligned settings: single-supply + clock-out enabled like the
# MxLWare601 reference integration; AGC/gain sweeps still via env
echo "${H831_SUPPLY:-1}"  | sudo tee "$sys/h831_single_supply" >/dev/null
echo "${H831_CLKOUT:-1}"  | sudo tee "$sys/h831_clk_out" >/dev/null
echo "${H831_GAIN:-11}"   | sudo tee "$sys/h831_if_gain"     >/dev/null
echo "${H831_AGCMODE:-1}" | sudo tee "$sys/h831_agc_mode"    >/dev/null
echo "${H831_AGCSEL:-1}"  | sudo tee "$sys/h831_agc_sel"     >/dev/null
echo "${H831_CAP:-12}"    | sudo tee "$sys/h831_xtal_cap"    >/dev/null

sudo dmesg -C  # clear dmesg: report only fresh lines from here on

echo "=== if=$IF inv=$INV path=$PATHP tag=$tagg -- NOW unplug the stick, wait 2s, replug ==="
for i in $(seq 1 120); do
  [ ! -e /dev/dvb/adapter0/frontend0 ] && break
  sleep 0.5
done
for i in $(seq 1 240); do
  [ -e /dev/dvb/adapter0/frontend0 ] && break
  sleep 0.5
done
if [ ! -e /dev/dvb/adapter0/frontend0 ]; then
  echo "!!! ADAPTER MISSING after replug — dmesg follows:"
  sudo dmesg | tail -30
  exit 1
fi
sleep 3

echo "--- attach banner ---"
sudo dmesg | grep -E "mxl601: cfg" | tail -2

timeout "${SCANSEC:-300}" w_scan2 -ft -c "${H831_COUNTRY:?set H831_COUNTRY to your ISO country code}" -X > "$LOG" 2>&1

echo "--- PLL lines ---"
sudo dmesg | grep -E "0x2B=0x|PLL unlocked|PLL now|PLL settled" | tail -8
echo "--- strength values ---"
sudo dmesg | grep "strength=" | awk '{print $NF}' | sort | uniq -c | head
echo "--- services found ---"
grep ';' "$LOG" | head -15
echo "--- scan log summary ---"
wc -l "$LOG"
head -5 "$LOG"
