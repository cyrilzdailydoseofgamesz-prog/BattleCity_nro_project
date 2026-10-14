/* Minimal NES core: NROM (mapper 0), 6502 CPU, scanline-based PPU, no APU. */
#include <string.h>
#include "nes.h"

uint32_t nes_fb[NES_WIDTH * NES_HEIGHT];

/* ------------------------------------------------------------ palette */
static const uint32_t nes_palette[64] = {
    0x666666,0x002A88,0x1412A7,0x3B00A4,0x5C007E,0x6E0040,0x6C0600,0x561D00,
    0x333500,0x0B4800,0x005200,0x004F08,0x00404D,0x000000,0x000000,0x000000,
    0xADADAD,0x155FD9,0x4240FF,0x7527FE,0xA01ACC,0xB71E7B,0xB53120,0x994E00,
    0x6B6D00,0x388700,0x0C9300,0x008F32,0x007C8D,0x000000,0x000000,0x000000,
    0xFFFEFF,0x64B0FF,0x9290FF,0xC676FF,0xF36AFF,0xFE6ECC,0xFE8170,0xEA9E22,
    0xBCBE00,0x88D800,0x5CE430,0x45E082,0x48CDDE,0x4F4F4F,0x000000,0x000000,
    0xFFFEFF,0xC0DFFF,0xD3D2FF,0xE8C8FF,0xFBC2FF,0xFEC4EA,0xFECCC5,0xF7D8A5,
    0xE4E594,0xCFEF96,0xBDF4AB,0xB3F3CC,0xB5EBF2,0xB8B8B8,0x000000,0x000000
};

/* ------------------------------------------------------------ cartridge */
static uint8_t prg[32768];
static int     prg_size;
static uint8_t chr[8192];
static int     chr_is_ram;
static int     vmirror;
static uint8_t prg_ram[8192];

/* ------------------------------------------------------------ memory */
static uint8_t ram[2048];
static uint8_t ciram[2048];
static uint8_t palram[32];
static uint8_t oam[256];

/* ------------------------------------------------------------ PPU state */
static uint8_t  ppu_ctrl, ppu_mask, ppu_status, oam_addr, read_buf;
static uint16_t vreg, treg;
static uint8_t  fine_x, wtoggle;
static int      nmi_pending;

/* ------------------------------------------------------------ input */
static uint8_t pad_state, pad_shift, pad_strobe;

/* ------------------------------------------------------------ CPU state */
static uint16_t pc;
static uint8_t  rA, rX, rY, rSP;
static uint8_t  fc, fz, fi, fd, fv, fn;
static int      cpu_stall;

/* ============================================================ PPU bus */
static int nt_index(uint16_t a)
{
    int nt = (a >> 10) & 3;
    int page = vmirror ? (nt & 1) : (nt >> 1);
    return page * 0x400 + (a & 0x3FF);
}

static int pal_index(int i)
{
    i &= 0x1F;
    if ((i & 3) == 0) i &= 0x0F;
    return i;
}

static uint8_t ppu_rd(uint16_t a)
{
    a &= 0x3FFF;
    if (a < 0x2000) return chr[a];
    if (a < 0x3F00) return ciram[nt_index(a)];
    return palram[pal_index(a)];
}

static void ppu_wr(uint16_t a, uint8_t d)
{
    a &= 0x3FFF;
    if (a < 0x2000) { if (chr_is_ram) chr[a] = d; }
    else if (a < 0x3F00) ciram[nt_index(a)] = d;
    else palram[pal_index(a)] = d & 0x3F;
}

static uint8_t ppu_reg_rd(int r)
{
    uint8_t d;
    switch (r) {
    case 2:
        d = ppu_status;
        ppu_status &= ~0x80;
        wtoggle = 0;
        return d;
    case 4:
        return oam[oam_addr];
    case 7: {
        uint16_t a = vreg & 0x3FFF;
        if (a < 0x3F00) { d = read_buf; read_buf = ppu_rd(a); }
        else { d = ppu_rd(a); read_buf = ppu_rd(a - 0x1000); }
        vreg += (ppu_ctrl & 4) ? 32 : 1;
        return d;
    }
    }
    return 0;
}

static void ppu_reg_wr(int r, uint8_t d)
{
    switch (r) {
    case 0: {
        uint8_t old = ppu_ctrl;
        ppu_ctrl = d;
        treg = (treg & 0xF3FF) | ((d & 3) << 10);
        if (!(old & 0x80) && (d & 0x80) && (ppu_status & 0x80)) nmi_pending = 1;
        break;
    }
    case 1: ppu_mask = d; break;
    case 3: oam_addr = d; break;
    case 4: oam[oam_addr++] = d; break;
    case 5:
        if (!wtoggle) {
            treg = (treg & 0xFFE0) | (d >> 3);
            fine_x = d & 7;
            wtoggle = 1;
        } else {
            treg = (treg & 0x8C1F) | ((d & 7) << 12) | ((d & 0xF8) << 2);
            wtoggle = 0;
        }
        break;
    case 6:
        if (!wtoggle) {
            treg = (treg & 0x00FF) | ((d & 0x3F) << 8);
            wtoggle = 1;
        } else {
            treg = (treg & 0xFF00) | d;
            vreg = treg;
            wtoggle = 0;
        }
        break;
    case 7:
        ppu_wr(vreg, d);
        vreg += (ppu_ctrl & 4) ? 32 : 1;
        break;
    }
}

/* ============================================================ CPU bus */
static uint8_t rd(uint16_t a)
{
    if (a < 0x2000) return ram[a & 0x7FF];
    if (a < 0x4000) return ppu_reg_rd(a & 7);
    if (a == 0x4016) {
        uint8_t r;
        if (pad_strobe) return (pad_state & 1) | 0x40;
        r = pad_shift & 1;
        pad_shift = (pad_shift >> 1) | 0x80;
        return r | 0x40;
    }
    if (a >= 0x8000) return prg[(a - 0x8000) & (prg_size - 1)];
    if (a >= 0x6000) return prg_ram[a - 0x6000];
    return 0;
}

static void wr(uint16_t a, uint8_t d)
{
    if (a < 0x2000) { ram[a & 0x7FF] = d; return; }
    if (a < 0x4000) { ppu_reg_wr(a & 7, d); return; }
    if (a == 0x4014) {
        int i;
        uint16_t base = (uint16_t)d << 8;
        for (i = 0; i < 256; i++) oam[(uint8_t)(oam_addr + i)] = rd(base + i);
        cpu_stall += 513;
        return;
    }
    if (a == 0x4016) {
        pad_strobe = d & 1;
        if (pad_strobe) pad_shift = pad_state;
        else pad_shift = pad_state;
        return;
    }
    if (a >= 0x6000 && a < 0x8000) prg_ram[a - 0x6000] = d;
}

static uint16_t rd16(uint16_t a)
{
    return rd(a) | (rd(a + 1) << 8);
}

/* ============================================================ CPU */
enum {
    I_NOP, I_ADC, I_AND, I_ASL, I_BCC, I_BCS, I_BEQ, I_BIT, I_BMI, I_BNE,
    I_BPL, I_BRK, I_BVC, I_BVS, I_CLC, I_CLD, I_CLI, I_CLV, I_CMP, I_CPX,
    I_CPY, I_DEC, I_DEX, I_DEY, I_EOR, I_INC, I_INX, I_INY, I_JMP, I_JSR,
    I_LDA, I_LDX, I_LDY, I_LSR, I_ORA, I_PHA, I_PHP, I_PLA, I_PLP, I_ROL,
    I_ROR, I_RTI, I_RTS, I_SBC, I_SEC, I_SED, I_SEI, I_STA, I_STX, I_STY,
    I_TAX, I_TAY, I_TSX, I_TXA, I_TXS, I_TYA
};
enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY,
       M_IND, M_IZX, M_IZY, M_REL };

static uint8_t op_ins[256], op_mode[256], op_cyc[256], op_pg[256];

#define O(c,i,m,cy,pg) do { op_ins[c]=I_##i; op_mode[c]=M_##m; \
                            op_cyc[c]=cy; op_pg[c]=pg; } while (0)

static void init_ops(void)
{
    int i;
    for (i = 0; i < 256; i++) { op_ins[i]=I_NOP; op_mode[i]=M_IMP; op_cyc[i]=2; op_pg[i]=0; }

    O(0x00,BRK,IMP,7,0); O(0x01,ORA,IZX,6,0); O(0x05,ORA,ZP,3,0);  O(0x06,ASL,ZP,5,0);
    O(0x08,PHP,IMP,3,0); O(0x09,ORA,IMM,2,0); O(0x0A,ASL,ACC,2,0); O(0x0D,ORA,ABS,4,0);
    O(0x0E,ASL,ABS,6,0);
    O(0x10,BPL,REL,2,0); O(0x11,ORA,IZY,5,1); O(0x15,ORA,ZPX,4,0); O(0x16,ASL,ZPX,6,0);
    O(0x18,CLC,IMP,2,0); O(0x19,ORA,ABY,4,1); O(0x1D,ORA,ABX,4,1); O(0x1E,ASL,ABX,7,0);
    O(0x20,JSR,ABS,6,0); O(0x21,AND,IZX,6,0); O(0x24,BIT,ZP,3,0);  O(0x25,AND,ZP,3,0);
    O(0x26,ROL,ZP,5,0);  O(0x28,PLP,IMP,4,0); O(0x29,AND,IMM,2,0); O(0x2A,ROL,ACC,2,0);
    O(0x2C,BIT,ABS,4,0); O(0x2D,AND,ABS,4,0); O(0x2E,ROL,ABS,6,0);
    O(0x30,BMI,REL,2,0); O(0x31,AND,IZY,5,1); O(0x35,AND,ZPX,4,0); O(0x36,ROL,ZPX,6,0);
    O(0x38,SEC,IMP,2,0); O(0x39,AND,ABY,4,1); O(0x3D,AND,ABX,4,1); O(0x3E,ROL,ABX,7,0);
    O(0x40,RTI,IMP,6,0); O(0x41,EOR,IZX,6,0); O(0x45,EOR,ZP,3,0);  O(0x46,LSR,ZP,5,0);
    O(0x48,PHA,IMP,3,0); O(0x49,EOR,IMM,2,0); O(0x4A,LSR,ACC,2,0); O(0x4C,JMP,ABS,3,0);
    O(0x4D,EOR,ABS,4,0); O(0x4E,LSR,ABS,6,0);
    O(0x50,BVC,REL,2,0); O(0x51,EOR,IZY,5,1); O(0x55,EOR,ZPX,4,0); O(0x56,LSR,ZPX,6,0);
    O(0x58,CLI,IMP,2,0); O(0x59,EOR,ABY,4,1); O(0x5D,EOR,ABX,4,1); O(0x5E,LSR,ABX,7,0);
    O(0x60,RTS,IMP,6,0); O(0x61,ADC,IZX,6,0); O(0x65,ADC,ZP,3,0);  O(0x66,ROR,ZP,5,0);
    O(0x68,PLA,IMP,4,0); O(0x69,ADC,IMM,2,0); O(0x6A,ROR,ACC,2,0); O(0x6C,JMP,IND,5,0);
    O(0x6D,ADC,ABS,4,0); O(0x6E,ROR,ABS,6,0);
    O(0x70,BVS,REL,2,0); O(0x71,ADC,IZY,5,1); O(0x75,ADC,ZPX,4,0); O(0x76,ROR,ZPX,6,0);
    O(0x78,SEI,IMP,2,0); O(0x79,ADC,ABY,4,1); O(0x7D,ADC,ABX,4,1); O(0x7E,ROR,ABX,7,0);
    O(0x81,STA,IZX,6,0); O(0x84,STY,ZP,3,0);  O(0x85,STA,ZP,3,0);  O(0x86,STX,ZP,3,0);
    O(0x88,DEY,IMP,2,0); O(0x8A,TXA,IMP,2,0); O(0x8C,STY,ABS,4,0); O(0x8D,STA,ABS,4,0);
    O(0x8E,STX,ABS,4,0);
    O(0x90,BCC,REL,2,0); O(0x91,STA,IZY,6,0); O(0x94,STY,ZPX,4,0); O(0x95,STA,ZPX,4,0);
    O(0x96,STX,ZPY,4,0); O(0x98,TYA,IMP,2,0); O(0x99,STA,ABY,5,0); O(0x9A,TXS,IMP,2,0);
    O(0x9D,STA,ABX,5,0);
    O(0xA0,LDY,IMM,2,0); O(0xA1,LDA,IZX,6,0); O(0xA2,LDX,IMM,2,0); O(0xA4,LDY,ZP,3,0);
    O(0xA5,LDA,ZP,3,0);  O(0xA6,LDX,ZP,3,0);  O(0xA8,TAY,IMP,2,0); O(0xA9,LDA,IMM,2,0);
    O(0xAA,TAX,IMP,2,0); O(0xAC,LDY,ABS,4,0); O(0xAD,LDA,ABS,4,0); O(0xAE,LDX,ABS,4,0);
    O(0xB0,BCS,REL,2,0); O(0xB1,LDA,IZY,5,1); O(0xB4,LDY,ZPX,4,0); O(0xB5,LDA,ZPX,4,0);
    O(0xB6,LDX,ZPY,4,0); O(0xB8,CLV,IMP,2,0); O(0xB9,LDA,ABY,4,1); O(0xBA,TSX,IMP,2,0);
    O(0xBC,LDY,ABX,4,1); O(0xBD,LDA,ABX,4,1); O(0xBE,LDX,ABY,4,1);
    O(0xC0,CPY,IMM,2,0); O(0xC1,CMP,IZX,6,0); O(0xC4,CPY,ZP,3,0);  O(0xC5,CMP,ZP,3,0);
    O(0xC6,DEC,ZP,5,0);  O(0xC8,INY,IMP,2,0); O(0xC9,CMP,IMM,2,0); O(0xCA,DEX,IMP,2,0);
    O(0xCC,CPY,ABS,4,0); O(0xCD,CMP,ABS,4,0); O(0xCE,DEC,ABS,6,0);
    O(0xD0,BNE,REL,2,0); O(0xD1,CMP,IZY,5,1); O(0xD5,CMP,ZPX,4,0); O(0xD6,DEC,ZPX,6,0);
    O(0xD8,CLD,IMP,2,0); O(0xD9,CMP,ABY,4,1); O(0xDD,CMP,ABX,4,1); O(0xDE,DEC,ABX,7,0);
    O(0xE0,CPX,IMM,2,0); O(0xE1,SBC,IZX,6,0); O(0xE4,CPX,ZP,3,0);  O(0xE5,SBC,ZP,3,0);
    O(0xE6,INC,ZP,5,0);  O(0xE8,INX,IMP,2,0); O(0xE9,SBC,IMM,2,0); O(0xEA,NOP,IMP,2,0);
    O(0xEC,CPX,ABS,4,0); O(0xED,SBC,ABS,4,0); O(0xEE,INC,ABS,6,0);
    O(0xF0,BEQ,REL,2,0); O(0xF1,SBC,IZY,5,1); O(0xF5,SBC,ZPX,4,0); O(0xF6,INC,ZPX,6,0);
    O(0xF8,SED,IMP,2,0); O(0xF9,SBC,ABY,4,1); O(0xFD,SBC,ABX,4,1); O(0xFE,INC,ABX,7,0);
}

static void push(uint8_t d) { ram[0x100 + rSP] = d; rSP--; }
static uint8_t pop(void)    { rSP++; return ram[0x100 + rSP]; }

static uint8_t get_p(int brk)
{
    return fc | (fz << 1) | (fi << 2) | (fd << 3) | (brk ? 0x10 : 0) |
           0x20 | (fv << 6) | (fn << 7);
}

static void set_p(uint8_t p)
{
    fc = p & 1; fz = (p >> 1) & 1; fi = (p >> 2) & 1;
    fd = (p >> 3) & 1; fv = (p >> 6) & 1; fn = (p >> 7) & 1;
}

static void set_nz(uint8_t v) { fz = (v == 0); fn = (v >> 7) & 1; }

static void do_adc(uint8_t m)
{
    uint16_t s = rA + m + fc;
    fv = ((~(rA ^ m) & (rA ^ s)) & 0x80) ? 1 : 0;
    fc = s > 0xFF;
    rA = (uint8_t)s;
    set_nz(rA);
}

static void do_cmp(uint8_t r, uint8_t m)
{
    fc = r >= m;
    set_nz((uint8_t)(r - m));
}

static uint8_t do_asl(uint8_t v) { fc = v >> 7; v <<= 1; set_nz(v); return v; }
static uint8_t do_lsr(uint8_t v) { fc = v & 1; v >>= 1; set_nz(v); return v; }
static uint8_t do_rol(uint8_t v) { uint8_t c = fc; fc = v >> 7; v = (v << 1) | c; set_nz(v); return v; }
static uint8_t do_ror(uint8_t v) { uint8_t c = fc; fc = v & 1; v = (v >> 1) | (c << 7); set_nz(v); return v; }

#define RMW(f) do { if (mode == M_ACC) rA = f(rA); else wr(addr, f(rd(addr))); } while (0)
#define BRANCH(cond) do { if (cond) { cyc++; if ((pc ^ addr) & 0xFF00) cyc++; pc = addr; } } while (0)

static int cpu_step(void)
{
    uint8_t code = rd(pc++);
    int ins = op_ins[code], mode = op_mode[code], cyc = op_cyc[code];
    uint16_t addr = 0;
    uint8_t m;

    switch (mode) {
    case M_IMM: addr = pc++; break;
    case M_ZP:  addr = rd(pc++); break;
    case M_ZPX: addr = (uint8_t)(rd(pc++) + rX); break;
    case M_ZPY: addr = (uint8_t)(rd(pc++) + rY); break;
    case M_ABS: addr = rd16(pc); pc += 2; break;
    case M_ABX: {
        uint16_t b = rd16(pc); pc += 2; addr = b + rX;
        if (op_pg[code] && ((b ^ addr) & 0xFF00)) cyc++;
        break;
    }
    case M_ABY: {
        uint16_t b = rd16(pc); pc += 2; addr = b + rY;
        if (op_pg[code] && ((b ^ addr) & 0xFF00)) cyc++;
        break;
    }
    case M_IND: {
        uint16_t p = rd16(pc); pc += 2;
        addr = rd(p) | (rd((p & 0xFF00) | ((p + 1) & 0xFF)) << 8);
        break;
    }
    case M_IZX: {
        uint8_t z = (uint8_t)(rd(pc++) + rX);
        addr = rd(z) | (rd((uint8_t)(z + 1)) << 8);
        break;
    }
    case M_IZY: {
        uint8_t z = rd(pc++);
        uint16_t b = rd(z) | (rd((uint8_t)(z + 1)) << 8);
        addr = b + rY;
        if (op_pg[code] && ((b ^ addr) & 0xFF00)) cyc++;
        break;
    }
    case M_REL: {
        int8_t o = (int8_t)rd(pc++);
        addr = pc + o;
        break;
    }
    default: break;
    }

    switch (ins) {
    case I_ADC: do_adc(rd(addr)); break;
    case I_SBC: do_adc(rd(addr) ^ 0xFF); break;
    case I_AND: rA &= rd(addr); set_nz(rA); break;
    case I_ORA: rA |= rd(addr); set_nz(rA); break;
    case I_EOR: rA ^= rd(addr); set_nz(rA); break;
    case I_ASL: RMW(do_asl); break;
    case I_LSR: RMW(do_lsr); break;
    case I_ROL: RMW(do_rol); break;
    case I_ROR: RMW(do_ror); break;
    case I_BCC: BRANCH(!fc); break;
    case I_BCS: BRANCH(fc);  break;
    case I_BEQ: BRANCH(fz);  break;
    case I_BNE: BRANCH(!fz); break;
    case I_BMI: BRANCH(fn);  break;
    case I_BPL: BRANCH(!fn); break;
    case I_BVC: BRANCH(!fv); break;
    case I_BVS: BRANCH(fv);  break;
    case I_BIT: m = rd(addr); fz = (rA & m) == 0; fn = (m >> 7) & 1; fv = (m >> 6) & 1; break;
    case I_BRK:
        pc++;
        push(pc >> 8); push(pc & 0xFF); push(get_p(1));
        fi = 1; pc = rd16(0xFFFE);
        break;
    case I_CLC: fc = 0; break;
    case I_CLD: fd = 0; break;
    case I_CLI: fi = 0; break;
    case I_CLV: fv = 0; break;
    case I_SEC: fc = 1; break;
    case I_SED: fd = 1; break;
    case I_SEI: fi = 1; break;
    case I_CMP: do_cmp(rA, rd(addr)); break;
    case I_CPX: do_cmp(rX, rd(addr)); break;
    case I_CPY: do_cmp(rY, rd(addr)); break;
    case I_DEC: m = rd(addr) - 1; wr(addr, m); set_nz(m); break;
    case I_INC: m = rd(addr) + 1; wr(addr, m); set_nz(m); break;
    case I_DEX: rX--; set_nz(rX); break;
    case I_DEY: rY--; set_nz(rY); break;
    case I_INX: rX++; set_nz(rX); break;
    case I_INY: rY++; set_nz(rY); break;
    case I_JMP: pc = addr; break;
    case I_JSR: push((pc - 1) >> 8); push((pc - 1) & 0xFF); pc = addr; break;
    case I_RTS: m = pop(); pc = ((pop() << 8) | m) + 1; break;
    case I_RTI: set_p(pop()); m = pop(); pc = (pop() << 8) | m; break;
    case I_LDA: rA = rd(addr); set_nz(rA); break;
    case I_LDX: rX = rd(addr); set_nz(rX); break;
    case I_LDY: rY = rd(addr); set_nz(rY); break;
    case I_PHA: push(rA); break;
    case I_PHP: push(get_p(1)); break;
    case I_PLA: rA = pop(); set_nz(rA); break;
    case I_PLP: set_p(pop()); break;
    case I_STA: wr(addr, rA); break;
    case I_STX: wr(addr, rX); break;
    case I_STY: wr(addr, rY); break;
    case I_TAX: rX = rA; set_nz(rX); break;
    case I_TAY: rY = rA; set_nz(rY); break;
    case I_TSX: rX = rSP; set_nz(rX); break;
    case I_TXA: rA = rX; set_nz(rA); break;
    case I_TXS: rSP = rX; break;
    case I_TYA: rA = rY; set_nz(rA); break;
    default: break; /* NOP and unofficial opcodes */
    }

    cyc += cpu_stall;
    cpu_stall = 0;
    return cyc;
}

static int do_nmi(void)
{
    push(pc >> 8); push(pc & 0xFF); push(get_p(0));
    fi = 1;
    pc = rd16(0xFFFA);
    return 7;
}

/* ============================================================ PPU render */
static void render_scanline(int sl)
{
    uint8_t bg[264];
    uint8_t spr[256], sflag[256];
    uint32_t *dst = &nes_fb[sl * NES_WIDTH];
    int x, i, b, tile;

    if (!(ppu_mask & 0x18)) {
        uint32_t back = nes_palette[palram[0] & 0x3F];
        for (x = 0; x < 256; x++) dst[x] = back;
        return;
    }

    /* background */
    memset(bg, 0, sizeof(bg));
    if (ppu_mask & 0x08) {
        uint16_t vv = vreg;
        for (tile = 0; tile < 33; tile++) {
            uint8_t tn = ppu_rd(0x2000 | (vv & 0x0FFF));
            uint8_t at = ppu_rd(0x23C0 | (vv & 0x0C00) | ((vv >> 4) & 0x38) | ((vv >> 2) & 0x07));
            int sh = ((vv >> 4) & 4) | (vv & 2);
            uint8_t pal = (at >> sh) & 3;
            uint16_t base = ((ppu_ctrl & 0x10) ? 0x1000 : 0) + tn * 16 + ((vv >> 12) & 7);
            uint8_t lo = ppu_rd(base), hi = ppu_rd(base + 8);
            for (b = 0; b < 8; b++) {
                int p = ((lo >> (7 - b)) & 1) | (((hi >> (7 - b)) & 1) << 1);
                bg[tile * 8 + b] = p ? ((pal << 2) | p) : 0;
            }
            if ((vv & 0x1F) == 31) { vv &= ~0x1F; vv ^= 0x400; } else vv++;
        }
    }

    /* sprites */
    memset(spr, 0, sizeof(spr));
    memset(sflag, 0, sizeof(sflag));
    if (ppu_mask & 0x10) {
        int h = (ppu_ctrl & 0x20) ? 16 : 8, count = 0;
        for (i = 0; i < 64; i++) {
            int top = oam[i * 4] + 1, row = sl - top;
            uint8_t tn, at, lo, hi;
            int sx;
            uint16_t base;
            if (row < 0 || row >= h) continue;
            if (++count > 8) { ppu_status |= 0x20; break; }
            tn = oam[i * 4 + 1]; at = oam[i * 4 + 2]; sx = oam[i * 4 + 3];
            if (at & 0x80) row = h - 1 - row;
            if (h == 8)
                base = ((ppu_ctrl & 0x08) ? 0x1000 : 0) + tn * 16 + row;
            else
                base = ((tn & 1) ? 0x1000 : 0) + (tn & 0xFE) * 16 + (row & 7) + (row >= 8 ? 16 : 0);
            lo = ppu_rd(base); hi = ppu_rd(base + 8);
            for (b = 0; b < 8; b++) {
                int bit = (at & 0x40) ? b : 7 - b;
                int p = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                int px = sx + b;
                if (!p || px > 255 || spr[px]) continue;
                spr[px] = ((at & 3) << 2) | p;
                sflag[px] = ((at & 0x20) ? 1 : 0) | (i == 0 ? 2 : 0);
            }
        }
    }

    /* compose */
    for (x = 0; x < 256; x++) {
        uint8_t bp = bg[x + fine_x], sp = spr[x];
        int idx;
        if (x < 8) {
            if (!(ppu_mask & 0x02)) bp = 0;
            if (!(ppu_mask & 0x04)) sp = 0;
        }
        if ((bp & 3) && sp && (sflag[x] & 2) && x < 255) ppu_status |= 0x40;
        if (sp && (!(sflag[x] & 1) || !(bp & 3))) idx = 0x10 + sp;
        else if (bp & 3) idx = bp;
        else idx = 0;
        dst[x] = nes_palette[palram[pal_index(idx)] & 0x3F];
    }
}

/* ============================================================ public API */
int nes_load(const uint8_t *d, size_t size)
{
    int prg_banks, chr_banks, mapper;
    size_t off = 16;

    if (size < 16 || d[0] != 'N' || d[1] != 'E' || d[2] != 'S' || d[3] != 0x1A) return -1;
    prg_banks = d[4]; chr_banks = d[5];
    mapper = (d[6] >> 4) | (d[7] & 0xF0);
    if (mapper != 0 || prg_banks < 1 || prg_banks > 2) return -2;
    if (d[6] & 4) off += 512;
    if (size < off + (size_t)prg_banks * 16384 + (size_t)chr_banks * 8192) return -3;

    memcpy(prg, d + off, (size_t)prg_banks * 16384);
    prg_size = prg_banks * 16384;
    off += (size_t)prg_banks * 16384;
    memset(chr, 0, sizeof(chr));
    if (chr_banks) { memcpy(chr, d + off, 8192); chr_is_ram = 0; }
    else chr_is_ram = 1;
    vmirror = d[6] & 1;
    init_ops();
    nes_reset();
    return 0;
}

void nes_reset(void)
{
    memset(ram, 0, sizeof(ram));
    memset(ciram, 0, sizeof(ciram));
    memset(palram, 0, sizeof(palram));
    memset(oam, 0, sizeof(oam));
    memset(prg_ram, 0, sizeof(prg_ram));
    ppu_ctrl = ppu_mask = ppu_status = oam_addr = read_buf = 0;
    vreg = treg = 0; fine_x = wtoggle = 0; nmi_pending = 0;
    pad_state = pad_shift = pad_strobe = 0;
    rA = rX = rY = 0; rSP = 0xFD;
    set_p(0x24);
    cpu_stall = 0;
    pc = rd16(0xFFFC);
}

void nes_set_pad(uint8_t s) { pad_state = s; }

void nes_run_frame(void)
{
    int sl;
    long budget = 0;
    for (sl = 0; sl < 262; sl++) {
        int rendering = (ppu_mask & 0x18) != 0;
        if (sl < 240) render_scanline(sl);
        if (sl == 241) {
            ppu_status |= 0x80;
            if (ppu_ctrl & 0x80) nmi_pending = 1;
        }
        if (sl == 261) {
            ppu_status &= ~0xE0;
            if (rendering) vreg = (vreg & ~0x7BE0) | (treg & 0x7BE0);
        }
        budget += 341;
        while (budget > 0) {
            int c;
            if (nmi_pending) { nmi_pending = 0; c = do_nmi(); }
            else c = cpu_step();
            budget -= c * 3;
        }
        if ((sl < 240 || sl == 261) && (ppu_mask & 0x18)) {
            if (sl < 240) {
                if ((vreg & 0x7000) != 0x7000) vreg += 0x1000;
                else {
                    int y = (vreg & 0x03E0) >> 5;
                    vreg &= ~0x7000;
                    if (y == 29) { y = 0; vreg ^= 0x0800; }
                    else if (y == 31) y = 0;
                    else y++;
                    vreg = (vreg & ~0x03E0) | (y << 5);
                }
            }
            vreg = (vreg & ~0x041F) | (treg & 0x041F);
        }
    }
}
