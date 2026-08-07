#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2

struct ble_store_value_sec {
    uint8_t peer_addr[6];
    uint8_t payload[16];
};

union ble_store_value {
    struct ble_store_value_sec sec;
};

typedef int (*ble_store_iterator_fn)(int obj_type, union ble_store_value* value, void* cookie);

int ble_store_iterate(int obj_type, ble_store_iterator_fn callback, void* cookie);
int ble_store_write_our_sec(const struct ble_store_value_sec* value);
int ble_store_write_peer_sec(const struct ble_store_value_sec* value);

#ifdef __cplusplus
}
#endif
