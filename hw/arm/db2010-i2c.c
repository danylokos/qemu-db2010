/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-i2c.h"

typedef enum DB2010I2cEventKind {
    DBI_EVENT_READ,
    DBI_EVENT_STAGE_DATA,
    DBI_EVENT_WRITE_CLOCK,
    DBI_EVENT_REQUEST_CONTROL,
    DBI_EVENT_COMPLETE,
    DBI_EVENT_IRQ_CONSUMED,
} DB2010I2cEventKind;

typedef struct DB2010I2cEvent {
    DB2010I2cEventKind kind;
    unsigned off;
    uint64_t now;
    uint32_t *value;
} DB2010I2cEvent;

void dbi_transport_reset(DB2010I2c *t)
{
    t->state = (DB2010I2cRuntime){
        .staged = -1, .received = -1, .slave = -1, .reg = -1,
    };
}

void dbi_reset(DB2010I2c *t)
{
    dbi_transport_reset(t);
    dbpmic_reset(t->pmic);
    dbfr_reset(t->fm);
}

void dbi_request(DB2010I2c *t, DB2010I2cOperation operation,
                 unsigned mask, uint64_t now)
{
    t->state.pending = operation;
    t->state.mask = mask;
    t->state.due = now + 128;
    t->state.control |= mask;
}

void dbi_begin(DB2010I2c *t, unsigned operation, unsigned mask, uint64_t now)
{
    dbi_request(t, operation, mask, now);
}

void dbi_complete_async_request(DB2010I2c *t)
{
    t->state.control &= ~t->state.mask;
    t->state.pending = DBI_NONE;
    t->state.status_known = true;
    t->state.async_completions++;
}

static void dbi_async_init_complete(DB2010I2c *t)
{
    dbi_complete_async_request(t);
    switch (t->state.async_phase) {
    case DBI_ASYNC_START_PENDING:
    case DBI_ASYNC_BYTE_PENDING:
        t->state.status = t->state.async_phase == DBI_ASYNC_START_PENDING ?
                          0x08 : 0x48;
        t->state.async_phase++;
        t->state.control |= 8;
        t->state.irq_pending = true;
        break;
    case DBI_ASYNC_STOP_PENDING:
        t->state.status = 0xf8;
        t->state.async_phase = DBI_ASYNC_PEER_READY;
        t->state.phase = DBI_IDLE;
        t->state.transactions++;
        break;
    default:
        break;
    }
}

static void dbi_polled_complete(DB2010I2c *t)
{
    t->state.control &= ~t->state.mask;
    switch (t->state.pending) {
    case DBI_TRANSMIT:
    case DBI_RECEIVE:
        t->state.control |= 8;
        break;
    case DBI_STOP:
        t->state.phase = DBI_IDLE;
        t->state.transactions++;
        break;
    default:
        break;
    }
    t->state.pending = DBI_NONE;
}

static void dbi_complete(DB2010I2c *t, uint64_t now)
{
    if (!t->state.pending || now < t->state.due) {
        return;
    }
    if (t->pmic->config.capture_async_power &&
        t->state.async_phase == DBI_ASYNC_PEER_READY && t->state.peer_step >= DBI_PEER_START_PENDING) {
        dbpmic_async_finish(t);
    } else if (t->config.complete_async_init &&
               t->state.async_init_step == DBI_INIT_READY) {
        dbi_async_init_complete(t);
    } else {
        dbi_polled_complete(t);
    }
}

static const char *dbi_async_start_ready(DB2010I2c *t,
                                        const DB2010I2cEvent *e)
{
    if (e->off != 2 || *e->value != 0xa4) {
        return "i2c_async_init_sequence_unmodeled";
    }
    t->state.control = 0x84;
    t->state.async_phase = DBI_ASYNC_START_PENDING;
    dbi_request(t, DBI_START, 0x20, e->now);
    return NULL;
}

static const char *dbi_async_byte_ready(DB2010I2c *t,
                                       const DB2010I2cEvent *e)
{
    if (e->off == 0 && *e->value == 1 && t->state.staged < 0) {
        t->state.staged = 1;
        return NULL;
    }
    if (e->off == 2 && *e->value == 0x80 && t->state.staged == 1) {
        t->state.staged = -1;
        t->state.irq_pending = false;
        t->state.control = 0x80;
        t->state.async_phase = DBI_ASYNC_BYTE_PENDING;
        dbi_request(t, DBI_TRANSMIT, 0, e->now);
        return NULL;
    }
    return "i2c_async_init_sequence_unmodeled";
}

static const char *dbi_async_stop_ready(DB2010I2c *t,
                                       const DB2010I2cEvent *e)
{
    if (e->off != 2 || *e->value != 0x90) {
        return "i2c_async_init_sequence_unmodeled";
    }
    t->state.irq_pending = false;
    t->state.control = 0x80;
    t->state.async_phase = DBI_ASYNC_STOP_PENDING;
    dbi_request(t, DBI_STOP, 0x10, e->now);
    return NULL;
}

static const char *dbi_async_access(DB2010I2c *t, const DB2010I2cEvent *e)
{
    if (e->kind == DBI_EVENT_READ) {
        if (e->off == 0 && t->pmic->config.read_power_status &&
            t->state.peer_step == DBI_PEER_RECEIVED && t->state.received >= 0) {
            *e->value = t->state.received;
            t->state.peer_step = DBI_PEER_READ_COMPLETE;
            return NULL;
        }
        if (e->off == 2) {
            *e->value = t->state.control;
            return NULL;
        }
        if (e->off == 6 && t->state.status_known) {
            *e->value = t->state.status;
            return NULL;
        }
        return "i2c_async_read_unmodeled";
    }
    if (t->state.pending) {
        return "i2c_command_while_busy";
    }
    if (t->pmic->config.capture_async_power &&
        t->state.async_phase == DBI_ASYNC_PEER_READY) {
        return dbpmic_async_access(t, e->off, e->now, e->value);
    }
    switch (t->state.async_phase) {
    case DBI_ASYNC_START_READY:
        return dbi_async_start_ready(t, e);
    case DBI_ASYNC_BYTE_READY:
        return dbi_async_byte_ready(t, e);
    case DBI_ASYNC_STOP_READY:
        return dbi_async_stop_ready(t, e);
    case DBI_ASYNC_PEER_READY:
        return "i2c_async_next_transaction_unmodeled";
    default:
        return "i2c_async_init_sequence_unmodeled";
    }
}

static const char *dbi_async_setup(DB2010I2c *t, const DB2010I2cEvent *e)
{
    if (e->kind == DBI_EVENT_READ || t->state.pending) {
        return "i2c_async_init_sequence_unmodeled";
    }
    switch (t->state.async_init_step) {
    case DBI_INIT_CONTROL:
        if (e->off == 2 && *e->value == 0x84) {
            t->state.control = *e->value;
            t->state.async_init_step = DBI_INIT_CLOCK;
            return NULL;
        }
        break;
    case DBI_INIT_CLOCK:
        if (e->off == 4 && *e->value == 0x60) {
            t->state.clock = *e->value;
            t->state.async_init_step = DBI_INIT_READY;
            return NULL;
        }
        break;
    case DBI_INIT_READY:
        if (e->off == 2 && *e->value == 0xa4) {
            return "i2c_async_start_unmodeled";
        }
        break;
    default:
        break;
    }
    return "i2c_async_init_sequence_unmodeled";
}

static const char *dbi_polled_write_address(DB2010I2c *t, int byte)
{
    if (byte != 0x94) {
        return "i2c_slave_unmodeled";
    }
    t->state.slave = byte >> 1;
    t->state.phase = DBI_REGISTER;
    return NULL;
}

static const char *dbi_polled_register(DB2010I2c *t, int byte)
{
    t->state.reg = byte;
    t->state.phase = DBI_SELECTED;
    return NULL;
}

static const char *dbi_polled_read_address(DB2010I2c *t, int byte)
{
    if (byte != 0x95 || (byte >> 1) != t->state.slave) {
        return "i2c_read_address_unmodeled";
    }
    t->state.phase = DBI_RECEIVE_READY;
    return NULL;
}

static const char *dbi_polled_receive_ready(DB2010I2c *t, uint64_t now)
{
    const char *fault = dbpmic_polled_read(t);

    if (fault) {
        return fault;
    }
    t->state.phase = DBI_RECEIVED;
    t->state.control = 0;
    dbi_request(t, DBI_RECEIVE, 0, now);
    return NULL;
}

static const char *dbi_polled_selected(DB2010I2c *t, int byte)
{
    const char *fault = dbpmic_polled_write(t, byte);

    if (fault) {
        return fault;
    }
    t->state.phase = DBI_WRITE_COMPLETE;
    return NULL;
}

static const char *dbi_polled_transfer(DB2010I2c *t, uint64_t now)
{
    int byte = t->state.staged;
    const char *fault;

    if (t->state.phase == DBI_RECEIVE_READY && byte < 0) {
        return dbi_polled_receive_ready(t, now);
    }
    if (byte < 0) {
        return "i2c_transfer_sequence_unmodeled";
    }
    switch (t->state.phase) {
    case DBI_WRITE_ADDRESS:
        fault = dbi_polled_write_address(t, byte);
        break;
    case DBI_REGISTER:
        fault = dbi_polled_register(t, byte);
        break;
    case DBI_READ_ADDRESS:
        fault = dbi_polled_read_address(t, byte);
        break;
    case DBI_SELECTED:
        fault = dbi_polled_selected(t, byte);
        break;
    default:
        return "i2c_transfer_sequence_unmodeled";
    }
    if (fault) {
        return fault;
    }
    /* Retain the original post-transition wire-budget check. */
    if (t->state.wire_size >= sizeof(t->state.wire)) {
        return "i2c_wire_budget";
    }
    t->state.wire[t->state.wire_size++] = byte;
    t->state.staged = -1;
    t->state.control = 0;
    dbi_request(t, DBI_TRANSMIT, 0, now);
    return NULL;
}

static const char *dbi_polled_control(DB2010I2c *t,
                                     const DB2010I2cEvent *e)
{
    uint32_t value = *e->value;

    if (value == 4) {
        t->state.control = 4;
        t->state.control_known = true;
        t->state.phase = DBI_CONFIGURED;
        t->state.pending = DBI_NONE;
        t->state.staged = t->state.received = t->state.slave = t->state.reg = -1;
        t->state.wire_size = 0;
        return NULL;
    }
    if (!t->state.control_known || !t->state.clock_known) {
        return "i2c_initial_state_unknown";
    }
    if (t->state.pending) {
        return "i2c_command_while_busy";
    }
    if (value == 0x24) {
        if (t->state.staged >= 0 ||
            (t->state.phase != DBI_IDLE && t->state.phase != DBI_SELECTED)) {
            return "i2c_start_sequence_unmodeled";
        }
        t->state.phase = t->state.phase == DBI_IDLE ?
                         DBI_WRITE_ADDRESS : DBI_READ_ADDRESS;
        t->state.control = 4;
        dbi_request(t, DBI_START, 0x20, e->now);
        return NULL;
    }
    if (value == 0x10) {
        if (t->state.phase != DBI_READ_COMPLETE &&
            t->state.phase != DBI_WRITE_COMPLETE) {
            return "i2c_stop_sequence_unmodeled";
        }
        t->state.control = 0;
        dbi_request(t, DBI_STOP, 0x10, e->now);
        return NULL;
    }
    if (value) {
        return "i2c_control_value_unmodeled";
    }
    if (t->config.capture_async_init && t->state.phase == DBI_IDLE &&
        t->state.staged == 0) {
        t->state.staged = -1;
        t->state.control = 0;
        t->state.async_init_step = DBI_INIT_CONTROL;
        return NULL;
    }
    if (t->state.phase == DBI_CONFIGURED && t->state.staged == 0) {
        t->state.staged = -1;
        t->state.phase = DBI_IDLE;
        t->state.control = 0;
        return NULL;
    }
    return dbi_polled_transfer(t, e->now);
}

static const char *dbi_polled_access(DB2010I2c *t, const DB2010I2cEvent *e)
{
    switch (e->kind) {
    case DBI_EVENT_READ:
        if (e->off == 2 || e->off == 4) {
            if (!(e->off == 2 ? t->state.control_known : t->state.clock_known)) {
                return "i2c_initial_state_unknown";
            }
            *e->value = e->off == 2 ? t->state.control : t->state.clock;
            return NULL;
        }
        if (t->state.phase != DBI_RECEIVED || t->state.pending ||
            t->state.received < 0) {
            return "i2c_data_read_unmodeled";
        }
        t->state.phase = DBI_READ_COMPLETE;
        *e->value = t->state.received;
        return NULL;
    case DBI_EVENT_WRITE_CLOCK:
        if (*e->value != 0x60) {
            return "i2c_clock_value_unmodeled";
        }
        t->state.clock = *e->value;
        t->state.clock_known = true;
        return NULL;
    case DBI_EVENT_STAGE_DATA:
        if (*e->value > 255 || t->state.staged >= 0 || t->state.pending) {
            return "i2c_data_write_unmodeled";
        }
        t->state.staged = *e->value;
        return NULL;
    case DBI_EVENT_REQUEST_CONTROL:
        return dbi_polled_control(t, e);
    default:
        g_assert_not_reached();
    }
}

static const char *dbi_dispatch(DB2010I2c *t, const DB2010I2cEvent *e)
{
    if (e->kind == DBI_EVENT_COMPLETE) {
        dbi_complete(t, e->now);
        return NULL;
    }
    if (e->kind == DBI_EVENT_IRQ_CONSUMED) {
        t->state.irq_pending = false;
        return NULL;
    }
    /* Async setup follows the polled transactions without resetting peers. */
    if (t->config.capture_async_init && t->state.async_init_step) {
        if (t->config.complete_async_init &&
            t->state.async_init_step == DBI_INIT_READY) {
            return dbi_async_access(t, e);
        }
        return dbi_async_setup(t, e);
    }
    return dbi_polled_access(t, e);
}

void dbi_finish(DB2010I2c *t, uint64_t now)
{
    DB2010I2cEvent e = {.kind = DBI_EVENT_COMPLETE, .now = now};

    dbi_dispatch(t, &e);
}

static const char *i2c_async_port_guard(const void *opaque,
    const DB2010DecodedAccess *d, const DB2010Access *a)
{
    const DB2010I2c *t = opaque;
    return t->config.complete_async_init ? NULL : "i2c_register_access_unmodeled";
}
static const char *i2c_port_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010I2c *t = opaque;
    DB2010I2cEvent e = {.off = d->id, .now = a->now, .value = &a->value};
    dbi_finish(t, a->now);
    if (a->write) { a->value &= 0xffff; }
    /* Offset six deliberately retains the legacy polled fallback. */
    e.kind = !a->write ? DBI_EVENT_READ :
             d->id == 0 ? DBI_EVENT_STAGE_DATA :
             d->id == 4 ? DBI_EVENT_WRITE_CLOCK : DBI_EVENT_REQUEST_CONTROL;
    return dbi_dispatch(t, &e);
}
const DB2010RegMap *dbi_map(void)
{
#define I2C_PORT(off, guard_fn) DB_REG(off, .id = off, .alignment = 2, .backing_width = 4, \
    .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .guard = guard_fn, .callback = i2c_port_action}, \
    .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .guard = guard_fn, .callback = i2c_port_action})
    static const DB2010RegRule children[] = {
        I2C_PORT(0, NULL), I2C_PORT(2, NULL), I2C_PORT(4, NULL),
        I2C_PORT(6, i2c_async_port_guard),
    };
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 4096, .children = children, .child_count = ARRAY_SIZE(children)),
    };
#undef I2C_PORT
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf9040000, rules,
            ARRAY_SIZE(rules), "i2c_register_access_unmodeled");
        if (fault) { g_error("DB2010 I2C layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *dbi_access(DB2010I2c *t, uint32_t address, unsigned size,
                       uint64_t now, bool write, uint32_t *value)
{
    DB2010Access a = {.address = address, .size = size, .now = now, .write = write,
                      .value = *value, .commit = true};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbi_map(), &a, &d);
    if (!fault) { fault = db2010_reg_execute(t, &d, &a); }
    *value = a.value;
    return fault;
}

const char *dbi_bus_access(void *opaque, DB2010Access *a)
{
    DB2010I2c *i2c = opaque, preview = *i2c;
    DB2010Pmic pmic = *i2c->pmic;
    DB2010FmRadio fm = *i2c->fm;
    preview.pmic = &pmic; preview.fm = &fm;
    return dbi_access(a->commit ? i2c : &preview, a->address, a->size,
                      a->now, a->write, &a->value);
}

void dbi_export(DB2010I2c *model, QDict *snapshot)
{

        if (model->fm->config.enabled) {
            DB2010FmRadio *f = model->fm;
            QDict *fm = qdict_new(); char latest[2 * DBFR_MAX_BYTES + 1];
            char pending[2 * DBFR_MAX_BYTES + 1];
            for (unsigned i = 0; i < f->state.latest_size; i++) { snprintf(latest+2*i, 3, "%02x", f->state.latest[i]); }
            for (unsigned i = 0; i < f->state.used; i++) { snprintf(pending+2*i, 3, "%02x", f->state.pending[i]); }
            latest[2*f->state.latest_size] = pending[2*f->state.used] = 0;
            qdict_put_str(fm, "profile", "i2c-frame-sink");
            qdict_put_int(fm, "wire_write_address", 0x20);
            qdict_put_int(fm, "declared_frames", f->config.count);
            qdict_put_int(fm, "completed_frames", f->state.frames);
            qdict_put_int(fm, "completed_bytes", f->state.bytes);
            qdict_put_bool(fm, "active", f->state.active);
            qdict_put_str(fm, "latest_frame_hex", latest);
            qdict_put_str(fm, "pending_frame_hex", pending);
            qdict_put(snapshot, "fm_radio", fm);
        }
        QDict *i2c = qdict_new();
        if (model->pmic->config.vibrator_write_latch) { qdict_put_bool(i2c,"vibrator_write_latch",true); }
        if (model->pmic->config.control_b1_write_latch) { qdict_put_bool(i2c,"control_b1_write_latch",true); }
        if (model->pmic->config.control36_37_write_latches) { qdict_put_bool(i2c,"control36_37_write_latches",true); }
        if (model->pmic->config.control45_write_latch) { qdict_put_bool(i2c,"control45_write_latch",true); }
        QList *audio_latches = qlist_new();
        for (unsigned r = 0; r < 16; r++) {
            if (model->pmic->config.audio_write_latch[r]) { qlist_append_int(audio_latches, 0xc1 + r); }
        }
        if (!qlist_empty(audio_latches)) { qdict_put(i2c, "audio_write_latches", audio_latches); }
        else { qobject_unref(audio_latches); }
        static const char *phases[] = {"uninitialized", "configured", "idle", "write-address", "register", "register-selected", "read-address", "receive-ready", "received", "read-complete", "write-complete"};
        qdict_put_str(i2c, "phase", phases[model->state.phase]);
        if (model->state.control_known) { qdict_put_int(i2c, "control", model->state.control); } else { qdict_put_null(i2c, "control"); }
        if (model->state.clock_known) { qdict_put_int(i2c, "clock", model->state.clock); } else { qdict_put_null(i2c, "clock"); }
        qdict_put_int(i2c, "transactions", model->state.transactions);
        qdict_put_int(i2c, "received", model->state.received);
        qdict_put_int(i2c, "pending_operation", model->state.pending);
        qdict_put_int(i2c, "deadline_ns", model->state.due);
        qdict_put_int(i2c, "staged", model->state.staged);
        qdict_put_int(i2c, "independent_register", model->pmic->state.extra_reg);
        qdict_put_int(i2c, "slave", model->state.slave); qdict_put_int(i2c, "register", model->state.reg);
        if (model->config.capture_async_init) {
            qdict_put_int(i2c, "async_init_step", model->state.async_init_step);
        }
        if (model->config.complete_async_init) {
            qdict_put_int(i2c, "async_phase", model->state.async_phase);
            qdict_put_int(i2c, "async_completions", model->state.async_completions);
            qdict_put_bool(i2c, "irq_pending", model->state.irq_pending);
            if (model->state.status_known) { qdict_put_int(i2c, "bus_state", model->state.status); }
            else { qdict_put_null(i2c, "bus_state"); }
        }
        if (model->pmic->config.capture_async_power) {
            qdict_put_int(i2c, "power_step", model->state.peer_step);
            qdict_put_int(i2c, "power_writes", model->pmic->state.power_writes);
            if (model->pmic->config.extra_write_count) {
                qdict_put_int(i2c, "extra_write_declared", model->pmic->config.extra_write_count);
                qdict_put_int(i2c, "extra_writes_completed", model->pmic->state.extra_writes);
                qdict_put_bool(i2c, "extra_write_active", model->pmic->state.extra_active && !model->pmic->state.extra_read);
            }
            if (model->pmic->config.extra_read_count) {
                qdict_put_int(i2c, "extra_reads_completed", model->pmic->state.extra_reads);
                qdict_put_bool(i2c, "extra_read_active", model->pmic->state.extra_active && model->pmic->state.extra_read);
                QList *reads = qlist_new();
                for (unsigned n = 0; n < model->pmic->config.extra_read_count; n++) {
                    QDict *read = qdict_new(); qdict_put_int(read, "register", model->pmic->config.extra_read_reg[n]);
                    if (!model->pmic->config.extra_read_kind[n]) { qdict_put_null(read, "value"); }
                    else if (model->pmic->config.extra_read_kind[n] == 2) { qdict_put_str(read, "value", "last-write"); }
                    else { qdict_put_int(read, "value", model->pmic->config.extra_read_value[n]); }
                    qlist_append(reads, read);
                }
                qdict_put(i2c, "extra_read_inputs", reads);
            }
            if (model->pmic->config.followup_count) {
                qdict_put_int(i2c, "post_startup_completed", model->pmic->state.followup_cursor);
                qdict_put_int(i2c, "post_startup_declared", model->pmic->config.followup_count);
                if (model->pmic->config.repeat_followups) {
                    qdict_put_int(i2c, "post_startup_repeat_from", model->pmic->config.followup_repeat_from);
                    qdict_put_int(i2c, "post_startup_next_index", dbi_followup_index(model));
                }
            }
        }
        if (model->pmic->config.read_power_status) {
            qdict_put_int(i2c, "status_reads", model->pmic->state.status_reads);
            QList *inputs = qlist_new();
            for (unsigned n = 0; n < 2; n++) {
                if (model->pmic->config.status_input_known[n]) qlist_append_int(inputs, model->pmic->config.status_input[n]);
                else qlist_append_null(inputs);
            }
            qdict_put(i2c, "status_inputs", inputs);
            if (model->pmic->config.polled_startup_adc) {
                if (model->pmic->config.polled_adc_known) {
                    qdict_put_int(i2c, "polled_startup_adc", model->pmic->config.polled_adc_value);
                } else { qdict_put_null(i2c, "polled_startup_adc"); }
            }
            if (model->pmic->config.sampled_battery_adc) {
                if (model->pmic->config.sampled_battery_known) {
                    qdict_put_int(i2c, "sampled_battery_adc", model->pmic->config.sampled_battery_value);
                } else { qdict_put_null(i2c, "sampled_battery_adc"); }
            }
        }
        if (model->pmic->config.startup_register_reads) {
            qdict_put_int(i2c, "register_reads", model->pmic->state.register_reads);
            QList *reads = qlist_new();
            for (unsigned n = 0; n < model->pmic->config.register_read_count; n++) {
                QDict *read = qdict_new(); qdict_put_int(read, "register", model->pmic->config.read_reg[n]);
                if (model->pmic->config.read_kind[n] == 2) { qdict_put_str(read, "value", "last-write"); }
                else if (model->pmic->config.read_kind[n] == 1) { qdict_put_int(read, "value", model->pmic->config.read_value[n]); }
                else { qdict_put_null(read, "value"); }
                qlist_append(reads, read);
            }
            qdict_put(i2c, "register_read_inputs", reads);
        }
        QList *writes = qlist_new();
        for (unsigned i = 0; i < dbi_retained_writes(model); i++) {
            uint64_t index = dbi_write_index(model, i);
            unsigned slot = dbi_write_slot(model, index);
            QDict *w = qdict_new(); qdict_put_int(w, "slave", 0x4a);
            qdict_put_int(w, "register", model->pmic->state.write_reg[slot]); qdict_put_int(w, "value", model->pmic->state.write_value[slot]);
            if (model->pmic->config.bounded_history) { qdict_put_int(w, "sequence", index + 1); }
            qlist_append(writes, w);
        }
        if (model->pmic->config.bounded_history) {
            qdict_put_int(i2c, "captured_writes_total", model->pmic->state.write_count);
            qdict_put_int(i2c, "captured_writes_dropped", model->pmic->state.write_count - dbi_retained_writes(model));
            QDict *cache = qdict_new();
            for (unsigned reg = 0; reg < 256; reg++) {
                if (model->pmic->state.last_known[reg]) {
                    g_autofree char *key = g_strdup_printf("%02x", reg);
                    qdict_put_int(cache, key, model->pmic->state.last_value[reg]);
                }
            }
            qdict_put(i2c, "last_write_registers", cache);
        }
        qdict_put(i2c, "captured_writes", writes); qdict_put(snapshot, "i2c", i2c);
    }

void dbi_irq_consumed(DB2010I2c *i2c)
{
    DB2010I2cEvent e = {.kind = DBI_EVENT_IRQ_CONSUMED};

    dbi_dispatch(i2c, &e);
}
