/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kernel/inputev.c: key events between keyboard reports (modifiers, several
 * keys, releases before presses, characters, rollover), keypad events, and
 * the queue (order, full, merged mouse motion). */
#include <stdint.h>
#include <string.h>
#include "../../kernel/inputev.h"
#include "check.h"

static struct nn_input_event ev[16];

static int keys(const uint8_t *prev, const uint8_t *now, int caps)
{
    memset(ev, 0, sizeof ev);
    return input_key_events(prev, now, caps, 7, ev, 16);
}

static int is(int i, int type, int code, int value, char ch)
{
    return ev[i].type == type && ev[i].code == code && ev[i].value == value && ev[i].ch == ch && ev[i].ms == 7;
}

int main(void)
{
    static const uint8_t none[8];
    uint8_t a[8] = { 0, 0, 0x04 }, shift_a[8] = { 0x02, 0, 0x04 }, ab[8] = { 0, 0, 0x05, 0x04 };
    uint8_t up[8] = { 0, 0, 0x52 }, roll[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };

    CHECK_EQ_INT(sizeof(struct nn_input_event), 8);
    CHECK_EQ_INT(keys(none, a, 0), 1);
    CHECK(is(0, NN_INPUT_KEY, 0x04, 1, 'a'));
    CHECK_EQ_INT(keys(none, a, 1), 1);
    CHECK(is(0, NN_INPUT_KEY, 0x04, 1, 'A'));                 /* Caps Lock */
    CHECK_EQ_INT(keys(a, shift_a, 0), 1);                       /* only the modifier is new */
    CHECK(is(0, NN_INPUT_KEY, NN_KEY_LSHIFT, 1, 0));
    CHECK_EQ_INT(keys(none, shift_a, 0), 2);                    /* modifier first, then the key with it */
    CHECK(is(0, NN_INPUT_KEY, NN_KEY_LSHIFT, 1, 0) && is(1, NN_INPUT_KEY, 0x04, 1, 'A'));
    CHECK_EQ_INT(keys(a, ab, 0), 1);                            /* a stays held, at another place */
    CHECK(is(0, NN_INPUT_KEY, 0x05, 1, 'b'));
    CHECK_EQ_INT(keys(ab, up, 0), 3);                           /* releases first */
    CHECK(is(0, NN_INPUT_KEY, 0x05, 0, 0) && is(1, NN_INPUT_KEY, 0x04, 0, 0) && is(2, NN_INPUT_KEY, NN_KEY_UP, 1, 0));
    CHECK_EQ_INT(keys(shift_a, none, 0), 2);
    CHECK(is(0, NN_INPUT_KEY, NN_KEY_LSHIFT, 0, 0) && is(1, NN_INPUT_KEY, 0x04, 0, 0));
    CHECK_EQ_INT(keys(a, roll, 0), 0);                          /* ErrorRollOver: nothing changes */
    CHECK_EQ_INT(keys(a, a, 0), 0);
    uint8_t all_mods[8] = { 0xff };
    memset(ev, 0, sizeof ev);
    CHECK_EQ_INT(input_key_events(none, all_mods, 0, 7, ev, 3), 3);   /* cut at max */
    CHECK(is(2, NN_INPUT_KEY, 0xe2, 1, 0));

    memset(ev, 0, sizeof ev);
    CHECK_EQ_INT(input_pad_events(0, 0x011, 7, ev, 9), 2);      /* 1 and 5 */
    CHECK(is(0, NN_INPUT_PAD, 1, 1, 0) && is(1, NN_INPUT_PAD, 5, 1, 0));
    CHECK_EQ_INT(input_pad_events(0x011, 0x110, 7, ev, 9), 2);  /* 1 up, 9 down */
    CHECK(is(0, NN_INPUT_PAD, 1, 0, 0) && is(1, NN_INPUT_PAD, 9, 1, 0));

    static struct input_queue q;
    struct nn_input_event e, m = { NN_INPUT_MOUSE, 0, 0, 0, 100, -100, 1 };
    CHECK_EQ_INT(inputq_get(&q, &e), 0);
    CHECK_EQ_INT(inputq_put(&q, &m), 0);
    m.ms = 2;
    CHECK_EQ_INT(inputq_put(&q, &m), 0);                        /* 200 does not fit: another event */
    m.dx = 20; m.dy = -20; m.ms = 3;
    CHECK_EQ_INT(inputq_put(&q, &m), 0);                        /* joins the second */
    CHECK_EQ_INT(q.len, 2);
    m.code = NN_MOUSE_LEFT;
    CHECK_EQ_INT(inputq_put(&q, &m), 0);                        /* a button changed: its own event */
    m.value = 1;
    CHECK_EQ_INT(inputq_put(&q, &m), 0);                        /* the wheel: never merged */
    CHECK_EQ_INT(inputq_put(&q, &m), 0);
    CHECK_EQ_INT(q.len, 5);
    CHECK(inputq_get(&q, &e) && e.dx == 100 && e.dy == -100 && e.ms == 1);
    CHECK(inputq_get(&q, &e) && e.dx == 120 && e.dy == -120 && e.ms == 3);
    CHECK(inputq_get(&q, &e) && e.code == NN_MOUSE_LEFT && e.value == 0);
    CHECK(inputq_get(&q, &e) && e.value == 1);
    CHECK(inputq_get(&q, &e) && e.value == 1);
    CHECK_EQ_INT(inputq_get(&q, &e), 0);
    struct nn_input_event k = { NN_INPUT_KEY, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < INPUT_EVENTS; ++i) { k.code = (uint8_t)i; CHECK_EQ_INT(inputq_put(&q, &k), 0); }
    CHECK_EQ_INT(inputq_put(&q, &k), -1);                       /* full: dropped, counted */
    CHECK_EQ_INT(q.lost, 1);
    int order = 1;
    for (int i = 0; i < INPUT_EVENTS; ++i) order &= inputq_get(&q, &e) && e.code == i;
    CHECK(order);                                               /* oldest first, across the wrap */
    return check_summary("test_inputev");
}
