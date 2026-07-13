#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons;
    int8_t  wheel;
    uint8_t present;
} mouse_state_t;

void mouse_init(void);
void mouse_get_state(mouse_state_t *state);

#endif
