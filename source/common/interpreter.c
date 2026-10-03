#include <math.h>
#include "v810_cpu.h"
#include "v810_mem.h"
#include "v810_opt.h"
#include "vb_types.h"
#include "drc_core.h"

static const uint16_t cond_lut[32] = {
    0xf0f0, 0xff00, 0xaaaa, 0xffaa, 0xcccc, 0xffff, 0x3c3c, 0xbebe,
    0x0f0f, 0x00ff, 0x5555, 0x0055, 0x3333, 0x0000, 0xc3c3, 0x4141,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
};
#define COND(code, psw_val) ((cond_lut[(code) & 31] >> ((psw_val) & 15)) & 1)

#define RET(code) do { S[PSW] = psw; return (code); } while (0)

#define ROM_MASK (0x07000000u | (MAX_ROM_SIZE - 1))

#define FETCH16(a) ({ \
    WORD _a = (a); \
    (_a & 0x07000000) == 0x07000000 \
        ? *(const HWORD *)(rom_off + (_a & ROM_MASK & ~1u)) \
        : (HWORD)mem_rhword(_a); })

#define RD8(a) ({ \
    WORD _a = (a); BYTE _v; \
    switch (_a & 0x07000000) { \
    case 0x05000000: _v = *(const BYTE *)(wram_off + (_a & 0x0500ffffu)); break; \
    case 0x07000000: _v = *(const BYTE *)(rom_off + (_a & ROM_MASK)); break; \
    default: _v = (BYTE)mem_rbyte(_a); } \
    _v; })

#define RD16(a) ({ \
    WORD _a = (a) & ~1u; HWORD _v; \
    switch (_a & 0x07000000) { \
    case 0x05000000: _v = *(const HWORD *)(wram_off + (_a & 0x0500fffeu)); break; \
    case 0x07000000: _v = *(const HWORD *)(rom_off + (_a & ROM_MASK & ~1u)); break; \
    default: _v = (HWORD)mem_rhword(_a); } \
    _v; })

#define RD32(a) ({ \
    WORD _a = (a) & ~3u; WORD _v; \
    switch (_a & 0x07000000) { \
    case 0x05000000: _v = *(const WORD *)(wram_off + (_a & 0x0500fffcu)); break; \
    case 0x07000000: _v = *(const WORD *)(rom_off + (_a & ROM_MASK & ~3u)); break; \
    default: _v = (WORD)mem_rword(_a); } \
    _v; })

#define HW_WRITE(call) do { \
    st->cycles = cycles; \
    st->cycles_until_event_full = st->cycles_until_event_partial = -1; \
    call; \
    if (st->cycles_until_event_partial >= 0) target = cycles + st->cycles_until_event_partial; \
    else st->cycles_until_event_full = st->cycles_until_event_partial = (int)(target - cycles); \
} while (0)

#define WR8(a, v) do { \
    WORD _a = (a); WORD _d = (v); \
    switch (_a & 0x07000000) { \
    case 0x05000000: *(BYTE *)(wram_off + (_a & 0x0500ffffu)) = _d; break; \
    case 0x02000000: HW_WRITE(mem_wbyte(_a, _d)); break; \
    default: mem_wbyte(_a, _d); } } while (0)

#define WR16(a, v) do { \
    WORD _a = (a) & ~1u; WORD _d = (v); \
    switch (_a & 0x07000000) { \
    case 0x05000000: *(HWORD *)(wram_off + (_a & 0x0500fffeu)) = _d; break; \
    case 0x02000000: HW_WRITE(mem_whword(_a, _d)); break; \
    default: mem_whword(_a, _d); } } while (0)

#define WR32(a, v) do { \
    WORD _a = (a) & ~3u; WORD _d = (v); \
    switch (_a & 0x07000000) { \
    case 0x05000000: *(WORD *)(wram_off + (_a & 0x0500fffcu)) = _d; break; \
    case 0x02000000: HW_WRITE(mem_wword(_a, _d)); break; \
    default: mem_wword(_a, _d); } } while (0)

int interpreter_run(void) {
    cpu_state *const st = &vb_state->v810_state;
    WORD *const R = st->P_REG;
    WORD *const S = st->S_REG;
    const size_t rom_off = V810_ROM1.off;
    const size_t wram_off = vb_state->V810_VB_RAM.off;

    WORD psw = S[PSW];
    WORD PC = st->PC;
    WORD last_PC = PC;
    WORD cycles = st->cycles;
    BYTE last_opcode = 0;
    WORD target = cycles;
    do {
        if ((SWORD)(target - cycles) <= 0) {
            st->PC = PC;
            st->cycles = cycles;
            S[PSW] = psw;
            if (serviceInt(cycles, PC) && (PC != st->PC || st->ret)) {
                st->cycles = cycles;
                return 0;
            }
            psw = S[PSW];
            target = cycles + st->cycles_until_event_partial;
        }
        HWORD instr = FETCH16(PC);
        PC += 2;
        BYTE opcode = instr >> 10;
        BYTE reg1 = instr & 31;
        BYTE reg2 = (instr >> 5) & 31;
        cycles += opcycle[opcode];
        if (opcode < 0x20) {
            WORD reg1_val = 0;
            if (!(opcode & 0x10) && reg1) reg1_val = R[reg1];
            switch (opcode) {
                case V810_OP_MOV:
                    R[reg2] = reg1_val;
                    break;
                case V810_OP_ADD: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    WORD res = reg2_val + reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg2_val ^ reg1_val) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg2_val;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_SUB: case V810_OP_CMP: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    WORD res = reg2_val - reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)((reg2_val ^ reg1_val) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)reg2_val < (unsigned)reg1_val;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    if (opcode == V810_OP_SUB) R[reg2] = res;
                    break;
                }
                case V810_OP_SHL: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = reg2_val << reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (32 - reg1_val)) & 1 : 0;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_SHR: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = reg2_val >> reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (reg1_val - 1)) & 1 : 0;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_JMP:
                    PC = reg1_val;
                    break;
                case V810_OP_SAR: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = (SWORD)reg2_val >> reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (reg1_val - 1)) & 1 : 0;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_MUL: {
                    SWORD reg2_val = reg2 ? (SWORD)R[reg2] : 0;
                    int64_t res = (int64_t)(SWORD)reg1_val * (int64_t)reg2_val;
                    bool ov = res != (int64_t)(int32_t)res;
                    bool z = res == 0;
                    bool s = res < 0;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2);
                    R[30] = (WORD)(res >> 32);
                    R[reg2] = (WORD)res;
                    break;
                }
                case V810_OP_DIV: {
                    SWORD reg2_val = reg2 ? (SWORD)R[reg2] : 0;
                    if (reg2_val == 0x80000000 && (SWORD)reg1_val == -1) {
                        R[30] = 0;
                        psw = (psw & ~0x7) | 6;
                    } else {
                        R[30] = reg2_val % (SWORD)reg1_val;
                        SWORD res = reg2_val / (SWORD)reg1_val;
                        bool z = res == 0;
                        bool s = res < 0;
                        psw = (psw & ~0x7) | z | (s << 1);
                        R[reg2] = res;
                    }
                    break;
                }
                case V810_OP_MULU: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    uint64_t res = (uint64_t)reg1_val * (uint64_t)reg2_val;
                    bool ov = res != (uint64_t)(uint32_t)res;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2);
                    R[30] = (WORD)(res >> 32);
                    R[reg2] = (WORD)res;
                    break;
                }
                case V810_OP_DIVU: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    R[30] = reg2_val % reg1_val;
                    WORD res = reg2_val / reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    psw = (psw & ~0x7) | z | (s << 1);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_OR: {
                    WORD res = (reg2 ? R[reg2] : 0) | reg1_val;
                    psw = (psw & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_AND: {
                    WORD res = (reg2 ? R[reg2] : 0) & reg1_val;
                    psw = (psw & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_XOR: {
                    WORD res = (reg2 ? R[reg2] : 0) ^ reg1_val;
                    psw = (psw & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_NOT: {
                    WORD res = ~reg1_val;
                    psw = (psw & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_MOV_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    R[reg2] = imm;
                    break;
                }
                case V810_OP_ADD_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    WORD res = reg2_val + imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg2_val ^ imm) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg2_val;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_SETF: {
                    R[reg2] = COND(reg1, psw);
                    break;
                }
                case V810_OP_CMP_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    WORD res = reg2_val - imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)((reg2_val ^ imm) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)reg2_val < (unsigned)imm;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    if (opcode == V810_OP_SUB) R[reg2] = res;
                    break;
                }
                case V810_OP_SHL_I: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    WORD res = reg2_val << reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (32 - reg1)) & 1 : 0;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_SHR_I: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    WORD res = reg2_val >> reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (reg1 - 1)) & 1 : 0;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_CLI:
                    psw &= ~(1 << 12);
                    break;
                case V810_OP_SAR_I: {
                    WORD reg2_val = reg2 ? R[reg2] : 0;
                    WORD res = (SWORD)reg2_val >> reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (reg1 - 1)) & 1 : 0;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_RETI:
                    if (psw & PSW_NP) {
                        PC = S[FEPC];
                        psw = S[FEPSW];
                    } else {
                        PC = S[EIPC];
                        psw = S[EIPSW];
                    }
                    break;
                case V810_OP_HALT: {
                    cycles = target;
                    st->PC = PC;
                    S[PSW] = psw;
                    do {
                        cycles += st->cycles_until_event_partial;
                        st->cycles_until_event_partial = st->cycles_until_event_full = 0;
                        st->cycles = cycles;
                        serviceInt(cycles, PC);
                    } while (!st->ret && st->PC == PC);
                    if (st->PC == PC) {
                        st->PC = last_PC;
                    }
                    return 0;
                }
                case V810_OP_LDSR:
                    S[reg1] = (reg2 ? R[reg2] : 0);
                    if (reg1 == PSW) psw = S[PSW];
                    break;
                case V810_OP_STSR:
                    R[reg2] = reg1 == PSW ? psw : S[reg1];
                    break;
                case V810_OP_SEI:
                    psw |= 1 << 12;
                    break;
                case V810_OP_BSTR: {
                    typedef bool (*bstr_func)(WORD,WORD,WORD,WORD);
                    bstr_func func = (bstr_func)bssuboptable[reg1].func;
                    WORD lastarg = reg1 < 4 ? R[27] & 31 : ((R[27] & 31)) | ((R[26] & 31) << 5) | ((target - cycles) << 10);
                    WORD res = func(R[30], R[29], R[28], lastarg);
                    if (reg1 < 4) {
                        psw = (psw & ~1) | !res;
                    } else {
                        cycles += res;
                        if (R[28]) {
                            PC = last_PC;
                        }
                    }
                    break;
                }
                default: {
                    st->PC = last_PC;
                    RET(DRC_ERR_BAD_INST);
                }
            }
        } else if (opcode < 0x28) {
            if (COND(instr >> 9, psw)) {
                SHWORD disp = instr & (1 << 8) ? (instr | 0xfe00) : (instr & ~0xfe00);
                PC += disp - 2;
            } else {
                cycles -= 2;
            }
        } else {
            HWORD instr2 = FETCH16(PC);
            PC += 2;
            switch (opcode) {
                case V810_OP_MOVEA: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    R[reg2] = reg1_val + (SHWORD)instr2;
                    break;
                }
                case V810_OP_ADDI: {
                    WORD reg1_val = reg1 ? R[reg1] : 0;
                    WORD imm = (SHWORD)instr2;
                    WORD res = reg1_val + imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg1_val ^ imm) & (reg1_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg1_val;
                    psw = (psw & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_JAL:
                    R[31] = PC;
                case V810_OP_JR: {
                    SWORD disp = instr2 | ((SWORD)instr << 16);
                    if (disp & 0x02000000) disp |= 0xfc000000;
                    else disp &= ~(0xfc000000);
                    PC += disp - 4;
                    break;
                }
                case V810_OP_ORI: {
                    WORD res = instr2;
                    if (reg1) res |= R[reg1];
                    psw = (psw & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_ANDI: {
                    WORD res = 0;
                    if (reg1) res = R[reg1] & instr2;
                    psw = (psw & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_XORI: {
                    WORD res = instr2;
                    if (reg1) res ^= R[reg1];
                    psw = (psw & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    R[reg2] = res;
                    break;
                }
                case V810_OP_MOVHI: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    R[reg2] = reg1_val + ((WORD)instr2 << 16);
                    break;
                }
                case V810_OP_LD_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    R[reg2] = (SBYTE)RD8(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_LD_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    R[reg2] = (SHWORD)RD16(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_LD_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    R[reg2] = RD32(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        cycles -= 4;
                    }
                    break;
                }
                case V810_OP_IN_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    R[reg2] = RD8(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_IN_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    R[reg2] = RD16(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_IN_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    R[reg2] = RD32(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        cycles -= 4;
                    }
                    break;
                }
                case V810_OP_ST_B: case V810_OP_OUT_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    BYTE reg2_val = 0;
                    if (reg2) reg2_val = R[reg2];
                    WR8(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        cycles += 1;
                    }
                    break;
                }
                case V810_OP_ST_H: case V810_OP_OUT_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    HWORD reg2_val = 0;
                    if (reg2) reg2_val = R[reg2];
                    WR16(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        cycles += 1;
                    }
                    break;
                }
                case V810_OP_ST_W: case V810_OP_OUT_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = R[reg1];
                    WORD reg2_val = 0;
                    if (reg2) reg2_val = R[reg2];
                    WR32(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        cycles += 3;
                    }
                    break;
                }
                case V810_OP_FPP: {
                    int subop = instr2 >> 10;
                    #pragma GCC diagnostic push
                    #pragma GCC diagnostic ignored "-Wstrict-aliasing"
                    if (subop == V810_OP_CVT_WS) {
                        float res = reg1 ? (float)(SWORD)R[reg1] : 0;
                        bool z = res == 0;
                        int scy = res < 0 ? 0xa : 0;
                        psw = (psw & ~0xf) | z | scy;
                        *(float*)&R[reg2] = res;
                    } else if (!(subop & 8) || subop == V810_OP_TRNC_SW) {
                        float reg1_val = reg1 ? *(float*)&R[reg1] : 0;
                        if (subop == V810_OP_CVT_SW) {
                            SWORD res = round(reg1_val);
                            bool z = res == 0;
                            int scy = res < 0 ? 2 : 0;
                            psw = (psw & ~0xf) | z | scy;
                            R[reg2] = res;
                        } else if (subop == V810_OP_TRNC_SW) {
                            SWORD res = (SWORD)(reg1_val);
                            bool z = res == 0;
                            int scy = res < 0 ? 2 : 0;
                            psw = (psw & ~0xf) | z | scy;
                            R[reg2] = res;
                        } else {
                            float reg2_val = reg2 ? *(float*)&R[reg2] : 0;
                            float res;
                            switch (subop) {
                                case V810_OP_ADDF_S:
                                    res = reg2_val + reg1_val;
                                    break;
                                case V810_OP_CMPF_S:
                                case V810_OP_SUBF_S:
                                    res = reg2_val - reg1_val;
                                    break;
                                case V810_OP_MULF_S:
                                    res = reg2_val * reg1_val;
                                    break;
                                case V810_OP_DIVF_S:
                                    res = reg2_val / reg1_val;
                                    break;
                                default:
                                    RET(DRC_ERR_BAD_INST);
                            }
                            bool z = res == 0;
                            int scy = res < 0 ? 0xa : 0;
                            psw = (psw & ~0xf) | z | scy;
                            if (subop != V810_OP_CMPF_S) *(float*)&R[reg2] = res;
                        }
                    } else {
                        WORD reg2_val = reg2 ? R[reg2] : 0;
                        switch (subop) {
                            case V810_OP_MPYHW:
                                R[reg2] *= reg1 ? (int)(R[reg1] << 15) >> 15 : 0;
                                break;
                            case V810_OP_REV:
                                R[reg2] = reg1 ? ins_rev(R[reg1]) : 0;
                                break;
                            case V810_OP_XB:
                                R[reg2] = (reg2_val & 0xFFFF0000) | ((reg2_val << 8) & 0xFF00) | ((reg2_val >> 8) & 0xFF);
                                break;
                            case V810_OP_XH:
                                R[reg2] = (reg2_val << 16) | (reg2_val >> 16);
                                break;
                            default:
                                RET(DRC_ERR_BAD_INST);
                        }
                    }
                    #pragma GCC diagnostic pop
                    break;
                }
                default: {
                    st->PC = last_PC;
                    RET(DRC_ERR_BAD_INST);
                }
            }
        }
        last_opcode = opcode;
        if ((PC & 0x07000000) < 0x05000000) {
            st->PC = last_PC;
            RET(DRC_ERR_BAD_PC);
        }
        last_PC = PC;
    } while (!DRC_AVAILABLE || !drc_ready || (PC & 0x07000000) != 0x07000000);
    S[PSW] = psw;
    st->PC = PC;
    st->cycles = cycles;
    st->cycles_until_event_full = st->cycles_until_event_partial = (int)(target - cycles);
    return 0;
}
