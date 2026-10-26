/* Simple access owners; SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "db2010-components.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"


const char *db2010_rtc_access(void *opaque, DB2010Access *a)
{
    DB2010Rtc *rtc = opaque, preview = *rtc;
    uint64_t now = a->now;
    const char *fault = dbrtc_access(a->commit ? rtc : &preview, now, a->idle,
                        a->address, a->size, a->write, &a->value);
    if (!fault && a->commit) { dbrtc_update_irq(rtc); }
    return fault;
}

const char *db2010_retained_access(void *opaque, DB2010Access *a)
{
    DB2010RtcRetained *rtc = opaque, preview = *rtc;
    return dbrtr_access(a->commit ? rtc : &preview, a->address, a->size,
                        a->write, &a->value);
}

const char *db2010_startup_access(void *opaque, DB2010Access *a)
{
    return startup_status_access(opaque, a->address, a->size, a->write,
                                 &a->value, a->commit);
}

static const char *power_action(void *opaque, const DB2010DecodedAccess *d,
                                DB2010Access *a)
{
    DB2010Power *s = opaque;
    if (!s->power_known) { return "power_hold_initial_state_unknown"; }
    if (!a->write) { a->value = s->power_value; return NULL; }
    uint32_t value = a->value & 0xffff;
    if ((value & ~s->power_mask) != (s->power_value & ~s->power_mask)) {
        return "power_hold_other_bits_unmodeled";
    }
    bool release = (s->power_value & 1) && !(value & 1);
    if (release && (!s->allow_startup_release || a->pc != 0x44aceab8 ||
                    s->startup_releases || s->power_value != 0x8d || value != 0x8c)) {
        return "power_hold_release_unmodeled";
    }
    if (a->commit) {
        s->power_value = value;
        if (release) { s->startup_releases++; }
    }
    return NULL;
}

const DB2010RegMap *db2010_power_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_REG(0, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16,
                     .callback = power_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16,
                      .callback = power_action}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf9050058, rules,
            ARRAY_SIZE(rules), "power_hold_access_unmodeled");
        if (fault) { g_error("DB2010 power layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *db2010_power_access(void *opaque, DB2010Access *a)
{
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(db2010_power_map(), a, &d);
    if (!fault) { fault = db2010_reg_execute(opaque, &d, a); }
    return fault;
}

/* Task105: synthetic UART3 initialization handshake; other exact frames fail.
 * ack_level zero retains status03; positive levels opt into scoped setup acks.
 * No OS signals, buffers or execution state are supplied by the model. */
static const struct { uint8_t length, bytes[252], ack_level; } bluetooth_commands[] = {
    {4, {1, 3, 0x0c, 0}},
    {13, {1, 3, 0xfd, 9, 0, 0x38, 0xc6, 0x11, 1, 2, 0, 0, 0}},
    {13, {1, 3, 0xfd, 9, 0, 0x34, 0xc6, 0x11, 1, 0, 0xf8, 0, 0}},
    {7, {1, 0x2b, 0xfc, 3, 0, 0, 0xfd}},
    {6, {1, 0x0f, 8, 2, 5, 0}},
    {7, {1, 0x24, 0x0c, 3, 4, 2, 0x52}},
    {4, {1, 0x46, 0x0c, 0}},
    {4, {1, 0x42, 0x0c, 0}},
    {4, {1, 0x23, 0x0c, 0}},
    {7, {1, 0x24, 0x0c, 3, 4, 2, 0x42}},
    {4, {1, 5, 0x10, 0}},
    {7, {1, 0x24, 0x0c, 3, 4, 2, 0}},
    {5, {1, 0x0a, 0x0c, 1, 0}, 3},
    {5, {1, 0x22, 0x0c, 1, 0}, 3},
    {6, {1, 0x16, 0x0c, 2, 0x40, 0x1f}, 3},
    {7, {1, 0x24, 0x0c, 3, 0x14, 2, 0x42}, 3},
    {252, {1, 0x13, 0x0c, 0xf8, 'W', '8', '0', '0', 'i'}, 3},
    {5, {1, 0x20, 0x0c, 1, 0}, 3},
    {6, {1, 0x0f, 8, 2, 0x0f, 0}},
    {6, {1, 0x26, 0x0c, 2, 0x60, 0}},
    {6, {1, 0x18, 0x0c, 2, 0, 0x20}, 2},
    {4, {1, 9, 0x10, 0}},
    {5, {1, 0x0c, 0xfd, 1, 5}, 1},
    {12, {1, 0x22, 0xfc, 8, 0xfe, 6, 0xe7, 0x65, 0x90, 0xee, 0x12, 0}},
    {49, {
        0x1, 0x14, 0xfd, 0x2d, 0xff, 0xbc, 0x20, 0x2a, 0x0, 0x3, 0x0, 0x0,
        0x0, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x0, 0x0, 0x0, 0x1,
        0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xe2, 0xeb, 0xf7, 0xcf, 0x7f, 0xcb,
        0xff, 0x74, 0xfb, 0xfe, 0xb5, 0xf1, 0x7b, 0x77, 0x7d, 0xf8, 0xdd, 0xf,
        0x87
    }},
    {4, {1, 1, 0x10, 0}},
    {4, {1, 0x0f, 0xfc, 0}},
    {9, {1, 4, 0xfd, 5, 1, 0x14, 0x40, 0x11, 1}},
    {7, {1, 5, 0x0c, 3, 2, 0, 1}},
    {5, {1, 0x31, 0x0c, 1, 1}},
    {11, {1, 0x33, 0x0c, 7, 0xa0, 2, 0xff, 0x0a, 0, 0x0a, 0}},
};
G_STATIC_ASSERT(ARRAY_SIZE(bluetooth_commands) < 32);
static const uint8_t bluetooth_error[] = {4, 0x0e, 4, 1, 3, 0x0c, 3};
static unsigned bluetooth_response_length(const DB2010Uart *s)
{
    if (s->bluetooth_ack_level >= 4 && s->bluetooth_opcode == 0x1005) { return 14; }
    return s->bluetooth_ack_level >= 3 && s->bluetooth_opcode == 0x1009 ? 13 : sizeof(bluetooth_error);
}
bool db2010_bluetooth_state_valid(const DB2010Uart *s)
{
    return s->bluetooth_command < ARRAY_SIZE(bluetooth_commands) &&
        s->bluetooth_tx_pos < sizeof(bluetooth_commands[0].bytes) &&
        s->bluetooth_rx_pos <= bluetooth_response_length(s);
}
static uint8_t bluetooth_response_byte(const DB2010Uart *s)
{
    unsigned pos = s->bluetooth_rx_pos;
    if (s->bluetooth_ack_level >= 4 && s->bluetooth_opcode == 0x1005) {
        static const uint8_t no_buffers[] = {4, 0x0e, 11, 1, 5, 0x10, 0, 0, 0, 0, 0, 0, 0, 0};
        return no_buffers[pos];
    }
    if (s->bluetooth_ack_level >= 3 && s->bluetooth_opcode == 0x1009) {
        /* Declared synthetic identity; no controller memory or RF operation. */
        static const uint8_t identity[] = {4, 0x0e, 10, 1, 9, 0x10, 0, 2, 0, 0, 0, 0, 0};
        return identity[pos];
    }
    if (pos == 4) { return s->bluetooth_opcode & 255; }
    if (pos == 5) { return s->bluetooth_opcode >> 8; }
    unsigned level = bluetooth_commands[s->bluetooth_command].ack_level;
    if (pos == 6 && level && s->bluetooth_ack_level >= level) { return 0; }
    return bluetooth_error[pos];
}
static uint16_t bluetooth_status(const DB2010Uart *s)
{
    return s->bluetooth_mask & (2 | (s->bluetooth_rx_pos < bluetooth_response_length(s) ? 1 : 0) | (s->bluetooth_rx_timeout ? 0x40 : 0));
}
static void bluetooth_update_irq(DB2010Uart *s)
{
    db2010_signal_set(&s->bluetooth_irq, bluetooth_status(s) != 0);
}
static const char *bluetooth_uart_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Uart *s = opaque;
    switch (d->rule->offset) {
    case 0:
        if (s->bluetooth_rx_pos >= bluetooth_response_length(s)) { return "bluetooth_uart_rx_empty"; }
        a->value = bluetooth_response_byte(s);
        if (a->commit) { s->bluetooth_rx_pos++; s->bluetooth_rx_bytes++; }
        break;
    case 0x12:
        if (a->write) {
            if (a->value & ~0x1fff) { return "bluetooth_uart_mask_unmodeled"; }
            if (a->commit) { s->bluetooth_mask = a->value; }
        } else { a->value = s->bluetooth_mask; }
        break;
    case 0x14:
        if (a->write) {
            if (a->value != 0x40) { return "bluetooth_uart_irq_ack_unmodeled"; }
            if (a->commit) { s->bluetooth_rx_timeout = false; }
            break;
        }
        a->value = bluetooth_status(s); break;
    case 0x2e: a->value = bluetooth_response_length(s) - s->bluetooth_rx_pos; break;
    default: g_assert_not_reached();
    }
    if (a->commit) { bluetooth_update_irq(s); }
    return NULL;
}
const DB2010RegMap *db2010_bluetooth_uart_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_REG(0, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = bluetooth_uart_action}),
        DB_REG(0x12, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = bluetooth_uart_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = bluetooth_uart_action}),
        DB_REG(0x14, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = bluetooth_uart_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = bluetooth_uart_action}),
        DB_REG(0x2e, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = bluetooth_uart_action}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xfc030000, rules, ARRAY_SIZE(rules), "bluetooth_uart_access_unmodeled");
        if (fault) { g_error("DB2010 Bluetooth UART layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
static const char *uart_tx(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Uart *s = opaque;
    bool bt = s->bluetooth_enabled && d->bank == 2;
    unsigned candidates = 0, complete = 0;
    if (bt) {
        for (unsigned i = 0; i < ARRAY_SIZE(bluetooth_commands); i++) {
            if ((s->bluetooth_candidates & (1u << i)) &&
                s->bluetooth_tx_pos < bluetooth_commands[i].length &&
                a->value == bluetooth_commands[i].bytes[s->bluetooth_tx_pos]) {
                candidates |= 1u << i;
                if (s->bluetooth_tx_pos + 1 == bluetooth_commands[i].length) { complete = i + 1; }
            }
        }
        if (!candidates) { return "bluetooth_h4_command_unmodeled"; }
    }
    if (bt && s->bluetooth_rx_pos < bluetooth_response_length(s)) { return "bluetooth_response_not_drained"; }
    if (a->commit) {
        /* An open cable cannot deliver UART0 bytes to the serial peer. The
         * original CPU store still succeeds under the immediate-drain model. */
        if (s->data_enabled && d->bank == 0 && !s->data_present) { return NULL; }
        uint8_t byte = a->value;
        if (qemu_chr_fe_write_all(&s->uart[d->bank], &byte, 1) != 1) {
            return "uart_host_capture_failed";
        }
        s->uart_bytes[d->bank]++;
        if (bt) {
            s->bluetooth_tx_pos++;
            s->bluetooth_candidates = candidates;
            if (complete) {
                unsigned i = complete - 1;
                s->bluetooth_command = i;
                s->bluetooth_opcode = bluetooth_commands[i].bytes[1] | (bluetooth_commands[i].bytes[2] << 8);
                s->bluetooth_tx_pos = 0;
                s->bluetooth_candidates = (1u << ARRAY_SIZE(bluetooth_commands)) - 1;
                s->bluetooth_commands++;
                if (s->bluetooth_opcode == 0x0c03) { s->bluetooth_resets++; }
                if (s->bluetooth_error_peer) { s->bluetooth_rx_pos = 0; s->bluetooth_rx_timeout = true; }
                bluetooth_update_irq(s);
            }
        }
    }
    return NULL;
}
/* Task126 controlled host-backed UART0. FIFO delivery and short-packet timeout
 * are immediate batch assumptions; only real host bytes can create RX/autobaud.
 * No AT reply, OS event or firmware state is supplied here. */
static uint16_t data_uart_status(const DB2010Uart *s)
{
    return s->data_mask & (2 | (s->data_count ? 1 : 0) |
        (s->data_timeout ? 0x40 : 0) | (s->data_autobaud ? 0x20 : 0) | s->data_presence_events);
}
static void data_uart_irq(DB2010Uart *s)
{
    db2010_signal_set(&s->data_irq, data_uart_status(s) != 0);
}
/* Declared ideal cable pad plus the native UART's presence edges. The
 * original ISR owns ACKs, driver state and OS notifications. */
void db2010_data_uart_cable(DB2010Uart *s, bool present)
{
    if (s->data_present == present) { return; }
    s->data_present = present;
    s->data_presence_events |= present ? 0x80 : 0x100;
    s->data_cable_transitions++;
    if (s->data_bank) {
        s->data_bank->serial_levels = (s->data_bank->serial_levels & ~1) | present;
    }
    data_uart_irq(s);
    if (present) { qemu_chr_fe_accept_input(&s->uart[0]); }
}
void db2010_data_uart_receive(DB2010Uart *s, const uint8_t *bytes, unsigned count)
{
    g_assert(s->data_enabled && count <= sizeof(s->data_rx) - s->data_count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t byte = bytes[i];
        s->data_rx[(s->data_head + s->data_count++) % sizeof(s->data_rx)] = byte;
        if (s->data_mode_known && (s->data_mode & 7) == 2) {
            if (s->data_training == 1 && (byte == 'T' || byte == 't')) {
                s->data_autobaud = true;
                s->data_autobauds++;
                s->data_training = 2;
            } else if (s->data_training != 2) {
                s->data_training = (byte == 'A' || byte == 'a') ? 1 : 0;
            }
        }
    }
    s->data_received += count;
    if (count) { s->data_timeout = true; }
    data_uart_irq(s);
}
static int data_uart_can_receive(void *opaque)
{
    DB2010Uart *s = opaque;
    return s->data_present && s->data_mode_known && (s->data_mode & 7) != 7 ?
        sizeof(s->data_rx) - s->data_count : 0;
}
static void data_uart_receive(void *opaque, const uint8_t *bytes, int count)
{
    db2010_data_uart_receive(opaque, bytes, count);
}
static const char *data_uart_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Uart *s = opaque;
    switch (d->id) {
    case 0:
        if (!s->data_count) { return "data_uart_rx_empty"; }
        a->value = s->data_rx[s->data_head];
        if (a->commit) {
            s->data_head = (s->data_head + 1) % sizeof(s->data_rx);
            s->data_count--; s->data_rx_bytes++;
            qemu_chr_fe_accept_input(&s->uart[0]);
        }
        break;
    case 0x0a:
        if (a->write) {
            if (a->value != 7 && a->value != 0x100 && a->value != 0x102) {
                return "data_uart_mode_unmodeled";
            }
            if (a->commit) {
                if (!s->data_mode_known || s->data_mode != a->value) { s->data_training = 0; }
                s->data_mode = a->value; s->data_mode_known = true;
            }
        } else {
            if (!s->data_mode_known) { return "data_uart_mode_unknown"; }
            a->value = s->data_mode;
        }
        break;
    case 0x12:
        if (a->write) {
            if (a->value & ~0x1fff) { return "data_uart_mask_unmodeled"; }
            if (a->commit) { s->data_mask = a->value; }
        } else { a->value = s->data_mask; }
        break;
    case 0x14:
        if (a->write) {
            if (!a->value || (a->value & ~0x1f8)) { return "data_uart_ack_unmodeled"; }
            if (a->commit) {
                if (a->value & 0x40) { s->data_timeout = false; }
                if (a->value & 0x20) { s->data_autobaud = false; }
                s->data_presence_events &= ~a->value;
            }
        } else { a->value = data_uart_status(s); }
        break;
    case 0x2e: a->value = s->data_count; break;
    default: g_assert_not_reached();
    }
    if (a->commit) { data_uart_irq(s); }
    return NULL;
}
const DB2010RegMap *db2010_data_uart_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_REG(0, .id = 0, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = data_uart_action}),
        DB_REG(0x0a, .id = 0x0a, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = data_uart_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = data_uart_action}),
        DB_REG(0x12, .id = 0x12, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = data_uart_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = data_uart_action}),
        DB_REG(0x14, .id = 0x14, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = data_uart_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = data_uart_action}),
        DB_REG(0x2e, .id = 0x2e, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = data_uart_action}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xfb060000, rules, ARRAY_SIZE(rules), "data_uart_access_unmodeled");
        if (fault) { g_error("DB2010 data UART layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const DB2010RegMap *db2010_uart_map(void)
{
    static const uint32_t bases[] = {0xfb060000, 0xfc040000, 0xfc030000, 0xfb040000};
    static const DB2010RegRule port[] = {
        DB_REG(2, .alignment = 2,
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16,
                .callback = uart_tx, .constraint = {.kind = DB_VALUE_RANGE,
                    .maximum = 255, .fault = "uart_access_unmodeled"}}),
        DB_REG(0x2c, .alignment = 2,
            .read = {.kind = DB_ACTION_CONSTANT, .widths = DB_WIDTH16, .constant = 1}),
    };
    static const DB2010RegRule rules[] = {
        DB_REPEAT(0, 4, .bases = bases, .children = port, .child_count = ARRAY_SIZE(port)),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0, rules,
            ARRAY_SIZE(rules), "uart_access_unmodeled");
        if (fault) { g_error("DB2010 UART layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *db2010_uart_access(void *opaque, DB2010Access *a)
{
    DB2010DecodedAccess d;
    DB2010Uart *s = opaque;
    const DB2010RegMap *map = db2010_uart_map();
    const DB2010RegMap *bt = db2010_bluetooth_uart_map();
    if (s->bluetooth_enabled && db2010_claims(bt->claims, bt->claim_count, a->address, a->size)) { map = bt; }
    const DB2010RegMap *data = db2010_data_uart_map();
    if (s->data_enabled && db2010_claims(data->claims, data->claim_count, a->address, a->size)) { map = data; }
    const char *fault = db2010_reg_decode(map, a, &d);
    if (!fault) { fault = db2010_reg_execute(opaque, &d, a); }
    return fault;
}

bool startup_status_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = db2010_startup_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}

static const char *startup_action(void *opaque, const DB2010DecodedAccess *d,
                                   DB2010Access *a)
{
    DB2010Startup *s = opaque;
    if (a->write) {
        if (a->commit) { s->startup_status_value = 0; s->startup_status_clears++; }
    } else {
        a->value = s->startup_status_value;
        if (a->commit) { s->startup_status_reads++; }
    }
    return NULL;
}

const DB2010RegMap *db2010_startup_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_VIEW(0, .alignment = 1, .backing_width = 1,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16,
                     .callback = startup_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH8,
                .callback = startup_action,
                .constraint = {.kind = DB_VALUE_MASKED, .mask = 255, .value = 0,
                               .fault = "startup_status_write_unmodeled"}}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf905005c, rules,
            ARRAY_SIZE(rules), "startup_status_access_unmodeled");
        if (fault) { g_error("DB2010 startup layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}

const char *startup_status_access(DB2010Startup *s, uint32_t address,
                                  unsigned size, bool write, uint32_t *value,
                                  bool commit)
{
    DB2010Access a = {.address = address, .size = size, .write = write,
                      .value = *value, .commit = commit};
    DB2010DecodedAccess decoded;
    const char *fault = db2010_reg_decode(db2010_startup_map(), &a, &decoded);
    if (!fault) { fault = db2010_reg_execute(s, &decoded, &a); }
    *value = a.value;
    return fault;
}


void db2010_power_reset(DB2010Power *s)
{
    s->startup_releases = 0;
    s->power_value = s->initial_power; s->power_known = s->initial_power_known;
}
void db2010_startup_reset(DB2010Startup *s)
{
    s->startup_status_value = s->startup_status_initial;
    s->startup_status_reads = s->startup_status_clears = 0;
}
void db2010_uart_reset(DB2010Uart *s)
{
    memset(s->uart_bytes, 0, sizeof(s->uart_bytes));
    s->bluetooth_mask = 0; s->bluetooth_rx_timeout = false;
    s->bluetooth_tx_pos = 0; s->bluetooth_rx_pos = sizeof(bluetooth_error);
    s->bluetooth_resets = s->bluetooth_rx_bytes = s->bluetooth_commands = 0;
    s->bluetooth_candidates = (1u << ARRAY_SIZE(bluetooth_commands)) - 1;
    s->bluetooth_opcode = 0; s->bluetooth_command = 0;
    db2010_signal_set(&s->bluetooth_irq, false);
    s->data_mode_known = s->data_timeout = s->data_autobaud = false;
    s->data_present = !s->data_bank || !s->data_bank->has_serial || (s->data_bank->serial_levels & 1);
    s->data_presence_events = 0; s->data_cable_transitions = 0;
    s->data_mode = s->data_mask = s->data_head = s->data_count = s->data_training = 0;
    s->data_rx_bytes = s->data_received = s->data_autobauds = 0;
    memset(s->data_rx, 0, sizeof(s->data_rx));
    db2010_signal_set(&s->data_irq, false);
}

void db2010_startup_export(DB2010Startup *model, QDict *snapshot)
{

        QDict *r = qdict_new();
        qdict_put_int(r, "initial_value", model->startup_status_initial);
        qdict_put_int(r, "value", model->startup_status_value);
        qdict_put_int(r, "reads", model->startup_status_reads);
        qdict_put_int(r, "clears", model->startup_status_clears);
        qdict_put_bool(r, "synthetic_initial_state", true);
        qdict_put(snapshot, "startup_status", r);
    }

void db2010_uart_export(DB2010Uart *model, QDict *snapshot)
{

        QList *counts = qlist_new();
        for (unsigned port = 0; port < 4; port++) { qlist_append_int(counts, model->uart_bytes[port]); }
        qdict_put(snapshot, "uart_bytes", counts);
        if (model->data_enabled) {
            QDict *data = qdict_new();
            qdict_put_bool(data, "cable_present", model->data_present);
            qdict_put_int(data, "presence_events", model->data_presence_events);
            qdict_put_int(data, "cable_transitions", model->data_cable_transitions);
            qdict_put_int(data, "mode", model->data_mode);
            qdict_put_bool(data, "mode_known", model->data_mode_known);
            qdict_put_int(data, "mask", model->data_mask);
            qdict_put_int(data, "status", data_uart_status(model));
            qdict_put_int(data, "rx_count", model->data_count);
            qdict_put_int(data, "received", model->data_received);
            qdict_put_int(data, "rx_bytes", model->data_rx_bytes);
            qdict_put_int(data, "autobauds", model->data_autobauds);
            qdict_put_bool(data, "timeout", model->data_timeout);
            qdict_put_bool(data, "autobaud", model->data_autobaud);
            qdict_put(snapshot, "serial_transport", data);
        }
        if (model->bluetooth_enabled) {
            QDict *bt = qdict_new();
            qdict_put_str(bt, "peer", model->bluetooth_ack_level >= 4 ? "zero-capacity-errors" :
                model->bluetooth_ack_level >= 3 ? "init-ack-errors" :
                model->bluetooth_ack_level >= 2 ? "setup-ack-errors" :
                model->bluetooth_ack_level >= 1 ? "baud-ack-errors" :
                model->bluetooth_error_peer ? "command-error-03" : "silent");
            qdict_put_int(bt, "mask", model->bluetooth_mask);
            qdict_put_int(bt, "status", bluetooth_status(model));
            qdict_put_int(bt, "tx_position", model->bluetooth_tx_pos);
            qdict_put_int(bt, "rx_position", model->bluetooth_rx_pos);
            qdict_put_bool(bt, "rx_timeout", model->bluetooth_rx_timeout);
            qdict_put_int(bt, "resets", model->bluetooth_resets);
            qdict_put_int(bt, "commands", model->bluetooth_commands);
            qdict_put_int(bt, "opcode", model->bluetooth_opcode);
            qdict_put_int(bt, "rx_bytes", model->bluetooth_rx_bytes);
            qdict_put_bool(bt, "synthetic", true);
            qdict_put(snapshot, "bluetooth_transport", bt);
        }
    }

void db2010_power_export(DB2010Power *model, QDict *snapshot)
{

        if (model->allow_startup_release) qdict_put_int(snapshot, "power_startup_releases", model->startup_releases);
        if (model->power_known) {
            qdict_put_int(snapshot, "power_hold", model->power_value);
        } else {
            qdict_put_null(snapshot, "power_hold");
        }
    }

void db2010_uart_connect(DB2010Uart *s)
{
        for (unsigned port = 0; port < 4; port++) {
            g_autofree char *name = g_strdup_printf("uart%u", port);
            Chardev *chr = qemu_chr_find(name);
            if (!chr) { error_report("db2010: missing host UART chardev %s", name); exit(EXIT_FAILURE); }
            qemu_chr_fe_init(&s->uart[port], chr, &error_fatal);
            if (port == 0 && s->data_enabled) {
                qemu_chr_fe_set_handlers(&s->uart[0], data_uart_can_receive, data_uart_receive,
                                        NULL, NULL, s, NULL, true);
            }
        }
    }
