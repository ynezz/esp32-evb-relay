#pragma once

#include <stdint.h>

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ipaddr)                                                                     \
    (unsigned)(((ipaddr)->addr >> 0) & 0xFFU),                                             \
    (unsigned)(((ipaddr)->addr >> 8) & 0xFFU),                                             \
    (unsigned)(((ipaddr)->addr >> 16) & 0xFFU),                                            \
    (unsigned)(((ipaddr)->addr >> 24) & 0xFFU)
