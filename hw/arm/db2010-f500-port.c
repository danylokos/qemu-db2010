/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-f500-port.h"

/* Each row constrains a tagged opaque TX word: (word & mask) == required.
 * These are recovered caller constraints, not physical peer registers. */
static const DB2010F500Command primary_commands[] = {
    {1, 0x00ff, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0},
};
static const DB2010F500Command auxiliary_commands[] = {
    {1, 0xffff, 1}, {1, 0xffff, 0x401},
    {5, (uint16_t)~0x2e5u, 0}, {6, (uint16_t)~0x7c6u, 0},
    {7, (uint16_t)~0x7f7u, 0}, {8, (uint16_t)~0x7e8u, 0},
    {9, (uint16_t)~0x600u, 0x89},
};
#define PORT_REG(a, s, r, w) {a, s, 1u << s, 1, r, w}
const DB2010F500Variant dbfp_primary = {
    .profile = "word-ready-strobe-latency",
    .registers = {PORT_REG(0xf50005d6, 1, false, true),
                  PORT_REG(0xf50005d8, 2, false, true),
                  PORT_REG(0xf50005e2, 1, false, true),
                  PORT_REG(0xf50005e6, 1, true, false)},
    .setup_value = 0x19, .tag_shift = 8, .tag_mask = 255, .snapshot_tags = 4,
    .commands = primary_commands, .command_count = ARRAY_SIZE(primary_commands),
};
const DB2010F500Variant dbfp_auxiliary = {
    .profile = "lowtag-ready-strobe-latency",
    .registers = {PORT_REG(0xf50005ce, 1, false, true),
                  PORT_REG(0xf50005da, 2, false, true),
                  PORT_REG(0xf50005e4, 1, false, true),
                  PORT_REG(0xf50005e8, 1, true, false)},
    .setup_value = 0x0f, .tag_shift = 0, .tag_mask = 15, .snapshot_tags = 9,
    .commands = auxiliary_commands, .command_count = ARRAY_SIZE(auxiliary_commands),
};
#undef PORT_REG

void dbfp_reset(DB2010F500Port *p)
{
    p->state = (DB2010F500PortRuntime){0};
}

bool dbfp_contains(uint32_t address, unsigned size,
                    const DB2010F500Variant *variant)
{
    const DB2010RegMap *map = dbfp_map(variant);
    return db2010_claims(map->claims, map->claim_count, address, size);
}

typedef enum DB2010F500EventKind {
    DBFP_COMPLETE, DBFP_READ_STATUS, DBFP_SETUP, DBFP_STAGE_WORD, DBFP_STROBE,
} DB2010F500EventKind;
typedef struct DB2010F500Event {
    DB2010F500EventKind kind;
    uint64_t now;
    uint32_t *value;
} DB2010F500Event;

static void dbfp_complete(DB2010F500Port *p)
{
    p->state.pending = DBFP_READY;
    p->state.completions++;
    unsigned tag = (p->state.word >> p->config.variant->tag_shift) & p->config.variant->tag_mask;
    p->state.last_by_tag[tag] = p->state.word;
    p->state.tag_known[tag] = true;
}
static const char *dbfp_setup(DB2010F500Port *p, const DB2010F500Event *e)
{
    const DB2010F500Variant *v = p->config.variant;
    uint32_t *value = e->value;
    if ((*value & 0xff) != v->setup_value) { return "f500_port_setup_unmodeled"; }
    p->state.setup_value = *value & 0xff;
    p->state.setup_known = true;
    return NULL;

}
static const char *dbfp_stage(DB2010F500Port *p, const DB2010F500Event *e)
{
    const DB2010F500Variant *v = p->config.variant;
    uint32_t *value = e->value;
    uint64_t now = e->now;
    unsigned word = *value & 0xffff;
    unsigned tag = (word >> v->tag_shift) & v->tag_mask;
    bool valid = false;
    for (unsigned i = 0; i < v->command_count; i++) {
        const DB2010F500Command *c = &v->commands[i];
        if (tag == c->tag && (word & c->mask) == c->required) {
            valid = true;
            break;
        }
    }
    if (!valid) { return "f500_port_command_unmodeled"; }
    if (!p->state.setup_known) { return "f500_port_setup_unknown"; }
    if (p->state.strobe_high) { return "f500_port_strobe_incomplete"; }
    p->state.word = word;
    p->state.pending = DBFP_PENDING;
    p->state.status_known = true;
    p->state.due = now + p->config.latency;
    p->state.commands++;
    return NULL;

}
static const char *dbfp_strobe(DB2010F500Port *p, const DB2010F500Event *e)
{
    uint32_t *value = e->value;
    if (!p->state.status_known) { return "f500_port_strobe_before_command"; }
    unsigned strobe = *value & 0xff;
    if (strobe > 1) { return "f500_port_strobe_unmodeled"; }
    if (strobe == p->state.strobe_high) { return "f500_port_strobe_order_unmodeled"; }
    p->state.strobe_high = strobe;
    p->state.strobe_known = true;
    if (!strobe) { p->state.pulses++; }
    return NULL;
}
static const char *dbfp_ready(DB2010F500Port *p, const DB2010F500Event *e)
{
    switch (e->kind) {
    case DBFP_SETUP: return dbfp_setup(p, e);
    case DBFP_STAGE_WORD: return dbfp_stage(p, e);
    case DBFP_STROBE: return dbfp_strobe(p, e);
    default: g_assert_not_reached();
    }
}
static const char *dbfp_dispatch(DB2010F500Port *p, const DB2010F500Event *e)
{
    if (e->kind == DBFP_COMPLETE) {
        if (p->state.pending == DBFP_PENDING && e->now >= p->state.due) { dbfp_complete(p); }
        return NULL;
    }
    if (e->kind == DBFP_READ_STATUS) {
        if (!p->state.status_known) { return "f500_port_status_unknown"; }
        p->state.reads++;
        if (p->state.pending == DBFP_PENDING) { p->state.busy_reads++; }
        *e->value = p->state.pending == DBFP_READY;
        return NULL;
    }
    switch (p->state.pending) {
    case DBFP_PENDING: return "f500_port_write_while_busy_unmodeled";
    case DBFP_READY: return dbfp_ready(p, e);
    default: g_assert_not_reached();
    }
}
void dbfp_finish(DB2010F500Port *p, uint64_t now)
{
    DB2010F500Event e = {.kind = DBFP_COMPLETE, .now = now};
    dbfp_dispatch(p, &e);
}
static const char *dbfp_rule_access(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010F500Port *p = opaque;
    const DB2010RegisterSpec *port = d->selector;
    /* Shape precedes completion; direction faults deliberately follow it. */
    dbfp_finish(p, a->now);
    DB2010F500Event e = {.now = a->now, .value = &a->value};
    if (!a->write) {
        if (!port->readable) { return "f500_port_read_unmodeled"; }
        e.kind = DBFP_READ_STATUS;
    } else {
        if (!port->writable) { return "f500_port_status_write_unmodeled"; }
        e.kind = d->id == 0 ? DBFP_SETUP : d->id == 1 ? DBFP_STAGE_WORD : DBFP_STROBE;
    }
    return dbfp_dispatch(p, &e);
}
const DB2010RegMap *dbfp_map(const DB2010F500Variant *variant)
{
    static DB2010RegMap maps[2];
    static DB2010RegRule rules[2][4];
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const DB2010F500Variant *variants[] = {&dbfp_primary, &dbfp_auxiliary};
        for (unsigned bank = 0; bank < ARRAY_SIZE(variants); bank++) {
            const DB2010F500Variant *v = variants[bank];
            for (unsigned i = 0; i < ARRAY_SIZE(v->registers); i++) {
                const DB2010RegisterSpec *port = &v->registers[i];
                rules[bank][i] = (DB2010RegRule)DB_REG(port->address,
                    .id = i, .alignment = port->alignment, .backing_width = port->span,
                    .selector = port,
                    .read = {.kind = DB_ACTION_CALLBACK, .widths = port->widths,
                             .callback = dbfp_rule_access},
                    .write = {.kind = DB_ACTION_CALLBACK, .widths = port->widths,
                              .callback = dbfp_rule_access});
            }
            const char *fault = db2010_regmap_init(&maps[bank], 0, rules[bank],
                ARRAY_SIZE(rules[bank]), "f500_port_access_unmodeled");
            if (fault) { g_error("DB2010 F500 port layout: %s", fault); }
        }
        g_once_init_leave(&initialized, 1);
    }
    if (variant == &dbfp_primary) { return &maps[0]; }
    g_assert(variant == &dbfp_auxiliary);
    return &maps[1];
}
static const char *dbfp_commit(DB2010F500Port *p, uint32_t address,
                               unsigned size, uint64_t now, bool write, uint32_t *value)
{
    DB2010Access a = {.address = address, .size = size, .now = now, .write = write,
                      .value = *value, .commit = true};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbfp_map(p->config.variant), &a, &d);
    if (!fault) { fault = db2010_reg_execute(p, &d, &a); }
    *value = a.value;
    return fault;
}

const char *dbfp_access(DB2010F500Port *p, uint32_t address, unsigned size,
                        uint64_t now, bool write, uint32_t *value, bool apply)
{
    DB2010F500Port preview;
    if (!apply) { preview = *p; p = &preview; }
    return dbfp_commit(p, address, size, now, write, value);
}

QDict *dbfp_snapshot(DB2010F500Port *p)
{
    QDict *d = qdict_new(), *tags = qdict_new();
    qdict_put_str(d, "profile", p->config.variant->profile);
    qdict_put_bool(d, "synthetic", true);
    qdict_put_bool(d, "physical_identity_established", false);
    qdict_put_bool(d, "peer_replies_or_IRQ_supplied", false);
    qdict_put_int(d, "latency_ns", p->config.latency);
    if (p->state.setup_known) { qdict_put_int(d, "setup_value", p->state.setup_value); }
    else { qdict_put_null(d, "setup_value"); }
    if (p->state.status_known) { qdict_put_int(d, "ready", !p->state.pending); }
    else { qdict_put_null(d, "ready"); }
    if (p->state.strobe_known) { qdict_put_int(d, "strobe", p->state.strobe_high); }
    else { qdict_put_null(d, "strobe"); }
    qdict_put_bool(d, "pending", p->state.pending);
    qdict_put_int(d, "deadline_ns", p->state.due);
    qdict_put_int(d, "commands", p->state.commands);
    qdict_put_int(d, "completions", p->state.completions);
    qdict_put_int(d, "status_reads", p->state.reads);
    qdict_put_int(d, "busy_reads", p->state.busy_reads);
    qdict_put_int(d, "pulses", p->state.pulses);
    for (unsigned tag = 1; tag <= p->config.variant->snapshot_tags; tag++) {
        g_autofree char *key = g_strdup_printf("%u", tag);
        if (p->state.tag_known[tag]) { qdict_put_int(tags, key, p->state.last_by_tag[tag]); }
        else { qdict_put_null(tags, key); }
    }
    qdict_put(d, "last_completed_word_by_tag", tags);
    return d;
}

const char *dbfp_bus_access(void *opaque, DB2010Access *a)
{
    return dbfp_access(opaque, a->address, a->size, a->now, a->write,
                        &a->value, a->commit);
}
