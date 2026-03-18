#include <string.h>

#include "esp_idf_stubs.h"
#include "esp_ota_stubs.h"
#include "freertos_stubs.h"
#include "ota.h"
#include "unity.h"

static const esp_partition_t TEST_RUNNING_PARTITION = {
    .address = 0x20000U,
    .size = 0x1E0000U,
    .type = 0U,
    .subtype = 0U,
    .encrypted = false,
    .label = "ota_0",
};

static const esp_partition_t TEST_UPDATE_PARTITION = {
    .address = 0x200000U,
    .size = 0x1E0000U,
    .type = 0U,
    .subtype = 1U,
    .encrypted = false,
    .label = "ota_1",
};

static void test_ota_begin_update_rejects_oversized_images(void)
{
    ota_target_info_t target = {0};

    esp_ota_stub_set_next_update_partition(&TEST_UPDATE_PARTITION);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      ota_begin_update(TEST_UPDATE_PARTITION.size + 1U, &target));
    TEST_ASSERT_EQUAL_UINT32(0U, esp_ota_stub_get_begin_count());
}

static void test_ota_begin_write_finalize_and_schedule_reboot(void)
{
    ota_target_info_t target = {0};
    static const uint8_t first_chunk[] = {0x01U, 0x02U, 0x03U};
    static const uint8_t second_chunk[] = {0x04U, 0x05U};

    esp_ota_stub_set_next_update_partition(&TEST_UPDATE_PARTITION);

    TEST_ASSERT_EQUAL(ESP_OK, ota_begin_update(5U, &target));
    TEST_ASSERT_EQUAL_STRING("ota_1", target.partition_label);
    TEST_ASSERT_EQUAL_UINT(TEST_UPDATE_PARTITION.size, target.partition_size);
    TEST_ASSERT_EQUAL_PTR(&TEST_UPDATE_PARTITION, esp_ota_stub_get_last_begin_partition());
    TEST_ASSERT_EQUAL_UINT(5U, esp_ota_stub_get_last_begin_size());

    TEST_ASSERT_EQUAL(ESP_OK, ota_write_chunk(first_chunk, sizeof(first_chunk)));
    TEST_ASSERT_EQUAL(ESP_OK, ota_write_chunk(second_chunk, sizeof(second_chunk)));
    TEST_ASSERT_EQUAL_UINT(5U, esp_ota_stub_get_total_bytes_written());

    TEST_ASSERT_EQUAL(ESP_OK, ota_finalize_update());
    TEST_ASSERT_EQUAL_PTR(&TEST_UPDATE_PARTITION, esp_ota_stub_get_last_boot_partition());

    TEST_ASSERT_EQUAL(ESP_OK, ota_schedule_reboot());
    TEST_ASSERT_EQUAL_UINT32(1U, freertos_stub_get_task_create_count());
    TEST_ASSERT_EQUAL_STRING("ota_reboot", freertos_stub_get_last_task_name());
}

static void test_ota_abort_cleans_up_partial_update_state(void)
{
    esp_ota_stub_set_next_update_partition(&TEST_UPDATE_PARTITION);

    TEST_ASSERT_EQUAL(ESP_OK, ota_begin_update(4U, NULL));
    ota_abort_update();

    TEST_ASSERT_EQUAL_UINT32(1U, esp_ota_stub_get_abort_count());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_write_chunk("x", 1U));
}

static void test_ota_schedule_reboot_restarts_immediately_when_task_creation_fails(void)
{
    esp_ota_stub_set_next_update_partition(&TEST_UPDATE_PARTITION);
    freertos_stub_set_task_create_result(pdFALSE);

    TEST_ASSERT_EQUAL(ESP_OK, ota_begin_update(1U, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ota_write_chunk("x", 1U));
    TEST_ASSERT_EQUAL(ESP_OK, ota_finalize_update());
    TEST_ASSERT_EQUAL(ESP_OK, ota_schedule_reboot());
    TEST_ASSERT_EQUAL_UINT32(1U, esp_stub_get_restart_count());
}

static void test_ota_confirm_pending_image_marks_it_valid(void)
{
    esp_ota_stub_set_running_partition(&TEST_RUNNING_PARTITION);
    esp_ota_stub_set_state_result(ESP_OK);
    esp_ota_stub_set_running_partition_state(ESP_OTA_IMG_PENDING_VERIFY);

    TEST_ASSERT_EQUAL(ESP_OK, ota_confirm_running_image_if_pending());
    TEST_ASSERT_EQUAL_UINT32(1U, esp_ota_stub_get_mark_valid_count());
}

static void test_ota_confirm_ignores_non_ota_running_partition(void)
{
    esp_ota_stub_set_running_partition(&TEST_RUNNING_PARTITION);
    esp_ota_stub_set_state_result(ESP_ERR_NOT_SUPPORTED);

    TEST_ASSERT_EQUAL(ESP_OK, ota_confirm_running_image_if_pending());
    TEST_ASSERT_EQUAL_UINT32(0U, esp_ota_stub_get_mark_valid_count());
}

void test_ota_suite(void)
{
    RUN_TEST(test_ota_begin_update_rejects_oversized_images);
    RUN_TEST(test_ota_begin_write_finalize_and_schedule_reboot);
    RUN_TEST(test_ota_abort_cleans_up_partial_update_state);
    RUN_TEST(test_ota_schedule_reboot_restarts_immediately_when_task_creation_fails);
    RUN_TEST(test_ota_confirm_pending_image_marks_it_valid);
    RUN_TEST(test_ota_confirm_ignores_non_ota_running_partition);
}
