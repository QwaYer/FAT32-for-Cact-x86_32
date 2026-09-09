#!/usr/bin/env bash
# Build the module test binary, create a FAT32 "ESP" image with mkfs.fat +
# mtools, and run the module against it plus against a zeroed (non-FAT) image.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

make -s test

WORK="$(mktemp -d /tmp/kilo/fat32test.XXXXXX)"
IMG="$WORK/esp.img"
ZERO="$WORK/zero.img"
MT="$WORK/mtools.conf"
PAY="$WORK/payload.bin"

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

echo "== building test ESP image: $IMG"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mkfs.fat -F 32 -n CACTESP "$IMG" >/dev/null
dd if=/dev/zero of="$ZERO" bs=1K count=64 status=none

# mtools drive mapped to the raw image.
printf 'drive B: file="%s"\n' "$IMG" > "$MT"
export MTOOLSRC="$MT"

# Payload: 100000 bytes of a deterministic pattern (index mod 251).
python3 - "$PAY" <<'EOF'
import sys
open(sys.argv[1], "wb").write(bytes(i % 251 for i in range(100000)))
EOF

# An empty-ish file for the VFAT long-name test (first byte 0x00).
dd if=/dev/zero of="$WORK/lfn.txt" bs=1 count=40 status=none
printf '\xAB' > "$WORK/short.bin"

echo "== populating"
mmd B:/EFI
mmd B:/EFI/BOOT
mmd B:/GRUB
mmd B:/GRUB/x86_64-efi
mcopy "$PAY" "B:/EFI/BOOT/BIGFILE.BIN"
mcopy "$WORK/short.bin" "B:/SHORT.BIN"
mcopy "$WORK/lfn.txt" "B:/Long File Name 123.txt"

echo "== mtools sees:"
mdir -/ B:/

echo "== running module test"
"$ROOT/test/test_fat32" "$IMG" "$ZERO"

echo "== mtools interop on the module-modified image =="
mdir -/ B:/
mdir B:/NEWDIR

printf 'set timeout=5\nmenuentry "cactos" {\n  linux /cactos\n}\n' > "$WORK/grub.expected"
mcopy -o "B:/grub.cfg" "$WORK/grub.out"
if cmp -s "$WORK/grub.out" "$WORK/grub.expected"; then
    echo "  ok: mtools reads module-written grub.cfg (LFN + content)"
else
    echo "  FAIL: grub.cfg mismatch"; exit 1
fi

mcopy -o "B:/NEWDIR/datafile.bin" "$WORK/data.out"
python3 - "$WORK/data.out" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
exp = bytes(i % 251 for i in range(200000)) + b"TAIL-MARKER"
ok = data == exp
print("  ok: mtools reads module-written datafile.bin (multi-cluster chain)" if ok
      else "  FAIL: datafile.bin mismatch")
sys.exit(0 if ok else 1)
PY

mcopy -o "B:/NEWDIR/shrink.bin" "$WORK/shrink.out"
python3 - "$WORK/shrink.out" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
exp = bytes(i % 251 for i in range(3000)) + bytes(6000)
ok = data == exp and len(data) == 9000
print("  ok: mtools reads truncated/grown shrink.bin" if ok else "  FAIL: shrink.bin")
sys.exit(0 if ok else 1)
PY

mdir B:/ | grep -qi '^SHORT' && echo "  ok: recreated SHORT.BIN visible (8.3, no LFN)" || { echo "  FAIL: SHORT.BIN missing"; exit 1; }
mdir B:/NEWDIR | grep -qi 'Sub Dir' && echo "  ok: module-written LFN dir 'Sub Dir' visible" || { echo "  FAIL: Sub Dir missing"; exit 1; }
