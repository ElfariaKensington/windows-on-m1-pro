/* SPDX-License-Identifier: MIT */
#ifndef J316_SPI_PROTOCOL_H
#define J316_SPI_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

#define JI_PACKET 256u
#define JI_MESSAGE 2048u
#define JI_DESCRIPTOR 512u

typedef void (*ji_message_fn)(void *, uint8_t, uint8_t, const uint8_t *, size_t);
struct ji_assembly {
    uint8_t data[JI_MESSAGE];
    size_t used, total;
    uint8_t device, flags;
};
struct ji_mouse {
    int x, y, contacts, rem_x, rem_y, scroll_x, scroll_y;
    uint8_t buttons;
};
struct ji_control_request {
    uint8_t command, report, interface_id, sequence;
};
/* Reply matching follows validated message semantics; counters are per type. */
int ji_is_control_reply(const struct ji_control_request *, uint8_t flags,
                         const uint8_t *message, size_t size);
typedef int32_t (*ji_discovery_query_fn)(void *, uint8_t report,
                                        uint8_t interface_id, uint16_t limit);
int32_t ji_discover(ji_discovery_query_fn, void *);

uint16_t ji_crc(const uint8_t *data, size_t size);
size_t ji_request(uint8_t packet[JI_PACKET], uint8_t target, uint8_t command,
                  uint8_t report, uint8_t interface_id, uint8_t sequence,
                  uint16_t response_size, const uint8_t *data, size_t size);
/* 1 = delivered message, 2 = boot notification, 0 = fragment, -1 = invalid. */
int ji_receive(struct ji_assembly *, const uint8_t packet[JI_PACKET],
               ji_message_fn, void *);
/* 1 = mouse report, 2 = controller reset, -1 = malformed/unknown. */
int ji_trackpad(struct ji_mouse *, const uint8_t *, size_t, uint8_t report[8]);
extern const uint8_t ji_mouse_descriptor[];
extern const size_t ji_mouse_descriptor_size;
#endif
