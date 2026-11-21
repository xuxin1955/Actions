#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""UDX710 DHTB/Android/SPRD boot image layout parser and builder."""
import hashlib
import struct
import zlib

PAGE = 2048


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(size, boundary=PAGE):
    return (size + boundary - 1) // boundary * boundary


def image_header(data):
    if len(data) < 64 or data[56:60] != b"ARMd":
        raise ValueError("not an AArch64 raw Image")
    off, size, flags = struct.unpack_from("<3Q", data, 8)
    if size < len(data) or (flags & 1) or (flags >> 4):
        raise ValueError("unsupported Image memory/flags")
    return {
        "bytes": len(data),
        "sha256": sha(data),
        "text_offset": off,
        "memory_size": size,
        "flags": flags,
    }


def fdt_props(data):
    magic, total, so, st, rm, version, last, cpu, ssz, dsz = struct.unpack_from(">10I", data)
    if magic != 0xD00DFEED or total != len(data) or so + dsz > total or st + ssz > total or version != 17:
        raise ValueError("invalid FDT")
    strings = data[st : st + ssz]
    pos = so
    stack = []
    nodes = {}
    while pos < so + dsz:
        token = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if token == 1:
            end = data.index(b"\0", pos, so + dsz)
            stack.append(data[pos:end].decode())
            pos = align(end + 1, 4)
            nodes["/" + "/".join(stack[1:])] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, name_off = struct.unpack_from(">II", data, pos)
            pos += 8
            if name_off >= len(strings) or pos + size > so + dsz:
                raise ValueError("invalid FDT property")
            name = strings[name_off : strings.index(b"\0", name_off)].decode()
            nodes["/" + "/".join(stack[1:])][name] = data[pos : pos + size]
            pos = align(pos + size, 4)
        elif token == 4:
            pass
        elif token == 9:
            if stack:
                raise ValueError("unbalanced FDT")
            return nodes
        else:
            raise ValueError("FDT token")
    raise ValueError("missing FDT end")


def boot_abi(nodes, target):
    """Validate the six retained properties of the installed SY108 boot ABI."""
    if target not in ("openwrt", "recovery"):
        raise ValueError("Unknown boot target")
    chosen = nodes.get("/chosen", {})
    strings = {"udx,boot-policy": "openwrt-v1", "udx,boot-target": target}
    for name, value in strings.items():
        if chosen.get(name) != (value + "\0").encode():
            raise ValueError("Invalid boot ABI string: " + name)
    if chosen.get("udx,trial-stage-schema") != struct.pack(">I", 6):
        raise ValueError("Invalid RTC stage schema")
    for name in ("udx,trial-rtc-stages", "udx,trial-local-lease"):
        if chosen.get(name) != b"":
            raise ValueError("Missing or non-boolean boot flag: " + name)
    recovery = chosen.get("udx,recovery-ram")
    if (target == "recovery" and recovery != b"") or (target == "openwrt" and recovery is not None):
        raise ValueError("Recovery RAM flag does not match the boot target")
    return {"boot_policy": "openwrt-v1", "boot_target": target,
            "trial_stage_schema": 6, "trial_rtc_stages": True,
            "trial_local_lease": True, "recovery_ram": target == "recovery"}


def dt_region(dtb):
    fdt_props(dtb)
    padded = align(len(dtb))
    buf = bytearray(PAGE + padded)
    buf[:4] = b"SPRD"
    struct.pack_into("<7I", buf, 4, 1, 1, 710, 1, 0x20000, PAGE, padded)
    buf[PAGE : PAGE + len(dtb)] = dtb
    return bytes(buf)


def build(kernel, ramdisk, dtb, addresses=(0x8000, 0x1000000, 0xF00000, 0x100)):
    image_header(kernel)
    raw = zlib.decompress(ramdisk, 31)
    if not (raw.startswith(b"070701") or raw == b"This is not an initrd\n"):
        raise ValueError("not newc or the exact stock placeholder")
    ka, ra, sa, ta = addresses
    dr = dt_region(dtb)
    hdr = bytearray(PAGE)
    hdr[:8] = b"ANDROID!"
    struct.pack_into("<9I", hdr, 8, len(kernel), ka, len(ramdisk), ra, 0, sa, ta, PAGE, len(dr))
    digest = hashlib.sha1(
        kernel
        + struct.pack("<I", len(kernel))
        + ramdisk
        + struct.pack("<II", len(ramdisk), 0)
        + dr
        + struct.pack("<I", len(dr))
    ).digest()
    hdr[572:592] = digest
    payload = (
        bytes(hdr)
        + kernel
        + bytes(align(len(kernel)) - len(kernel))
        + ramdisk
        + bytes(align(len(ramdisk)) - len(ramdisk))
        + dr
    )
    prefix = bytearray(512)
    prefix[:8] = b"DHTB\1\0\0\0"
    struct.pack_into("<I", prefix, 48, len(payload))
    return bytes(prefix) + payload


def parse(data):
    if data[:8] != b"DHTB\1\0\0\0" or data[512:520] != b"ANDROID!":
        raise ValueError("legacy wrapper required")
    k, ka, r, ra, s, sa, ta, page, ds = struct.unpack_from("<9I", data, 520)
    if page != PAGE or s or not 0 < k < 64 * 1024 * 1024 or not 0 < r < 64 * 1024 * 1024:
        raise ValueError("unsupported header")
    ko = 512 + PAGE
    ro = ko + align(k)
    dto = ro + align(r)
    end = dto + ds
    if end > len(data) or struct.unpack_from("<I", data, 48)[0] != end - 512:
        raise ValueError("bounds/payload length")
    if data[dto : dto + 4] != b"SPRD" or struct.unpack_from("<7I", data, dto + 4)[:6] != (1, 1, 710, 1, 0x20000, PAGE):
        raise ValueError("SPRD DT profile")
    padded = struct.unpack_from("<I", data, dto + 28)[0]
    total = struct.unpack_from(">I", data, dto + PAGE + 4)[0]
    if ds != PAGE + padded or not 40 <= total <= padded:
        raise ValueError("DT length")
    kernel = data[ko : ko + k]
    ramdisk = data[ro : ro + r]
    dtb = data[dto + PAGE : dto + PAGE + total]
    rebuilt = build(kernel, ramdisk, dtb, (ka, ra, sa, ta))
    if rebuilt != data[:end]:
        raise ValueError("header/hash/padding differs from supported exact layout")
    tail = data[end:]
    info = {
        "file_bytes": len(data),
        "file_sha256": sha(data),
        "payload_end": end,
        "payload_sha256": sha(data[512:end]),
        "legacy_sha1_offset": 512 + 572,
        "legacy_name_cmdline": "empty; command line is in FDT",
        "page": PAGE,
        "section_offsets": {"kernel": ko, "ramdisk": ro, "SPRD_DT": dto, "FDT": dto + PAGE},
        "legacy_address_metadata": [ka, ra, sa, ta],
        "Image": image_header(kernel),
        "ramdisk_bytes": r,
        "dtb_bytes": len(dtb),
        "dtb_sha256": sha(dtb),
        "security_tail_present": bool(tail),
    }
    if tail:
        size, off = struct.unpack_from("<QQ", data, end + 32)
        if size != 564 or off != end + 96 or off + size > len(data):
            raise ValueError("unsupported security tail")
        cert = data[off : off + size]
        certtype, keybits = struct.unpack_from("<II", cert)
        if (certtype, keybits) != (0, 2048) or cert[268:300] != hashlib.sha256(data[512:end]).digest():
            raise ValueError("certificate payload digest mismatch")
        info["security"] = {
            "header_bytes": 96,
            "certificate_bytes": size,
            "certificate_offset": off,
            "key_bits": keybits,
            "payload_hash_matches": True,
            "signature_validity_and_trust": "NOT_VERIFIED",
            "bytes_before_ff": len(tail.rstrip(b"\xff")),
            "no_signature_bytes_exported": True,
        }
    return info, kernel, ramdisk, dtb
