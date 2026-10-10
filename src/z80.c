#include "z80.h"

z80_t z80;
const uint8_t *z80_rmap[64];

#define FC 0x01
#define FN 0x02
#define FP 0x04
#define FV FP
#define FX 0x08
#define FH 0x10
#define FY 0x20
#define FZ 0x40
#define FS 0x80

#define A  z80.af.b.h
#define F  z80.af.b.l
#define B  z80.bc.b.h
#define C  z80.bc.b.l
#define D  z80.de.b.h
#define E  z80.de.b.l
#define BC z80.bc.w
#define DE z80.de.w
#define HL z80.hl.w
#define SP z80.sp.w
#define PC z80.pc.w

/* Base cycle counts for unprefixed opcodes; taken branches add more below. */
static const uint8_t cycles_main[256] = {
     4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
     8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
     7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
     7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     5,10,10,10,10,11, 7,11, 5,10,10, 0,10,17, 7,11,
     5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 0, 7,11,
     5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 0, 7,11,
     5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 0, 7,11,
};

/* S, Z, X, Y flags of a byte, and the same plus parity. */
static uint8_t sz[256];
static uint8_t szp[256];

static int cycles_extra;            /* added by taken branches and (IX+d) */

static inline uint8_t rd(uint16_t addr)
{
    return z80_rmap[addr >> 10][addr & 0x3FF];
}

static inline uint16_t rd16(uint16_t addr)
{
    return rd(addr) | rd((uint16_t)(addr + 1)) << 8;
}

static inline void wr16(uint16_t addr, uint16_t value)
{
    z80_mem_write(addr, (uint8_t)value);
    z80_mem_write((uint16_t)(addr + 1), value >> 8);
}

static inline uint8_t fetch(void)
{
    return rd(PC++);
}

static inline uint16_t fetch16(void)
{
    uint16_t v = rd16(PC);
    PC += 2;
    return v;
}

static inline void push(uint16_t v)
{
    SP -= 2;
    wr16(SP, v);
}

static inline uint16_t pop(void)
{
    uint16_t v = rd16(SP);
    SP += 2;
    return v;
}

static inline void bump_r(void)
{
    z80.r = (z80.r & 0x80) | ((z80.r + 1) & 0x7F);
}

/* ---- arithmetic ---- */

static void alu(uint8_t which, uint8_t v)
{
    unsigned a = A, r;

    switch (which)
    {
    case 0: /* ADD */
    case 1: /* ADC */
        r = a + v + (which == 1 ? (F & FC) : 0);
        F = sz[r & 0xFF] | ((a ^ v ^ r) & FH) | (r >> 8 & FC) |
            ((~(a ^ v) & (a ^ r) & 0x80) >> 5);
        A = (uint8_t)r;
        break;
    case 2: /* SUB */
    case 3: /* SBC */
    case 7: /* CP */
        r = a - v - (which == 3 ? (F & FC) : 0);
        F = (sz[r & 0xFF] & ~(FX | FY)) | FN | ((a ^ v ^ r) & FH) | (r >> 8 & FC) |
            (((a ^ v) & (a ^ r) & 0x80) >> 5);
        if (which == 7)
            F |= v & (FX | FY);
        else
        {
            F |= r & (FX | FY);
            A = (uint8_t)r;
        }
        break;
    case 4: /* AND */
        A &= v;
        F = szp[A] | FH;
        break;
    case 5: /* XOR */
        A ^= v;
        F = szp[A];
        break;
    case 6: /* OR */
        A |= v;
        F = szp[A];
        break;
    }
}

static uint8_t inc8(uint8_t v)
{
    uint8_t r = v + 1;
    F = (F & FC) | sz[r] | ((r & 0x0F) ? 0 : FH) | (r == 0x80 ? FV : 0);
    return r;
}

static uint8_t dec8(uint8_t v)
{
    uint8_t r = v - 1;
    F = (F & FC) | FN | sz[r] | ((v & 0x0F) ? 0 : FH) | (v == 0x80 ? FV : 0);
    return r;
}

static uint16_t add16(uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b;
    F = (F & (FS | FZ | FV)) | (((a ^ b ^ r) >> 8) & FH) | (uint8_t)(r >> 16 & FC) |
        ((r >> 8) & (FX | FY));
    return (uint16_t)r;
}

static uint16_t adc16(uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b + (F & FC);
    F = ((r >> 8) & (FS | FX | FY)) | ((r & 0xFFFF) ? 0 : FZ) | (((a ^ b ^ r) >> 8) & FH) |
        ((~(a ^ b) & (a ^ r) & 0x8000) >> 13) | (uint8_t)(r >> 16 & FC);
    return (uint16_t)r;
}

static uint16_t sbc16(uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a - b - (F & FC);
    F = FN | ((r >> 8) & (FS | FX | FY)) | ((r & 0xFFFF) ? 0 : FZ) | (((a ^ b ^ r) >> 8) & FH) |
        (((a ^ b) & (a ^ r) & 0x8000) >> 13) | (uint8_t)(r >> 16 & FC);
    return (uint16_t)r;
}

static void daa(void)
{
    uint8_t a = A, lo = a & 0x0F, diff = 0, carry = F & FC, half;

    if ((F & FH) || lo > 9)
        diff = 0x06;
    if (carry || a > 0x99)
    {
        diff |= 0x60;
        carry = FC;
    }
    if (F & FN)
    {
        half = (F & FH) && lo < 6 ? FH : 0;
        A = a - diff;
    }
    else
    {
        half = lo > 9 ? FH : 0;
        A = a + diff;
    }
    F = szp[A] | carry | (F & FN) | half;
}

/* CB-prefix rotates and shifts: RLC RRC RL RR SLA SRA SLL SRL. */
static uint8_t rot(uint8_t which, uint8_t v)
{
    uint8_t r, c;

    switch (which)
    {
    case 0: c = v >> 7; r = (v << 1) | c; break;
    case 1: c = v & 1; r = (v >> 1) | (c << 7); break;
    case 2: c = v >> 7; r = (v << 1) | (F & FC); break;
    case 3: c = v & 1; r = (v >> 1) | ((F & FC) << 7); break;
    case 4: c = v >> 7; r = v << 1; break;
    case 5: c = v & 1; r = (v >> 1) | (v & 0x80); break;
    case 6: c = v >> 7; r = (v << 1) | 1; break;
    default: c = v & 1; r = v >> 1; break;
    }
    F = szp[r] | c;
    return r;
}

static void bit(uint8_t n, uint8_t v)
{
    uint8_t m = v & (1 << n);
    F = (F & FC) | FH | (v & (FX | FY)) | (m ? (m & FS) : (FZ | FP));
}

/* ---- register access for opcodes that name a register in 3 bits ---- */

static uint8_t get_reg(uint8_t r, z80_pair_t *ip)
{
    switch (r)
    {
    case 0: return B;
    case 1: return C;
    case 2: return D;
    case 3: return E;
    case 4: return ip->b.h;
    case 5: return ip->b.l;
    default: return A;
    }
}

static void set_reg(uint8_t r, uint8_t v, z80_pair_t *ip)
{
    switch (r)
    {
    case 0: B = v; break;
    case 1: C = v; break;
    case 2: D = v; break;
    case 3: E = v; break;
    case 4: ip->b.h = v; break;
    case 5: ip->b.l = v; break;
    default: A = v; break;
    }
}

static uint16_t *rp(uint8_t n, z80_pair_t *ip)
{
    switch (n)
    {
    case 0: return &BC;
    case 1: return &DE;
    case 2: return &ip->w;
    default: return &SP;
    }
}

/* Address of the (HL) operand: (IX+d)/(IY+d) when prefixed. */
static uint16_t mem_operand(z80_pair_t *ip)
{
    if (ip == &z80.hl)
        return HL;
    cycles_extra += 8;
    return (uint16_t)(ip->w + (int8_t)fetch());
}

static bool condition(uint8_t cc)
{
    switch (cc)
    {
    case 0: return !(F & FZ);
    case 1: return F & FZ;
    case 2: return !(F & FC);
    case 3: return F & FC;
    case 4: return !(F & FP);
    case 5: return F & FP;
    case 6: return !(F & FS);
    default: return F & FS;
    }
}

/* ---- prefixed groups ---- */

static int exec_cb(void)
{
    uint8_t op = fetch(), r = op & 7, y = (op >> 3) & 7, v;

    bump_r();
    v = r == 6 ? rd(HL) : get_reg(r, &z80.hl);
    switch (op >> 6)
    {
    case 0: v = rot(y, v); break;
    case 1: bit(y, v); return r == 6 ? 12 : 8;
    case 2: v &= ~(1 << y); break;
    default: v |= 1 << y; break;
    }
    if (r == 6)
    {
        z80_mem_write(HL, v);
        return 15;
    }
    set_reg(r, v, &z80.hl);
    return 8;
}

/* DD CB d op / FD CB d op. Returns cycles not counting the DD/FD prefix. */
static int exec_index_cb(z80_pair_t *ip)
{
    uint16_t addr = (uint16_t)(ip->w + (int8_t)fetch());
    uint8_t op = fetch(), r = op & 7, y = (op >> 3) & 7, v = rd(addr);

    switch (op >> 6)
    {
    case 0: v = rot(y, v); break;
    case 1: bit(y, v); return 16;
    case 2: v &= ~(1 << y); break;
    default: v |= 1 << y; break;
    }
    z80_mem_write(addr, v);
    if (r != 6)
        set_reg(r, v, &z80.hl);     /* undocumented copy to a register */
    return 19;
}

static int block_op(uint8_t op)
{
    uint8_t v, n;
    bool dec = op & 0x08, repeat = op & 0x10;

    switch (op & 3)
    {
    case 0: /* LDI LDD LDIR LDDR */
        v = rd(HL);
        z80_mem_write(DE, v);
        HL += dec ? -1 : 1;
        DE += dec ? -1 : 1;
        BC--;
        n = v + A;
        F = (F & (FS | FZ | FC)) | (BC ? FV : 0) | (n & FX) | ((n << 4) & FY);
        if (repeat && BC)
        {
            PC -= 2;
            return 21;
        }
        return 16;
    case 1: /* CPI CPD CPIR CPDR */
    {
        uint8_t r;
        v = rd(HL);
        r = A - v;
        HL += dec ? -1 : 1;
        BC--;
        F = (F & FC) | FN | (sz[r] & ~(FX | FY)) | ((A ^ v ^ r) & FH) | (BC ? FV : 0);
        n = r - ((F & FH) ? 1 : 0);
        F |= (n & FX) | ((n << 4) & FY);
        if (repeat && BC && r)
        {
            PC -= 2;
            return 21;
        }
        return 16;
    }
    case 2: /* INI IND INIR INDR */
        v = z80_io_read(BC);
        z80_mem_write(HL, v);
        HL += dec ? -1 : 1;
        B--;
        break;
    default: /* OUTI OUTD OTIR OTDR */
        v = rd(HL);
        B--;
        z80_io_write(BC, v);
        HL += dec ? -1 : 1;
        break;
    }
    F = (F & FC) | FN | sz[B];
    if (repeat && B)
    {
        PC -= 2;
        return 21;
    }
    return 16;
}

static int exec_ed(void)
{
    uint8_t op = fetch(), y = (op >> 3) & 7, v;

    bump_r();
    if (op >= 0xA0 && op <= 0xBB && (op & 7) <= 3)
        return block_op(op);
    if (op < 0x40 || op > 0x7F)
        return 8;

    switch (op & 7)
    {
    case 0: /* IN r,(C) */
        v = z80_io_read(BC);
        F = (F & FC) | szp[v];
        if (y != 6)
            set_reg(y, v, &z80.hl);
        return 12;
    case 1: /* OUT (C),r */
        z80_io_write(BC, y == 6 ? 0 : get_reg(y, &z80.hl));
        return 12;
    case 2: /* SBC/ADC HL,rr */
        if (op & 8)
            HL = adc16(HL, *rp(y >> 1, &z80.hl));
        else
            HL = sbc16(HL, *rp(y >> 1, &z80.hl));
        return 15;
    case 3: /* LD (nn),rr / LD rr,(nn) */
    {
        uint16_t addr = fetch16();
        if (op & 8)
            *rp(y >> 1, &z80.hl) = rd16(addr);
        else
            wr16(addr, *rp(y >> 1, &z80.hl));
        return 20;
    }
    case 4: /* NEG */
        v = A;
        A = 0;
        alu(2, v);
        return 8;
    case 5: /* RETN / RETI */
        PC = pop();
        z80.iff1 = z80.iff2;
        return 14;
    case 6: /* IM */
        z80.im = (y & 3) == 2 ? 1 : (y & 3) == 3 ? 2 : 0;
        return 8;
    default:
        switch (y)
        {
        case 0: z80.i = A; return 9;
        case 1: z80.r = A; return 9;
        case 2:
        case 3:
            A = y == 2 ? z80.i : z80.r;
            F = (F & FC) | sz[A] | (z80.iff2 ? FV : 0);
            return 9;
        case 4: /* RRD */
            v = rd(HL);
            z80_mem_write(HL, (uint8_t)((v >> 4) | (A << 4)));
            A = (A & 0xF0) | (v & 0x0F);
            F = (F & FC) | szp[A];
            return 18;
        case 5: /* RLD */
            v = rd(HL);
            z80_mem_write(HL, (uint8_t)((v << 4) | (A & 0x0F)));
            A = (A & 0xF0) | (v >> 4);
            F = (F & FC) | szp[A];
            return 18;
        default:
            return 8;
        }
    }
}

/*
 * Executes one unprefixed opcode, with ip standing in for HL (it points at
 * IX or IY after a DD/FD prefix). Returns the cycles used.
 */
static int exec_main(uint8_t op, z80_pair_t *ip)
{
    uint16_t addr, t;
    uint8_t v, y = (op >> 3) & 7, z = op & 7;

    cycles_extra = 0;

    switch (op)
    {
    /* 8-bit loads, ALU and HALT: 0x40-0xBF */
    case 0x76:
        z80.halted = true;
        break;
    case 0x40 ... 0x75:
    case 0x77 ... 0x7F:
        if (z == 6)
            set_reg(y, rd(mem_operand(ip)), &z80.hl);
        else if (y == 6)
            z80_mem_write(mem_operand(ip), get_reg(z, &z80.hl));
        else
            set_reg(y, get_reg(z, ip), ip);
        break;
    case 0x80 ... 0xBF:
        alu(y, z == 6 ? rd(mem_operand(ip)) : get_reg(z, ip));
        break;

    /* INC r / DEC r / LD r,n */
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        if (y == 6)
        {
            addr = mem_operand(ip);
            v = rd(addr);
            z80_mem_write(addr, z == 4 ? inc8(v) : dec8(v));
        }
        else
        {
            v = get_reg(y, ip);
            set_reg(y, z == 4 ? inc8(v) : dec8(v), ip);
        }
        break;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
        if (y == 6)
        {
            addr = mem_operand(ip);
            if (cycles_extra)
                cycles_extra = 5;
            z80_mem_write(addr, fetch());
        }
        else
            set_reg(y, fetch(), ip);
        break;

    /* 16-bit loads and arithmetic */
    case 0x01: case 0x11: case 0x21: case 0x31:
        *rp(y >> 1, ip) = fetch16();
        break;
    case 0x03: case 0x13: case 0x23: case 0x33:
        (*rp(y >> 1, ip))++;
        break;
    case 0x0B: case 0x1B: case 0x2B: case 0x3B:
        (*rp(y >> 1, ip))--;
        break;
    case 0x09: case 0x19: case 0x29: case 0x39:
        ip->w = add16(ip->w, *rp(y >> 1, ip));
        break;
    case 0x02: z80_mem_write(BC, A); break;
    case 0x12: z80_mem_write(DE, A); break;
    case 0x0A: A = rd(BC); break;
    case 0x1A: A = rd(DE); break;
    case 0x22: wr16(fetch16(), ip->w); break;
    case 0x2A: ip->w = rd16(fetch16()); break;
    case 0x32: z80_mem_write(fetch16(), A); break;
    case 0x3A: A = rd(fetch16()); break;

    /* accumulator and flag operations */
    case 0x00: break;
    case 0x07:
        A = (A << 1) | (A >> 7);
        F = (F & (FS | FZ | FP)) | (A & (FX | FY | FC));
        break;
    case 0x0F:
        F = (F & (FS | FZ | FP)) | (A & FC);
        A = (A >> 1) | (A << 7);
        F |= A & (FX | FY);
        break;
    case 0x17:
        v = A >> 7;
        A = (A << 1) | (F & FC);
        F = (F & (FS | FZ | FP)) | (A & (FX | FY)) | v;
        break;
    case 0x1F:
        v = A & 1;
        A = (A >> 1) | ((F & FC) << 7);
        F = (F & (FS | FZ | FP)) | (A & (FX | FY)) | v;
        break;
    case 0x27: daa(); break;
    case 0x2F:
        A = ~A;
        F = (F & (FS | FZ | FP | FC)) | FH | FN | (A & (FX | FY));
        break;
    case 0x37:
        F = (F & (FS | FZ | FP)) | FC | (A & (FX | FY));
        break;
    case 0x3F:
        F = ((F & (FS | FZ | FP | FC)) | ((F & FC) << 4) | (A & (FX | FY))) ^ FC;
        break;

    /* exchanges */
    case 0x08:
        t = z80.af.w; z80.af.w = z80.af2.w; z80.af2.w = t;
        break;
    case 0xD9:
        t = BC; BC = z80.bc2.w; z80.bc2.w = t;
        t = DE; DE = z80.de2.w; z80.de2.w = t;
        t = HL; HL = z80.hl2.w; z80.hl2.w = t;
        break;
    case 0xEB:
        t = DE; DE = HL; HL = t;
        break;
    case 0xE3:
        t = rd16(SP);
        wr16(SP, ip->w);
        ip->w = t;
        break;

    /* jumps, calls, returns */
    case 0x10:
        v = fetch();
        if (--B)
        {
            PC += (int8_t)v;
            cycles_extra = 5;
        }
        break;
    case 0x18:
        PC += (int8_t)fetch();
        break;
    case 0x20: case 0x28: case 0x30: case 0x38:
        v = fetch();
        if (condition(y - 4))
        {
            PC += (int8_t)v;
            cycles_extra = 5;
        }
        break;
    case 0xC3:
        PC = fetch16();
        break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE2: case 0xEA: case 0xF2: case 0xFA:
        t = fetch16();
        if (condition(y))
            PC = t;
        break;
    case 0xCD:
        t = fetch16();
        push(PC);
        PC = t;
        break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: case 0xE4: case 0xEC: case 0xF4: case 0xFC:
        t = fetch16();
        if (condition(y))
        {
            push(PC);
            PC = t;
            cycles_extra = 7;
        }
        break;
    case 0xC9:
        PC = pop();
        break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: case 0xE0: case 0xE8: case 0xF0: case 0xF8:
        if (condition(y))
        {
            PC = pop();
            cycles_extra = 6;
        }
        break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        push(PC);
        PC = op & 0x38;
        break;
    case 0xE9:
        PC = ip->w;
        break;

    /* stack */
    case 0xC5: push(BC); break;
    case 0xD5: push(DE); break;
    case 0xE5: push(ip->w); break;
    case 0xF5: push(z80.af.w); break;
    case 0xC1: BC = pop(); break;
    case 0xD1: DE = pop(); break;
    case 0xE1: ip->w = pop(); break;
    case 0xF1: z80.af.w = pop(); break;
    case 0xF9: SP = ip->w; break;

    /* ALU with immediate */
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu(y, fetch());
        break;

    /* I/O and interrupts */
    case 0xD3:
        v = fetch();
        z80_io_write((uint16_t)(A << 8 | v), A);
        break;
    case 0xDB:
        v = fetch();
        A = z80_io_read((uint16_t)(A << 8 | v));
        break;
    case 0xF3:
        z80.iff1 = z80.iff2 = 0;
        break;
    case 0xFB:
        z80.iff1 = z80.iff2 = 1;
        z80.ei_pending = true;
        break;

    /* prefixes */
    case 0xCB:
        if (ip == &z80.hl)
            return exec_cb();
        return exec_index_cb(ip);
    case 0xED:
        return exec_ed();
    case 0xDD:
        bump_r();
        return 4 + exec_main(fetch(), &z80.ix);
    case 0xFD:
        bump_r();
        return 4 + exec_main(fetch(), &z80.iy);
    }

    return cycles_main[op] + cycles_extra;
}

static int interrupt(void)
{
    z80.halted = false;
    z80.iff1 = z80.iff2 = 0;
    bump_r();
    push(PC);
    if (z80.im == 2)
    {
        PC = rd16((uint16_t)(z80.i << 8 | 0xFF));
        return 19;
    }
    PC = 0x38;
    return 13;
}

void z80_reset(void)
{
    for (unsigned i = 0; i < 256; i++)
    {
        uint8_t p = i;
        p ^= p >> 4;
        p ^= p >> 2;
        p ^= p >> 1;
        sz[i] = (i & (FS | FX | FY)) | (i ? 0 : FZ);
        szp[i] = sz[i] | ((p & 1) ? 0 : FP);
    }

    z80.af.w = z80.sp.w = 0xFFFF;
    z80.pc.w = 0;
    z80.i = z80.r = 0;
    z80.iff1 = z80.iff2 = z80.im = 0;
    z80.halted = z80.ei_pending = z80.irq_line = false;
}

int z80_run(int cycles)
{
    int done = 0;

    while (done < cycles)
    {
        if (z80.irq_line && z80.iff1 && !z80.ei_pending)
            done += interrupt();
        if (z80.halted)
        {
            /* HALT repeats NOPs until an interrupt; skip ahead in one step. */
            done += (cycles - done + 3) & ~3;
            break;
        }
        z80.ei_pending = false;
        bump_r();
        done += exec_main(fetch(), &z80.hl);
    }
    return done;
}
