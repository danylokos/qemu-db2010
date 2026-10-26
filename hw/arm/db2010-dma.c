/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-dma.h"

typedef enum DB2010DmaRequestState { DBDMA_DISABLED = 0, DBDMA_ENABLED = 1 } DB2010DmaRequestState;
typedef enum DB2010DmaEventKind {
    DBDMA_READ_CHANNEL, DBDMA_STAGE_CHANNEL, DBDMA_ADVANCE_SOURCE,
    DBDMA_TRANSFER_WORD, DBDMA_COMPLETE,
} DB2010DmaEventKind;
typedef struct DB2010DmaEvent {
    DB2010DmaEventKind kind;
    unsigned channel, field, bytes;
    DB2010Access *access;
} DB2010DmaEvent;
static const char *dma_configure(DB2010Dma *d, const DB2010DmaEvent *e)
{
    unsigned channel = e->channel;
    DB2010Access *a = e->access;
    DB2010DmaRequestState requested = (a->value & 1) ? DBDMA_ENABLED : DBDMA_DISABLED;
    if (a->value & ~0x5ffffu) { return "dma_channel_configuration_unmodeled"; }
    if ((requested == DBDMA_ENABLED) &&
        !((d->config.display_first_burst && channel == 5 && a->value == 0xea53) ||
          (d->config.fa_tx && channel == 2 && a->value == 0xc8c7) ||
          (d->config.fa_rx && channel == 2 && a->value == 0xf0c7))) {
        return "dma_channel_execution_unmodeled";
    }
    if (a->commit) { d->state.channel[channel][4] = a->value; }
    return NULL;
}
static const char *dma_stage(DB2010Dma *d, const DB2010DmaEvent *e)
{
    unsigned channel = e->channel, field = e->field;
    DB2010Access *a = e->access;
    if (field == 2 && (a->value & 2)) { return "dma_lli_reserved_bit_unmodeled"; }
    if (field == 3 && (((a->value >> 18) & 7) > 2 || ((a->value >> 21) & 7) > 2)) {
        return "dma_control_width_unmodeled";
    }
    if (a->commit) { d->state.channel[channel][field] = a->value; }
    return NULL;
}
static const char *dma_dispatch(DB2010Dma *d, const DB2010DmaEvent *e)
{
    switch (e->kind) {
    case DBDMA_READ_CHANNEL:
        e->access->value = d->state.channel[e->channel][e->field]; return NULL;
    case DBDMA_STAGE_CHANNEL:
        return e->field == 4 ? dma_configure(d, e) : dma_stage(d, e);
    case DBDMA_ADVANCE_SOURCE:
        d->state.channel[e->channel][0] += e->bytes; return NULL;
    case DBDMA_TRANSFER_WORD:
        d->state.channel[e->channel][0] += 4;
        d->state.channel[e->channel][3]--; return NULL;
    case DBDMA_COMPLETE:
        d->state.channel[e->channel][4] &= ~1u;
        d->state.raw_tc |= 1u << e->channel; return NULL;
    default: g_assert_not_reached();
    }
}
static const char *dma_channel(void *opaque, const DB2010DecodedAccess *r, DB2010Access *a)
{
    DB2010DmaEvent e = {.kind = a->write ? DBDMA_STAGE_CHANNEL : DBDMA_READ_CHANNEL,
                        .channel = r->bank, .field = r->element, .access = a};
    return dma_dispatch(opaque, &e);
}

static const char *dma_global(void *opaque, const DB2010DecodedAccess *r, DB2010Access *a)
{
    DB2010Dma *d = opaque;
    if (!a->write) { a->value = d->state.global[r->element]; return NULL; }
    if (a->value & ~(r->element == 4 ? 7u : 0xffffu)) { return "dma_global_bits_unmodeled"; }
    if (r->element < 4 && a->value) { return "dma_software_request_unmodeled"; }
    if (a->commit) { d->state.global[r->element] = a->value; }
    return NULL;
}

static const char *dma_clear_field(void *opaque, const DB2010DecodedAccess *r,
                                   const DB2010Access *a, DB2010FieldRef *field)
{
    DB2010Dma *d = opaque;
    *field = (DB2010FieldRef){.value = r->id == 8 ? &d->state.raw_tc : &d->state.raw_error,
                             .mask = 255};
    return NULL;
}

static const char *dma_status(void *opaque, const DB2010DecodedAccess *r, DB2010Access *a)
{
    DB2010Dma *d = opaque;
    uint32_t tc_mask = 0, error_mask = 0, enabled = 0;
    for (unsigned ch = 0; ch < 8; ch++) {
        if (d->state.channel[ch][4] & 0x8000) { tc_mask |= 1u << ch; }
        if (d->state.channel[ch][4] & 0x4000) { error_mask |= 1u << ch; }
        if (d->state.channel[ch][4] & 1) { enabled |= 1u << ch; }
    }
    switch (r->id) {
    case 0: a->value = (d->state.raw_tc & tc_mask) | (d->state.raw_error & error_mask); break;
    case 4: a->value = d->state.raw_tc & tc_mask; break;
    case 12: a->value = d->state.raw_error & error_mask; break;
    case 20: a->value = d->state.raw_tc; break;
    case 24: a->value = d->state.raw_error; break;
    default: a->value = enabled; break;
    }
    return NULL;
}

const DB2010RegMap *dbdma_map(void)
{
#define DMA_RW(fn) .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = fn}, \
                   .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = fn}
#define DMA_STATUS(off) DB_REG(off, .id = off, .alignment = 4, \
    .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = dma_status}, \
    .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "dma_status_write_unmodeled"})
#define DMA_CLEAR(off) DB_REG(off, .id = off, .alignment = 4, .backing_width = 4, \
    .read = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "dma_clear_read_unmodeled"}, \
    .write = {.kind = DB_ACTION_W1C, .widths = DB_WIDTH32, .field = dma_clear_field, \
        .fault = "dma_clear_bits_unmodeled", .constraint = {.kind = DB_VALUE_RANGE, \
            .maximum = 255, .fault = "dma_clear_bits_unmodeled"}})
    static const DB2010RegRule channel[] = {
        DB_ARRAY(0, 5, .stride = 4, .alignment = 4, DMA_RW(dma_channel)),
    };
    static const DB2010RegRule children[] = {
        DMA_STATUS(0), DMA_STATUS(4), DMA_CLEAR(8), DMA_STATUS(12),
        DMA_CLEAR(16), DMA_STATUS(20), DMA_STATUS(24), DMA_STATUS(28),
        DB_ARRAY(0x20, 6, .stride = 4, .alignment = 4, DMA_RW(dma_global)),
        DB_REPEAT(0x100, 8, .stride = 0x20, .children = channel,
                  .child_count = ARRAY_SIZE(channel)),
    };
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 4096, .children = children, .child_count = ARRAY_SIZE(children)),
    };
#undef DMA_RW
#undef DMA_STATUS
#undef DMA_CLEAR
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf2000000, rules,
            ARRAY_SIZE(rules), "dma_register_unmodeled");
        if (fault) { g_error("DB2010 DMA layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}

const char *dbdma_access(DB2010Dma *d, uint32_t address,
                         unsigned size, bool write, uint32_t *value)
{
    DB2010Access a = {.address = address, .size = size, .write = write,
                      .value = *value, .commit = true};
    DB2010DecodedAccess decoded;
    const char *fault = db2010_reg_decode(dbdma_map(), &a, &decoded);
    /* Preserve the historical width/alignment fault even at guarded holes. */
    if (fault && (size != 4 || address % 4)) { return "dma_access_width_unmodeled"; }
    if (!fault) { fault = db2010_reg_execute(d, &decoded, &a); }
    *value = a.value;
    return fault;
}

void dbdma_source_advance(DB2010Dma *dma, unsigned channel, unsigned bytes)
{
    DB2010DmaEvent e = {.kind = DBDMA_ADVANCE_SOURCE, .channel = channel, .bytes = bytes};
    dma_dispatch(dma, &e);
}
void dbdma_word_transferred(DB2010Dma *dma, unsigned channel)
{
    DB2010DmaEvent e = {.kind = DBDMA_TRANSFER_WORD, .channel = channel};
    dma_dispatch(dma, &e);
}
void dbdma_complete(DB2010Dma *dma, unsigned channel)
{
    DB2010DmaEvent e = {.kind = DBDMA_COMPLETE, .channel = channel};
    dma_dispatch(dma, &e);
}

void dbdma_reset(DB2010Dma *d)
{
    d->state = (DB2010DmaRuntime){0};
}

void dbdma_export(DB2010Dma *model, QDict *snapshot)
{

        QDict *dma = qdict_new(), *global = qdict_new();
        static const char *global_names[] = {"soft_burst", "soft_single", "soft_last_burst", "soft_last_single", "configuration", "sync"};
        static const char *channel_names[] = {"source", "destination", "lli", "control", "configuration"};
        for (unsigned field = 0; field < 6; field++) { qdict_put_int(global, global_names[field], model->state.global[field]); }
        qdict_put(dma, "global_words", global);
        qdict_put_int(dma, "raw_tc", model->state.raw_tc); qdict_put_int(dma, "raw_error", model->state.raw_error);
        QList *channels = qlist_new();
        for (unsigned ch = 0; ch < 8; ch++) {
            QDict *values = qdict_new();
            for (unsigned field = 0; field < 5; field++) { qdict_put_int(values, channel_names[field], model->state.channel[ch][field]); }
            qlist_append(channels, values);
        }
        qdict_put(dma, "channels", channels); qdict_put(snapshot, "pl080", dma);
    }
