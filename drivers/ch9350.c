#include "ch9350.h"

static uint8_t frame_size(uint8_t code)
{
    switch (code) {
    case 0x01: return 11;
    case 0x02: return 7;
    case 0x80: return 4;
    case 0x86: return 3;
    default: return 0;
    }
}

int ch9350_feed(struct ch9350_parser *p, uint8_t byte, struct ch9350_event *ev)
{
    if (p->len == 0) {
        if (byte == 0x57) p->frame[p->len++] = byte;
        else p->bad_bytes++;
        return 0;
    }
    if (p->len == 1) {
        if (byte == 0xab) p->frame[p->len++] = byte;
        else { p->bad_bytes++; p->len = byte == 0x57; }
        return 0;
    }
    if (p->len == 2) {
        p->need = frame_size(byte);
        if (!p->need) { p->bad_bytes += 3; p->len = 0; return 0; }
    }
    p->frame[p->len++] = byte;
    if (p->len < p->need) return 0;
    p->len = 0;
    for (int i = 0; i < 8; ++i) ev->data[i] = 0;
    switch (p->frame[2]) {
    case 0x01:
        ev->type = CH9350_KEYBOARD;
        for (int i = 0; i < 8; ++i) ev->data[i] = p->frame[3 + i];
        break;
    case 0x02:
        ev->type = CH9350_MOUSE;
        for (int i = 0; i < 4; ++i) ev->data[i] = p->frame[3 + i];
        break;
    case 0x80:
        ev->type = CH9350_STATUS;
        ev->data[0] = p->frame[3];
        break;
    default:
        ev->type = CH9350_REMOVED;
        break;
    }
    return 1;
}
