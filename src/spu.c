#include <string.h>

#include <tamtypes.h>

#include "spu.h"
#include "frame.h"

/*
 * Pacote SPU: até 64 KB (tamanho em 16 bits no início).
 */
#define SPU_PKT_MAX   65536
#define SPU_QUEUE     4

typedef struct
{
    u8 data[SPU_PKT_MAX];
    int size;
    u32 pos;
} SpuPacket;

static u8 asm_buf[SPU_PKT_MAX];
static int asm_len = 0;
static int asm_active = 0;

static SpuPacket queue[SPU_QUEUE];
static int q_head = 0;
static int q_count = 0;

static u8 bitmap[SPU_MAX_W * SPU_MAX_H] __attribute__((aligned(128)));
static SpuInfo info;


static int be16(const u8 *p)
{
    return (p[0] << 8) | p[1];
}


void spu_reset(void)
{
    u32 serial = info.serial;

    asm_len = 0;
    asm_active = 0;
    q_head = 0;
    q_count = 0;

    memset(&info, 0, sizeof(info));
    info.serial = serial + 1;
}


const SpuInfo *spu_info(void)
{
    return &info;
}


const u8 *spu_bitmap(void)
{
    return bitmap;
}


/* ============================================================
 * REMONTAGEM
 * ============================================================ */

static void enqueue(const u8 *data, int size, u32 pos)
{
    SpuPacket *pk;

    if (q_count == SPU_QUEUE)
    {
        /* fila cheia: descarta o mais antigo */
        q_head = (q_head + 1) % SPU_QUEUE;
        q_count--;
    }

    pk = &queue[(q_head + q_count) % SPU_QUEUE];
    memcpy(pk->data, data, size);
    pk->size = size;
    pk->pos = pos;
    q_count++;
}


void spu_feed(const u8 *data, int len, int has_pts, u32 pos)
{
    if (len <= 0)
        return;

    if (has_pts)
    {
        asm_len = 0;
        asm_active = 1;
    }

    if (!asm_active)
        return;

    if (asm_len + len > SPU_PKT_MAX)
    {
        asm_len = 0;
        asm_active = 0;
        return;
    }

    memcpy(asm_buf + asm_len, data, len);
    asm_len += len;

    if (asm_len >= 4)
    {
        int size = be16(asm_buf);

        if (size < 4)
        {
            asm_len = 0;
            asm_active = 0;
            return;
        }

        if (asm_len >= size)
        {
            enqueue(asm_buf, size, pos);
            asm_len = 0;
            asm_active = 0;
        }
    }
}


/* ============================================================
 * DECODIFICAÇÃO
 * ============================================================ */

typedef struct
{
    const u8 *p;
    int nib;        /* posição em nibbles */
    int end;        /* limite em nibbles */
} NibReader;


static int get_nib(NibReader *r)
{
    int v;

    if (r->nib >= r->end)
        return 0;

    v = r->p[r->nib >> 1];
    v = (r->nib & 1) ? (v & 0x0F) : (v >> 4);
    r->nib++;
    return v;
}


static void decode_rle(const u8 *p, int size, int top, int bottom,
                       int x0, int y0, int w, int h)
{
    NibReader field[2];
    int y;

    field[0].p = p;
    field[0].nib = top * 2;
    field[0].end = size * 2;
    field[1].p = p;
    field[1].nib = bottom * 2;
    field[1].end = size * 2;

    for (y = 0; y < h; y++)
    {
        NibReader *r = &field[y & 1];
        int line = y0 + y;
        u8 *out = (line >= 0 && line < SPU_MAX_H) ? bitmap + line * SPU_MAX_W : NULL;
        int x = 0;

        while (x < w)
        {
            int v = get_nib(r);
            int count;
            int color;
            int i;

            if (v < 0x4)
            {
                v = (v << 4) | get_nib(r);
                if (v < 0x10)
                {
                    v = (v << 4) | get_nib(r);
                    if (v < 0x40)
                        v = (v << 4) | get_nib(r);
                }
            }

            count = v >> 2;
            color = v & 3;

            if (count == 0 || x + count > w)
                count = w - x;

            if (out)
            {
                for (i = 0; i < count; i++)
                {
                    int px = x0 + x + i;

                    if (px >= 0 && px < SPU_MAX_W)
                        out[px] = (u8)color;
                }
            }

            x += count;

            if (r->nib >= r->end)
                break;
        }

        /* cada linha começa num byte inteiro */
        if (r->nib & 1)
            r->nib++;
    }
}


static void decode_packet(const u8 *p, int size)
{
    int dcsq = be16(p + 2);
    int off = dcsq;
    int top = -1;
    int bottom = -1;
    int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
    int guard;

    info.visible = 0;
    info.forced = 0;

    for (guard = 0; guard < 16 && off + 4 <= size; guard++)
    {
        int next = be16(p + off + 2);
        int i = off + 4;
        int done = 0;

        while (!done && i < size)
        {
            int cmd = p[i++];

            switch (cmd)
            {
                case 0x00:      /* forced start display */
                    info.forced = 1;
                    info.visible = 1;
                    break;

                case 0x01:      /* start display */
                    info.visible = 1;
                    break;

                case 0x02:      /* stop display: ignorado por enquanto */
                    break;

                case 0x03:      /* cores: e2 e1 p b */
                    if (i + 2 > size) { done = 1; break; }
                    info.color[3] = p[i] >> 4;
                    info.color[2] = p[i] & 0x0F;
                    info.color[1] = p[i + 1] >> 4;
                    info.color[0] = p[i + 1] & 0x0F;
                    i += 2;
                    break;

                case 0x04:      /* contraste: e2 e1 p b */
                    if (i + 2 > size) { done = 1; break; }
                    info.alpha[3] = p[i] >> 4;
                    info.alpha[2] = p[i] & 0x0F;
                    info.alpha[1] = p[i + 1] >> 4;
                    info.alpha[0] = p[i + 1] & 0x0F;
                    i += 2;
                    break;

                case 0x05:      /* área: x0 x1 (12 bits) y0 y1 */
                    if (i + 6 > size) { done = 1; break; }
                    x0 = (p[i] << 4) | (p[i + 1] >> 4);
                    x1 = ((p[i + 1] & 0x0F) << 8) | p[i + 2];
                    y0 = (p[i + 3] << 4) | (p[i + 4] >> 4);
                    y1 = ((p[i + 4] & 0x0F) << 8) | p[i + 5];
                    i += 6;
                    break;

                case 0x06:      /* offsets dos campos */
                    if (i + 4 > size) { done = 1; break; }
                    top = be16(p + i);
                    bottom = be16(p + i + 2);
                    i += 4;
                    break;

                case 0x07:      /* troca de cor por região: pula */
                    if (i + 2 > size) { done = 1; break; }
                    i += be16(p + i);
                    break;

                case 0xFF:
                default:
                    done = 1;
                    break;
            }
        }

        if (next == off)
            break;

        off = next;
    }

    memset(bitmap, SPU_NONE, sizeof(bitmap));

    info.x0 = x0;
    info.y0 = y0;
    info.x1 = x1;
    info.y1 = y1;

    if (top >= 4 && bottom >= 4 && x1 >= x0 && y1 >= y0 &&
        top < dcsq && bottom < dcsq)
    {
        decode_rle(p, dcsq, top, bottom, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }
    else
    {
        info.visible = 0;
    }

    info.serial++;

    frame_logf("SPU: %dx%d at (%d,%d) %s%s colors %d/%d/%d/%d alpha %d/%d/%d/%d\n",
               x1 - x0 + 1, y1 - y0 + 1, x0, y0,
               info.visible ? "visible" : "hidden",
               info.forced ? " forced" : "",
               info.color[0], info.color[1], info.color[2], info.color[3],
               info.alpha[0], info.alpha[1], info.alpha[2], info.alpha[3]);
}


int spu_update(u32 consumed)
{
    int changed = 0;

    while (q_count > 0 && (s32)(consumed - queue[q_head].pos) >= 0)
    {
        decode_packet(queue[q_head].data, queue[q_head].size);
        q_head = (q_head + 1) % SPU_QUEUE;
        q_count--;
        changed = 1;
    }

    return changed;
}
