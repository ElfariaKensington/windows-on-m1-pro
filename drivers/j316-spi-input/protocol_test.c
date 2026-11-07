/* SPDX-License-Identifier: MIT */
#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int calls;
static size_t message_size;
static void put16(uint8_t *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
static void deliver(void *ctx, uint8_t device, uint8_t flags, const uint8_t *p, size_t n)
{
    (void)ctx; assert(device == 1 && flags == 0x20 && p[0] == 0x10);
    ++calls; message_size = n;
}
static void packet(uint8_t *p, const uint8_t *m, unsigned off, unsigned n, unsigned remain)
{
    memset(p, 0, 256); p[0] = 0x20; p[1] = 1;
    put16(p + 2, off); put16(p + 4, remain); put16(p + 6, n);
    memcpy(p + 8, m + off, n); put16(p + 254, ji_crc(p, 254));
}
static void finger(uint8_t *p, int x, int y)
{
    memset(p, 0, 30); put16(p + 4, x); put16(p + 6, y); put16(p + 18, 20);
}
int main(void)
{
    uint8_t p[256], msg[600] = {0x10}, report[8], touch[526] = {2};
    struct ji_assembly a = {0}; struct ji_mouse mouse = {0};
    assert(ji_crc((const uint8_t *)"123456789", 9) == 0xbb3d);
    assert(ji_request(p, 0xd0, 0x20, 1, 0xd0, 7, 0, NULL, 0) == 256);
    assert(p[11] == 7 && p[6] == 10 && !ji_crc(p, 256) && !ji_crc(p + 8, 10));
    assert(!ji_request(p, 1, 0, 0, 0, 0, 0, msg, 237));
    put16(msg + 6, sizeof(msg) - 10); put16(msg + sizeof(msg) - 2, ji_crc(msg, sizeof(msg) - 2));
    packet(p, msg, 0, 246, 354); assert(ji_receive(&a, p, deliver, NULL) == 0);
    packet(p, msg, 246, 246, 108); assert(ji_receive(&a, p, deliver, NULL) == 0);
    packet(p, msg, 492, 108, 0); assert(ji_receive(&a, p, deliver, NULL) == 1);
    assert(calls == 1 && message_size == sizeof(msg));
    /* Out-of-order, corrupt, mixed-interface, inconsistent and huge fragments. */
    assert(ji_receive(&a, p, deliver, NULL) == -1);
    packet(p, msg, 0, 246, 354); p[9] ^= 1; assert(ji_receive(&a, p, deliver, NULL) == -1);
    packet(p, msg, 0, 246, 354); assert(ji_receive(&a, p, deliver, NULL) == 0);
    packet(p, msg, 246, 246, 107); assert(ji_receive(&a, p, deliver, NULL) == -1);
    packet(p, msg, 0, 246, 354); assert(ji_receive(&a, p, deliver, NULL) == 0);
    packet(p, msg, 246, 246, 108); p[1] = 2; put16(p+254, ji_crc(p,254));
    assert(ji_receive(&a, p, deliver, NULL) == -1);
    packet(p, msg, 0, 246, 2048); assert(ji_receive(&a, p, deliver, NULL) == -1);
    msg[0] ^= 1; packet(p, msg, 0, 200, 0); assert(ji_receive(&a, p, deliver, NULL) == -1);
    packet(p, (const uint8_t *)"\xa0\x80\0\0", 0, 4, 0);
    assert(ji_receive(&a, p, deliver, NULL) == 2);
    p[0] = 0; put16(p + 254, ji_crc(p, 254));
    assert(ji_receive(&a, p, deliver, NULL) == 2);
    touch[30] = 1; finger(touch + 46, 100, 100);
    assert(ji_trackpad(&mouse, touch, 76, report) == 1 && !report[2]);
    finger(touch + 46, 180, 20);
    assert(ji_trackpad(&mouse, touch, 76, report) == 1 && report[2] == 10 && report[4] == 10);
    touch[31] = 1; ji_trackpad(&mouse, touch, 76, report); assert(report[1] == 1);
    touch[30] = 2; finger(touch + 76, 500, 500);
    ji_trackpad(&mouse, touch, 106, report); assert(!report[2] && report[1] == 1);
    touch[31] = 0; ji_trackpad(&mouse, touch, 106, report); assert(!report[1]);
    touch[31] = 1; ji_trackpad(&mouse, touch, 106, report); assert(report[1] == 2);
    touch[31] = 0; ji_trackpad(&mouse, touch, 106, report);
    finger(touch + 46, 300, 140); finger(touch + 76, 620, 620);
    ji_trackpad(&mouse, touch, 106, report); assert(report[6] == 1 && report[7] == 1);
    touch[30] = 0; ji_trackpad(&mouse, touch, 46, report); assert(!report[1] && !mouse.contacts);
    touch[30] = 17; assert(ji_trackpad(&mouse, touch, 526, report) == -1);
    touch[30] = 1; assert(ji_trackpad(&mouse, touch, 75, report) == -1);
    touch[0] = 0x60; assert(ji_trackpad(&mouse, touch, 1, report) == 2);
    /* Deterministic malformed-packet stress under ASan/UBSan. */
    unsigned rng = 123;
    for (unsigned i = 0; i < 100000; ++i) {
        for (unsigned j = 0; j < 256; ++j) { rng = rng * 1664525 + 1013904223; p[j] = rng >> 24; }
        if (i & 1) put16(p + 254, ji_crc(p, 254));
        ji_receive(&a, p, deliver, NULL);
        ji_trackpad(&mouse, p, i % 256, report);
    }
    puts("J316 protocol: CRC, discovery, fragments, pointer, clicks, scroll, reset and 100000 malformed packets PASS");
    return 0;
}
