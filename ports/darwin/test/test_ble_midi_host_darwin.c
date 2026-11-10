#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../../include/ble_midi_host.h"
#include "../ext/Sources/CBLEMIDIHost/include/ble_midi_host_darwin.h"
#include "midi.h"
#include "midi_darwin.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static volatile int scans_on, scans_off, connects, cancels, writes, remembered, forgotten, powers_on, powers_off;
static volatile bool accept_write = true;
static uint8_t written[64];
static volatile int written_len;

void bmh_swift_load(void) {}
void bmh_swift_power(bool on) { if (on) powers_on++; else powers_off++; }
void bmh_swift_scan(bool on) { if (on) scans_on++; else scans_off++; }
bool bmh_swift_connect(int32_t slot, const uint8_t addr[6]) { (void)slot; (void)addr; connects++; return true; }
void bmh_swift_cancel(int32_t slot) { (void)slot; cancels++; }
bool bmh_swift_write(int32_t slot, const uint8_t *data, int32_t length)
{
    (void)slot;
    if (!accept_write) return false;
    memcpy(written, data, (size_t)length);
    written_len = length;
    writes++;
    return true;
}
void bmh_swift_remember(const uint8_t addr[6], const char *name) { (void)addr; (void)name; remembered++; }
void bmh_swift_forget(const uint8_t addr[6]) { (void)addr; forgotten++; }

static void on_queue(void (^block)(void))
{
    dispatch_sync(BLE_MIDI_HOST_darwin_queue(), block);
}

static void drain(void)
{
    on_queue(^{});
}

static const uint8_t phone[6] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };

int main(void)
{
    CHECK(BLE_MIDI_HOST_init() == 0, "init");
    CHECK(BLE_MIDI_HOST_enable(true) == 0, "enable");
    drain();
    CHECK(powers_on == 1 && !BLE_MIDI_HOST_is_ready(), "power asked, not ready yet");

    CHECK(BLE_MIDI_HOST_scan_start(300) == 0, "scan");
    CHECK(BLE_MIDI_HOST_scanning(), "scanning before power");
    drain();
    CHECK(scans_on == 0, "the scan waits for power");
    on_queue(^{ BLE_MIDI_HOST_darwin_powered(true); });
    CHECK(BLE_MIDI_HOST_is_ready() && scans_on == 1, "scan starts once powered");
    on_queue(^{ BLE_MIDI_HOST_darwin_discovered(phone, "SendaiRK03", -50); });
    ble_midi_peer_t peers[4];
    int n = BLE_MIDI_HOST_scan_results(peers, 4);
    CHECK(n == 1 && strcmp(peers[0].name, "SendaiRK03") == 0 && !peers[0].known, "scan result");
    usleep(400000);
    drain();
    CHECK(!BLE_MIDI_HOST_scanning() && scans_off == 1, "scan ends after its time");

    int slot = BLE_MIDI_HOST_connect(phone, 0);
    CHECK(slot == 0, "slot 0");
    CHECK(BLE_MIDI_HOST_dev_bit(0) != 0, "slot has a transport bit");
    drain();
    CHECK(connects == 1 && BLE_MIDI_HOST_dev_status(0) == BLE_MIDI_HOST_SLOT_CONNECTING, "connecting");
    on_queue(^{ BLE_MIDI_HOST_darwin_linked(0); });
    CHECK(BLE_MIDI_HOST_dev_status(0) == BLE_MIDI_HOST_SLOT_DISCOVERING, "discovering");
    on_queue(^{ BLE_MIDI_HOST_darwin_subscribing(0); });
    CHECK(BLE_MIDI_HOST_dev_status(0) == BLE_MIDI_HOST_SLOT_SUBSCRIBING, "subscribing");
    on_queue(^{ BLE_MIDI_HOST_darwin_subscribed(0, 182); });
    CHECK(BLE_MIDI_HOST_dev_connected(0) && remembered == 1, "connected and remembered");
    n = BLE_MIDI_HOST_known(peers, 4);
    CHECK(n == 1 && peers[0].connected && memcmp(peers[0].addr, phone, 6) == 0, "known and connected");
    CHECK(BLE_MIDI_HOST_find("sendai") == 0, "found by part of its name");

    BLE_MIDI_HOST_set_rx_latency(0);
    static const uint8_t note[] = { 0x80, 0x80, 0x90, 62, 80, 0x80, 0x80, 62, 0 };
    on_queue(^{ BLE_MIDI_HOST_darwin_notified(0, note, (int32_t)sizeof(note)); });
    uint8_t buf[16];
    n = BLE_MIDI_HOST_dev_read_packet(0, buf, sizeof(buf));
    CHECK(n == 8 && buf[0] == 0x09 && buf[1] == 0x90 && buf[2] == 62 && buf[3] == 80
          && buf[4] == 0x08 && buf[5] == 0x80 && buf[6] == 62, "note on and off from the peripheral (%d)", n);

    BLE_MIDI_HOST_set_rx_latency(20000);
    static const uint8_t first[] = { 0x80, 0x80, 0xF8 };
    static const uint8_t later[] = { 0x80, 0x80 | 10, 0xF8, 0x80 | 30, 0xF8 };
    on_queue(^{ BLE_MIDI_HOST_darwin_notified(0, first, (int32_t)sizeof(first)); });
    usleep(40000);
    on_queue(^{ BLE_MIDI_HOST_darwin_notified(0, later, (int32_t)sizeof(later)); });
    usleep(30000);
    const midi_transport_t *t = MIDI_transport_get(BLE_MIDI_HOST_dev_bit(0));
    int64_t sent[4];
    n = t->ops->read_sent(t->ctx, buf, sent, sizeof(buf));
    CHECK(n == 12 && buf[1] == 0xF8 && buf[5] == 0xF8 && buf[9] == 0xF8
          && sent[1] - sent[0] == 10000 && sent[2] - sent[1] == 20000,
          "clocks of one connection event keep the times they were sent (%d, %lld, %lld)",
          n, (long long)(sent[1] - sent[0]), (long long)(sent[2] - sent[1]));
    BLE_MIDI_HOST_set_rx_latency(0);

    CHECK(BLE_MIDI_HOST_dev_send_packet(0, 0, 0xB, 0xB0, 7, 100) == 0, "control change queued");
    usleep(20000);
    CHECK(writes == 1 && written_len == 5 && written[2] == 0xB0 && written[3] == 7 && written[4] == 100,
          "written as one BLE-MIDI packet (%d)", written_len);
    accept_write = false;
    CHECK(BLE_MIDI_HOST_dev_send_packet(0, 0, 0xB, 0xB0, 7, 90) == 0, "second queued");
    usleep(20000);
    accept_write = true;
    on_queue(^{ BLE_MIDI_HOST_darwin_ready(0); });
    CHECK(writes == 2 && written[4] == 90, "written again when ready");

    on_queue(^{ BLE_MIDI_HOST_darwin_failed(0, 0); });
    CHECK(BLE_MIDI_HOST_dev_status(0) == BLE_MIDI_HOST_SLOT_FREE, "free after the link goes");
    on_queue(^{ BLE_MIDI_HOST_darwin_discovered(phone, "SendaiRK03", -50); });
    drain();
    CHECK(connects == 2 && BLE_MIDI_HOST_dev_status(0) == BLE_MIDI_HOST_SLOT_CONNECTING, "known peripheral connected again");

    BLE_MIDI_HOST_disconnect(0);
    drain();
    CHECK(BLE_MIDI_HOST_dev_status(0) == BLE_MIDI_HOST_SLOT_FREE && cancels >= 1, "disconnected by the user");
    on_queue(^{ BLE_MIDI_HOST_darwin_discovered(phone, "SendaiRK03", -50); });
    drain();
    CHECK(connects == 2, "a parked peripheral is not connected again");

    CHECK(BLE_MIDI_HOST_forget(phone) == 0 && BLE_MIDI_HOST_known(peers, 4) == 0, "forgotten");
    drain();
    CHECK(forgotten == 1, "forgotten in Swift");

    uint8_t parsed[6];
    char text[18];
    BLE_MIDI_HOST_format_addr(phone, text);
    CHECK(strcmp(text, "A1:B2:C3:D4:E5:F6") == 0 && BLE_MIDI_HOST_parse_addr(text, parsed) == 0
          && memcmp(parsed, phone, 6) == 0, "address text");

    static const uint8_t keys[6] = { 1, 1, 1, 1, 1, 1 };
    static const uint8_t pads[6] = { 2, 2, 2, 2, 2, 2 };
    on_queue(^{ BLE_MIDI_HOST_darwin_powered(false); });
    int before = connects;
    CHECK(BLE_MIDI_HOST_connect(keys, 0) == 1 && BLE_MIDI_HOST_connect(pads, 0) == 0, "both wait in their slots");
    CHECK(connects == before, "nothing connects without power");
    on_queue(^{ BLE_MIDI_HOST_darwin_powered(true); });
    CHECK(connects == before + 1, "one connect at a time once powered");
    on_queue(^{ BLE_MIDI_HOST_darwin_linked(0); });
    CHECK(connects == before + 2 && BLE_MIDI_HOST_dev_status(1) == BLE_MIDI_HOST_SLOT_CONNECTING,
          "the waiting slot connects after the first links");
    on_queue(^{ BLE_MIDI_HOST_darwin_linked(1); });
    on_queue(^{ BLE_MIDI_HOST_darwin_subscribing(1); });
    on_queue(^{ BLE_MIDI_HOST_darwin_subscribed(1, 20); });
    on_queue(^{ BLE_MIDI_HOST_darwin_failed(1, 0); });
    CHECK(BLE_MIDI_HOST_scan_start(200) == 0, "user scan");
    drain();
    int during = connects;
    on_queue(^{ BLE_MIDI_HOST_darwin_discovered(keys, "Keys", -40); });
    CHECK(connects == during && BLE_MIDI_HOST_dev_status(1) == BLE_MIDI_HOST_SLOT_FREE,
          "a user scan only records what it finds");
    CHECK(BLE_MIDI_HOST_scan_results(peers, 4) == 1 && peers[0].known, "found and known");
    on_queue(^{ BLE_MIDI_HOST_darwin_powered(false); });
    CHECK(BLE_MIDI_HOST_scan_start(100) == 0, "scan without power");
    usleep(200000);
    drain();
    CHECK(!BLE_MIDI_HOST_scanning(), "a scan without power still ends after its time");
    on_queue(^{ BLE_MIDI_HOST_darwin_powered(true); });

    CHECK(BLE_MIDI_HOST_enable(false) == 0 && !BLE_MIDI_HOST_is_ready(), "not ready at once when disabled");
    drain();
    CHECK(powers_off == 1, "power off on the queue");

    if (failures) {
        printf("test_ble_midi_host_darwin: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ble_midi_host_darwin: ok\n");
    return 0;
}
