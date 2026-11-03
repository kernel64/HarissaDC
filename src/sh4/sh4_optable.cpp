// HarissaDC - Dreamcast emulator for the Xbox 360
// Copyright (C) 2026 Mohamed Aymen
// SPDX-License-Identifier: GPL-2.0-only
#include "sh4.h"
#include "sh4_internal.h"

#include "../core/log.h"

#include <string.h>

#define OPS(X) \
    X(mov, "0110nnnnmmmm0011", "mov Rm,Rn") \
    X(mov_imm, "1110nnnniiiiiiii", "mov #imm,Rn") \
    X(movw_pc, "1001nnnndddddddd", "mov.w @(disp,PC),Rn") \
    X(movl_pc, "1101nnnndddddddd", "mov.l @(disp,PC),Rn") \
    X(mova, "11000111dddddddd", "mova @(disp,PC),R0") \
    X(movb_st, "0010nnnnmmmm0000", "mov.b Rm,@Rn") \
    X(movw_st, "0010nnnnmmmm0001", "mov.w Rm,@Rn") \
    X(movl_st, "0010nnnnmmmm0010", "mov.l Rm,@Rn") \
    X(movb_ld, "0110nnnnmmmm0000", "mov.b @Rm,Rn") \
    X(movw_ld, "0110nnnnmmmm0001", "mov.w @Rm,Rn") \
    X(movl_ld, "0110nnnnmmmm0010", "mov.l @Rm,Rn") \
    X(movb_dec, "0010nnnnmmmm0100", "mov.b Rm,@-Rn") \
    X(movw_dec, "0010nnnnmmmm0101", "mov.w Rm,@-Rn") \
    X(movl_dec, "0010nnnnmmmm0110", "mov.l Rm,@-Rn") \
    X(movb_inc, "0110nnnnmmmm0100", "mov.b @Rm+,Rn") \
    X(movw_inc, "0110nnnnmmmm0101", "mov.w @Rm+,Rn") \
    X(movl_inc, "0110nnnnmmmm0110", "mov.l @Rm+,Rn") \
    X(movb_st_r0disp, "10000000nnnndddd", "mov.b R0,@(disp,Rn)") \
    X(movw_st_r0disp, "10000001nnnndddd", "mov.w R0,@(disp,Rn)") \
    X(movl_st_disp, "0001nnnnmmmmdddd", "mov.l Rm,@(disp,Rn)") \
    X(movb_ld_r0disp, "10000100mmmmdddd", "mov.b @(disp,Rm),R0") \
    X(movw_ld_r0disp, "10000101mmmmdddd", "mov.w @(disp,Rm),R0") \
    X(movl_ld_disp, "0101nnnnmmmmdddd", "mov.l @(disp,Rm),Rn") \
    X(movb_st_r0, "0000nnnnmmmm0100", "mov.b Rm,@(R0,Rn)") \
    X(movw_st_r0, "0000nnnnmmmm0101", "mov.w Rm,@(R0,Rn)") \
    X(movl_st_r0, "0000nnnnmmmm0110", "mov.l Rm,@(R0,Rn)") \
    X(movb_ld_r0, "0000nnnnmmmm1100", "mov.b @(R0,Rm),Rn") \
    X(movw_ld_r0, "0000nnnnmmmm1101", "mov.w @(R0,Rm),Rn") \
    X(movl_ld_r0, "0000nnnnmmmm1110", "mov.l @(R0,Rm),Rn") \
    X(movb_st_gbr, "11000000dddddddd", "mov.b R0,@(disp,GBR)") \
    X(movw_st_gbr, "11000001dddddddd", "mov.w R0,@(disp,GBR)") \
    X(movl_st_gbr, "11000010dddddddd", "mov.l R0,@(disp,GBR)") \
    X(movb_ld_gbr, "11000100dddddddd", "mov.b @(disp,GBR),R0") \
    X(movw_ld_gbr, "11000101dddddddd", "mov.w @(disp,GBR),R0") \
    X(movl_ld_gbr, "11000110dddddddd", "mov.l @(disp,GBR),R0") \
    X(movt, "0000nnnn00101001", "movt Rn") \
    X(swapb, "0110nnnnmmmm1000", "swap.b Rm,Rn") \
    X(swapw, "0110nnnnmmmm1001", "swap.w Rm,Rn") \
    X(xtrct, "0010nnnnmmmm1101", "xtrct Rm,Rn") \
    X(add, "0011nnnnmmmm1100", "add Rm,Rn") \
    X(add_imm, "0111nnnniiiiiiii", "add #imm,Rn") \
    X(addc, "0011nnnnmmmm1110", "addc Rm,Rn") \
    X(addv, "0011nnnnmmmm1111", "addv Rm,Rn") \
    X(cmpeq_imm, "10001000iiiiiiii", "cmp/eq #imm,R0") \
    X(cmpeq, "0011nnnnmmmm0000", "cmp/eq Rm,Rn") \
    X(cmphs, "0011nnnnmmmm0010", "cmp/hs Rm,Rn") \
    X(cmpge, "0011nnnnmmmm0011", "cmp/ge Rm,Rn") \
    X(cmphi, "0011nnnnmmmm0110", "cmp/hi Rm,Rn") \
    X(cmpgt, "0011nnnnmmmm0111", "cmp/gt Rm,Rn") \
    X(cmppl, "0100nnnn00010101", "cmp/pl Rn") \
    X(cmppz, "0100nnnn00010001", "cmp/pz Rn") \
    X(cmpstr, "0010nnnnmmmm1100", "cmp/str Rm,Rn") \
    X(div0s, "0010nnnnmmmm0111", "div0s Rm,Rn") \
    X(div0u, "0000000000011001", "div0u") \
    X(div1, "0011nnnnmmmm0100", "div1 Rm,Rn") \
    X(dmuls, "0011nnnnmmmm1101", "dmuls.l Rm,Rn") \
    X(dmulu, "0011nnnnmmmm0101", "dmulu.l Rm,Rn") \
    X(dt, "0100nnnn00010000", "dt Rn") \
    X(extsb, "0110nnnnmmmm1110", "exts.b Rm,Rn") \
    X(extsw, "0110nnnnmmmm1111", "exts.w Rm,Rn") \
    X(extub, "0110nnnnmmmm1100", "extu.b Rm,Rn") \
    X(extuw, "0110nnnnmmmm1101", "extu.w Rm,Rn") \
    X(macl, "0000nnnnmmmm1111", "mac.l @Rm+,@Rn+") \
    X(macw, "0100nnnnmmmm1111", "mac.w @Rm+,@Rn+") \
    X(mull, "0000nnnnmmmm0111", "mul.l Rm,Rn") \
    X(mulsw, "0010nnnnmmmm1111", "muls.w Rm,Rn") \
    X(muluw, "0010nnnnmmmm1110", "mulu.w Rm,Rn") \
    X(neg, "0110nnnnmmmm1011", "neg Rm,Rn") \
    X(negc, "0110nnnnmmmm1010", "negc Rm,Rn") \
    X(sub, "0011nnnnmmmm1000", "sub Rm,Rn") \
    X(subc, "0011nnnnmmmm1010", "subc Rm,Rn") \
    X(subv, "0011nnnnmmmm1011", "subv Rm,Rn") \
    X(and_, "0010nnnnmmmm1001", "and Rm,Rn") \
    X(and_imm, "11001001iiiiiiii", "and #imm,R0") \
    X(andb, "11001101iiiiiiii", "and.b #imm,@(R0,GBR)") \
    X(not_, "0110nnnnmmmm0111", "not Rm,Rn") \
    X(or_, "0010nnnnmmmm1011", "or Rm,Rn") \
    X(or_imm, "11001011iiiiiiii", "or #imm,R0") \
    X(orb, "11001111iiiiiiii", "or.b #imm,@(R0,GBR)") \
    X(tasb, "0100nnnn00011011", "tas.b @Rn") \
    X(tst, "0010nnnnmmmm1000", "tst Rm,Rn") \
    X(tst_imm, "11001000iiiiiiii", "tst #imm,R0") \
    X(tstb, "11001100iiiiiiii", "tst.b #imm,@(R0,GBR)") \
    X(xor_, "0010nnnnmmmm1010", "xor Rm,Rn") \
    X(xor_imm, "11001010iiiiiiii", "xor #imm,R0") \
    X(xorb, "11001110iiiiiiii", "xor.b #imm,@(R0,GBR)") \
    X(rotl, "0100nnnn00000100", "rotl Rn") \
    X(rotr, "0100nnnn00000101", "rotr Rn") \
    X(rotcl, "0100nnnn00100100", "rotcl Rn") \
    X(rotcr, "0100nnnn00100101", "rotcr Rn") \
    X(shad, "0100nnnnmmmm1100", "shad Rm,Rn") \
    X(shal, "0100nnnn00100000", "shal Rn") \
    X(shar, "0100nnnn00100001", "shar Rn") \
    X(shld, "0100nnnnmmmm1101", "shld Rm,Rn") \
    X(shll, "0100nnnn00000000", "shll Rn") \
    X(shlr, "0100nnnn00000001", "shlr Rn") \
    X(shll2, "0100nnnn00001000", "shll2 Rn") \
    X(shlr2, "0100nnnn00001001", "shlr2 Rn") \
    X(shll8, "0100nnnn00011000", "shll8 Rn") \
    X(shlr8, "0100nnnn00011001", "shlr8 Rn") \
    X(shll16, "0100nnnn00101000", "shll16 Rn") \
    X(shlr16, "0100nnnn00101001", "shlr16 Rn") \
    X(bf, "10001011dddddddd", "bf disp") \
    X(bfs, "10001111dddddddd", "bf/s disp") \
    X(bt, "10001001dddddddd", "bt disp") \
    X(bts, "10001101dddddddd", "bt/s disp") \
    X(bra, "1010dddddddddddd", "bra disp") \
    X(braf, "0000nnnn00100011", "braf Rn") \
    X(bsr, "1011dddddddddddd", "bsr disp") \
    X(bsrf, "0000nnnn00000011", "bsrf Rn") \
    X(jmp, "0100nnnn00101011", "jmp @Rn") \
    X(jsr, "0100nnnn00001011", "jsr @Rn") \
    X(rts, "0000000000001011", "rts") \
    X(rte, "0000000000101011", "rte") \
    X(clrmac, "0000000000101000", "clrmac") \
    X(clrs, "0000000001001000", "clrs") \
    X(clrt, "0000000000001000", "clrt") \
    X(sets, "0000000001011000", "sets") \
    X(sett, "0000000000011000", "sett") \
    X(nop, "0000000000001001", "nop") \
    X(sleep, "0000000000011011", "sleep") \
    X(ldtlb, "0000000000111000", "ldtlb") \
    X(trapa, "11000011iiiiiiii", "trapa #imm") \
    X(ldc_sr, "0100nnnn00001110", "ldc Rm,SR") \
    X(ldc_gbr, "0100nnnn00011110", "ldc Rm,GBR") \
    X(ldc_vbr, "0100nnnn00101110", "ldc Rm,VBR") \
    X(ldc_ssr, "0100nnnn00111110", "ldc Rm,SSR") \
    X(ldc_spc, "0100nnnn01001110", "ldc Rm,SPC") \
    X(ldc_dbr, "0100nnnn11111010", "ldc Rm,DBR") \
    X(ldc_bank, "0100nnnn1bbb1110", "ldc Rm,Rn_BANK") \
    X(ldcl_sr, "0100nnnn00000111", "ldc.l @Rm+,SR") \
    X(ldcl_gbr, "0100nnnn00010111", "ldc.l @Rm+,GBR") \
    X(ldcl_vbr, "0100nnnn00100111", "ldc.l @Rm+,VBR") \
    X(ldcl_ssr, "0100nnnn00110111", "ldc.l @Rm+,SSR") \
    X(ldcl_spc, "0100nnnn01000111", "ldc.l @Rm+,SPC") \
    X(ldcl_dbr, "0100nnnn11110110", "ldc.l @Rm+,DBR") \
    X(ldcl_bank, "0100nnnn1bbb0111", "ldc.l @Rm+,Rn_BANK") \
    X(lds_mach, "0100nnnn00001010", "lds Rm,MACH") \
    X(lds_macl, "0100nnnn00011010", "lds Rm,MACL") \
    X(lds_pr, "0100nnnn00101010", "lds Rm,PR") \
    X(ldsl_mach, "0100nnnn00000110", "lds.l @Rm+,MACH") \
    X(ldsl_macl, "0100nnnn00010110", "lds.l @Rm+,MACL") \
    X(ldsl_pr, "0100nnnn00100110", "lds.l @Rm+,PR") \
    X(stc_sr, "0000nnnn00000010", "stc SR,Rn") \
    X(stc_gbr, "0000nnnn00010010", "stc GBR,Rn") \
    X(stc_vbr, "0000nnnn00100010", "stc VBR,Rn") \
    X(stc_ssr, "0000nnnn00110010", "stc SSR,Rn") \
    X(stc_spc, "0000nnnn01000010", "stc SPC,Rn") \
    X(stc_sgr, "0000nnnn00111010", "stc SGR,Rn") \
    X(stc_dbr, "0000nnnn11111010", "stc DBR,Rn") \
    X(stc_bank, "0000nnnn1bbb0010", "stc Rm_BANK,Rn") \
    X(stcl_sr, "0100nnnn00000011", "stc.l SR,@-Rn") \
    X(stcl_gbr, "0100nnnn00010011", "stc.l GBR,@-Rn") \
    X(stcl_vbr, "0100nnnn00100011", "stc.l VBR,@-Rn") \
    X(stcl_ssr, "0100nnnn00110011", "stc.l SSR,@-Rn") \
    X(stcl_spc, "0100nnnn01000011", "stc.l SPC,@-Rn") \
    X(stcl_sgr, "0100nnnn00110010", "stc.l SGR,@-Rn") \
    X(stcl_dbr, "0100nnnn11110010", "stc.l DBR,@-Rn") \
    X(stcl_bank, "0100nnnn1bbb0011", "stc.l Rm_BANK,@-Rn") \
    X(sts_mach, "0000nnnn00001010", "sts MACH,Rn") \
    X(sts_macl, "0000nnnn00011010", "sts MACL,Rn") \
    X(sts_pr, "0000nnnn00101010", "sts PR,Rn") \
    X(stsl_mach, "0100nnnn00000010", "sts.l MACH,@-Rn") \
    X(stsl_macl, "0100nnnn00010010", "sts.l MACL,@-Rn") \
    X(stsl_pr, "0100nnnn00100010", "sts.l PR,@-Rn") \
    X(pref, "0000nnnn10000011", "pref @Rn") \
    X(ocbi, "0000nnnn10010011", "ocbi @Rn") \
    X(ocbp, "0000nnnn10100011", "ocbp @Rn") \
    X(ocbwb, "0000nnnn10110011", "ocbwb @Rn") \
    X(movcal, "0000nnnn11000011", "movca.l R0,@Rn") \
    X(fmov, "1111nnnnmmmm1100", "fmov FRm,FRn") \
    X(fmov_load, "1111nnnnmmmm1000", "fmov.s @Rm,FRn") \
    X(fmov_store, "1111nnnnmmmm1010", "fmov.s FRm,@Rn") \
    X(fmov_inc, "1111nnnnmmmm1001", "fmov.s @Rm+,FRn") \
    X(fmov_dec, "1111nnnnmmmm1011", "fmov.s FRm,@-Rn") \
    X(fmov_load_r0, "1111nnnnmmmm0110", "fmov.s @(R0,Rm),FRn") \
    X(fmov_store_r0, "1111nnnnmmmm0111", "fmov.s FRm,@(R0,Rn)") \
    X(fldi0, "1111nnnn10001101", "fldi0 FRn") \
    X(fldi1, "1111nnnn10011101", "fldi1 FRn") \
    X(flds, "1111nnnn00011101", "flds FRm,FPUL") \
    X(fsts, "1111nnnn00001101", "fsts FPUL,FRn") \
    X(fabs_, "1111nnnn01011101", "fabs FRn") \
    X(fneg, "1111nnnn01001101", "fneg FRn") \
    X(fadd, "1111nnnnmmmm0000", "fadd FRm,FRn") \
    X(fsub, "1111nnnnmmmm0001", "fsub FRm,FRn") \
    X(fmul, "1111nnnnmmmm0010", "fmul FRm,FRn") \
    X(fdiv, "1111nnnnmmmm0011", "fdiv FRm,FRn") \
    X(fcmpeq, "1111nnnnmmmm0100", "fcmp/eq FRm,FRn") \
    X(fcmpgt, "1111nnnnmmmm0101", "fcmp/gt FRm,FRn") \
    X(fmac, "1111nnnnmmmm1110", "fmac FR0,FRm,FRn") \
    X(fsqrt, "1111nnnn01101101", "fsqrt FRn") \
    X(float_, "1111nnnn00101101", "float FPUL,FRn") \
    X(ftrc, "1111nnnn00111101", "ftrc FRm,FPUL") \
    X(fcnvsd, "1111nnnn10101101", "fcnvsd FPUL,DRn") \
    X(fcnvds, "1111nnnn10111101", "fcnvds DRm,FPUL") \
    X(fsrra, "1111nnnn01111101", "fsrra FRn") \
    X(fipr, "1111nnmm11101101", "fipr FVm,FVn") \
    X(frchg, "1111101111111101", "frchg") \
    X(fschg, "1111001111111101", "fschg") \
    X(ftrv, "1111nn0111111101", "ftrv XMTRX,FVn") \
    X(fsca, "1111nnn011111101", "fsca FPUL,DRn") \
    X(hle_trap, "1111111111111101", "hle trap") \
    X(lds_fpscr, "0100nnnn01101010", "lds Rm,FPSCR") \
    X(lds_fpul, "0100nnnn01011010", "lds Rm,FPUL") \
    X(ldsl_fpscr, "0100nnnn01100110", "lds.l @Rm+,FPSCR") \
    X(ldsl_fpul, "0100nnnn01010110", "lds.l @Rm+,FPUL") \
    X(sts_fpscr, "0000nnnn01101010", "sts FPSCR,Rn") \
    X(sts_fpul, "0000nnnn01011010", "sts FPUL,Rn") \
    X(stsl_fpscr, "0100nnnn01100010", "sts.l FPSCR,@-Rn") \
    X(stsl_fpul, "0100nnnn01010010", "sts.l FPUL,@-Rn")

#define DECLARE(fn, pattern, name) void sh4op_##fn(u16 op);
OPS(DECLARE)
void sh4op_illegal(u16 op);

struct OpPattern
{
    Sh4OpFn fn;
    const char *pattern;
    const char *name;
};

#define ENTRY(fn, pattern, name) { sh4op_##fn, pattern, name },
static const OpPattern s_patterns[] = { OPS(ENTRY) };

Sh4OpFn g_sh4_ops[0x10000];
static u16 s_patternOf[0x10000];
static int s_unknown;

static void pattern_mask(const char *p, u16 *mask, u16 *key)
{
    int i;
    *mask = *key = 0;
    for (i = 0; i < 16; i++)
    {
        u16 bit = (u16)(0x8000 >> i);
        if (p[i] == '0' || p[i] == '1')
        {
            *mask |= bit;
            if (p[i] == '1') *key |= bit;
        }
    }
}

void sh4_init(void)
{
    u32 op, i, conflicts = 0;
    for (op = 0; op < 0x10000; op++)
    {
        g_sh4_ops[op] = sh4op_illegal;
        s_patternOf[op] = 0;
    }
    for (i = 0; i < EMU_ARRAY_COUNT(s_patterns); i++)
    {
        u16 mask, key;
        pattern_mask(s_patterns[i].pattern, &mask, &key);
        for (op = 0; op < 0x10000; op++)
        {
            if ((op & mask) != key) continue;
            if (s_patternOf[op]) conflicts++;
            else
            {
                g_sh4_ops[op] = s_patterns[i].fn;
                s_patternOf[op] = (u16)(i + 1);
            }
        }
    }
    s_unknown = 0;
    for (op = 0; op < 0x10000; op++)
        if (!s_patternOf[op]) s_unknown++;
    if (conflicts) LOG_ERROR("[sh4] opcode table: %u overlapping patterns", conflicts);
    LOG_DEBUG("[sh4] opcode table: %u patterns, %d illegal opcodes", (u32)EMU_ARRAY_COUNT(s_patterns), s_unknown);
}

static int ends_block(const char *name)
{
    static const char *const prefixes[] = { "bf", "bt", "bra", "bsr", "jmp", "jsr", "rts", "rte", "trapa", "sleep",
                                            "ldc.l @Rm+,SR", "ldtlb", "illegal", "hle trap" };
    u32 i;
    for (i = 0; i < EMU_ARRAY_COUNT(prefixes); i++)
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    return 0;
}

int sh4_op_writes_memory(u16 op)
{
    const char *name = sh4_op_name(op);
    return strstr(name, ",@") != NULL || strncmp(name, "tas", 3) == 0 || strncmp(name, "and.b", 5) == 0 ||
           strncmp(name, "or.b", 4) == 0 || strncmp(name, "xor.b", 5) == 0 || strncmp(name, "movca", 5) == 0 ||
           strncmp(name, "pref", 4) == 0 || strncmp(name, "trapa", 5) == 0 || strncmp(name, "bsr", 3) == 0 ||
           strncmp(name, "jsr", 3) == 0 || strncmp(name, "illegal", 7) == 0 || strncmp(name, "hle", 3) == 0;
}

int sh4_op_ends_block(u16 op)
{
    return ends_block(sh4_op_name(op));
}

const char *sh4_op_name(u16 op)
{
    return s_patternOf[op] ? s_patterns[s_patternOf[op] - 1].name : "illegal";
}

int sh4_op_unknown_count(void)
{
    return s_unknown;
}
