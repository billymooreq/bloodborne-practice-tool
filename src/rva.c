/* rva.c: the RVAs of rva.h, looked up by name in the COFF symbol table of bb-probe.exe on disk
 * (the port keeps it in every release), the same way as gen_rva.py: a name that occurs more than
 * once is picked by its distance to the first occurrence of a neighbour name. Runs in DllMain. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define RVA_NAMES
#include "rva.h"

uint64_t g_rva[RVA_COUNT];

#pragma pack(push, 1)
typedef struct { char name[8]; uint32_t value; int16_t sec; uint16_t type; uint8_t cls, naux; } CoffSym;
#pragma pack(pop)

static const char *sym_name(const CoffSym *s, const char *strtab, char tmp[9]) {
    if (!*(const uint32_t *)s->name) return strtab + *(const uint32_t *)(s->name + 4);
    memcpy(tmp, s->name, 8); tmp[8] = 0;
    return tmp;
}

/* Fills g_rva; returns the number of names not found (their RVA stays 0), -1 when the exe has no
 * symbol table. `missing` gets the first missing names. */
int rva_resolve(const char *exe, void (*log)(const char *fmt, ...), char *missing, size_t mlen) {
    if (missing && mlen) missing[0] = 0;
    HANDLE f = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { log("rva: cannot open %s", exe); return -1; }
    HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    const uint8_t *b = m ? (const uint8_t *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    int res = -1;
    if (b) {
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        uint32_t nsec = nt->FileHeader.NumberOfSections, ptr = nt->FileHeader.PointerToSymbolTable, n = nt->FileHeader.NumberOfSymbols;
        if (ptr && n && (uint64_t)ptr + (uint64_t)n * 18 < (uint64_t)sz.QuadPart) {
            const CoffSym *syms = (const CoffSym *)(b + ptr);
            const char *strtab = (const char *)(b + ptr + n * 18);
            /* pass 1: index (as gen_rva.py counts: symbols in a section) of the first occurrence of each neighbour */
            int64_t anchor[RVA_COUNT]; uint64_t best[RVA_COUNT]; int64_t dist[RVA_COUNT];
            for (int k = 0; k < RVA_COUNT; ++k) { anchor[k] = -1; best[k] = 0; dist[k] = INT64_MAX; }
            char tmp[9];
            for (int pass = 0; pass < 2; ++pass) {
                int64_t idx = 0;
                for (uint32_t i = 0; i < n; i += 1 + syms[i].naux) {
                    const CoffSym *s = &syms[i];
                    if (s->sec <= 0 || (uint32_t)s->sec > nsec) continue;
                    const char *nm = sym_name(s, strtab, tmp);
                    for (int k = 0; k < RVA_COUNT; ++k) {
                        if (pass == 0) {
                            if (k_rva_syms[k].nb && anchor[k] < 0 && !strcmp(nm, k_rva_syms[k].nb)) anchor[k] = idx;
                        } else if (!strcmp(nm, k_rva_syms[k].name)) {
                            int64_t d = anchor[k] < 0 ? 0 : (idx > anchor[k] ? idx - anchor[k] : anchor[k] - idx);
                            if (d < dist[k]) { dist[k] = d; best[k] = sec[s->sec - 1].VirtualAddress + s->value; }   /* ties: the earlier one */
                            if (anchor[k] < 0) dist[k] = -1;   /* no neighbour: the first occurrence */
                        }
                    }
                    ++idx;
                }
            }
            res = 0;
            int moved = 0;
            for (int k = 0; k < RVA_COUNT; ++k) {
                g_rva[k] = best[k];
                if (!best[k]) {
                    if (missing && strlen(missing) + strlen(k_rva_syms[k].name) + 3 < mlen) { if (missing[0]) strcat_s(missing, mlen, ", "); strcat_s(missing, mlen, k_rva_syms[k].name); }
                    log("rva: symbol %s not found", k_rva_syms[k].name);
                    ++res;
                } else if (best[k] != k_rva_syms[k].v195) ++moved;
            }
            log("rva: %d symbols resolved from %u COFF records (%d differ from 1.9.5, %d missing)", RVA_COUNT - res, n, moved, res);
        } else log("rva: %s has no COFF symbol table", exe);
    }
    if (b) UnmapViewOfFile(b);
    if (m) CloseHandle(m);
    CloseHandle(f);
    return res;
}
