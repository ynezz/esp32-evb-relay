#pragma once

#include <stdbool.h>

#include "device_config.h"

void network_test_stubs_reset(void);
void network_test_stubs_set_network_policy(device_config_network_policy_t policy);
void network_test_stubs_set_eth_mac_new_failure(bool fail);
