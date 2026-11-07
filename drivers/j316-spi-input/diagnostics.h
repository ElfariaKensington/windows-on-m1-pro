/* SPDX-License-Identifier: MIT */
#ifndef J316_SPI_DIAGNOSTICS_H
#define J316_SPI_DIAGNOSTICS_H
#include <stdint.h>
#define JI_DIAGNOSTIC_FIELDS(X) \
    X(version) X(phase) X(status) X(booted) X(vendor) X(product) \
    X(packets) X(malformed) X(timeouts) X(keyboard) X(mouse) X(submit_errors) X(resets) \
    X(spi_control) X(spi_fifo) X(gpio_input) X(descriptor_size) \
    X(spi_shift) X(spi_pinconfig) X(gpio_enable) X(spi_xfer) X(spi_fifo_flags) \
    X(rx_count) X(last_flags) X(last_device) X(last_length) X(last_crc) X(last_ack) X(bad_acks)
typedef struct {
#define JI_FIELD(name) uint32_t name;
    JI_DIAGNOSTIC_FIELDS(JI_FIELD)
#undef JI_FIELD
} JI_DIAG;
#endif
