#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <dispatch/dispatch.h>

#include "../../include/ble_midi_host.h"
#include "ext/Sources/CBLEMIDIHost/include/ble_midi_host_darwin.h"

#include "ble_midi_codec.h"
#include "midi_timed_ring.h"
#include "midi.h"
#include "midi_darwin.h"

void bmh_swift_load(void);
void bmh_swift_power(bool on);
void bmh_swift_scan(bool on);
bool bmh_swift_connect(int32_t slot, const uint8_t addr[6]);
void bmh_swift_cancel(int32_t slot);
bool bmh_swift_write(int32_t slot, const uint8_t *data, int32_t length);
void bmh_swift_remember(const uint8_t addr[6], const char *name);
void bmh_swift_forget(const uint8_t addr[6]);

#define TX_MASK (BLE_MIDI_HOST_TX_PACKETS - 1)
#define TX_BUF_MAX 512
#define SERVICE_PERIOD_MS 500
#define CONNECT_TIMEOUT_MS 10000

typedef struct {
    uint8_t  p[4];
    uint16_t ts;
} tx_entry_t;

typedef struct {
    volatile ble_midi_host_slot_status_t status;
    uint8_t    addr[6];
    bool       used;
    bool       parked;
    char       name[32];
    volatile uint16_t mtu;
    uint32_t   connect_attempt;

    midi_transport_t transport;
    midi_tmask_t bit;

    midi_timed_ring_t *rx;
    ble_midi_decoder_t dec;
    ble_midi_sync_t sync;
    int64_t rx_now;
    int32_t rx_budget;

    tx_entry_t *tx;
    volatile uint32_t tx_head, tx_tail;
    pthread_mutex_t tx_mux;
    uint8_t *txbuf;
    size_t   tx_pending;
    uint8_t  tx_done;
    ble_midi_encoder_t enc;
    bool     enc_open;

    ble_midi_host_stats_t stats;
} slot_t;

typedef struct {
    uint8_t addr[6];
    char    name[25];
} known_t;

static bool s_inited;
static slot_t *s_slots;
static dispatch_queue_t s_q;
static dispatch_source_t s_service_timer;

static volatile bool s_wanted;
static volatile bool s_held;
static volatile bool s_powered;
static volatile bool s_unavailable;
static volatile bool s_auto = true;
static volatile int32_t s_rx_latency = BLE_MIDI_HOST_RX_LATENCY_US;
static volatile bool s_flush_scheduled;

static volatile bool s_scanning;
static volatile bool s_scan_is_user;
static volatile bool s_scan_pending;
static uint32_t s_scan_generation;
static ble_midi_peer_t *s_results;
static volatile int s_result_count;
static pthread_mutex_t s_results_mux = PTHREAD_MUTEX_INITIALIZER;
static int64_t s_next_reconnect_us;

static volatile int s_connecting = -1;

static known_t s_known[BLE_MIDI_HOST_MAX_KNOWN];
static int s_known_count;
static pthread_mutex_t s_known_mux = PTHREAD_MUTEX_INITIALIZER;

static inline uint16_t now_ts(void)
{
    return (uint16_t)((midi_darwin_now_us() / 1000) & BLE_MIDI_TS_MASK);
}

static slot_t *slot_at(int slot)
{
    if (s_slots == NULL || slot < 0 || slot >= BLE_MIDI_HOST_MAX_DEVICES) return NULL;
    return &s_slots[slot];
}

static int known_find(const uint8_t addr[6])
{
    for (int i = 0; i < s_known_count; i++) {
        if (memcmp(s_known[i].addr, addr, 6) == 0) return i;
    }
    return -1;
}

static void known_put(const uint8_t addr[6], const char *name)
{
    pthread_mutex_lock(&s_known_mux);
    int k = known_find(addr);
    if (k < 0) {
        if (s_known_count == BLE_MIDI_HOST_MAX_KNOWN) {
            memmove(&s_known[0], &s_known[1], sizeof(known_t) * (BLE_MIDI_HOST_MAX_KNOWN - 1));
            s_known_count--;
        }
        k = s_known_count++;
        memcpy(s_known[k].addr, addr, 6);
    }
    strncpy(s_known[k].name, name ? name : "", sizeof(s_known[k].name) - 1);
    s_known[k].name[sizeof(s_known[k].name) - 1] = '\0';
    pthread_mutex_unlock(&s_known_mux);
}

static bool in_slot(const uint8_t addr[6])
{
    for (int j = 0; j < BLE_MIDI_HOST_MAX_DEVICES; j++) {
        if (s_slots[j].status != BLE_MIDI_HOST_SLOT_FREE && memcmp(s_slots[j].addr, addr, 6) == 0) return true;
    }
    return false;
}

static bool parked(const uint8_t addr[6])
{
    for (int j = 0; j < BLE_MIDI_HOST_MAX_DEVICES; j++) {
        if (s_slots[j].used && s_slots[j].parked && memcmp(s_slots[j].addr, addr, 6) == 0) return true;
    }
    return false;
}

static bool known_missing(void)
{
    bool missing = false;
    pthread_mutex_lock(&s_known_mux);
    for (int i = 0; i < s_known_count && !missing; i++) {
        if (!in_slot(s_known[i].addr) && !parked(s_known[i].addr)) missing = true;
    }
    pthread_mutex_unlock(&s_known_mux);
    return missing;
}

static int32_t rx_budget(slot_t *s)
{
    int32_t l = s_rx_latency;
    if (l < 0) return ble_midi_sync_auto_budget(s->stats.interval_1250us);
    return l;
}

static void rx_push(void *arg, const uint8_t pkt[4], uint16_t ts)
{
    slot_t *s = (slot_t *)arg;
    int64_t at = ble_midi_sync_release(&s->sync, ts, s->rx_now, s->rx_budget);
    int64_t sent = s->rx_budget > 0 ? ble_midi_sync_sent(&s->sync) : at;
    if (!midi_timed_ring_push_sent(s->rx, pkt, at, sent)) {
        s->stats.rx_dropped++;
        return;
    }
    s->stats.rx_messages++;
}

static void tx_clear(slot_t *s)
{
    pthread_mutex_lock(&s->tx_mux);
    s->tx_tail = s->tx_head;
    pthread_mutex_unlock(&s->tx_mux);
    s->tx_pending = 0;
    s->tx_done = 0;
    s->enc_open = false;
    s->enc.sysex_open = false;
}

static bool write_packet(slot_t *s, const uint8_t *buf, size_t len)
{
    if (s->status != BLE_MIDI_HOST_SLOT_CONNECTED) return true;
    if (bmh_swift_write((int32_t)(s - s_slots), buf, (int32_t)len)) {
        s->stats.tx_packets++;
        return true;
    }
    s->stats.tx_retries++;
    return false;
}

static void tx_flush(slot_t *s)
{
    if (s->status != BLE_MIDI_HOST_SLOT_CONNECTED) {
        tx_clear(s);
        return;
    }
    if (s->tx_pending) {
        if (!write_packet(s, s->txbuf, s->tx_pending)) return;
        s->tx_pending = 0;
        s->enc_open = false;
    }
    size_t cap = (size_t)s->mtu - 3;
    if (cap > TX_BUF_MAX) cap = TX_BUF_MAX;
    if (cap < 5) cap = 5;

    for (;;) {
        tx_entry_t ent;
        pthread_mutex_lock(&s->tx_mux);
        bool empty = (s->tx_tail == s->tx_head);
        if (!empty) ent = s->tx[s->tx_tail];
        pthread_mutex_unlock(&s->tx_mux);
        if (empty) break;

        if (!s->enc_open) {
            ble_midi_encoder_begin(&s->enc, s->txbuf, cap, ent.ts);
            s->enc_open = true;
        }
        uint8_t done = s->tx_done;
        if (ble_midi_encode(&s->enc, ent.p, ent.ts, &done)) {
            s->tx_done = 0;
            pthread_mutex_lock(&s->tx_mux);
            s->tx_tail = (s->tx_tail + 1) & TX_MASK;
            pthread_mutex_unlock(&s->tx_mux);
            continue;
        }
        s->tx_done = done;
        if (!write_packet(s, s->txbuf, s->enc.len)) {
            s->tx_pending = s->enc.len;
            return;
        }
        s->enc_open = false;
    }
    if (s->enc_open && ble_midi_encoder_pending(&s->enc)) {
        if (!write_packet(s, s->txbuf, s->enc.len)) {
            s->tx_pending = s->enc.len;
            return;
        }
    }
    s->enc_open = false;
}

static void slot_link_down(slot_t *s)
{
    s->status = BLE_MIDI_HOST_SLOT_FREE;
    s->mtu = 23;
    tx_clear(s);
    ble_midi_decoder_reset(&s->dec);
    ble_midi_sync_reset(&s->sync);
    if (s->rx) midi_timed_ring_clear(s->rx);
}

static void connect_next(void);

static void scan_finish(void)
{
    if (s_scanning && !s_scan_pending) bmh_swift_scan(false);
    s_scanning = false;
    s_scan_is_user = false;
    s_scan_pending = false;
}

static void scan_begin(bool user, uint32_t ms)
{
    uint32_t generation = ++s_scan_generation;
    s_scanning = true;
    s_scan_is_user = user;
    s_scan_pending = !s_powered;
    if (s_powered) bmh_swift_scan(true);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * 1000000), s_q, ^{
        if (generation == s_scan_generation) scan_finish();
    });
}

static void connect_done(int slot)
{
    if (s_connecting == slot) s_connecting = -1;
    connect_next();
}

static void service_tick(void)
{
    if (!s_held || !s_powered || !s_auto || s_scanning || s_connecting >= 0) return;
    if (midi_darwin_now_us() < s_next_reconnect_us || !known_missing()) return;
    scan_begin(false, BLE_MIDI_HOST_RECONNECT_SCAN_MS);
    s_next_reconnect_us = midi_darwin_now_us() + (int64_t)BLE_MIDI_HOST_RECONNECT_PERIOD_MS * 1000;
}

static void do_enable(void)
{
    if (!s_wanted || s_held) return;
    s_held = true;
    s_next_reconnect_us = 0;
    bmh_swift_power(true);
}

static void do_disable(void)
{
    if (s_wanted) return;
    scan_finish();
    s_scan_pending = false;
    for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
        if (s_slots[i].status != BLE_MIDI_HOST_SLOT_FREE) {
            bmh_swift_cancel(i);
            slot_link_down(&s_slots[i]);
        }
    }
    s_connecting = -1;
    s_held = false;
    s_powered = false;
    bmh_swift_power(false);
}

dispatch_queue_t BLE_MIDI_HOST_darwin_queue(void)
{
    return s_q;
}

void BLE_MIDI_HOST_darwin_known(const uint8_t addr[6], const char *name)
{
    known_put(addr, name);
}

void BLE_MIDI_HOST_darwin_unavailable(bool unavailable)
{
    s_unavailable = unavailable;
}

void BLE_MIDI_HOST_darwin_powered(bool on)
{
    s_powered = on && s_held;
    if (!on) {
        s_scanning = false;
        s_scan_is_user = false;
        s_scan_pending = false;
        for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
            if (s_slots[i].status != BLE_MIDI_HOST_SLOT_FREE) slot_link_down(&s_slots[i]);
        }
        s_connecting = -1;
        return;
    }
    if (s_scan_pending) {
        s_scan_pending = false;
        bmh_swift_scan(true);
    }
    connect_next();
}

static void results_add(const uint8_t addr[6], const char *name, int32_t rssi)
{
    pthread_mutex_lock(&s_results_mux);
    int idx = -1;
    for (int i = 0; i < s_result_count; i++) {
        if (memcmp(s_results[i].addr, addr, 6) == 0) { idx = i; break; }
    }
    if (idx < 0 && s_result_count < BLE_MIDI_HOST_MAX_RESULTS) {
        idx = s_result_count++;
        memset(&s_results[idx], 0, sizeof(s_results[idx]));
        memcpy(s_results[idx].addr, addr, 6);
    }
    if (idx >= 0) {
        s_results[idx].rssi = (int8_t)rssi;
        if (name && name[0]) {
            strncpy(s_results[idx].name, name, sizeof(s_results[idx].name) - 1);
            s_results[idx].name[sizeof(s_results[idx].name) - 1] = '\0';
        }
    }
    pthread_mutex_unlock(&s_results_mux);
}

static int request_connect(const uint8_t addr[6]);

void BLE_MIDI_HOST_darwin_discovered(const uint8_t addr[6], const char *name, int32_t rssi)
{
    if (s_scan_is_user) {
        results_add(addr, name, rssi);
        return;
    }
    if (!s_auto || in_slot(addr) || parked(addr)) return;
    pthread_mutex_lock(&s_known_mux);
    bool known = known_find(addr) >= 0;
    pthread_mutex_unlock(&s_known_mux);
    if (known) request_connect(addr);
}

void BLE_MIDI_HOST_darwin_linked(int32_t slot)
{
    slot_t *s = slot_at(slot);
    if (s == NULL || s->status != BLE_MIDI_HOST_SLOT_CONNECTING) return;
    s->status = BLE_MIDI_HOST_SLOT_DISCOVERING;
    s->stats.connections++;
    connect_done(slot);
}

void BLE_MIDI_HOST_darwin_subscribing(int32_t slot)
{
    slot_t *s = slot_at(slot);
    if (s && s->status == BLE_MIDI_HOST_SLOT_DISCOVERING) s->status = BLE_MIDI_HOST_SLOT_SUBSCRIBING;
}

void BLE_MIDI_HOST_darwin_subscribed(int32_t slot, uint16_t max_write)
{
    slot_t *s = slot_at(slot);
    if (s == NULL || s->status != BLE_MIDI_HOST_SLOT_SUBSCRIBING) return;
    s->mtu = (uint16_t)(max_write + 3);
    s->status = BLE_MIDI_HOST_SLOT_CONNECTED;
    known_put(s->addr, s->name);
    bmh_swift_remember(s->addr, s->name);
    if (MIDI_Input_was_started() && !MIDI_Input_is_running()) MIDI_Input_start();
}

void BLE_MIDI_HOST_darwin_failed(int32_t slot, int32_t error)
{
    slot_t *s = slot_at(slot);
    if (s == NULL || s->status == BLE_MIDI_HOST_SLOT_FREE) return;
    if (error != 0) s->stats.last_error = error;
    slot_link_down(s);
    if (!s->parked) s_next_reconnect_us = 0;
    connect_done(slot);
}

void BLE_MIDI_HOST_darwin_notified(int32_t slot, const uint8_t *data, int32_t length)
{
    slot_t *s = slot_at(slot);
    if (s == NULL || s->status != BLE_MIDI_HOST_SLOT_CONNECTED || length <= 0) return;
    s->stats.rx_packets++;
    s->rx_now = midi_darwin_now_us();
    s->rx_budget = rx_budget(s);
    if (ble_midi_decode(&s->dec, data, (size_t)length, rx_push, s) != 0) {
        s->stats.decode_errors = s->dec.errors;
    }
    midi_timed_ring_commit(s->rx);
}

void BLE_MIDI_HOST_darwin_ready(int32_t slot)
{
    slot_t *s = slot_at(slot);
    if (s) tx_flush(s);
}

static int t_send(void *ctx, uint8_t cable, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    return BLE_MIDI_HOST_dev_send_packet((int)(intptr_t)ctx, cable, cin, b1, b2, b3);
}
static int t_read(void *ctx, uint8_t *buf, size_t maxlen)
{
    return BLE_MIDI_HOST_dev_read_packet((int)(intptr_t)ctx, buf, maxlen);
}
static int t_avail(void *ctx) { return BLE_MIDI_HOST_dev_bytes_available((int)(intptr_t)ctx); }
static bool t_connected(void *ctx) { return BLE_MIDI_HOST_dev_connected((int)(intptr_t)ctx); }
static int t_read_sent(void *ctx, uint8_t *buf, int64_t *sent_us, size_t maxlen)
{
    slot_t *s = slot_at((int)(intptr_t)ctx);
    if (s == NULL || s->rx == NULL) return 0;
    return midi_timed_ring_read_sent(s->rx, buf, sent_us, maxlen);
}

static const midi_transport_ops_t s_ops = {
    .send_packet     = t_send,
    .read_bytes      = t_read,
    .bytes_available = t_avail,
    .is_connected    = t_connected,
    .transport_id    = MIDI_TRANSPORT_ID_BLE,
    .read_sent       = t_read_sent,
};

static void register_slot(int i)
{
    slot_t *s = &s_slots[i];
    if (s->bit != 0) return;
    s->transport.ops = &s_ops;
    s->transport.ctx = (void *)(intptr_t)i;
    s->bit = MIDI_transport_register(&s->transport);
    if (s->bit == 0) return;
    MIDI_Input_add_packet_source(s->bit);
    MIDI_Input_track_clock(s->bit);
}

int BLE_MIDI_HOST_init(void)
{
    if (s_inited) return 0;
    s_slots = calloc(BLE_MIDI_HOST_MAX_DEVICES, sizeof(slot_t));
    s_results = calloc(BLE_MIDI_HOST_MAX_RESULTS, sizeof(ble_midi_peer_t));
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    if (s_q == NULL) s_q = dispatch_queue_create("picoruby.ble_midi_host", attr);
    if (!s_slots || !s_results || !s_q) goto fail;
    for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
        slot_t *s = &s_slots[i];
        s->rx = midi_timed_ring_new(BLE_MIDI_HOST_RX_PACKETS, "picoruby.ble_midi_host.rx");
        s->tx = calloc(BLE_MIDI_HOST_TX_PACKETS, sizeof(tx_entry_t));
        s->txbuf = malloc(TX_BUF_MAX);
        if (!s->rx || !s->tx || !s->txbuf) goto fail;
        pthread_mutex_init(&s->tx_mux, NULL);
        s->mtu = 23;
        ble_midi_decoder_reset(&s->dec);
    }
    s_service_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, s_q);
    if (s_service_timer == NULL) goto fail;
    dispatch_source_set_event_handler(s_service_timer, ^{ service_tick(); });
    dispatch_source_set_timer(s_service_timer, dispatch_time(DISPATCH_TIME_NOW, SERVICE_PERIOD_MS * 1000000LL),
                              SERVICE_PERIOD_MS * 1000000ULL, 50 * 1000000ULL);
    dispatch_resume(s_service_timer);
    s_inited = true;
    dispatch_sync(s_q, ^{ bmh_swift_load(); });
    return 0;

fail:
    if (s_slots) {
        for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
            midi_timed_ring_free(s_slots[i].rx);
            free(s_slots[i].tx);
            free(s_slots[i].txbuf);
        }
    }
    free(s_slots); s_slots = NULL;
    free(s_results); s_results = NULL;
    return -1;
}

bool BLE_MIDI_HOST_available(void) { return !s_unavailable; }

int BLE_MIDI_HOST_enable(bool on)
{
    if (BLE_MIDI_HOST_init() != 0) return -1;
    s_wanted = on;
    if (!on) s_held = false;
    dispatch_async(s_q, ^{ if (on) do_enable(); else do_disable(); });
    return 0;
}

bool BLE_MIDI_HOST_is_enabled(void) { return s_wanted; }
bool BLE_MIDI_HOST_is_ready(void) { return s_held && s_powered; }

void BLE_MIDI_HOST_set_auto_connect(bool on)
{
    s_auto = on;
    if (on) s_next_reconnect_us = 0;
}
bool BLE_MIDI_HOST_auto_connect(void) { return s_auto; }

int BLE_MIDI_HOST_scan_start(uint32_t duration_ms)
{
    if (BLE_MIDI_HOST_init() != 0) return -1;
    if (!s_wanted) BLE_MIDI_HOST_enable(true);
    pthread_mutex_lock(&s_results_mux);
    s_result_count = 0;
    pthread_mutex_unlock(&s_results_mux);
    uint32_t ms = duration_ms ? duration_ms : 5000;
    s_scanning = true;
    s_scan_is_user = true;
    dispatch_async(s_q, ^{ scan_begin(true, ms); });
    return 0;
}

void BLE_MIDI_HOST_scan_stop(void)
{
    if (s_q == NULL) return;
    dispatch_async(s_q, ^{
        s_scan_generation++;
        scan_finish();
    });
}

bool BLE_MIDI_HOST_scanning(void) { return s_scanning && s_scan_is_user; }

int BLE_MIDI_HOST_scan_results(ble_midi_peer_t *out, int max)
{
    if (s_results == NULL || out == NULL) return 0;
    int n;
    pthread_mutex_lock(&s_results_mux);
    n = s_result_count < max ? s_result_count : max;
    memcpy(out, s_results, sizeof(ble_midi_peer_t) * (size_t)n);
    pthread_mutex_unlock(&s_results_mux);
    pthread_mutex_lock(&s_known_mux);
    for (int i = 0; i < n; i++) out[i].known = known_find(out[i].addr) >= 0;
    pthread_mutex_unlock(&s_known_mux);
    for (int i = 0; i < n; i++) out[i].connected = in_slot(out[i].addr);
    return n;
}

static void lookup_name(const uint8_t addr[6], char *out, size_t cap)
{
    out[0] = '\0';
    pthread_mutex_lock(&s_results_mux);
    for (int i = 0; i < s_result_count; i++) {
        if (memcmp(s_results[i].addr, addr, 6) == 0 && s_results[i].name[0]) {
            strncpy(out, s_results[i].name, cap - 1);
            out[cap - 1] = '\0';
        }
    }
    pthread_mutex_unlock(&s_results_mux);
    if (out[0]) return;
    pthread_mutex_lock(&s_known_mux);
    int k = known_find(addr);
    if (k >= 0) {
        strncpy(out, s_known[k].name, cap - 1);
        out[cap - 1] = '\0';
    }
    pthread_mutex_unlock(&s_known_mux);
    if (out[0] == '\0') snprintf(out, cap, "BLE %02X%02X", addr[4], addr[5]);
}

static void connect_next(void)
{
    if (!s_powered || s_connecting >= 0) return;
    for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES && s_connecting < 0; i++) {
        slot_t *s = &s_slots[i];
        if (s->status != BLE_MIDI_HOST_SLOT_CONNECTING) continue;
        if (bmh_swift_connect(i, s->addr)) {
            s_connecting = i;
        } else {
            s->stats.last_error = -1;
            slot_link_down(s);
        }
    }
}

static int request_connect(const uint8_t addr[6])
{
    int pick = -1;
    for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
        if (s_slots[i].used && memcmp(s_slots[i].addr, addr, 6) == 0) {
            if (s_slots[i].status != BLE_MIDI_HOST_SLOT_FREE) return i;
            pick = i;
            break;
        }
    }
    for (int i = 0; pick < 0 && i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
        if (!s_slots[i].used) pick = i;
    }
    for (int i = 0; pick < 0 && i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
        if (s_slots[i].status == BLE_MIDI_HOST_SLOT_FREE) pick = i;
    }
    if (pick < 0) return -1;

    slot_t *s = &s_slots[pick];
    s->used = true;
    s->parked = false;
    memcpy(s->addr, addr, 6);
    lookup_name(addr, s->name, sizeof(s->name));
    memset(&s->stats, 0, sizeof(s->stats));
    s->status = BLE_MIDI_HOST_SLOT_CONNECTING;
    register_slot(pick);
    uint32_t attempt = ++s->connect_attempt;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)CONNECT_TIMEOUT_MS * 1000000), s_q, ^{
        if (attempt != s->connect_attempt || s->status != BLE_MIDI_HOST_SLOT_CONNECTING) return;
        bmh_swift_cancel(pick);
        s->stats.last_error = -2;
        slot_link_down(s);
        connect_done(pick);
    });
    connect_next();
    return pick;
}

int BLE_MIDI_HOST_connect(const uint8_t addr[6], uint8_t addr_type)
{
    (void)addr_type;
    if (BLE_MIDI_HOST_init() != 0 || addr == NULL) return -1;
    if (!s_wanted) BLE_MIDI_HOST_enable(true);
    __block int slot = -1;
    dispatch_sync(s_q, ^{ slot = request_connect(addr); });
    return slot;
}

void BLE_MIDI_HOST_disconnect(int slot)
{
    slot_t *s = slot_at(slot);
    if (s == NULL) return;
    s->parked = true;
    dispatch_async(s_q, ^{
        if (s->status == BLE_MIDI_HOST_SLOT_FREE) return;
        bmh_swift_cancel(slot);
        slot_link_down(s);
        connect_done(slot);
    });
}

int BLE_MIDI_HOST_known(ble_midi_peer_t *out, int max)
{
    if (out == NULL) return 0;
    BLE_MIDI_HOST_init();
    int n;
    pthread_mutex_lock(&s_known_mux);
    n = s_known_count < max ? s_known_count : max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].addr, s_known[i].addr, 6);
        strncpy(out[i].name, s_known[i].name, sizeof(out[i].name) - 1);
        out[i].known = true;
    }
    pthread_mutex_unlock(&s_known_mux);
    for (int i = 0; s_slots && i < n; i++) out[i].connected = in_slot(out[i].addr);
    return n;
}

int BLE_MIDI_HOST_forget(const uint8_t addr[6])
{
    if (addr == NULL || BLE_MIDI_HOST_init() != 0) return -1;
    for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
        if (s_slots[i].status != BLE_MIDI_HOST_SLOT_FREE && memcmp(s_slots[i].addr, addr, 6) == 0) {
            BLE_MIDI_HOST_disconnect(i);
        }
    }
    int found = -1;
    pthread_mutex_lock(&s_known_mux);
    int k = known_find(addr);
    if (k >= 0) {
        found = k;
        memmove(&s_known[k], &s_known[k + 1], sizeof(known_t) * (size_t)(s_known_count - 1 - k));
        s_known_count--;
    }
    pthread_mutex_unlock(&s_known_mux);
    if (found < 0) return -1;
    struct { uint8_t a[6]; } copy;
    memcpy(copy.a, addr, 6);
    dispatch_async(s_q, ^{ bmh_swift_forget(copy.a); });
    return 0;
}

int BLE_MIDI_HOST_dev_status(int slot)
{
    slot_t *s = slot_at(slot);
    return s ? (int)s->status : BLE_MIDI_HOST_SLOT_FREE;
}

midi_tmask_t BLE_MIDI_HOST_dev_bit(int slot)
{
    slot_t *s = slot_at(slot);
    return s ? s->bit : 0;
}

bool BLE_MIDI_HOST_dev_connected(int slot)
{
    slot_t *s = slot_at(slot);
    return s && s->status == BLE_MIDI_HOST_SLOT_CONNECTED;
}

int BLE_MIDI_HOST_dev_info(int slot, ble_midi_peer_t *out)
{
    slot_t *s = slot_at(slot);
    if (s == NULL || out == NULL || !s->used) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->addr, s->addr, 6);
    strncpy(out->name, s->name, sizeof(out->name) - 1);
    out->connected = s->status == BLE_MIDI_HOST_SLOT_CONNECTED;
    pthread_mutex_lock(&s_known_mux);
    out->known = known_find(s->addr) >= 0;
    pthread_mutex_unlock(&s_known_mux);
    return 0;
}

void BLE_MIDI_HOST_dev_stats(int slot, ble_midi_host_stats_t *out)
{
    if (out == NULL) return;
    slot_t *s = slot_at(slot);
    if (s == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s->stats;
    out->status = s->status;
    out->mtu = s->mtu;
    out->decode_errors = s->dec.errors;
    out->rx_late = s->sync.late;
    out->rx_latency_us = rx_budget(s);
}

int BLE_MIDI_HOST_dev_send_packet(int slot, uint8_t cable, uint8_t cin,
                                  uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)cable;
    slot_t *s = slot_at(slot);
    if (s == NULL) return -1;
    if (s->status != BLE_MIDI_HOST_SLOT_CONNECTED) {
        s->stats.tx_dropped++;
        return -1;
    }
    uint16_t ts = now_ts();
    pthread_mutex_lock(&s->tx_mux);
    uint32_t head = s->tx_head;
    uint32_t next = (head + 1) & TX_MASK;
    if (next == s->tx_tail) {
        pthread_mutex_unlock(&s->tx_mux);
        s->stats.tx_dropped++;
        return -1;
    }
    s->tx[head].p[0] = cin & 0x0F;
    s->tx[head].p[1] = b1;
    s->tx[head].p[2] = b2;
    s->tx[head].p[3] = b3;
    s->tx[head].ts = ts;
    s->tx_head = next;
    pthread_mutex_unlock(&s->tx_mux);
    s->stats.tx_messages++;
    if (!__atomic_exchange_n(&s_flush_scheduled, true, __ATOMIC_ACQ_REL)) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)BLE_MIDI_HOST_TX_BATCH_US * 1000), s_q, ^{
            __atomic_store_n(&s_flush_scheduled, false, __ATOMIC_RELEASE);
            for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES; i++) tx_flush(&s_slots[i]);
        });
    }
    return 0;
}

int BLE_MIDI_HOST_dev_bytes_available(int slot)
{
    slot_t *s = slot_at(slot);
    if (s == NULL) return 0;
    return s->rx ? midi_timed_ring_bytes_due(s->rx) : 0;
}

int BLE_MIDI_HOST_dev_read_packet(int slot, uint8_t *buf, size_t maxlen)
{
    slot_t *s = slot_at(slot);
    if (s == NULL || s->rx == NULL) return 0;
    return midi_timed_ring_read(s->rx, buf, maxlen);
}

void BLE_MIDI_HOST_set_rx_latency(int32_t us)
{
    if (us < -1) us = -1;
    if (us > 100000) us = 100000;
    s_rx_latency = us;
}

int32_t BLE_MIDI_HOST_rx_latency_setting(void) { return s_rx_latency; }

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0) return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

int BLE_MIDI_HOST_find(const char *name)
{
    if (s_slots == NULL || name == NULL) return -1;
    for (int i = 0; i < BLE_MIDI_HOST_MAX_DEVICES; i++) {
        if (s_slots[i].status == BLE_MIDI_HOST_SLOT_CONNECTED && contains_ci(s_slots[i].name, name)) return i;
    }
    return -1;
}

void BLE_MIDI_HOST_format_addr(const uint8_t a[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

int BLE_MIDI_HOST_parse_addr(const char *s, uint8_t a[6])
{
    if (s == NULL) return -1;
    for (int i = 0; i < 6; i++) {
        int hi, lo;
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return -1;
        hi = isdigit((unsigned char)s[0]) ? s[0] - '0' : (toupper((unsigned char)s[0]) - 'A' + 10);
        lo = isdigit((unsigned char)s[1]) ? s[1] - '0' : (toupper((unsigned char)s[1]) - 'A' + 10);
        a[i] = (uint8_t)(hi << 4 | lo);
        s += 2;
        if (i < 5) {
            if (*s != ':' && *s != '-') return -1;
            s++;
        }
    }
    return *s == '\0' ? 0 : -1;
}
