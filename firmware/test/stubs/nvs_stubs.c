#include "nvs_stubs.h"

#include <stdbool.h>
#include <string.h>

#define NVS_STUB_MAX_NAMESPACES 8
#define NVS_STUB_MAX_ENTRIES 32
#define NVS_STUB_MAX_NAMESPACE_LEN 31
#define NVS_STUB_MAX_KEY_LEN 31
#define NVS_STUB_MAX_STRING_LEN 255

typedef enum {
    NVS_STUB_VALUE_NONE = 0,
    NVS_STUB_VALUE_STR,
    NVS_STUB_VALUE_U8,
    NVS_STUB_VALUE_U32,
} nvs_stub_value_type_t;

typedef struct {
    bool in_use;
    char name[NVS_STUB_MAX_NAMESPACE_LEN + 1];
} nvs_stub_namespace_t;

typedef struct {
    bool in_use;
    uint8_t namespace_index;
    char key[NVS_STUB_MAX_KEY_LEN + 1];
    nvs_stub_value_type_t value_type;
    union {
        char str_value[NVS_STUB_MAX_STRING_LEN + 1];
        uint8_t u8_value;
        uint32_t u32_value;
    } value;
} nvs_stub_entry_t;

static nvs_stub_namespace_t s_namespaces[NVS_STUB_MAX_NAMESPACES];
static nvs_stub_entry_t s_entries[NVS_STUB_MAX_ENTRIES];

static bool nvs_stub_handle_is_valid(nvs_handle_t handle)
{
    return (handle > 0U) && (handle <= NVS_STUB_MAX_NAMESPACES) &&
           s_namespaces[handle - 1U].in_use;
}

static int nvs_stub_find_namespace(const char *namespace_name)
{
    for (size_t i = 0; i < NVS_STUB_MAX_NAMESPACES; ++i) {
        if (s_namespaces[i].in_use && (strcmp(s_namespaces[i].name, namespace_name) == 0)) {
            return (int)i;
        }
    }

    return -1;
}

static int nvs_stub_get_or_create_namespace(const char *namespace_name)
{
    int existing_index = nvs_stub_find_namespace(namespace_name);

    if (existing_index >= 0) {
        return existing_index;
    }

    for (size_t i = 0; i < NVS_STUB_MAX_NAMESPACES; ++i) {
        if (!s_namespaces[i].in_use) {
            s_namespaces[i].in_use = true;
            strncpy(s_namespaces[i].name, namespace_name, sizeof(s_namespaces[i].name) - 1U);
            s_namespaces[i].name[sizeof(s_namespaces[i].name) - 1U] = '\0';
            return (int)i;
        }
    }

    return -1;
}

static nvs_stub_entry_t *nvs_stub_find_entry(uint8_t namespace_index, const char *key)
{
    for (size_t i = 0; i < NVS_STUB_MAX_ENTRIES; ++i) {
        if (!s_entries[i].in_use) {
            continue;
        }
        if ((s_entries[i].namespace_index == namespace_index) &&
            (strcmp(s_entries[i].key, key) == 0)) {
            return &s_entries[i];
        }
    }

    return NULL;
}

static nvs_stub_entry_t *nvs_stub_get_or_create_entry(uint8_t namespace_index, const char *key)
{
    nvs_stub_entry_t *existing = nvs_stub_find_entry(namespace_index, key);

    if (existing != NULL) {
        return existing;
    }

    for (size_t i = 0; i < NVS_STUB_MAX_ENTRIES; ++i) {
        if (!s_entries[i].in_use) {
            s_entries[i].in_use = true;
            s_entries[i].namespace_index = namespace_index;
            strncpy(s_entries[i].key, key, sizeof(s_entries[i].key) - 1U);
            s_entries[i].key[sizeof(s_entries[i].key) - 1U] = '\0';
            return &s_entries[i];
        }
    }

    return NULL;
}

void nvs_stub_reset(void)
{
    memset(s_namespaces, 0, sizeof(s_namespaces));
    memset(s_entries, 0, sizeof(s_entries));
}

esp_err_t nvs_open(const char *namespace_name, int open_mode, nvs_handle_t *out_handle)
{
    int namespace_index;

    (void)open_mode;

    if ((namespace_name == NULL) || (out_handle == NULL) || (namespace_name[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    namespace_index = nvs_stub_get_or_create_namespace(namespace_name);
    if (namespace_index < 0) {
        return ESP_ERR_NO_MEM;
    }

    *out_handle = (nvs_handle_t)(namespace_index + 1);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return nvs_stub_handle_is_valid(handle) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    nvs_stub_entry_t *entry;

    if (!nvs_stub_handle_is_valid(handle) || (key == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    entry = nvs_stub_find_entry((uint8_t)(handle - 1U), key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    memset(entry, 0, sizeof(*entry));
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length)
{
    const nvs_stub_entry_t *entry;
    size_t required_size;

    if (!nvs_stub_handle_is_valid(handle) || (key == NULL) || (length == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    entry = nvs_stub_find_entry((uint8_t)(handle - 1U), key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->value_type != NVS_STUB_VALUE_STR) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    required_size = strnlen(entry->value.str_value, sizeof(entry->value.str_value)) + 1U;
    if (out_value == NULL) {
        *length = required_size;
        return ESP_OK;
    }
    if (*length < required_size) {
        *length = required_size;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    memcpy(out_value, entry->value.str_value, required_size);
    *length = required_size;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    nvs_stub_entry_t *entry;
    size_t value_len;

    if (!nvs_stub_handle_is_valid(handle) || (key == NULL) || (value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    value_len = strnlen(value, sizeof(entry->value.str_value));
    if (value_len >= sizeof(entry->value.str_value)) {
        return ESP_ERR_INVALID_SIZE;
    }

    entry = nvs_stub_get_or_create_entry((uint8_t)(handle - 1U), key);
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }

    entry->value_type = NVS_STUB_VALUE_STR;
    memcpy(entry->value.str_value, value, value_len);
    entry->value.str_value[value_len] = '\0';
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    const nvs_stub_entry_t *entry;

    if (!nvs_stub_handle_is_valid(handle) || (key == NULL) || (out_value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    entry = nvs_stub_find_entry((uint8_t)(handle - 1U), key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->value_type != NVS_STUB_VALUE_U8) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    *out_value = entry->value.u8_value;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    nvs_stub_entry_t *entry;

    if (!nvs_stub_handle_is_valid(handle) || (key == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    entry = nvs_stub_get_or_create_entry((uint8_t)(handle - 1U), key);
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }

    entry->value_type = NVS_STUB_VALUE_U8;
    entry->value.u8_value = value;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    const nvs_stub_entry_t *entry;

    if (!nvs_stub_handle_is_valid(handle) || (key == NULL) || (out_value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    entry = nvs_stub_find_entry((uint8_t)(handle - 1U), key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->value_type != NVS_STUB_VALUE_U32) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    *out_value = entry->value.u32_value;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    nvs_stub_entry_t *entry;

    if (!nvs_stub_handle_is_valid(handle) || (key == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    entry = nvs_stub_get_or_create_entry((uint8_t)(handle - 1U), key);
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }

    entry->value_type = NVS_STUB_VALUE_U32;
    entry->value.u32_value = value;
    return ESP_OK;
}

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    nvs_stub_reset();
    return ESP_OK;
}
