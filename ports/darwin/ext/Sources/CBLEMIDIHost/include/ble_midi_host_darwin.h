#ifndef BLE_MIDI_HOST_DARWIN_DEFINED_H_
#define BLE_MIDI_HOST_DARWIN_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include <dispatch/dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif

dispatch_queue_t BLE_MIDI_HOST_darwin_queue(void);
void BLE_MIDI_HOST_darwin_known(const uint8_t addr[6], const char *name);
void BLE_MIDI_HOST_darwin_powered(bool on);
void BLE_MIDI_HOST_darwin_discovered(const uint8_t addr[6], const char *name, int32_t rssi);
void BLE_MIDI_HOST_darwin_linked(int32_t slot);
void BLE_MIDI_HOST_darwin_subscribing(int32_t slot);
void BLE_MIDI_HOST_darwin_subscribed(int32_t slot, uint16_t max_write);
void BLE_MIDI_HOST_darwin_failed(int32_t slot, int32_t error);
void BLE_MIDI_HOST_darwin_notified(int32_t slot, const uint8_t *data, int32_t length);
void BLE_MIDI_HOST_darwin_ready(int32_t slot);

#ifdef __cplusplus
}
#endif

#endif
