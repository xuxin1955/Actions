#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a SY108 boot or Recovery capsule from validated source-build outputs."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / "tools/lib"))
from boot_layout import build, image_header, fdt_props, align, boot_abi
from memory_layout import ranges, overlap
from newc import encode, parse as parse_cpio, gz


def sha(data):
    return hashlib.sha256(data).hexdigest()


def package(image, dtb, ramdisk, target, output, storage_budget=None):
    kernel = image.read_bytes()
    tree = dtb.read_bytes()
    header = image_header(kernel)
    nodes = fdt_props(tree)
    rd = ramdisk.read_bytes() if ramdisk else gz(encode({}))
    entries = parse_cpio(gzip.decompress(rd))
    chosen = nodes["/chosen"]
    if b"sprd,sy108\0" not in nodes["/"].get("compatible", b""):
        raise ValueError("Capsule must describe the SY108 board")
    if header["text_offset"] or header["flags"] != 0xa or not 64 <= len(kernel) <= header["memory_size"] <= 0x1e00000:
        raise ValueError("Unsupported ARM64 Image geometry")
    boot_abi(nodes, target)
    if any(name.startswith("linux,initrd-") for name in chosen):
        raise ValueError("Stale initrd bounds")
    args = chosen.get("bootargs", b"").rstrip(b"\0").split()
    if target == "openwrt":
        required = {b"ubi.mtd=ubipac", b"ubi.block=0,rootfs", b"root=/dev/ubiblock0_21", b"rootfstype=squashfs", b"ro", b"rootwait"}
        if entries or not required.issubset(args) or any(x.startswith((b"rdinit=", b"init=")) for x in args):
            raise ValueError("Main system requires direct SquashFS boot and an empty initramfs")
    elif entries.get("etc/udx-recovery-root", {}).get("data") != b"OpenWrt Recovery RAM root\n" or "init" not in entries or b"rdinit=/init" not in args:
        raise ValueError("Recovery requires its independent marked RAM root")
    compressed = gzip.compress(kernel, compresslevel=9, mtime=0)
    target_id = 2 if target == "openwrt" else 1
    words = [0x315a4758, 1, target_id, len(compressed), len(kernel), header["memory_size"],
             zlib.crc32(compressed), zlib.crc32(kernel), len(rd), zlib.crc32(rd), len(tree), zlib.crc32(tree)]
    metadata = struct.pack("<12I", *words)
    metadata += struct.pack("<4I", zlib.crc32(metadata), 0, 0, 0)
    output.parent.mkdir(parents=True, exist_ok=True)
    prefix = bytearray(4096)
    struct.pack_into("<3Q", prefix, 8, 0x80000,
                     0x80200000 + header["memory_size"] - 0x80080000, 0xa)
    struct.pack_into("<I", prefix, 56, 0x644d5241)
    prefix[128:192] = metadata
    wrapper = bytes(prefix) + compressed
    data = build(wrapper, rd, tree)
    format_capacity = (133 if target == "openwrt" else 149) * 126976
    capacity = storage_budget if storage_budget is not None else (108 if target == "openwrt" else 149) * 126976
    if capacity <= 0 or len(data) > capacity or len(data) > format_capacity:
        raise ValueError("Capsule exceeds the storage budget or native-loader limit")
    memory, reserved = ranges(nodes)
    position = struct.unpack_from(">I", tree, 16)[0]
    while True:
        start, size = struct.unpack_from(">QQ", tree, position)
        position += 16
        if not start and not size:
            break
        reserved.append({"start": start, "end": start + size})
    regions = {"wrapper": {"start": 0x80080000, "end": 0x80080000 + len(wrapper)},
               "kernel": {"start": 0x80200000, "end": 0x80200000 + header["memory_size"]},
               "cached_package": {"start": 0x82000000, "end": 0x82000000 + len(data)},
               "fdt": {"start": 0x85400000, "end": 0x85500000},
               "initrd": {"start": 0x85500000, "end": 0x85500000 + align(len(rd))}}
    for name, region in regions.items():
        if not any(item["start"] <= region["start"] < region["end"] <= item["end"] for item in memory) or any(overlap(region, item) for item in reserved):
            raise ValueError("Invalid capsule RAM region: " + name)
        for other, test in regions.items():
            if name < other and {name, other} != {"wrapper", "kernel"} and overlap(region, test):
                raise ValueError("Overlapping capsule regions")
    output.write_bytes(data)
    report = {"board": "sy108", "soc": "udx710", "target": target, "bytes": len(data),
              "sha256": sha(data), "kernel_sha256": sha(kernel), "dtb_sha256": sha(tree),
              "ramdisk_sha256": sha(rd), "volume_capacity": capacity,
              "storage_budget_bytes": capacity, "native_loader_limit_bytes": format_capacity, "regions": regions}
    output.with_suffix(output.suffix + ".json").write_text(json.dumps(report, indent=2) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--ramdisk", type=Path)
    parser.add_argument("--target", choices=("openwrt", "recovery"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--storage-budget", type=int, help="Target volume budget in bytes; defaults to the smallest supported Main slot")
    arguments = parser.parse_args()
    print(json.dumps(package(arguments.image, arguments.dtb, arguments.ramdisk, arguments.target,
                             arguments.output, arguments.storage_budget), indent=2))
