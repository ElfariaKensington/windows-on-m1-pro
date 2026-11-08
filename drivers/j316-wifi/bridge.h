/* SPDX-License-Identifier: MIT; shared guest/EL2 register ABI. */
#ifndef J316_WIFI_BRIDGE_H
#define J316_WIFI_BRIDGE_H
#define JW_BASE 0x410000000ULL
#define JW_SIZE 0x80000u
#define JW_MAGIC 0x4649574au
#define JW_VERSION 1u
#define JW_REG_MAGIC 0x00u
#define JW_REG_VERSION 0x04u
#define JW_REG_STATUS 0x08u
#define JW_REG_ERROR 0x0cu
#define JW_REG_MAC_LOW 0x10u
#define JW_REG_MAC_HIGH 0x14u
#define JW_REG_PHASE 0x18u
#define JW_REG_TX_PROD 0x20u
#define JW_REG_TX_CONS 0x24u
#define JW_REG_RX_PROD 0x28u
#define JW_REG_RX_CONS 0x2cu
#define JW_REG_COMMAND 0x30u
#define JW_REG_COMMAND_STATUS 0x34u
#define JW_STATUS_READY 1u
#define JW_STATUS_LINK 2u
#define JW_COMMAND_JOIN 1u
#define JW_CONFIG_SSID_LEN 0x100u
#define JW_CONFIG_PSK_LEN 0x104u
#define JW_CONFIG_SSID 0x108u
#define JW_CONFIG_PSK 0x128u
#define JW_TX_BASE 0x10000u
#define JW_RX_BASE 0x30000u
#define JW_RING_COUNT 64u
#define JW_SLOT_SIZE 2048u
#define JW_FRAME_MAX 1514u
/* Unsigned monotonic producer/consumer counters, occupancy <=64. Each slot
 * starts with a little-endian u32 length followed by one Ethernet frame.
 * Publish producer only after data+length; advance consumer after copying.
 * Config is copied on JOIN. Passphrases must never be printed or persisted. */
#endif
