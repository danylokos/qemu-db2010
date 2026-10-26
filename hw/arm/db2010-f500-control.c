/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-f500-control.h"


void dbfc_reset(DB2010F500Control *s)
{
    s->state = (DB2010F500ControlRuntime){0};
}

typedef enum DB2010F500ControlEventKind { DBFC_COMPLETE, DBFC_COMMAND, DBFC_READ } DB2010F500ControlEventKind;
typedef struct DB2010F500ControlEvent {
    DB2010F500ControlEventKind kind;
    uint32_t *latch, *value;
    uint64_t now;
} DB2010F500ControlEvent;
static const char *dbfc_command(DB2010F500Control *s, const DB2010F500ControlEvent *e)
{
    uint32_t *value = e->value;
    uint64_t now = e->now;
    switch (*value & 0xffff) {
    case 0:
    case 0x4000:
        if (s->state.commands) {
            return "f500_control_later_power_write_unmodeled";
        }
        break;
    case 0x0400:
        if (s->state.commands) {
            return "f500_control_repeat_command_unmodeled";
        }
        s->state.commands++;
        s->state.pending = DBFC_PENDING;
        s->state.due = now + s->config.latency;
        break;
    case 0x43ff:
    case 0x4bff:
        if (!s->state.completions) {
            return "f500_control_configuration_before_completion";
        }
        s->state.config_writes++;
        break;
    default:
        return "f500_control_command_unmodeled";
    }
    *e->latch = *value & 0xffff;
    return NULL;
}
static const char *dbfc_dispatch(DB2010F500Control *s, const DB2010F500ControlEvent *e)
{
    switch (e->kind) {
    case DBFC_COMPLETE:
        if (s->state.pending == DBFC_PENDING && e->now >= s->state.due) {
            *e->latch &= ~0x0400;
            s->state.pending = DBFC_READY;
            s->state.completions++;
        }
        return NULL;
    case DBFC_READ:
        if (s->state.pending == DBFC_PENDING) { s->state.busy_reads++; }
        else { s->state.ready_reads++; }
        *e->value = *e->latch;
        return NULL;
    case DBFC_COMMAND:
        switch (s->state.pending) {
        case DBFC_PENDING: return "f500_control_busy_write_unmodeled";
        case DBFC_READY: return dbfc_command(s, e);
        default: g_assert_not_reached();
        }
    default: g_assert_not_reached();
    }
}
void dbfc_advance(DB2010F500Control *s, uint32_t *latch, uint64_t now)
{
    DB2010F500ControlEvent e = {.kind = DBFC_COMPLETE, .latch = latch, .now = now};
    dbfc_dispatch(s, &e);
}
typedef struct DB2010F500LatchAccess {
    DB2010F500Control *control;
    uint32_t *latch;
    bool known;
} DB2010F500LatchAccess;
static const char *dbfc_latch_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010F500LatchAccess *ctx = opaque;
    if (!a->write && !ctx->known) { return "device_register_initial_state_unknown"; }
    dbfc_advance(ctx->control, ctx->latch, a->now);
    DB2010F500ControlEvent e = {.kind = a->write ? DBFC_COMMAND : DBFC_READ,
                               .latch = ctx->latch, .value = &a->value, .now = a->now};
    return dbfc_dispatch(ctx->control, &e);
}
const DB2010RegMap *dbfc_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_VIEW(0, .alignment = 2, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16,
                     .callback = dbfc_latch_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16,
                      .callback = dbfc_latch_action}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf5000800, rules,
            ARRAY_SIZE(rules), "f500_control_access_unmodeled");
        if (fault) { g_error("DB2010 F500 control layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *dbfc_access(DB2010F500Control *s, uint32_t *latch,
                       bool known, uint64_t now, unsigned size, bool write, uint32_t *value)
{
    const DB2010RegMap *map = dbfc_map();
    DB2010Access a = {.address = map->claims[0].address, .size = size, .now = now,
                      .write = write, .value = *value, .commit = true};
    DB2010F500LatchAccess ctx = {s, latch, known};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(map, &a, &d);
    if (!fault) { fault = db2010_reg_execute(&ctx, &d, &a); }
    *value = a.value;
    return fault;
}

const char *dbfc_bus_access(void *opaque, DB2010Access *a)
{
    if (a->address != dbfc_map()->claims[0].address) { return "device_register_access_unmodeled"; }
    DB2010F500ControlLink *link = opaque;
    DB2010F500Control preview = *link->control;
    uint32_t latch = *link->latch;
    const char *fault = dbfc_access(a->commit ? link->control : &preview,
        a->commit ? link->latch : &latch, *link->known_mask == 0xffff, a->now,
        a->size, a->write, &a->value);
    if (!fault && a->commit && a->write) { *link->known_mask = 0xffff; }
    return fault;
}

void dbfc_export(DB2010F500Control *model, QDict *snapshot)
{

        DB2010F500Control *sfc = model;
        QDict *fc = qdict_new();
        qdict_put_str(fc, "profile", "bit10-self-clear-latency");
        qdict_put_bool(fc, "synthetic", true);
        qdict_put_bool(fc, "physical_identity_established", false);
        qdict_put_int(fc, "latency_ns", sfc->config.latency);
        qdict_put_bool(fc, "pending", sfc->state.pending);
        qdict_put_int(fc, "deadline_ns", sfc->state.due);
        qdict_put_int(fc, "commands", sfc->state.commands);
        qdict_put_int(fc, "completions", sfc->state.completions);
        qdict_put_int(fc, "busy_reads", sfc->state.busy_reads);
        qdict_put_int(fc, "ready_reads", sfc->state.ready_reads);
        qdict_put_int(fc, "config_writes", sfc->state.config_writes);
        qdict_put(snapshot, "f500_control", fc);
    }
