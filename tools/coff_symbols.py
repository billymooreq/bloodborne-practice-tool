"""Dump the COFF symbol table of a PE (MinGW/clang builds keep it unless stripped).
usage: python coff_symbols.py <exe> > symbols.txt   (columns: RVA section class name)"""
import struct, sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
data = bytes(pe.__data__)
ptr, n = pe.FILE_HEADER.PointerToSymbolTable, pe.FILE_HEADER.NumberOfSymbols
strtab = ptr + n * 18
secs = pe.sections
i = 0
while i < n:
    rec = data[ptr + i * 18: ptr + i * 18 + 18]
    name, value, secnum, typ, sclass, naux = struct.unpack("<8sIhHBB", rec)
    if name[:4] == b"\0\0\0\0":
        off = struct.unpack("<I", name[4:])[0]
        end = data.index(b"\0", strtab + off)
        name = data[strtab + off:end]
    else:
        name = name.rstrip(b"\0")
    if 0 < secnum <= len(secs):
        rva = secs[secnum - 1].VirtualAddress + value
        print("%08x %-8s %3d %s" % (rva, secs[secnum - 1].Name.rstrip(b"\0").decode(), sclass, name.decode(errors="replace")))
    i += 1 + naux
