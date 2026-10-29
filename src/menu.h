#pragma once
#include <stdint.h>
#include <stdbool.h>

// Controller keys, mapped from the pad by main.c
enum {
    KEY_UP      = 1 << 0,
    KEY_DOWN    = 1 << 1,
    KEY_LEFT    = 1 << 2,
    KEY_RIGHT   = 1 << 3,
    KEY_CONFIRM = 1 << 4, // Cross
    KEY_BACK    = 1 << 5, // Circle
    KEY_ALT     = 1 << 6, // Square
    KEY_L1      = 1 << 7,
    KEY_R1      = 1 << 8,
    KEY_MENU    = 1 << 9, // OPTIONS
};

typedef enum {
    MENU_NONE,
    MENU_RECONNECT,      // settings changed that need a new connection
    MENU_TARGET_CHANGED, // Switch IP changed
} MenuAction;

void menu_open(void);
bool menu_is_open(void);
// Handles the keys pressed this frame
MenuAction menu_input(uint32_t keys);
void menu_draw(uint32_t *fb);
