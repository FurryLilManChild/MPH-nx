"""MPH-nx ROM tools: header, ARM9/ARM7 images, overlays, BLZ decompression.

Reads a Nintendo DS ROM and returns plain code images with their load addresses.
Nothing it extracts is game-independent; never commit its output.
"""
import struct, hashlib

def u32(b, o): return struct.unpack_from('<I', b, o)[0]

def blz_decompress(data):
    """Undo the DS bottom-up LZ ("BLZ") compression used on ARM9 binaries and overlays.
    The footer (last 8 bytes) holds: u32 (header_len << 24 | compressed_len), u32 extra_size."""
    if len(data) < 8: return data
    extra = u32(data, len(data) - 4)
    if extra == 0: return data
    hdr = u32(data, len(data) - 8)
    hdr_len = hdr >> 24
    comp_len = hdr & 0xFFFFFF
    src = bytearray(data)
    out = bytearray(len(data) + extra)
    out[:len(data)] = data
    start = len(data) - comp_len          # untouched prefix
    sp = len(data) - hdr_len              # read pointer (moves down)
    dp = len(out)                         # write pointer (moves down)
    while sp > start:
        sp -= 1; flags = src[sp]
        for _ in range(8):
            if sp <= start: break
            if flags & 0x80:
                sp -= 2
                v = src[sp] | (src[sp + 1] << 8)
                n = (v >> 12) + 3
                disp = (v & 0xFFF) + 3
                for _ in range(n):
                    dp -= 1
                    out[dp] = out[dp + disp]
            else:
                sp -= 1; dp -= 1
                out[dp] = src[sp]
            flags = (flags << 1) & 0xFF
    return bytes(out)

class Rom:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        self.title = d[0:12].rstrip(b'\0').decode()
        self.code = d[12:16].decode()
        self.version = d[0x1E]
        self.sha1 = hashlib.sha1(d).hexdigest()
        self.arm9_off, self.arm9_entry, self.arm9_load, self.arm9_size = struct.unpack_from('<4I', d, 0x20)
        self.arm7_off, self.arm7_entry, self.arm7_load, self.arm7_size = struct.unpack_from('<4I', d, 0x30)
        self.fnt_off, self.fnt_size, self.fat_off, self.fat_size = struct.unpack_from('<4I', d, 0x40)
        self.ov9_off, self.ov9_size = struct.unpack_from('<2I', d, 0x50)

    def file(self, fid):
        s, e = struct.unpack_from('<2I', self.data, self.fat_off + fid * 8)
        return self.data[s:e]

    def arm9(self):
        """Decompressed ARM9 image. The module params block (after the
        0xDEC00621 magic) says where the compressed part ends."""
        raw = self.data[self.arm9_off:self.arm9_off + self.arm9_size]
        magic = raw.find(struct.pack('<I', 0xDEC00621))
        if magic < 8: return raw, False
        params = magic - 0x1C
        comp_end = u32(raw, params + 0x14)
        if comp_end == 0: return raw, False
        n = comp_end - self.arm9_load
        img = blz_decompress(raw[:n]) + raw[n:]
        return img, True

    def arm9_segments(self):
        """[(name, load_addr, bytes)]: the static main image, plus the autoload
        sections the startup code copies into ITCM and DTCM (module params)."""
        img, _ = self.arm9()
        base = self.arm9_load
        m = img.find(struct.pack('<I', 0xDEC00621))
        if m < 0x1C: return [('main', base, img)]
        pa = m - 0x1C
        al_start, al_end, al_src = u32(img, pa), u32(img, pa + 4), u32(img, pa + 8)
        segs = [('main', base, img[:al_src - base])]
        o, src = al_start - base, al_src
        while o < al_end - base:
            dest, size, bss = struct.unpack_from('<3I', img, o)
            name = 'itcm' if dest < 0x02000000 else ('dtcm' if dest >= 0x027C0000 else 'autoload')
            segs.append((name, dest, img[src - base:src - base + size]))
            src += size; o += 12
        return segs

    def arm7(self):
        return self.data[self.arm7_off:self.arm7_off + self.arm7_size]

    def arm7_segments(self):
        """[(name, load_addr, bytes)]: the ARM7 startup code, plus the sections
        it copies into WRAM and main RAM before jumping to the program. The
        copy table pointer is the literal the startup code's copy routine
        loads (NitroSDK crt0: entry, then BL autoload; the table holds
        start/end of a {dest, size, bss} list and the source address)."""
        img = self.arm7()
        base = self.arm7_load
        u = lambda a: u32(img, a - base)
        # crt0's autoload routine is the first BL target; its first instruction
        # is LDR r0, [pc, #imm] naming the table descriptor.
        bl = None
        for o in range(0, 0x100, 4):
            w = u32(img, o)
            if (w & 0xFF000000) == 0xEB000000:
                off = ((w & 0xFFFFFF) ^ 0x800000) - 0x800000
                bl = base + o + 8 + off * 4
                break
        if bl is None: return [('main', base, img)]
        w = u(bl)
        if (w & 0xFFFFF000) != 0xE59F0000: return [('main', base, img)]
        desc = u(bl + 8 + (w & 0xFFF))
        start, end, src = u(desc), u(desc + 4), u(desc + 8)
        segs = [('crt0', base, img[:src - base])]
        o = start
        while o < end:
            dest, size = u(o), u(o + 4)
            name = 'wram' if dest >= 0x03000000 else 'main'
            segs.append((name, dest, img[src - base:src - base + size]))
            src += size; o += 12
        return segs

    def overlays9(self):
        out = []
        for i in range(self.ov9_size // 32):
            ov_id, ram, size, bss, sinit_s, sinit_e, fid, flags = struct.unpack_from('<8I', self.data, self.ov9_off + i * 32)
            raw = self.file(fid)
            compressed = bool(flags & 0x01000000)
            img = blz_decompress(raw[:flags & 0xFFFFFF]) if compressed else raw
            out.append(dict(id=ov_id, ram=ram, size=size, bss=bss, sinit=(sinit_s, sinit_e),
                            file=fid, compressed=compressed, image=img))
        return out

if __name__ == '__main__':
    import sys
    r = Rom(sys.argv[1])
    print(r.title, r.code, 'rev', r.version, r.sha1)
    a9, c = r.arm9()
    print('arm9 load %08x entry %08x raw %d -> %d bytes (compressed=%s)' % (r.arm9_load, r.arm9_entry, r.arm9_size, len(a9), c))
    print('arm7 load %08x entry %08x %d bytes' % (r.arm7_load, r.arm7_entry, r.arm7_size))
    for o in r.overlays9():
        print('ov%02d ram %08x size %6x (image %6x) bss %5x sinit %08x-%08x %s' % (
            o['id'], o['ram'], o['size'], len(o['image']), o['bss'], o['sinit'][0], o['sinit'][1],
            'blz' if o['compressed'] else ''))
