/* SPDX-License-Identifier: MIT
 * Independently written from the transport behavior documented in README.md.
 */
#include "protocol.h"
#include <string.h>

static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static int signed16(const uint8_t *p) { return (int16_t)le16(p); }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

uint16_t ji_crc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit != 8; ++bit)
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
    }
    return crc;
}

size_t ji_request(uint8_t *p, uint8_t target, uint8_t command, uint8_t report,
                  uint8_t interface_id, uint8_t sequence, uint16_t response_size,
                  const uint8_t *data, size_t n)
{
    if (n > 236 || (n && !data)) return 0;
    memset(p, 0, JI_PACKET);
    p[0] = 0x40; p[1] = target;
    put16(p + 6, (unsigned)n + 10);
    p[8] = command; p[9] = report; p[10] = interface_id; p[11] = sequence;
    put16(p + 12, response_size); put16(p + 14, (unsigned)n);
    if (n) memcpy(p + 16, data, n);
    put16(p + 16 + n, ji_crc(p + 8, n + 8));
    put16(p + 254, ji_crc(p, 254));
    return JI_PACKET;
}

int ji_receive(struct ji_assembly *a, const uint8_t *p, ji_message_fn deliver, void *ctx)
{
    size_t offset = le16(p + 2), remain = le16(p + 4), n = le16(p + 6);
    if (ji_crc(p, JI_PACKET) || n > 246 || !n ||
        offset + n + remain > JI_MESSAGE)
        goto invalid;
    if (!offset && !remain && n == 4 && !memcmp(p + 8, "\xa0\x80\0\0", 4)) {
        a->used = a->total = 0;
        return 2;
    }
    if (p[0] != 0x20 && p[0] != 0x40) goto invalid;
    if (!offset) {
        a->used = 0; a->total = n + remain;
        a->device = p[1]; a->flags = p[0];
    }
    if (offset != a->used || a->total != offset + n + remain ||
        a->device != p[1] || a->flags != p[0]) goto invalid;
    memcpy(a->data + offset, p + 8, n);
    a->used += n;
    if (remain) return 0;
    if (a->used < 10 || le16(a->data + 6) != a->used - 10 ||
        ji_crc(a->data, a->used)) goto invalid;
    deliver(ctx, a->device, a->flags, a->data, a->used);
    a->used = a->total = 0;
    return 1;
invalid:
    a->used = a->total = 0;
    return -1;
}

const uint8_t ji_mouse_descriptor[] = {
    0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x85,0x01,
    0x09,0x01, 0xa1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x03,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x03, 0x81,0x02,
    0x75,0x05, 0x95,0x01, 0x81,0x03,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x16,0x01,0x80, 0x26,0xff,0x7f,
    0x75,0x10, 0x95,0x02, 0x81,0x06,
    0x09,0x38, 0x15,0x81, 0x25,0x7f, 0x75,0x08, 0x95,0x01, 0x81,0x06,
    0x05,0x0c, 0x0a,0x38,0x02, 0x81,0x06, 0xc0,0xc0
};
const size_t ji_mouse_descriptor_size = sizeof(ji_mouse_descriptor);

static int clamp(int x, int limit) { return x > limit ? limit : x < -limit ? -limit : x; }

int ji_trackpad(struct ji_mouse *m, const uint8_t *p, size_t n, uint8_t out[8])
{
    int count = 0, x = 0, y = 0, dx = 0, dy = 0, wheel = 0, pan = 0;
    uint8_t pressed;
    if (n && p[0] == 0x60) { memset(m, 0, sizeof(*m)); return 2; }
    if (n < 8 || p[0] != 2) return -1;
    memset(out, 0, 8); out[0] = 1;
    /* The eight-byte standard mouse prefix is also usable before MT mode. */
    if (n == 8) {
        out[1] = p[1] & 7;
        put16(out + 2, (unsigned)(int)(int8_t)p[2]);
        put16(out + 4, (unsigned)(int)(int8_t)p[3]);
        memset(m, 0, sizeof(*m)); m->buttons = out[1];
        return 1;
    }
    /* Eight-byte mouse prefix + 38-byte vendor header + 30 per contact. */
    if (n < 46 || (n - 46) % 30 || p[30] > 16 || p[30] > (n - 46) / 30)
        return -1;
    for (unsigned i = 0; i < p[30]; ++i) {
        const uint8_t *f = p + 46 + i * 30;
        if (signed16(f + 18) <= 0) continue;
        x += signed16(f + 4); y -= signed16(f + 6); ++count;
    }
    if (count) { x /= count; y /= count; }
    pressed = p[31] & 1;
    if (!pressed) m->buttons = 0;
    else if (!m->buttons) m->buttons = count >= 2 ? 2 : 1;
    if (count && count == m->contacts) {
        int mx = x - m->x, my = y - m->y;
        if (count == 1 || m->buttons) {
            m->rem_x += mx; m->rem_y += my;
            dx = m->rem_x / 8; dy = m->rem_y / 8;
            m->rem_x %= 8; m->rem_y %= 8;
        } else if (count == 2) {
            m->scroll_x += mx; m->scroll_y -= my;
            pan = clamp(m->scroll_x / 120, 127);
            wheel = clamp(m->scroll_y / 120, 127);
            m->scroll_x -= pan * 120; m->scroll_y -= wheel * 120;
        }
    } else {
        m->rem_x = m->rem_y = m->scroll_x = m->scroll_y = 0;
    }
    m->x = x; m->y = y; m->contacts = count;
    out[1] = m->buttons;
    put16(out + 2, (unsigned)clamp(dx, 32767));
    put16(out + 4, (unsigned)clamp(dy, 32767));
    out[6] = (uint8_t)wheel; out[7] = (uint8_t)pan;
    return 1;
}
