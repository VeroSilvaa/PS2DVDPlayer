#ifndef PS2DVDPLAYER_INPUT_H
#define PS2DVDPLAYER_INPUT_H

#include <tamtypes.h>

typedef struct
{
    u16 buttons;
    u16 pressed;
    u16 released;

    u8 connected;

    u8 lx;
    u8 ly;
    u8 rx;
    u8 ry;
} InputState;

int input_init(void);
void input_update(void);
void input_shutdown(void);

const InputState *input_get_state(void);

int input_pressed(u16 button);
int input_down(u16 button);
int input_released(u16 button);

#endif
