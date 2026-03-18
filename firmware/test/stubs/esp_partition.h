#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct esp_partition_t {
    uint32_t address;
    size_t size;
    uint8_t type;
    uint8_t subtype;
    bool encrypted;
    char label[17];
} esp_partition_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;
