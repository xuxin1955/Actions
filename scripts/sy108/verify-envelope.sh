#!/bin/sh
set -eu
uboot=$1
sml=$2
spl=$3
out=$4

for f in "$uboot" "$sml" "$spl"; do
  if [ ! -f "$f" ]; then
    echo "[precheck] missing input: $f" >&2
    echo "[precheck] /inputs contains:" >&2
    ls -l /inputs >&2 2>&1 || true
    exit 2
  fi
done

echo "[1/3] envelope"
python3 /opt/boot/envelope.py "$uboot" "$sml" "$out"

echo "[2/3] spl-check"
rc=0
qemu-aarch64 /base/spl-check "$spl" "$out" 0xc0000 0 "$sml" || rc=$?
echo "spl-check rc=$rc"
case "$rc" in
  0)  ;;
  90) echo "spl-check: unexpected version write" >&2 ;;
  91) echo "spl-check: mmap/mprotect failed" >&2 ;;
  92) echo "spl-check: cannot open input file" >&2 ;;
  94) echo "spl-check: wrong argument count" >&2 ;;
  95) echo "spl-check: read limit out of range" >&2 ;;
  96) echo "spl-check: out-of-bounds access while parsing the certificate" >&2 ;;
  *)  echo "spl-check: secboot_verify rejected the image" >&2 ;;
esac
[ "$rc" = 0 ] || exit 1

echo "[3/3] payload check"
python3 /scripts/check-embedded.py "$out" "$sml"
echo "[done]"
