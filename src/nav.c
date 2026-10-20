#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <tamtypes.h>
#include <timer.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

#include <dvdnav/dvdnav.h>

#include "nav.h"
#include "frame.h"
#include "memlog.h"

/* ============================================================
 * STREAM (callbacks da libdvdread)
 * ============================================================ */

/*
 * A libdvdnav lê de 1 em 1 setor (2 KB). Pela rede isso é lento,
 * então a leitura passa por um cache de NAV_CACHE bytes. Com
 * prefetch, o bloco seguinte é lido em segundo plano no outro buffer.
 */
#define NAV_CACHE   (128 * 1024)

static u8 nav_buf[2][NAV_CACHE] __attribute__((aligned(64)));

typedef struct
{
    int fd;
    u64 size;
    u64 pos;            /* posição pedida pela libdvdread */
    int prefetch;       /* leitura em segundo plano habilitada */

    int cur;            /* buffer do cache atual (0/1) */
    u64 cache_pos;      /* início do cache na ISO */
    int cache_len;      /* bytes válidos no cache */

    int pf_pending;     /* fileXioRead assíncrono em curso */
    int pf_done;        /* leitura terminou, dados em nav_buf[!cur] */
    u64 pf_pos;
    int pf_len;

    u32 reads;          /* leituras reais (rede) */
    u64 read_bytes;
} NavStream;

static NavStream ns;
static dvdnav_t *nav = NULL;
static int last_buttons = -1;

static u32 spu_clut[16];
static u32 clut_serial = 0;
static int spu_stream = 0;
static int audio_stream = 0;


static u64 cache_end(void)
{
    return ns.cache_pos + (u64)ns.cache_len;
}


/*
 * Conclui a leitura em segundo plano (esperando se wait != 0).
 */
static void pf_poll(int wait)
{
    int result = 0;

    if (!ns.pf_pending)
        return;

    if (fileXioWaitAsync(wait ? FXIO_WAIT : FXIO_NOWAIT, &result) != FXIO_COMPLETE)
        return;

    ns.pf_pending = 0;
    fileXioSetBlockMode(FXIO_WAIT);

    if (result > 0)
    {
        ns.pf_done = 1;
        ns.pf_len = result;
        ns.reads++;
        ns.read_bytes += (u64)result;
    }
    else
    {
        ns.pf_done = 0;
        frame_logf("NAV: prefetch at %llu returned %d\n",
                   (unsigned long long)ns.pf_pos, result);
    }
}


/*
 * Começa a ler o bloco seguinte ao cache (a posição do arquivo já
 * está no fim do cache).
 */
static void pf_start(void)
{
    int r;

    if (!ns.prefetch || ns.pf_pending || ns.cache_len < NAV_CACHE)
        return;

    if (cache_end() >= ns.size)
        return;

    /* mensagens adiadas: escrever antes de ocupar o fileXio */
    frame_log_flush();

    ns.pf_pos = cache_end();
    ns.pf_done = 0;

    fileXioSetBlockMode(FXIO_NOWAIT);

    r = fileXioRead(ns.fd, nav_buf[!ns.cur], NAV_CACHE);

    if (r < 0)
    {
        fileXioSetBlockMode(FXIO_WAIT);
        frame_logf("NAV: prefetch start failed: %d\n", r);
        return;
    }

    ns.pf_pending = 1;
}


static int nav_fill(void)
{
    u64 start = ns.pos & ~(u64)(NAV_BLOCK_SIZE - 1);
    s64 r;
    int n;

    if (ns.pf_pending)
        pf_poll(1);

    if (ns.pf_done && start >= ns.pf_pos && start < ns.pf_pos + (u64)ns.pf_len)
    {
        /* o bloco seguinte já chegou: troca de buffer */
        ns.cur = !ns.cur;
        ns.cache_pos = ns.pf_pos;
        ns.cache_len = ns.pf_len;
        ns.pf_done = 0;

        pf_start();
        return ns.cache_len;
    }

    /* salto: leitura síncrona */
    ns.pf_done = 0;

    r = fileXioLseek64(ns.fd, (s64)start, SEEK_SET);
    if (r != (s64)start)
    {
        frame_logf("NAV: seek %llu failed (%lld)\n",
                   (unsigned long long)start, (long long)r);
        return -1;
    }

    n = fileXioRead(ns.fd, nav_buf[ns.cur], NAV_CACHE);
    if (n < 0)
    {
        frame_logf("NAV: read at %llu returned %d\n",
                   (unsigned long long)start, n);
        return -1;
    }

    ns.cache_pos = start;
    ns.cache_len = n;
    ns.reads++;
    ns.read_bytes += (u64)n;

    pf_start();
    return n;
}


static int nav_seek(void *p, uint64_t pos)
{
    (void)p;

    if (pos > ns.size)
        return -1;

    ns.pos = pos;
    return 0;
}


static int nav_read(void *p, void *buffer, int len)
{
    u8 *dst = buffer;
    int done = 0;

    (void)p;

    while (done < len)
    {
        int n;

        if (ns.cache_len <= 0 || ns.pos < ns.cache_pos || ns.pos >= cache_end())
        {
            n = nav_fill();
            if (n < 0)
                return done ? done : -1;
            if (n == 0 || ns.pos >= cache_end())
                break;              /* fim da ISO */
        }

        n = (int)(cache_end() - ns.pos);
        if (n > len - done)
            n = len - done;

        memcpy(dst + done, nav_buf[ns.cur] + (ns.pos - ns.cache_pos), n);
        done += n;
        ns.pos += (u64)n;
    }

    return done;
}


int nav_ready(void)
{
    if (!nav)
        return 0;

    pf_poll(0);

    if (ns.cache_len > 0 && ns.pos >= ns.cache_pos && ns.pos < cache_end())
        return 1;

    return !ns.pf_pending;
}


int nav_io_pending(void)
{
    return ns.pf_pending;
}


void nav_wait_io(void)
{
    pf_poll(1);
}


int nav_buttons(void)
{
    return last_buttons > 0 ? last_buttons : 0;
}


/* ============================================================
 * BOTÕES
 * ============================================================ */

static pci_t *menu_pci(void)
{
    pci_t *pci;

    if (!nav)
        return NULL;

    pci = dvdnav_get_current_nav_pci(nav);

    if (!pci || pci->hli.hl_gi.hli_ss == 0 || pci->hli.hl_gi.btn_ns == 0)
        return NULL;

    return pci;
}


int nav_button_rect(int *button, int *x0, int *y0, int *x1, int *y1)
{
    pci_t *pci = menu_pci();
    int32_t b = 0;
    btni_t *bt;

    if (!pci)
        return 0;

    dvdnav_get_current_highlight(nav, &b);

    if (b < 1 || b > pci->hli.hl_gi.btn_ns || b > 36)
        return 0;

    bt = &pci->hli.btnit[b - 1];

    *button = (int)b;
    *x0 = bt->x_start;
    *y0 = bt->y_start;
    *x1 = bt->x_end;
    *y1 = bt->y_end;
    return 1;
}


int nav_button_move(int dir)
{
    pci_t *pci = menu_pci();
    dvdnav_status_t st;
    int32_t b = 0;

    if (!pci)
        return 0;

    switch (dir)
    {
        case NAV_UP:    st = dvdnav_upper_button_select(nav, pci); break;
        case NAV_DOWN:  st = dvdnav_lower_button_select(nav, pci); break;
        case NAV_LEFT:  st = dvdnav_left_button_select(nav, pci);  break;
        default:        st = dvdnav_right_button_select(nav, pci); break;
    }

    dvdnav_get_current_highlight(nav, &b);
    frame_logf("NAV: button move %d -> %d (%s)\n", dir, (int)b,
               st == DVDNAV_STATUS_OK ? "ok" : dvdnav_err_to_string(nav));

    return st == DVDNAV_STATUS_OK;
}


int nav_button_activate(void)
{
    pci_t *pci = menu_pci();
    dvdnav_status_t st;
    int32_t b = 0;

    if (!pci)
        return 0;

    /* a VM pode abrir outro IFO: fileXio livre antes */
    nav_wait_io();

    dvdnav_get_current_highlight(nav, &b);
    st = dvdnav_button_activate(nav, pci);

    frame_logf("NAV: button %d activated (%s)\n", (int)b,
               st == DVDNAV_STATUS_OK ? "ok" : dvdnav_err_to_string(nav));

    last_buttons = -1;
    return st == DVDNAV_STATUS_OK;
}


const u32 *nav_clut(void)
{
    return spu_clut;
}


u32 nav_clut_serial(void)
{
    return clut_serial;
}


int nav_audio_stream(void)
{
    return audio_stream;
}


int nav_spu_stream(void)
{
    return spu_stream;
}


int nav_button_palette(int action, u32 *palette)
{
    pci_t *pci = menu_pci();
    int32_t b = 0;
    int coln;

    if (!pci)
        return 0;

    dvdnav_get_current_highlight(nav, &b);

    if (b < 1 || b > pci->hli.hl_gi.btn_ns || b > 36)
        return 0;

    coln = pci->hli.btnit[b - 1].btn_coln;

    if (coln < 1 || coln > 3)
        return 0;

    *palette = pci->hli.btn_colit.btn_coli[coln - 1][action ? 1 : 0];
    return 1;
}


u64 nav_bytes_read(void)
{
    return ns.read_bytes;
}


/* ============================================================
 * LOG DA LIBDVDNAV / LIBDVDREAD
 * ============================================================ */

static void nav_log(void *p, dvdnav_logger_level_t level,
                    const char *fmt, va_list args)
{
    static const char *names[] = { "INFO", "ERROR", "WARN", "DEBUG" };
    char msg[160];

    (void)p;

    if (level == DVDNAV_LOGGER_LEVEL_DEBUG)
        return;

    vsnprintf(msg, sizeof(msg), fmt, args);
    frame_logf("DVDNAV %s: %s\n", names[level & 3], msg);
}


/* ============================================================
 * ABERTURA / EVENTOS
 * ============================================================ */

static const char *dom_str(int d)
{
    switch (d)
    {
        case DVD_DOMAIN_FirstPlay: return "FirstPlay";
        case DVD_DOMAIN_VTSTitle:  return "Title";
        case DVD_DOMAIN_VMGM:      return "VMGM";
        case DVD_DOMAIN_VTSMenu:   return "VTSM";
    }
    return "?";
}


static int cur_domain(void)
{
    if (dvdnav_is_domain_fp(nav))   return DVD_DOMAIN_FirstPlay;
    if (dvdnav_is_domain_vmgm(nav)) return DVD_DOMAIN_VMGM;
    if (dvdnav_is_domain_vtsm(nav)) return DVD_DOMAIN_VTSMenu;
    if (dvdnav_is_domain_vts(nav))  return DVD_DOMAIN_VTSTitle;
    return 0;
}


int nav_open(const char *path, u64 size, int prefetch)
{
    static const dvdnav_logger_cb logcb = { nav_log };
    static dvdnav_stream_cb stream_cb = { nav_seek, nav_read, NULL };

    nav_close();

    memset(&ns, 0, sizeof(ns));
    ns.size = size;
    ns.prefetch = prefetch;
    last_buttons = -1;
    spu_stream = 0;
    audio_stream = 0;

    fileXioSetBlockMode(FXIO_WAIT);
    fileXioSetRWBufferSize(NAV_CACHE);

    ns.fd = fileXioOpen(path, FIO_O_RDONLY, 0);

    if (ns.fd < 0)
    {
        frame_logf("NAV: open %s failed (%d)\n", path, ns.fd);
        return -1;
    }

    if (dvdnav_open_stream2(&nav, &ns, &logcb, &stream_cb) != DVDNAV_STATUS_OK)
    {
        frame_logf("NAV: dvdnav_open_stream2 failed\n");
        nav = NULL;
        nav_wait_io();
        fileXioClose(ns.fd);
        ns.fd = -1;
        return -2;
    }

    dvdnav_set_readahead_flag(nav, 0);
    dvdnav_set_PGC_positioning_flag(nav, 1);
    dvdnav_menu_language_select(nav, "pt");
    dvdnav_audio_language_select(nav, "pt");
    dvdnav_spu_language_select(nav, "pt");

    return 0;
}


void nav_close(void)
{
    nav_wait_io();

    if (nav)
    {
        dvdnav_close(nav);
        nav = NULL;
    }

    if (ns.fd >= 0 && ns.size)
    {
        fileXioSetBlockMode(FXIO_WAIT);
        fileXioClose(ns.fd);
    }

    ns.fd = -1;
    ns.size = 0;
}


void nav_still_skip(void)
{
    if (nav)
        dvdnav_still_skip(nav);
}


int nav_next(u8 *buf, NavEvent *ev)
{
    int32_t event = 0;
    int32_t len = 0;

    memset(ev, 0, sizeof(*ev));

    if (!nav)
        return NAV_EV_ERROR;

    if (dvdnav_get_next_block(nav, buf, &event, &len) != DVDNAV_STATUS_OK)
    {
        frame_logf("NAV: get_next_block error: %s\n", dvdnav_err_to_string(nav));
        return NAV_EV_ERROR;
    }

    switch (event)
    {
        case DVDNAV_BLOCK_OK:
            return NAV_EV_BLOCK;

        case DVDNAV_STILL_FRAME:
        {
            dvdnav_still_event_t *e = (dvdnav_still_event_t *)buf;
            pci_t *pci = dvdnav_get_current_nav_pci(nav);

            ev->still_len = e->length;
            ev->domain = cur_domain();
            ev->buttons = pci ? pci->hli.hl_gi.btn_ns : 0;

            frame_logf("NAV: STILL length=%d%s (%s, buttons=%d)\n",
                       e->length, e->length == 0xff ? " (infinite)" : "",
                       dom_str(ev->domain), ev->buttons);
            return NAV_EV_STILL;
        }

        case DVDNAV_WAIT:
            /* sem fila de áudio/vídeo para esvaziar: segue */
            dvdnav_wait_skip(nav);
            return NAV_EV_NONE;

        case DVDNAV_VTS_CHANGE:
        {
            dvdnav_vts_change_event_t *e = (dvdnav_vts_change_event_t *)buf;

            frame_logf("NAV: VTS_CHANGE %d/%s -> %d/%s\n",
                       e->old_vtsN, dom_str(e->old_domain),
                       e->new_vtsN, dom_str(e->new_domain));
            return NAV_EV_NONE;
        }

        case DVDNAV_CELL_CHANGE:
        {
            dvdnav_cell_change_event_t *e = (dvdnav_cell_change_event_t *)buf;
            int32_t title = 0, part = 0;

            dvdnav_current_title_info(nav, &title, &part);

            frame_logf("NAV: CELL %s title=%d part=%d cell=%d pg=%d len=%lld sectors\n",
                       dom_str(cur_domain()), (int)title, (int)part,
                       e->cellN, e->pgN, (long long)e->cell_length);
            last_buttons = -1;
            return NAV_EV_NONE;
        }

        case DVDNAV_NAV_PACKET:
        {
            pci_t *pci = dvdnav_get_current_nav_pci(nav);
            int n = pci ? pci->hli.hl_gi.btn_ns : 0;

            if (n != last_buttons)
            {
                frame_logf("NAV: buttons=%d (%s)\n", n, dom_str(cur_domain()));
                last_buttons = n;
            }
            return NAV_EV_NONE;
        }

        case DVDNAV_STOP:
            frame_logf("NAV: STOP\n");
            return NAV_EV_STOP;

        case DVDNAV_HOP_CHANNEL:
            frame_logf("NAV: HOP_CHANNEL\n");
            return NAV_EV_NONE;

        case DVDNAV_SPU_CLUT_CHANGE:
            memcpy(spu_clut, buf, sizeof(spu_clut));
            clut_serial++;
            return NAV_EV_NONE;

        case DVDNAV_AUDIO_STREAM_CHANGE:
        {
            dvdnav_audio_stream_change_event_t *e =
                (dvdnav_audio_stream_change_event_t *)buf;
            int s = e->physical >= 0 ? (e->physical & 7) : 0;

            if (s != audio_stream)
                frame_logf("NAV: audio stream %d\n", s);

            audio_stream = s;
            return NAV_EV_NONE;
        }

        case DVDNAV_SPU_STREAM_CHANGE:
        {
            dvdnav_spu_stream_change_event_t *e =
                (dvdnav_spu_stream_change_event_t *)buf;

            spu_stream = e->physical_wide >= 0 ? (e->physical_wide & 0x1F) : 0;
            return NAV_EV_NONE;
        }

        case DVDNAV_NOP:
        case DVDNAV_HIGHLIGHT:
            return NAV_EV_NONE;

        default:
            frame_logf("NAV: event %d\n", (int)event);
            return NAV_EV_NONE;
    }
}


/* ============================================================
 * TESTE DE EVENTOS (R1)
 * ============================================================ */

#define NAV_MAX_BLOCKS  40000   /* ~80 MB */
#define NAV_MAX_STILLS  20

static u8 test_block[NAV_BLOCK_SIZE] __attribute__((aligned(64)));


static u32 ms_since(u64 t0)
{
    return (u32)(((GetTimerSystemTime() - t0) * 1000ULL) / kBUSCLK);
}


int nav_test(const char *path, u64 size)
{
    u64 t0 = GetTimerSystemTime();
    u32 blocks = 0;
    int stills = 0;
    int done = 0;
    int result = 0;

    printf("\n");
    printf("==============================\n");
    printf(" DVDNAV EVENT TEST\n");
    printf("==============================\n");
    printf("NAV: path=%s\n", path);
    mem_log("nav open");

    /* sem prefetch: o teste usa printf livremente */
    if (nav_open(path, size, 0) < 0)
    {
        frame_log_flush();
        return -1;
    }

    frame_log_flush();
    printf("NAV: opened in %u ms (%u reads)\n", ms_since(t0), ns.reads);

    while (!done)
    {
        NavEvent ev;
        int r = nav_next(test_block, &ev);

        frame_log_flush();

        switch (r)
        {
            case NAV_EV_BLOCK:
                if (++blocks >= NAV_MAX_BLOCKS)
                {
                    printf("NAV: block limit - stop\n");
                    done = 1;
                }
                break;

            case NAV_EV_STILL:
                if (ev.buttons > 0 || ++stills > NAV_MAX_STILLS)
                {
                    printf("NAV: still with buttons / too many stills - stop\n");
                    done = 1;
                    break;
                }
                nav_still_skip();
                break;

            case NAV_EV_STOP:
                done = 1;
                break;

            case NAV_EV_ERROR:
                result = -3;
                done = 1;
                break;
        }
    }

    printf("NAV: end: %u blocks, %u network reads (%llu KB), %u ms\n",
           blocks, ns.reads, (unsigned long long)(ns.read_bytes / 1024), ms_since(t0));

    nav_close();
    frame_log_flush();
    mem_log("nav closed");
    printf("==============================\n");

    return result;
}
