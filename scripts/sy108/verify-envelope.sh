#!/bin/sh
# Wrap one model's capsule and run the original SPL signature verification.
set -eu
uboot=$1
sml=$2
spl=$3
out=$4

echo "[1/3] envelope"
python3 /opt/boot/envelope.py "$uboot" "$sml" "$out"

echo "[2/3] spl-check"
rc=0
qemu-aarch64 /base/spl-check "$spl" "$out" 0xc0000 0 "$sml" || rc=$?
echo "spl-check rc=$rc"
[ "$rc" = 0 ] || exit 1

echo "[3/3] payload check"
python3 /scripts/check-embedded.py "$out" "$sml"
echo "[done]"
