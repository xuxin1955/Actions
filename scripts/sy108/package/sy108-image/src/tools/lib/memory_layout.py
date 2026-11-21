# SPDX-License-Identifier: GPL-2.0-only
"""RAM range checks shared by the SY108 capsule builder."""
import struct


def cells(data):
    return list(struct.unpack(">" + str(len(data) // 4) + "I", data))


def ranges(nodes):
    memory = []
    reserved = []
    for path, props in nodes.items():
        if props.get("status") == b"disabled\0" or "reg" not in props:
            continue
        if path.startswith("/memory@") or path.startswith("/reserved-memory/"):
            vals = cells(props["reg"])
            if len(vals) % 4:
                raise ValueError("unsupported address/size cell geometry")
            for i in range(0, len(vals), 4):
                addr = (vals[i] << 32) | vals[i + 1]
                size = (vals[i + 2] << 32) | vals[i + 3]
                region = {"node": path, "start": addr, "end": addr + size}
                (reserved if path.startswith("/reserved-memory/") else memory).append(region)
    return memory, reserved


def overlap(a, b):
    return a["start"] < b["end"] and b["start"] < a["end"]
