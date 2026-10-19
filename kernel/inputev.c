/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "inputev.h"
#include "../drivers/hidkey.h"

static void note(struct input_told *t, const struct nn_input_event *e)
{
    if (e->type == NN_INPUT_KEY) {
        if (e->value) t->keys[e->code >> 3] |= (uint8_t)(1u << (e->code & 7u));
        else t->keys[e->code >> 3] &= (uint8_t)~(1u << (e->code & 7u));
    } else if (e->type == NN_INPUT_MOUSE) {
        t->buttons = e->code;
    } else if (e->type == NN_INPUT_PAD && e->code >= 1 && e->code <= 9) {
        if (e->value) t->pad |= (uint16_t)(1u << (e->code - 1));
        else t->pad &= (uint16_t)~(1u << (e->code - 1));
    }
}

static int put(struct input_queue *q, const struct nn_input_event *e)
{
    if (e->type == NN_INPUT_MOUSE && !e->value && q->len) {
        struct nn_input_event *last = &q->ev[(q->head + q->len - 1u) % INPUT_EVENTS];
        int dx = last->dx + e->dx, dy = last->dy + e->dy;
        if (last->type == NN_INPUT_MOUSE && !last->value && last->code == e->code &&
            dx >= -127 && dx <= 127 && dy >= -127 && dy <= 127) {
            last->dx = (int8_t)dx;
            last->dy = (int8_t)dy;
            last->ms = e->ms;
            return 0;
        }
    }
    if (q->len == INPUT_EVENTS) return -1;
    q->ev[(q->head + q->len++) % INPUT_EVENTS] = *e;
    note(&q->told, e);
    return 0;
}

int inputq_put(struct input_queue *q, const struct nn_input_event *e)
{
    if (!q->resync && !put(q, e)) return 0;
    q->resync = 1;
    q->lost++;
    return -1;
}

static int report_has(const uint8_t r[8], uint8_t usage);

static int sync_one(struct input_queue *q, struct nn_input_event e)
{
    if (q->len == INPUT_EVENTS) return -1;
    q->ev[(q->head + q->len++) % INPUT_EVENTS] = e;
    note(&q->told, &e);
    return 0;
}

static int is_down(const struct input_state *now, unsigned u)
{
    return u >= 0xe0 && u < 0xe8 ? (now->report[0] >> (u - 0xe0)) & 1 : report_has(now->report, (uint8_t)u);
}

/* Interrupts are off around this in the kernel: it looks only at the keys
 * held (told or now), not at every usage. */
void inputq_sync(struct input_queue *q, const struct input_state *now, uint16_t ms)
{
    if (!q->resync) return;
    struct input_told *t = &q->told;
    for (unsigned i = 0; i < sizeof t->keys; ++i)       /* releases first */
        for (unsigned b = 0; t->keys[i] && b < 8; ++b) {
            unsigned u = i * 8 + b;
            if (!(t->keys[i] >> b & 1) || is_down(now, u)) continue;
            if (sync_one(q, (struct nn_input_event){ NN_INPUT_KEY, (uint8_t)u, 0, 0, 0, 0, ms })) return;
        }
    for (int k = 0; k < 9; ++k)
        if ((t->pad >> k & 1) && !(now->pad >> k & 1) &&
            sync_one(q, (struct nn_input_event){ NN_INPUT_PAD, (uint8_t)(k + 1), 0, 0, 0, 0, ms })) return;
    for (unsigned i = 0; i < 14; ++i) {                /* then presses: 8 modifiers, 6 keys */
        unsigned u = i < 8 ? 0xe0 + i : now->report[i - 6];
        if (u <= 1 || (t->keys[u >> 3] >> (u & 7) & 1) || !is_down(now, u)) continue;
        char ch = u < 0xe0 ? hid_to_ascii((uint8_t)u, now->report[0], now->caps) : 0;
        if (sync_one(q, (struct nn_input_event){ NN_INPUT_KEY, (uint8_t)u, 1, ch, 0, 0, ms })) return;
    }
    for (int k = 0; k < 9; ++k)
        if (!(t->pad >> k & 1) && (now->pad >> k & 1) &&
            sync_one(q, (struct nn_input_event){ NN_INPUT_PAD, (uint8_t)(k + 1), 1, 0, 0, 0, ms })) return;
    if (t->buttons != now->buttons &&
        sync_one(q, (struct nn_input_event){ NN_INPUT_MOUSE, now->buttons, 0, 0, 0, 0, ms })) return;
    q->resync = 0;
}

int inputq_get(struct input_queue *q, struct nn_input_event *e)
{
    if (!q->len) return 0;
    *e = q->ev[q->head];
    q->head = (uint8_t)((q->head + 1u) % INPUT_EVENTS);
    q->len--;
    return 1;
}

static int report_has(const uint8_t r[8], uint8_t usage)
{
    for (int i = 2; i < 8; ++i) if (r[i] == usage) return 1;
    return 0;
}

int input_report_valid(const uint8_t r[8])
{
    for (int i = 2; i < 8; ++i) if (r[i] == 1) return 0;
    return 1;
}


static int key(struct nn_input_event *out, int n, int max, uint8_t usage, int down, char ch, uint16_t ms)
{
    if (n < max) out[n] = (struct nn_input_event){ NN_INPUT_KEY, usage, (uint8_t)down, down ? ch : 0, 0, 0, ms };
    return n < max ? n + 1 : n;
}

int input_key_events(const uint8_t prev[8], const uint8_t now[8], int caps, uint16_t ms,
                     struct nn_input_event *out, int max)
{
    for (int i = 2; i < 8; ++i) if (now[i] == 1) return 0;       /* ErrorRollOver: too many keys */
    int n = 0;
    for (int b = 0; b < 8; ++b)                                  /* modifiers */
        if ((prev[0] & ~now[0]) & (1u << b)) n = key(out, n, max, (uint8_t)(0xe0 + b), 0, 0, ms);
    for (int i = 2; i < 8; ++i)
        if (prev[i] > 1 && !report_has(now, prev[i])) n = key(out, n, max, prev[i], 0, 0, ms);
    for (int b = 0; b < 8; ++b)
        if ((now[0] & ~prev[0]) & (1u << b)) n = key(out, n, max, (uint8_t)(0xe0 + b), 1, 0, ms);
    for (int i = 2; i < 8; ++i)
        if (now[i] > 1 && !report_has(prev, now[i])) n = key(out, n, max, now[i], 1, hid_to_ascii(now[i], now[0], caps), ms);
    return n;
}

int input_pad_events(uint16_t prev, uint16_t now, uint16_t ms, struct nn_input_event *out, int max)
{
    int n = 0;
    for (int k = 0; k < 9 && n < max; ++k)
        if ((prev ^ now) & (1u << k))
            out[n++] = (struct nn_input_event){ NN_INPUT_PAD, (uint8_t)(k + 1), (uint8_t)((now >> k) & 1u), 0, 0, 0, ms };
    return n;
}
