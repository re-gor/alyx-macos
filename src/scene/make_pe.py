"""Package one relocation-free COFF text section as a minimal diagnostic PE."""
from pathlib import Path
import struct
import sys

root = Path(__file__).parent
stem = sys.argv[1] if len(sys.argv) > 1 else 'buffer_probe'
obj = (root / (stem + '.obj')).read_bytes()
machine, sections, _, symoff, symbols, _, _ = struct.unpack_from('<HHIIIHH', obj)
assert machine == 0x8664
size, raw, relocs, _, nrelocs = struct.unpack_from('<IIIIH', obj, 20 + 16)
assert nrelocs == 0
code = obj[raw:raw + size]
names = {}
strings = obj[symoff + 18 * symbols:]
i = 0
while i < symbols:
    offset = symoff + i * 18
    name = obj[offset:offset + 8]
    if name[:4] == b'\0' * 4:
        j = struct.unpack_from('<I', name, 4)[0]
        name = strings[j:strings.index(0, j)]
    else:
        name = name.rstrip(b'\0')
    value, section, _, _, aux = struct.unpack_from('<IhHBB', obj, offset + 8)
    if section == 1:
        names[name.decode()] = value
    i += 1 + aux

aligned = (len(code) + 511) & ~511
header = bytearray(512)
header[:2] = b'MZ'
struct.pack_into('<I', header, 0x3c, 0x80)
header[0x80:0x84] = b'PE\0\0'
struct.pack_into('<HHIIIHH', header, 0x84, 0x8664, 1, 0, 0, 0, 0xf0, 0x23)
opt = 0x98
struct.pack_into('<H', header, opt, 0x20b)
struct.pack_into('<I', header, opt + 4, aligned)
struct.pack_into('<IIQII', header, opt + 16, 0x1000 + names['entry'], 0x1000,
                 0x140000000, 0x1000, 0x200)
struct.pack_into('<HH', header, opt + 40, 6, 0)
struct.pack_into('<HH', header, opt + 48, 6, 0)
struct.pack_into('<II', header, opt + 56, 0x1000 + ((len(code) + 4095) & ~4095), 0x200)
struct.pack_into('<HH', header, opt + 68, 3, 0x100)
struct.pack_into('<QQQQII', header, opt + 72, 0x100000, 0x1000,
                 0x100000, 0x1000, 0, 16)
struct.pack_into('<II', header, opt + 120, 0x1000 + names['import_descriptors'], 40)
section = opt + 0xf0
header[section:section + 8] = b'.text\0\0\0'
struct.pack_into('<IIIIIIHHI', header, section + 8, len(code), 0x1000, aligned,
                 0x200, 0, 0, 0, 0, 0xe0000020)
(root / (stem + '.exe')).write_bytes(header + code + bytes(aligned - len(code)))
print('Built', stem + '.exe:', len(header) + aligned, 'bytes')
