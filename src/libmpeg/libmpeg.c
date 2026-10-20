/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright (c) 2006-2007 Eugene Plotnikov <e-plotnikov@operamail.com>
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
# Based on refernce software of MSSG
*/

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#ifndef _EE
// for MIN / MAX macros
#include <sys/param.h>
#endif

#include "libmpeg.h"
#include "ee_regs.h"
#include "libmpeg_internal.h"

static _MPEGContext s_MPEG12Ctx;
static s64 *s_pCurPTS;

static void (*LumaOp[8])(_MPEGMotion *m, u8 *a2, short *a3, int a4, int var1, int ta, int a7, int a8) = {
    _MPEG_put_luma, _MPEG_put_luma_X, _MPEG_put_luma_Y, _MPEG_put_luma_XY,
    _MPEG_avg_luma, _MPEG_avg_luma_X, _MPEG_avg_luma_Y, _MPEG_avg_luma_XY};

static void (*ChromaOp[8])(_MPEGMotion *m, u8 *a2, short *a3, int a4, int var1, int ta, int a7, int a8) = {
    _MPEG_put_chroma, _MPEG_put_chroma_X, _MPEG_put_chroma_Y, _MPEG_put_chroma_XY,
    _MPEG_avg_chroma, _MPEG_avg_chroma_X, _MPEG_avg_chroma_Y, _MPEG_avg_chroma_XY};

static void (*PutBlockOp[3])(_MPEGMotions *arg0) = {
    _MPEG_put_block_il, _MPEG_put_block_fr, _MPEG_put_block_fl};

static void (*AddBlockOp[2][2])(_MPEGMotions *arg0) = {
    {_MPEG_add_block_frfr, _MPEG_add_block_frfl},
    {_MPEG_add_block_ilfl, _MPEG_add_block_ilfl}};

static float s_FrameRate[16] = {
    0.0F, ((23.0F * 1000.0F) / 1001.0F),
    24.0F, 25.0F, ((30.0F * 1000.0F) / 1001.0F),
    30.0F, 50.0F, ((60.0F * 1000.0F) / 1001.0F),
    60.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};

static void *(*_init_cb)(void *userdata, MPEGSequenceInfo *si);
static void *s_pInitCBParam;

static void *_init_seq(void);
static void _destroy_seq(void);

static int _get_hdr(void);

static void _seq_header(void);
static void _gop_header(void);
static void _pic_header(void);
static void _ext_and_ud(void);
static void _ext_unknown(void);
static void _ext_seq(void);
static void _ext_seq_dsp(void);
static void _ext_qnt_mtx(void);
static void _ext_cpy_rgt(void);
static void _ext_seq_scl(void);
static void _ext_pic_dsp(void);
static void _ext_pic_cod(void);
static void _ext_pic_ssc(void);
static void _ext_pic_tsc(void);
static void _xtra_bitinf(void);

static int _get_next_picture(void *apData, s64 *apPTS);
static int _get_first_picture(void *apData, s64 *apPTS);

/*
 * PS2DVDPlayer: último quadro de referência (I/P) decodificado e
 * ainda não exibido. No fim da sequência ele é entregue (a ordem de
 * exibição só o libera quando chega a referência seguinte); sem isso
 * um menu com imagem parada (um único quadro I) nunca aparece.
 */
static int s_RefPending;

/*
 * PS2DVDPlayer: 1 = o próximo quadro é decodificado (referências em
 * dia) mas não convertido para RGB (quadro atrasado, não vai à tela).
 */
static int s_SkipCSC;

void MPEG_SetSkipCSC(int skip)
{
    s_SkipCSC = skip;
}


int (*MPEG_Picture)(void *apData, s64 *apPTS);

static void (*DoMC)(void);

static void _mpeg12_picture_data(void);
static int _mpeg12_slice(int aMBAMax);

/*
 * PS2DVDPlayer: conversão de cor (YCbCr 4:2:0 -> RGBA32) em
 * software no EE, no lugar da CSC do IPU. A CSC do IPU exige
 * suspender/retomar o fluxo de bits no meio da decodificação e,
 * no PCSX2, a retomada perde a posição (o decodificador para no
 * quadro seguinte).
 */
#ifndef PS2DVD_SW_CSC
#define PS2DVD_SW_CSC 1
#endif

/*
 * Informa ao chamador se o quadro sai direto em CT16 linear
 * (CSC em software) ou em blocos RGBA32 (CSC no IPU).
 */
int MPEG_DirectCT16(void)
{
    return PS2DVD_SW_CSC;
}

/*
 * Saída direta (otimizada): RGBA 16 bits (GS_PSM_CT16) linear,
 * escrita no buffer indicado por MPEG_SetDirectCT16. Evita a saída
 * intermediária RGBA32 em blocos e a segunda conversão no chamador.
 *
 * Tabelas (BT.601, faixa 16-235):
 *   R = 1.164(Y-16) + 1.596(Cr-128)
 *   G = 1.164(Y-16) - 0.391(Cb-128) - 0.813(Cr-128)
 *   B = 1.164(Y-16) + 2.018(Cb-128)
 * em ponto fixo (x256). Os limites 0..255 e a redução a 5 bits
 * já posicionados no campo de cor ficam nas tabelas s_Clamp*.
 */
static unsigned short *s_pDirect;
static int s_DirectStride;

static int s_TabY[256];
static int s_TabCrR[256];
static int s_TabCbG[256];
static int s_TabCrG[256];
static int s_TabCbB[256];

#define CLAMP_BIAS 640
static unsigned short s_ClampR[CLAMP_BIAS * 2];
static unsigned short s_ClampG[CLAMP_BIAS * 2];
static unsigned short s_ClampB[CLAMP_BIAS * 2];
static int s_TabsReady;

static void _sw_csc_init_tabs(void)
{
    int i;

    for (i = 0; i < 256; ++i) {
        s_TabY[i]   = 298 * (i - 16) + 128;
        s_TabCrR[i] = 409 * (i - 128);
        s_TabCbG[i] = -100 * (i - 128);
        s_TabCrG[i] = -208 * (i - 128);
        s_TabCbB[i] = 516 * (i - 128);
    }

    /* índice = (valor >> 8) + CLAMP_BIAS */
    for (i = 0; i < CLAMP_BIAS * 2; ++i) {
        int v = i - CLAMP_BIAS;

        if (v < 0)
            v = 0;
        else if (v > 255)
            v = 255;

        s_ClampR[i] = (unsigned short)(v >> 3);
        s_ClampG[i] = (unsigned short)((v >> 3) << 5);
        s_ClampB[i] = (unsigned short)(((v >> 3) << 10) | 0x8000);
    }

    s_TabsReady = 1;
}

/*
 * PS2DVDPlayer: saída em planos Y / Cb / Cr (8 bits) para a GS fazer
 * a conversão de cor. Com planos definidos, _sw_csc só copia os
 * macroblocos (sem RGB). ystride = largura do plano Y; os planos de
 * cor têm ystride / 2.
 */
static unsigned char *s_pPlanarY;
static unsigned char *s_pPlanarCb;
static unsigned char *s_pPlanarCr;
static int s_PlanarStride;

void MPEG_SetPlanar(unsigned char *apY, unsigned char *apCb, unsigned char *apCr, int aStride)
{
    s_pPlanarY      = apY;
    s_pPlanarCb     = apCb;
    s_pPlanarCr     = apCr;
    s_PlanarStride  = aStride;
}

static void _mb_to_planar(const _MPEGMacroBlock8 *lpMB, int aMX, int aMY)
{
    int ys = s_PlanarStride;
    int cs = s_PlanarStride >> 1;
    u64 *dy = (u64 *)(s_pPlanarY + aMY * 16 * ys + aMX * 16);
    u64 *db = (u64 *)(s_pPlanarCb + aMY * 8 * cs + aMX * 8);
    u64 *dr = (u64 *)(s_pPlanarCr + aMY * 8 * cs + aMX * 8);
    const u64 *sy = (const u64 *)lpMB->m_Y;
    const u64 *sb = (const u64 *)lpMB->m_Cb;
    const u64 *sr = (const u64 *)lpMB->m_Cr;
    int r;

    for (r = 0; r < 16; ++r) {
        dy[0] = sy[0];
        dy[1] = sy[1];
        sy += 2;
        dy = (u64 *)((unsigned char *)dy + ys);
    }

    for (r = 0; r < 8; ++r) {
        *db = *sb++;
        *dr = *sr++;
        db = (u64 *)((unsigned char *)db + cs);
        dr = (u64 *)((unsigned char *)dr + cs);
    }
}

void MPEG_SetDirectCT16(unsigned short *apDst, int aStride)
{
    s_pDirect      = apDst;
    s_DirectStride = aStride;

    if (!s_TabsReady)
        _sw_csc_init_tabs();
}

/*
 * Versão escalar (referência): um macrobloco -> CT16 linear.
 */
static void _sw_csc_mb_scalar(const _MPEGMacroBlock8 *lpMB, unsigned short *lpOut, int lStride)
{
    int cy;

    /* duas linhas de luma por linha de croma */
    for (cy = 0; cy < 8; ++cy) {
        const unsigned char *lpY0 = lpMB->m_Y[cy * 2];
        const unsigned char *lpY1 = lpMB->m_Y[cy * 2 + 1];
        const unsigned char *lpCb = lpMB->m_Cb[cy];
        const unsigned char *lpCr = lpMB->m_Cr[cy];
        unsigned short *lpR0      = lpOut + cy * 2 * lStride;
        unsigned short *lpR1      = lpR0 + lStride;
        int cx;

        for (cx = 0; cx < 8; ++cx) {
            int lR = s_TabCrR[lpCr[cx]];
            int lG = s_TabCbG[lpCb[cx]] + s_TabCrG[lpCr[cx]];
            int lB = s_TabCbB[lpCb[cx]];
            int y;

#define PUT(row, yv, x)                                                  \
    y          = s_TabY[yv];                                              \
    row[x]     = s_ClampR[((y + lR) >> 8) + CLAMP_BIAS] |                 \
             s_ClampG[((y + lG) >> 8) + CLAMP_BIAS] |                     \
             s_ClampB[((y + lB) >> 8) + CLAMP_BIAS]

            PUT(lpR0, lpY0[cx * 2], cx * 2);
            PUT(lpR0, lpY0[cx * 2 + 1], cx * 2 + 1);
            PUT(lpR1, lpY1[cx * 2], cx * 2);
            PUT(lpR1, lpY1[cx * 2 + 1], cx * 2 + 1);
#undef PUT
        }
    }
}

/*
 * Versão MMI (SIMD 128 bits do EE): 8 pixels por instrução, em 16 bits.
 * Escrita em assembly porque o GCC do PS2 não mantém valores de
 * 128 bits num único registrador (usaria pares ld/sd de 64 bits).
 *
 * Coeficientes BT.601 em ponto fixo Q5 (x32), sem estouro de 16 bits:
 *   Y' = 37*Y - 37*16 + 128   (o +128 arredonda o >>8 final)
 *   R  = Y' + 51*(Cr-128)
 *   G  = Y' - 13*(Cb-128) - 26*(Cr-128)
 *   B  = Y' + 65*(Cb-128)
 * canal de 5 bits = clamp(x, 0, 8191) >> 8.
 * Multiplicações por constante com deslocamentos e somas.
 *
 * Constantes (s_KTab, 16 bytes cada): 464, 128, 8191, 0x8000.
 * Registradores (caller-saved): $8=8191 $9=0x8000 $10=R $11=G $12=B
 * (termos de croma), $24=Cb dup, $15=Cr dup, $13/$14/$2/$3 temporários.
 */
static const unsigned short s_KTab[32] __attribute__((aligned(16))) = {
    464, 464, 464, 464, 464, 464, 464, 464,
    128, 128, 128, 128, 128, 128, 128, 128,
    8191, 8191, 8191, 8191, 8191, 8191, 8191, 8191,
    0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000
};

static void _sw_csc_mb_mmi(const _MPEGMacroBlock8 *lpMB, unsigned short *lpOut, int lStride)
{
    unsigned int lStrideB = (unsigned int)lStride * 2;
    int cy;

    for (cy = 0; cy < 8; ++cy) {
        const unsigned char *pY = lpMB->m_Y[cy * 2];
        const unsigned char *pC = lpMB->m_Cb[cy]; /* Cr = pC + 64 */
        unsigned short *pD      = lpOut + cy * 2 * lStride;

        __asm__ __volatile__(
        "lq    $8, 32(%[pK])\n"
        "lq    $9, 48(%[pK])\n"
        "ld    $24, 0(%[pC])\n"
        "pextlb $24, $24, $24\n"
        "ld    $15, 64(%[pC])\n"
        "pextlb $15, $15, $15\n"
        "pextlb $13, $0, $24\n"
        "pextlb $14, $0, $15\n"
        "lq    $2, 16(%[pK])\n"
        "psubh $13, $13, $2\n"
        "psubh $14, $14, $2\n"
        "psllh $10, $14, 5\n"
        "psllh $3, $14, 4\n"
        "paddh $10, $10, $3\n"
        "psllh $3, $14, 1\n"
        "paddh $10, $10, $3\n"
        "paddh $10, $10, $14\n"
        "psllh $12, $13, 6\n"
        "paddh $12, $12, $13\n"
        "psllh $11, $13, 3\n"
        "psllh $3, $13, 2\n"
        "paddh $11, $11, $3\n"
        "paddh $11, $11, $13\n"
        "psllh $3, $14, 3\n"
        "psllh $2, $14, 2\n"
        "paddh $3, $3, $2\n"
        "paddh $3, $3, $14\n"
        "psllh $3, $3, 1\n"
        "paddh $11, $11, $3\n"
        "lq    $13, 0(%[pY])\n"
        "pextlb $13, $0, $13\n"
        "psllh $14, $13, 5\n"
        "psllh $2, $13, 2\n"
        "paddh $14, $14, $2\n"
        "paddh $14, $14, $13\n"
        "lq    $2, 0(%[pK])\n"
        "psubh $14, $14, $2\n"
        "paddh $13, $14, $10\n"
        "pmaxh $13, $13, $0\n"
        "pminh $13, $13, $8\n"
        "psrah $13, $13, 8\n"
        "psubh $2, $14, $11\n"
        "pmaxh $2, $2, $0\n"
        "pminh $2, $2, $8\n"
        "psrah $2, $2, 8\n"
        "psllh $2, $2, 5\n"
        "por   $13, $13, $2\n"
        "paddh $3, $14, $12\n"
        "pmaxh $3, $3, $0\n"
        "pminh $3, $3, $8\n"
        "psrah $3, $3, 8\n"
        "psllh $3, $3, 10\n"
        "por   $13, $13, $3\n"
        "por   $13, $13, $9\n"
        "sq    $13, 0(%[pD])\n"
        "lq    $13, 16(%[pY])\n"
        "pextlb $13, $0, $13\n"
        "psllh $14, $13, 5\n"
        "psllh $2, $13, 2\n"
        "paddh $14, $14, $2\n"
        "paddh $14, $14, $13\n"
        "lq    $2, 0(%[pK])\n"
        "psubh $14, $14, $2\n"
        "paddh $13, $14, $10\n"
        "pmaxh $13, $13, $0\n"
        "pminh $13, $13, $8\n"
        "psrah $13, $13, 8\n"
        "psubh $2, $14, $11\n"
        "pmaxh $2, $2, $0\n"
        "pminh $2, $2, $8\n"
        "psrah $2, $2, 8\n"
        "psllh $2, $2, 5\n"
        "por   $13, $13, $2\n"
        "paddh $3, $14, $12\n"
        "pmaxh $3, $3, $0\n"
        "pminh $3, $3, $8\n"
        "psrah $3, $3, 8\n"
        "psllh $3, $3, 10\n"
        "por   $13, $13, $3\n"
        "por   $13, $13, $9\n"
        "addu  $3, %[pD], %[sB]\n"
        "sq    $13, 0($3)\n"
        "pextub $13, $0, $24\n"
        "pextub $14, $0, $15\n"
        "lq    $2, 16(%[pK])\n"
        "psubh $13, $13, $2\n"
        "psubh $14, $14, $2\n"
        "psllh $10, $14, 5\n"
        "psllh $3, $14, 4\n"
        "paddh $10, $10, $3\n"
        "psllh $3, $14, 1\n"
        "paddh $10, $10, $3\n"
        "paddh $10, $10, $14\n"
        "psllh $12, $13, 6\n"
        "paddh $12, $12, $13\n"
        "psllh $11, $13, 3\n"
        "psllh $3, $13, 2\n"
        "paddh $11, $11, $3\n"
        "paddh $11, $11, $13\n"
        "psllh $3, $14, 3\n"
        "psllh $2, $14, 2\n"
        "paddh $3, $3, $2\n"
        "paddh $3, $3, $14\n"
        "psllh $3, $3, 1\n"
        "paddh $11, $11, $3\n"
        "lq    $13, 0(%[pY])\n"
        "pextub $13, $0, $13\n"
        "psllh $14, $13, 5\n"
        "psllh $2, $13, 2\n"
        "paddh $14, $14, $2\n"
        "paddh $14, $14, $13\n"
        "lq    $2, 0(%[pK])\n"
        "psubh $14, $14, $2\n"
        "paddh $13, $14, $10\n"
        "pmaxh $13, $13, $0\n"
        "pminh $13, $13, $8\n"
        "psrah $13, $13, 8\n"
        "psubh $2, $14, $11\n"
        "pmaxh $2, $2, $0\n"
        "pminh $2, $2, $8\n"
        "psrah $2, $2, 8\n"
        "psllh $2, $2, 5\n"
        "por   $13, $13, $2\n"
        "paddh $3, $14, $12\n"
        "pmaxh $3, $3, $0\n"
        "pminh $3, $3, $8\n"
        "psrah $3, $3, 8\n"
        "psllh $3, $3, 10\n"
        "por   $13, $13, $3\n"
        "por   $13, $13, $9\n"
        "sq    $13, 16(%[pD])\n"
        "lq    $13, 16(%[pY])\n"
        "pextub $13, $0, $13\n"
        "psllh $14, $13, 5\n"
        "psllh $2, $13, 2\n"
        "paddh $14, $14, $2\n"
        "paddh $14, $14, $13\n"
        "lq    $2, 0(%[pK])\n"
        "psubh $14, $14, $2\n"
        "paddh $13, $14, $10\n"
        "pmaxh $13, $13, $0\n"
        "pminh $13, $13, $8\n"
        "psrah $13, $13, 8\n"
        "psubh $2, $14, $11\n"
        "pmaxh $2, $2, $0\n"
        "pminh $2, $2, $8\n"
        "psrah $2, $2, 8\n"
        "psllh $2, $2, 5\n"
        "por   $13, $13, $2\n"
        "paddh $3, $14, $12\n"
        "pmaxh $3, $3, $0\n"
        "pminh $3, $3, $8\n"
        "psrah $3, $3, 8\n"
        "psllh $3, $3, 10\n"
        "por   $13, $13, $3\n"
        "por   $13, $13, $9\n"
        "addu  $3, %[pD], %[sB]\n"
        "sq    $13, 16($3)\n"
            :
            : [pY] "r"(pY), [pC] "r"(pC), [pD] "r"(pD), [sB] "r"(lStrideB), [pK] "r"(s_KTab)
            : "$2", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "memory");
    }
}

/*
 * Verificação única: no primeiro quadro, compara MMI com a escalar
 * (diferença esperada de no máximo 1 nível de 5 bits por canal,
 * pelo arredondamento dos coeficientes Q5 vs Q8).
 */
#ifndef PS2DVD_CSC_VERIFY
#define PS2DVD_CSC_VERIFY 0 /* 1 = compara MMI x escalar no primeiro quadro */
#endif

static int __attribute__((unused)) s_CSCVerified;

static void __attribute__((unused)) _sw_csc_verify(const _MPEGMacroBlock8 *lpMB, int aMBCount)
{
    unsigned short lRef[256] __attribute__((aligned(16)));
    unsigned short lMMI[256] __attribute__((aligned(16)));
    int lDiff1 = 0, lDiffBig = 0, lMax = 0;
    int i, p, c;

    for (i = 0; i < aMBCount; ++i) {
        _sw_csc_mb_scalar(lpMB + i, lRef, 16);
        _sw_csc_mb_mmi(lpMB + i, lMMI, 16);

        for (p = 0; p < 256; ++p) {
            for (c = 0; c < 15; c += 5) {
                int a = (lRef[p] >> c) & 31;
                int b = (lMMI[p] >> c) & 31;
                int d = a > b ? a - b : b - a;

                if (d > lMax)
                    lMax = d;
                if (d == 1)
                    lDiff1++;
                else if (d > 1)
                    lDiffBig++;
            }
        }
    }

    printf("CSC: MMI vs scalar: %d channels differ by 1, %d by more (max %d) of %d\n",
           lDiff1, lDiffBig, lMax, aMBCount * 256 * 3);
}

static int __attribute__((unused)) _sw_csc(void *apSrc, void *apDst, int aMBCount)
{
    const _MPEGMacroBlock8 *lpMB = (const _MPEGMacroBlock8 *)apSrc;
    int lMBW                     = s_MPEG12Ctx.m_MBWidth;
    int lStride                  = s_DirectStride;
    int i;

    (void)apDst;

    if (s_pPlanarY) {
        /* conversão de cor pela GS: só os planos Y/Cb/Cr */
        InvalidDCache((void *)lpMB, (void *)(lpMB + aMBCount));

        for (i = 0; i < aMBCount; ++i, ++lpMB)
            _mb_to_planar(lpMB, i % lMBW, i / lMBW);

        return aMBCount;
    }

    if (!s_pDirect)
        return aMBCount;

    /* quadro escrito por DMA (fromSPR): descartar linhas antigas */
    InvalidDCache((void *)lpMB, (void *)(lpMB + aMBCount));

#if PS2DVD_CSC_VERIFY
    if (!s_CSCVerified) {
        _sw_csc_verify(lpMB, aMBCount);
        s_CSCVerified = 1;
    }
#endif

    for (i = 0; i < aMBCount; ++i, ++lpMB)
        _sw_csc_mb_mmi(lpMB, s_pDirect + (i / lMBW) * 16 * lStride + (i % lMBW) * 16, lStride);

    return aMBCount;
}

static int _mpeg12_dec_mb(int *apMBType, int *apMotionType, int aPMV[2][2][2], int aMVFS[2][2], int aDMVector[2]);

void MPEG_Initialize(
    int (*apDataCB)(void *userdata), void *apDataCBParam,
    void *(*apInitCB)(void *userdata, MPEGSequenceInfo *si), void *apInitCBParam,
    s64 *apCurPTS)
{
    memset(&s_MPEG12Ctx, 0, sizeof(s_MPEG12Ctx));

    _MPEG_Initialize(&s_MPEG12Ctx, apDataCB, apDataCBParam, &s_MPEG12Ctx.m_SI.m_fEOF);

    _init_cb       = apInitCB;
    s_pInitCBParam = apInitCBParam;
    s_pCurPTS      = apCurPTS;

    s_MPEG12Ctx.m_SI.m_FrameCnt  = 0;
    s_MPEG12Ctx.m_SI.m_fEOF      = 0;
    s_RefPending                 = 0;
    s_MPEG12Ctx.m_SI.m_Profile   = -1;
    s_MPEG12Ctx.m_SI.m_Level     = -1;
    s_MPEG12Ctx.m_SI.m_ChromaFmt = MPEG_CHROMA_FORMAT_420;
    s_MPEG12Ctx.m_SI.m_VideoFmt  = MPEG_VIDEO_FORMAT_UNSPEC;
    s_MPEG12Ctx.m_fMPEG2         = 0;

    s_MPEG12Ctx.m_MC[0].m_pSPRBlk = (void *)0x70000000;
    s_MPEG12Ctx.m_MC[0].m_pSPRRes = (void *)0x70000300;
    s_MPEG12Ctx.m_MC[0].m_pSPRMC  = (void *)0x70000600;
    s_MPEG12Ctx.m_MC[1].m_pSPRBlk = (void *)0x70001E00;
    s_MPEG12Ctx.m_MC[1].m_pSPRRes = (void *)0x70002100;
    s_MPEG12Ctx.m_MC[1].m_pSPRMC  = (void *)0x70002400;

    MPEG_Picture = _get_first_picture;
}

/*
 * PS2DVDPlayer: 1 se o decodificador está no início de uma sequência
 * (antes do primeiro quadro ou depois de um sequence end code já
 * consumido). Usado para segurar a tela num still frame do DVD.
 */
int MPEG_AtSequenceStart(void)
{
    return MPEG_Picture == _get_first_picture;
}

void MPEG_Destroy(void)
{
    _destroy_seq();
    _MPEG_Destroy();
}

static void *_init_seq(void)
{
    int lMBWidth, lMBHeight;
    char *pA;

    if (!s_MPEG12Ctx.m_fMPEG2) {
        s_MPEG12Ctx.m_fProgSeq   = 1;
        s_MPEG12Ctx.m_PictStruct = _MPEG_PS_FRAME;
        s_MPEG12Ctx.m_fFPFrmDCT  = 1;
    }

    lMBWidth = (s_MPEG12Ctx.m_SI.m_Width + 15) / 16;
    if (s_MPEG12Ctx.m_fMPEG2 && !s_MPEG12Ctx.m_fProgSeq) {
        lMBHeight = 2 * ((s_MPEG12Ctx.m_SI.m_Height + 31) / 32);
    } else {
        lMBHeight = (s_MPEG12Ctx.m_SI.m_Height + 15) / 16;
    }

    if (lMBWidth != s_MPEG12Ctx.m_MBWidth || lMBHeight != s_MPEG12Ctx.m_MBHeight) {
        unsigned int lAllocSize;
        int i, lSize;

        if (s_MPEG12Ctx.m_pFwdFrame)
            _destroy_seq();

        s_MPEG12Ctx.m_MBWidth     = lMBWidth;
        s_MPEG12Ctx.m_MBHeight    = lMBHeight;
        s_MPEG12Ctx.m_SI.m_Width  = lMBWidth << 4;
        s_MPEG12Ctx.m_SI.m_Height = lMBHeight << 4;

        s_MPEG12Ctx.m_MBStride = lMBWidth * sizeof(_MPEGMacroBlock8);

        lSize = lMBWidth * (lMBHeight + 1) * sizeof(_MPEGMacroBlock8) + sizeof(_MPEGMacroBlock8);

        s_MPEG12Ctx.m_MBCount = s_MPEG12Ctx.m_MBWidth * s_MPEG12Ctx.m_MBHeight;

        lAllocSize = (lSize * 3) + (s_MPEG12Ctx.m_MBCount * sizeof(_MPEGMBXY));

        pA = memalign(64, lAllocSize);

        s_MPEG12Ctx.m_pFrameArena = pA;
        s_MPEG12Ctx.m_pFwdFrame   = (_MPEGMacroBlock8 *)pA;
        pA += lSize;
        s_MPEG12Ctx.m_pBckFrame = (_MPEGMacroBlock8 *)pA;
        pA += lSize;
        s_MPEG12Ctx.m_pAuxFrame = (_MPEGMacroBlock8 *)pA;
        pA += lSize;
        s_MPEG12Ctx.m_pMBXY = (_MPEGMBXY *)pA;

        for (i = 0; i < s_MPEG12Ctx.m_MBCount; ++i) {
            s_MPEG12Ctx.m_pMBXY[i].m_X = i % lMBWidth;
            s_MPEG12Ctx.m_pMBXY[i].m_Y = i / lMBWidth;
        }
    }

    return _init_cb(s_pInitCBParam, &s_MPEG12Ctx.m_SI);
}

static void _destroy_seq(void)
{
    MPEG_Picture = _get_first_picture;

    if (s_MPEG12Ctx.m_pFrameArena) {
        free(s_MPEG12Ctx.m_pFrameArena);
        s_MPEG12Ctx.m_pAuxFrame = NULL;
        s_MPEG12Ctx.m_pBckFrame = NULL;
        s_MPEG12Ctx.m_pFwdFrame = NULL;
        s_MPEG12Ctx.m_pMBXY     = NULL;
    }

    s_MPEG12Ctx.m_MBWidth  = 0;
    s_MPEG12Ctx.m_MBHeight = 0;
}

/*
 * decode headers from one input stream
 * until an End of Sequence or picture start code
 * is found
 */
static int _get_hdr(void)
{
    while (1) {
        unsigned int lCode = _MPEG_NextStartCode();
        _MPEG_GetBits(32);

        switch (lCode) {
            case _MPEG_CODE_SEQ_HDR:
                _seq_header();
                break;
            case _MPEG_CODE_GRP_START:
                _gop_header();
                break;
            case _MPEG_CODE_PIC_START:
                _pic_header();
                return 1;
            case _MPEG_CODE_SEQ_END:
                MPEG_Picture = _get_first_picture;
                return 0;
        }
    }
}

static void _seq_header(void)
{
    s_MPEG12Ctx.m_SI.m_Width  = _MPEG_GetBits(12);
    s_MPEG12Ctx.m_SI.m_Height = _MPEG_GetBits(12);

    _MPEG_GetBits(4); /* aspect_ratio_information    */
    s_MPEG12Ctx.m_SI.m_MSPerFrame = (int)((1000.0F / s_FrameRate[s_MPEG12Ctx.m_FRCode = _MPEG_GetBits(4)]) + 0.5F);
    _MPEG_GetBits(18); /* bit_rate_value              */
    _MPEG_GetBits(1);  /* marker_bit                  */
    _MPEG_GetBits(10); /* vbv_buffer_size             */
    _MPEG_GetBits(1);  /* constrained_parameters_flag */

    if (_MPEG_GetBits(1))
        _MPEG_SetQM(0);
    else
        _MPEG_SetDefQM(0);

    if (_MPEG_GetBits(1))
        _MPEG_SetQM(1);
    else
        _MPEG_SetDefQM(1);

    _ext_and_ud();
}

static void _gop_header(void)
{
#ifdef _DEBUG
    _MPEG_GetBits(1); /* drop_flag   */
    _MPEG_GetBits(5); /* hour        */
    _MPEG_GetBits(6); /* minute      */
    _MPEG_GetBits(1); /* marker_bit  */
    _MPEG_GetBits(6); /* sec         */
    _MPEG_GetBits(6); /* frame       */
    _MPEG_GetBits(1); /* closed_gop  */
    _MPEG_GetBits(1); /* broken_link */
#else
    _MPEG_GetBits(27);
#endif /* _DEBUG */
    _ext_and_ud();
}

static void _pic_header(void)
{
    unsigned int lPicCT;

    _MPEG_GetBits(10); /* temporal_reference */
    lPicCT = _MPEG_GetBits(3);
    _MPEG_GetBits(16); /* vbv_delay          */

    if (lPicCT == _MPEG_PT_P || lPicCT == _MPEG_PT_B) {
        s_MPEG12Ctx.m_FPFVector = _MPEG_GetBits(1);
        s_MPEG12Ctx.m_FwdFCode  = _MPEG_GetBits(3);
    }

    if (lPicCT == _MPEG_PT_B) {
        s_MPEG12Ctx.m_FPBVector = _MPEG_GetBits(1);
        s_MPEG12Ctx.m_BckFCode  = _MPEG_GetBits(3);
    }

    _xtra_bitinf();
    _ext_and_ud();

    _MPEG_SetPCT(s_MPEG12Ctx.m_PictCodingType = lPicCT);
}

static void _ext_and_ud(void)
{
    int lCode, lXID;
    lCode = _MPEG_NextStartCode();

    while (lCode == _MPEG_CODE_EXTENSION || lCode == _MPEG_CODE_USER_DATA) {
        if (lCode == _MPEG_CODE_EXTENSION) {
            _MPEG_GetBits(32);
            lXID = _MPEG_GetBits(4);

            switch (lXID) {
                case _MPEG_XID_0:
                    _ext_unknown();
                    break;
                case _MPEG_XID_SEQUENCE:
                    _ext_seq();
                    break;
                case _MPEG_XID_DISPLAY:
                    _ext_seq_dsp();
                    break;
                case _MPEG_XID_QMATRIX:
                    _ext_qnt_mtx();
                    break;
                case _MPEG_XID_COPYRIGHT:
                    _ext_cpy_rgt();
                    break;
                case _MPEG_XID_SCALABLE:
                    _ext_seq_scl();
                    break;
                case _MPEG_XID_6:
                    _ext_unknown();
                    break;
                case _MPEG_XID_PIC_DSP:
                    _ext_pic_dsp();
                    break;
                case _MPEG_XID_PIC_COD:
                    _ext_pic_cod();
                    break;
                case _MPEG_XID_PIC_SSC:
                    _ext_pic_ssc();
                    break;
                case _MPEG_XID_PIC_TSC:
                    _ext_pic_tsc();
                    break;
            }

            lCode = _MPEG_NextStartCode();
        } else {
            _MPEG_GetBits(32);
            lCode = _MPEG_NextStartCode();
        }
    }
}

static void _ext_unknown(void) {}

static void _ext_seq(void)
{
    int lHSzX;
    int lVSzX;
    int lProfLevel;
    int lFRXn, lFRXd;

    s_MPEG12Ctx.m_fMPEG2 = 1;

    *R_EE_IPU_CTRL &= 0xFF7FFFFF;

    lProfLevel                   = _MPEG_GetBits(8);
    s_MPEG12Ctx.m_fProgSeq       = _MPEG_GetBits(1);
    s_MPEG12Ctx.m_SI.m_ChromaFmt = _MPEG_GetBits(2);
    lHSzX                        = _MPEG_GetBits(2);
    lVSzX                        = _MPEG_GetBits(2);
#ifdef _DEBUG
    _MPEG_GetBits(12); /* bit_rate_extension        */
    _MPEG_GetBits(1);  /* marker_bit                */
    _MPEG_GetBits(8);  /* vbv_buffer_size_extension */
    _MPEG_GetBits(1);  /* low_delay                 */
    lFRXn = _MPEG_GetBits(2);
    lFRXd = _MPEG_GetBits(5);
#else
    _MPEG_GetBits(22);
    lFRXn = _MPEG_GetBits(2);
    lFRXd = _MPEG_GetBits(5);
#endif /* _DEBUG */
    s_MPEG12Ctx.m_SI.m_MSPerFrame = (int)((1000.0F / (s_FrameRate[s_MPEG12Ctx.m_FRCode] * ((lFRXn + 1.0F) / (lFRXd + 1.0F)))) + 0.5F);

    if ((lProfLevel >> 7) & 1) {
        if ((lProfLevel & 15) == 5) {
            s_MPEG12Ctx.m_SI.m_Profile = MPEG_PROFILE_422;
            s_MPEG12Ctx.m_SI.m_Level   = MPEG_LEVEL_MAIN;
        } else {
            s_MPEG12Ctx.m_SI.m_Profile = s_MPEG12Ctx.m_SI.m_Level = -1;
        }
    } else {
        s_MPEG12Ctx.m_SI.m_Profile = lProfLevel >> 4;
        s_MPEG12Ctx.m_SI.m_Level   = lProfLevel & 0xF;
    }

    s_MPEG12Ctx.m_SI.m_Width  = (lHSzX << 12) | (s_MPEG12Ctx.m_SI.m_Width & 0x0FFF);
    s_MPEG12Ctx.m_SI.m_Height = (lVSzX << 12) | (s_MPEG12Ctx.m_SI.m_Height & 0x0FFF);
}


static void _ext_seq_dsp(void)
{
    s_MPEG12Ctx.m_SI.m_VideoFmt = _MPEG_GetBits(3);

    if (_MPEG_GetBits(1)) { /* color_description */
#ifdef _DEBUG
        _MPEG_GetBits(8); /* color_primaries          */
        _MPEG_GetBits(8); /* transfer_characteristics */
        _MPEG_GetBits(8); /* matrix_coefficients      */
#else
        _MPEG_GetBits(24);
#endif /* _DEBUG */
    }
#ifdef _DEBUG
    _MPEG_GetBits(14); /* display_horizontal_size */
    _MPEG_GetBits(1);  /* marker_bit              */
    _MPEG_GetBits(14); /* display_vertical_size   */
#else
    _MPEG_GetBits(29);
#endif /* _DEBUG */
}

static void _ext_qnt_mtx(void)
{
    int i;

    if (_MPEG_GetBits(1))
        _MPEG_SetQM(0);

    if (_MPEG_GetBits(1))
        _MPEG_SetQM(1);

    if (_MPEG_GetBits(1)) {
        for (i = 0; i < 16; ++i)
            _MPEG_GetBits(32);
    }

    if (_MPEG_GetBits(1)) {
        for (i = 0; i < 16; ++i)
            _MPEG_GetBits(32);
    }
}

static void _ext_cpy_rgt(void)
{
#ifdef _DEBUG
    _MPEG_GetBits(1);  /* copyright_flag       */
    _MPEG_GetBits(8);  /* copyright_identifier */
    _MPEG_GetBits(1);  /* original_or_copy     */
    _MPEG_GetBits(7);  /* reserved_data        */
    _MPEG_GetBits(1);  /* marker_bit           */
    _MPEG_GetBits(20); /* copyright_number_1   */
    _MPEG_GetBits(1);  /* marker_bit           */
    _MPEG_GetBits(22); /* copyright_number_2   */
    _MPEG_GetBits(1);  /* marker_bit           */
    _MPEG_GetBits(22); /* copyright_number_3   */
#else
    _MPEG_GetBits(32);
    _MPEG_GetBits(32);
    _MPEG_GetBits(20);
#endif /* _DEBUG */
}

static void _ext_seq_scl(void) {}

static void _ext_pic_dsp(void)
{
    int i;
    int lnFCO;

    if (s_MPEG12Ctx.m_fProgSeq) {
        if (s_MPEG12Ctx.m_fRepFF)
            lnFCO = s_MPEG12Ctx.m_fTopFF ? 3 : 2;
        else
            lnFCO = 1;
    } else {
        if (s_MPEG12Ctx.m_PictStruct != _MPEG_PS_FRAME)
            lnFCO = 1;
        else
            lnFCO = s_MPEG12Ctx.m_fRepFF ? 3 : 2;
    }


    for (i = 0; i < lnFCO; ++i) {
        _MPEG_GetBits(16); /* frame_center_horizontal_offset[ i ] */
        _MPEG_GetBits(1);  /* marker_bit                          */
        _MPEG_GetBits(16); /* frame_center_vertical_offset [ i ]  */
        _MPEG_GetBits(1);  /* marker_bit                          */
    }
}

static void _ext_pic_cod(void)
{

    s_MPEG12Ctx.m_FCode[0][0] = _MPEG_GetBits(4);
    s_MPEG12Ctx.m_FCode[0][1] = _MPEG_GetBits(4);
    s_MPEG12Ctx.m_FCode[1][0] = _MPEG_GetBits(4);
    s_MPEG12Ctx.m_FCode[1][1] = _MPEG_GetBits(4);
    _MPEG_SetIDCP();
    s_MPEG12Ctx.m_PictStruct = _MPEG_GetBits(2);
    s_MPEG12Ctx.m_fTopFF     = _MPEG_GetBits(1);
    s_MPEG12Ctx.m_fFPFrmDCT  = _MPEG_GetBits(1);
    s_MPEG12Ctx.m_fConsMV    = _MPEG_GetBits(1);
    _MPEG_SetQSTIVFAS();
    s_MPEG12Ctx.m_fRepFF = _MPEG_GetBits(1);
#ifdef _DEBUG
    _MPEG_GetBits(1); /* chroma_420_type   */
    _MPEG_GetBits(1); /* progressive_frame */
#else
    _MPEG_GetBits(2);
#endif                      /* _DEBUG */
    if (_MPEG_GetBits(1)) { /* composite_display_flag */
#ifdef _DEBUG
        _MPEG_GetBits(1); /* v_axis            */
        _MPEG_GetBits(3); /* field_sequence    */
        _MPEG_GetBits(1); /* sub_carrier       */
        _MPEG_GetBits(7); /* burst_amplitude   */
        _MPEG_GetBits(8); /* sub_carrier_phase */
#else
        _MPEG_GetBits(20);
#endif /* _DEBUG */
    }
}

static void _ext_pic_ssc(void) {}


static void _ext_pic_tsc(void) {}

static void _xtra_bitinf(void)
{
    while (_MPEG_GetBits(1))
        _MPEG_GetBits(8);
}

static int _get_first_picture(void *apData, s64 *apPTS)
{
    int retVal = _get_hdr();

    if (retVal) {
        s_MPEG12Ctx.m_SI.m_FrameCnt = 0;

        apData = _init_seq();
        _mpeg12_picture_data();

        if (s_MPEG12Ctx.m_PictCodingType != _MPEG_PT_B &&
            (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME || s_MPEG12Ctx.m_fSecField))
            s_RefPending = 1;

        if (s_MPEG12Ctx.m_PictStruct != _MPEG_PS_FRAME)
            s_MPEG12Ctx.m_fSecField ^= 1;

        MPEG_Picture = _get_next_picture;

        if (!s_MPEG12Ctx.m_fSecField)
            ++s_MPEG12Ctx.m_SI.m_FrameCnt;

        retVal = _get_next_picture(apData, apPTS);
    }

    return retVal;
}

static int _get_next_picture(void *apData, s64 *apPTS)
{
    int retVal;

    while (1) {
        if ((retVal = _get_hdr())) {
            int lfPic = 0;

            _mpeg12_picture_data();

            if ((s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME || s_MPEG12Ctx.m_fSecField) && s_MPEG12Ctx.m_SI.m_FrameCnt) {
                void *lpData;

                if (s_MPEG12Ctx.m_PictCodingType == _MPEG_PT_B) {
                    lpData = s_MPEG12Ctx.m_pAuxFrame;
                    *apPTS = s_MPEG12Ctx.m_AuxPTS;
                } else {
                    lpData = s_MPEG12Ctx.m_pFwdFrame;
                    *apPTS = s_MPEG12Ctx.m_FwdPTS;
                }

#if PS2DVD_SW_CSC
                lfPic = s_SkipCSC ? 1 : _sw_csc(lpData, apData, s_MPEG12Ctx.m_MBCount);
#else
                lfPic = _MPEG_CSCImage(lpData, apData, s_MPEG12Ctx.m_MBCount);
#endif
            }

            if (s_MPEG12Ctx.m_PictCodingType != _MPEG_PT_B &&
                (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME || s_MPEG12Ctx.m_fSecField))
                s_RefPending = 1;

            if (s_MPEG12Ctx.m_PictStruct != _MPEG_PS_FRAME)
                s_MPEG12Ctx.m_fSecField ^= 1;

            if (!s_MPEG12Ctx.m_fSecField)
                ++s_MPEG12Ctx.m_SI.m_FrameCnt;

            if (lfPic)
                break;
        } else {
            /* fim da sequência: entregar a referência pendente */
            if (s_RefPending && s_MPEG12Ctx.m_pBckFrame) {
                s_RefPending = 0;
                *apPTS       = s_MPEG12Ctx.m_BckPTS;
#if PS2DVD_SW_CSC
                retVal = _sw_csc(s_MPEG12Ctx.m_pBckFrame, apData, s_MPEG12Ctx.m_MBCount) ? 1 : 0;
#else
                retVal = _MPEG_CSCImage(s_MPEG12Ctx.m_pBckFrame, apData, s_MPEG12Ctx.m_MBCount) ? 1 : 0;
#endif
            }
            break;
        }
    }

    return retVal;
}

static void _mpeg12_do_next_mc(void)
{
    _MPEGMotions *lpMotions = &s_MPEG12Ctx.m_MC[!s_MPEG12Ctx.m_CurMC];
    _MPEGMotion *lpMotion   = &lpMotions->m_Motion[0];

    while (lpMotion->MC_Luma) {
        _MPEG_do_mc(lpMotion);
        ++lpMotion;
    }

    lpMotions->BlockOp(lpMotions);
}

static void _mpeg12_do_first_mc(void)
{
    DoMC = _mpeg12_do_next_mc;
}

/* Decode all macroblocks of the current picture */
static void _mpeg12_picture_data(void)
{
    int lMBAMax = s_MPEG12Ctx.m_MBWidth * s_MPEG12Ctx.m_MBHeight;
    _MPEGMacroBlock8 *lpMB;

    if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME && s_MPEG12Ctx.m_fSecField)
        s_MPEG12Ctx.m_fSecField = 0;

    if (s_MPEG12Ctx.m_PictCodingType == _MPEG_PT_B) {
        s_MPEG12Ctx.m_pCurFrame = s_MPEG12Ctx.m_pAuxFrame;
        s_MPEG12Ctx.m_AuxPTS    = *s_pCurPTS;
    } else {
        s64 lPTS;

        if (!s_MPEG12Ctx.m_fSecField) {
            lpMB                    = s_MPEG12Ctx.m_pFwdFrame;
            lPTS                    = s_MPEG12Ctx.m_FwdPTS;
            s_MPEG12Ctx.m_pFwdFrame = s_MPEG12Ctx.m_pBckFrame;
            s_MPEG12Ctx.m_FwdPTS    = s_MPEG12Ctx.m_BckPTS;
            s_MPEG12Ctx.m_pBckFrame = lpMB;
            s_MPEG12Ctx.m_BckPTS    = lPTS;
        }

        s_MPEG12Ctx.m_pCurFrame = s_MPEG12Ctx.m_pBckFrame;
        s_MPEG12Ctx.m_BckPTS    = *s_pCurPTS;
    }

    s_MPEG12Ctx.m_pCurFrameY    = (unsigned char *)s_MPEG12Ctx.m_pCurFrame;
    s_MPEG12Ctx.m_pCurFrameCbCr = (unsigned char *)s_MPEG12Ctx.m_pCurFrame + 256;
    if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_BOTTOM_FIELD) {
        s_MPEG12Ctx.m_pCurFrameY += 16;
        s_MPEG12Ctx.m_pCurFrameCbCr += 8;
    }

    if (s_MPEG12Ctx.m_PictStruct != _MPEG_PS_FRAME)
        lMBAMax >>= 1;

    s_MPEG12Ctx.m_CurMC = 0;

    DoMC = _mpeg12_do_first_mc;

    while (_mpeg12_slice(lMBAMax) >= 0)
        ;

    _MPEG_WaitBDEC();
    DoMC();
}

/* calculate motion vector component */
/* ISO/IEC 13818-2 section 7.6.3.1: Decoding the motion vectors */
static void _mpeg12_decode_motion_vector(int *apPred, int aRSize, int aMotionCode,
                                         int aMotionResidual, int aFullPelVector)
{
    int lLim = 16 << aRSize;
    int lVec = aFullPelVector ? *apPred >> 1 : *apPred;

    if (aMotionCode > 0) {
        lVec += ((aMotionCode - 1) << aRSize) + aMotionResidual + 1;

        if (lVec >= lLim)
            lVec -= lLim + lLim;

    } else if (aMotionCode < 0) {
        lVec -= ((-aMotionCode - 1) << aRSize) + aMotionResidual + 1;

        if (lVec < -lLim)
            lVec += lLim + lLim;
    }

    *apPred = aFullPelVector ? lVec << 1 : lVec;
}

/* ISO/IEC 13818-2 section 7.6.3.6: Dual prime additional arithmetic */
static void _mpeg12_dual_prime_vector(int aDMV[][2], const int *apDMVector, int aMVX, int aMVY)
{
    if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME) {
        if (s_MPEG12Ctx.m_fTopFF) {
            aDMV[0][0] = ((aMVX + (aMVX > 0)) >> 1) + apDMVector[0];
            aDMV[0][1] = ((aMVY + (aMVY > 0)) >> 1) + apDMVector[1] - 1;

            aDMV[1][0] = ((3 * aMVX + (aMVX > 0)) >> 1) + apDMVector[0];
            aDMV[1][1] = ((3 * aMVY + (aMVY > 0)) >> 1) + apDMVector[1] + 1;
        } else {
            aDMV[0][0] = ((3 * aMVX + (aMVX > 0)) >> 1) + apDMVector[0];
            aDMV[0][1] = ((3 * aMVY + (aMVY > 0)) >> 1) + apDMVector[1] - 1;

            aDMV[1][0] = ((aMVX + (aMVX > 0)) >> 1) + apDMVector[0];
            aDMV[1][1] = ((aMVY + (aMVY > 0)) >> 1) + apDMVector[1] + 1;
        }
    } else {
        aDMV[0][0] = ((aMVX + (aMVX > 0)) >> 1) + apDMVector[0];
        aDMV[0][1] = ((aMVY + (aMVY > 0)) >> 1) + apDMVector[1];

        if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_TOP_FIELD)
            --aDMV[0][1];
        else
            ++aDMV[0][1];
    }
}

/* get and decode motion vector and differential motion vector
   for one prediction */
static void _mpeg12_motion_vector(int *apPMV, int *apDMVector, int aHRSize,
                                  int aVRSize, int aDMV, int aMVScale, int aFullPelVector)
{
    int lMotionCode;
    int lMotionResidual;

    /* horizontal component */
    lMotionCode     = _MPEG_GetMotionCode();
    lMotionResidual = aHRSize && lMotionCode ? _MPEG_GetBits(aHRSize) : 0;

    _mpeg12_decode_motion_vector(&apPMV[0], aHRSize, lMotionCode, lMotionResidual, aFullPelVector);

    if (aDMV)
        apDMVector[0] = _MPEG_GetDMVector();

    s_MPEG12Ctx.m_fError = lMotionCode == -32768;

    /* vertical component */
    lMotionCode     = _MPEG_GetMotionCode();
    lMotionResidual = aVRSize && lMotionCode ? _MPEG_GetBits(aVRSize) : 0;

    if (aMVScale)
        apPMV[1] >>= 1;

    _mpeg12_decode_motion_vector(&apPMV[1], aVRSize, lMotionCode, lMotionResidual, aFullPelVector);

    if (aMVScale)
        apPMV[1] <<= 1;

    if (aDMV)
        apDMVector[1] = _MPEG_GetDMVector();

    s_MPEG12Ctx.m_fError = lMotionCode == -32768;
}

static void _mpeg12_motion_vectors(
    int aPMV[2][2][2], int aDMVector[2], int aMVFS[2][2], int aS, int anMV,
    int aMVFmt, int aHRSize, int aVRSize, int aDMV, int aMVScale)
{
    if (anMV == 1) {
        if (aMVFmt == _MPEG_MV_FIELD && !aDMV)
            aMVFS[1][aS] = aMVFS[0][aS] = _MPEG_GetBits(1);

        _mpeg12_motion_vector(aPMV[0][aS], aDMVector, aHRSize, aVRSize, aDMV, aMVScale, 0);

        aPMV[1][aS][0] = aPMV[0][aS][0];
        aPMV[1][aS][1] = aPMV[0][aS][1];
    } else {
        aMVFS[0][aS] = _MPEG_GetBits(1);
        _mpeg12_motion_vector(aPMV[0][aS], aDMVector, aHRSize, aVRSize, aDMV, aMVScale, 0);

        aMVFS[1][aS] = _MPEG_GetBits(1);
        _mpeg12_motion_vector(aPMV[1][aS], aDMVector, aHRSize, aVRSize, aDMV, aMVScale, 0);
    }
}

static int _mpeg12_dec_mb(
    int *apMBType, int *apMotionType,
    int aPMV[2][2][2], int aMVFS[2][2], int aDMVector[2])
{

    int lMBType;
    int lDMV;
    int lMVScale;
    int lnMV;
    int lMVFmt;
    int lDCType;
    int lMotionType;
    int lfIntra;

    lMotionType = 0;
    lMBType     = _MPEG_GetMBType();

    if (!lMBType)
        return 0;

    lfIntra = lMBType & _MPEG_MBT_INTRA;

    if (lMBType & (_MPEG_MBT_MOTION_FORWARD | _MPEG_MBT_MOTION_BACKWARD)) {

        if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME)
            lMotionType = s_MPEG12Ctx.m_fFPFrmDCT ? _MPEG_MC_FRAME : _MPEG_GetBits(2);
        else
            lMotionType = _MPEG_GetBits(2);

    } else if (lfIntra && s_MPEG12Ctx.m_fConsMV)
        lMotionType = (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME) ? _MPEG_MC_FRAME : _MPEG_MC_FIELD;

    if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME) {
        lnMV   = lMotionType == _MPEG_MC_FIELD ? 2 : 1;
        lMVFmt = lMotionType == _MPEG_MC_FRAME ? _MPEG_MV_FRAME : _MPEG_MV_FIELD;
    } else {
        lnMV   = (lMotionType == _MPEG_MC_16X8) ? 2 : 1;
        lMVFmt = _MPEG_MV_FIELD;
    }

    lDMV     = lMotionType == _MPEG_MC_DMV;
    lMVScale = lMVFmt == _MPEG_MV_FIELD && s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME;
    lDCType  = s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME && !s_MPEG12Ctx.m_fFPFrmDCT && lMBType & (_MPEG_MBT_PATTERN | _MPEG_MBT_INTRA) ? _MPEG_GetBits(1) : 0;

    if (lMBType & _MPEG_MBT_QUANT)
        s_MPEG12Ctx.m_QScale = _MPEG_GetBits(5);

    if ((lMBType & _MPEG_MBT_MOTION_FORWARD) || (lfIntra && s_MPEG12Ctx.m_fConsMV)) {
        if (s_MPEG12Ctx.m_fMPEG2)
            _mpeg12_motion_vectors(aPMV, aDMVector, aMVFS, 0, lnMV, lMVFmt, s_MPEG12Ctx.m_FCode[0][0] - 1, s_MPEG12Ctx.m_FCode[0][1] - 1, lDMV, lMVScale);
        else
            _mpeg12_motion_vector(aPMV[0][0], aDMVector, s_MPEG12Ctx.m_FwdFCode - 1, s_MPEG12Ctx.m_FwdFCode - 1, 0, 0, s_MPEG12Ctx.m_FPFVector);
    }

    if (s_MPEG12Ctx.m_fError)
        return 0;

    if (lMBType & _MPEG_MBT_MOTION_BACKWARD) {
        if (s_MPEG12Ctx.m_fMPEG2)
            _mpeg12_motion_vectors(aPMV, aDMVector, aMVFS, 1, lnMV, lMVFmt, s_MPEG12Ctx.m_FCode[1][0] - 1, s_MPEG12Ctx.m_FCode[1][1] - 1, 0, lMVScale);
        else
            _mpeg12_motion_vector(aPMV[0][1], aDMVector, s_MPEG12Ctx.m_BckFCode - 1, s_MPEG12Ctx.m_BckFCode - 1, 0, 0, s_MPEG12Ctx.m_FPBVector);
    }

    if (s_MPEG12Ctx.m_fError)
        return 0;

    if (lfIntra && s_MPEG12Ctx.m_fConsMV)
        _MPEG_GetBits(1);

    if (lMBType & (_MPEG_MBT_INTRA | _MPEG_MBT_PATTERN))
        _MPEG_BDEC(lfIntra, s_MPEG12Ctx.m_fDCRst, lDCType, s_MPEG12Ctx.m_QScale, s_MPEG12Ctx.m_pCurMotions->m_pSPRBlk);

    s_MPEG12Ctx.m_fDCRst = !lfIntra;

    if (lfIntra && !s_MPEG12Ctx.m_fConsMV) {
        aPMV[0][0][0] = 0;
        aPMV[0][0][1] = 0;
        aPMV[1][0][0] = 0;
        aPMV[1][0][1] = 0;
        aPMV[0][1][0] = 0;
        aPMV[0][1][1] = 0;
        aPMV[1][1][0] = 0;
        aPMV[1][1][1] = 0;
    }

    if ((s_MPEG12Ctx.m_PictCodingType == _MPEG_PT_P) && !(lMBType & (_MPEG_MBT_MOTION_FORWARD | _MPEG_MBT_INTRA))) {
        aPMV[0][0][0] = 0;
        aPMV[0][0][1] = 0;
        aPMV[1][0][0] = 0;
        aPMV[1][0][1] = 0;

        if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME) {
            lMotionType = _MPEG_MC_FRAME;
        } else {
            lMotionType = _MPEG_MC_FIELD;
            aMVFS[0][0] = s_MPEG12Ctx.m_PictStruct == _MPEG_PS_BOTTOM_FIELD;
        }
    }

    *apMBType     = lMBType;
    *apMotionType = lMotionType;

    return 1;
}

static void _mpeg12_get_ref(_MPEGMacroBlock8 *apMBSrc, int aX, int anY,
                            int aDX, int aDY, int aH, int aFSrc, int aFDst, int afAvg)
{

    int lfInt             = (aH & 8) >> 3;
    int lDXY              = ((aDY & 1) << 1) | (aDX & 1);
    int lUVXY             = ((aDX & 2) >> 1) | (aDY & 2);
    int lSrcX             = aX + (aDX >> 1);
    int lSrcY             = ((anY + (aDY >> 1)) << lfInt) + aFSrc;
    int lMBX              = lSrcX >> 4;
    int lMBY              = lSrcY >> 4;
    _MPEGMotion *lpMotion = &s_MPEG12Ctx.m_pCurMotions->m_Motion[s_MPEG12Ctx.m_pCurMotions->m_nMotions++];

    afAvg <<= 2;

#ifdef _EE
    __asm__ __volatile__(
        "pnor     $v1, $zero, $zero \n"
        "ld       $v0, %4           \n"
        "pextlw   %0, %3, %2        \n"
        "paddw    $v0, $v0, $v1     \n"
        "pmaxw    %0, %0, $zero     \n"
        "pminw    %0, %0, $v0       \n"
        "dsrl32   %1, %0, 0         \n"
        "sll      %0, %0, 0         \n"
        : "=r"(lMBX), "=r"(lMBY) : "r"(lMBX), "r"(lMBY), "m"(s_MPEG12Ctx.m_MBWidth)
        : "v0", "v1");
#else
    lMBX = MIN(MAX(lMBX, 0), s_MPEG12Ctx.m_MBWidth - 1);
    lMBY = MIN(MAX(lMBY, 0), s_MPEG12Ctx.m_MBWidth - 1);
#endif

    lpMotion->m_pSrc     = (unsigned char *)(apMBSrc + lMBX + lMBY * s_MPEG12Ctx.m_MBWidth);
    lpMotion->m_pDstY    = (short *)(s_MPEG12Ctx.m_pCurMotions->m_pSPRRes + (aFDst << 5));
    lpMotion->m_pDstCbCr = (short *)(s_MPEG12Ctx.m_pCurMotions->m_pSPRRes + 512 + (aFDst << 3));
    lpMotion->m_X        = lSrcX & 0xF;
    lpMotion->m_Y        = lSrcY & 0xF;
    lpMotion->m_H        = aH;
    lpMotion->m_fInt     = lfInt;
    lpMotion->m_Field    = aFSrc;
    lpMotion->MC_Luma    = LumaOp[lDXY + afAvg];
    lpMotion->MC_Chroma  = ChromaOp[lUVXY + afAvg];
}

static void _mpeg12_get_refs(int aBX, int aBY, int aMBType, int aMotionType, int aPMV[2][2][2],
                             int aMVFS[2][2], int aDMVector[2])
{
    int lfAdd = 0;

    s_MPEG12Ctx.m_pCurMotions->m_nMotions = 0;

    if ((aMBType & _MPEG_MBT_MOTION_FORWARD) ||
        (s_MPEG12Ctx.m_PictCodingType == _MPEG_PT_P)) {

        int lDMV[2][2];

        if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME) {

            if ((aMotionType == _MPEG_MC_FRAME) || !(aMBType & _MPEG_MBT_MOTION_FORWARD)) {
                _mpeg12_get_ref(s_MPEG12Ctx.m_pFwdFrame, aBX, aBY, aPMV[0][0][0], aPMV[0][0][1],
                                16, 0, 0, 0);
            } else if (aMotionType == _MPEG_MC_FIELD) {
                _mpeg12_get_ref(s_MPEG12Ctx.m_pFwdFrame, aBX, aBY >> 1, aPMV[0][0][0], aPMV[0][0][1] >> 1,
                                8, aMVFS[0][0], 0, 0);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pFwdFrame, aBX, aBY >> 1, aPMV[1][0][0], aPMV[1][0][1] >> 1,
                                8, aMVFS[1][0], 8, 0);
            } else if (aMotionType == _MPEG_MC_DMV) {

                _mpeg12_dual_prime_vector(lDMV, aDMVector, aPMV[0][0][0], aPMV[0][0][1] >> 1);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pFwdFrame, aBX, aBY >> 1, aPMV[0][0][0], aPMV[0][0][1] >> 1,
                                8, 0, 0, 0);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pFwdFrame, aBX, aBY >> 1, lDMV[0][0], lDMV[0][1],
                                8, 1, 0, 1);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pFwdFrame, aBX, aBY >> 1, aPMV[0][0][0], aPMV[0][0][1] >> 1,
                                8, 1, 8, 0);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pFwdFrame, aBX, aBY >> 1, lDMV[1][0], lDMV[1][1],
                                8, 0, 8, 1);
            }
        } else {
            int lCurField;
            _MPEGMacroBlock8 *lpMBSrc;

            lCurField = (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_BOTTOM_FIELD);
            lpMBSrc   = (s_MPEG12Ctx.m_PictCodingType == _MPEG_PT_P) && s_MPEG12Ctx.m_fSecField && (lCurField != aMVFS[0][0]) ? s_MPEG12Ctx.m_pBckFrame : s_MPEG12Ctx.m_pFwdFrame;

            if ((aMotionType == _MPEG_MC_FIELD) || !(aMBType & _MPEG_MBT_MOTION_FORWARD)) {
                _mpeg12_get_ref(lpMBSrc, aBX, aBY, aPMV[0][0][0], aPMV[0][0][1], 8,
                                aMVFS[0][0], 0, 0);
                _mpeg12_get_ref(lpMBSrc, aBX, aBY + 8, aPMV[0][0][0], aPMV[0][0][1], 8,
                                aMVFS[0][0], 8, 0);
            } else if (aMotionType == _MPEG_MC_16X8) {
                _mpeg12_get_ref(lpMBSrc, aBX, aBY, aPMV[0][0][0], aPMV[0][0][1], 8,
                                aMVFS[0][0], 0, 0);

                lpMBSrc = (s_MPEG12Ctx.m_PictCodingType == _MPEG_PT_P) &&
                                  s_MPEG12Ctx.m_fSecField &&
                                  (lCurField != aMVFS[1][0]) ?
                              s_MPEG12Ctx.m_pBckFrame :
                              s_MPEG12Ctx.m_pFwdFrame;
                _mpeg12_get_ref(lpMBSrc, aBX, aBY + 8, aPMV[1][0][0], aPMV[1][0][1], 8,
                                aMVFS[1][0], 8, 0);
            } else if (aMotionType == _MPEG_MC_DMV) {
                lpMBSrc = s_MPEG12Ctx.m_fSecField ? s_MPEG12Ctx.m_pBckFrame : s_MPEG12Ctx.m_pFwdFrame;
                _mpeg12_dual_prime_vector(lDMV, aDMVector, aPMV[0][0][0], aPMV[0][0][1]);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pFwdFrame, aBX, aBY, aPMV[0][0][0], aPMV[0][0][1], 16,
                                lCurField, 0, 0);
                _mpeg12_get_ref(lpMBSrc, aBX, aBY, lDMV[0][0], lDMV[0][1], 16, !lCurField, 1, 1);
            }
        }

        lfAdd = 1;
    }

    if (aMBType & _MPEG_MBT_MOTION_BACKWARD) {
        if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME) {
            if (aMotionType == _MPEG_MC_FRAME) {
                _mpeg12_get_ref(s_MPEG12Ctx.m_pBckFrame, aBX, aBY, aPMV[0][1][0], aPMV[0][1][1],
                                16, 0, 0, lfAdd);
            } else {
                _mpeg12_get_ref(s_MPEG12Ctx.m_pBckFrame, aBX, aBY >> 1, aPMV[0][1][0], aPMV[0][1][1] >> 1,
                                8, aMVFS[0][1], 0, lfAdd);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pBckFrame, aBX, aBY >> 1, aPMV[1][1][0], aPMV[1][1][1] >> 1,
                                8, aMVFS[1][1], 8, lfAdd);
            }
        } else {
            if (aMotionType == _MPEG_MC_FIELD) {
                _mpeg12_get_ref(s_MPEG12Ctx.m_pBckFrame, aBX, aBY, aPMV[0][1][0], aPMV[0][1][1], 8,
                                aMVFS[0][1], 0, lfAdd);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pBckFrame, aBX, aBY + 8, aPMV[0][1][0], aPMV[0][1][1], 8,
                                aMVFS[0][1], 8, lfAdd);
            } else if (aMotionType == _MPEG_MC_16X8) {
                _mpeg12_get_ref(s_MPEG12Ctx.m_pBckFrame, aBX, aBY, aPMV[0][1][0], aPMV[0][1][1],
                                8, aMVFS[0][1], 0, lfAdd);
                _mpeg12_get_ref(s_MPEG12Ctx.m_pBckFrame, aBX, aBY + 8, aPMV[1][1][0], aPMV[1][1][1],
                                8, aMVFS[1][1], 8, lfAdd);
            }
        }
    }

    _MPEG_dma_ref_image((_MPEGMacroBlock8 *)s_MPEG12Ctx.m_pCurMotions->m_pSPRMC,
                        &s_MPEG12Ctx.m_pCurMotions->m_Motion[0],
                        s_MPEG12Ctx.m_pCurMotions->m_nMotions, s_MPEG12Ctx.m_MBWidth);
}

/* Motion compensation */
static void _mpeg2_mc(int aMBA, int aMBType, int aMotionType,
                      int aPMV[2][2][2], int aMVFS[2][2],
                      int aDMVector[2], int aMBAI)
{
    /* block x,y */
    int lBX, lBY;
    int lfField;
    int lfIntra;
    int lOffset;
    int lfNoSkip;
    int lfFiledMV;

    lBX       = s_MPEG12Ctx.m_pMBXY[aMBA].m_X;
    lBY       = s_MPEG12Ctx.m_pMBXY[aMBA].m_Y;
    lfField   = s_MPEG12Ctx.m_PictStruct != _MPEG_PS_FRAME;
    lOffset   = (lBY * (s_MPEG12Ctx.m_MBWidth << lfField) + lBX) * sizeof(_MPEGMacroBlock8);
    lfIntra   = aMBType & _MPEG_MBT_INTRA;
    lfNoSkip  = aMBAI == 1;
    lfFiledMV = aMotionType & _MPEG_MC_FIELD;

    s_MPEG12Ctx.m_pCurMotions->m_Stride     = s_MPEG12Ctx.m_MBStride;
    s_MPEG12Ctx.m_pCurMotions->m_pMBDstY    = UNCACHED_SEG(s_MPEG12Ctx.m_pCurFrameY + lOffset);
    s_MPEG12Ctx.m_pCurMotions->m_pMBDstCbCr = UNCACHED_SEG(s_MPEG12Ctx.m_pCurFrameCbCr + lOffset);

    lBX <<= 4;
    lBY <<= 4;

    if (!lfIntra) {
        _mpeg12_get_refs(lBX, lBY, aMBType, aMotionType, aPMV, aMVFS, aDMVector);
        if (lfNoSkip && (aMBType & _MPEG_MBT_PATTERN))
            s_MPEG12Ctx.m_pCurMotions->BlockOp = AddBlockOp[lfField][lfFiledMV];
        else {
            s_MPEG12Ctx.m_pCurMotions->m_pSrc  = s_MPEG12Ctx.m_pCurMotions->m_pSPRRes;
            s_MPEG12Ctx.m_pCurMotions->BlockOp = PutBlockOp[!lfField + (lfNoSkip && lfFiledMV && !lfField)];
        }
    } else {
        s_MPEG12Ctx.m_pCurMotions->m_Motion[0].MC_Luma = NULL;
        s_MPEG12Ctx.m_pCurMotions->m_pSrc              = s_MPEG12Ctx.m_pCurMotions->m_pSPRBlk;
        s_MPEG12Ctx.m_pCurMotions->BlockOp             = PutBlockOp[!lfField];
    }
}

/* decode all macroblocks of the current picture */
static int _mpeg12_slice(int aMBAMax)
{
    int lPMV[2][2][2];
    int lMVFS[2][2];
    int lDMVector[2];
    int lMBA, lMBAI, lMBType, lMotionType;
    int retVal;

    s_MPEG12Ctx.m_fError = 0;
    lMBType              = _MPEG_NextStartCode();

    /* only slice headers are allowed in picture_data */
    if (lMBType < _MPEG_CODE_SLICE_MIN || lMBType > _MPEG_CODE_SLICE_MAX)
        return -1;

    _MPEG_GetBits(32);

    s_MPEG12Ctx.m_QScale = _MPEG_GetBits(5);

    if (_MPEG_GetBits(1)) {
        _MPEG_GetBits(8);
        _xtra_bitinf();
    }

    lMBAI = _MPEG_GetMBAI();

    if (lMBAI) {
        lMBA = ((lMBType & 255) - 1) * s_MPEG12Ctx.m_MBWidth + lMBAI - 1;

        lMBAI = s_MPEG12Ctx.m_fDCRst = 1;

        lPMV[0][0][0] = lPMV[0][0][1] = lPMV[1][0][0] = lPMV[1][0][1] = 0;
        lPMV[0][1][0] = lPMV[0][1][1] = lPMV[1][1][0] = lPMV[1][1][1] = 0;
    } else {
        return 0;
    }

    while (1) {
        s_MPEG12Ctx.m_pCurMotions = &s_MPEG12Ctx.m_MC[s_MPEG12Ctx.m_CurMC];

        if (lMBA >= aMBAMax || !_MPEG_WaitBDEC())
            return -1;

        if (!lMBAI) {
            if (!_MPEG_ShowBits(23) || s_MPEG12Ctx.m_fError) {
            resync:
                s_MPEG12Ctx.m_fError = 0;
                return 0;
            } else {
                lMBAI = _MPEG_GetMBAI();

                if (!lMBAI)
                    goto resync;
            }
        }

        if (lMBA >= aMBAMax)
            return -1;

        if (lMBAI == 1) {
            retVal = _mpeg12_dec_mb(&lMBType, &lMotionType, lPMV, lMVFS, lDMVector);

            if (retVal < 0)
                return retVal;

            if (!retVal)
                goto resync;
        } else {
            /* skipped macroblock */
            s_MPEG12Ctx.m_fDCRst = 1;

            if (s_MPEG12Ctx.m_PictCodingType == _MPEG_PT_P) {
                lPMV[0][0][0] = 0;
                lPMV[0][0][1] = 0;
                lPMV[1][0][0] = 0;
                lPMV[1][0][1] = 0;
            }

            if (s_MPEG12Ctx.m_PictStruct == _MPEG_PS_FRAME) {
                lMotionType = _MPEG_MC_FRAME;
            } else {
                lMotionType = _MPEG_MC_FIELD;
                lMVFS[0][0] = s_MPEG12Ctx.m_PictStruct == _MPEG_PS_BOTTOM_FIELD;
                lMVFS[0][1] = s_MPEG12Ctx.m_PictStruct == _MPEG_PS_BOTTOM_FIELD;
            }

            lMBType &= ~_MPEG_MBT_INTRA;
        }

        _mpeg2_mc(lMBA, lMBType, lMotionType, lPMV, lMVFS, lDMVector, lMBAI);

        DoMC();

        ++lMBA;
        --lMBAI;

        s_MPEG12Ctx.m_CurMC ^= 1;

        if (lMBA >= aMBAMax)
            return -1;
    }
}
