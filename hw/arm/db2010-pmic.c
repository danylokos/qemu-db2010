#include "qemu/osdep.h"
#include "db2010-i2c.h"

unsigned dbi_write_slot(const DB2010I2c *t, uint64_t index)
{
    index -= t->pmic->history_baseline;
    return t->pmic->config.bounded_history && index >= 64 ? 64 + (index - 64) % 64 : index;
}

unsigned dbi_retained_writes(const DB2010I2c *t)
{
    if (t->pmic->config.omit_diagnostics) { return 0; }
    uint64_t count = t->pmic->state.write_count - t->pmic->history_baseline;
    return t->pmic->config.bounded_history && count > 128 ? 128 : count;
}

uint64_t dbi_write_index(const DB2010I2c *t, unsigned ordinal)
{
    uint64_t count = t->pmic->state.write_count - t->pmic->history_baseline;
    return t->pmic->config.bounded_history && count > 128 && ordinal >= 64 ?
           t->pmic->state.write_count - 64 + ordinal - 64 : t->pmic->history_baseline + ordinal;
}

void dbi_capture_write(DB2010I2c *t, uint8_t reg, uint8_t value)
{
    if (!t->pmic->config.omit_diagnostics) {
        unsigned slot = dbi_write_slot(t, t->pmic->state.write_count);
        t->pmic->state.write_reg[slot] = reg; t->pmic->state.write_value[slot] = value;
    }
    t->pmic->state.write_count++;
    t->pmic->state.last_value[reg] = value; t->pmic->state.last_known[reg] = true;
}

bool dbi_extra_write_known(const DB2010I2c *t, unsigned reg, int value)
{
    /* Original44910544 sub0 packs (request[8] & 0x38) | (request[9] & 3)
     * into logical44/wireA0; sub1 writes zero. Store the software control
     * fields only. No motor waveform, cold value, readback or IRQ is invented. */
    if (reg == 0xa0 && t->pmic->config.vibrator_write_latch) {
        return value < 0 || (value <= 255 && !(value & ~0x3b));
    }
    /* Original44910544 changes36 bits0/1 and5:3,37 bits5:3, preserving
     * the other cached bits. This is a bounded digital storage mock only. */
    if (t->pmic->config.control36_37_write_latches && (reg == 0x36 || reg == 0x37)) {
        unsigned mask = reg == 0x36 ? 0x3b : 0x38;
        if (value < 0) { return true; }
        if (value <= 255 && t->pmic->state.last_known[reg] &&
            !((value ^ t->pmic->state.last_value[reg]) & ~mask)) { return true; }
    }
    if (reg == 0xb1 && t->pmic->config.control_b1_write_latch) { return value < 0 || value <= 255; }
    if (reg == 0x45 && t->pmic->config.control45_write_latch) { return value < 0 || value <= 0x7f; }
    if (reg >= 0xc1 && reg <= 0xd0 && t->pmic->config.audio_write_latch[reg - 0xc1]) {
        return value < 0 || value <= 255;
    }
    for (unsigned n = 0; n < t->pmic->config.extra_write_count; n++) {
        if (t->pmic->config.extra_write_reg[n] == reg &&
            (value < 0 || t->pmic->config.extra_write_value[n] == value)) { return true; }
    }
    return false;
}

int dbi_extra_read_index(const DB2010I2c *t, unsigned reg)
{
    for (unsigned n = 0; n < t->pmic->config.extra_read_count; n++) {
        if (t->pmic->config.extra_read_reg[n] == reg) { return n; }
    }
    return -1;
}

bool dbi_burst_has_next(unsigned reg)
{
    return (reg >= 0x92 && reg < 0x94) || reg == 0xb3;
}

unsigned dbi_followup_index(const DB2010I2c *t)
{
    if (t->pmic->config.repeat_followups && t->pmic->state.followup_cursor >= t->pmic->config.followup_count) {
        return t->pmic->config.followup_repeat_from +
            (t->pmic->state.followup_cursor - t->pmic->config.followup_repeat_from) %
            (t->pmic->config.followup_count - t->pmic->config.followup_repeat_from);
    }
    return t->pmic->state.followup_cursor;
}

const char *dbpmic_polled_read(DB2010I2c *t)
{
    if (t->state.slave != 0x4a) {
        return "i2c_slave_unmodeled";
    }
    if (t->state.reg == 0x23 && t->pmic->config.power_known) {
        t->state.received = t->pmic->config.power_value;
    } else if (t->pmic->config.polled_startup_adc && t->state.reg == 0x71 &&
               t->pmic->state.last_known[0x70] &&
               t->pmic->state.last_value[0x70] == 0x6a) {
        if (!t->pmic->config.polled_adc_known) {
            return "i2c_polled_adc_input_unknown";
        }
        t->state.received = t->pmic->config.polled_adc_value;
    } else {
        return "i2c_slave_register_unmodeled";
    }
    return NULL;
}

const char *dbpmic_polled_write(DB2010I2c *t, int byte)
{
    if (!t->pmic->config.capture_writes || t->state.slave != 0x4a ||
        !((t->state.reg == 0xb0 && byte == 0) ||
          (t->state.reg == 0xb1 && byte == 0xaa) ||
          (t->pmic->config.polled_startup_adc &&
           t->state.reg == 0x70 && byte == 0x6a))) {
        return "i2c_slave_register_write_unmodeled";
    }
    if (!t->pmic->config.bounded_history && !t->pmic->config.omit_diagnostics &&
        t->pmic->state.write_count - t->pmic->history_baseline >= 16) {
        return "i2c_capture_budget";
    }
    dbi_capture_write(t, t->state.reg, byte);
    return NULL;
}

typedef enum DB2010PmicEventKind {
    DBPMIC_EVENT_STAGE,
    DBPMIC_EVENT_TRANSMIT,
    DBPMIC_EVENT_START,
    DBPMIC_EVENT_RECEIVE_ACK,
    DBPMIC_EVENT_STOP,
    DBPMIC_EVENT_CONTROL,
    DBPMIC_EVENT_CLOCK,
    DBPMIC_EVENT_COMPLETE,
} DB2010PmicEventKind;

typedef struct DB2010PmicEvent {
    DB2010PmicEventKind kind;
    unsigned off;
    uint64_t now;
    uint32_t *value;
} DB2010PmicEvent;

static const uint8_t regs[] = {
    0x60, 0x61, 0x40, 0x43, 0x41, 0x42, 0x44, 0x26, 0x27, 0x38,
    0xa1, 0x50, 0xb6, 0x72, 0x73, 0xa0, 0x36, 0x63, 0xc0, 0xb0,
    0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb0, 0x91,
};
static const uint8_t values[] = {
    0x88, 0x13, 0x2a, 0x04, 0x26, 0xa2, 0x0a, 0x08, 0x72, 0x1b,
    0x0c, 0x00, 0x92, 0xf3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x80,
};

/* Selection is independent of wire progress and of ACK/NACK continuation. */
static const char *dbpmic_select_client(DB2010I2c *t, unsigned off,
    uint32_t *value, bool followup,
    unsigned next)
{
    /* An independent client may select the same register as the
     * ordered tail, but write a different explicitly allowed byte.
     * Classify at DATA, when the two transactions first differ;
     * do not consume the ordered operation on its completion. */
    if (followup && !t->pmic->state.extra_active && t->state.peer_step == DBI_PEER_DATA_READY &&
        off == 0 && t->state.staged < 0 && !t->pmic->config.followup_read[next] &&
        *value != t->pmic->config.followup_value[next] &&
        dbi_extra_write_known(t, t->pmic->config.followup_reg[next], *value)) {
        t->pmic->state.extra_active = DBPMIC_INDEPENDENT;
        t->pmic->state.extra_read = DBPMIC_WRITE;
        t->pmic->state.extra_reg = t->pmic->config.followup_reg[next];
    }
    /* A register may support both a bounded read and exact writes.
     * Its following DATA byte selects write;
     * repeated START selects read. */
    if (t->pmic->state.extra_active && t->pmic->state.extra_read && t->state.peer_step == DBI_PEER_DATA_READY &&
        off == 0 && dbi_extra_write_known(t, t->pmic->state.extra_reg, -1)) {
        if (!dbi_extra_write_known(t, t->pmic->state.extra_reg, *value)) {
            return "i2c_extra_write_value_unmodeled";
        }
        t->pmic->state.extra_read = DBPMIC_WRITE;
    }
    return NULL;
}

static const char *dbpmic_receive(DB2010I2c *t, uint64_t now,
    uint32_t *value, bool followup, unsigned next)
{
    if (*value == 0x84 && !dbi_burst_has_next(t->pmic->state.extra_reg)) {
        return "i2c_multibyte_register_unmodeled";
    }
    if (!t->pmic->config.read_power_status ||
        (t->pmic->state.status_reads < 2 && !t->pmic->config.status_input_known[t->pmic->state.status_reads]) ||
        (t->pmic->state.status_reads == 2 && !t->pmic->config.power_known)) {
        return "i2c_async_power_input_unknown";
    }
    if (t->pmic->state.extra_active && t->pmic->state.extra_read) {
        int n = dbi_extra_read_index(t, t->pmic->state.extra_reg);
        if (n < 0 || !t->pmic->config.extra_read_kind[n]) { return "i2c_extra_read_input_unknown"; }
        if (t->pmic->state.extra_reg == 0x71 &&
            t->pmic->config.sampled_battery_adc &&
            t->pmic->state.last_known[0x70] &&
            t->pmic->state.last_value[0x70] == 0xa1) {
            /* Only the original battery sample channel. Other independent
             * samples (including battery identification) retain their inputs. */
            if (!t->pmic->config.sampled_battery_known) { return "i2c_extra_read_input_unknown"; }
            t->state.received = t->pmic->config.sampled_battery_value;
        } else if (t->pmic->config.extra_read_kind[n] == 2) {
            if (!t->pmic->state.last_known[t->pmic->state.extra_reg]) { return "i2c_extra_read_input_unknown"; }
            t->state.received = t->pmic->state.last_value[t->pmic->state.extra_reg];
        } else { t->state.received = t->pmic->config.extra_read_value[n]; }
    } else if (followup) {
        if (!t->pmic->config.followup_known[next]) { return "i2c_followup_input_unknown"; }
        t->state.received = t->pmic->config.followup_value[next];
    } else if (t->pmic->state.status_reads >= 3) {
        unsigned n = t->pmic->state.register_reads;
        if (t->pmic->config.read_kind[n] == 1) {
            t->state.received = t->pmic->config.read_value[n];
        } else if (t->pmic->config.read_kind[n] == 2) {
            /* Explicit last-write latch hypothesis, not physical readback. */
            t->state.received = t->pmic->state.last_known[t->pmic->config.read_reg[n]] ? t->pmic->state.last_value[t->pmic->config.read_reg[n]] : -1;
            if (t->state.received < 0) { return "i2c_async_power_input_unknown"; }
        } else { return "i2c_async_power_input_unknown"; }
    } else {
        t->state.received = t->pmic->state.status_reads < 2 ? t->pmic->config.status_input[t->pmic->state.status_reads] : t->pmic->config.power_value;
    }
    t->pmic->state.extra_receive_more = *value == 0x84 ?
        DBPMIC_MORE_ACK : DBPMIC_FINAL_NACK;
    t->state.control = 0x80;
    t->state.irq_pending = false;
    t->state.peer_step = DBI_PEER_RECEIVE_PENDING;
    dbi_request(t, DBI_RECEIVE, 0, now);
    return NULL;
}

static const char *dbpmic_read_phase(DB2010I2c *t,
    const DB2010PmicEvent *e,
    bool followup, unsigned next, bool *handled)
{
    unsigned off = e->off;
    uint32_t *value = e->value;
    uint64_t now = e->now;

    *handled = true;
    /* Continue a burst before dispatching the same request at RECEIVE_READY. */
    if (t->pmic->state.extra_active && t->pmic->state.extra_read &&
        t->state.peer_step == DBI_PEER_READ_COMPLETE && off == 2 &&
        (*value == 0x84 || *value == 0x80)) {
        if (!t->pmic->state.extra_receive_more ||
            !dbi_burst_has_next(t->pmic->state.extra_reg)) {
            return "i2c_multibyte_continuation_unmodeled";
        }
        t->pmic->state.extra_reg++;
        t->state.peer_step = DBI_PEER_RECEIVE_READY;
    }
    switch (t->state.peer_step) {
    case DBI_PEER_DATA_READY:
        if (e->kind == DBPMIC_EVENT_START) {
            t->state.irq_pending = false;
            t->state.control = 0x84;
            t->state.peer_step = DBI_PEER_RESTART_PENDING;
            dbi_request(t, DBI_START, 0x20, now);
            return NULL;
        }
        break;
    case DBI_PEER_READ_ADDRESS_READY:
        if (off == 0 && t->state.staged < 0 && *value == 0x95) {
            t->state.staged = 0x95;
            return NULL;
        }
        if (e->kind == DBPMIC_EVENT_TRANSMIT && t->state.staged == 0x95) {
            t->state.staged = -1;
            t->state.irq_pending = false;
            t->state.control = 0x80;
            t->state.peer_step = DBI_PEER_READ_ADDRESS_PENDING;
            dbi_request(t, DBI_TRANSMIT, 0, now);
            return NULL;
        }
        break;
    case DBI_PEER_RECEIVE_READY:
        if (e->kind == DBPMIC_EVENT_TRANSMIT ||
            (e->kind == DBPMIC_EVENT_RECEIVE_ACK &&
            t->pmic->state.extra_active && t->pmic->state.extra_read)) {
            return dbpmic_receive(t, now, value, followup, next);
        }
        break;
    case DBI_PEER_READ_COMPLETE:
        if (t->pmic->config.read_power_status && e->kind == DBPMIC_EVENT_STOP) {
            if (t->pmic->state.extra_receive_more) {
                return "i2c_multibyte_stop_before_nack";
            }
            t->state.control = 0x80;
            t->state.irq_pending = false;
            t->state.peer_step = DBI_PEER_READ_STOP_PENDING;
            dbi_request(t, DBI_STOP, 0x10, now);
            return NULL;
        }
        break;
    default:
        break;
    }
    *handled = false;
    return NULL;
}

static unsigned dbpmic_expected_byte(DB2010I2c *t, bool followup,
                                      unsigned next, bool read_status)
{
    switch (t->state.peer_step) {
    case DBI_PEER_ADDRESS_READY:
        return 0x94;
    case DBI_PEER_FIRST_BYTE_READY:
        if (followup) {
            return t->pmic->config.followup_reg[next];
        }
        if (read_status) {
            if (t->pmic->state.status_reads >= 3) {
                return t->pmic->config.read_reg[t->pmic->state.register_reads];
            }
            return 0x21 + (t->pmic->config.read_power_status ?
                           t->pmic->state.status_reads : 0);
        }
        return regs[t->pmic->state.power_writes];
    default:
        return followup ? t->pmic->config.followup_value[next] :
                          values[t->pmic->state.power_writes];
    }
}

static const char *dbpmic_transmit_phase(DB2010I2c *t,
    const DB2010PmicEvent *e,
    bool followup, unsigned next,
    bool read_status)
{
    unsigned off = e->off;
    uint32_t *value = e->value;
    uint64_t now = e->now;

    unsigned expected = dbpmic_expected_byte(t, followup, next, read_status);
    if (t->state.peer_step == DBI_PEER_FIRST_BYTE_READY &&
        t->pmic->state.extra_active) {
        expected = t->pmic->state.extra_reg;
    }
    if (t->state.peer_step == DBI_PEER_DATA_READY && t->pmic->state.extra_active) {
        int data = off == 0 && t->state.staged < 0 ? *value : t->state.staged;
        if (data < 0 || !dbi_extra_write_known(t, t->pmic->state.extra_reg, data)) {
            return "i2c_extra_write_value_unmodeled";
        }
        expected = data;
    }
    if (off == 0 && t->state.staged < 0) {
        if (*value != expected) {
            if (followup && t->state.peer_step == DBI_PEER_FIRST_BYTE_READY &&
                dbi_extra_write_known(t, *value, -1)) {
                t->pmic->state.extra_active = DBPMIC_INDEPENDENT;
                t->pmic->state.extra_read = dbi_extra_read_index(t, *value) >= 0 ?
                    DBPMIC_READ : DBPMIC_WRITE;
                t->pmic->state.extra_reg = *value;
            } else if (followup &&
                       t->state.peer_step == DBI_PEER_FIRST_BYTE_READY &&
                       dbi_extra_read_index(t, *value) >= 0) {
                t->pmic->state.extra_active = DBPMIC_INDEPENDENT;
                t->pmic->state.extra_read = DBPMIC_READ;
                t->pmic->state.extra_reg = *value;
            } else {
                return "i2c_async_power_byte_unmodeled";
            }
        }
        t->state.staged = *value;
        return NULL;
    }
    if (off == 2 && *value == 0x80 && t->state.staged == expected) {
        if (t->state.peer_step == DBI_PEER_DATA_READY) {
            unsigned budget = t->pmic->config.complete_power_init ?
                (t->pmic->config.repeat_followups ?
                 ARRAY_SIZE(t->pmic->state.write_reg) : 64) : 16;
            if (!t->pmic->config.bounded_history && !t->pmic->config.omit_diagnostics &&
                t->pmic->state.write_count - t->pmic->history_baseline >= budget) {
                return "i2c_capture_budget";
            }
            unsigned reg = t->pmic->state.extra_active ? t->pmic->state.extra_reg :
                followup ? t->pmic->config.followup_reg[next] : regs[t->pmic->state.power_writes];
            dbi_capture_write(t, reg, expected);
            t->state.slave = 0x4a;
            t->state.reg = reg;
        }
        t->state.staged = -1;
        t->state.irq_pending = false;
        t->state.control = 0x80;
        t->state.peer_step++;
        dbi_request(t, DBI_TRANSMIT, 0, now);
        return NULL;
    }
    return "i2c_async_power_sequence_unmodeled";
}

static const char *dbpmic_wire_phase(DB2010I2c *t,
    const DB2010PmicEvent *e,
    bool followup, unsigned next,
    bool read_status)
{
    switch (t->state.peer_step) {
    case DBI_PEER_CONTROL_READY:
        if (e->kind == DBPMIC_EVENT_RECEIVE_ACK) {
            t->state.control = 0x84;
            t->state.peer_step = DBI_PEER_CLOCK_READY;
            return NULL;
        }
        break;
    case DBI_PEER_CLOCK_READY:
        if (e->kind == DBPMIC_EVENT_CLOCK && *e->value == 0x60) {
            t->state.clock = 0x60;
            t->state.peer_step = DBI_PEER_START_READY;
            return NULL;
        }
        break;
    case DBI_PEER_START_READY:
        if (e->kind == DBPMIC_EVENT_START) {
            t->state.control = 0x84;
            t->state.peer_step = DBI_PEER_START_PENDING;
            dbi_request(t, DBI_START, 0x20, e->now);
            return NULL;
        }
        break;
    case DBI_PEER_ADDRESS_READY:
    case DBI_PEER_FIRST_BYTE_READY:
        return dbpmic_transmit_phase(t, e, followup, next, read_status);
    case DBI_PEER_DATA_READY:
        if (!read_status) {
            return dbpmic_transmit_phase(t, e, followup, next, read_status);
        }
        break;
    case DBI_PEER_STOP_READY:
        if (e->kind == DBPMIC_EVENT_STOP) {
            t->state.irq_pending = false;
            t->state.control = 0x80;
            t->state.peer_step = DBI_PEER_STOP_PENDING;
            dbi_request(t, DBI_STOP, 0x10, e->now);
            return NULL;
        }
        break;
    default:
        break;
    }
    return "i2c_async_power_sequence_unmodeled";
}

static const char *dbpmic_request(DB2010I2c *t, const DB2010PmicEvent *e)
{
    unsigned limit = t->pmic->config.complete_power_init ? ARRAY_SIZE(regs) : 2;
    bool followup = t->pmic->config.followup_count && t->pmic->state.power_writes >= limit &&
        t->pmic->state.register_reads >= t->pmic->config.register_read_count;
    unsigned next = dbi_followup_index(t);
    if (followup && next >= t->pmic->config.followup_count) {
        return "i2c_followup_next_dependency_unmodeled";
    }
    const char *fault = dbpmic_select_client(t, e->off, e->value, followup, next);
    if (fault) {
        return fault;
    }
    bool read_status = t->pmic->config.complete_power_init && t->pmic->state.power_writes >= limit &&
        (t->pmic->state.extra_active ? t->pmic->state.extra_read : (!followup || t->pmic->config.followup_read[next]));
    if (!followup && t->pmic->state.power_writes >= limit && !read_status) { return "i2c_async_power_next_transaction_unmodeled"; }
    if (!followup && read_status && t->pmic->config.read_power_status && t->pmic->state.status_reads >= 3 &&
        (!t->pmic->config.startup_register_reads || t->pmic->state.register_reads >= t->pmic->config.register_read_count)) {
        return "i2c_async_power_next_dependency_unmodeled";
    }
    if (read_status) {
        bool handled;
        fault = dbpmic_read_phase(t, e, followup, next, &handled);
        if (handled) {
            return fault;
        }
    }
    return dbpmic_wire_phase(t, e, followup, next, read_status);
}

static void dbpmic_read_complete(DB2010I2c *t, bool followup)
{
    if (t->pmic->state.extra_active && t->pmic->state.extra_read) {
        t->pmic->state.extra_active = DBPMIC_ORDERED;
        t->pmic->state.extra_read = DBPMIC_WRITE;
        t->pmic->state.extra_reads++;
    } else if (followup) {
        t->pmic->state.followup_cursor++;
    }
    t->state.status = 0xf8;
    t->state.peer_step = DBI_PEER_CONTROL_READY;
    if (t->pmic->state.status_reads < 3) {
        t->pmic->state.status_reads++;
    } else {
        t->pmic->state.register_reads++;
    }
    t->state.transactions++;
}

static void dbpmic_write_complete(DB2010I2c *t, bool followup)
{
    if (t->pmic->state.extra_active) {
        t->pmic->state.extra_active = DBPMIC_ORDERED;
        t->pmic->state.extra_writes++;
    } else if (followup) {
        t->pmic->state.followup_cursor++;
    }
    t->state.status = 0xf8;
    t->state.phase = DBI_IDLE;
    t->state.peer_step = DBI_PEER_CONTROL_READY;
    t->pmic->state.power_writes++;
    t->state.transactions++;
}

static void dbpmic_complete(DB2010I2c *t)
{
    dbi_complete_async_request(t);
    if (t->fm->state.active && t->state.peer_step == DBI_PEER_STOP_PENDING) {
        dbfr_complete(t->fm);
        t->state.status = 0xf8;
        t->state.phase = DBI_IDLE;
        t->state.peer_step = DBI_PEER_CONTROL_READY;
        t->state.transactions++;
        return;
    }
    bool followup = t->pmic->config.followup_count &&
        t->pmic->state.power_writes >= 27 &&
        t->pmic->state.register_reads >= t->pmic->config.register_read_count;
    switch (t->state.peer_step) {
    case DBI_PEER_READ_STOP_PENDING:
        dbpmic_read_complete(t, followup);
        break;
    case DBI_PEER_STOP_PENDING:
        dbpmic_write_complete(t, followup);
        break;
    default:
        switch (t->state.peer_step) {
        case DBI_PEER_START_PENDING:
            t->state.status = 0x08;
            break;
        case DBI_PEER_ADDRESS_PENDING:
            t->state.status = 0x18;
            break;
        case DBI_PEER_RESTART_PENDING:
            t->state.status = 0x10;
            break;
        case DBI_PEER_READ_ADDRESS_PENDING:
            t->state.status = 0x40;
            break;
        case DBI_PEER_RECEIVE_PENDING:
            t->state.status = t->pmic->state.extra_receive_more ? 0x50 : 0x58;
            break;
        default:
            t->state.status = 0x28;
            break;
        }
        t->state.peer_step++;
        t->state.control |= 8;
        t->state.irq_pending = true;
        break;
    }
}

static const char *dbpmic_dispatch(DB2010I2c *t, const DB2010PmicEvent *e)
{
    if (e->kind == DBPMIC_EVENT_COMPLETE) {
        dbpmic_complete(t);
        return NULL;
    }
    return dbpmic_request(t, e);
}

const char *dbpmic_async_access(DB2010I2c *t, unsigned off, uint64_t now,
    uint32_t *value)
{
    bool handled;
    const char *fault = dbfr_transport(t, off, now, value, &handled);
    DB2010PmicEvent e = {.off = off, .now = now, .value = value};

    if (handled) {
        return fault;
    }
    e.kind = off == 0 ? DBPMIC_EVENT_STAGE :
        off == 4 ? DBPMIC_EVENT_CLOCK :
        off != 2 ? DBPMIC_EVENT_CONTROL :
        *value == 0xa4 ? DBPMIC_EVENT_START :
        *value == 0x80 ? DBPMIC_EVENT_TRANSMIT :
        *value == 0x84 ? DBPMIC_EVENT_RECEIVE_ACK :
        *value == 0x90 ? DBPMIC_EVENT_STOP : DBPMIC_EVENT_CONTROL;
    return dbpmic_dispatch(t, &e);
}

void dbpmic_async_finish(DB2010I2c *t)
{
    DB2010PmicEvent e = {.kind = DBPMIC_EVENT_COMPLETE};

    dbpmic_dispatch(t, &e);
}

void dbpmic_reset(DB2010Pmic *pmic)
{
    pmic->state = (DB2010PmicRuntime){0};
    pmic->history_baseline = 0;
}
