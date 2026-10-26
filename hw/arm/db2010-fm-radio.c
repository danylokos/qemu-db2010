/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-fm-radio.h"
#include "db2010-i2c.h"

bool dbfr_byte_known(const DB2010FmRadio *f, uint8_t value)
{
    for (unsigned i = 0; i < f->config.count; i++) {
        if (f->state.used < f->config.size[i] &&
            !memcmp(f->state.pending, f->config.allowed[i], f->state.used) &&
            f->config.allowed[i][f->state.used] == value) { return true; }
    }
    return false;
}

bool dbfr_frame_known(const DB2010FmRadio *f)
{
    for (unsigned i = 0; i < f->config.count; i++) {
        if (f->state.used == f->config.size[i] &&
            !memcmp(f->state.pending, f->config.allowed[i], f->state.used)) { return true; }
    }
    return false;
}

typedef enum DB2010FmEventKind {
    DBFR_EVENT_SELECT,
    DBFR_EVENT_READ_ADDRESS,
    DBFR_EVENT_STAGE,
    DBFR_EVENT_TRANSMIT,
    DBFR_EVENT_STOP,
    DBFR_EVENT_COMPLETE,
    DBFR_EVENT_UNSUPPORTED,
} DB2010FmEventKind;

typedef struct DB2010FmEvent {
    DB2010FmEventKind kind;
    uint32_t value;
    uint64_t now;
} DB2010FmEvent;

static void dbfr_frame_complete(DB2010FmRadio *f)
{
    memcpy(f->state.latest, f->state.pending, f->state.used);
    f->state.latest_size = f->state.used;
    f->state.frames++;
    f->state.bytes += f->state.used;
    f->state.active = DBFR_IDLE;
    f->state.used = 0;
}

static const char *dbfr_select(DB2010FmRadio *f, DB2010I2c *t)
{
    f->state.active = DBFR_SELECTED;
    f->state.used = 0;
    t->state.staged = 0x20;
    return NULL;
}

static const char *dbfr_address_ready(DB2010I2c *t,
                                      const DB2010FmEvent *e)
{
    if (e->kind != DBFR_EVENT_TRANSMIT || t->state.staged != 0x20) {
        return "fm_i2c_sequence_unmodeled";
    }
    t->state.staged = -1;
    t->state.irq_pending = false;
    t->state.control = 0x80;
    t->state.peer_step = DBI_PEER_ADDRESS_PENDING;
    dbi_request(t, DBI_TRANSMIT, 0, e->now);
    return NULL;
}

static const char *dbfr_payload_ready(DB2010FmRadio *f, DB2010I2c *t,
                                      const DB2010FmEvent *e)
{
    switch (e->kind) {
    case DBFR_EVENT_STAGE:
        if (e->value > 255 || !dbfr_byte_known(f, e->value)) {
            return "fm_i2c_payload_unmodeled";
        }
        t->state.staged = e->value;
        return NULL;
    case DBFR_EVENT_TRANSMIT:
        f->state.pending[f->state.used++] = t->state.staged;
        t->state.staged = -1;
        t->state.irq_pending = false;
        t->state.control = 0x80;
        t->state.peer_step = DBI_PEER_BYTE_PENDING;
        dbi_request(t, DBI_TRANSMIT, 0, e->now);
        return NULL;
    case DBFR_EVENT_STOP:
        if (!dbfr_frame_known(f)) {
            return "fm_i2c_short_frame_unmodeled";
        }
        t->state.irq_pending = false;
        t->state.control = 0x80;
        t->state.peer_step = DBI_PEER_STOP_PENDING;
        dbi_request(t, DBI_STOP, 0x10, e->now);
        return NULL;
    default:
        return "fm_i2c_sequence_unmodeled";
    }
}

static const char *dbfr_selected(DB2010FmRadio *f, DB2010I2c *t,
                                 const DB2010FmEvent *e)
{
    switch (t->state.peer_step) {
    case DBI_PEER_ADDRESS_READY:
        return dbfr_address_ready(t, e);
    case DBI_PEER_FIRST_BYTE_READY:
    case DBI_PEER_DATA_READY:
        return dbfr_payload_ready(f, t, e);
    default:
        return "fm_i2c_sequence_unmodeled";
    }
}

static const char *dbfr_dispatch(DB2010FmRadio *f, DB2010I2c *t,
                                 const DB2010FmEvent *e)
{
    switch (e->kind) {
    case DBFR_EVENT_COMPLETE:
        dbfr_frame_complete(f);
        return NULL;
    case DBFR_EVENT_READ_ADDRESS:
        return "fm_i2c_read_unmodeled";
    case DBFR_EVENT_SELECT:
        return dbfr_select(f, t);
    default:
        switch (f->state.active) {
        case DBFR_SELECTED:
            return dbfr_selected(f, t, e);
        case DBFR_IDLE:
            return NULL; /* The transport adapter declines ownership. */
        default:
            g_assert_not_reached();
        }
    }
}

void dbfr_complete(DB2010FmRadio *f)
{
    DB2010FmEvent e = {.kind = DBFR_EVENT_COMPLETE};

    dbfr_dispatch(f, NULL, &e);
}

void dbfr_reset(DB2010FmRadio *d)
{
    d->state = (DB2010FmRadioRuntime){0};
}

const char *dbfr_transport(DB2010I2c *t, unsigned off, uint64_t now,
                           uint32_t *value, bool *handled)
{
    DB2010FmEvent e = {.kind = DBFR_EVENT_UNSUPPORTED,
                       .value = *value, .now = now};

    /* Selection precedes active-peer dispatch, including read-address rejection. */
    if (t->fm->config.enabled && t->state.peer_step == DBI_PEER_ADDRESS_READY &&
        off == 0 && t->state.staged < 0 && (*value == 0x20 || *value == 0x21)) {
        e.kind = *value == 0x20 ? DBFR_EVENT_SELECT : DBFR_EVENT_READ_ADDRESS;
    } else if (!t->fm->state.active) {
        *handled = false;
        return NULL;
    } else if (off == 0 && t->state.staged < 0) {
        e.kind = DBFR_EVENT_STAGE;
    } else if (off == 2 && *value == 0x80 && t->state.staged >= 0) {
        e.kind = DBFR_EVENT_TRANSMIT;
    } else if (off == 2 && *value == 0x90 && t->state.staged < 0) {
        e.kind = DBFR_EVENT_STOP;
    }
    *handled = true;
    return dbfr_dispatch(t->fm, t, &e);
}
