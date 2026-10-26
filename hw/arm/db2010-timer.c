/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-timer.h"
#include "db2010-bus.h"

typedef enum DB2010TimerEventKind { DBIRQ_SELECT, DBIRQ_ACK, DBIRQ_READ_SELECTION, DBIRQ_SOURCE } DB2010TimerEventKind;
typedef struct DB2010TimerEvent {
    DB2010TimerEventKind kind;
    uint32_t address, *value;
    DB2010IrqLine line;
    bool level;
} DB2010TimerEvent;
static const char *dbtimer_dispatch(DB2010Timer *t, const DB2010TimerEvent *e);

bool dbtimer_routed(const DB2010Timer *t)
{
    return t->state.pending && !t->state.selected && t->state.masks_known[0] && t->state.masks_known[1] &&
        !(t->state.masks[0] & (1u << 28)) && !(t->state.masks[1] & (1u << 25));
}

/* Original TIMER2 ifunit1/intline54 -> secondary26/cascade28. */
static bool paired_timer_routed(const DB2010Timer *t)
{
    return t->config.paired_channel && t->state.paired_pending && !t->state.selected &&
        t->state.masks_known[0] && t->state.masks_known[1] &&
        !(t->state.masks[0] & (1u << 28)) && !(t->state.masks[1] & (1u << 26));
}

bool dbirq_i2c_routed(const DB2010Timer *t)
{
    return t->config.i2c_route && t->state.i2c_pending && !t->state.selected &&
        t->state.masks_known[0] && t->state.masks_known[2] &&
        !(t->state.masks[0] & (1u << 30)) && !(t->state.masks[2] & (1u << 10));
}

bool dbirq_pdi_routed(const DB2010Timer *t)
{
    return t->config.display_route && t->state.pdi_pending && !t->state.selected &&
           t->state.masks_known[0] && t->state.masks_known[1] &&
           !(t->state.masks[0] & (1u << 28)) && !(t->state.masks[1] & (1u << 13));
}

bool dbirq_dma_routed(const DB2010Timer *t)
{
    return t->config.display_route && t->state.dma_pending && !t->state.selected &&
           t->state.masks_known[0] && t->state.masks_known[2] &&
           !(t->state.masks[0] & (1u << 30)) && !(t->state.masks[2] & (1u << 17));
}

bool dbirq_dsp_routed(const DB2010Timer *t)
{
    return t->config.dsp_route && t->state.dsp_pending && !t->state.selected &&
           t->state.masks_known[0] && !(t->state.masks[0] & (1u << 9));
}

static bool dbirq_keypad_routed(const DB2010Timer *t)
{
    return t->config.keypad_route && t->state.keypad_pending && !t->state.selected &&
        t->state.masks_known[0] && t->state.masks_known[2] &&
        !(t->state.masks[0] & (1u << 30)) && !(t->state.masks[2] & 1);
}
static bool dbirq_rtc_routed(const DB2010Timer *t)
{
    return t->config.rtc_route && t->state.rtc_pending && !t->state.selected &&
        t->state.masks_known[0] && t->state.masks_known[1] &&
        !(t->state.masks[0] & (1u << 28)) && !(t->state.masks[1] & (1u << 15));
}

static bool sim_rx_routed(const DB2010Timer *t)
{
    return t->config.simif_route && t->state.sim_rx_pending && !t->state.selected &&
        t->state.masks_known[0] && !(t->state.masks[0] & (1u<<5));
}
static bool sim_tx_routed(const DB2010Timer *t)
{
    return t->config.simif_route && t->state.sim_tx_pending && !t->state.selected &&
        t->state.masks_known[0] && t->state.masks_known[1] &&
        !(t->state.masks[0] & (1u<<28)) && !(t->state.masks[1] & (1u<<6));
}
/* Native binding: compact47 -> secondary19/cascade28 -> OSE142 ->44A984B8. */
static bool bluetooth_routed(const DB2010Timer *t)
{
    return t->config.bluetooth_route && t->state.bluetooth_pending && !t->state.selected &&
        t->state.masks_known[0] && t->state.masks_known[1] &&
        !(t->state.masks[0] & (1u << 28)) && !(t->state.masks[1] & (1u << 19));
}
static bool data_uart_routed(const DB2010Timer *t)
{
    return t->config.data_uart_route && t->state.data_uart_pending && !t->state.selected &&
        t->state.masks_known[0] && t->state.masks_known[1] &&
        !(t->state.masks[0] & (1u << 28)) && !(t->state.masks[1] & (1u << 16));
}
bool dbirq_any_routed(const DB2010Timer *t)
{
    return dbirq_keypad_routed(t) || dbtimer_routed(t) || paired_timer_routed(t) || dbirq_i2c_routed(t) || dbirq_pdi_routed(t) || dbirq_dma_routed(t) || dbirq_dsp_routed(t) ||
        dbirq_rtc_routed(t) || sim_rx_routed(t) || sim_tx_routed(t) || bluetooth_routed(t) || data_uart_routed(t);
}

void dbtimer_update_irq(DB2010Timer *t)
{
    db2010_signal_set(&t->irq, dbirq_any_routed(t));
}
const char *dbtimer_advance(DB2010Timer *t, uint64_t now)
{
    uint64_t delta = db2010_clock_sync(&t->state.phase, t->clock, now);
    t->state.last_cycles = t->state.phase.ticks;
    if (delta && (t->state.control & 1) && t->state.load_known) {
        uint64_t period = (uint64_t)t->state.load + 1;
        uint64_t sum = (uint64_t)t->state.counter + delta;
        if (sum >= period) { t->state.pending = true; }
        t->state.counter = sum % period;
    }
    if (delta && t->config.paired_channel && (t->state.control & 2) && t->state.paired_load_known) {
        uint64_t period = (uint64_t)t->state.paired_load + 1;
        uint64_t sum = (uint64_t)t->state.paired_counter + delta;
        if (sum >= period) { t->state.paired_pending = true; }
        t->state.paired_counter = sum % period;
    }
    return NULL;
}
int64_t dbtimer_deadline(DB2010Timer *t)
{
    /* Reload changes keep the live counter; an exceeded threshold expires on
     * the next source edge, matching the inclusive inherited contract. */
    int64_t due = -1;
    if ((t->state.control & 1) && t->state.load_known) {
        uint64_t period = (uint64_t)t->state.load + 1;
        uint64_t remaining = t->state.counter >= period ? 1 : period - t->state.counter;
        due = db2010_clock_deadline(&t->state.phase, t->clock, remaining);
    }
    if (t->config.paired_channel && (t->state.control & 2) && t->state.paired_load_known) {
        uint64_t period = (uint64_t)t->state.paired_load + 1;
        uint64_t remaining = t->state.paired_counter >= period ? 1 : period - t->state.paired_counter;
        int64_t paired = db2010_clock_deadline(&t->state.phase, t->clock, remaining);
        if (due < 0 || (paired >= 0 && paired < due)) { due = paired; }
    }
    return due;
}

typedef enum DB2010TimerRegister {
    DBT_CONTROL, DBT_LOAD, DBT_PAIRED_LOAD, DBT_COUNTER, DBT_PRIMARY_PENDING, DBT_SECONDARY_PENDING,
    DBT_PRIMARY_SELECT, DBT_SECONDARY_SELECT, DBT_TERTIARY_PENDING, DBT_TERTIARY_SELECT,
} DB2010TimerRegister;

bool dbtimer_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = dbtimer_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}

static const char *timer_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Timer *t = opaque;
    uint32_t *value = &a->value;
    bool write = a->write;
    if (d->id == DBT_PAIRED_LOAD && !t->config.paired_channel) {
        return "timer_paired_channel_unmodeled";
    }
    if (write) {
        if (d->id == DBT_CONTROL) {
            if (*value & ~(t->config.paired_channel ? 3u : 1u)) {
                return "timer_other_channel_unmodeled";
            }
            t->state.control = *value;
            if (!(*value & 1)) {
                t->state.counter = 0;
            }
            if (!(*value & 2)) { t->state.paired_counter = 0; }
        } else if (d->id == DBT_LOAD) {
            t->state.load = *value;
            t->state.load_known = true;
        } else if (d->id == DBT_PAIRED_LOAD) {
            t->state.paired_load = *value;
            t->state.paired_load_known = true;
        } else {
            return "timer_irq_register_write_unmodeled";
        }
    } else {
        switch (d->id) {
        case DBT_CONTROL: *value = t->state.control; break;
        case DBT_LOAD:
            if (!t->state.load_known) {
                return "timer_load_unknown";
            }
            *value = t->state.load; break;
        case DBT_PAIRED_LOAD:
            if (!t->state.paired_load_known) { return "timer_paired_load_unknown"; }
            *value = t->state.paired_load; break;
        case DBT_COUNTER: *value = t->state.counter; break;
        case DBT_SECONDARY_PENDING: *value = (t->state.pending ? (1u << 25) : 0) | (t->config.paired_channel && t->state.paired_pending ? (1u << 26) : 0) | (t->state.pdi_pending ? (1u << 13) : 0) | (t->state.rtc_pending ? (1u << 15) : 0) | (t->state.sim_tx_pending ? (1u << 6) : 0) | (t->state.bluetooth_pending ? (1u << 19) : 0) | (t->state.data_uart_pending ? (1u << 16) : 0); break;
        case DBT_PRIMARY_PENDING:
            *value = t->state.pending && t->state.masks_known[1] && !(t->state.masks[1] & (1u << 25)) ? (1u << 28) : 0;
            if (t->config.paired_channel && t->state.paired_pending && t->state.masks_known[1] && !(t->state.masks[1] & (1u << 26))) { *value |= 1u << 28; }
            if (t->config.i2c_route && t->state.i2c_pending && t->state.masks_known[2] && !(t->state.masks[2] & (1u << 10))) {
                *value |= 1u << 30;
            }
            if (t->config.display_route && t->state.pdi_pending && t->state.masks_known[1] && !(t->state.masks[1] & (1u << 13))) { *value |= 1u << 28; }
            if (t->config.rtc_route && t->state.rtc_pending && t->state.masks_known[1] && !(t->state.masks[1] & (1u << 15))) { *value |= 1u << 28; }
            if (t->config.display_route && t->state.dma_pending && t->state.masks_known[2] && !(t->state.masks[2] & (1u << 17))) { *value |= 1u << 30; }
            if (t->config.keypad_route && t->state.keypad_pending && t->state.masks_known[2] && !(t->state.masks[2] & 1)) { *value |= 1u << 30; }
            if (t->config.simif_route && t->state.sim_rx_pending) { *value |= 1u<<5; }
            if (t->config.simif_route && t->state.sim_tx_pending && t->state.masks_known[1] && !(t->state.masks[1] & (1u<<6))) { *value |= 1u<<28; }
            if (t->config.data_uart_route && t->state.data_uart_pending && t->state.masks_known[1] && !(t->state.masks[1] & (1u << 16))) { *value |= 1u << 28; }
            if (t->config.bluetooth_route && t->state.bluetooth_pending && t->state.masks_known[1] && !(t->state.masks[1] & (1u << 19))) { *value |= 1u << 28; }
            if (t->config.dsp_route && t->state.dsp_pending) { *value |= 1u << 9; }
            break;
        case DBT_PRIMARY_SELECT:
        case DBT_SECONDARY_SELECT:
            return dbtimer_dispatch(t, &(DB2010TimerEvent){.kind = DBIRQ_READ_SELECTION, .address = d->rule->offset, .value = value});
        case DBT_TERTIARY_PENDING:
            if (!t->config.i2c_route && !t->config.display_route && !t->config.keypad_route) { return "timer_irq_register_unmodeled"; }
            *value = (t->state.keypad_pending ? 1 : 0) | (t->state.i2c_pending ? (1u << 10) : 0) | (t->state.dma_pending ? (1u << 17) : 0); break;
        case DBT_TERTIARY_SELECT:
            return dbtimer_dispatch(t, &(DB2010TimerEvent){.kind = DBIRQ_READ_SELECTION, .address = d->rule->offset, .value = value});
        default: return "timer_irq_register_unmodeled";
        }
    }
    return NULL;
}
const DB2010RegMap *dbtimer_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_REG(0xf9010000, .id = DBT_CONTROL, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action}),
        DB_REG(0xf9010004, .id = DBT_LOAD, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action}),
        DB_REG(0xf9010008, .id = DBT_PAIRED_LOAD, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action}),
        DB_REG(0xf901001c, .id = DBT_COUNTER, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "timer_irq_register_write_unmodeled"}),
        DB_REG(0xfb020100, .id = DBT_PRIMARY_PENDING, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "timer_irq_register_write_unmodeled"}),
        DB_REG(0xfb020200, .id = DBT_SECONDARY_PENDING, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "timer_irq_register_write_unmodeled"}),
        DB_REG(0xfb020110, .id = DBT_PRIMARY_SELECT, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "timer_irq_register_write_unmodeled"}),
        DB_REG(0xfb020210, .id = DBT_SECONDARY_SELECT, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "timer_irq_register_write_unmodeled"}),
        DB_REG(0xfb020300, .id = DBT_TERTIARY_PENDING, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "timer_irq_register_write_unmodeled"}),
        DB_REG(0xfb020310, .id = DBT_TERTIARY_SELECT, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = timer_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "timer_irq_register_write_unmodeled"}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0, rules,
            ARRAY_SIZE(rules), "timer_irq_register_unmodeled");
        if (fault) { g_error("DB2010 timer/IRQ layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *dbtimer_access(DB2010Timer *t, uint32_t address,
                           unsigned size, bool write, uint32_t *value)
{
    DB2010Access a = {.address = address, .size = size, .write = write,
                      .value = *value, .commit = true};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbtimer_map(), &a, &d);
    if (fault) {
        if (size != 4 || address % 4) { return "timer_irq_access_width_unmodeled"; }
        return write ? "timer_irq_register_write_unmodeled" : fault;
    }
    fault = db2010_reg_execute(t, &d, &a);
    *value = a.value;
    return fault;
}

static const char *dbtimer_ack_selected(DB2010Timer *t, const DB2010TimerEvent *e)
{
    uint32_t address = e->address, value = *e->value;
    if (value != 1) {
        return "irq_ack_value_unmodeled";
    }
    if ((t->state.dsp_selected || t->state.sim_rx_selected) && address != 0xfb020118) { return "irq_ack_wrong_bank"; }
    if (address != 0xfb020118) {
        if (address != ((t->state.i2c_selected || t->state.dma_selected || t->state.keypad_selected) ? 0xfb020318 : 0xfb020218)) { return "irq_ack_wrong_bank"; }
        if (t->state.paired_selected) { t->state.paired_pending = false; }
        else if (!t->state.keypad_selected && !t->state.i2c_selected && !t->state.pdi_selected && !t->state.dma_selected && !t->state.rtc_selected && !t->state.sim_tx_selected && !t->state.bluetooth_selected && !t->state.data_uart_selected) { t->state.pending = false; }
        t->state.secondary_acked = DBIRQ_SECONDARY_ACKED;
    } else {
        if (!t->state.secondary_acked && !t->state.dsp_selected && !t->state.sim_rx_selected) {
            return "irq_primary_ack_before_secondary";
        }
        t->state.selected = DBIRQ_UNSELECTED;
        t->state.sim_rx_selected = t->state.sim_tx_selected = false;
        t->state.keypad_selected = false;
        t->state.rtc_selected = false;
        t->state.bluetooth_selected = false;
        t->state.data_uart_selected = false;
        t->state.paired_selected = false;
        t->state.i2c_selected = t->state.pdi_selected = t->state.dma_selected = t->state.dsp_selected = false;
    }
    return NULL;
}
static const char *dbtimer_select_source(DB2010Timer *t)
{
    bool first_timer = dbtimer_routed(t);
    bool paired = !first_timer && paired_timer_routed(t);
    bool timer = first_timer || paired;
    bool i2c = dbirq_i2c_routed(t), pdi = dbirq_pdi_routed(t), dma = dbirq_dma_routed(t);
    bool dsp = dbirq_dsp_routed(t);
    bool keypad = dbirq_keypad_routed(t);
    bool rtc = dbirq_rtc_routed(t);
    bool sim_rx=sim_rx_routed(t), sim_tx=sim_tx_routed(t), bt=bluetooth_routed(t), serial=data_uart_routed(t);
    if (!timer && !i2c && !pdi && !dma && !dsp && !keypad && !rtc && !sim_rx && !sim_tx && !bt && !serial) {
        return "irq_entry_without_routed_source";
    }
    if (!t->config.unlimited_irqs && t->state.irqs >= t->config.max_irqs) {
        return "timer_irq_budget";
    }
    t->state.selected = DBIRQ_SELECTED;
    t->state.paired_selected = paired;
    if (paired) { t->state.paired_irqs++; }
    t->state.i2c_selected = !timer && i2c;
    t->state.rtc_selected = !timer && !i2c && rtc;
    t->state.pdi_selected = !timer && !i2c && !rtc && pdi;
    t->state.dsp_selected = !timer && !i2c && !rtc && !pdi && dsp;
    t->state.dma_selected = !timer && !i2c && !rtc && !pdi && !dsp && dma;
    t->state.keypad_selected = !timer && !i2c && !rtc && !pdi && !dsp && !dma && keypad;
    bool other=timer || i2c || pdi || dma || dsp || keypad || rtc;
    t->state.sim_rx_selected=!other && sim_rx;
    t->state.sim_tx_selected=!other && !sim_rx && sim_tx;
    t->state.bluetooth_selected = !other && !sim_rx && !sim_tx && bt;
    t->state.data_uart_selected = !other && !sim_rx && !sim_tx && !bt && serial;
    if (t->state.data_uart_selected) { t->state.data_uart_irqs++; }
    if (t->state.bluetooth_selected) { t->state.bluetooth_irqs++; }
    if (t->state.sim_rx_selected) { t->state.sim_rx_irqs++; }
    if (t->state.sim_tx_selected) { t->state.sim_tx_pending=false; t->state.sim_tx_irqs++; }
    if (t->state.rtc_selected) {
        t->state.rtc_pending = false; /* controller edge, distinct from RTC status */
        t->state.rtc_irqs++;
    }
    if (t->state.keypad_selected) {
        t->state.keypad_pending = false; /* consume controller edge */
        t->state.keypad_irqs++;
    }
    if (t->state.dsp_selected) { t->state.dsp_irqs++; }
    if (t->state.i2c_selected) { t->state.i2c_irqs++; }
    if (t->state.pdi_selected) { t->state.pdi_irqs++; }
    if (t->state.dma_selected) { t->state.dma_irqs++; }
    t->state.secondary_acked = DBIRQ_WAIT_SECONDARY;
    t->state.irqs++;
    return NULL;
}
static const char *dbtimer_read_selected(DB2010Timer *t, const DB2010TimerEvent *e)
{
    if (e->address == 0xfb020310) {
        if (!t->state.selected || (!t->state.keypad_selected && !t->state.i2c_selected && !t->state.dma_selected)) { return "irq_selection_without_pending"; }
        *e->value = t->state.keypad_selected ? 0 : t->state.dma_selected ? 17 : 10;
    } else {
        if (!t->state.selected) { return "irq_selection_without_pending"; }
        if ((t->state.keypad_selected || t->state.i2c_selected || t->state.dma_selected || t->state.dsp_selected || t->state.sim_rx_selected) && e->address == 0xfb020210) { return "irq_selection_wrong_bank"; }
        *e->value = t->state.data_uart_selected ? (e->address == 0xfb020110 ? 28 : 16) : t->state.bluetooth_selected ? (e->address == 0xfb020110 ? 28 : 19) : t->state.sim_rx_selected ? 5 : t->state.sim_tx_selected ? (e->address==0xfb020110 ? 28:6) : t->state.dsp_selected ? 9 : e->address == 0xfb020110 ?
            ((t->state.i2c_selected || t->state.dma_selected || t->state.keypad_selected) ? 30 : 28) : (t->state.rtc_selected ? 15 : t->state.pdi_selected ? 13 : t->state.paired_selected ? 26 : 25);
    }
    return NULL;
}
static const char *dbtimer_dispatch(DB2010Timer *t, const DB2010TimerEvent *e)
{
    switch (e->kind) {
    case DBIRQ_SELECT: return dbtimer_select_source(t);
    case DBIRQ_ACK:
        switch (t->state.selected) {
        case DBIRQ_UNSELECTED: return NULL;
        case DBIRQ_SELECTED: return dbtimer_ack_selected(t, e);
        default: g_assert_not_reached();
        }
    case DBIRQ_READ_SELECTION: return dbtimer_read_selected(t, e);
    case DBIRQ_SOURCE:
        switch (e->line) {
        case DBIRQ_DATA_UART: t->state.data_uart_pending=e->level; break;
        case DBIRQ_BLUETOOTH: t->state.bluetooth_pending=e->level; break;
        case DBIRQ_SIM_RX: t->state.sim_rx_pending=e->level; break;
        case DBIRQ_SIM_TX:
            if (e->level && !t->state.sim_tx_input_level) { t->state.sim_tx_pending=true; }
            t->state.sim_tx_input_level=e->level; break;
        case DBIRQ_KEYPAD: t->state.keypad_pending = e->level; break;
        case DBIRQ_I2C: t->state.i2c_pending = e->level; break;
        case DBIRQ_PDI: t->state.pdi_pending = e->level; break;
        case DBIRQ_DMA: t->state.dma_pending = e->level; break;
        case DBIRQ_DSP: t->state.dsp_pending = e->level; break;
        case DBIRQ_RTC:
            if (e->level && !t->state.rtc_input_level) { t->state.rtc_pending = true; }
            t->state.rtc_input_level = e->level;
            break;
        }
        return NULL;
    default: g_assert_not_reached();
    }
}
const char *dbtimer_observe(DB2010Timer *t, uint32_t address, uint32_t value)
{
    if (address == 0xfb020104 || address == 0xfb020204 || address == 0xfb020304) {
        unsigned bank = (address - 0xfb020104) / 0x100;
        t->state.masks[bank] = value; t->state.masks_known[bank] = true;
    } else if (address == 0xfb020118 || address == 0xfb020218 || address == 0xfb020318) {
        DB2010TimerEvent e = {.kind = DBIRQ_ACK, .address = address, .value = &value};
        return dbtimer_dispatch(t, &e);
    }
    return NULL;
}
const char *dbtimer_select(DB2010Timer *t)
{
    DB2010TimerEvent e = {.kind = DBIRQ_SELECT};
    return dbtimer_dispatch(t, &e);
}


void dbtimer_reset(DB2010Timer *d)
{
    d->state = (DB2010TimerRuntime){.phase.ns = db2010_virtual_ns()};
    memcpy(d->state.masks, d->config.initial_masks, sizeof(d->state.masks));
    memcpy(d->state.masks_known, d->config.initial_masks_known, sizeof(d->state.masks_known));
}

void dbtimer_export(DB2010Timer *model, QDict *snapshot, QList * irq_events)
{

        DB2010Timer preview = *model;
        DB2010Timer *t = &preview;
        dbtimer_advance(t, db2010_virtual_ns());
        QDict *timer = qdict_new();
        qdict_put_int(timer, "control", t->state.control);
        qdict_put_int(timer, "deadline_ns", dbtimer_deadline(t));
        qdict_put_int(timer, "phase_billionths", t->state.phase.fraction);
        if (t->state.load_known) { qdict_put_int(timer, "load", t->state.load); } else { qdict_put_null(timer, "load"); }
        qdict_put_int(timer, "counter", t->state.counter);
        qdict_put_int(timer, "last_cycles", t->state.last_cycles);
        qdict_put_int(timer, "idle_cycles", 0);
        qdict_put_str(timer, "idle_accounting", "native QEMU; no synthetic cycles");
        qdict_put_int(timer, "irqs", t->state.irqs);
        qdict_put_bool(timer, "pending", t->state.pending);
        qdict_put_bool(timer, "selected", t->state.selected);
        qdict_put_bool(timer, "secondary_acked", t->state.secondary_acked);
        if (model->config.paired_channel) {
            QDict *paired = qdict_new();
            if (t->state.paired_load_known) { qdict_put_int(paired, "load", t->state.paired_load); }
            else { qdict_put_null(paired, "load"); }
            qdict_put_int(paired, "counter", t->state.paired_counter);
            qdict_put_bool(paired, "pending", t->state.paired_pending);
            qdict_put_bool(paired, "selected", t->state.paired_selected);
            qdict_put_int(paired, "irqs", t->state.paired_irqs);
            qdict_put(timer, "paired", paired);
        }
        if (model->config.i2c_route) {
            qdict_put_int(timer, "i2c_irqs", t->state.i2c_irqs);
            qdict_put_bool(timer, "i2c_selected", t->state.i2c_selected);
        }
        QDict *masks = qdict_new();
        static const char *mask_keys[] = {"4211212548", "4211212804", "4211213060"};
        for (unsigned bank = 0; bank < (model->config.i2c_route ? 3 : 2); bank++) {
            const char *key = mask_keys[bank];
            if (t->state.masks_known[bank]) { qdict_put_int(masks, key, t->state.masks[bank]); } else { qdict_put_null(masks, key); }
        }
        if (model->config.display_route) {
            qdict_put_int(timer, "pdi_irqs", t->state.pdi_irqs); qdict_put_int(timer, "dma_irqs", t->state.dma_irqs);
            qdict_put_bool(timer, "pdi_pending", t->state.pdi_pending); qdict_put_bool(timer, "dma_pending", t->state.dma_pending);
            qdict_put_bool(timer, "pdi_selected", t->state.pdi_selected); qdict_put_bool(timer, "dma_selected", t->state.dma_selected);
        }
        if (model->config.dsp_route) {
            qdict_put_int(timer, "dsp_irqs", t->state.dsp_irqs);
            qdict_put_bool(timer, "dsp_pending", t->state.dsp_pending);
            qdict_put_bool(timer, "dsp_selected", t->state.dsp_selected);
        }
        if (model->config.keypad_route) {
            qdict_put_int(timer, "keypad_irqs", t->state.keypad_irqs);
            qdict_put_bool(timer, "keypad_pending", t->state.keypad_pending);
            qdict_put_bool(timer, "keypad_selected", t->state.keypad_selected);
        }
        if (model->config.rtc_route) {
            qdict_put_int(timer, "rtc_irqs", t->state.rtc_irqs);
            qdict_put_bool(timer, "rtc_pending", t->state.rtc_pending);
            qdict_put_bool(timer, "rtc_selected", t->state.rtc_selected);
        }
        qdict_put(timer, "masks", masks);
        if (model->config.simif_route) {
            qdict_put_int(timer,"sim_rx_irqs",model->state.sim_rx_irqs);
            qdict_put_int(timer,"sim_tx_irqs",model->state.sim_tx_irqs);
        }
        if (model->config.data_uart_route) {
            qdict_put_int(timer, "data_uart_irqs", t->state.data_uart_irqs);
            qdict_put_bool(timer, "data_uart_pending", t->state.data_uart_pending);
            qdict_put_bool(timer, "data_uart_selected", t->state.data_uart_selected);
        }
        if (model->config.bluetooth_route) {
            qdict_put_int(timer, "bluetooth_irqs", t->state.bluetooth_irqs);
            qdict_put_bool(timer, "bluetooth_pending", t->state.bluetooth_pending);
            qdict_put_bool(timer, "bluetooth_selected", t->state.bluetooth_selected);
        }
        qdict_put(snapshot, "timer_irq", timer);
        qdict_put(snapshot, "irq_events", qobject_ref(irq_events));
        if (model->config.unlimited_irqs) {
            QDict *history = qdict_new();
            qdict_put_str(history, "profile", "startup64-recent64-irq512");
            qdict_put_int(history, "irq_events_total", t->state.irqs);
            qdict_put_int(history, "irq_events_dropped", t->state.irqs - qlist_size(irq_events));
            qdict_put_bool(history, "irq_budget_enabled", false);
            qdict_put(snapshot, "recording_history", history);
        }
    }

void dbtimer_set_line(DB2010Timer *timer, DB2010IrqLine line, bool level)
{
    DB2010TimerEvent e = {.kind = DBIRQ_SOURCE, .line = line, .level = level};
    dbtimer_dispatch(timer, &e);
    dbtimer_update_irq(timer);
}
void dbtimer_input(void *opaque, bool level)
{
    DB2010IrqInput *input = opaque;
    dbtimer_set_line(input->timer, input->line, level);
}

const bool *dbtimer_input_latch(const DB2010Timer *timer, DB2010IrqLine line)
{
    switch (line) {
    case DBIRQ_DATA_UART: return &timer->state.data_uart_pending;
    case DBIRQ_BLUETOOTH: return &timer->state.bluetooth_pending;
    case DBIRQ_SIM_RX: return &timer->state.sim_rx_pending;
    case DBIRQ_SIM_TX: return &timer->state.sim_tx_input_level;
    case DBIRQ_KEYPAD: return &timer->state.keypad_pending;
    case DBIRQ_I2C: return &timer->state.i2c_pending;
    case DBIRQ_PDI: return &timer->state.pdi_pending;
    case DBIRQ_DMA: return &timer->state.dma_pending;
    case DBIRQ_DSP: return &timer->state.dsp_pending;
    case DBIRQ_RTC: return &timer->state.rtc_input_level;
    default: g_assert_not_reached();
    }
}
