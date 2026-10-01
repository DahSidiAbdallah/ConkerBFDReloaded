#!/usr/bin/env python3
"""
Prepare the decomp's conker.us.elf for N64Recomp.

N64Recomp splits code into functions using the ELF's STT_FUNC symbols and
their sizes, and turns every branch out of a function into a tail call to a
known function start. The decomp ELF needs some help to fit that model:

  * Functions still coming from splat `glabel`s in asm have st_size 0, and
    N64Recomp skips size-0 functions. Each gets a size reaching to the next
    function start in its section (or to the section's *_TEXT_END marker).
  * A function that falls through into the next label instead of ending in a
    jump is extended over that label (N64Recomp returns at a function's end).
    Overlapping functions are fine for N64Recomp.
  * The linker script's undefined_funcs_auto.txt (`func_X = 0x...;`) turns
    some asm glabels into SHN_ABS symbols. Those inside a code section are
    moved back into it. ABS `D_` labels inside code are continuations that
    hand-written asm jumps to through a register; they become `func_<addr>`.
  * Hand-written asm (the inflate code in .init, the math blob in .game)
    branches and jumps into the middle of other routines. Every such target is
    given a synthetic `func_<addr>` symbol so the branch becomes a tail call.
    This repeats until no new targets appear. Code addresses the game takes as
    values (in data, or built with lui/addiu) get one too, so callbacks resolve.
  * Data embedded in asm text without a symbol (EMBEDDED_DATA) ends the code
    before it.
  * .game's duplicate libultra functions (`osPfsInit2`, ...) get libultra's
    names when N64Recomp skips or replaces that name (needs symbol_lists.cpp).
  * Jump-table labels (`.L<addr>...`) that splat emitted as FUNC glabels become
    NOTYPE; `D_` symbols in code (data embedded in asm) become OBJECT and act
    only as boundaries.

  * Code that loads $ra with a code address: a hand-made call (`$ra = ret;
    j F`) becomes a jal, and a loop head kept in $ra moves to $k1, with the
    `jr $ra` gotos to it rewritten to `jr $k1` (see ra_as_code_pointer).
  * `jr rX` where rX holds the return address is rewritten to `jr $ra`, because
    recompiled calls never set $ra: either rX is a copy of $ra (hand-written asm
    saves it in a temporary), or every caller passes its return point in rX
    (`lui/addiu $t0, ret; jal F`, F returns with `jr $t0`).

Apart from the optional --original overlay, that return rewrite is the only
change to section contents. Symbols are
rewritten in place, and the synthetic ones are appended by moving
.symtab/.strtab to the end of the file.

  * With --original, the code sections are first overlaid with the original
    game's bytes, so functions whose decomp C doesn't match yet are recompiled
    from what actually shipped.

Usage: prepare_elf.py <in.elf> <out.elf> [symbol_lists.cpp] [--original .game=game.us.bin ...]
"""

import re
import struct
import sys

STT_NOTYPE, STT_OBJECT, STT_FUNC = 0, 1, 2
STB_GLOBAL = 1
SHN_LORESERVE, SHN_ABS = 0xFF00, 0xFFF1
CODE_SECTIONS = {".init", ".game", ".debugger"}
DATA_SECTIONS = (".init_data", ".game_data")

# Data embedded in hand-written asm text with no D_ symbol of its own.
EMBEDDED_DATA = {
    0x150A9C3C: "padding and float table (D_150A9C40) in the math code (D7980.s)",
    0x150AA98C: "debug printf format string before func_150AA9A0 (math blob in D7980.s)",
}
SYM_ENT = 16


class Elf:
    def __init__(self, raw):
        self.data = bytearray(raw)
        if self.data[:4] != b"\x7fELF" or self.data[4] != 1 or self.data[5] != 2:
            sys.exit("expected a 32-bit big-endian ELF")
        self.shoff, = struct.unpack_from(">I", self.data, 0x20)
        self.shentsize, self.shnum, shstrndx = struct.unpack_from(">HHH", self.data, 0x2E)
        self.shdrs = [list(struct.unpack_from(">10I", self.data, self.shoff + i * self.shentsize))
                      for i in range(self.shnum)]
        shstr = self.shdrs[shstrndx][4]
        self.names = [self.cstr(shstr + sh[0]) for sh in self.shdrs]
        self.symtab = self.names.index(".symtab")
        self.strtab = self.shdrs[self.symtab][6]

    def cstr(self, off):
        return self.data[off:self.data.index(b"\0", off)].decode()

    def symbols(self):
        _, _, _, _, off, size, _, _, _, _ = self.shdrs[self.symtab]
        str_off = self.shdrs[self.strtab][4]
        out = []
        for k in range(size // SYM_ENT):
            name, value, sz, info, other, shndx = struct.unpack_from(">IIIBBH", self.data, off + k * SYM_ENT)
            out.append({"name": self.cstr(str_off + name), "name_off": name, "value": value,
                        "size": sz, "info": info, "other": other, "shndx": shndx})
        return out

    def write(self, path, syms):
        """Rewrite .symtab/.strtab (appended at the end of the file) from `syms`."""
        old_str = self.shdrs[self.strtab]
        strtab = bytearray(self.data[old_str[4]:old_str[4] + old_str[5]])
        symtab = bytearray()
        for s in syms:
            if s.get("new"):
                s["name_off"] = len(strtab)
                strtab += s["name"].encode() + b"\0"
            symtab += struct.pack(">IIIBBH", s["name_off"], s["value"], s["size"], s["info"], s["other"], s["shndx"])
        out = bytearray(self.data)
        for idx, blob in ((self.symtab, symtab), (self.strtab, strtab)):
            while len(out) % 4:
                out.append(0)
            self.shdrs[idx][4] = len(out)
            self.shdrs[idx][5] = len(blob)
            out += blob
        for i, sh in enumerate(self.shdrs):
            struct.pack_into(">10I", out, self.shoff + i * self.shentsize, *sh)
        open(path, "wb").write(out)

    def word(self, shndx, addr):
        sh = self.shdrs[shndx]
        return struct.unpack_from(">I", self.data, sh[4] + addr - sh[3])[0]

    def set_word(self, shndx, addr, value):
        sh = self.shdrs[shndx]
        struct.pack_into(">I", self.data, sh[4] + addr - sh[3], value)


def is_unconditional(w):
    op = w >> 26
    return (op == 2                                        # j
            or (op == 0 and (w & 0x3F) == 8)               # jr
            or (op == 4 and (w >> 16) & 0x3FF == 0)        # b (beq $zero, $zero)
            or w == 0x42000018)                            # eret


def written_gpr(w):
    """The general register an instruction writes (0 or None if none)."""
    op = w >> 26
    if op == 0:                                            # SPECIAL: rd
        return (w >> 11) & 0x1F
    if op == 3 or (op == 1 and (w >> 16) & 0x1F in (16, 17, 18, 19)):
        return 31                                          # jal, bltzal/bgezal(l)
    if op in (16, 17, 18) and (w >> 21) & 0x1F in (0, 1, 2):
        return (w >> 16) & 0x1F                            # mfc/dmfc/cfc
    if 8 <= op <= 15 or op in (24, 25, 26, 27, 55) or 32 <= op <= 39:
        return (w >> 16) & 0x1F                            # immediate ALU ops, loads
    return None


RA = 31
JR_RA = 0x03E00008


def move_source(w, dest):
    """If w copies a register into `dest` (or/addu dest, rs, $zero), return rs, else None."""
    if w >> 26 != 0 or (w & 0x3F) not in (0x21, 0x25) or (w >> 11) & 0x1F != dest:
        return None
    rs, rt = (w >> 21) & 0x1F, (w >> 16) & 0x1F
    if rt == 0:
        return rs
    if rs == 0:
        return rt
    return None


def writes_gpr(w):
    """Destination GPR of common instructions (enough to invalidate tracked copies)."""
    op = w >> 26
    if op == 0:
        return (w >> 11) & 0x1F
    if op in (3,):                                         # jal writes $ra
        return RA
    if op in (8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38, 39, 55):  # imm ALU, lui, loads, ld
        return (w >> 16) & 0x1F
    if op == 17 and ((w >> 21) & 0x1F) in (0, 1, 2):      # mfc1, dmfc1, cfc1
        return (w >> 16) & 0x1F
    return None


def normalise_returns(elf, sec, start, end):
    """Rewrite `jr rX` to `jr $ra` where rX holds the function's return address.

    Hand-written asm keeps $ra in a temporary (`move $t7, $ra` on entry) and returns
    with `jr $t7`, sometimes through a shared epilogue that first restores
    `move $ra, $t7`. N64Recomp treats a non-$ra jr as an indirect jump through the
    register's value, but recompiled calls never set $ra, so those must be returns.
    """
    rewritten = 0
    copies = set()
    for pc in range(start, end, 4):
        w = elf.word(sec, pc)
        if w >> 26 == 0 and (w & 0x3F) == 8:                # jr
            rs = (w >> 21) & 0x1F
            restored = any(move_source(elf.word(sec, q), RA) == rs
                           for q in (pc - 4, pc + 4) if start <= q < end)
            if rs != RA and (rs in copies or restored):
                elf.set_word(sec, pc, JR_RA)
                rewritten += 1
            continue
        src = move_source(w, (w >> 11) & 0x1F)
        dest = writes_gpr(w)
        if dest is None or dest == 0:
            continue
        if src == RA and dest != RA:
            copies.add(dest)
        else:
            copies.discard(dest)
    return rewritten


def manual_link_returns(elf, funcs):
    """Rewrite `jr rX` to `jr $ra` where callers pass their return address in rX.

    Hand-written asm sometimes loads a register with the call's return point
    before a jal (`lui/addiu $t0, ret; jal F`) and F returns with `jr $t0`,
    possibly from code F tail-jumps to. Recompiled, that jr would call the
    continuation and then return into the caller, running the code after the
    call twice. Only done when every jal to F passes the same register.
    """
    by_start = {s["value"]: s for s in funcs}
    starts = sorted(by_start)

    def containing(addr):
        return [s for s in funcs if s["value"] <= addr < s["value"] + s["size"]]

    # 1. jal sites: which register (if any) holds the return point.
    links = {}                                     # callee -> set of link regs (None = plain call)
    for s in funcs:
        sec, start, end = s["shndx"], s["value"], s["value"] + s["size"]
        regs = {}                                  # reg -> value built by lui/addiu or lui/ori
        for pc in range(start, end, 4):
            w = elf.word(sec, pc)
            op, rs, rt = w >> 26, (w >> 21) & 0x1F, (w >> 16) & 0x1F
            if op == 15:
                regs[rt] = (w & 0xFFFF) << 16
            elif op in (9, 13) and rs == rt and rt in regs:
                regs[rt] = (regs[rt] + (((w & 0xFFFF) ^ 0x8000) - 0x8000 if op == 9 else (w & 0xFFFF))) & 0xFFFFFFFF
            elif op == 3:
                callee = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                link = next((r for r, v in regs.items() if v == pc + 8), None)
                links.setdefault(callee, set()).add(link)
                regs.clear()
            else:
                d = writes_gpr(w)
                if d:
                    regs.pop(d, None)

    # 2. For callees always called with the same manual link register, rewrite
    #    `jr reg` in the callee and in everything it reaches by j/b (tail jumps).
    rewritten = 0
    for callee, regset in links.items():
        if len(regset) != 1 or None in regset or callee not in by_start:
            continue
        reg = next(iter(regset))
        # A callee that code above it can also fall into (the instruction before the
        # delay slot isn't an unconditional jump) is entered without a call there,
        # and the rewritten return would leave the recompiled function instead of
        # jumping to whatever that path put in the link register. Those entries need
        # a conker.toml hook (see func_150AC1B4).
        above = containing(callee - 8)
        if above and not is_unconditional(elf.word(above[0]["shndx"], callee - 8)):
            print(f"prepare_elf: warning: {callee:#010x} returns through ${reg} (rewritten to $ra) "
                  f"but can be entered by falling through from {callee - 8:#010x}", file=sys.stderr)
        seen, work = set(), [callee]
        while work:
            f = by_start.get(work.pop())
            if f is None or f["value"] in seen:
                continue
            seen.add(f["value"])
            sec, start, end = f["shndx"], f["value"], f["value"] + f["size"]
            for pc in range(start, end, 4):
                w = elf.word(sec, pc)
                if w >> 26 == 0 and (w & 0x3F) == 8 and (w >> 21) & 0x1F == reg:
                    elf.set_word(sec, pc, JR_RA)
                    rewritten += 1
                t = branch_target(w, pc)
                if t is not None and w >> 26 != 3 and not (start <= t < end) and t in by_start:
                    work.append(t)
        for addr, how in other_entries(elf, funcs, [by_start[v] for v in seen]):
            print(f"  warning: {addr:08X} enters {callee:08X}'s manual-link code by {how}; "
                  f"its jr ${reg} became jr $ra (see conker.toml)")
    return rewritten


def other_entries(elf, funcs, group):
    """Ways into a group of functions other than calls: fall-through from the
    preceding code, and j/branches from code outside the group."""
    inside = lambda a: any(f["value"] <= a < f["value"] + f["size"] for f in group)
    found = []
    for f in group:
        # Functions that run into f from before it (sized over a fall-through).
        for g in funcs:
            if g not in group and g["value"] < f["value"] < g["value"] + g["size"]:
                found.append((f["value"] - 4, f"fall-through (in func_{g['value']:08X})"))
    for g in funcs:
        if g in group:
            continue
        for pc in range(g["value"], g["value"] + g["size"], 4):
            w = elf.word(g["shndx"], pc)
            t = branch_target(w, pc)
            if t is not None and w >> 26 != 3 and inside(t) and not (g["value"] <= t < g["value"] + g["size"]):
                found.append((pc, "jump"))
    return found


K1 = 27


def ra_as_code_pointer(elf, sec_of, text_ranges):
    """Handle hand-written asm that loads $ra with a code address.

    Two shapes occur in Conker's math code:
      * `lui/addiu $ra, X` then `j F` where X is the j's return point: a call
        written by hand. The j becomes a jal (which sets $ra = X itself) and the
        lui/addiu become nops.
      * `lui/addiu $ra, HEAD` with the real return address parked in an FPR:
        every `jr $ra` reached while $ra still holds HEAD loops back to HEAD. The
        constant goes into $k1 instead (never used by game code), and those jr
        become `jr $k1`, which N64Recomp emits as an indirect jump to HEAD. The
        real `jr $ra` returns (after `mfc1 $ra, $fN`) are left alone.
    Returns (calls, loops, gotos, conflicts).
    """
    def word(pc):
        return elf.word(sec_of(pc), pc)

    def jr_reg(w):
        return (w >> 21) & 0x1F if w >> 26 == 0 and (w & 0x3F) == 8 else None

    calls = loops = gotos = 0
    conflicts = []
    goto_sites, return_sites = set(), set()
    loads = []
    for lo_, hi_ in text_ranges:
        for pc in range(lo_, hi_ - 4, 4):
            w1, w2 = word(pc), word(pc + 4)
            if w1 >> 16 == 0x3C1F and w2 >> 26 in (9, 13) and (w2 >> 16) & 0x3FF == (RA << 5) | RA:
                imm = ((w2 & 0xFFFF) ^ 0x8000) - 0x8000 if w2 >> 26 == 9 else (w2 & 0xFFFF)
                value = (((w1 & 0xFFFF) << 16) + imm) & 0xFFFFFFFF
                if any(a <= value < b for a, b in text_ranges):
                    loads.append((pc, value))

    for pc, value in loads:
        nxt = word(pc + 8)
        if nxt >> 26 == 2 and value == pc + 16:            # lui; addiu; j F; <delay>  -> jal F
            elf.set_word(sec_of(pc + 8), pc + 8, (3 << 26) | (nxt & 0x03FFFFFF))
            elf.set_word(sec_of(pc), pc, 0)
            elf.set_word(sec_of(pc + 4), pc + 4, 0)
            calls += 1
            continue
        # Walk every path on which $ra still holds `value`.
        seen, work, found = set(), [pc + 8], []
        while work:
            p = work.pop()
            while p not in seen and any(a <= p < b for a, b in text_ranges):
                seen.add(p)
                w = word(p)
                if jr_reg(w) == RA:
                    found.append(p)
                    break
                t = branch_target(w, p)
                op = w >> 26
                if op == 3 or (op == 0 and (w & 0x3F) == 9):  # jal/jalr: $ra is overwritten
                    break
                if writes_gpr(w) == RA or (op == 17 and (w >> 21) & 0x1F == 0 and (w >> 16) & 0x1F == RA):
                    break
                if jr_reg(w) is not None:                    # jump table / indirect: stop
                    break
                if t is not None:
                    work.append(t)
                    if op == 2 or (op == 4 and (w >> 16) & 0x3FF == 0):   # j, b: no fall-through
                        seen.add(p + 4)
                        # the delay slot still executes, but can't change $ra here in practice
                        break
                p += 4
        if not found:
            continue
        loops += 1
        elf.set_word(sec_of(pc), pc, (elf.word(sec_of(pc), pc) & ~(0x1F << 16)) | (K1 << 16))
        w2 = elf.word(sec_of(pc + 4), pc + 4)
        elf.set_word(sec_of(pc + 4), pc + 4, (w2 & ~(0x3FF << 16)) | (K1 << 21) | (K1 << 16))
        for p in found:
            goto_sites.add(p)

    # A jr $ra reached as a goto must not also be a genuine return: those are the
    # jr $ra right after the $ra restore (mfc1 $ra) or an epilogue (lw $ra).
    for p in sorted(goto_sites):
        prev = word(p - 4), word(p - 8)
        restores = any(writes_gpr(x) == RA or (x >> 26 == 17 and (x >> 21) & 0x1F == 0 and (x >> 16) & 0x1F == RA)
                       for x in prev)
        if restores:
            conflicts.append(p)
            continue
        elf.set_word(sec_of(p), p, (K1 << 21) | 8)
        gotos += 1
    return calls, loops, gotos, conflicts


def branch_target(w, pc):
    """Static target of a branch/jump at pc, or None."""
    op = w >> 26
    if op in (2, 3):                                       # j, jal
        return ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    off = (pc + 4 + (((w & 0xFFFF) ^ 0x8000) - 0x8000) * 4) & 0xFFFFFFFF
    if op in (4, 5, 6, 7, 20, 21, 22, 23):                 # beq bne blez bgtz (+likely)
        return off
    if op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 16, 17, 18, 19):  # bltz bgez (+likely, +al)
        return off
    if op == 17 and ((w >> 21) & 0x1F) == 8:               # bc1f bc1t (+likely)
        return off
    return None


def load_n64recomp_names(path):
    """Names N64Recomp never emits: its reimplemented and ignored lists in symbol_lists.cpp."""
    text = open(path).read()
    names = set()
    for table in ("reimplemented_funcs", "ignored_funcs"):
        m = re.search(table + r"\s*\{(.*?)\};", text, re.S)
        if m:
            names.update(re.findall(r'"(\w+)"', m.group(1)))
    return names


def overlay_original_code(elf, originals):
    """Replace code section contents with the original game's bytes.

    `originals` maps a section name to a dump of that segment starting at the
    section's vram. The decomp has drift 0, so every function in the ELF sits at
    its original address; recompiling the original bytes with the decomp's
    symbols avoids inheriting any C function that doesn't match yet.
    """
    replaced = {}
    for name, path in originals.items():
        idx = elf.names.index(name)
        sh = elf.shdrs[idx]
        blob = open(path, "rb").read()
        if len(blob) < sh[5]:
            sys.exit(f"{path} is shorter than {name} (0x{len(blob):X} < 0x{sh[5]:X})")
        old = bytes(elf.data[sh[4]:sh[4] + sh[5]])
        elf.data[sh[4]:sh[4] + sh[5]] = blob[:sh[5]]
        replaced[name] = sum(1 for k in range(0, sh[5], 4) if old[k:k + 4] != blob[k:k + 4])
    return replaced


def main(src, dst, symbol_lists=None, originals=None):
    elf = Elf(open(src, "rb").read())
    syms = elf.symbols()
    if originals:
        for name, words in overlay_original_code(elf, originals).items():
            print(f"{name}: original code overlaid ({words} words differed from the decomp build)")

    # The decomp names .game's second copy of libultra's controller/Controller Pak
    # code `<name>2`. Give those libultra's names so N64Recomp treats them like the
    # .init originals. Only names on N64Recomp's lists are safe: they are never
    # emitted, so the duplicate name doesn't produce two C definitions.
    aliased = 0
    # The decomp has .game's EEPROM read and write swapped: 0x151DD140 packs
    # command 4 (read: 2 bytes out, 8 in) and 0x151DD4E0 command 5 (write: 10 out,
    # 1 in). N64Recomp replaces these libultra functions by name, so every save
    # read the EEPROM over the game's buffer and every load wrote it.
    corrected = {0x151DD140: "osEepromRead", 0x151DD304: "__osPackEepReadData",
                 0x151DD4E0: "osEepromWrite", 0x151DD65C: "__osPackEepWriteData"}
    for s in syms:
        if (s["info"] & 0xF) == STT_FUNC and corrected.get(s["value"], s["name"]) != s["name"]:
            s["name"], s["new"] = corrected[s["value"]], True
    if symbol_lists:
        known = load_n64recomp_names(symbol_lists)
        func_names = {s["name"] for s in syms if (s["info"] & 0xF) == STT_FUNC}
        for s in syms:
            base = s["name"][:-1]
            if (s["info"] & 0xF) == STT_FUNC and s["name"].endswith("2") and base in known and base in func_names:
                s["name"], s["new"] = base, True
                aliased += 1
    code = {i for i, n in enumerate(elf.names) if n in CODE_SECTIONS}
    lo = {i: elf.shdrs[i][3] for i in code}
    text_end = {i: elf.shdrs[i][3] + elf.shdrs[i][5] for i in code}
    for s in syms:
        if s["name"].endswith("_TEXT_END"):
            for i in code:
                if elf.names[i] == "." + s["name"][:-len("_TEXT_END")]:
                    text_end[i] = min(text_end[i], s["value"])

    def owner(addr):
        return next((i for i in code if lo[i] <= addr < text_end[i]), None)

    stats = dict(adopted=0, labels=0, data=0, interior=0, synthetic=0, extended=0, pointers=0)
    kinds = {i: {} for i in code}          # start addr -> "func" | "data"
    all_names = {s["name"] for s in syms}
    c_funcs = {i: [] for i in code}        # (start, end) of sized (compiled C) functions
    asm_funcs = []                         # symbols whose size we compute

    for s in syms:
        typ = s["info"] & 0xF
        if typ != STT_FUNC:
            continue
        if s["shndx"] == SHN_ABS:
            sec = owner(s["value"])
            if sec is None or s["name"].startswith(".L") or s["value"] in EMBEDDED_DATA:
                continue
            if s["name"].startswith("D_"):
                # Hand-written asm loads code addresses as continuations
                # (`lui/addiu $t1, D_...` then `jr $t1`), so splat named them D_.
                code_name = f"func_{s['value']:08X}"
                if code_name in all_names:
                    continue
                s["name"], s["new"] = code_name, True
                all_names.add(code_name)
            s["shndx"] = sec
            stats["adopted"] += 1
        sec = s["shndx"]
        if sec >= SHN_LORESERVE or sec not in code:
            continue
        if s["name"].startswith(".L"):
            s["info"] = (s["info"] & 0xF0) | STT_NOTYPE
            stats["labels"] += 1
        elif s["name"].startswith("D_") or s["value"] >= text_end[sec]:
            if s["value"] < text_end[sec]:
                kinds[sec].setdefault(s["value"], "data")
            s["info"] = (s["info"] & 0xF0) | STT_OBJECT
            stats["data"] += 1
        else:
            kinds[sec][s["value"]] = "func"
            if s["size"]:
                c_funcs[sec].append((s["value"], s["value"] + s["size"]))
            else:
                asm_funcs.append(s)

    for addr in EMBEDDED_DATA:
        sec = owner(addr)
        if sec is not None:
            kinds[sec][addr] = "data"

    def inside_c(sec, addr):
        return any(a < addr < b for a, b in c_funcs[sec])

    # An adopted label inside a compiled C function is an interior label, not a start.
    for s in list(asm_funcs):
        if inside_c(s["shndx"], s["value"]):
            kinds[s["shndx"]].pop(s["value"], None)
            s["info"] = (s["info"] & 0xF0) | STT_NOTYPE
            asm_funcs.remove(s)
            stats["interior"] += 1

    def ends_control_flow(sec, a, b):
        p = b - 4
        while p >= a and elf.word(sec, p) == 0:
            p -= 4
        return any(q >= a and is_unconditional(elf.word(sec, q)) for q in (p, p - 4))

    taken = {s["name"] for s in syms}
    jtbl_labels = {s["value"] for s in syms if s["name"].startswith(".L")}

    def add_start(sec, t):
        name = f"func_{t:08X}"
        if name in taken:
            name += "_label"
        taken.add(name)
        s = {"name": name, "new": True, "value": t, "size": 0, "info": (STB_GLOBAL << 4) | STT_FUNC,
             "other": 0, "shndx": sec}
        syms.append(s)
        asm_funcs.append(s)
        kinds[sec][t] = "func"

    # Code addresses taken as values (callbacks, function-pointer tables): from
    # words in the data sections and from lui/addiu (or lui/ori) pairs in code.
    # Anything not already a start or a jump-table label becomes one, even inside
    # a compiled C function (e.g. a bare `jr $ra` used as an empty callback).
    # Data words inside a compiled C function are nearly always jump-table cases
    # the decomp hasn't labelled, so those are only taken from lui pairs, and only
    # when the code from there to the function's end doesn't branch back out.
    def c_function_containing(sec, t):
        return next(((a, b) for a, b in c_funcs[sec] if a < t < b), None)

    def self_contained(sec, t, end):
        for pc in range(t, end, 4):
            tgt = branch_target(elf.word(sec, pc), pc)
            if tgt is not None and not (t <= tgt < end) and owner(tgt) is not None and tgt not in kinds[owner(tgt)]:
                return False
        return True

    data_pointers, code_pointers = set(), set()
    for name in DATA_SECTIONS:
        if name in elf.names:
            sh = elf.shdrs[elf.names.index(name)]
            for k in range(0, sh[5] - 3, 4):
                data_pointers.add(struct.unpack_from(">I", elf.data, sh[4] + k)[0])
    for i in code:
        regs = {}
        for pc in range(lo[i], text_end[i], 4):
            w = elf.word(i, pc)
            op, rs, rt = w >> 26, (w >> 21) & 0x1F, (w >> 16) & 0x1F
            if op == 15:
                regs[rt] = (w & 0xFFFF) << 16
                continue
            if op in (9, 13) and rs in regs:
                imm = ((w & 0xFFFF) ^ 0x8000) - 0x8000 if op == 9 else (w & 0xFFFF)
                code_pointers.add((regs[rs] + imm) & 0xFFFFFFFF)
            # Any other write replaces the lui's value (e.g. `lw $a0, 0x134($sp)` then
            # `addiu $a1, $a0, -0x20` in func_151978xx isn't a pointer into 0x1518FFE0).
            regs.pop(written_gpr(w), None)
    for t in sorted(data_pointers | code_pointers):
        sec = owner(t)
        if sec is None or t & 3 or t in kinds[sec] or t in jtbl_labels:
            continue
        in_c = c_function_containing(sec, t)
        # A data word may still point at a function that the decomp's symbol
        # swallowed: one starting right after the previous code's `jr $ra` return
        # (e.g. the empty callback 0x1506D4EC at the end of func_1506D2E8.s).
        after_return = in_c and t - 8 >= in_c[0] and elf.word(sec, t - 8) == JR_RA
        if in_c and not ((t in code_pointers or after_return) and self_contained(sec, t, in_c[1])):
            continue
        if in_c and t not in code_pointers:
            print(f"prepare_elf: code pointer {t:#010x} from data, after a return inside {in_c[0]:#010x}")
        add_start(sec, t)
        stats["pointers"] += 1

    def size_functions():
        ordered = {i: sorted(set(k) | {text_end[i]}) for i, k in kinds.items()}
        for s in asm_funcs:
            sec, start = s["shndx"], s["value"]
            b = ordered[sec]
            k = next(n for n, a in enumerate(b) if a > start)
            while not ends_control_flow(sec, start, b[k]) and b[k] < text_end[sec] and kinds[sec].get(b[k]) == "func":
                k += 1
            s["size"] = b[k] - start

    while True:
        size_functions()

        new_targets = set()
        for s in asm_funcs:
            sec, start, end = s["shndx"], s["value"], s["value"] + s["size"]
            for pc in range(start, end, 4):
                t = branch_target(elf.word(sec, pc), pc)
                if t is None or start <= t < end or t & 3:
                    continue
                tsec = owner(t)
                if tsec is None or t in kinds[tsec] or inside_c(tsec, t):
                    continue
                new_targets.add((tsec, t))
        if not new_targets:
            break
        for sec, t in sorted(new_targets):
            add_start(sec, t)
            stats["synthetic"] += 1

    starts = {i: sorted(k) for i, k in kinds.items()}
    stats["returns"] = sum(normalise_returns(elf, s["shndx"], s["value"], s["value"] + s["size"]) for s in asm_funcs)
    stats["returns"] += manual_link_returns(elf, asm_funcs)
    ranges = [(lo[i], text_end[i]) for i in code]
    calls, loops, gotos, conflicts = ra_as_code_pointer(elf, owner, ranges)
    stats["ra_calls"], stats["ra_loops"], stats["ra_gotos"] = calls, loops, gotos
    if calls:
        # A hand-made call's j ended its function when it was sized; as a jal it
        # returns into the code after it (e.g. func_150A7A00, whose continuation
        # at 0x150A7A14 stores the transform's W and returns with jr $t9).
        size_functions()
        stats["returns"] += sum(normalise_returns(elf, s["shndx"], s["value"], s["value"] + s["size"]) for s in asm_funcs)
    for p in conflicts:
        print(f"  warning: jr $ra at {p:08X} is both a loop goto and a return; left as a return")
    for s in asm_funcs:
        nxt = next(a for a in starts[s["shndx"]] + [text_end[s["shndx"]]] if a > s["value"])
        if s["value"] + s["size"] > nxt:
            stats["extended"] += 1

    elf.write(dst, syms)
    print("{n} asm functions sized ({extended} extended over a fall-through), {synthetic} synthetic branch "
          "targets, {adopted} ABS labels adopted ({interior} interior), {labels} jump-table labels, "
          "{data} data symbols, {pointers} code-pointer targets, {ra_calls} hand-made calls and {ra_loops} "
          "$ra loop heads ({ra_gotos} gotos), {aliased} libultra duplicates aliased, {returns} jr-through-$ra-copy returns -> {dst}".format(
              n=len(asm_funcs), dst=dst, aliased=aliased, **stats))


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="Prepare the decomp ELF for N64Recomp.")
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("symbol_lists", nargs="?", help="N64Recomp/src/symbol_lists.cpp")
    ap.add_argument("--original", action="append", default=[], metavar="SECTION=FILE",
                    help="overlay a code section with the original segment dump (repeatable)")
    args = ap.parse_args()
    originals = dict(o.split("=", 1) for o in args.original)
    main(args.src, args.dst, args.symbol_lists, originals)
