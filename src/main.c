#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <tamtypes.h>
#include <kernel.h>
#include <timer.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libpad.h>

#include "smb.h"
#include "iso.h"
#include "frame.h"
#include "memlog.h"
#include "nav.h"
#include "spu.h"
#include "audio.h"
#include "input.h"
#include "memcard.h"
#include "network.h"

#include "bdm.h"
#include "iop_boot.h"

/* ============================================================
 * CORES
 * ============================================================ */

static u64 COLOR_BG;
static u64 COLOR_WHITE;
static u64 COLOR_SELECTED;
static u64 COLOR_PANEL;
static u64 COLOR_OK;
static u64 COLOR_ERROR;
static u64 COLOR_DISABLED;


/* ============================================================
 * TELAS
 * ============================================================ */

typedef enum
{
    SCREEN_MAIN = 0,
    SCREEN_SMB,
    SCREEN_ISO_LIST,
    SCREEN_ISO_INFO,
    SCREEN_FRAME

} Screen;

static Screen current_screen = SCREEN_MAIN;


/* ============================================================
 * MENU PRINCIPAL
 * ============================================================ */

static int main_selected = 0;


/* ============================================================
 * MENU SMB
 * ============================================================ */

enum
{
    SMB_ITEM_MODE = 0,
    SMB_ITEM_IP,
    SMB_ITEM_MASK,
    SMB_ITEM_GATEWAY,
    SMB_ITEM_DNS,
    SMB_ITEM_SERVER,
    SMB_ITEM_APPLY,
    SMB_ITEM_BACK,

    SMB_ITEM_COUNT
};

static int smb_selected = 0;

static NetworkMode smb_mode = NETWORK_DHCP;


/*
 * Valores editáveis:
 *
 * 0 = IP
 * 1 = MASK
 * 2 = GATEWAY
 * 3 = DNS
 * 4 = SERVIDOR SMB (porta 445)
 */

#define SMB_SERVER_ROW 4

static int static_values[5][4] =
{
    {192, 168, 1, 50},
    {255, 255, 255, 0},
    {192, 168, 1, 1},
    {192, 168, 1, 1},
    {192, 168, 1, 9}
};


static int smb_editing = 0;
static int smb_octet = 0;

static int smb_hold_frames = 0;
static int smb_repeat_frames = 0;

static int input_result = -99;
static int network_result = -1;


/* ============================================================
 * STATUS SMB
 * ============================================================ */

/*
 * 0  = pronto
 * 1  = aguardando rede / inicializando
 * 2  = conectando SMB
 * 3  = conectado
 * 4  = listando DVD
 * 5  = concluído
 * -1 = erro
 */

static int smb_test_state = 0;


/*
 * Espera assíncrona pela rede.
 */

static int smb_waiting_network = 0;
static int smb_wait_frames = 0;


/*
 * Aproximadamente 30 segundos em 60 FPS.
 */

#define SMB_NETWORK_TIMEOUT_FRAMES 1800


/*
 * Mensagem de erro exibida na tela SMB
 * (etapa + código, vinda de smb_get_info()->error).
 */

static char smb_error_line[128] = "";


/* ============================================================
 * LISTA DE ISOs
 * ============================================================ */

#define ISO_LIST_VISIBLE 8

static int iso_selected = 0;
static int iso_scroll = 0;


/*
 * Biblioteca: lista de ISOs da fonte escolhida (SMB, USB ou HDD).
 * A lista, os detalhes e o player usam só lib_dir + nome; a leitura
 * (fileXio) é a mesma para todas as fontes.
 */
enum { LIB_SMB = 0, LIB_USB, LIB_HDD };

#define LIB_MAX_FILES 64
#define LIB_MAX_NAME  128
#define LIB_PATH_MAX  (16 + 8 + LIB_MAX_NAME)

static int lib_source = LIB_SMB;
static char lib_dir[24];                /* ex.: "usb0:/FILMES" */
static char lib_label[96];              /* cabeçalho da lista */
static int lib_count = 0;

static struct
{
    char name[LIB_MAX_NAME];
    u64 size;
} lib_files[LIB_MAX_FILES];


/*
 * Resultado da identificação da ISO selecionada.
 */

static IsoInfo iso_info;
static int iso_info_result = 0;
static char iso_info_name[LIB_MAX_NAME];
static u64 iso_info_size = 0;


/* Caminho completo da ISO selecionada na fonte atual. */
static void lib_iso_path(char *out, int size)
{
    snprintf(out, size, "%s/%s", lib_dir, iso_info_name);
}


/* Abre a lista de ISOs já preenchida. */
static void lib_show(void)
{
    iso_selected = 0;
    iso_scroll = 0;
    current_screen = SCREEN_ISO_LIST;

    printf("LIBRARY: %s, %d ISO file(s) in %s\n", lib_label, lib_count, lib_dir);
}


/* Biblioteca a partir da listagem do SMB (smb_list_dvd). */
static void lib_from_smb(void)
{
    const SMBInfo *info = smb_get_info();
    int i;

    lib_source = LIB_SMB;
    lib_count = 0;
    snprintf(lib_dir, sizeof(lib_dir), "%s", SMB_DVD_PATH);
    snprintf(lib_label, sizeof(lib_label), "SMB %.24s/%.32s  /FILMES", info->server, info->share);

    for (i = 0; i < info->file_count && i < LIB_MAX_FILES; i++)
    {
        snprintf(lib_files[i].name, sizeof(lib_files[i].name), "%s", info->files[i].name);
        lib_files[i].size = info->files[i].size;
        lib_count++;
    }
}


/* Biblioteca a partir da listagem do BDM (USB/HDD). */
static void lib_from_bdm(int kind)
{
    const BDMInfo *info = bdm_get_info(kind);
    int i;

    lib_source = kind == BDM_USB ? LIB_USB : LIB_HDD;
    lib_count = 0;
    snprintf(lib_dir, sizeof(lib_dir), "%s" BDM_DVD_DIR, info->root);
    snprintf(lib_label, sizeof(lib_label), "%s %s  /FILMES", bdm_kind_name(kind), info->root);

    for (i = 0; i < info->file_count && i < LIB_MAX_FILES; i++)
    {
        snprintf(lib_files[i].name, sizeof(lib_files[i].name), "%s", info->files[i].name);
        lib_files[i].size = info->files[i].size;
        lib_count++;
    }
}


/*
 * Teste de velocidade:
 * 0 = não executado, 1 = pedido (desenha "RUNNING" e executa
 * no próximo frame), 2 = concluído, -1 = erro.
 */

static int speed_state = 0;
static int nav_state = 0;        /* teste dvdnav: 0, 1 rodando, 2 ok, -1 erro */
static IsoSpeedResult speed_results[ISO_SPEED_TESTS];


/*
 * Teste de vídeo (primeiro quadro):
 * 0 = inativo, 1 = pedido (desenha "LOADING" e executa no
 * próximo frame), 2 = quadro na tela, 3 = próximo quadro
 * pedido, -1 = erro, 4 = fim do trecho lido.
 */

static int frame_state = 0;
static FrameInfo frame_info;
static int play_from_start = 1;     /* 1 = First Play, 0 = título principal */

/*
 * Player: 1 = abrindo, 5 = buffering, 2 = tocando, 6 = pausado,
 * 4 = fim, -1 = erro (0 = inativo).
 */
static u64 play_next_due = 0;       /* instante do próximo quadro */
static int play_late = 0;           /* vezes que o relógio foi reajustado */
static int play_rebuffer = 0;       /* vezes que o buffer secou */
static int play_skipped = 0;        /* quadros atrasados não mostrados */
static u64 fps_t0 = 0;
static int fps_frames = 0;
static int fps_x10 = 0;             /* quadros/s x10, último segundo */
static int play_stats_due = 0;      /* imprimir quando a leitura parar */
static char play_osd[48];           /* aviso na tela (ex.: capítulo) */
static u64 play_osd_until = 0;

/*
 * Barra de controle do player (SELECT mostra/esconde).
 */
enum { BAR_PREV = 0, BAR_PLAY, BAR_NEXT, BAR_MENU, BAR_STOP, BAR_COUNT };

#define PLAY_BAR_SECONDS 5          /* some sozinha sem uso (tocando) */

static int play_bar = 0;
static int play_bar_sel = BAR_PLAY;
static u64 play_bar_until = 0;
static u16 *frame_pixels = NULL;
static int frame_upload = 0;
static GSTEXTURE frame_tex;
static int frame_tex_ready = 0;


/* ============================================================
 * PROTÓTIPOS
 * ============================================================ */

static void smb_test(void);


/* ============================================================
 * UTILITÁRIOS
 * ============================================================ */

static void make_ip_string(
    char *buffer,
    int row
)
{
    sprintf(
        buffer,
        "%d.%d.%d.%d",
        static_values[row][0],
        static_values[row][1],
        static_values[row][2],
        static_values[row][3]
    );
}


static void load_current_network_values(void)
{
    const NetworkInfo *net;

    net = network_get_info();

    if (!net)
        return;


    if (net->mode == NETWORK_STATIC)
    {
        unsigned int a;
        unsigned int b;
        unsigned int c;
        unsigned int d;


        if (sscanf(
                net->ip,
                "%u.%u.%u.%u",
                &a,
                &b,
                &c,
                &d
            ) == 4)
        {
            static_values[0][0] = a;
            static_values[0][1] = b;
            static_values[0][2] = c;
            static_values[0][3] = d;
        }


        if (sscanf(
                net->netmask,
                "%u.%u.%u.%u",
                &a,
                &b,
                &c,
                &d
            ) == 4)
        {
            static_values[1][0] = a;
            static_values[1][1] = b;
            static_values[1][2] = c;
            static_values[1][3] = d;
        }


        if (sscanf(
                net->gateway,
                "%u.%u.%u.%u",
                &a,
                &b,
                &c,
                &d
            ) == 4)
        {
            static_values[2][0] = a;
            static_values[2][1] = b;
            static_values[2][2] = c;
            static_values[2][3] = d;
        }


        if (sscanf(
                net->dns,
                "%u.%u.%u.%u",
                &a,
                &b,
                &c,
                &d
            ) == 4)
        {
            static_values[3][0] = a;
            static_values[3][1] = b;
            static_values[3][2] = c;
            static_values[3][3] = d;
        }
    }
}


/* ============================================================
 * DESENHO DA TELA PRINCIPAL
 * ============================================================ */

static void draw_main(
    GSGLOBAL *gsGlobal,
    GSFONTM *font
)
{
    const char *usb_text = "  USB";
    const char *hdd_text = "  HDD";
    const char *smb_text = "  SMB";


    gsKit_clear(
        gsGlobal,
        COLOR_BG
    );


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        220.0f,
        45.0f,
        2,
        1.0f,
        COLOR_WHITE,
        "PS2DVDPlayer"
    );


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        245.0f,
        82.0f,
        2,
        0.65f,
        COLOR_WHITE,
        "DVD ISO Player"
    );


    gsKit_prim_sprite(
        gsGlobal,
        170.0f,
        130.0f,
        470.0f,
        325.0f,
        2,
        COLOR_PANEL
    );


    if (main_selected == 0)
    {
        gsKit_prim_sprite(
            gsGlobal,
            195.0f,
            155.0f,
            445.0f,
            200.0f,
            3,
            COLOR_SELECTED
        );
    }
    else if (main_selected == 1)
    {
        gsKit_prim_sprite(
            gsGlobal,
            195.0f,
            205.0f,
            445.0f,
            250.0f,
            3,
            COLOR_SELECTED
        );
    }
    else
    {
        gsKit_prim_sprite(
            gsGlobal,
            195.0f,
            255.0f,
            445.0f,
            300.0f,
            3,
            COLOR_SELECTED
        );
    }


    if (main_selected == 0)
        usb_text = "> USB";

    if (main_selected == 1)
        hdd_text = "> HDD";

    if (main_selected == 2)
        smb_text = "> SMB";


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        220.0f,
        166.0f,
        4,
        0.75f,
        COLOR_WHITE,
        usb_text
    );


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        220.0f,
        216.0f,
        4,
        0.75f,
        COLOR_WHITE,
        hdd_text
    );


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        220.0f,
        266.0f,
        4,
        0.75f,
        COLOR_WHITE,
        smb_text
    );


    {
        const NetworkInfo *net;

        net = network_get_info();

        if (net && net->initialized)
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                195.0f,
                355.0f,
                2,
                0.45f,
                net->link_up
                    ? COLOR_OK
                    : COLOR_ERROR,
                net->link_up
                    ? "Network: CONNECTED"
                    : "Network: DISCONNECTED"
            );
        }
        else
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                195.0f,
                355.0f,
                2,
                0.45f,
                COLOR_ERROR,
                "Network: NOT INITIALIZED"
            );
        }
    }


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        195.0f,
        390.0f,
        2,
        0.45f,
        COLOR_WHITE,
        "UP/DOWN = Select"
    );


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        195.0f,
        415.0f,
        2,
        0.45f,
        COLOR_WHITE,
        "CROSS = Enter"
    );
}


/* ============================================================
 * DESENHO DE IP
 * ============================================================ */

static void draw_editable_ip(
    GSGLOBAL *gsGlobal,
    GSFONTM *font,
    float x,
    float y,
    int row,
    int enabled
)
{
    int i;
    int editing_row = -1;
    char number[8];


    if (smb_selected == SMB_ITEM_IP)
        editing_row = 0;
    else if (smb_selected == SMB_ITEM_MASK)
        editing_row = 1;
    else if (smb_selected == SMB_ITEM_GATEWAY)
        editing_row = 2;
    else if (smb_selected == SMB_ITEM_DNS)
        editing_row = 3;
    else if (smb_selected == SMB_ITEM_SERVER)
        editing_row = SMB_SERVER_ROW;


    for (i = 0; i < 4; i++)
    {
        sprintf(
            number,
            "%03d",
            static_values[row][i]
        );


        if (
            enabled &&
            smb_editing &&
            row == editing_row &&
            i == smb_octet
        )
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                x + (i * 55.0f),
                y,
                2,
                0.48f,
                COLOR_ERROR,
                number
            );
        }
        else
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                x + (i * 55.0f),
                y,
                2,
                0.48f,
                enabled
                    ? COLOR_WHITE
                    : COLOR_DISABLED,
                number
            );
        }


        if (i < 3)
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                x + (i * 55.0f) + 42.0f,
                y,
                2,
                0.48f,
                enabled
                    ? COLOR_WHITE
                    : COLOR_DISABLED,
                "."
            );
        }
    }
}


/* ============================================================
 * DESENHO DA TELA SMB
 * ============================================================ */

static void draw_smb(
    GSGLOBAL *gsGlobal,
    GSFONTM *font
)
{
    const NetworkInfo *net;

    net = network_get_info();


    gsKit_clear(
        gsGlobal,
        COLOR_BG
    );


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        220.0f,
        30.0f,
        2,
        0.85f,
        COLOR_WHITE,
        "SMB NETWORK"
    );


    gsKit_prim_sprite(
        gsGlobal,
        90.0f,
        70.0f,
        550.0f,
        450.0f,
        2,
        COLOR_PANEL
    );


    /*
     * Barra de seleção.
     */
    {
        float x1 = 110.0f;
        float x2 = 530.0f;
        float y1;
        float y2;

        if (smb_selected == SMB_ITEM_MODE)
        {
            y1 = 85.0f;
            y2 = 115.0f;
        }
        else if (smb_selected == SMB_ITEM_IP)
        {
            y1 = 120.0f;
            y2 = 150.0f;
        }
        else if (smb_selected == SMB_ITEM_MASK)
        {
            y1 = 155.0f;
            y2 = 185.0f;
        }
        else if (smb_selected == SMB_ITEM_GATEWAY)
        {
            y1 = 190.0f;
            y2 = 220.0f;
        }
        else if (smb_selected == SMB_ITEM_DNS)
        {
            y1 = 225.0f;
            y2 = 255.0f;
        }
        else if (smb_selected == SMB_ITEM_SERVER)
        {
            y1 = 260.0f;
            y2 = 290.0f;
        }
        else if (smb_selected == SMB_ITEM_APPLY)
        {
            /*
             * APPLY e BACK lado a lado.
             */
            x2 = 315.0f;
            y1 = 300.0f;
            y2 = 335.0f;
        }
        else
        {
            x1 = 325.0f;
            y1 = 300.0f;
            y2 = 335.0f;
        }


        if (!smb_editing)
        {
            gsKit_prim_sprite(
                gsGlobal,
                x1,
                y1,
                x2,
                y2,
                3,
                COLOR_SELECTED
            );
        }
    }


    /*
     * MODE
     */

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        90.0f,
        2,
        0.48f,
        COLOR_WHITE,
        "Mode:"
    );


    if (smb_mode == NETWORK_DHCP)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            300.0f,
            90.0f,
            2,
            0.48f,
            COLOR_OK,
            "< DHCP >"
        );
    }
    else
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            285.0f,
            90.0f,
            2,
            0.48f,
            COLOR_OK,
            "< STATIC >"
        );
    }


    /*
     * IP
     */

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        125.0f,
        2,
        0.48f,
        COLOR_WHITE,
        "IP:"
    );

    draw_editable_ip(
        gsGlobal,
        font,
        280.0f,
        125.0f,
        0,
        smb_mode == NETWORK_STATIC
    );


    /*
     * MASK
     */

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        160.0f,
        2,
        0.48f,
        COLOR_WHITE,
        "Mask:"
    );

    draw_editable_ip(
        gsGlobal,
        font,
        280.0f,
        160.0f,
        1,
        smb_mode == NETWORK_STATIC
    );


    /*
     * GATEWAY
     */

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        195.0f,
        2,
        0.48f,
        COLOR_WHITE,
        "Gateway:"
    );

    draw_editable_ip(
        gsGlobal,
        font,
        280.0f,
        195.0f,
        2,
        smb_mode == NETWORK_STATIC
    );


    /*
     * DNS
     */

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        230.0f,
        2,
        0.48f,
        COLOR_WHITE,
        "DNS:"
    );

    draw_editable_ip(
        gsGlobal,
        font,
        280.0f,
        230.0f,
        3,
        smb_mode == NETWORK_STATIC
    );


    /*
     * SERVIDOR SMB
     *
     * Sempre editável, em DHCP ou STATIC.
     */

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        265.0f,
        2,
        0.48f,
        COLOR_WHITE,
        "SMB Server:"
    );

    draw_editable_ip(
        gsGlobal,
        font,
        280.0f,
        265.0f,
        SMB_SERVER_ROW,
        1
    );


    /*
     * Editor.
     */

    if (smb_editing)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            300.0f,
            2,
            0.43f,
            COLOR_OK,
            "EDIT NETWORK ADDRESS"
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            322.0f,
            2,
            0.40f,
            COLOR_WHITE,
            "LEFT/RIGHT=Octet  UP/DOWN=Value"
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            344.0f,
            2,
            0.40f,
            COLOR_WHITE,
            "CROSS=Finish"
        );
    }
    else
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            180.0f,
            307.0f,
            2,
            0.55f,
            COLOR_WHITE,
            "APPLY"
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            400.0f,
            307.0f,
            2,
            0.55f,
            COLOR_WHITE,
            "BACK"
        );
    }


    /*
     * STATUS SMB / NETWORK.
     */

    if (smb_waiting_network)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            365.0f,
            2,
            0.42f,
            COLOR_WHITE,
            "NETWORK: WAITING FOR IP..."
        );
    }
    else if (smb_test_state == 1)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            365.0f,
            2,
            0.42f,
            COLOR_WHITE,
            "SMB: INITIALIZING..."
        );
    }
    else if (smb_test_state == 2)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            365.0f,
            2,
            0.42f,
            COLOR_WHITE,
            "SMB: CONNECTING..."
        );
    }
    else if (smb_test_state == 3)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            365.0f,
            2,
            0.42f,
            COLOR_OK,
            "SMB: CONNECTED"
        );
    }
    else if (smb_test_state == 4)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            365.0f,
            2,
            0.42f,
            COLOR_OK,
            "SMB: CONNECTED / LISTING DVD"
        );
    }
    else if (smb_test_state == 5)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            365.0f,
            2,
            0.42f,
            COLOR_OK,
            "SMB: CONNECTED / DVD OK"
        );
    }
    else if (smb_test_state == -1)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            365.0f,
            2,
            0.42f,
            COLOR_ERROR,
            smb_error_line[0] != '\0'
                ? smb_error_line
                : "SMB: ERROR"
        );
    }


    /*
     * Estado atual da rede.
     */

    if (net)
    {
        char line[80];

        sprintf(
            line,
            "Current: %s",
            net->mode == NETWORK_DHCP
                ? "DHCP"
                : "STATIC"
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            385.0f,
            2,
            0.40f,
            COLOR_WHITE,
            line
        );


        sprintf(
            line,
            "IP: %s",
            net->ip
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            405.0f,
            2,
            0.40f,
            net->link_up
                ? COLOR_OK
                : COLOR_ERROR,
            line
        );


        if (net->link_up)
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                400.0f,
                405.0f,
                2,
                0.40f,
                COLOR_OK,
                "LINK UP"
            );
        }
        else
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                400.0f,
                405.0f,
                2,
                0.40f,
                COLOR_ERROR,
                "LINK DOWN"
            );
        }
    }


    /*
     * Ajuda.
     */

    if (!smb_editing)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            430.0f,
            2,
            0.38f,
            COLOR_WHITE,
            "UP/DOWN=Select"
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            350.0f,
            430.0f,
            2,
            0.38f,
            COLOR_WHITE,
            "CROSS=Enter"
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            448.0f,
            2,
            0.38f,
            COLOR_WHITE,
            "LEFT/RIGHT=Change Mode"
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            390.0f,
            448.0f,
            2,
            0.38f,
            COLOR_WHITE,
            "CIRCLE=Back"
        );
    }
}


/* ============================================================
 * APLICA CONFIGURAÇÃO DE REDE
 * ============================================================ */

static void smb_apply(void)
{
    char ip[32];
    char mask[32];
    char gateway[32];
    char dns[32];

    int result;


    printf("\n");
    printf("==============================\n");
    printf(" SMB APPLY\n");
    printf("==============================\n");

    printf(
        "SMB APPLY: mode=%s\n",
        smb_mode == NETWORK_DHCP
            ? "DHCP"
            : "STATIC"
    );


    make_ip_string(ip, 0);
    make_ip_string(mask, 1);
    make_ip_string(gateway, 2);
    make_ip_string(dns, 3);


    /*
     * Limpa tentativa anterior.
     */

    smb_waiting_network = 0;
    smb_wait_frames = 0;
    smb_test_state = 1;


    /*
     * DHCP
     */

    if (smb_mode == NETWORK_DHCP)
    {
        printf(
            "SMB APPLY: starting DHCP\n"
        );

        result = network_start_dhcp();

        printf(
            "SMB APPLY: network_start_dhcp returned %d\n",
            result
        );

        if (result < 0)
        {
            printf(
                "SMB APPLY: ERROR starting DHCP\n"
            );

            smb_test_state = -1;

            return;
        }


        smb_waiting_network = 1;
        smb_wait_frames = 0;


        printf(
            "NETWORK: waiting for DHCP address...\n"
        );

        return;
    }


    /*
     * STATIC
     */

    printf(
        "SMB APPLY: applying STATIC\n"
    );

    printf(
        "SMB APPLY: IP      %s\n",
        ip
    );

    printf(
        "SMB APPLY: MASK    %s\n",
        mask
    );

    printf(
        "SMB APPLY: GATEWAY %s\n",
        gateway
    );

    printf(
        "SMB APPLY: DNS     %s\n",
        dns
    );


    result = network_apply_static(
        ip,
        mask,
        gateway,
        dns
    );


    printf(
        "SMB APPLY: network_apply_static returned %d\n",
        result
    );


    if (result < 0)
    {
        printf(
            "SMB APPLY: ERROR applying STATIC configuration\n"
        );

        smb_test_state = -1;

        return;
    }


    /*
     * Espera link ficar disponível.
     */

    smb_waiting_network = 1;
    smb_wait_frames = 0;


    printf(
        "NETWORK: waiting for static network link...\n"
    );
}


/* ============================================================
 * CONTROLE DA TELA PRINCIPAL
 * ============================================================ */

static void update_main(void)
{
    if (input_pressed(PAD_UP))
    {
        main_selected--;

        if (main_selected < 0)
            main_selected = 2;
    }


    if (input_pressed(PAD_DOWN))
    {
        main_selected++;

        if (main_selected > 2)
            main_selected = 0;
    }


    if (input_pressed(PAD_CROSS))
    {
        if (main_selected == 2)
        {
            const NetworkInfo *net;

            net = network_get_info();

            if (net)
                smb_mode = net->mode;

            load_current_network_values();

            smb_selected = SMB_ITEM_MODE;
            smb_editing = 0;
            smb_octet = 0;

            current_screen = SCREEN_SMB;
        }
        else
        {
            /*
             * USB (0) / HDD (1): FAT32/exFAT pelo BDM. Monta,
             * lista /FILMES e abre a mesma biblioteca do SMB.
             */
            int kind = main_selected == 0 ? BDM_USB : BDM_HDD;
            const BDMInfo *info;
            int result;

            result = bdm_init(kind);

            printf("MAIN: bdm_init(%s) returned %d\n", bdm_kind_name(kind), result);

            if (result == 0)
            {
                result = bdm_list_dvd(kind);

                printf("MAIN: bdm_list_dvd(%s) returned %d\n", bdm_kind_name(kind), result);
            }

            info = bdm_get_info(kind);

            if (result < 0 && info)
                printf("MAIN: %s ERROR: %s\n", bdm_kind_name(kind), info->error);

            /* unidade montada: abre a biblioteca (mesmo se vazia) */
            if (info && info->mounted && result >= 0)
            {
                lib_from_bdm(kind);
                lib_show();
            }
        }
    }
}


/* ============================================================
 * EDITOR SMB
 * ============================================================ */

static void smb_change_value(
    int row,
    int octet,
    int delta
)
{
    static_values[row][octet] += delta;

    if (static_values[row][octet] > 255)
        static_values[row][octet] = 0;

    if (static_values[row][octet] < 0)
        static_values[row][octet] = 255;
}


static void update_smb_editor(void)
{
    int row;
    int direction = 0;


    if (smb_selected == SMB_ITEM_IP)
        row = 0;
    else if (smb_selected == SMB_ITEM_MASK)
        row = 1;
    else if (smb_selected == SMB_ITEM_GATEWAY)
        row = 2;
    else if (smb_selected == SMB_ITEM_DNS)
        row = 3;
    else
        row = SMB_SERVER_ROW;


    if (input_pressed(PAD_LEFT))
    {
        smb_octet--;

        if (smb_octet < 0)
            smb_octet = 3;
    }


    if (input_pressed(PAD_RIGHT))
    {
        smb_octet++;

        if (smb_octet > 3)
            smb_octet = 0;
    }


    if (
        input_down(PAD_UP) &&
        !input_down(PAD_DOWN)
    )
    {
        direction = 1;
    }


    if (
        input_down(PAD_DOWN) &&
        !input_down(PAD_UP)
    )
    {
        direction = -1;
    }


    if (
        input_pressed(PAD_UP) ||
        input_pressed(PAD_DOWN)
    )
    {
        smb_change_value(
            row,
            smb_octet,
            direction
        );

        smb_hold_frames = 0;
        smb_repeat_frames = 0;
    }
    else if (direction != 0)
    {
        smb_hold_frames++;

        if (smb_hold_frames >= 15)
        {
            smb_repeat_frames++;

            if (smb_repeat_frames >= 3)
            {
                smb_change_value(
                    row,
                    smb_octet,
                    direction
                );

                smb_repeat_frames = 0;
            }
        }
    }
    else
    {
        smb_hold_frames = 0;
        smb_repeat_frames = 0;
    }


    if (input_pressed(PAD_CROSS))
    {
        smb_editing = 0;

        smb_hold_frames = 0;
        smb_repeat_frames = 0;
    }


    if (input_pressed(PAD_CIRCLE))
    {
        smb_editing = 0;

        smb_hold_frames = 0;
        smb_repeat_frames = 0;
    }
}


/* ============================================================
 * TESTE SMB
 * ============================================================ */

static void smb_test(void)
{
    const SMBInfo *info;
    char server[32];
    int result;


    smb_test_state = 1;

    smb_error_line[0] = '\0';


    printf("\n");
    printf("=================================\n");
    printf(" PS2DVDPlayer - SMB TEST\n");
    printf("=================================\n");


    printf(
        "SMB TEST: smb_init()\n"
    );


    result = smb_init();


    printf(
        "SMB TEST: smb_init returned %d\n",
        result
    );


    if (result < 0)
    {
        smb_test_state = -1;

        info = smb_get_info();

        if (info)
        {
            printf(
                "SMB TEST: INIT ERROR: %s\n",
                info->error
            );

            snprintf(
                smb_error_line,
                sizeof(smb_error_line),
                "INIT: %.120s",
                info->error
            );
        }

        return;
    }


    /*
     * Conecta ao servidor.
     */

    smb_test_state = 2;


    make_ip_string(
        server,
        SMB_SERVER_ROW
    );


    printf(
        "SMB TEST: connecting to %s:445\n",
        server
    );


    /*
     * Share/usuário/senha ainda fixos (etapa A2b).
     */

    result = smb_connect(
        server,
        "samba",
        "guest",
        ""
    );


    printf(
        "SMB TEST: smb_connect returned %d\n",
        result
    );


    if (result < 0)
    {
        smb_test_state = -1;

        info = smb_get_info();


        printf(
            "SMB TEST: CONNECTION FAILED\n"
        );


        if (info)
        {
            printf(
                "SMB TEST: ERROR: %s\n",
                info->error
            );

            snprintf(
                smb_error_line,
                sizeof(smb_error_line),
                "%s",
                info->error
            );
        }


        smb_shutdown();

        return;
    }


    /*
     * Conectado.
     */

    smb_test_state = 3;


    printf(
        "=================================\n"
    );

    printf(
        "SMB TEST: CONNECTION OK\n"
    );

    printf(
        "SERVER : %s\n",
        server
    );

    printf(
        "PORT   : 445\n"
    );

    printf(
        "SHARE  : samba\n"
    );

    printf(
        "USER   : guest\n"
    );

    printf(
        "=================================\n"
    );


    /*
     * Lista SMB_DVD_PATH.
     */

    smb_test_state = 4;


    printf(
        "SMB TEST: listing " SMB_DVD_PATH "\n"
    );


    result = smb_list_dvd();


    printf(
        "SMB TEST: smb_list_dvd returned %d\n",
        result
    );


    if (result < 0)
    {
        smb_test_state = -1;

        info = smb_get_info();


        printf(
            "SMB TEST: DVD LISTING FAILED\n"
        );


        if (info)
        {
            printf(
                "SMB TEST: ERROR: %s\n",
                info->error
            );

            snprintf(
                smb_error_line,
                sizeof(smb_error_line),
                "%s",
                info->error
            );
        }

        return;
    }


    info = smb_get_info();


    if (!info)
    {
        smb_test_state = -1;

        printf(
            "SMB TEST: smb_get_info failed\n"
        );

        return;
    }


    smb_test_state = 5;


    printf("\n");

    printf(
        "SMB TEST: %d ISO files found\n",
        info->file_count
    );

    printf(
        "---------------------------------\n"
    );


    {
        int i;

        for (
            i = 0;
            i < info->file_count;
            i++
        )
        {
            printf(
                "%02d: %s\n",
                i + 1,
                info->files[i].name
            );

            printf(
                "    size: %llu bytes\n",
                (unsigned long long)
                    info->files[i].size
            );
        }
    }


    printf(
        "---------------------------------\n"
    );

    printf(
        "SMB TEST: connection kept alive\n"
    );


    /*
     * Abre a biblioteca de ISOs.
     */

    printf(
        "SMB TEST: opening ISO list screen\n"
    );

    lib_from_smb();
    lib_show();
}


/* ============================================================
 * AGUARDA REDE
 * ============================================================ */

static void smb_wait_network_update(void)
{
    const NetworkInfo *net;


    if (!smb_waiting_network)
        return;


    smb_wait_frames++;


    net = network_get_info();


    if (net)
    {
        /*
         * DHCP
         */

        if (smb_mode == NETWORK_DHCP)
        {
            if (
                net->initialized &&
                net->link_up &&
                net->dhcp_bound &&
                net->ip[0] != '\0' &&
                strcmp(
                    net->ip,
                    "0.0.0.0"
                ) != 0
            )
            {
                printf("\n");
                printf("==============================\n");
                printf(" NETWORK READY\n");
                printf("==============================\n");

                printf(
                    "NETWORK: DHCP ready\n"
                );

                printf(
                    "NETWORK: IP      %s\n",
                    net->ip
                );

                printf(
                    "NETWORK: NETMASK %s\n",
                    net->netmask
                );

                printf(
                    "NETWORK: GATEWAY %s\n",
                    net->gateway
                );

                printf(
                    "NETWORK: DNS     %s\n",
                    net->dns
                );

                printf(
                    "NETWORK: LINK UP\n"
                );


                smb_waiting_network = 0;
                smb_wait_frames = 0;


                printf(
                    "NETWORK: starting SMB test now\n"
                );


                smb_test();

                return;
            }
        }

        /*
         * STATIC
         */

        else
        {
            if (
                net->initialized &&
                net->link_up &&
                net->ip[0] != '\0' &&
                strcmp(
                    net->ip,
                    "0.0.0.0"
                ) != 0
            )
            {
                printf("\n");
                printf("==============================\n");
                printf(" NETWORK READY\n");
                printf("==============================\n");

                printf(
                    "NETWORK: STATIC ready\n"
                );

                printf(
                    "NETWORK: IP      %s\n",
                    net->ip
                );

                printf(
                    "NETWORK: NETMASK %s\n",
                    net->netmask
                );

                printf(
                    "NETWORK: GATEWAY %s\n",
                    net->gateway
                );

                printf(
                    "NETWORK: DNS     %s\n",
                    net->dns
                );

                printf(
                    "NETWORK: LINK UP\n"
                );


                smb_waiting_network = 0;
                smb_wait_frames = 0;


                printf(
                    "NETWORK: starting SMB test now\n"
                );


                smb_test();

                return;
            }
        }
    }


    /*
     * Timeout.
     */

    if (
        smb_wait_frames >=
        SMB_NETWORK_TIMEOUT_FRAMES
    )
    {
        printf("\n");
        printf("==============================\n");
        printf(" NETWORK TIMEOUT\n");
        printf("==============================\n");


        if (net)
        {
            printf(
                "NETWORK: initialized=%d\n",
                net->initialized
            );

            printf(
                "NETWORK: link_up=%d\n",
                net->link_up
            );

            printf(
                "NETWORK: dhcp_bound=%d\n",
                net->dhcp_bound
            );

            printf(
                "NETWORK: IP=%s\n",
                net->ip
            );
        }
        else
        {
            printf(
                "NETWORK: network_get_info returned NULL\n"
            );
        }


        printf(
            "NETWORK: SMB will NOT be started\n"
        );


        smb_waiting_network = 0;
        smb_wait_frames = 0;

        smb_test_state = -1;

        snprintf(
            smb_error_line,
            sizeof(smb_error_line),
            "NETWORK: timeout waiting for IP"
        );
    }
}


/* ============================================================
 * TELA LISTA DE ISOs
 * ============================================================ */

static void format_size(
    char *buffer,
    size_t size,
    u64 bytes
)
{
    /*
     * GiB com duas casas, só com inteiros.
     */

    u64 gib100 =
        (bytes * 100ULL) >> 30;

    snprintf(
        buffer,
        size,
        "%llu.%02llu GB",
        (unsigned long long)(gib100 / 100ULL),
        (unsigned long long)(gib100 % 100ULL)
    );
}


static void draw_iso_list(
    GSGLOBAL *gsGlobal,
    GSFONTM *font
)
{
    char line[160];
    int i;


    gsKit_clear(
        gsGlobal,
        COLOR_BG
    );


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        215.0f,
        30.0f,
        2,
        0.85f,
        COLOR_WHITE,
        "DVD LIBRARY"
    );


    gsKit_prim_sprite(
        gsGlobal,
        90.0f,
        70.0f,
        550.0f,
        450.0f,
        2,
        COLOR_PANEL
    );


    snprintf(
        line,
        sizeof(line),
        "%s  (%d ISO)",
        lib_label,
        lib_count
    );

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        80.0f,
        2,
        0.40f,
        COLOR_OK,
        line
    );


    if (lib_count == 0)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            130.0f,
            2,
            0.45f,
            COLOR_DISABLED,
            "No ISO files found in /FILMES"
        );
    }


    for (
        i = 0;
        i < ISO_LIST_VISIBLE;
        i++
    )
    {
        int index = iso_scroll + i;
        float y = 105.0f + (float)i * 36.0f;
        char name[40];
        char size_text[24];
        size_t len;

        if (index >= lib_count)
            break;


        if (index == iso_selected)
        {
            gsKit_prim_sprite(
                gsGlobal,
                110.0f,
                y,
                530.0f,
                y + 32.0f,
                3,
                COLOR_SELECTED
            );
        }


        /*
         * Nome truncado para caber no painel.
         */

        len = strlen(lib_files[index].name);

        if (len > 28)
        {
            snprintf(
                name,
                sizeof(name),
                "%.25s...",
                lib_files[index].name
            );
        }
        else
        {
            snprintf(
                name,
                sizeof(name),
                "%s",
                lib_files[index].name
            );
        }


        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            120.0f,
            y + 6.0f,
            2,
            0.42f,
            COLOR_WHITE,
            name
        );


        format_size(
            size_text,
            sizeof(size_text),
            lib_files[index].size
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            440.0f,
            y + 6.0f,
            2,
            0.40f,
            COLOR_WHITE,
            size_text
        );
    }


    /*
     * Indicadores de rolagem.
     */

    if (iso_scroll > 0)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            510.0f,
            80.0f,
            2,
            0.40f,
            COLOR_WHITE,
            "^"
        );
    }

    if (iso_scroll + ISO_LIST_VISIBLE < lib_count)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            510.0f,
            395.0f,
            2,
            0.40f,
            COLOR_WHITE,
            "v"
        );
    }


    /*
     * Tamanho exato do item selecionado.
     */

    if (
        iso_selected >= 0 &&
        iso_selected < lib_count
    )
    {
        snprintf(
            line,
            sizeof(line),
            "%d/%d  %llu bytes",
            iso_selected + 1,
            lib_count,
            (unsigned long long)
                lib_files[iso_selected].size
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            405.0f,
            2,
            0.40f,
            COLOR_WHITE,
            line
        );
    }


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        430.0f,
        2,
        0.38f,
        COLOR_WHITE,
        "UP/DOWN=Select"
    );

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        270.0f,
        430.0f,
        2,
        0.38f,
        COLOR_WHITE,
        "CROSS=Open"
    );

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        390.0f,
        430.0f,
        2,
        0.38f,
        COLOR_WHITE,
        "CIRCLE=Back"
    );
}


static void update_iso_list(void)
{
    int count = lib_count;


    /* volta para onde a fonte foi escolhida */
    if (input_pressed(PAD_CIRCLE))
    {
        current_screen = lib_source == LIB_SMB ? SCREEN_SMB : SCREEN_MAIN;
        return;
    }


    if (count <= 0)
        return;


    /*
     * CROSS = abrir e identificar a ISO.
     */

    if (input_pressed(PAD_CROSS))
    {
        char path[LIB_PATH_MAX];

        snprintf(
            iso_info_name,
            sizeof(iso_info_name),
            "%s",
            lib_files[iso_selected].name
        );

        iso_info_size = lib_files[iso_selected].size;

        lib_iso_path(path, sizeof(path));

        iso_info_result =
            iso_identify(
                path,
                iso_info_size,
                &iso_info
            );

        speed_state = 0;
        nav_state = 0;

        printf(
            "ISO LIST: iso_identify returned %d\n",
            iso_info_result
        );

        current_screen = SCREEN_ISO_INFO;
        return;
    }


    if (input_pressed(PAD_UP))
    {
        iso_selected--;

        if (iso_selected < 0)
            iso_selected = count - 1;
    }


    if (input_pressed(PAD_DOWN))
    {
        iso_selected++;

        if (iso_selected >= count)
            iso_selected = 0;
    }


    /*
     * Mantém o item selecionado visível.
     */

    if (iso_selected < iso_scroll)
        iso_scroll = iso_selected;

    if (iso_selected >= iso_scroll + ISO_LIST_VISIBLE)
        iso_scroll = iso_selected - ISO_LIST_VISIBLE + 1;
}


/* ============================================================
 * TELA DETALHES DA ISO
 * ============================================================ */

static void draw_iso_line(
    GSGLOBAL *gsGlobal,
    GSFONTM *font,
    float y,
    const char *label,
    const char *value,
    u64 color
)
{
    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        y,
        2,
        0.42f,
        COLOR_WHITE,
        label
    );

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        300.0f,
        y,
        2,
        0.42f,
        color,
        value
    );
}


static void draw_iso_info(
    GSGLOBAL *gsGlobal,
    GSFONTM *font
)
{
    char line[64];
    char name[40];


    gsKit_clear(
        gsGlobal,
        COLOR_BG
    );


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        230.0f,
        30.0f,
        2,
        0.85f,
        COLOR_WHITE,
        "DVD INFO"
    );


    gsKit_prim_sprite(
        gsGlobal,
        90.0f,
        70.0f,
        550.0f,
        450.0f,
        2,
        COLOR_PANEL
    );


    if (strlen(iso_info_name) > 34)
        snprintf(name, sizeof(name), "%.31s...", iso_info_name);
    else
        snprintf(name, sizeof(name), "%s", iso_info_name);

    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        125.0f,
        85.0f,
        2,
        0.42f,
        COLOR_OK,
        name
    );


    format_size(line, sizeof(line), iso_info_size);

    draw_iso_line(gsGlobal, font, 112.0f, "Size:", line, COLOR_WHITE);

    if (!iso_info.opened)
        snprintf(line, sizeof(line), "OPEN FAILED");
    else if (!iso_info.last_read_ok)
        snprintf(line, sizeof(line), "READ FAILED");
    else
        snprintf(
            line,
            sizeof(line),
            "OK (%s%s)",
            iso_info.iso9660 ? "ISO9660" : "",
            iso_info.udf_anchor ? "+UDF" : ""
        );

    draw_iso_line(
        gsGlobal, font, 136.0f, "Disc:",
        line,
        (iso_info.opened && iso_info.last_read_ok) ? COLOR_OK : COLOR_ERROR
    );

    draw_iso_line(
        gsGlobal, font, 160.0f, "Volume:",
        iso_info.volume_id[0] ? iso_info.volume_id : "-",
        COLOR_WHITE
    );

    if (iso_info.dvd_parsed)
    {
        char text[64];
        int i;

        snprintf(
            line,
            sizeof(line),
            "%d (main #%d)",
            iso_info.titles,
            iso_info.main_title
        );

        draw_iso_line(gsGlobal, font, 184.0f, "Titles:", line, COLOR_WHITE);

        snprintf(
            line,
            sizeof(line),
            "%u:%02u:%02u  %d ch.",
            (unsigned)(iso_info.main_seconds / 3600),
            (unsigned)((iso_info.main_seconds / 60) % 60),
            (unsigned)(iso_info.main_seconds % 60),
            iso_info.main_chapters
        );

        draw_iso_line(gsGlobal, font, 208.0f, "Duration:", line, COLOR_WHITE);

        snprintf(
            line,
            sizeof(line),
            "%s %s %s",
            iso_info.video_mpeg2 ? "MPEG-2" : "MPEG-1",
            iso_info.video_pal ? "PAL" : "NTSC",
            iso_info.video_wide ? "16:9" : "4:3"
        );

        draw_iso_line(gsGlobal, font, 232.0f, "Video:", line, COLOR_WHITE);

        /*
         * Áudio: primeira faixa + total.
         */
        if (iso_info.audio_count > 1)
            snprintf(line, sizeof(line), "%s +%d", iso_info.audio[0], iso_info.audio_count - 1);
        else if (iso_info.audio_count == 1)
            snprintf(line, sizeof(line), "%s", iso_info.audio[0]);
        else
            snprintf(line, sizeof(line), "-");

        draw_iso_line(gsGlobal, font, 256.0f, "Audio:", line, COLOR_WHITE);

        /*
         * Legendas: códigos de idioma, até caber.
         */
        text[0] = '\0';

        for (i = 0; i < iso_info.sub_count; i++)
        {
            if (strlen(text) + 4 > 18)
            {
                strcat(text, "...");
                break;
            }

            if (i > 0)
                strcat(text, " ");

            strcat(text, iso_info.subs[i]);
        }

        draw_iso_line(
            gsGlobal, font, 280.0f, "Subtitles:",
            iso_info.sub_count ? text : "-",
            COLOR_WHITE
        );

        snprintf(
            line,
            sizeof(line),
            "%u KB/s avg",
            (unsigned)iso_info.avg_kbps
        );

        draw_iso_line(gsGlobal, font, 304.0f, "Bitrate:", line, COLOR_WHITE);

        draw_iso_line(
            gsGlobal, font, 328.0f, "Disc start:",
            iso_info.fp_desc[0] ? iso_info.fp_desc : "-",
            iso_info.start_bytes ? COLOR_OK : COLOR_DISABLED
        );
    }
    else
    {
        draw_iso_line(
            gsGlobal, font, 184.0f, "VIDEO_TS:",
            iso_info.vmg_ifo ? "OK (details unavailable)" :
            iso_info.video_ts ? "FOUND" : "NOT FOUND",
            iso_info.video_ts ? COLOR_OK : COLOR_ERROR
        );
    }


    /*
     * Resultado.
     */

    if (speed_state != 0)
    {
        int i;

        if (speed_state == 1)
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                125.0f,
                345.0f,
                2,
                0.42f,
                COLOR_WHITE,
                "SPEED TEST RUNNING..."
            );
        }
        else if (speed_state < 0)
        {
            gsKit_fontm_print_scaled(
                gsGlobal,
                font,
                125.0f,
                345.0f,
                2,
                0.42f,
                COLOR_ERROR,
                "SPEED TEST: OPEN FAILED"
            );
        }
        else
        {
            /*
             * Resultados em duas colunas.
             */
            for (i = 0; i < ISO_SPEED_TESTS; i++)
            {
                float x = (i < 3) ? 125.0f : 330.0f;
                float y = 354.0f + (float)(i % 3) * 19.0f;

                if (speed_results[i].error)
                {
                    snprintf(
                        line,
                        sizeof(line),
                        "%3dK: ERR %d",
                        speed_results[i].block_size / 1024,
                        speed_results[i].error
                    );
                }
                else
                {
                    snprintf(
                        line,
                        sizeof(line),
                        "%3dK: %u KB/s",
                        speed_results[i].block_size / 1024,
                        (unsigned)speed_results[i].kbps
                    );
                }

                gsKit_fontm_print_scaled(
                    gsGlobal,
                    font,
                    x,
                    y,
                    2,
                    0.40f,
                    speed_results[i].error
                        ? COLOR_ERROR
                        : (speed_results[i].kbps >= 1300 ? COLOR_OK : COLOR_WHITE),
                    line
                );
            }
        }
    }
    else if (iso_info_result == 0)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            360.0f,
            2,
            0.48f,
            COLOR_OK,
            "DVD-VIDEO IDENTIFIED"
        );

        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            385.0f,
            2,
            0.40f,
            COLOR_WHITE,
            "Ready to play"
        );
    }
    else
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            360.0f,
            2,
            0.45f,
            COLOR_ERROR,
            iso_info.error[0] ? iso_info.error : "IDENTIFY FAILED"
        );
    }


    gsKit_fontm_print_scaled(
        gsGlobal,
        font,
        390.0f,
        430.0f,
        2,
        0.38f,
        COLOR_WHITE,
        "CIRCLE=Back"
    );

    if (iso_info.dvd_parsed && iso_info.vob1_lba && speed_state != 1)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            412.0f,
            2,
            0.38f,
            COLOR_OK,
            iso_info.start_bytes ? "CROSS=Play (disc start)  SQUARE=Main title" : "CROSS=Play video"
        );
    }

    if (iso_info.opened && speed_state != 1)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            430.0f,
            2,
            0.38f,
            COLOR_WHITE,
            "TRIANGLE=Speed test  R1=Nav test (log)"
        );
    }

    if (nav_state != 0)
    {
        gsKit_fontm_print_scaled(
            gsGlobal,
            font,
            125.0f,
            394.0f,
            2,
            0.38f,
            nav_state < 0 ? COLOR_ERROR : COLOR_WHITE,
            nav_state == 1 ? "NAV TEST RUNNING (see log)..." :
            nav_state == 2 ? "NAV TEST DONE (see log)" :
                             "NAV TEST FAILED (see log)"
        );
    }
}


static void update_iso_info(void)
{
    /*
     * Executa o teste um frame depois do pedido, para que
     * "SPEED TEST RUNNING..." apareça antes do bloqueio.
     */
    if (speed_state == 1)
    {
        char path[LIB_PATH_MAX];

        lib_iso_path(path, sizeof(path));

        speed_state =
            iso_speed_test(
                path,
                iso_info_size,
                speed_results
            ) < 0 ? -1 : 2;

        return;
    }

    /* Teste da libdvdnav, um frame depois do pedido (como o speed test). */
    if (nav_state == 1)
    {
        char path[LIB_PATH_MAX];

        lib_iso_path(path, sizeof(path));

        nav_state = nav_test(path, iso_info_size) < 0 ? -1 : 2;
        return;
    }

    if (input_pressed(PAD_CIRCLE))
    {
        current_screen = SCREEN_ISO_LIST;
        return;
    }

    if (iso_info.opened && input_pressed(PAD_R1))
    {
        nav_state = 1;

        printf("ISO INFO: dvdnav test requested\n");
        return;
    }

    if (
        iso_info.dvd_parsed &&
        iso_info.vob1_lba &&
        (input_pressed(PAD_CROSS) || input_pressed(PAD_SQUARE))
    )
    {
        /*
         * X: começa pelo que o disco manda (First Play).
         * Quadrado: direto no título principal.
         */
        play_from_start = input_pressed(PAD_CROSS) && iso_info.start_bytes;

        frame_state = 1;
        current_screen = SCREEN_FRAME;

        printf("ISO INFO: video playback requested (%s)\n",
               play_from_start ? iso_info.fp_desc : "main title");
        return;
    }

    if (
        iso_info.opened &&
        input_pressed(PAD_TRIANGLE)
    )
    {
        speed_state = 1;

        printf("ISO INFO: speed test requested\n");
    }
}


/* ============================================================
 * TELA QUADRO DE VÍDEO
 * ============================================================ */

static u64 us_to_ticks(u32 us)
{
    return ((u64)us * kBUSCLK) / 1000000ULL;
}


/* ============================================================
 * SUBPICTURE (destaque dos botões / legendas forçadas)
 * ============================================================ */

/*
 * Textura T8 (índice por pixel) + paleta CT32:
 *   0..3   cores do SPU
 *   4..7   cores do botão selecionado (dentro do retângulo)
 *   0xFF   transparente
 */
static GSTEXTURE spu_tex;
static int spu_tex_ready = 0;
static u8 *spu_tex_mem = NULL;
static u32 *spu_tex_clut = NULL;

static u32 spu_key_serial = 0xFFFFFFFF;
static u32 spu_key_clut = 0xFFFFFFFF;
static int spu_key_button = -1;
static int spu_key_rect[4];
static u32 spu_key_pal = 0;
static int spu_key_height = 0;


static u32 yuv_to_gs(u32 yuv, int contrast)
{
    int y = (yuv >> 16) & 0xFF;
    int cr = (yuv >> 8) & 0xFF;
    int cb = yuv & 0xFF;
    int r = y + ((1436 * (cr - 128)) >> 10);
    int g = y - ((352 * (cb - 128) + 731 * (cr - 128)) >> 10);
    int b = y + ((1815 * (cb - 128)) >> 10);
    u32 a = (u32)((contrast * 0x80 + 7) / 15);

    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);

    return (a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}


/*
 * Desenha o subpicture sobre o vídeo. Retorna 1 se desenhou.
 */
static int draw_spu_overlay(GSGLOBAL *gsGlobal)
{
    const SpuInfo *si = spu_info();
    const u32 *clut = nav_clut();
    int h = frame_info.height;
    int button = 0, rx0 = 0, ry0 = 0, rx1 = -1, ry1 = -1;
    u32 pal = 0;
    int has_pal = 0;
    int i;

    if (!frame_info.nav || !si->visible || frame_info.frames <= 0)
        return 0;

    if (frame_info.buttons <= 0 && !si->forced)
        return 0;

    if (h <= 0 || h > SPU_MAX_H || frame_info.width <= 0)
        return 0;

    if (frame_info.buttons > 0 && nav_button_rect(&button, &rx0, &ry0, &rx1, &ry1))
        has_pal = nav_button_palette(0, &pal);

    if (!has_pal)
    {
        button = 0;
        rx1 = -1;
    }

    if (!spu_tex_ready)
    {
        spu_tex_mem = memalign(128, SPU_MAX_W * SPU_MAX_H);
        spu_tex_clut = memalign(128, 256 * 4);

        if (!spu_tex_mem || !spu_tex_clut)
            return 0;

        memset(&spu_tex, 0, sizeof(spu_tex));
        spu_tex.Width = SPU_MAX_W;
        spu_tex.Height = SPU_MAX_H;
        spu_tex.PSM = GS_PSM_T8;
        spu_tex.ClutPSM = GS_PSM_CT32;
        spu_tex.Filter = GS_FILTER_NEAREST;
        spu_tex.Mem = (u32 *)spu_tex_mem;
        spu_tex.Clut = spu_tex_clut;
        spu_tex.Vram = gsKit_vram_alloc(gsGlobal,
                                        gsKit_texture_size(SPU_MAX_W, SPU_MAX_H, GS_PSM_T8),
                                        GSKIT_ALLOC_USERBUFFER);
        spu_tex.VramClut = gsKit_vram_alloc(gsGlobal,
                                            gsKit_texture_size(16, 16, GS_PSM_CT32),
                                            GSKIT_ALLOC_USERBUFFER);

        frame_logf("SPU: texture VRAM 0x%08x clut 0x%08x\n",
                   (unsigned)spu_tex.Vram, (unsigned)spu_tex.VramClut);

        spu_tex_ready = 1;
    }

    /*
     * Refaz a textura só quando algo mudou.
     */
    if (si->serial != spu_key_serial || nav_clut_serial() != spu_key_clut ||
        button != spu_key_button || pal != spu_key_pal || h != spu_key_height ||
        rx0 != spu_key_rect[0] || ry0 != spu_key_rect[1] ||
        rx1 != spu_key_rect[2] || ry1 != spu_key_rect[3])
    {
        const u8 *bm = spu_bitmap();
        int y;

        memcpy(spu_tex_mem, bm, SPU_MAX_W * h);

        for (y = ry0; y <= ry1 && y < h; y++)
        {
            u8 *row = spu_tex_mem + y * SPU_MAX_W;
            int x;

            if (y < 0)
                continue;

            for (x = rx0; x <= rx1 && x < SPU_MAX_W; x++)
                if (x >= 0 && row[x] != SPU_NONE)
                    row[x] |= 4;
        }

        memset(spu_tex_clut, 0, 256 * 4);

        for (i = 0; i < 4; i++)
        {
            spu_tex_clut[i] = yuv_to_gs(clut[si->color[i] & 15], si->alpha[i]);
            spu_tex_clut[4 + i] = yuv_to_gs(clut[(pal >> (16 + 4 * i)) & 15], (pal >> (4 * i)) & 15);
        }

        spu_tex.Height = h;

        FlushCache(0);
        gsKit_texture_upload(gsGlobal, &spu_tex);

        spu_key_serial = si->serial;
        spu_key_clut = nav_clut_serial();
        spu_key_button = button;
        spu_key_pal = pal;
        spu_key_height = h;
        spu_key_rect[0] = rx0;
        spu_key_rect[1] = ry0;
        spu_key_rect[2] = rx1;
        spu_key_rect[3] = ry1;
    }

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    gsKit_prim_sprite_texture(
        gsGlobal,
        &spu_tex,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        640.0f,
        480.0f,
        (float)frame_info.width,
        (float)h,
        3,
        GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00)
    );

    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

    return 1;
}


/* ============================================================
 * CONVERSÃO DE COR PELA GS (FRAME_GS_CSC)
 * ============================================================ */

/*
 * Planos Y / Cb / Cr como texturas T8. A tela é montada em passadas:
 *   1. Y com paleta 1,164 (Y - 16) em cinza
 *   2. + parte positiva de Cr e de Cb (blending de soma)
 *   3. - parte negativa de Cr e de Cb (blending de subtração)
 * Coeficientes BT.601 (iguais aos da conversão em software):
 *   R = Y' + 1,598 Cr    G = Y' - 0,391 Cb - 0,8125 Cr    B = Y' + 2,016 Cb
 * Cada componente tem só um sinal por pixel em R e B; o G pode ter
 * os dois (corte em 0/255 entre passadas: erro pequeno nos extremos).
 */
enum { CSC_Y = 0, CSC_CR_ADD, CSC_CB_ADD, CSC_CR_SUB, CSC_CB_SUB, CSC_TEX };

static GSTEXTURE csc_tex[CSC_TEX];
static u32 *csc_clut[CSC_TEX];
static int csc_ready = 0;


/* ordem da paleta de 256 cores na VRAM (CSM1): troca 8-15 e 16-23 */
static int clut_pos(int i)
{
    return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
}


static u32 clut_rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);

    return (0x80u << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}


static void csc_build_cluts(void)
{
    int i;

    for (i = 0; i < 256; i++)
    {
        int y = (298 * (i - 16) + 128) >> 8;
        int c = i - 128;
        int p = c > 0 ? c : 0;          /* parte positiva */
        int n = c < 0 ? -c : 0;         /* parte negativa (módulo) */
        int pos = clut_pos(i);

        csc_clut[CSC_Y][pos] = clut_rgb(y, y, y);

        /* Cr: R +1,598 c   G -0,8125 c */
        csc_clut[CSC_CR_ADD][pos] = clut_rgb((409 * p + 128) >> 8, (208 * n + 128) >> 8, 0);
        csc_clut[CSC_CR_SUB][pos] = clut_rgb((409 * n + 128) >> 8, (208 * p + 128) >> 8, 0);

        /* Cb: G -0,391 c   B +2,016 c */
        csc_clut[CSC_CB_ADD][pos] = clut_rgb(0, (100 * n + 128) >> 8, (516 * p + 128) >> 8);
        csc_clut[CSC_CB_SUB][pos] = clut_rgb(0, (100 * p + 128) >> 8, (516 * n + 128) >> 8);
    }
}


static int csc_init(GSGLOBAL *gsGlobal)
{
    const u8 *py, *pb, *pr;
    u32 vram_y, vram_cr, vram_cb;
    int i;

    frame_planes(&py, &pb, &pr);

    if (!py)
        return -1;

    for (i = 0; i < CSC_TEX; i++)
    {
        csc_clut[i] = memalign(128, 256 * 4);
        if (!csc_clut[i])
            return -1;
    }

    csc_build_cluts();

    vram_y = gsKit_vram_alloc(gsGlobal,
                              gsKit_texture_size(FRAME_MAX_WIDTH, FRAME_MAX_HEIGHT, GS_PSM_T8),
                              GSKIT_ALLOC_USERBUFFER);
    vram_cr = gsKit_vram_alloc(gsGlobal,
                               gsKit_texture_size(FRAME_MAX_WIDTH / 2, FRAME_MAX_HEIGHT / 2, GS_PSM_T8),
                               GSKIT_ALLOC_USERBUFFER);
    vram_cb = gsKit_vram_alloc(gsGlobal,
                               gsKit_texture_size(FRAME_MAX_WIDTH / 2, FRAME_MAX_HEIGHT / 2, GS_PSM_T8),
                               GSKIT_ALLOC_USERBUFFER);

    for (i = 0; i < CSC_TEX; i++)
    {
        GSTEXTURE *t = &csc_tex[i];

        memset(t, 0, sizeof(*t));
        t->PSM = GS_PSM_T8;
        t->ClutPSM = GS_PSM_CT32;
        t->Filter = GS_FILTER_LINEAR;
        t->Clut = csc_clut[i];
        t->VramClut = gsKit_vram_alloc(gsGlobal,
                                       gsKit_texture_size(16, 16, GS_PSM_CT32),
                                       GSKIT_ALLOC_USERBUFFER);

        if (i == CSC_Y)
        {
            t->Width = FRAME_MAX_WIDTH;
            t->Height = FRAME_MAX_HEIGHT;
            t->Vram = vram_y;
            t->Mem = (u32 *)py;
        }
        else
        {
            int is_cr = (i == CSC_CR_ADD || i == CSC_CR_SUB);

            t->Width = FRAME_MAX_WIDTH / 2;
            t->Height = FRAME_MAX_HEIGHT / 2;
            t->Vram = is_cr ? vram_cr : vram_cb;
            t->Mem = (u32 *)(is_cr ? pr : pb);
        }
    }

    /* paletas e planos iniciais (as paletas não mudam depois) */
    FlushCache(0);

    for (i = 0; i < CSC_TEX; i++)
        gsKit_texture_upload(gsGlobal, &csc_tex[i]);

    frame_logf("FRAME: GS color conversion, VRAM Y 0x%08x Cr 0x%08x Cb 0x%08x\n",
               (unsigned)vram_y, (unsigned)vram_cr, (unsigned)vram_cb);

    csc_ready = 1;
    return 0;
}


/* Envia os planos do quadro novo (Y, Cr, Cb: uma vez cada). */
static void csc_upload(GSGLOBAL *gsGlobal)
{
    int w = frame_info.width;
    int h = frame_info.height;
    int i;

    for (i = 0; i < CSC_TEX; i++)
    {
        csc_tex[i].Width = i == CSC_Y ? w : w / 2;
        csc_tex[i].Height = i == CSC_Y ? h : h / 2;
    }

    FlushCache(0);

    /* *_ADD reenviam os pixels de Cr/Cb; as paletas *_SUB ficam na VRAM */
    gsKit_texture_upload(gsGlobal, &csc_tex[CSC_Y]);
    gsKit_texture_upload(gsGlobal, &csc_tex[CSC_CR_ADD]);
    gsKit_texture_upload(gsGlobal, &csc_tex[CSC_CB_ADD]);
}


static void csc_sprite(GSGLOBAL *gsGlobal, int i)
{
    gsKit_prim_sprite_texture(
        gsGlobal,
        &csc_tex[i],
        0.0f, 0.0f,
        0.0f, 0.0f,
        640.0f, 480.0f,
        (float)csc_tex[i].Width, (float)csc_tex[i].Height,
        2,
        GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00)
    );
}


static void csc_draw(GSGLOBAL *gsGlobal)
{
    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;
    csc_sprite(gsGlobal, CSC_Y);

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    /* soma: Cd + Cs */
    gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 2, 2, 1, 0x80), 0);
    csc_sprite(gsGlobal, CSC_CR_ADD);
    csc_sprite(gsGlobal, CSC_CB_ADD);

    /* subtração: Cd - Cs */
    gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(2, 0, 2, 1, 0x80), 0);
    csc_sprite(gsGlobal, CSC_CR_SUB);
    csc_sprite(gsGlobal, CSC_CB_SUB);

    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;
}


static void format_hms(char *out, int size, u32 ms)
{
    u32 s = ms / 1000;

    snprintf(out, size, "%02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}


/*
 * Ícone de um botão da barra, centrado em (cx, cy).
 */
static void draw_bar_icon(GSGLOBAL *gsGlobal, int id, float cx, float cy, u64 c)
{
    switch (id)
    {
        case BAR_PREV:
            gsKit_prim_sprite(gsGlobal, cx - 9, cy - 7, cx - 6, cy + 7, 4, c);
            gsKit_prim_triangle(gsGlobal, cx + 8, cy - 7, cx + 8, cy + 7, cx - 5, cy, 4, c);
            break;

        case BAR_PLAY:
            /* gsKit_prim_triangle é macro com ";": chaves obrigatórias */
            if (frame_state == 6)
            {
                gsKit_prim_triangle(gsGlobal, cx - 6, cy - 8, cx - 6, cy + 8, cx + 8, cy, 4, c);
            }
            else
            {
                gsKit_prim_sprite(gsGlobal, cx - 7, cy - 7, cx - 2, cy + 7, 4, c);
                gsKit_prim_sprite(gsGlobal, cx + 2, cy - 7, cx + 7, cy + 7, 4, c);
            }
            break;

        case BAR_NEXT:
            gsKit_prim_triangle(gsGlobal, cx - 8, cy - 7, cx - 8, cy + 7, cx + 5, cy, 4, c);
            gsKit_prim_sprite(gsGlobal, cx + 6, cy - 7, cx + 9, cy + 7, 4, c);
            break;

        case BAR_MENU:
            gsKit_prim_sprite(gsGlobal, cx - 8, cy - 7, cx + 8, cy - 4, 4, c);
            gsKit_prim_sprite(gsGlobal, cx - 8, cy - 1, cx + 8, cy + 2, 4, c);
            gsKit_prim_sprite(gsGlobal, cx - 8, cy + 5, cx + 8, cy + 8, 4, c);
            break;

        default:
            gsKit_prim_sprite(gsGlobal, cx - 7, cy - 7, cx + 7, cy + 7, 4, c);
            break;
    }
}


/*
 * Barra do player (tela 10 do modelo): título, capítulo e tempo em
 * cima; progresso e botões embaixo.
 */
static void draw_play_bar(GSGLOBAL *gsGlobal, GSFONTM *font)
{
    /* nome do botão + atalho equivalente */
    static const char *names[BAR_COUNT] = {
        "Previous  (L1)", "Play  (START)", "Next  (R1)", "Menu  (TRIANGLE)", "Stop  (CIRCLE)"
    };
    char line[96];
    char name[48];
    char t_now[16], t_total[16];
    u32 ms = 0, total = 0;
    int part = 0, parts = 0;
    int timed = frame_info.nav &&
                nav_play_time(frame_info.pts, &ms, &total, &part, &parts);
    u64 panel = GS_SETREG_RGBAQ(0x08, 0x0C, 0x18, 0x60, 0x00);
    float x;
    int i;
    char *dot;

    if (timed && total > 0 && ms > total)
        ms = total;

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    gsKit_prim_sprite(gsGlobal, 0.0f, 0.0f, 640.0f, 34.0f, 3, panel);
    gsKit_prim_sprite(gsGlobal, 0.0f, 384.0f, 640.0f, 480.0f, 3, panel);

    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

    /* nome da ISO sem a extensão (cortado para caber antes do tempo) */
    snprintf(name, sizeof(name), "%.38s", iso_info_name);
    dot = strrchr(name, '.');
    if (dot)
        *dot = '\0';

    gsKit_fontm_print_scaled(gsGlobal, font, 16.0f, 9.0f, 4, 0.38f, COLOR_WHITE, name);

    if (timed)
    {
        format_hms(t_now, sizeof(t_now), ms);
        format_hms(t_total, sizeof(t_total), total);

        snprintf(line, sizeof(line), "Chapter %02d/%02d   %s / %s", part, parts, t_now, t_total);
        gsKit_fontm_print_scaled(gsGlobal, font, 330.0f, 9.0f, 4, 0.38f, COLOR_WHITE, line);
    }

    /* progresso */
    gsKit_prim_sprite(gsGlobal, 20.0f, 396.0f, 620.0f, 402.0f, 4, COLOR_DISABLED);

    if (timed && total > 0)
    {
        float fx = 20.0f + 600.0f * (float)ms / (float)total;

        gsKit_prim_sprite(gsGlobal, 20.0f, 396.0f, fx, 402.0f, 4, COLOR_SELECTED);
        gsKit_prim_sprite(gsGlobal, fx - 4.0f, 392.0f, fx + 4.0f, 406.0f, 4, COLOR_WHITE);

        gsKit_fontm_print_scaled(gsGlobal, font, 20.0f, 410.0f, 4, 0.34f, COLOR_WHITE, t_now);
        gsKit_fontm_print_scaled(gsGlobal, font, 548.0f, 410.0f, 4, 0.34f, COLOR_WHITE, t_total);
    }

    /* botões */
    for (i = 0, x = 20.0f; i < BAR_COUNT; i++, x += 58.0f)
    {
        int sel = i == play_bar_sel;

        gsKit_prim_sprite(gsGlobal, x, 432.0f, x + 50.0f, 464.0f, 4,
                          sel ? COLOR_SELECTED : COLOR_PANEL);

        draw_bar_icon(gsGlobal, i, x + 25.0f, 448.0f, sel ? COLOR_WHITE : COLOR_DISABLED);
    }

    snprintf(line, sizeof(line), "%s", names[play_bar_sel]);
    if (play_bar_sel == BAR_PLAY && frame_state != 6)
        snprintf(line, sizeof(line), "Pause  (START)");

    gsKit_fontm_print_scaled(gsGlobal, font, 320.0f, 434.0f, 4, 0.40f, COLOR_WHITE, line);
    gsKit_fontm_print_scaled(gsGlobal, font, 320.0f, 452.0f, 4, 0.32f, COLOR_DISABLED,
                             "LEFT/RIGHT=Choose  CROSS=OK  SELECT=Hide");
}


static void draw_frame(
    GSGLOBAL *gsGlobal,
    GSFONTM *font
)
{
    char line[128];

    gsKit_clear(
        gsGlobal,
        GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00)
    );


    /*
     * Envia o quadro decodificado para a VRAM (uma vez por quadro).
     */
    if (FRAME_GS_CSC && frame_upload)
    {
        if (!csc_ready && csc_init(gsGlobal) == 0)
            frame_tex_ready = 1;

        if (csc_ready)
            csc_upload(gsGlobal);

        frame_upload = 0;
    }
    else if (frame_upload && frame_pixels)
    {
        if (!frame_tex_ready)
        {
            memset(&frame_tex, 0, sizeof(frame_tex));

            frame_tex.Width = FRAME_MAX_WIDTH;
            frame_tex.Height = FRAME_MAX_HEIGHT;
            frame_tex.PSM = GS_PSM_CT16;
            frame_tex.Filter = GS_FILTER_LINEAR;
            frame_tex.Vram =
                gsKit_vram_alloc(
                    gsGlobal,
                    gsKit_texture_size(FRAME_MAX_WIDTH, FRAME_MAX_HEIGHT, GS_PSM_CT16),
                    GSKIT_ALLOC_USERBUFFER
                );

            frame_logf("FRAME: texture VRAM 0x%08x\n", (unsigned)frame_tex.Vram);

            frame_tex_ready = 1;
        }

        frame_tex.Width = frame_info.width;
        frame_tex.Height = frame_info.height;
        frame_tex.Mem = (u32 *)frame_pixels;

        FlushCache(0);
        gsKit_texture_upload(gsGlobal, &frame_tex);

        frame_upload = 0;
    }


    if (frame_state < 0)
    {
        gsKit_fontm_print_scaled(gsGlobal, font, 60.0f, 200.0f, 3, 0.45f, COLOR_ERROR, "VIDEO ERROR");

        gsKit_fontm_print_scaled(
            gsGlobal, font, 60.0f, 230.0f, 3, 0.40f, COLOR_ERROR,
            frame_info.error[0] ? frame_info.error : "unknown"
        );

        gsKit_fontm_print_scaled(gsGlobal, font, 60.0f, 430.0f, 3, 0.38f, COLOR_WHITE, "CIRCLE=Back");
        return;
    }

    if (FRAME_GS_CSC && csc_ready && frame_info.frames > 0)
    {
        csc_draw(gsGlobal);
    }
    else if (frame_tex_ready && frame_info.frames > 0)
    {
        gsKit_prim_sprite_texture(
            gsGlobal,
            &frame_tex,
            0.0f,
            0.0f,
            0.0f,
            0.0f,
            640.0f,
            480.0f,
            (float)frame_info.width,
            (float)frame_info.height,
            2,
            GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00)
        );
    }

    /*
     * Menu do DVD: destaque original do disco (subpicture); se não
     * houver, contorno no botão selecionado.
     */
    if (frame_tex_ready && frame_info.frames > 0 && draw_spu_overlay(gsGlobal))
    {
        /* destaque original do disco desenhado */
    }
    else if (frame_info.nav && frame_info.buttons > 0 &&
             frame_info.width > 0 && frame_info.height > 0)
    {
        int b, x0, y0, x1, y1;

        if (nav_button_rect(&b, &x0, &y0, &x1, &y1))
        {
            float sx = 640.0f / (float)frame_info.width;
            float sy = 480.0f / (float)frame_info.height;
            float l = x0 * sx - 3.0f;
            float t = y0 * sy - 3.0f;
            float r = x1 * sx + 3.0f;
            float bt = y1 * sy + 3.0f;

            gsKit_prim_sprite(gsGlobal, l, t, r, t + 3.0f, 3, COLOR_OK);
            gsKit_prim_sprite(gsGlobal, l, bt - 3.0f, r, bt, 3, COLOR_OK);
            gsKit_prim_sprite(gsGlobal, l, t, l + 3.0f, bt, 3, COLOR_OK);
            gsKit_prim_sprite(gsGlobal, r - 3.0f, t, r, bt, 3, COLOR_OK);
        }
    }

    /*
     * Barra de status: tempo, quadros/s, buffer, atrasos.
     */
    if (frame_info.frame_us > 0 && !play_bar)
    {
        u32 secs = (u32)(((u64)frame_info.frames * frame_info.frame_us) / 1000000ULL);

        snprintf(
            line,
            sizeof(line),
            "%02u:%02u  %d.%d fps  buf %u KB  dec %u ms  late %d  a-v %d ms",
            secs / 60,
            secs % 60,
            fps_x10 / 10,
            fps_x10 % 10,
            (unsigned)(frame_info.buffered / 1024),
            (unsigned)frame_info.decode_ms,
            play_late,
            frame_info.nav ? (int)(audio_stats()->av_now / 90) : 0
        );

        gsKit_fontm_print_scaled(gsGlobal, font, 20.0f, 10.0f, 3, 0.36f, COLOR_OK, line);
    }

    /*
     * Navegação: still e número de botões do menu.
     */
    if (frame_info.nav && frame_state == 2 && !play_bar)
    {
        line[0] = '\0';

        if (frame_info.still && frame_info.still_len == 0xff)
            snprintf(line, sizeof(line), "STILL (waiting)");
        else if (frame_info.still)
            snprintf(line, sizeof(line), "STILL %u s", (unsigned)((frame_info.still_left_ms + 999) / 1000));

        if (frame_info.buttons > 0)
        {
            int n = strlen(line);

            snprintf(line + n, sizeof(line) - n, "%sMENU: %d buttons",
                     n ? "  " : "", frame_info.buttons);
        }

        if (line[0])
            gsKit_fontm_print_scaled(gsGlobal, font, 20.0f, 432.0f, 3, 0.36f, COLOR_OK, line);
    }

    if (frame_state == 1)
        gsKit_fontm_print_scaled(gsGlobal, font, 200.0f, 220.0f, 3, 0.48f, COLOR_WHITE, "OPENING VIDEO...");
    else if (frame_state == 5)
    {
        snprintf(line, sizeof(line), "BUFFERING %u KB", (unsigned)(frame_info.buffered / 1024));
        gsKit_fontm_print_scaled(gsGlobal, font, 200.0f, 220.0f, 3, 0.48f, COLOR_WHITE, line);
    }
    else if (frame_state == 6)
        gsKit_fontm_print_scaled(gsGlobal, font, 260.0f, 220.0f, 3, 0.55f, COLOR_WHITE, "PAUSED");
    else if (frame_state == 4)
    {
        /* fim da reprodução (vídeo direto ou navegação parada: NAV STOP) */
        gsKit_fontm_print_scaled(gsGlobal, font, 230.0f, 220.0f, 3, 0.50f, COLOR_WHITE, "END OF VIDEO");
    }

    if (play_osd_until && GetTimerSystemTime() < play_osd_until)
        gsKit_fontm_print_scaled(gsGlobal, font, 470.0f, 40.0f, 4, 0.45f, COLOR_WHITE, play_osd);

    if (play_bar)
    {
        draw_play_bar(gsGlobal, font);
        return;
    }

    {
        int part, parts;

        gsKit_fontm_print_scaled(
            gsGlobal, font, 20.0f, 455.0f, 3, 0.36f, COLOR_WHITE,
            (frame_info.nav && frame_info.buttons > 0) ?
                "D-PAD=Move  CROSS=Select  SELECT=Controls  CIRCLE=Stop" :
            (frame_info.nav && nav_chapter_info(&part, &parts)) ?
                "L1/R1=Chapter  START=Play/Pause  TRIANGLE=Menu  SELECT=Controls" :
                "START=Play/Pause  SELECT=Controls  CIRCLE=Stop"
        );
    }
}


static void play_stats(void)
{
    printf(
        "PLAY: %d frames, %d.%d fps, buffer %u KB, read %llu/%llu KB at %u KB/s, "
        "decode %u ms, late %d, rebuffer %d, skipped %d\n",
        frame_info.frames,
        fps_x10 / 10,
        fps_x10 % 10,
        (unsigned)(frame_info.buffered / 1024),
        (unsigned long long)(frame_info.read_total / 1024),
        (unsigned long long)(frame_info.read_limit / 1024),
        (unsigned)frame_info.read_kbps,
        (unsigned)frame_info.decode_ms,
        play_late,
        play_rebuffer,
        play_skipped
    );

    if (frame_info.nav)
    {
        const AudioStats *as = audio_stats();

        printf(
            "AUDIO: %u AC3 frames (%u errors), %d Hz %d kbps flags 0x%x, "
            "decode avg %u us max %u us per frame (32000 us of audio)\n",
            (unsigned)as->frames,
            (unsigned)as->errors,
            as->sample_rate,
            as->bit_rate / 1000,
            as->channels_flags,
            as->frames ? (unsigned)(as->decode_us_total / as->frames) : 0,
            (unsigned)as->decode_us_max
        );

        printf(
            "AUDIO: output=%d played %u, dropped late %u jump %u full %u overlap %u catchup %u, queued %d, "
            "silence %u KB, rpc %u, last a-v %d ms, video pts %d ms\n"
            "AUDIO: sync (sound - picture): now %d ms, min %d, avg %d, max %d ms (%u samples)\n",
            as->output,
            (unsigned)as->played,
            (unsigned)as->dropped_late,
            (unsigned)as->dropped_jump,
            (unsigned)as->dropped_full,
            (unsigned)as->dropped_overlap,
            (unsigned)as->dropped_catchup,
            as->queued,
            (unsigned)(as->silence / 1024),
            (unsigned)as->rpc,
            (int)(as->last_diff / 90),
            (int)(frame_info.pts / 90),
            (int)(as->av_now / 90),
            (int)(as->av_min / 90),
            as->av_count ? (int)(as->av_sum / as->av_count / 90) : 0,
            (int)(as->av_max / 90),
            (unsigned)as->av_count
        );
    }
}


static void play_stop(void)
{
    /*
     * frame_close conclui a leitura em curso: depois disso o
     * printf é seguro.
     */
    frame_close();

    if (frame_state != 0 && frame_state != 1)
        play_stats();

    mem_log("video close");

    play_bar = 0;
    frame_state = 0;
    current_screen = SCREEN_ISO_INFO;
}


/* Pula de capítulo (+1/-1) e mostra o aviso. 1 se pulou. */
static int play_chapter(int delta, u64 now)
{
    int part, parts;
    int jumped = nav_chapter_step(delta);

    if (jumped)
    {
        frame_nav_flush(&frame_info);
        frame_state = 5;
    }

    if (nav_chapter_info(&part, &parts))
    {
        snprintf(play_osd, sizeof(play_osd), "CHAPTER %d/%d", part, parts);
        play_osd_until = now + (u64)kBUSCLK * 2;
    }

    return jumped;
}


/*
 * Ações do player: as mesmas pela barra (X) e pelos atalhos
 * (L1 anterior, START play/pausa, R1 próximo, TRIÂNGULO menu,
 * CÍRCULO parar). Com a barra aberta, o atalho destaca o botão dele.
 */
static void play_action(int id, u64 now)
{
    if (play_bar)
    {
        play_bar_sel = id;
        play_bar_until = now + (u64)kBUSCLK * PLAY_BAR_SECONDS;
    }

    switch (id)
    {
        case BAR_PREV:
        case BAR_NEXT:
            if (frame_info.nav)
                play_chapter(id == BAR_NEXT ? 1 : -1, now);
            break;

        case BAR_PLAY:
            if (frame_state == 6)
            {
                frame_state = 2;
                play_next_due = now;
            }
            else if (frame_state == 2)
                frame_state = 6;
            break;

        case BAR_MENU:
            if (frame_info.nav && nav_menu())
            {
                frame_nav_flush(&frame_info);
                frame_state = 5;
                play_bar = 0;   /* D-pad livre para os botões do disco */
            }
            break;

        case BAR_STOP:
            play_stop();
            break;
    }
}


/*
 * Atalhos do player. Retorna 1 se algum foi usado.
 */
static int play_shortcuts(u64 now)
{
    if (input_pressed(PAD_CIRCLE))
        play_action(BAR_STOP, now);
    else if (input_pressed(PAD_START))
        play_action(BAR_PLAY, now);
    else if (input_pressed(PAD_R1))
        play_action(BAR_NEXT, now);
    else if (input_pressed(PAD_L1))
        play_action(BAR_PREV, now);
    else if (input_pressed(PAD_TRIANGLE))
        play_action(BAR_MENU, now);
    else
        return 0;

    return 1;
}


/*
 * Barra de controle (SELECT): esquerda/direita escolhe, X ativa.
 * Retorna 1 se a entrada foi usada pela barra.
 */
static int play_bar_input(u64 now)
{
    if (input_pressed(PAD_SELECT))
    {
        play_bar = !play_bar;
        play_bar_sel = BAR_PLAY;
        play_bar_until = now + (u64)kBUSCLK * PLAY_BAR_SECONDS;
        return 1;
    }

    if (!play_bar)
        return 0;

    /* some sozinha durante a reprodução; pausado fica na tela */
    if (frame_state == 2 && now > play_bar_until)
    {
        play_bar = 0;
        return 0;
    }

    if (input_pressed(PAD_LEFT) || input_pressed(PAD_RIGHT))
    {
        play_bar_sel = (play_bar_sel + (input_pressed(PAD_RIGHT) ? 1 : BAR_COUNT - 1)) % BAR_COUNT;
        play_bar_until = now + (u64)kBUSCLK * PLAY_BAR_SECONDS;
        return 1;
    }

    if (!input_pressed(PAD_CROSS))
        return 0;

    play_action(play_bar_sel, now);
    return 1;
}


static void update_frame(void)
{
    u64 now;
    int result;
    int bar_used;

    /*
     * Abre o vídeo um frame depois do pedido, para "OPENING"
     * aparecer antes do bloqueio da abertura.
     */
    if (frame_state == 1)
    {
        char path[LIB_PATH_MAX];

        if (!frame_pixels)
            frame_pixels = memalign(128, FRAME_MAX_WIDTH * FRAME_MAX_HEIGHT * 2);

        if (!frame_pixels)
        {
            snprintf(frame_info.error, sizeof(frame_info.error), "Out of memory (pixels)");
            frame_state = -1;
            return;
        }

        lib_iso_path(path, sizeof(path));

        /*
         * X: navegação do DVD (libdvdnav) a partir do First Play.
         * Quadrado: título principal direto do VOB.
         */
        if (play_from_start)
            result = frame_open_nav(path, iso_info_size, &frame_info);
        else
            result = frame_open(path, iso_info.vob1_lba, iso_info.vob_bytes, &frame_info);

        printf("PLAYER: frame_open returned %d\n", result);
        mem_log("video open");

        if (result < 0)
        {
            frame_state = -1;
            return;
        }

        play_late = 0;
        play_rebuffer = 0;
        play_skipped = 0;
        fps_frames = 0;
        fps_x10 = 0;
        fps_t0 = GetTimerSystemTime();

        frame_state = 5;
        return;
    }

    if (input_pressed(PAD_CIRCLE))
    {
        play_stop();
        return;
    }

    if (frame_state < 0 || frame_state == 4)
    {
        play_bar = 0;

        if (play_stats_due && frame_read_idle())
        {
            play_stats();
            mem_log("end");
            play_stats_due = 0;
        }

        return;
    }

    frame_pump(&frame_info);

    if (play_stats_due && frame_read_idle())
    {
        play_stats();
        mem_log("playing");
        play_stats_due = 0;
    }

    now = GetTimerSystemTime();

    /*
     * Quadros/s medidos a cada segundo.
     */
    if (now - fps_t0 >= (u64)kBUSCLK)
    {
        fps_x10 = (int)(((u64)fps_frames * 10ULL * kBUSCLK) / (now - fps_t0));
        fps_frames = 0;
        fps_t0 = now;
    }

    /*
     * Barra e atalhos fazem as mesmas ações (play_action), tocando ou
     * pausado; quando a barra usa a entrada, o resto não a vê.
     */
    bar_used = 0;

    if (frame_state == 2 || frame_state == 6)
    {
        bar_used = play_bar_input(now);

        if (!bar_used)
            bar_used = play_shortcuts(now);

        if (frame_state == 0)
            return;     /* parou */
    }

    if (frame_state == 6)
        return;

    if (frame_state == 5)
    {
        if (frame_ready(&frame_info))
        {
            frame_state = 2;
            play_next_due = now;
        }

        return;
    }

    /*
     * Tocando. No menu do DVD o D-pad escolhe o botão e o X ativa.
     */
    if (!bar_used && frame_info.nav && frame_info.buttons > 0)
    {
        if (input_pressed(PAD_UP))
            nav_button_move(NAV_UP);
        else if (input_pressed(PAD_DOWN))
            nav_button_move(NAV_DOWN);
        else if (input_pressed(PAD_LEFT))
            nav_button_move(NAV_LEFT);
        else if (input_pressed(PAD_RIGHT))
            nav_button_move(NAV_RIGHT);
        else if (input_pressed(PAD_CROSS) && nav_button_activate())
        {
            frame_nav_flush(&frame_info);
            frame_state = 5;
            return;
        }
    }


    /*
     * Still frame do DVD: a imagem fica parada; frame_pump conta o
     * tempo e manda o disco seguir.
     */
    if (frame_still_hold(&frame_info))
    {
        play_next_due = now;
        return;
    }

    if (now < play_next_due)
        return;

    if (!frame_ready(&frame_info))
    {
        play_rebuffer++;
        frame_state = 5;
        return;
    }

    /*
     * Mais de um quadro atrasado: decodifica sem converter nem
     * mostrar, para o vídeo (e o som, que o segue) voltar ao horário.
     */
    if (now > play_next_due + us_to_ticks(frame_info.frame_us ? frame_info.frame_us : 33367) &&
        frame_info.frames > 0)
    {
        result = frame_next_skip(frame_pixels, &frame_info);

        if (result == 1)
        {
            play_skipped++;
            play_next_due += us_to_ticks(frame_info.frame_us ? frame_info.frame_us : 33367);

            if ((frame_info.frames % 300) == 0)
                play_stats_due = 1;

            return;
        }
    }
    else
        result = frame_next(frame_pixels, &frame_info);

    if (result == 2)
    {
        /* navegação: fim de sequência, o disco continua */
        return;
    }

    if (result > 0)
    {
        frame_upload = 1;
        fps_frames++;

        play_next_due += us_to_ticks(frame_info.frame_us ? frame_info.frame_us : 33367);

        /*
         * Mais de 4 quadros atrasado: reajusta o relógio em vez de
         * acumular atraso.
         */
        if (now > play_next_due + us_to_ticks(4 * frame_info.frame_us))
        {
            play_next_due = now;
            play_late++;
        }

        if ((frame_info.frames % 300) == 0)
            play_stats_due = 1;
    }
    else if (result == 0)
    {
        play_stats_due = 1;
        frame_state = 4;
    }
    else
    {
        frame_state = -1;
    }
}


/*
 * Tocando vídeo sem quadro novo e com mais de um retrace até o
 * próximo: não desenha nem espera o retrace, e o laço usa o tempo
 * para ler o disco e decodificar o som.
 */
static int frame_skip_render(void)
{
    if (current_screen != SCREEN_FRAME || frame_state != 2 || frame_upload)
        return 0;

    if (frame_still_hold(&frame_info))
        return 0;

    return GetTimerSystemTime() + us_to_ticks(17000) < play_next_due;
}


/* ============================================================
 * CONTROLE DA TELA SMB
 * ============================================================ */

static void update_smb(void)
{
    /*
     * Editor ativo.
     */

    if (smb_editing)
    {
        update_smb_editor();
        return;
    }


    /*
     * CIRCLE = voltar.
     */

    if (input_pressed(PAD_CIRCLE))
    {
        current_screen = SCREEN_MAIN;
        return;
    }


    /*
     * UP
     */

    if (input_pressed(PAD_UP))
    {
        smb_selected--;

        if (smb_selected < 0)
            smb_selected =
                SMB_ITEM_COUNT - 1;
    }


    /*
     * DOWN
     */

    if (input_pressed(PAD_DOWN))
    {
        smb_selected++;

        if (
            smb_selected >=
            SMB_ITEM_COUNT
        )
        {
            smb_selected = 0;
        }
    }


    /*
     * LEFT / RIGHT em MODE.
     */

    if (
        smb_selected ==
        SMB_ITEM_MODE
    )
    {
        if (
            input_pressed(PAD_LEFT) ||
            input_pressed(PAD_RIGHT)
        )
        {
            if (
                smb_mode ==
                NETWORK_DHCP
            )
            {
                smb_mode =
                    NETWORK_STATIC;
            }
            else
            {
                smb_mode =
                    NETWORK_DHCP;
            }


            printf(
                "SMB: selected mode = %s\n",
                smb_mode == NETWORK_DHCP
                    ? "DHCP"
                    : "STATIC"
            );
        }
    }


    /*
     * CROSS
     */

    if (input_pressed(PAD_CROSS))
    {
        printf(
            "SMB SCREEN: CROSS selected=%d\n",
            smb_selected
        );


        /*
         * MODE
         */

        if (
            smb_selected ==
            SMB_ITEM_MODE
        )
        {
            if (
                smb_mode ==
                NETWORK_DHCP
            )
            {
                smb_mode =
                    NETWORK_STATIC;
            }
            else
            {
                smb_mode =
                    NETWORK_DHCP;
            }


            printf(
                "SMB: selected mode = %s\n",
                smb_mode == NETWORK_DHCP
                    ? "DHCP"
                    : "STATIC"
            );
        }


        /*
         * IP
         */

        else if (
            smb_selected ==
                SMB_ITEM_IP &&
            smb_mode ==
                NETWORK_STATIC
        )
        {
            smb_editing = 1;
            smb_octet = 0;
        }


        /*
         * MASK
         */

        else if (
            smb_selected ==
                SMB_ITEM_MASK &&
            smb_mode ==
                NETWORK_STATIC
        )
        {
            smb_editing = 1;
            smb_octet = 0;
        }


        /*
         * GATEWAY
         */

        else if (
            smb_selected ==
                SMB_ITEM_GATEWAY &&
            smb_mode ==
                NETWORK_STATIC
        )
        {
            smb_editing = 1;
            smb_octet = 0;
        }


        /*
         * DNS
         */

        else if (
            smb_selected ==
                SMB_ITEM_DNS &&
            smb_mode ==
                NETWORK_STATIC
        )
        {
            smb_editing = 1;
            smb_octet = 0;
        }


        /*
         * SERVIDOR SMB (DHCP ou STATIC)
         */

        else if (
            smb_selected ==
            SMB_ITEM_SERVER
        )
        {
            smb_editing = 1;
            smb_octet = 0;
        }


        /*
         * APPLY
         */

        else if (
            smb_selected ==
            SMB_ITEM_APPLY
        )
        {
            printf("\n");
            printf(
                "=================================\n"
            );
            printf(
                " SMB APPLY REQUEST\n"
            );
            printf(
                "=================================\n"
            );

            printf(
                "SMB SCREEN: APPLY pressed\n"
            );


            if (!smb_waiting_network)
            {
                smb_error_line[0] = '\0';

                smb_apply();
            }
            else
            {
                printf(
                    "SMB SCREEN: already waiting for network\n"
                );
            }
        }


        /*
         * BACK
         */

        else if (
            smb_selected ==
            SMB_ITEM_BACK
        )
        {
            current_screen =
                SCREEN_MAIN;
        }
    }
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(
    int argc,
    char *argv[]
)
{
    GSGLOBAL *gsGlobal;
    GSFONTM *font;

    int iop_result;


    (void)argc;
    (void)argv;


    printf("\n");
    printf(
        "================================\n"
    );
    printf(
        " PS2DVDPlayer\n"
    );
    printf(
        " HDD TEST\n"
    );
    printf(
        "================================\n"
    );


    /* ========================================================
     * RESET LIMPO DO IOP
     * ========================================================
     *
     * IMPORTANTE:
     *
     * Este reset deve acontecer antes de:
     *
     * - input_init()
     * - DEV9
     * - HDD
     * - FILEXIO
     * - rede
     *
     * O reset acontece somente uma vez durante
     * a inicialização do PS2DVDPlayer.
     */

    printf("\n");
    printf(
        "=================================\n"
    );
    printf(
        " PS2DVDPlayer - BOOT\n"
    );
    printf(
        "=================================\n"
    );


    printf(
        "MAIN: resetting IOP...\n"
    );


    iop_result =
        iop_boot_reset();


    printf(
        "MAIN: iop_boot_reset returned %d\n",
        iop_result
    );


    if (iop_result < 0)
    {
        printf(
            "MAIN: ERROR initializing IOP\n"
        );

        printf(
            "MAIN: system halted\n"
        );


        while (1)
        {
        }
    }


    printf(
        "MAIN: IOP bootstrap OK\n"
    );


    /* ========================================================
     * REDE
     * ======================================================== */

    printf(
        "MAIN: calling network_init()...\n"
    );


    network_result =
        network_init();


    printf(
        "MAIN: network_init returned %d\n",
        network_result
    );

    printf("MAIN: audio_init_output returned %d\n", audio_init_output());

    mem_log("boot");


    {
        const NetworkInfo *net;

        net =
            network_get_info();

        if (net)
        {
            smb_mode =
                net->mode;
        }
    }


    /*
     * GS
     */

    /*
     * GS
     */

    printf(
        "MAIN: initializing GS...\n"
    );


    gsGlobal =
        gsKit_init_global();


    if (!gsGlobal)
    {
        printf(
            "MAIN: ERROR gsKit_init_global\n"
        );

        while (1)
        {
        }
    }


    gsGlobal->Mode =
        GS_MODE_DTV_480P;

    gsGlobal->Interlace =
        GS_NONINTERLACED;

    gsGlobal->Field =
        GS_FRAME;

    gsGlobal->Width =
        640;

    gsGlobal->Height =
        480;

    gsGlobal->PSM =
        GS_PSM_CT24;

    gsGlobal->PSMZ =
        GS_PSMZ_16S;

    gsGlobal->DoubleBuffering =
        GS_SETTING_ON;

    gsGlobal->ZBuffering =
        GS_SETTING_OFF;


    /*
     * DMA
     */

    printf(
        "MAIN: initializing DMA...\n"
    );


    dmaKit_init(
        D_CTRL_RELE_OFF,
        D_CTRL_MFD_OFF,
        D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF,
        D_CTRL_RCYC_8,
        1 << DMA_CHANNEL_GIF
    );


    dmaKit_chan_init(
        DMA_CHANNEL_GIF
    );


    /*
     * SCREEN
     */

    printf(
        "MAIN: initializing screen...\n"
    );


    gsKit_init_screen(
        gsGlobal
    );


    /*
     * FONT
     */

    printf(
        "MAIN: initializing font...\n"
    );


    font =
        gsKit_init_fontm();


    if (!font)
    {
        printf(
            "MAIN: ERROR FontM\n"
        );

        while (1)
        {
        }
    }


    gsKit_fontm_upload(
        gsGlobal,
        font
    );


    /*
     * CORES
     */

    COLOR_BG =
        GS_SETREG_RGBAQ(
            0x05,
            0x08,
            0x10,
            0x80,
            0x00
        );


    COLOR_WHITE =
        GS_SETREG_RGBAQ(
            0xFF,
            0xFF,
            0xFF,
            0x80,
            0x00
        );


    COLOR_SELECTED =
        GS_SETREG_RGBAQ(
            0x20,
            0x60,
            0xA0,
            0x80,
            0x00
        );


    COLOR_PANEL =
        GS_SETREG_RGBAQ(
            0x12,
            0x18,
            0x28,
            0x80,
            0x00
        );


    COLOR_OK =
        GS_SETREG_RGBAQ(
            0x20,
            0xE0,
            0x60,
            0x80,
            0x00
        );


    COLOR_ERROR =
        GS_SETREG_RGBAQ(
            0xFF,
            0x30,
            0x30,
            0x80,
            0x00
        );


    COLOR_DISABLED =
        GS_SETREG_RGBAQ(
            0x70,
            0x70,
            0x70,
            0x80,
            0x00
        );


    /*
     * PRIMEIRO FRAME
     */

    draw_main(
        gsGlobal,
        font
    );


    gsKit_queue_exec(
        gsGlobal
    );


    gsKit_sync_flip(
        gsGlobal
    );


    /*
     * CONTROLE
     */

    printf(
        "MAIN: calling input_init()...\n"
    );


    input_result =
        input_init();


    printf(
        "MAIN: input_init returned %d\n",
        input_result
    );


    /*
     * MEMORY CARD (depende do XSIO2MAN de input_init)
     */

    printf(
        "MAIN: memcard_init returned %d\n",
        memcard_init()
    );


    /*
     * LOOP PRINCIPAL
     */

    while (1)
    {
        /*
         * Controle.
         */

        input_update();

        /*
         * Rede.
         */
      
        /*
         * Durante o vídeo, evita RPCs de rede a cada quadro.
         */
        if (current_screen != SCREEN_FRAME)
            network_update();

        /*
         * Aguarda DHCP/link.
         *
         * Quando a rede estiver pronta,
         * smb_test() será chamado
         * automaticamente.
         */
        smb_wait_network_update();


        /*
         * Processa tela.
         */

        if (
            current_screen ==
            SCREEN_MAIN
        )
        {
            update_main();
        }
        else if (
            current_screen ==
            SCREEN_ISO_LIST
        )
        {
            update_iso_list();
        }
        else if (
            current_screen ==
            SCREEN_ISO_INFO
        )
        {
            update_iso_info();
        }
        else if (
            current_screen ==
            SCREEN_FRAME
        )
        {
            update_frame();
        }
        else
        {
            update_smb();
        }


        if (frame_skip_render())
            continue;

        /*
         * Desenha.
         */

        if (
            current_screen ==
            SCREEN_MAIN
        )
        {
            draw_main(
                gsGlobal,
                font
            );
        }
        else if (
            current_screen ==
            SCREEN_ISO_LIST
        )
        {
            draw_iso_list(
                gsGlobal,
                font
            );
        }
        else if (
            current_screen ==
            SCREEN_ISO_INFO
        )
        {
            draw_iso_info(
                gsGlobal,
                font
            );
        }
        else if (
            current_screen ==
            SCREEN_FRAME
        )
        {
            draw_frame(
                gsGlobal,
                font
            );
        }
        else
        {
            draw_smb(
                gsGlobal,
                font
            );
        }


        /*
         * Envia frame.
         */

        gsKit_queue_exec(
            gsGlobal
        );


        gsKit_sync_flip(
            gsGlobal
        );
    }


    input_shutdown();
    network_shutdown();

    return 0;
}
