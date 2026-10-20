#include <stdio.h>
#include <string.h>

#include <tamtypes.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <kernel.h>
#include <libpad.h>

#include "input.h"

#define PAD_PORT 0
#define PAD_SLOT 0

static unsigned char padBuffer[256]
    __attribute__((aligned(64)));

static struct padButtonStatus padStatus
    __attribute__((aligned(64)));

static InputState inputState;

static u16 previousButtons;
static int padOpened;

int input_init(void)
{
    int ret;

    memset(&inputState, 0, sizeof(inputState));
    memset(&padStatus, 0, sizeof(padStatus));
    memset(padBuffer, 0, sizeof(padBuffer));

    printf("INPUT: SIF RPC\n");

    sceSifInitRpc(0);

    /*
     * Carrega os drivers oficiais da ROM.
     *
     * XSIO2MAN = controlador SIO2
     * XPADMAN  = controlador de gamepad
     */
    printf("INPUT: loading XSIO2MAN\n");

    ret = SifLoadModule(
        "rom0:XSIO2MAN",
        0,
        NULL
    );

    if (ret < 0)
    {
        printf(
            "INPUT: XSIO2MAN failed %d\n",
            ret
        );

        return -1;
    }

    printf("INPUT: loading XPADMAN\n");

    ret = SifLoadModule(
        "rom0:XPADMAN",
        0,
        NULL
    );

    if (ret < 0)
    {
        printf(
            "INPUT: XPADMAN failed %d\n",
            ret
        );

        return -1;
    }

    printf("INPUT: padInit\n");

    ret = padInit(0);

    if (ret != 1)
    {
        printf(
            "INPUT: padInit failed %d\n",
            ret
        );

        return -1;
    }

    printf("INPUT: padPortOpen\n");

    ret = padPortOpen(
        PAD_PORT,
        PAD_SLOT,
        padBuffer
    );

    if (!ret)
    {
        printf(
            "INPUT: padPortOpen failed\n"
        );

        return -1;
    }

    padOpened = 1;

    printf(
        "INPUT: controller opened\n"
    );

    return 0;
}

void input_update(void)
{
    int state;
    unsigned char result;

    if (!padOpened)
    {
        inputState.connected = 0;
        return;
    }

    state = padGetState(
        PAD_PORT,
        PAD_SLOT
    );

    if (state != PAD_STATE_STABLE)
    {
        inputState.connected = 0;

        inputState.buttons = 0;
        inputState.pressed = 0;
        inputState.released = 0;

        return;
    }

    result = padRead(
        PAD_PORT,
        PAD_SLOT,
        &padStatus
    );

    if (!result)
    {
        inputState.connected = 0;
        return;
    }

    inputState.connected = 1;

    inputState.buttons =
        (u16)(~padStatus.btns);

    inputState.pressed =
        inputState.buttons &
        (u16)~previousButtons;

    inputState.released =
        previousButtons &
        (u16)~inputState.buttons;

    previousButtons =
        inputState.buttons;

    inputState.lx =
        padStatus.ljoy_h;

    inputState.ly =
        padStatus.ljoy_v;

    inputState.rx =
        padStatus.rjoy_h;

    inputState.ry =
        padStatus.rjoy_v;
}

void input_shutdown(void)
{
    if (padOpened)
    {
        padPortClose(
            PAD_PORT,
            PAD_SLOT
        );

        padOpened = 0;
    }

    padEnd();

    memset(
        &inputState,
        0,
        sizeof(inputState)
    );

    previousButtons = 0;
}

const InputState *input_get_state(void)
{
    return &inputState;
}

int input_pressed(u16 button)
{
    return
        (inputState.pressed & button) != 0;
}

int input_down(u16 button)
{
    return
        (inputState.buttons & button) != 0;
}

int input_released(u16 button)
{
    return
        (inputState.released & button) != 0;
}
