#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>

#include "drc_core.h"

#if DRC_AVAILABLE

#include "v810_cpu.h"
#include "v810_mem.h"
#include "v810_ins.h"
#include "v810_opt.h"
#include "vb_types.h"

#ifdef DRC_HOST_SIM
void sim_enter(WORD stub, WORD entry, WORD cpsr);
void sim_unwind(void);
void *sim_alloc(size_t size);
#elif defined(__vita__)
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
void vita_log(const char *fmt, ...);
#endif

enum {
    R_AND = 0, R_EOR = 1, R_SUB = 2, R_RSB = 3, R_ADD = 4, R_TST = 8, R_TEQ = 9,
    R_CMP = 10, R_CMN = 11, R_ORR = 12, R_MOV = 13, R_BIC = 14, R_MVN = 15
};

enum {
    C_EQ = 0, C_NE = 1, C_CS = 2, C_CC = 3, C_MI = 4, C_PL = 5, C_VS = 6, C_VC = 7,
    C_HI = 8, C_LS = 9, C_GE = 10, C_LT = 11, C_GT = 12, C_LE = 13, C_AL = 14
};

enum {
    SH_LSL = 0, SH_LSR = 1, SH_ASR = 2
};

enum {
    H_RBYTE, H_RHWORD, H_RWORD, H_WBYTE, H_WHWORD, H_WWORD, H_SLOW, H_BSTR,
    H_STSR_PSW, H_LDSR_PSW, H_RETI, H_EXIT, H_RESOLVE, H_COUNT
};

#define SP 13
#define LR 14
#define PCR 15
#define ST 11
#define SCR_FLAGS 4
#define SCR_FN 12

#define OFF_P(i) ((i) * 4)
#define OFF_S(i) (128 + (i) * 4)
#define OFF_PC offsetof(cpu_state, PC)
#define OFF_PARTIAL offsetof(cpu_state, cycles_until_event_partial)
#define OFF_IRQ offsetof(cpu_state, irq_handler)
#define OFF_RELOC offsetof(cpu_state, reloc_table)

bool drc_ready = false;

static WORD *cache_base;
static WORD *cache_end;
static WORD *blocks_start;
static WORD *ep;

static WORD *stub_enter;
static WORD *stub_resolve;
static WORD *stub_leave;
static WORD *stub_dyn;
static WORD *stub_unwind;

static WORD *entry_map;
static WORD map_count;
static WORD rom_mask;

static struct {
    WORD map;
    WORD mask;
} dyn_ctx;

static WORD saved_sp;
static WORD helper_tab[H_COUNT];

#ifdef __vita__
static SceUID cache_uid = -1;
#endif

static size_t cache_size;
static WORD flush_count;
static bool sync_failed_logged;

static int t_cyc;
static int t_inv;
static int t_vz;
static BYTE t_last;

static inline WORD cpsr_from_psw(WORD psw) {
    return ((psw & 1) << 30) | ((psw & 2) << 30) | ((psw & 4) << 26) | ((psw & 8) << 26);
}

static inline WORD psw_from_cpsr(WORD c) {
    return ((c >> 30) & 1) | ((c >> 30) & 2) | ((c >> 26) & 4) | ((c >> 26) & 8);
}

static inline void E(WORD w) {
    *ep++ = w;
}

static bool enc_imm(WORD v, WORD *enc) {
    for (int rot = 0; rot < 16; rot++) {
        WORD r = rot ? ((v << (rot * 2)) | (v >> (32 - rot * 2))) : v;
        if (r <= 0xFF) {
            *enc = ((WORD)rot << 8) | r;
            return true;
        }
    }
    return false;
}

static inline WORD i_dpi(int cond, int op, int s, int rn, int rd, WORD enc) {
    return ((WORD)cond << 28) | (1u << 25) | ((WORD)op << 21) | ((WORD)s << 20) | ((WORD)rn << 16) | ((WORD)rd << 12) | enc;
}

static inline WORD i_dpr(int cond, int op, int s, int rn, int rd, int rm, int type, int amt) {
    return ((WORD)cond << 28) | ((WORD)op << 21) | ((WORD)s << 20) | ((WORD)rn << 16) | ((WORD)rd << 12) | ((WORD)amt << 7) | ((WORD)type << 5) | (WORD)rm;
}

static inline WORD i_dprs(int cond, int op, int s, int rn, int rd, int rm, int type, int rs) {
    return ((WORD)cond << 28) | ((WORD)op << 21) | ((WORD)s << 20) | ((WORD)rn << 16) | ((WORD)rd << 12) | ((WORD)rs << 8) | ((WORD)type << 5) | (1u << 4) | (WORD)rm;
}

static inline WORD i_ldst(int cond, int load, int rd, int rn, int off) {
    WORD u = 1;
    if (off < 0) {
        u = 0;
        off = -off;
    }
    return ((WORD)cond << 28) | (0x04000000u) | (1u << 24) | (u << 23) | ((WORD)load << 20) | ((WORD)rn << 16) | ((WORD)rd << 12) | (WORD)off;
}

static inline WORD i_ldr_reg_lsl(int cond, int rd, int rn, int rm, int amt) {
    return ((WORD)cond << 28) | 0x07900000u | ((WORD)rn << 16) | ((WORD)rd << 12) | ((WORD)amt << 7) | (WORD)rm;
}

static inline WORD i_b(int cond, WORD *at, WORD *target) {
    return ((WORD)cond << 28) | 0x0A000000u | ((WORD)((target - at) - 2) & 0xFFFFFF);
}

static inline WORD i_bx(int cond, int rm) {
    return ((WORD)cond << 28) | 0x012FFF10u | (WORD)rm;
}

static inline WORD i_blx(int cond, int rm) {
    return ((WORD)cond << 28) | 0x012FFF30u | (WORD)rm;
}

static inline WORD i_mrs(int rd) {
    return 0xE10F0000u | ((WORD)rd << 12);
}

static inline WORD i_msr_f(int rm) {
    return 0xE128F000u | (WORD)rm;
}

static inline WORD i_movw(int rd, WORD v) {
    return 0xE3000000u | ((v & 0xF000) << 4) | ((WORD)rd << 12) | (v & 0xFFF);
}

static inline WORD i_movt(int rd, WORD v) {
    return 0xE3400000u | ((v & 0xF0000000u) >> 12) | ((WORD)rd << 12) | ((v >> 16) & 0xFFF);
}

static void mov_imm(int rd, WORD v) {
    WORD enc;
    if (enc_imm(v, &enc)) {
        E(i_dpi(C_AL, R_MOV, 0, 0, rd, enc));
    } else if (enc_imm(~v, &enc)) {
        E(i_dpi(C_AL, R_MVN, 0, 0, rd, enc));
    } else {
        E(i_movw(rd, v & 0xFFFF));
        if (v >> 16) E(i_movt(rd, v));
    }
}

static void mov_reg(int rd, int rm) {
    E(i_dpr(C_AL, R_MOV, 0, 0, rd, rm, SH_LSL, 0));
}

static void addsub_imm(int rd, int rn, int v, int s) {
    int op = R_ADD;
    if (v < 0) {
        op = R_SUB;
        v = -v;
    }
    WORD lo = v & 0xFF;
    WORD mid = v & 0xFF00;
    if (!lo && !mid) {
        if (s) E(i_dpi(C_AL, R_ADD, 1, rn, rd, 0));
        else if (rd != rn) mov_reg(rd, rn);
        return;
    }
    if (lo && mid) {
        E(i_dpi(C_AL, op, 0, rn, rd, lo));
        E(i_dpi(C_AL, op, s, rd, rd, (12u << 8) | (mid >> 8)));
    } else if (lo) {
        E(i_dpi(C_AL, op, s, rn, rd, lo));
    } else {
        E(i_dpi(C_AL, op, s, rn, rd, (12u << 8) | (mid >> 8)));
    }
}

static void patch_b(WORD *at, WORD *target) {
    *at = (*at & 0xFF000000u) | ((WORD)((target - at) - 2) & 0xFFFFFF);
}

static void call_fn(int idx) {
    E(i_ldst(C_AL, 1, SCR_FN, ST, OFF_RELOC));
    E(i_ldst(C_AL, 1, SCR_FN, SCR_FN, idx * 4));
    E(i_blx(C_AL, SCR_FN));
}

static void ld_v(int arm, int v) {
    if (v == 0) mov_imm(arm, 0);
    else E(i_ldst(C_AL, 1, arm, ST, OFF_P(v)));
}

static void st_v(int arm, int v) {
    if (v != 0) E(i_ldst(C_AL, 0, arm, ST, OFF_P(v)));
}

static void flush_icache(WORD *start, WORD *end) {
#if defined(DRC_HOST_SIM)
    (void)start; (void)end;
#elif defined(__vita__)
    uintptr_t base = (uintptr_t)cache_base;
    uintptr_t lo = ((uintptr_t)start - base) & ~(uintptr_t)0xFFF;
    uintptr_t hi = (((uintptr_t)end - base) + 0xFFF) & ~(uintptr_t)0xFFF;
    if (hi > cache_size) hi = cache_size;
    int r = sceKernelSyncVMDomain(cache_uid, (void *)(base + lo), (SceSize)(hi - lo));
    if (r < 0 && !sync_failed_logged) {
        sync_failed_logged = true;
        vita_log("drc: sync failed 0x%08x", r);
    }
#else
    __builtin___clear_cache((char *)start, (char *)end);
#endif
}

static inline WORD map_index(WORD pc) {
    return (pc & rom_mask) >> 1;
}

static void map_reset(void) {
    for (WORD i = 0; i < map_count; i++) entry_map[i] = (WORD)(uintptr_t)stub_resolve;
}

static void norm_flags(void) {
    if (!t_inv && !t_vz) return;
    WORD enc;
    E(i_mrs(0));
    if (t_inv) {
        enc_imm(0x20000000u, &enc);
        E(i_dpi(C_AL, R_EOR, 0, 0, 0, enc));
    }
    if (t_vz) {
        enc_imm(0x10000000u, &enc);
        E(i_dpi(C_AL, R_BIC, 0, 0, 0, enc));
    }
    E(i_msr_f(0));
    t_inv = 0;
    t_vz = 0;
}

static void flush_cycles(void) {
    if (!t_cyc) return;
    E(i_ldst(C_AL, 1, 2, ST, OFF_PARTIAL));
    addsub_imm(2, 2, -t_cyc, 0);
    E(i_ldst(C_AL, 0, 2, ST, OFF_PARTIAL));
    t_cyc = 0;
}

static void event_check(int cyc, bool dynamic, WORD pc) {
    if (dynamic) mov_reg(4, 1);
    E(i_mrs(0));
    E(i_ldst(C_AL, 1, 2, ST, OFF_PARTIAL));
    addsub_imm(2, 2, -cyc, 1);
    E(i_ldst(C_AL, 0, 2, ST, OFF_PARTIAL));
    if (!dynamic) mov_imm(1, pc);
    E(i_ldst(C_LE, 1, 3, ST, OFF_IRQ));
    E(i_blx(C_LE, 3));
    E(i_msr_f(0));
    if (dynamic) mov_reg(1, 4);
}

static void jump_target(WORD target) {
    if ((target & 0x07000000) == 0x07000000) {
        WORD idx = map_index(target);
        WORD *e = (WORD *)(uintptr_t)entry_map[idx];
        if (e != stub_resolve) {
            E(i_b(C_AL, ep, e));
        } else {
            mov_imm(0, (WORD)(uintptr_t)&entry_map[idx]);
            E(i_ldst(C_AL, 1, PCR, 0, 0));
        }
    } else {
        E(i_mrs(0));
        E(i_b(C_AL, ep, stub_leave));
    }
}

static void exit_const(WORD target, int cyc, bool checked) {
    norm_flags();
    if (checked) {
        event_check(cyc, false, target);
        mov_imm(1, target);
    } else {
        mov_imm(1, target);
    }
    if (!checked && cyc) {
        E(i_ldst(C_AL, 1, 2, ST, OFF_PARTIAL));
        addsub_imm(2, 2, -cyc, 0);
        E(i_ldst(C_AL, 0, 2, ST, OFF_PARTIAL));
    }
    jump_target(target);
}

static void exit_dyn(int cyc) {
    norm_flags();
    event_check(cyc, true, 0);
    E(i_b(C_AL, ep, stub_dyn));
}

typedef struct {
    int kind;
    int c1;
    int c2;
} condplan;

enum { K_NEVER, K_ALWAYS, K_SINGLE, K_OR, K_AND };

static condplan plan_cond(int c) {
    condplan p = { K_SINGLE, 0, 0 };
    int cy_set = t_inv ? C_CC : C_CS;
    int cy_clr = t_inv ? C_CS : C_CC;
    switch (c & 15) {
        case 0: if (t_vz) p.kind = K_NEVER; else p.c1 = C_VS; break;
        case 1: p.c1 = cy_set; break;
        case 2: p.c1 = C_EQ; break;
        case 3:
            if (t_inv) p.c1 = C_LS;
            else { p.kind = K_OR; p.c1 = C_CS; p.c2 = C_EQ; }
            break;
        case 4: p.c1 = C_MI; break;
        case 5: p.kind = K_ALWAYS; break;
        case 6: p.c1 = t_vz ? C_MI : C_LT; break;
        case 7:
            if (t_vz) { p.kind = K_OR; p.c1 = C_MI; p.c2 = C_EQ; }
            else p.c1 = C_LE;
            break;
        case 8: if (t_vz) p.kind = K_ALWAYS; else p.c1 = C_VC; break;
        case 9: p.c1 = cy_clr; break;
        case 10: p.c1 = C_NE; break;
        case 11:
            if (t_inv) p.c1 = C_HI;
            else { p.kind = K_AND; p.c1 = C_CC; p.c2 = C_NE; }
            break;
        case 12: p.c1 = C_PL; break;
        case 13: p.kind = K_NEVER; break;
        case 14: p.c1 = t_vz ? C_PL : C_GE; break;
        case 15:
            if (t_vz) { p.kind = K_AND; p.c1 = C_PL; p.c2 = C_NE; }
            else p.c1 = C_GT;
            break;
    }
    return p;
}

static int emit_branch_if(condplan p, WORD **fix) {
    int n = 0;
    switch (p.kind) {
        case K_SINGLE:
            fix[n++] = ep;
            E(i_b(p.c1, ep, ep));
            break;
        case K_OR:
            fix[n++] = ep;
            E(i_b(p.c1, ep, ep));
            fix[n++] = ep;
            E(i_b(p.c2, ep, ep));
            break;
        case K_AND: {
            WORD *skip = ep;
            E(i_b(p.c1 ^ 1, ep, ep));
            fix[n++] = ep;
            E(i_b(p.c2, ep, ep));
            patch_b(skip, ep);
            break;
        }
    }
    return n;
}

static HWORD fetch16(WORD pc) {
    return *(HWORD *)(V810_ROM1.off + (pc & (0x07000000 | (MAX_ROM_SIZE - 1)) & ~1));
}

typedef enum { T_CONT, T_END, T_BAD } tres;

static bool fpp_valid(int subop) {
    if (subop == V810_OP_CVT_WS) return true;
    if (!(subop & 8) || subop == V810_OP_TRNC_SW) {
        switch (subop) {
            case V810_OP_CVT_SW: case V810_OP_TRNC_SW: case V810_OP_ADDF_S: case V810_OP_CMPF_S:
            case V810_OP_SUBF_S: case V810_OP_MULF_S: case V810_OP_DIVF_S:
                return true;
        }
        return false;
    }
    switch (subop) {
        case V810_OP_MPYHW: case V810_OP_REV: case V810_OP_XB: case V810_OP_XH:
            return true;
    }
    return false;
}

static void emit_mem_load(int helper, int reg1, int reg2, SWORD disp, int ext) {
    flush_cycles();
    if (reg1) {
        E(i_ldst(C_AL, 1, 0, ST, OFF_P(reg1)));
        addsub_imm(0, 0, disp, 0);
    } else {
        mov_imm(0, (WORD)(SWORD)disp);
    }
    E(i_mrs(SCR_FLAGS));
    call_fn(helper);
    E(i_msr_f(SCR_FLAGS));
    switch (ext) {
        case 1:
            E(i_dpr(C_AL, R_MOV, 0, 0, 0, 0, SH_LSL, 24));
            E(i_dpr(C_AL, R_MOV, 0, 0, 0, 0, SH_ASR, 24));
            break;
        case 2:
            E(i_dpr(C_AL, R_MOV, 0, 0, 0, 0, SH_LSL, 16));
            E(i_dpr(C_AL, R_MOV, 0, 0, 0, 0, SH_ASR, 16));
            break;
        case 3:
            E(i_dpi(C_AL, R_AND, 0, 0, 0, 0xFF));
            break;
        case 4:
            E(i_dpr(C_AL, R_MOV, 0, 0, 0, 0, SH_LSL, 16));
            E(i_dpr(C_AL, R_MOV, 0, 0, 0, 0, SH_LSR, 16));
            break;
    }
    st_v(0, reg2);
}

static void emit_mem_store(int helper, int reg1, int reg2, SWORD disp, int ext) {
    flush_cycles();
    if (reg1) {
        E(i_ldst(C_AL, 1, 0, ST, OFF_P(reg1)));
        addsub_imm(0, 0, disp, 0);
    } else {
        mov_imm(0, (WORD)(SWORD)disp);
    }
    ld_v(1, reg2);
    if (ext == 1) {
        E(i_dpi(C_AL, R_AND, 0, 1, 1, 0xFF));
    } else if (ext == 2) {
        E(i_dpr(C_AL, R_MOV, 0, 0, 1, 1, SH_LSL, 16));
        E(i_dpr(C_AL, R_MOV, 0, 0, 1, 1, SH_LSR, 16));
    }
    E(i_mrs(SCR_FLAGS));
    call_fn(helper);
    E(i_msr_f(SCR_FLAGS));
}

static int load_adjust(int width_word) {
    if ((t_last & 0x34) == 0x30 && (t_last & 3) != 2) return -1;
    if (opcycle[t_last] > 4) return width_word ? -4 : -2;
    return 0;
}

static int store_adjust(int width_word) {
    if ((t_last & 0x34) == 0x34 && (t_last & 3) != 2) return width_word ? 3 : 1;
    return 0;
}

static void shift_reg(int type, int reg1, int reg2) {
    ld_v(1, reg1);
    E(i_dpi(C_AL, R_AND, 0, 1, 1, 31));
    ld_v(2, reg2);
    E(i_dpi(C_AL, R_CMN, 1, 1, 0, 0));
    E(i_dprs(C_AL, R_MOV, 1, 0, 2, 2, type, 1));
    st_v(2, reg2);
    t_inv = 0;
    t_vz = 0;
}

static void shift_imm(int type, int n, int reg2) {
    ld_v(2, reg2);
    E(i_dpi(C_AL, R_CMN, 1, 2, 0, 0));
    if (n == 0) type = SH_LSL;
    E(i_dpr(C_AL, R_MOV, 1, 0, 2, 2, type, n));
    st_v(2, reg2);
    t_inv = 0;
    t_vz = 0;
}

static void alu_rr(int op, int s, int store, int reg1, int reg2) {
    ld_v(1, reg1);
    ld_v(2, reg2);
    E(i_dpr(C_AL, op, s, 2, store ? 2 : 0, 1, SH_LSL, 0));
    if (store) st_v(2, reg2);
}

static tres tr_inst(WORD pc, WORD *next) {
    HWORD ins = fetch16(pc);
    BYTE opcode = ins >> 10;
    int reg1 = ins & 31;
    int reg2 = (ins >> 5) & 31;
    int save_cyc = t_cyc;
    *next = pc + 2;

    if (opcode < 0x20) {
        t_cyc += opcycle[opcode];
        switch (opcode) {
            case V810_OP_MOV:
                ld_v(1, reg1);
                st_v(1, reg2);
                break;
            case V810_OP_ADD:
                alu_rr(R_ADD, 1, 1, reg1, reg2);
                t_inv = 0; t_vz = 0;
                break;
            case V810_OP_SUB:
                alu_rr(R_SUB, 1, 1, reg1, reg2);
                t_inv = 1; t_vz = 0;
                break;
            case V810_OP_CMP:
                alu_rr(R_CMP, 1, 0, reg1, reg2);
                t_inv = 1; t_vz = 0;
                break;
            case V810_OP_SHL: shift_reg(SH_LSL, reg1, reg2); break;
            case V810_OP_SHR: shift_reg(SH_LSR, reg1, reg2); break;
            case V810_OP_SAR: shift_reg(SH_ASR, reg1, reg2); break;
            case V810_OP_JMP:
                ld_v(1, reg1);
                exit_dyn(t_cyc);
                t_cyc = 0;
                t_last = opcode;
                return T_END;
            case V810_OP_MUL: case V810_OP_DIV: case V810_OP_MULU: case V810_OP_DIVU:
                flush_cycles();
                norm_flags();
                E(i_mrs(SCR_FLAGS));
                mov_reg(0, SCR_FLAGS);
                mov_imm(1, ins);
                mov_imm(2, 0);
                call_fn(H_SLOW);
                E(i_msr_f(0));
                break;
            case V810_OP_OR:
                alu_rr(R_ORR, 1, 1, reg1, reg2);
                t_vz = 1;
                break;
            case V810_OP_AND:
                alu_rr(R_AND, 1, 1, reg1, reg2);
                t_vz = 1;
                break;
            case V810_OP_XOR:
                alu_rr(R_EOR, 1, 1, reg1, reg2);
                t_vz = 1;
                break;
            case V810_OP_NOT:
                ld_v(1, reg1);
                E(i_dpr(C_AL, R_MVN, 1, 0, 2, 1, SH_LSL, 0));
                st_v(2, reg2);
                t_vz = 1;
                break;
            case V810_OP_MOV_I:
                mov_imm(1, (WORD)(SWORD)(reg1 & 0x10 ? reg1 | 0xFFFFFFF0 : reg1));
                st_v(1, reg2);
                break;
            case V810_OP_ADD_I:
                mov_imm(1, (WORD)(SWORD)(reg1 & 0x10 ? reg1 | 0xFFFFFFF0 : reg1));
                ld_v(2, reg2);
                E(i_dpr(C_AL, R_ADD, 1, 2, 2, 1, SH_LSL, 0));
                st_v(2, reg2);
                t_inv = 0; t_vz = 0;
                break;
            case V810_OP_CMP_I:
                mov_imm(1, (WORD)(SWORD)(reg1 & 0x10 ? reg1 | 0xFFFFFFF0 : reg1));
                ld_v(2, reg2);
                E(i_dpr(C_AL, R_CMP, 1, 2, 0, 1, SH_LSL, 0));
                t_inv = 1; t_vz = 0;
                break;
            case V810_OP_SETF: {
                if (reg1 & 0x10) {
                    mov_imm(1, (reg1 & 8) ? 1 : 0);
                } else {
                    condplan p = plan_cond(reg1);
                    mov_imm(1, p.kind == K_ALWAYS ? 1 : 0);
                    if (p.kind == K_SINGLE) {
                        E(i_dpi(p.c1, R_MOV, 0, 0, 1, 1));
                    } else if (p.kind == K_OR) {
                        E(i_dpi(p.c1, R_MOV, 0, 0, 1, 1));
                        E(i_dpi(p.c2, R_MOV, 0, 0, 1, 1));
                    } else if (p.kind == K_AND) {
                        E(i_dpi(p.c1, R_MOV, 0, 0, 1, 1));
                        E(i_dpi(p.c2 ^ 1, R_MOV, 0, 0, 1, 0));
                    }
                }
                st_v(1, reg2);
                break;
            }
            case V810_OP_SHL_I: shift_imm(SH_LSL, reg1, reg2); break;
            case V810_OP_SHR_I: shift_imm(SH_LSR, reg1, reg2); break;
            case V810_OP_SAR_I: shift_imm(SH_ASR, reg1, reg2); break;
            case V810_OP_CLI: case V810_OP_SEI: {
                WORD enc;
                enc_imm(1u << 12, &enc);
                E(i_ldst(C_AL, 1, 0, ST, OFF_S(PSW)));
                E(i_dpi(C_AL, opcode == V810_OP_CLI ? R_BIC : R_ORR, 0, 0, 0, enc));
                E(i_ldst(C_AL, 0, 0, ST, OFF_S(PSW)));
                break;
            }
            case V810_OP_RETI:
                flush_cycles();
                call_fn(H_RETI);
                E(i_msr_f(0));
                E(i_ldst(C_AL, 1, 1, ST, OFF_PC));
                t_inv = 0; t_vz = 0;
                exit_dyn(0);
                t_last = opcode;
                return T_END;
            case V810_OP_HALT:
                flush_cycles();
                norm_flags();
                mov_imm(1, (pc + 2) | 1);
                E(i_mrs(0));
                E(i_ldst(C_AL, 1, 3, ST, OFF_IRQ));
                E(i_blx(C_AL, 3));
                mov_imm(1, pc + 2);
                E(i_mrs(0));
                E(i_b(C_AL, ep, stub_leave));
                t_last = opcode;
                return T_END;
            case V810_OP_LDSR:
                ld_v(2, reg2);
                if (reg1 == PSW) {
                    mov_reg(0, 2);
                    call_fn(H_LDSR_PSW);
                    E(i_msr_f(0));
                    t_inv = 0; t_vz = 0;
                } else {
                    E(i_ldst(C_AL, 0, 2, ST, OFF_S(reg1)));
                }
                break;
            case V810_OP_STSR:
                if (reg1 == PSW) {
                    norm_flags();
                    E(i_mrs(SCR_FLAGS));
                    mov_reg(0, SCR_FLAGS);
                    call_fn(H_STSR_PSW);
                    E(i_msr_f(SCR_FLAGS));
                    st_v(0, reg2);
                } else {
                    E(i_ldst(C_AL, 1, 0, ST, OFF_S(reg1)));
                    st_v(0, reg2);
                }
                break;
            case V810_OP_BSTR:
                if (reg1 > 15 || (reg1 >= 4 && reg1 <= 7)) {
                    t_cyc = save_cyc;
                    return T_BAD;
                }
                flush_cycles();
                norm_flags();
                E(i_mrs(0));
                mov_imm(1, reg1);
                mov_imm(2, pc);
                mov_imm(3, pc + 2);
                call_fn(H_BSTR);
                E(i_msr_f(0));
                exit_dyn(0);
                t_last = opcode;
                return T_END;
            default:
                t_cyc = save_cyc;
                return T_BAD;
        }
        t_last = opcode;
        return T_CONT;
    }

    if (opcode < 0x28) {
        SHWORD disp = ins & (1 << 8) ? (ins | 0xFE00) : (ins & ~0xFE00);
        WORD target = pc + disp;
        t_cyc += opcycle[opcode];
        condplan p = plan_cond((ins >> 9) & 15);
        if (p.kind == K_NEVER) {
            t_cyc -= 2;
            t_last = opcode;
            return T_CONT;
        }
        if (p.kind == K_ALWAYS) {
            exit_const(target, t_cyc, target <= pc);
            t_cyc = 0;
            t_last = opcode;
            return T_END;
        }
        WORD *fix[2];
        int nfix = emit_branch_if(p, fix);
        int sv_inv = t_inv, sv_vz = t_vz;
        exit_const(pc + 2, t_cyc - 2, false);
        t_inv = sv_inv; t_vz = sv_vz;
        for (int i = 0; i < nfix; i++) patch_b(fix[i], ep);
        exit_const(target, t_cyc, target <= pc);
        t_cyc = 0;
        t_last = opcode;
        return T_END;
    }

    HWORD ins2 = fetch16(pc + 2);
    *next = pc + 4;
    t_cyc += opcycle[opcode];
    switch (opcode) {
        case V810_OP_MOVEA:
            if (reg1) {
                ld_v(1, reg1);
                addsub_imm(1, 1, (SHWORD)ins2, 0);
            } else {
                mov_imm(1, (WORD)(SWORD)(SHWORD)ins2);
            }
            st_v(1, reg2);
            break;
        case V810_OP_ADDI:
            ld_v(1, reg1);
            mov_imm(2, (WORD)(SWORD)(SHWORD)ins2);
            E(i_dpr(C_AL, R_ADD, 1, 1, 1, 2, SH_LSL, 0));
            st_v(1, reg2);
            t_inv = 0; t_vz = 0;
            break;
        case V810_OP_JAL:
            mov_imm(0, pc + 4);
            E(i_ldst(C_AL, 0, 0, ST, OFF_P(31)));
            /* fallthrough */
        case V810_OP_JR: {
            SWORD disp = ins2 | ((SWORD)ins << 16);
            if (disp & 0x02000000) disp |= 0xFC000000;
            else disp &= ~0xFC000000;
            WORD target = pc + disp;
            exit_const(target, t_cyc, target <= pc);
            t_cyc = 0;
            t_last = opcode;
            return T_END;
        }
        case V810_OP_ORI:
        case V810_OP_ANDI:
        case V810_OP_XORI:
            ld_v(1, reg1);
            mov_imm(2, ins2);
            E(i_dpr(C_AL, opcode == V810_OP_ORI ? R_ORR : opcode == V810_OP_ANDI ? R_AND : R_EOR, 1, 1, 1, 2, SH_LSL, 0));
            st_v(1, reg2);
            t_vz = 1;
            break;
        case V810_OP_MOVHI:
            if (reg1) {
                ld_v(1, reg1);
                mov_imm(2, (WORD)ins2 << 16);
                E(i_dpr(C_AL, R_ADD, 0, 1, 1, 2, SH_LSL, 0));
            } else {
                mov_imm(1, (WORD)ins2 << 16);
            }
            st_v(1, reg2);
            break;
        case V810_OP_LD_B: case V810_OP_LD_H: case V810_OP_LD_W:
        case V810_OP_IN_B: case V810_OP_IN_H: case V810_OP_IN_W: {
            int w = (opcode & 3) == 3;
            t_cyc += load_adjust(w);
            int kind = opcode & 3;
            int helper = kind == 0 ? H_RBYTE : kind == 1 ? H_RHWORD : H_RWORD;
            int ext;
            if (opcode < 0x38) ext = kind == 0 ? 1 : kind == 1 ? 2 : 0;
            else ext = kind == 0 ? 3 : kind == 1 ? 4 : 0;
            emit_mem_load(helper, reg1, reg2, (SHWORD)ins2, ext);
            break;
        }
        case V810_OP_ST_B: case V810_OP_ST_H: case V810_OP_ST_W:
        case V810_OP_OUT_B: case V810_OP_OUT_H: case V810_OP_OUT_W: {
            int kind = opcode & 3;
            t_cyc += store_adjust(kind == 3);
            int helper = kind == 0 ? H_WBYTE : kind == 1 ? H_WHWORD : H_WWORD;
            emit_mem_store(helper, reg1, reg2, (SHWORD)ins2, kind == 0 ? 1 : kind == 1 ? 2 : 0);
            break;
        }
        case V810_OP_FPP:
            if (!fpp_valid(ins2 >> 10)) {
                t_cyc = save_cyc;
                return T_BAD;
            }
            flush_cycles();
            norm_flags();
            E(i_mrs(SCR_FLAGS));
            mov_reg(0, SCR_FLAGS);
            mov_imm(1, ins);
            mov_imm(2, ins2);
            call_fn(H_SLOW);
            E(i_msr_f(0));
            break;
        default:
            t_cyc = save_cyc;
            return T_BAD;
    }
    t_last = opcode;
    return T_CONT;
}

static WORD *translate(WORD pc) {
    WORD *start = ep;
    WORD idx = map_index(pc);
    entry_map[idx] = (WORD)(uintptr_t)start;
    t_cyc = 0;
    t_inv = 0;
    t_vz = 0;
    t_last = 0;
    WORD cur = pc;
    int n = 0;
    for (;;) {
        WORD next;
        tres r = tr_inst(cur, &next);
        if (r == T_BAD) {
            if (n == 0) {
                ep = start;
                entry_map[idx] = (WORD)(uintptr_t)stub_resolve;
                return NULL;
            }
            exit_const(cur, t_cyc, false);
            break;
        }
        n++;
        cur = next;
        if (r == T_END) break;
        if (n >= DRC_MAX_V810_INST) {
            exit_const(cur, t_cyc, false);
            break;
        }
    }
    flush_icache(start, ep);
    return start;
}

static void clear_blocks(void) {
    ep = blocks_start;
    map_reset();
    flush_count++;
#ifdef __vita__
    if (flush_count > 1) vita_log("drc: cache flush %u", (unsigned)flush_count);
#endif
}

static WORD *get_entry(WORD pc) {
    WORD *e = (WORD *)(uintptr_t)entry_map[map_index(pc)];
    if (e != stub_resolve) return e;
    if ((size_t)(cache_end - ep) < DRC_BLOCK_SLACK) clear_blocks();
    return translate(pc);
}

static WORD h_resolve(WORD pc) {
    WORD *e = get_entry(pc);
    return (WORD)(uintptr_t)e;
}

static void h_exit(WORD cpsr, WORD pc) {
    cpu_state *st = &vb_state->v810_state;
    st->PC = pc;
    st->S_REG[PSW] = (st->S_REG[PSW] & ~0xF) | psw_from_cpsr(cpsr);
    st->cycles += st->cycles_until_event_full - st->cycles_until_event_partial;
    st->cycles_until_event_full = st->cycles_until_event_partial;
}

static inline void sync_time(void) {
    cpu_state *st = &vb_state->v810_state;
    st->cycles += st->cycles_until_event_full - st->cycles_until_event_partial;
    st->cycles_until_event_full = st->cycles_until_event_partial;
}

static WORD h_wbyte(WORD a, WORD d) {
    sync_time();
    return mem_wbyte(a, d);
}

static WORD h_whword(WORD a, WORD d) {
    sync_time();
    return mem_whword(a, d);
}

static WORD h_wword(WORD a, WORD d) {
    sync_time();
    return mem_wword(a, d);
}

static WORD h_ldsr_psw(WORD val) {
    vb_state->v810_state.S_REG[PSW] = val;
    return cpsr_from_psw(val);
}

static WORD h_stsr_psw(WORD cpsr) {
    return (vb_state->v810_state.S_REG[PSW] & ~0xF) | psw_from_cpsr(cpsr);
}

static WORD h_reti(WORD cpsr) {
    cpu_state *st = &vb_state->v810_state;
    (void)cpsr;
    if (st->S_REG[PSW] & PSW_NP) {
        st->PC = st->S_REG[FEPC];
        st->S_REG[PSW] = st->S_REG[FEPSW];
    } else {
        st->PC = st->S_REG[EIPC];
        st->S_REG[PSW] = st->S_REG[EIPSW];
    }
    return cpsr_from_psw(st->S_REG[PSW]);
}

static uint64_t h_bstr(WORD cpsr, WORD reg1, WORD pc, WORD next_pc) {
    cpu_state *st = &vb_state->v810_state;
    typedef bool (*bstr_func)(WORD, WORD, WORD, WORD);
    bstr_func func = (bstr_func)bssuboptable[reg1].func;
    WORD lastarg = reg1 < 4 ? st->P_REG[27] & 31 : (st->P_REG[27] & 31) | ((st->P_REG[26] & 31) << 5) | ((WORD)st->cycles_until_event_partial << 10);
    WORD res = func(st->P_REG[30], st->P_REG[29], st->P_REG[28], lastarg);
    WORD out_cpsr = cpsr;
    WORD out_pc = next_pc;
    if (reg1 < 4) {
        out_cpsr = (cpsr & ~(1u << 30)) | (res ? 0 : (1u << 30));
    } else {
        st->cycles_until_event_partial -= (int)res;
        if (st->P_REG[28]) out_pc = pc;
    }
    return (uint64_t)out_cpsr | ((uint64_t)out_pc << 32);
}

static WORD h_slow(WORD cpsr, WORD instr, WORD instr2) {
    cpu_state *st = &vb_state->v810_state;
    WORD psw = psw_from_cpsr(cpsr);
    int opcode = instr >> 10;
    int reg1 = instr & 31;
    int reg2 = (instr >> 5) & 31;
    WORD reg1_val = 0;
    if (reg1) reg1_val = st->P_REG[reg1];
    switch (opcode) {
        case V810_OP_MUL: {
            SWORD reg2_val = reg2 ? (SWORD)st->P_REG[reg2] : 0;
            int64_t res = (int64_t)(SWORD)reg1_val * (int64_t)reg2_val;
            bool ov = res != (int64_t)(int32_t)res;
            bool z = res == 0;
            bool s = res < 0;
            psw = (psw & ~0xf) | z | (s << 1) | (ov << 2);
            st->P_REG[30] = (WORD)(res >> 32);
            if (reg2) st->P_REG[reg2] = (WORD)res;
            break;
        }
        case V810_OP_DIV: {
            SWORD reg2_val = reg2 ? (SWORD)st->P_REG[reg2] : 0;
            if ((SWORD)reg1_val == 0) break;
            if (reg2_val == (SWORD)0x80000000 && (SWORD)reg1_val == -1) {
                st->P_REG[30] = 0;
                psw = (psw & ~0x7) | 6;
            } else {
                st->P_REG[30] = reg2_val % (SWORD)reg1_val;
                SWORD res = reg2_val / (SWORD)reg1_val;
                bool z = res == 0;
                bool s = res < 0;
                psw = (psw & ~0x7) | z | (s << 1);
                if (reg2) st->P_REG[reg2] = res;
            }
            break;
        }
        case V810_OP_MULU: {
            WORD reg2_val = reg2 ? st->P_REG[reg2] : 0;
            uint64_t res = (uint64_t)reg1_val * (uint64_t)reg2_val;
            bool ov = res != (uint64_t)(uint32_t)res;
            bool z = res == 0;
            bool s = (SWORD)res < 0;
            psw = (psw & ~0xf) | z | (s << 1) | (ov << 2);
            st->P_REG[30] = (WORD)(res >> 32);
            if (reg2) st->P_REG[reg2] = (WORD)res;
            break;
        }
        case V810_OP_DIVU: {
            WORD reg2_val = reg2 ? st->P_REG[reg2] : 0;
            if (reg1_val == 0) break;
            st->P_REG[30] = reg2_val % reg1_val;
            WORD res = reg2_val / reg1_val;
            bool z = res == 0;
            bool s = (SWORD)res < 0;
            psw = (psw & ~0x7) | z | (s << 1);
            if (reg2) st->P_REG[reg2] = res;
            break;
        }
        case V810_OP_FPP: {
            int subop = instr2 >> 10;
            if (subop == V810_OP_CVT_WS) {
                float res = reg1 ? (float)(SWORD)st->P_REG[reg1] : 0;
                bool z = res == 0;
                int scy = res < 0 ? 0xa : 0;
                psw = (psw & ~0xf) | z | scy;
                if (reg2) *(float *)&st->P_REG[reg2] = res;
            } else if (!(subop & 8) || subop == V810_OP_TRNC_SW) {
                float f1 = reg1 ? *(float *)&st->P_REG[reg1] : 0;
                if (subop == V810_OP_CVT_SW) {
                    SWORD res = round(f1);
                    bool z = res == 0;
                    int scy = res < 0 ? 2 : 0;
                    psw = (psw & ~0xf) | z | scy;
                    if (reg2) st->P_REG[reg2] = res;
                } else if (subop == V810_OP_TRNC_SW) {
                    SWORD res = (SWORD)f1;
                    bool z = res == 0;
                    int scy = res < 0 ? 2 : 0;
                    psw = (psw & ~0xf) | z | scy;
                    if (reg2) st->P_REG[reg2] = res;
                } else {
                    float f2 = reg2 ? *(float *)&st->P_REG[reg2] : 0;
                    float res = 0;
                    switch (subop) {
                        case V810_OP_ADDF_S: res = f2 + f1; break;
                        case V810_OP_CMPF_S:
                        case V810_OP_SUBF_S: res = f2 - f1; break;
                        case V810_OP_MULF_S: res = f2 * f1; break;
                        case V810_OP_DIVF_S: res = f2 / f1; break;
                    }
                    bool z = res == 0;
                    int scy = res < 0 ? 0xa : 0;
                    psw = (psw & ~0xf) | z | scy;
                    if (subop != V810_OP_CMPF_S && reg2) *(float *)&st->P_REG[reg2] = res;
                }
            } else {
                WORD reg2_val = reg2 ? st->P_REG[reg2] : 0;
                WORD out = 0;
                switch (subop) {
                    case V810_OP_MPYHW:
                        out = reg2_val * (reg1 ? (int)(st->P_REG[reg1] << 15) >> 15 : 0);
                        break;
                    case V810_OP_REV:
                        out = reg1 ? ins_rev(st->P_REG[reg1]) : 0;
                        break;
                    case V810_OP_XB:
                        out = (reg2_val & 0xFFFF0000) | ((reg2_val << 8) & 0xFF00) | ((reg2_val >> 8) & 0xFF);
                        break;
                    case V810_OP_XH:
                        out = (reg2_val << 16) | (reg2_val >> 16);
                        break;
                }
                if (reg2) st->P_REG[reg2] = out;
            }
            break;
        }
    }
    return cpsr_from_psw(psw);
}

int drc_handleInterrupts(WORD cpsr, WORD *PCp) {
    cpu_state *st = &vb_state->v810_state;
    WORD pc = (WORD)(uintptr_t)PCp;
    bool halt = pc & 1;
    pc &= ~1u;
    st->S_REG[PSW] = (st->S_REG[PSW] & ~0xF) | psw_from_cpsr(cpsr);
    st->PC = pc;
    if (halt) {
        do {
            st->cycles_until_event_partial = 0;
            predictEvent(true);
            serviceInt(st->cycles, pc);
        } while (!st->ret && st->PC == pc);
        if (st->PC == pc) st->PC = pc - 2;
#ifdef DRC_HOST_SIM
        sim_unwind();
#else
        ((void (*)(void))stub_unwind)();
#endif
        return cpsr;
    }
    predictEvent(true);
    int r = serviceInt(st->cycles, pc);
    if (r && (pc != st->PC || st->ret)) {
#ifdef DRC_HOST_SIM
        sim_unwind();
#else
        ((void (*)(void))stub_unwind)();
#endif
    }
    return cpsr;
}

void drc_relocTable(void) {
}

static void emit_stubs(void) {
    WORD enc;
    WORD *p;

    stub_enter = ep;
    E(0xE92D0000u | 0x4FF8u);
    E(0xED2D8B10u);
    mov_imm(2, (WORD)(uintptr_t)&saved_sp);
    E(i_ldst(C_AL, 0, SP, 2, 0));
    mov_imm(ST, (WORD)(uintptr_t)&vb_state);
    E(i_ldst(C_AL, 1, ST, ST, 0));
    E(i_msr_f(1));
    E(i_bx(C_AL, 0));

    stub_unwind = ep;
    mov_imm(2, (WORD)(uintptr_t)&saved_sp);
    E(i_ldst(C_AL, 1, SP, 2, 0));
    E(0xECBD8B10u);
    E(0xE8BD0000u | 0x8FF8u);

    stub_leave = ep;
    mov_imm(2, (WORD)(uintptr_t)&saved_sp);
    E(i_ldst(C_AL, 1, SP, 2, 0));
    call_fn(H_EXIT);
    E(0xECBD8B10u);
    E(0xE8BD0000u | 0x8FF8u);

    stub_resolve = ep;
    E(i_mrs(12));
    E(0xE92D0000u | (1u << 1) | (1u << 12));
    mov_reg(0, 1);
    call_fn(H_RESOLVE);
    E(0xE8BD0000u | (1u << 1) | (1u << 12));
    E(i_dpi(C_AL, R_CMP, 1, 0, 0, 0));
    p = ep;
    E(i_b(C_EQ, ep, ep));
    E(i_msr_f(12));
    E(i_bx(C_AL, 0));
    patch_b(p, ep);
    mov_reg(0, 12);
    E(i_b(C_AL, ep, stub_leave));

    stub_dyn = ep;
    E(i_mrs(12));
    enc_imm(0x07000000u, &enc);
    E(i_dpi(C_AL, R_AND, 0, 1, 2, enc));
    E(i_dpi(C_AL, R_CMP, 1, 2, 0, enc));
    p = ep;
    E(i_b(C_NE, ep, ep));
    mov_imm(0, (WORD)(uintptr_t)&dyn_ctx);
    E(i_ldst(C_AL, 1, 2, 0, 4));
    E(i_ldst(C_AL, 1, 0, 0, 0));
    E(i_dpr(C_AL, R_AND, 0, 1, 2, 2, SH_LSL, 0));
    E(i_ldr_reg_lsl(C_AL, 0, 0, 2, 1));
    E(i_msr_f(12));
    E(i_bx(C_AL, 0));
    patch_b(p, ep);
    mov_reg(0, 12);
    E(i_b(C_AL, ep, stub_leave));
}

static bool alloc_cache(void) {
#if defined(DRC_HOST_SIM)
    cache_size = CACHE_SIZE;
    cache_base = sim_alloc(cache_size);
#elif defined(__vita__)
    static const size_t sizes[] = { 0x1000000, 0x800000, 0x400000 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        cache_uid = sceKernelAllocMemBlockForVM("red_viter_drc", (SceSize)sizes[i]);
        if (cache_uid < 0) {
            vita_log("drc: alloc %u failed 0x%08x", (unsigned)sizes[i], cache_uid);
            continue;
        }
        void *base = NULL;
        int r = sceKernelGetMemBlockBase(cache_uid, &base);
        if (r < 0) {
            vita_log("drc: block base failed 0x%08x", r);
            return false;
        }
        r = sceKernelOpenVMDomain();
        if (r < 0) {
            vita_log("drc: open domain failed 0x%08x", r);
            return false;
        }
        cache_base = base;
        cache_size = sizes[i];
        vita_log("drc: cache %u bytes", (unsigned)cache_size);
        break;
    }
#else
    return false;
#endif
    return cache_base != NULL;
}

static bool drc_inited = false;

void drc_init(void) {
    if (drc_inited) return;
    if (!alloc_cache()) return;
    drc_inited = true;
    cache_end = cache_base + cache_size / 4;
    ep = cache_base;
    helper_tab[H_RBYTE] = (WORD)(uintptr_t)&mem_rbyte;
    helper_tab[H_RHWORD] = (WORD)(uintptr_t)&mem_rhword;
    helper_tab[H_RWORD] = (WORD)(uintptr_t)&mem_rword;
    helper_tab[H_WBYTE] = (WORD)(uintptr_t)&h_wbyte;
    helper_tab[H_WHWORD] = (WORD)(uintptr_t)&h_whword;
    helper_tab[H_WWORD] = (WORD)(uintptr_t)&h_wword;
    helper_tab[H_SLOW] = (WORD)(uintptr_t)&h_slow;
    helper_tab[H_BSTR] = (WORD)(uintptr_t)&h_bstr;
    helper_tab[H_STSR_PSW] = (WORD)(uintptr_t)&h_stsr_psw;
    helper_tab[H_LDSR_PSW] = (WORD)(uintptr_t)&h_ldsr_psw;
    helper_tab[H_RETI] = (WORD)(uintptr_t)&h_reti;
    helper_tab[H_EXIT] = (WORD)(uintptr_t)&h_exit;
    helper_tab[H_RESOLVE] = (WORD)(uintptr_t)&h_resolve;
#ifdef DRC_HOST_SIM
    for (int i = 0; i < H_COUNT; i++) helper_tab[i] = 0xF0000000u | ((WORD)i << 4);
#endif
    entry_map = NULL;
    map_count = 0;
    emit_stubs();
    blocks_start = ep;
    flush_icache(cache_base, ep);
}

static void bind_state(void) {
    for (int i = 0; i < 2; i++) {
        vb_players[i].v810_state.reloc_table = (void (*)(void))helper_tab;
#ifdef DRC_HOST_SIM
        vb_players[i].v810_state.irq_handler = (int (*)(WORD, WORD *))(uintptr_t)0xF0001000u;
#else
        vb_players[i].v810_state.irq_handler = &drc_handleInterrupts;
#endif
    }
}

static bool drc_user_enabled = true;

void drc_set_enabled(bool enabled) {
    drc_user_enabled = enabled;
    drc_ready = enabled && drc_inited && entry_map != NULL;
}

bool drc_get_enabled(void) {
    return drc_user_enabled;
}

bool drc_available(void) {
    return drc_inited && entry_map != NULL;
}

void drc_reset(void) {
    drc_ready = false;
    drc_init();
    if (!drc_inited) return;
    WORD size = V810_ROM1.size;
    WORD mask = (size && (size & (size - 1)) == 0) ? size - 1 : MAX_ROM_SIZE - 1;
    mask &= ~1u;
    WORD count = (mask >> 1) + 1;
    if (!entry_map || count != map_count) {
#ifdef DRC_HOST_SIM
        entry_map = sim_alloc((size_t)count * 4);
#else
        free(entry_map);
        entry_map = malloc((size_t)count * 4);
#endif
        if (!entry_map) {
            map_count = 0;
            drc_ready = false;
            return;
        }
        map_count = count;
    }
    rom_mask = mask;
    dyn_ctx.map = (WORD)(uintptr_t)entry_map;
    dyn_ctx.mask = mask;
    bind_state();
    clear_blocks();
    drc_ready = drc_user_enabled;
}

void drc_clearCache(void) {
    if (drc_ready && entry_map) clear_blocks();
}

void drc_exit(void) {
    drc_ready = false;
#ifndef DRC_HOST_SIM
    free(entry_map);
#endif
    entry_map = NULL;
    map_count = 0;
}

int drc_run(void) {
    cpu_state *st = &vb_state->v810_state;
    while (!st->ret && (st->PC & 0x07000000) == 0x07000000) {
        WORD *e = get_entry(st->PC);
        if (!e) {
#ifdef __vita__
            vita_log("drc: cannot translate pc=%08x", (unsigned)st->PC);
#endif
            return DRC_ERR_BAD_INST;
        }
        WORD cpsr = cpsr_from_psw(st->S_REG[PSW]);
#ifdef DRC_HOST_SIM
        sim_enter((WORD)(uintptr_t)stub_enter, (WORD)(uintptr_t)e, cpsr);
#else
        ((void (*)(WORD *, WORD))stub_enter)(e, cpsr);
#endif
    }
    return 0;
}

#ifdef DRC_HOST_SIM
uint64_t sim_call_helper(int id, WORD a0, WORD a1, WORD a2, WORD a3) {
    switch (id) {
        case H_RBYTE: return mem_rbyte(a0);
        case H_RHWORD: return mem_rhword(a0);
        case H_RWORD: return mem_rword(a0);
        case H_WBYTE: return h_wbyte(a0, a1);
        case H_WHWORD: return h_whword(a0, a1);
        case H_WWORD: return h_wword(a0, a1);
        case H_SLOW: return h_slow(a0, a1, a2);
        case H_BSTR: return h_bstr(a0, a1, a2, a3);
        case H_STSR_PSW: return h_stsr_psw(a0);
        case H_LDSR_PSW: return h_ldsr_psw(a0);
        case H_RETI: return h_reti(a0);
        case H_EXIT: h_exit(a0, a1); return 0;
        case H_RESOLVE: return h_resolve(a0);
    }
    return 0;
}
#endif

void drc_loadSavedCache(void) {
}

void drc_dumpCache(char *filename) {
    (void)filename;
}

void drc_dumpDebugInfo(int code) {
    (void)code;
}

#else

bool drc_ready = false;

void drc_set_enabled(bool enabled) { (void)enabled; }
bool drc_get_enabled(void) { return false; }
bool drc_available(void) { return false; }

#endif
