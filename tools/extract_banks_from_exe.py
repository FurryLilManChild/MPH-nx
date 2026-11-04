#!/usr/bin/env python3
"""Recover MPH coverage-bank page images from a Windows nds_runner.exe.

Upstream's 248 runtime/coverage banks are generated from RAM page captures
kept in a private store. A shipped Windows runner still carries, for every
recompiled function, its guest address, length and original ARM code bytes
(the NdsStaticValidation tables the runtime checks against live memory),
plus a COFF symbol table naming each bank's dispatch table.

This rebuilds each bank's 4 KiB page from those code bytes. Bytes no
function covered (data sharing the page) are unknown and left zero. The
recompiler is then run on the rebuilt page under a new identity; any code it
derives from an unknown byte fails live validation at runtime and falls back
to the interpreter, so a gap can never run wrong code.

Output (private, never commit: it is game code from the player's cartridge):
  <out>/capture/<bank>.bin    rebuilt page image
  <out>/config/<bank>.toml    upstream config with identity sha1 replaced
  <out>/report.json           per-bank coverage of the page

Usage: extract_banks_from_exe.py --exe nds_runner.exe --config-dir mph/config --out DIR
"""
import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys

import pefile

DISPATCH_ENTRY = struct.Struct("<IB3xQQ")      # addr, thumb, fn, validation*
VALIDATION = struct.Struct("<IIQQI4x")         # addr, size, expected*, deps*, count


def read_symbols(exe):
    """name -> virtual address, from the COFF symbol table (via binutils nm)."""
    out = subprocess.run(["nm", exe], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


class Image:
    def __init__(self, exe):
        self.pe = pefile.PE(exe, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.__data__

    def read(self, va, n):
        off = self.pe.get_offset_from_rva(va - self.base)
        return bytes(self.data[off:off + n])


def bank_ranges(img, syms, bank):
    table = syms.get("g_dispatch_" + bank)
    length = syms.get("g_dispatch_" + bank + "_len")
    if table is None or length is None:
        return None
    count = struct.unpack("<I", img.read(length, 4))[0]
    seen = {}
    for i in range(count):
        addr, thumb, fn, vptr = DISPATCH_ENTRY.unpack(img.read(table + i * DISPATCH_ENTRY.size, DISPATCH_ENTRY.size))
        if not vptr or vptr in seen:
            continue
        vaddr, vsize, expected, _deps, _n = VALIDATION.unpack(img.read(vptr, VALIDATION.size))
        if expected and 0 < vsize <= 0x10000:
            seen[vptr] = (vaddr, img.read(expected, vsize))
    return list(seen.values())


def toml_value(text, key):
    m = re.search(r"^\s*" + key + r"\s*=\s*(\S+)", text, re.M)
    return int(m.group(1), 0) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--config-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", help="regex of bank names to process")
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    (out / "capture").mkdir(parents=True, exist_ok=True)
    (out / "config").mkdir(parents=True, exist_ok=True)
    img = Image(args.exe)
    syms = read_symbols(args.exe)

    report = {}
    for cfg in sorted(pathlib.Path(args.config_dir).glob("*.toml")):
        bank = cfg.stem
        if args.only and not re.search(args.only, bank):
            continue
        text = cfg.read_text()
        base = toml_value(text, "load_address")
        size = toml_value(text, "size")
        if base is None or size is None:
            continue
        ranges = bank_ranges(img, syms, bank)
        if ranges is None:
            report[bank] = {"status": "not in exe"}
            continue
        page = bytearray(size)
        known = bytearray(size)
        conflicts = 0
        for vaddr, blob in ranges:
            for j, b in enumerate(blob):
                k = vaddr - base + j
                if 0 <= k < size:
                    if known[k] and page[k] != b:
                        conflicts += 1
                    page[k] = b
                    known[k] = 1
        sha1 = hashlib.sha1(page).hexdigest()
        (out / "capture" / (bank + ".bin")).write_bytes(page)
        new = re.sub(r'(\[identity\][^\[]*?sha1\s*=\s*")[0-9a-f]{40}(")',
                     lambda m: m.group(1) + sha1 + m.group(2), text, count=1, flags=re.S)
        new = ("# MPH-nx: page rebuilt from validation bytes in a shipped nds_runner.exe;\n"
               "# identity replaced. Private: derived from the player's cartridge.\n") + new
        (out / "config" / cfg.name).write_text(new)
        report[bank] = {"status": "rebuilt", "functions": len(ranges),
                        "known_bytes": sum(known), "page_bytes": size,
                        "conflicts": conflicts, "sha1": sha1}
    (out / "report.json").write_text(json.dumps(report, indent=1))
    rebuilt = [r for r in report.values() if r["status"] == "rebuilt"]
    print(f"{len(rebuilt)} banks rebuilt, {len(report) - len(rebuilt)} skipped")
    if rebuilt:
        kb = sum(r["known_bytes"] for r in rebuilt) / max(1, sum(r["page_bytes"] for r in rebuilt))
        print(f"code bytes recovered: {kb:.1%} of all page bytes; "
              f"conflicting overlaps: {sum(r['conflicts'] for r in rebuilt)}")


if __name__ == "__main__":
    sys.exit(main())
