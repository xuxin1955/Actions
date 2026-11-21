# SPDX-License-Identifier: GPL-2.0-only
"""Minimal CPIO newc archive parser and encoder for initramfs images."""
import gzip
import io
import stat


def parse(raw):
    entries = {}
    pos = 0
    while True:
        if raw[pos : pos + 6] != b"070701":
            raise ValueError("newc magic")
        hdr = [int(raw[pos + 6 + n * 8 : pos + 14 + n * 8], 16) for n in range(13)]
        pos += 110
        if hdr[11] < 1 or raw[pos + hdr[11] - 1] != 0:
            raise ValueError("newc name bounds")
        name = raw[pos : pos + hdr[11] - 1].decode()
        pos = (pos + hdr[11] + 3) & ~3
        data = raw[pos : pos + hdr[6]]
        if len(data) != hdr[6]:
            raise ValueError("newc payload bounds")
        pos = (pos + hdr[6] + 3) & ~3
        if name == "TRAILER!!!":
            break
        if name in entries or name.startswith("/") or ".." in name.split("/"):
            raise ValueError("unsafe/duplicate newc path")
        entries[name] = {
            "mode": hdr[1],
            "uid": hdr[2],
            "gid": hdr[3],
            "nlink": hdr[4],
            "mtime": hdr[5],
            "devmajor": hdr[7],
            "devminor": hdr[8],
            "rdevmajor": hdr[9],
            "rdevminor": hdr[10],
            "data": data,
        }
    if any(raw[pos:]):
        raise ValueError("newc trailing data")
    return entries


def entry(mode, data=b""):
    return {
        "mode": mode,
        "uid": 0,
        "gid": 0,
        "nlink": 2 if stat.S_ISDIR(mode) else 1,
        "mtime": 0,
        "devmajor": 0,
        "devminor": 0,
        "rdevmajor": 0,
        "rdevminor": 0,
        "data": data,
    }


def encode(entries):
    raw = bytearray()
    items = sorted(entries.items()) + [("TRAILER!!!", entry(0))]
    for ino, (name, item) in enumerate(items, 1):
        encoded_name = name.encode() + b"\0"
        data = item["data"]
        hdr = [
            ino,
            item["mode"],
            item["uid"],
            item["gid"],
            item["nlink"],
            item["mtime"],
            len(data),
            item["devmajor"],
            item["devminor"],
            item["rdevmajor"],
            item["rdevminor"],
            len(encoded_name),
            0,
        ]
        raw += b"070701" + b"".join(f"{v:08x}".encode() for v in hdr) + encoded_name
        raw += b"\0" * (-len(raw) % 4)
        raw += data
        raw += b"\0" * (-len(raw) % 4)
    raw += b"\0" * (-len(raw) % 512)
    return bytes(raw)


def gz(raw):
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0, filename="", compresslevel=9) as zf:
        zf.write(raw)
    return out.getvalue()
