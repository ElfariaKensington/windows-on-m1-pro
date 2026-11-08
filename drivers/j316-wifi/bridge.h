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
/* Native mailbox (FEATURES bit 0). Version 1 guests never touch these registers.
 * A guest first writes N_TX_PROD/N_RX_CONS = TX_PROD/RX_CONS, then JW_MODE_NATIVE to MODE; the host stops its own join/reconnect logic,
 * forwards every firmware event to the event ring, delivers EAPOL/data independent of
 * its link state and acks by copying the value to MODE_ACK.
 * Firmware commands: the guest fills CTL_BUF[0..CTL_LEN) (BCM dcmd payload; for
 * GET/SET_VAR "name\0data"), writes CTL_CMD, CTL_LEN, then increments CTL_SEQ.
 * The host submits it, copies the response (CTL_RESULT_LEN bytes, same buffer),
 * writes CTL_STATUS (0 or negative errno) and finally CTL_DONE = CTL_SEQ.
 * Events: slot = [u32 length][raw BCM event frame: Ethernet header, ethertype 0x886c].
 * Firmware commands carry no host addresses; the radio's DMA stays behind the host DART. */
#define JW_REG_FEATURES 0x1cu
#define JW_FEATURE_NATIVE 1u
/* Host-written native registers (0x40..0x7f). */
#define JW_REG_MODE_ACK 0x40u
#define JW_REG_CTL_DONE 0x44u
#define JW_REG_CTL_STATUS 0x48u
#define JW_REG_CTL_RESULT_LEN 0x4cu
#define JW_REG_EV_PROD 0x50u
#define JW_REG_EV_DROPS 0x54u
/* Guest-written native registers, alone in their own 128-byte line (0x80..0xff) so the
 * host's clean+invalidate of a host-written line can never overwrite a guest store.
 * In native mode TX_PROD/RX_CONS move here as N_TX_PROD/N_RX_CONS. */
#define JW_REG_MODE 0x80u
#define JW_MODE_NATIVE 0x5654414eu /* "NATV" */
#define JW_REG_CTL_SEQ 0x84u
#define JW_REG_CTL_CMD 0x88u
#define JW_REG_CTL_LEN 0x8cu
#define JW_REG_EV_CONS 0x90u
#define JW_REG_N_TX_PROD 0x94u
#define JW_REG_N_RX_CONS 0x98u
#define JW_CTL_BUF 0x1000u
#define JW_CTL_MAX 0x2000u
#define JW_EV_BASE 0x50000u
#define JW_EV_COUNT 32u
#define JW_EV_SLOT 4096u
/* Unsigned monotonic producer/consumer counters, occupancy <=64. Each slot
 * starts with a little-endian u32 length followed by one Ethernet frame.
 * Publish producer only after data+length; advance consumer after copying.
 * Config is copied on JOIN. Passphrases must never be printed or persisted. */
#endif
