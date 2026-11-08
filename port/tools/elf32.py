"""Minimal ELF32 big-endian reader (stdlib only): sections and the symbol table of build/pokemonsnap.elf."""
import struct

STT = {0: 'none', 1: 'data', 2: 'func', 3: 'section', 4: 'file'}
STB = {0: 'local', 1: 'global', 2: 'weak'}


class Elf:
    def __init__(self, path):
        self.b = b = open(path, 'rb').read()
        if b[:4] != b'\x7fELF' or b[4] != 1 or b[5] != 2:
            raise ValueError(f'{path}: not ELF32 big-endian')
        (self.shoff,) = struct.unpack_from('>I', b, 0x20)
        self.shentsize, self.shnum, self.shstrndx = struct.unpack_from('>HHH', b, 0x2E)
        self.sections = []
        for i in range(self.shnum):
            f = struct.unpack_from('>IIIIIIIIII', b, self.shoff + i * self.shentsize)
            self.sections.append(dict(name_off=f[0], type=f[1], flags=f[2], addr=f[3], offset=f[4], size=f[5],
                                      link=f[6], info=f[7], entsize=f[9]))
        strtab = self.sections[self.shstrndx]
        for s in self.sections:
            s['name'] = self._str(strtab, s['name_off'])

    def _str(self, sec, off):
        o = sec['offset'] + off
        return self.b[o:self.b.index(b'\0', o)].decode('latin-1')

    def symbols(self):
        """[(name, value, size, type, bind, section name or 'abs'/'undef'/'common')] in symbol-table order."""
        out = []
        for s in self.sections:
            if s['type'] != 2:  # SHT_SYMTAB
                continue
            strs = self.sections[s['link']]
            for i in range(1, s['size'] // 16):
                name, value, size, info, other, shndx = struct.unpack_from('>IIIBBH', self.b, s['offset'] + i * 16)
                typ, bind = STT.get(info & 15, str(info & 15)), STB.get(info >> 4, str(info >> 4))
                if typ in ('section', 'file'):
                    continue
                sec = {0: 'undef', 0xFFF1: 'abs', 0xFFF2: 'common'}.get(shndx)
                if sec is None:
                    sec = self.sections[shndx]['name']
                out.append((self._str(strs, name), value, size, typ, bind, sec))
        return out
