/* SPDX-License-Identifier: MIT */
#include "protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void put16(uint8_t *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
struct capture { struct ji_control_request request; unsigned replies, inputs; };
static void receive(void *context, uint8_t device, uint8_t flags, const uint8_t *m, size_t n)
{
    struct capture *c = context;
    if (ji_is_control_reply(&c->request, flags, m, n)) ++c->replies;
    else if (device == 1 && flags == 0x20 && m[0] == 0x10) ++c->inputs;
}
static void reply(uint8_t *p, unsigned report, unsigned iface, unsigned counter)
{
    uint8_t metadata[16] = {0};
    assert(ji_request(p, 0xd0, 0x20, report, iface, counter, 512, metadata, 16));
}
static void test_replies(void)
{
    struct ji_assembly assembly = {0};
    struct capture c = {{0x20, 2, 0, 77}, 0, 0};
    uint8_t p[256];
    /* A real request uses counter 77; the reply's per-type counter is zero. */
    reply(p, 2, 0, 77); assert(p[11] == 77);
    reply(p, 2, 0, 0);
    assert(ji_receive(&assembly, p, receive, &c) == 1);
    assert(c.replies == 1);
    /* Interleaved input must not complete discovery. */
    reply(p, 2, 0, 0); p[0] = 0x20; p[1] = 1; p[8] = 0x10;
    put16(p + 32, ji_crc(p + 8, 24)); put16(p + 254, ji_crc(p, 254));
    assert(ji_receive(&assembly, p, receive, &c) == 1);
    assert(c.inputs == 1 && c.replies == 1);
    reply(p, 2, 1, 0); assert(ji_receive(&assembly, p, receive, &c) == 1);
    reply(p, 0x10, 0, 0); assert(ji_receive(&assembly, p, receive, &c) == 1);
    assert(c.replies == 1);
    reply(p, 2, 0, 0); p[20] ^= 1;
    assert(ji_receive(&assembly, p, receive, &c) == -1 && c.replies == 1);
    reply(p, 2, 0, 255); assert(ji_receive(&assembly, p, receive, &c) == 1);
    assert(c.replies == 2);
}
struct discovery {
    unsigned calls, descriptors;
    int missing_metadata, missing_keyboard, missing_identity;
};
static int32_t query(void *context, uint8_t report, uint8_t iface, uint16_t limit)
{
    struct discovery *d = context;
    ++d->calls;
    if (report == 1) {
        assert(iface == 0xd0 && limit == 0);
        return d->missing_identity ? -1 : 0;
    }
    assert(limit == 512);
    if (report == 2) return d->missing_metadata ? -1 : 0;
    assert(report == 0x10 && (iface == 1 || iface == 2));
    ++d->descriptors;
    return d->missing_keyboard && iface == 1 ? -1 : 0;
}
static void test_optional_metadata(void)
{
    struct discovery d = {0, 0, 1, 0, 0};
    assert(ji_discover(query, &d) == 0);
    assert(d.calls == 6 && d.descriptors == 2);
    d = (struct discovery){0, 0, 1, 1, 0};
    assert(ji_discover(query, &d) < 0 && d.calls == 5 && d.descriptors == 1);
    d = (struct discovery){0, 0, 0, 0, 1};
    assert(ji_discover(query, &d) < 0 && d.calls == 1 && d.descriptors == 0);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "replies")) test_replies();
    else { assert(!strcmp(argv[1], "metadata")); test_optional_metadata(); }
    puts("discovery regression PASS");
    return 0;
}
