#!/usr/bin/env python3
"""Generates src/z80core.s, the eZ80 assembly version of the Z80 interpreter.

The generated file is committed, so building GGCE doesn't need Python. Run
this again after changing it:

    python3 tools/gen_z80core.py

How the core keeps the Z80 in eZ80 registers while it runs:

    A F B C D E H L   the Z80's own A F B C D E H L (upper bytes ignored)
    IY                host pointer to the next Z80 code byte
    IX                cycles left in this run (counts down)
    BC'               address of the main dispatch table (constant)
    AF' DE' HL'       scratch

Every handler starts with the shadow registers and the scratch AF active
("S"), because that is how dispatch leaves them. Z80 IX, IY, SP, the
alternate registers and the interrupt state live in the C struct `z80`.

Native eZ80 instructions do the Z80's arithmetic: their flags match except
the undocumented bits 3 and 5. Memory goes through the machine's tables
z80_rmap/z80_wmap (256-byte windows), code fetches through z80_cmap (8 KB
regions), and anything else through C functions.
"""

import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "z80core.s")

R8 = ["b", "c", "d", "e", "h", "l", None, "a"]
RP = ["bc", "de", "hl", "sp"]
ALU = ["add", "adc", "sub", "sbc", "and", "xor", "or", "cp"]
ROT = ["rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"]
CC = ["nz", "z", "nc", "c", "po", "pe", "p", "m"]
NOT_CC = ["z", "nz", "c", "nc", "pe", "po", "m", "p"]

CYCLES = [
    4, 10, 7, 6, 4, 4, 7, 4, 4, 11, 7, 6, 4, 4, 7, 4,
    8, 10, 7, 6, 4, 4, 7, 4, 12, 11, 7, 6, 4, 4, 7, 4,
    7, 10, 16, 6, 4, 4, 7, 4, 7, 11, 16, 6, 4, 4, 7, 4,
    7, 10, 13, 6, 11, 11, 10, 4, 7, 11, 13, 6, 4, 4, 7, 4,
] + [4] * 64 + [4] * 64 + [
    5, 10, 10, 10, 10, 11, 7, 11, 5, 10, 10, 0, 10, 17, 7, 11,
    5, 10, 10, 11, 10, 11, 7, 11, 5, 4, 10, 11, 10, 0, 7, 11,
    5, 10, 10, 19, 10, 11, 7, 11, 5, 4, 10, 4, 10, 0, 7, 11,
    5, 10, 10, 4, 10, 11, 7, 11, 5, 6, 10, 4, 10, 0, 7, 11,
]
for op in range(0x40, 0x80):
    CYCLES[op] = 7 if (op & 7) == 6 or (op >> 3) & 7 == 6 else 4
CYCLES[0x76] = 4
for op in range(0x80, 0xC0):
    CYCLES[op] = 7 if (op & 7) == 6 else 4

_label = 0


def label():
    global _label
    _label += 1
    return "L%d" % _label


# ---- context switches and dispatch ----

DISPATCH = [
    "ld\tl, (iy + 0)",
    "inc\tiy",
    "ld\th, 4",
    "mlt\thl",
    "add\thl, bc",
    "ld\thl, (hl)",
    "jp\t(hl)",
]


# Hot handlers dispatch inline; the rest jump to one shared copy to save space.
INLINE = True


def dispatch():
    return DISPATCH if INLINE else ["jp\tdispatch"]


def next_s(n):          # from shadow regs + scratch AF
    return ["lea\tix, ix - %d" % n] + dispatch()


def next_ms(n):         # from main regs + scratch AF
    return ["lea\tix, ix - %d" % n, "exx"] + dispatch()


def next_sz(n):         # from shadow regs + Z80 AF
    return ["lea\tix, ix - %d" % n, "ex\taf, af'"] + dispatch()


def next_m(n):          # from main regs + Z80 AF
    return ["lea\tix, ix - %d" % n, "exx", "ex\taf, af'"] + dispatch()


def branch_s(n):
    """After a jump: also leave the run once its cycles are used up."""
    return ["lea\tix, ix - %d" % n, "ld\ta, ixh", "or\ta, a", "jp\tnz, run_exit"] + dispatch()


TO_M = ["exx", "ex\taf, af'"]
MAIN_HL = ["exx", "push\thl", "exx", "pop\thl"]          # HL' = Z80 HL


def main_rp(rp):
    return ["exx", "push\t%s" % rp, "exx", "pop\thl"]   # HL' = Z80 BC/DE/HL


# Scratch A holds a value; make it the Z80's A and switch to main + Z80 AF.
SET_A = ["ld\t(tmp8), a", "ex\taf, af'", "exx", "ld\ta, (tmp8)"]
# Scratch A = the Z80's A, staying in S.
GET_A = ["ex\taf, af'", "ld\t(tmp8), a", "ex\taf, af'", "ld\ta, (tmp8)"]


def get_r(r):
    """Scratch A = Z80 register r (stays in S)."""
    if r == "a":
        return GET_A
    return ["exx", "ld\ta, %s" % r, "exx"]


def put_r_then_next(r, n):
    """Z80 register r = scratch A, then continue."""
    if r == "a":
        return SET_A + next_m(n)
    return ["exx", "ld\t%s, a" % r] + next_ms(n)


def sign_ext_add(reg):
    """reg (iy or hl) += sign-extended byte at (iy + 0); iy advances past it."""
    skip = label()
    return [
        "ld\tde, 0",
        "ld\te, (iy + 0)",
        "inc\tiy",
        "bit\t7, e",
        "jr\tz, %s" % skip,
        "ld\tde, -256",
        "ld\te, (iy - 1)",
        "%s:" % skip,
        "add\t%s, de" % reg,
    ]


def idx_addr(ix):
    """HL' = Z80 IX/IY + d, where d is the next code byte."""
    return ["ld\thl, (Z80_%s)" % ix.upper()] + sign_ext_add("hl")


def store_word(field):
    """Store HL' (low 16 bits) into a 16-bit Z80 field without touching its neighbors."""
    return ["ld\ta, l", "ld\t(%s), a" % field, "ld\ta, h", "ld\t(%s + 1), a" % field]


def cond_jump_over(cc, target):
    """Test a Z80 condition from S; jump to target (in S) when it is false."""
    taken = label()
    return ["ex\taf, af'", "jp\t%s, %s" % (CC[cc], taken), "ex\taf, af'", "jp\t%s" % target,
            "%s:" % taken, "ex\taf, af'"]


# ---- unprefixed opcodes (also used by DD/FD for HL-free instructions) ----

def main_handler(op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    n = CYCLES[op]

    if op == 0x76:                                   # HALT
        return ["ld\ta, 1", "ld\t(Z80_HALTED), a", "lea\tix, ix - 4", "jp\trun_exit"]

    if x == 1:                                       # LD r,r'
        dst, src = R8[y], R8[z]
        if src is None:
            return MAIN_HL + ["call\trd8"] + put_r_then_next(dst, n)
        if dst is None:
            if src == "a":
                return GET_A + ["ld\te, a"] + MAIN_HL + ["ld\ta, e", "call\twr8"] + next_s(n)
            return ["exx", "ld\ta, %s" % src, "push\thl", "exx", "pop\thl", "call\twr8"] + next_s(n)
        if dst == src:
            return next_s(n)
        if "a" in (dst, src):
            return TO_M + ["ld\t%s, %s" % (dst, src)] + next_m(n)
        return ["exx", "ld\t%s, %s" % (dst, src)] + next_ms(n)

    if x == 2:                                       # ALU A,r
        if R8[z] is None:
            return MAIN_HL + ["call\trd8", "ld\te, a", "ex\taf, af'", "%s\ta, e" % ALU[y]] + next_sz(n)
        return TO_M + ["%s\ta, %s" % (ALU[y], R8[z])] + next_m(n)

    if x == 0:
        if z == 0:
            if y == 0:                               # NOP
                return next_s(n)
            if y == 1:                               # EX AF,AF'
                return ["ex\taf, af'", "push\taf", "ld\thl, (Z80_AF2)", "push\thl", "pop\taf",
                        "pop\thl", "ex\taf, af'"] + store_word("Z80_AF2") + next_s(n)
            if y == 2:                               # DJNZ
                no = label()
                return (["exx", "dec\tb", "exx", "jp\tz, %s" % no] + sign_ext_add("iy") + branch_s(13) +
                        ["%s:" % no, "inc\tiy"] + next_s(8))
            if y == 3:                               # JR d
                return sign_ext_add("iy") + branch_s(12)
            no = label()                             # JR cc,d
            return (cond_jump_over(y - 4, no) + sign_ext_add("iy") + branch_s(12) +
                    ["%s:" % no, "inc\tiy"] + next_s(7))
        if z == 1:
            if q == 0:                               # LD rr,nn
                if p == 3:
                    return ["ld\thl, (iy + 0)", "lea\tiy, iy + 2", "ld\t(Z80_SP), hl"] + next_s(n)
                return ["exx", "ld\t%s, (iy + 0)" % RP[p], "lea\tiy, iy + 2"] + next_ms(n)
            if p == 3:                               # ADD HL,rr
                return TO_M + ["push\tde", "ld\tde, (Z80_SP)", "add.sis\thl, de", "pop\tde"] + next_m(n)
            return TO_M + ["add.sis\thl, %s" % RP[p]] + next_m(n)
        if z == 2:
            if p < 2:                                # LD (BC)/(DE),A and LD A,(BC)/(DE)
                if q == 0:
                    return GET_A + ["ld\te, a"] + main_rp(RP[p]) + ["ld\ta, e", "call\twr8"] + next_s(n)
                return main_rp(RP[p]) + ["call\trd8"] + SET_A + next_m(n)
            if p == 2:
                if q == 0:                           # LD (nn),HL
                    return (["exx", "push\thl", "exx", "pop\tde", "ld\thl, (iy + 0)", "lea\tiy, iy + 2",
                             "push\thl", "push\tde", "ld\ta, e", "call\twr8", "pop\tde", "pop\thl",
                             "inc\thl", "ld\ta, d", "call\twr8"] + next_s(n))
                return (["ld\thl, (iy + 0)", "lea\tiy, iy + 2", "push\thl", "call\trd8",   # LD HL,(nn)
                         "exx", "ld\tl, a", "exx", "pop\thl", "inc\thl", "call\trd8",
                         "exx", "ld\th, a"] + next_ms(n))
            if q == 0:                               # LD (nn),A
                return GET_A + ["ld\te, a", "ld\thl, (iy + 0)", "lea\tiy, iy + 2", "ld\ta, e",
                                "call\twr8"] + next_s(n)
            return ["ld\thl, (iy + 0)", "lea\tiy, iy + 2", "call\trd8"] + SET_A + next_m(n)   # LD A,(nn)
        if z == 3:                                   # INC/DEC rr
            ins = "inc" if q == 0 else "dec"
            if p == 3:
                return ["ld\thl, (Z80_SP)", "%s\thl" % ins, "ld\t(Z80_SP), hl"] + next_s(n)
            return ["exx", "%s\t%s" % (ins, RP[p])] + next_ms(n)
        if z in (4, 5):                              # INC/DEC r
            ins = "inc" if z == 4 else "dec"
            if R8[y] is None:
                return (MAIN_HL + ["push\thl", "call\trd8", "ld\te, a", "ex\taf, af'", "%s\te" % ins,
                                   "ex\taf, af'", "ld\ta, e", "pop\thl", "call\twr8"] + next_s(n))
            return TO_M + ["%s\t%s" % (ins, R8[y])] + next_m(n)
        if z == 6:                                   # LD r,n
            if R8[y] is None:
                return ["ld\ta, (iy + 0)", "inc\tiy"] + MAIN_HL + ["call\twr8"] + next_s(n)
            if R8[y] == "a":
                return TO_M + ["ld\ta, (iy + 0)", "inc\tiy"] + next_m(n)
            return ["exx", "ld\t%s, (iy + 0)" % R8[y], "inc\tiy"] + next_ms(n)
        # z == 7: RLCA RRCA RLA RRA DAA CPL SCF CCF
        return TO_M + [["rlca", "rrca", "rla", "rra", "daa", "cpl", "scf", "ccf"][y]] + next_m(n)

    # x == 3
    if z == 0:                                       # RET cc
        no = label()
        return cond_jump_over(y, no) + ["call\tpop16", "call\tset_pc"] + branch_s(11) + ["%s:" % no] + next_s(5)
    if z == 1:
        if q == 0:                                   # POP rr
            if p == 3:
                return ["call\tpop16", "push\thl", "ex\taf, af'", "pop\taf"] + next_sz(n)
            return ["call\tpop16", "push\thl", "exx", "pop\t%s" % RP[p]] + next_ms(n)
        if p == 0:                                   # RET
            return ["call\tpop16", "call\tset_pc"] + branch_s(n)
        if p == 1:                                   # EXX
            return (["exx", "push\tbc", "push\tde", "push\thl", "ld\tbc, (Z80_BC2)", "ld\tde, (Z80_DE2)",
                     "ld\thl, (Z80_HL2)", "exx", "pop\thl"] + store_word("Z80_HL2") +
                    ["pop\thl"] + store_word("Z80_DE2") + ["pop\thl"] + store_word("Z80_BC2") + next_s(n))
        if p == 2:                                   # JP (HL)
            return MAIN_HL + ["call\tset_pc"] + branch_s(n)
        return MAIN_HL + ["ld\t(Z80_SP), hl"] + next_s(n)   # LD SP,HL
    if z == 2:                                       # JP cc,nn
        no = label()
        return (cond_jump_over(y, no) + ["ld\thl, (iy + 0)", "call\tset_pc"] + branch_s(n) +
                ["%s:" % no, "lea\tiy, iy + 2"] + next_s(n))
    if z == 3:
        if y == 0:                                   # JP nn
            return ["ld\thl, (iy + 0)", "call\tset_pc"] + branch_s(n)
        if y == 1:                                   # CB prefix
            return ["ld\tl, (iy + 0)", "inc\tiy", "ld\th, 4", "mlt\thl", "ld\tde, cb_table", "add\thl, de",
                    "ld\thl, (hl)", "jp\t(hl)"]
        if y == 2:                                   # OUT (n),A
            return ["ex\taf, af'", "ld\th, a", "ld\td, a", "ex\taf, af'", "ld\tl, (iy + 0)", "inc\tiy",
                    "ld\ta, d", "call\tio_out"] + next_s(n)
        if y == 3:                                   # IN A,(n)
            return (["ex\taf, af'", "ld\th, a", "ex\taf, af'", "ld\tl, (iy + 0)", "inc\tiy", "call\tio_in"] +
                    SET_A + next_m(n))
        if y == 4:                                   # EX (SP),HL
            return (["ld\thl, (Z80_SP)", "call\trd16", "push\thl"] + MAIN_HL +
                    ["ex\tde, hl", "ld\thl, (Z80_SP)", "call\twr16", "pop\thl", "push\thl", "exx", "pop\thl"] +
                    next_ms(n))
        if y == 5:                                   # EX DE,HL
            return ["exx", "ex\tde, hl"] + next_ms(n)
        if y == 6:                                   # DI
            return ["xor\ta, a", "ld\t(Z80_IFF1), a", "ld\t(Z80_IFF2), a"] + next_s(n)
        cont = label()                               # EI
        return (["ld\ta, 1", "ld\t(Z80_IFF1), a", "ld\t(Z80_IFF2), a", "ld\t(Z80_EI_PENDING), a",
                 "ld\ta, (Z80_IRQ)", "or\ta, a", "jr\tz, %s" % cont, "lea\tix, ix - 4", "jp\trun_exit",
                 "%s:" % cont, "xor\ta, a", "ld\t(Z80_EI_PENDING), a"] + next_s(n))
    if z == 4:                                       # CALL cc,nn
        no = label()
        return (cond_jump_over(y, no) + call_nn() + branch_s(17) + ["%s:" % no, "lea\tiy, iy + 2"] + next_s(10))
    if z == 5:
        if q == 0:                                   # PUSH rr
            if p == 3:
                return ["ex\taf, af'", "push\taf", "ex\taf, af'", "pop\thl", "call\tpush16"] + next_s(n)
            return main_rp(RP[p]) + ["call\tpush16"] + next_s(n)
        if p == 0:                                   # CALL nn
            return call_nn() + branch_s(n)
        prefix_table = {1: "dd_table", 2: "ed_table", 3: "fd_table"}[p]
        if p == 2:
            return ed_prefix()
        return ["ld\tl, (iy + 0)", "inc\tiy", "ld\th, 4", "mlt\thl", "ld\tde, %s" % prefix_table,
                "add\thl, de", "ld\thl, (hl)", "jp\t(hl)"]
    if z == 6:                                       # ALU A,n
        return TO_M + ["%s\ta, (iy + 0)" % ALU[y], "inc\tiy"] + next_m(n)
    return ["call\tget_pc", "call\tpush16", "ld\thl, %d" % (y * 8), "call\tset_pc"] + branch_s(n)   # RST


def ed_prefix():
    """ED doubles as the guard byte the machine puts after code regions.

    When ED sits at an 8 KB boundary, execution may have run off the end of
    a region, so re-resolve the address; if it lands elsewhere, run what is
    really there instead.
    """
    normal, again = label(), label()
    return [
        "lea\thl, iy - 1",
        "ld\tde, (pc_base)",
        "or\ta, a",
        "sbc\thl, de",
        "ld\ta, l",
        "or\ta, a",
        "jr\tnz, %s" % normal,
        "ld\ta, h",
        "and\ta, 0x1F",
        "jr\tnz, %s" % normal,
        "lea\tde, iy - 1",
        "ld\t(guard_host), de",
        "call\tset_pc",
        "lea\thl, iy + 0",
        "ld\tde, (guard_host)",
        "or\ta, a",
        "sbc\thl, de",
        "jr\tnz, %s" % again,
        "inc\tiy",
        "%s:" % normal,
        "ld\tl, (iy + 0)", "inc\tiy", "ld\th, 4", "mlt\thl", "ld\tde, ed_table", "add\thl, de",
        "ld\thl, (hl)", "jp\t(hl)",
        "%s:" % again,
    ] + DISPATCH


def call_nn():
    return ["lea\thl, iy + 2", "ld\tde, (pc_base)", "or\ta, a", "sbc\thl, de", "call\tpush16",
            "ld\thl, (iy + 0)", "call\tset_pc"]


# ---- CB prefix ----

def rot_on_e(y):
    if y == 6:                                       # SLL: shift left, bit 0 set
        return ["scf", "rl\te"]
    return ["%s\te" % ROT[y]]


def cb_handler(op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    r = R8[z]
    if r is None:
        return "mcb_hl"
    if x == 0:
        if y == 6:
            return TO_M + ["scf", "rl\t%s" % r] + next_m(8)
        return TO_M + ["%s\t%s" % (ROT[y], r)] + next_m(8)
    if x == 1:
        return TO_M + ["bit\t%d, %s" % (y, r)] + next_m(8)
    ins = "res" if x == 2 else "set"
    if r == "a":
        return TO_M + ["%s\t%d, a" % (ins, y)] + next_m(8)
    return ["exx", "%s\t%d, %s" % (ins, y, r)] + next_ms(8)


# ---- DD/FD prefix ----

def idx_handler(ix, op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    reg = "Z80_" + ix.upper()
    hi, lo = reg + " + 1", reg

    def half(r):
        return hi if r == 4 else lo

    if op in (0xDD, 0xFD):
        table = "dd_table" if op == 0xDD else "fd_table"
        return ["lea\tix, ix - 4", "ld\tl, (iy + 0)", "inc\tiy", "ld\th, 4", "mlt\thl", "ld\tde, %s" % table,
                "add\thl, de", "ld\thl, (hl)", "jp\t(hl)"]
    if op == 0xED:
        return ["lea\tix, ix - 4", "jp\top_ED"]

    if op in (0x09, 0x19, 0x29, 0x39):              # ADD IX,rr
        if p == 2:
            src = ["ld\tde, (%s)" % reg]
        elif p == 3:
            src = ["ld\tde, (Z80_SP)"]
        else:
            src = ["exx", "push\t%s" % RP[p], "exx", "pop\tde"]
        return (src + ["ld\thl, (%s)" % reg, "ex\taf, af'", "add.sis\thl, de", "ex\taf, af'"] +
                store_word(reg) + next_s(15))
    if op == 0x21:
        return ["ld\thl, (iy + 0)", "lea\tiy, iy + 2"] + store_word(reg) + next_s(14)
    if op == 0x22:
        return (["ld\tde, (%s)" % reg, "ld\thl, (iy + 0)", "lea\tiy, iy + 2", "call\twr16"] + next_s(20))
    if op == 0x2A:
        return ["ld\thl, (iy + 0)", "lea\tiy, iy + 2", "call\trd16"] + store_word(reg) + next_s(20)
    if op in (0x23, 0x2B):
        return ["ld\thl, (%s)" % reg, "inc\thl" if op == 0x23 else "dec\thl"] + store_word(reg) + next_s(10)
    if op in (0x24, 0x25, 0x2C, 0x2D):              # INC/DEC IXH/IXL
        f = half(y)
        return (["ld\ta, (%s)" % f, "ld\te, a", "ex\taf, af'", "%s\te" % ("inc" if z == 4 else "dec"),
                 "ex\taf, af'", "ld\ta, e", "ld\t(%s), a" % f] + next_s(8))
    if op in (0x26, 0x2E):
        return ["ld\ta, (iy + 0)", "inc\tiy", "ld\t(%s), a" % half(y)] + next_s(11)
    if op in (0x34, 0x35):
        return (idx_addr(ix) + ["push\thl", "call\trd8", "ld\te, a", "ex\taf, af'",
                                "%s\te" % ("inc" if op == 0x34 else "dec"), "ex\taf, af'", "ld\ta, e",
                                "pop\thl", "call\twr8"] + next_s(23))
    if op == 0x36:
        return idx_addr(ix) + ["ld\ta, (iy + 0)", "inc\tiy", "call\twr8"] + next_s(19)

    if x == 1 and op != 0x76:
        if z == 6:                                   # LD r,(IX+d): real H/L
            return idx_addr(ix) + ["call\trd8"] + put_r_then_next(R8[y], 19)
        if y == 6:                                   # LD (IX+d),r
            return idx_addr(ix) + ["push\thl"] + get_r(R8[z]) + ["pop\thl", "call\twr8"] + next_s(19)
        if y in (4, 5) or z in (4, 5):               # IXH/IXL forms
            if z in (4, 5):
                load = ["ld\ta, (%s)" % half(z)]
            else:
                load = get_r(R8[z])
            if y in (4, 5):
                return load + ["ld\t(%s), a" % half(y)] + next_s(8)
            return load + put_r_then_next(R8[y], 8)
    if x == 2:
        if z == 6:
            return (idx_addr(ix) + ["call\trd8", "ld\te, a", "ex\taf, af'", "%s\ta, e" % ALU[y]] +
                    next_sz(19))
        if z in (4, 5):
            return ["ld\ta, (%s)" % half(z), "ld\te, a", "ex\taf, af'", "%s\ta, e" % ALU[y]] + next_sz(8)
    if op == 0xE1:
        return ["call\tpop16"] + store_word(reg) + next_s(14)
    if op == 0xE5:
        return ["ld\thl, (%s)" % reg, "call\tpush16"] + next_s(15)
    if op == 0xE3:
        return (["ld\thl, (Z80_SP)", "call\trd16", "push\thl", "ld\tde, (%s)" % reg, "ld\thl, (Z80_SP)",
                 "call\twr16", "pop\thl"] + store_word(reg) + next_s(23))
    if op == 0xE9:
        return ["ld\thl, (%s)" % reg, "call\tset_pc"] + branch_s(8)
    if op == 0xF9:
        return ["ld\thl, (%s)" % reg, "ld\t(Z80_SP), hl"] + next_s(10)

    return "idx_ignore"                               # the prefix does nothing


def idx_cb(ix):
    return idx_addr(ix) + ["ld\ta, (iy + 0)", "inc\tiy", "jp\tmcb_index"]


# ---- ED prefix ----

def flags_from_value():
    """Z80 F = S,Z,P of scratch A, H=N=0, carry kept (IN r,(C))."""
    return ["or\ta, a", "push\taf", "pop\thl", "ld\th, 0x01", "call\tsetf_mask"]


def ed_handler(op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1

    if x == 2 and z <= 3 and y >= 4:
        return block_op(op)
    if x != 1:
        return "ed_nop"

    if z == 0:                                       # IN r,(C)
        code = ["exx", "push\tbc", "exx", "pop\thl", "call\tio_in", "ld\t(tmp8 + 1), a"] + flags_from_value()
        if y == 6:
            return code + next_s(12)
        return code + ["ld\ta, (tmp8 + 1)"] + put_r_then_next(R8[y], 12)
    if z == 1:                                       # OUT (C),r
        value = ["xor\ta, a"] if y == 6 else get_r(R8[y])
        return value + ["ld\t(tmp8 + 1), a", "exx", "push\tbc", "exx", "pop\thl", "ld\ta, (tmp8 + 1)",
                        "call\tio_out"] + next_s(12)
    if z == 2:                                       # SBC/ADC HL,rr
        ins = "adc.sis" if q else "sbc.sis"
        if p == 3:
            return TO_M + ["push\tde", "ld\tde, (Z80_SP)", "%s\thl, de" % ins, "pop\tde"] + next_m(15)
        return TO_M + ["%s\thl, %s" % (ins, RP[p])] + next_m(15)
    if z == 3:                                       # LD (nn),rr / LD rr,(nn)
        if q == 0:
            if p == 3:
                src = ["ld\tde, (Z80_SP)"]
            else:
                src = ["exx", "push\t%s" % RP[p], "exx", "pop\tde"]
            return src + ["ld\thl, (iy + 0)", "lea\tiy, iy + 2", "call\twr16"] + next_s(20)
        code = ["ld\thl, (iy + 0)", "lea\tiy, iy + 2", "call\trd16"]
        if p == 3:
            return code + ["ld\t(Z80_SP), hl"] + next_s(20)
        return code + ["push\thl", "exx", "pop\t%s" % RP[p]] + next_ms(20)
    if z == 4:                                       # NEG
        return TO_M + ["neg"] + next_m(8)
    if z == 5:                                       # RETN / RETI
        return ["ld\ta, (Z80_IFF2)", "ld\t(Z80_IFF1), a", "call\tpop16", "call\tset_pc"] + branch_s(14)
    if z == 6:                                       # IM
        mode = {0: 0, 1: 0, 2: 1, 3: 2}[y & 3]
        return ["ld\ta, %d" % mode, "ld\t(Z80_IM), a"] + next_s(8)
    # z == 7
    if y == 0:                                       # LD I,A
        return GET_A + ["ld\t(Z80_I), a"] + next_s(9)
    if y == 1:                                       # LD R,A
        return GET_A + ["ld\t(Z80_R), a"] + next_s(9)
    if y in (2, 3):                                  # LD A,I / LD A,R
        value = ["ld\ta, (Z80_I)"] if y == 2 else ["ld\ta, r"]   # R: the eZ80's own counter
        return value + ["call\tld_a_ir"] + next_sz(9)
    if y in (4, 5):                                  # RRD / RLD
        return (MAIN_HL + ["push\thl", "call\trd8", "ld\t(tmp8), a", "ld\thl, tmp8", "ex\taf, af'",
                           "rrd" if y == 4 else "rld", "ex\taf, af'", "pop\thl", "ld\ta, (tmp8)",
                           "call\twr8"] + next_s(18))
    return "ed_nop"


def block_op(op):
    kind = op & 3
    dec = bool(op & 0x08)
    repeat = bool(op & 0x10)
    step = "dec" if dec else "inc"
    again = label()

    if kind == 0:                                    # LDI LDD LDIR LDDR
        code = (MAIN_HL + ["call\trd8", "ld\t(tmp8), a"] + main_rp("de") + ["ld\ta, (tmp8)", "call\twr8",
                "exx", "%s\thl" % step, "%s\tde" % step, "dec\tbc", "ld\ta, b", "or\ta, c", "exx",
                "ld\tl, 0", "jr\tz, %s_f" % again, "ld\tl, 0x04", "%s_f:" % again,
                "ld\th, 0xC1", "call\tsetf_mask"])
        if not repeat:
            return code + next_s(16)
        return code + ["exx", "ld\ta, b", "or\ta, c", "exx", "jp\tz, %s" % again,
                       "lea\tiy, iy - 2"] + branch_s(21) + ["%s:" % again] + next_s(16)

    if kind == 1:                                    # CPI CPD CPIR CPDR
        code = (MAIN_HL + ["call\trd8", "ld\te, a", "exx", "%s\thl" % step, "dec\tbc", "ld\ta, b",
                           "or\ta, c", "exx", "ld\ta, 0", "jr\tz, %s_f" % again, "ld\ta, 0x04",
                           "%s_f:" % again, "ld\t(tmp8 + 1), a",
                           # S, Z, H, N from the compare; carry from before it.
                           "ex\taf, af'", "push\taf", "cp\ta, e", "push\taf", "ex\taf, af'", "pop\thl",
                           "pop\tde", "ld\ta, l", "and\ta, 0xD2", "ld\tl, a", "ld\ta, e", "and\ta, 0x01",
                           "or\ta, l", "ld\tl, a", "ld\ta, (tmp8 + 1)", "or\ta, l", "ld\tl, a",
                           "ld\th, 0", "call\tsetf_mask"])
        if not repeat:
            return code + next_s(16)
        # Repeat while BC != 0 and A != (HL): P/V set and Z clear.
        return code + ["ex\taf, af'", "push\taf", "ex\taf, af'", "pop\thl", "ld\ta, l", "and\ta, 0x44",
                       "cp\ta, 0x04", "jp\tnz, %s" % again, "lea\tiy, iy - 2"] + branch_s(21) + \
            ["%s:" % again] + next_s(16)

    if kind == 2:                                    # INI IND INIR INDR
        code = (main_rp("bc") + ["call\tio_in", "ld\t(tmp8), a"] + MAIN_HL + ["ld\ta, (tmp8)", "call\twr8",
                "exx", "%s\thl" % step, "dec\tb", "ld\ta, b", "exx"])
    else:                                            # OUTI OUTD OTIR OTDR
        code = (MAIN_HL + ["call\trd8", "ld\t(tmp8), a", "exx", "dec\tb", "%s\thl" % step, "exx"] +
                main_rp("bc") + ["ld\ta, (tmp8)", "call\tio_out", "exx", "ld\ta, b", "exx"])
    # F = N | S,Z of B, carry kept.
    code += ["or\ta, a", "push\taf", "pop\thl", "ld\ta, l", "and\ta, 0xC0", "or\ta, 0x02", "ld\tl, a",
             "ld\th, 0x01", "call\tsetf_mask"]
    if not repeat:
        return code + next_s(16)
    return code + ["exx", "ld\ta, b", "or\ta, a", "exx", "jp\tz, %s" % again, "lea\tiy, iy - 2"] + \
        branch_s(21) + ["%s:" % again] + next_s(16)


# ---- runtime: entry, exit, memory, I/O ----

RUNTIME = r"""
	.assume	adl = 1

	.equ	Z80_F, _z80 + 0
	.equ	Z80_A, _z80 + 1
	.equ	Z80_BC, _z80 + 2
	.equ	Z80_DE, _z80 + 4
	.equ	Z80_HL, _z80 + 6
	.equ	Z80_IX, _z80 + 8
	.equ	Z80_IY, _z80 + 10
	.equ	Z80_SP, _z80 + 12
	.equ	Z80_PC, _z80 + 14
	.equ	Z80_AF2, _z80 + 16
	.equ	Z80_BC2, _z80 + 18
	.equ	Z80_DE2, _z80 + 20
	.equ	Z80_HL2, _z80 + 22
	.equ	Z80_I, _z80 + 24
	.equ	Z80_R, _z80 + 25
	.equ	Z80_IFF1, _z80 + 26
	.equ	Z80_IFF2, _z80 + 27
	.equ	Z80_IM, _z80 + 28
	.equ	Z80_HALTED, _z80 + 29
	.equ	Z80_EI_PENDING, _z80 + 30
	.equ	Z80_IRQ, _z80 + 31

	.section .bss
start_cycles:	.ds	3
pc_base:	.ds	3
tmp8:	.ds	3
wr_addr:	.ds	3
wr_value:	.ds	3
io_port:	.ds	3
io_value:	.ds	3
code_pc:	.ds	3
guard_host:	.ds	3

	.section .text

; int z80_run_asm(int cycles): runs until the cycles are used up (checked
; after jumps), HALT, or EI with an interrupt pending. Returns cycles used.
	.global	_z80_run_asm
_z80_run_asm:
	push	ix
	push	iy
	ld	iy, 0
	add	iy, sp
	ld	hl, (iy + 9)
	ld	(start_cycles), hl
	push	hl
	pop	ix
	ld	hl, (Z80_F)
	push	hl
	pop	af
	ex	af, af'
	ld	bc, (Z80_BC)
	ld	de, (Z80_DE)
	ld	hl, (Z80_HL)
	exx
	ld	bc, main_table
	ld	hl, (Z80_PC)
	call	set_pc
	; An interrupt the caller held back for EI's delay is taken after one
	; more stretch of code.
	ld	a, (Z80_IRQ)
	or	a, a
	jr	z, 1f
	ld	a, (Z80_IFF1)
	or	a, a
	call	nz, force_exit
1:
""" + "\t" + "\n\t".join(DISPATCH) + r"""

; Leaves the run (S context) and stores the Z80 state back in z80.
run_exit:
	call	get_pc
	ld	a, l
	ld	(Z80_PC), a
	ld	a, h
	ld	(Z80_PC + 1), a
	exx
	ld	a, c
	ld	(Z80_BC), a
	ld	a, b
	ld	(Z80_BC + 1), a
	ld	a, e
	ld	(Z80_DE), a
	ld	a, d
	ld	(Z80_DE + 1), a
	ld	a, l
	ld	(Z80_HL), a
	ld	a, h
	ld	(Z80_HL + 1), a
	ex	af, af'
	push	af
	pop	hl
	ld	a, l
	ld	(Z80_F), a
	ld	a, h
	ld	(Z80_A), a
	lea	de, ix + 0
	ld	hl, (start_cycles)
	or	a, a
	sbc	hl, de
	pop	iy
	pop	ix
	ret

; Makes the run end at the next jump, keeping the cycle count right.
force_exit:
	lea	de, ix + 0
	ld	hl, (start_cycles)
	or	a, a
	sbc	hl, de
	dec	hl
	ld	(start_cycles), hl
	ld	ix, -1
	ret

; HL = the Z80's PC (low 16 bits). Clobbers DE, F.
get_pc:
	lea	hl, iy + 0
	ld	de, (pc_base)
	or	a, a
	sbc	hl, de
	ret

; Jumps to the Z80 address in HL (low 16 bits): sets IY and pc_base.
; Clobbers A, DE, HL, F.
set_pc:
	ld	de, 0
	ld	e, l
	ld	d, h
	ld	(code_pc), de
	ld	a, h
	rlca
	rlca
	rlca
	and	a, 7
	add	a, a
	add	a, a
	ld	hl, _z80_cmap
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	hl, (hl)
	ld	de, 0
	or	a, a
	sbc	hl, de
	jr	nz, 1f
	call	save_all
	ld	hl, (code_pc)
	push	hl
	call	_z80_code_base
	pop	de
	ld	(wr_addr), hl
	call	restore_all
	ld	hl, (wr_addr)
1:	ld	(pc_base), hl
	ld	de, (code_pc)
	add	hl, de
	push	hl
	pop	iy
	ret

; save_all/restore_all wrap calls into C from S context: they keep every
; Z80 register, IX, IY and BC' (the dispatch table).
save_all:
	ld	(save_ret), hl
	pop	hl
	ld	(save_ret + 3), hl
	push	ix
	push	iy
	push	bc
	exx
	push	bc
	push	de
	push	hl
	ex	af, af'
	push	af
	ld	hl, (save_ret + 3)
	push	hl
	ld	hl, (save_ret)
	ret

restore_all:
	pop	hl
	ld	(save_ret + 3), hl
	pop	af
	ex	af, af'
	pop	hl
	pop	de
	pop	bc
	exx
	pop	bc
	pop	iy
	pop	ix
	ld	hl, (save_ret + 3)
	push	hl
	ret

	.section .bss
save_ret:	.ds	6
	.section .text

; A = byte at the Z80 address in HL. Clobbers DE, HL, F.
rd8:
	ld	a, l
	ld	l, h
	ld	h, 4
	mlt	hl
	ld	de, _z80_rmap
	add	hl, de
	ld	hl, (hl)
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	a, (hl)
	ret

; HL = word at the Z80 address in HL. Clobbers A, DE, F.
rd16:
	push	hl
	call	rd8
	ld	(tmp8), a
	pop	hl
	inc	hl
	call	rd8
	ld	(tmp8 + 1), a
	ld	hl, (tmp8)
	ret

; Writes A to the Z80 address in HL. Clobbers A, DE, HL, F.
wr8:
	ld	(wr_value), a
	ld	(wr_addr), hl
	ld	l, h
	ld	h, 4
	mlt	hl
	ld	de, _z80_wmap
	add	hl, de
	ld	hl, (hl)
	ld	de, 0
	or	a, a
	sbc	hl, de
	jr	z, 1f
	ld	a, (wr_addr)
	ld	e, a
	add	hl, de
	ld	a, (wr_value)
	ld	(hl), a
	ret
1:	call	save_all
	ld	hl, (wr_value)
	push	hl
	ld	hl, (wr_addr)
	push	hl
	call	_z80_mem_write
	pop	hl
	pop	hl
	call	restore_all
	; A mapper write may have swapped the bank this code runs from.
	call	get_pc
	jp	set_pc

; Writes DE (low 16 bits) to the Z80 address in HL. Clobbers A, DE, HL, F.
wr16:
	push	hl
	push	de
	ld	a, e
	call	wr8
	pop	de
	pop	hl
	inc	hl
	ld	a, d
	jp	wr8

; Pushes HL (low 16 bits) onto the Z80 stack. Clobbers A, DE, HL, F.
push16:
	ex	de, hl
	ld	hl, (Z80_SP)
	dec	hl
	dec	hl
	ld	(Z80_SP), hl
	jp	wr16

; HL = word popped from the Z80 stack. Clobbers A, DE, F.
pop16:
	ld	hl, (Z80_SP)
	call	rd16
	push	hl
	ld	hl, (Z80_SP)
	inc	hl
	inc	hl
	ld	(Z80_SP), hl
	pop	hl
	ret

; A = z80_io_read(HL). Ends the run soon if that raised an interrupt.
io_in:
	ld	(io_port), hl
	call	save_all
	ld	hl, (io_port)
	push	hl
	call	_z80_io_read
	pop	hl
	ld	(io_value), a
	call	restore_all
	call	irq_check
	ld	a, (io_value)
	ret

; z80_io_write(HL, A).
io_out:
	ld	(io_port), hl
	ld	(io_value), a
	call	save_all
	ld	hl, (io_value)
	push	hl
	ld	hl, (io_port)
	push	hl
	call	_z80_io_write
	pop	hl
	pop	hl
	call	restore_all
	jp	irq_check

irq_check:
	ld	a, (Z80_IRQ)
	or	a, a
	ret	z
	ld	a, (Z80_IFF1)
	or	a, a
	ret	z
	jp	force_exit

; Z80 F = (F & H) | L, keeping the Z80's A. Clobbers A, DE, F (scratch).
setf_mask:
	ex	af, af'
	push	af
	pop	de
	ex	af, af'
	ld	a, e
	and	a, h
	or	a, l
	ld	e, a
	push	de
	ex	af, af'
	pop	af
	ex	af, af'
	ret

; ED opcodes that do nothing.
ed_nop:
	lea	ix, ix - 8
	jp	dispatch

; DD/FD before an instruction that doesn't use HL: run it unprefixed.
idx_ignore:
	lea	ix, ix - 4
	ld	l, (iy - 1)
	ld	h, 4
	mlt	hl
	add	hl, bc
	ld	hl, (hl)
	jp	(hl)

; CB op on (HL). The opcode is the byte just fetched.
mcb_hl:
	ld	a, (iy - 1)
	ld	(cb_op), a
	exx
	push	hl
	exx
	pop	hl
	xor	a, a
	ld	(cb_index), a
	jr	mcb_common

; DD CB d op / FD CB d op: HL = IX/IY + d, A = op. The result is also copied
; to the register in bits 0-2 (an undocumented Z80 behavior), unless (HL).
mcb_index:
	ld	(cb_op), a
	ld	a, 1
	ld	(cb_index), a

mcb_common:
	push	hl
	call	rd8
	ld	e, a
	ld	a, (cb_op)
	rrca
	rrca
	rrca
	and	a, 0x1F
	add	a, a
	add	a, a
	ld	hl, cb_ops
	push	de
	ld	de, 0
	ld	e, a
	add	hl, de
	pop	de
	ld	hl, (hl)
	ex	af, af'
	call	call_hl
	ex	af, af'
	pop	hl
	ld	a, (cb_op)
	and	a, 0xC0
	cp	a, 0x40
	jr	nz, 1f
	; BIT: no write. 12 cycles, or 20 with an index prefix.
	ld	a, (cb_index)
	or	a, a
	jr	z, 2f
	lea	ix, ix - 8
2:	lea	ix, ix - 12
	jp	dispatch
1:	ld	a, e
	ld	(tmp8 + 1), a
	call	wr8
	ld	a, (cb_index)
	or	a, a
	jr	nz, 3f
	lea	ix, ix - 15
	jp	dispatch
3:	lea	ix, ix - 23
	ld	a, (cb_op)
	and	a, 7
	cp	a, 6
	jp	z, dispatch
	add	a, a
	add	a, a
	ld	hl, put_reg
	ld	de, 0
	ld	e, a
	add	hl, de
	ld	hl, (hl)
	ld	a, (tmp8 + 1)
	call	call_hl
	jp	dispatch

call_hl:
	jp	(hl)

; Scratch A -> Z80 register, staying in S.
put_b:	exx
	ld	b, a
	exx
	ret
put_c:	exx
	ld	c, a
	exx
	ret
put_d:	exx
	ld	d, a
	exx
	ret
put_e:	exx
	ld	e, a
	exx
	ret
put_h:	exx
	ld	h, a
	exx
	ret
put_l:	exx
	ld	l, a
	exx
	ret
put_a:	ld	(tmp8), a
	ex	af, af'
	ld	a, (tmp8)
	ex	af, af'
	ret

; The 32 CB operations on E, with the Z80's AF active.
CB_OPS_PLACEHOLDER

	.section .bss
cb_op:	.ds	1
cb_index:	.ds	1
	.section .text

; LD A,I / LD A,R with the value in scratch A: S and Z from the value,
; P/V = IFF2, H = N = 0, carry kept. Returns with the Z80's AF active.
ld_a_ir:
	ld	(tmp8 + 2), a
	or	a, a
	push	af
	pop	hl
	ld	a, l
	and	a, 0xC0
	ld	l, a
	ld	a, (Z80_IFF2)
	or	a, a
	jr	z, 1f
	set	2, l
1:	ld	h, 0x01
	call	setf_mask
	ld	a, (tmp8 + 2)
	ld	(tmp8), a
	ex	af, af'
	ld	a, (tmp8)
	ret
"""


def emit_handler(out, name, lines):
    """Emits a handler; returns the label its table entry should use."""
    if isinstance(lines, str):
        return lines
    out.append("%s:" % name)
    for line in lines:
        if line.endswith(":"):
            out.append(line)
        else:
            out.append("\t" + line)
    return name


def emit_table(out, name, targets):
    out.append("\t.balign\t4")
    out.append("%s:" % name)
    for target in targets:
        out.append("\t.d24\t%s" % target)
        out.append("\t.db\t0")


def main():
    global INLINE
    out = ["; Generated by tools/gen_z80core.py. Do not edit.", RUNTIME, "dispatch:"] + \
        ["\t" + line for line in DISPATCH]

    tables = {}
    tables["main_table"] = [emit_handler(out, "op_%02X" % op, main_handler(op)) for op in range(256)]
    tables["cb_table"] = [emit_handler(out, "cb_%02X" % op, cb_handler(op)) for op in range(256)]
    INLINE = False
    tables["ed_table"] = [emit_handler(out, "ed_%02X" % op, ed_handler(op)) for op in range(256)]
    for ix, prefix in (("ix", "dd"), ("iy", "fd")):
        tables["%s_table" % prefix] = [
            emit_handler(out, "%s_%02X" % (prefix, op), idx_cb(ix) if op == 0xCB else idx_handler(ix, op))
            for op in range(256)]

    out.append("\t.section .rodata")
    for name, targets in tables.items():
        emit_table(out, name, targets)

    out.append("\t.section .text")
    ops = []
    for x in range(4):
        for y in range(8):
            name = "cbop_%d%d" % (x, y)
            if x == 0:
                body = ["scf", "rl\te"] if y == 6 else ["%s\te" % ROT[y]]
            else:
                body = ["%s\t%d, e" % (["bit", "res", "set"][x - 1], y)]
            ops.append(name)
            out.append("%s:" % name)
            out += ["\t" + b for b in body] + ["\tret"]
    out.append("\t.section .rodata")
    out.append("\t.balign\t4")
    out.append("cb_ops:")
    for name in ops:
        out += ["\t.d24\t%s" % name, "\t.db\t0"]
    out.append("put_reg:")
    for r in ["b", "c", "d", "e", "h", "l", "b", "a"]:
        out += ["\t.d24\tput_%s" % r, "\t.db\t0"]

    text = "\n".join(out).replace("CB_OPS_PLACEHOLDER", "")
    with open(OUT, "w") as f:
        f.write(text + "\n")
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
