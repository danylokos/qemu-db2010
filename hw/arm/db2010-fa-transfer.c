/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-fa-transfer.h"


bool dbfa_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = dbfa_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}

bool dbfa_word(DB2010FaTransfer *f, uint32_t address, uint16_t *value)
{
    gpointer stored = g_hash_table_lookup(f->memory, GUINT_TO_POINTER(address + 1));
    if (!stored) {
        return false;
    }
    *value = GPOINTER_TO_UINT(stored) - 1;
    return true;
}

void dbfa_set_word(DB2010FaTransfer *f, uint32_t address, uint16_t value)
{
    g_hash_table_insert(f->memory, GUINT_TO_POINTER(address + 1),
                       GUINT_TO_POINTER((unsigned)value + 1));
}

void dbfa_reset(DB2010FaTransfer *f)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_remove_all(f->memory);
    g_hash_table_iter_init(&iter, f->initial);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_hash_table_insert(f->memory, key, value);
    }
    f->address = f->data = 0;
    f->command = DBFA_IDLE;
    f->address_known = f->data_known = false;
    f->due = f->writes = f->reads = f->completions = f->polls = 0;
}

typedef enum DB2010FaEventKind {
    DBFA_STAGE_ADDRESS, DBFA_STAGE_DATA, DBFA_SUBMIT, DBFA_COMPLETE,
    DBFA_READ_COMMAND, DBFA_READ_DATA, DBFA_READ_ADDRESS,
} DB2010FaEventKind;
typedef struct DB2010FaEvent {
    DB2010FaEventKind kind;
    uint32_t *value;
    uint64_t now;
    bool apply;
} DB2010FaEvent;
static void dbfa_complete_write(DB2010FaTransfer *f)
{
    dbfa_set_word(f, f->address - 1, f->data >> 16);
    dbfa_set_word(f, f->address, f->data);
}
static void dbfa_complete_read(DB2010FaTransfer *f)
{
    uint16_t hi, lo;
    bool known = dbfa_word(f, f->address - 1, &hi) && dbfa_word(f, f->address, &lo);
    g_assert(known); /* Validated before command submission. */
    f->data = (uint32_t)hi << 16 | lo;
    f->data_known = true;
}
static const char *dbfa_submit_idle(DB2010FaTransfer *f, const DB2010FaEvent *e)
{
    uint32_t value = *e->value;
    uint16_t hi, lo;
    if (value != DBFA_WRITE_PENDING && value != DBFA_READ_PENDING) {
        return "fa_transfer_command_unmodeled";
    }
    if (!f->address_known || (value == DBFA_WRITE_PENDING && !f->data_known)) {
        return "fa_transfer_staging_unknown";
    }
    if (value == DBFA_READ_PENDING && (!dbfa_word(f, f->address - 1, &hi) ||
                                      !dbfa_word(f, f->address, &lo))) {
        return "fa_transfer_peer_memory_unknown";
    }
    if (e->apply) {
        f->command = value; f->due = e->now + 128;
        if (value == DBFA_WRITE_PENDING) { f->writes++; }
        else { f->reads++; f->data_known = false; }
    }
    return NULL;
}
static const char *dbfa_idle(DB2010FaTransfer *f, const DB2010FaEvent *e)
{
    switch (e->kind) {
    case DBFA_STAGE_ADDRESS:
        if (!*e->value || *e->value > 0x7fffff) { return "fa_transfer_address_unmodeled"; }
        if (e->apply) { f->address = *e->value; f->address_known = true; }
        return NULL;
    case DBFA_STAGE_DATA:
        if (e->apply) { f->data = *e->value; f->data_known = true; }
        return NULL;
    case DBFA_SUBMIT: return dbfa_submit_idle(f, e);
    default: g_assert_not_reached();
    }
}
static const char *dbfa_dispatch(DB2010FaTransfer *f, const DB2010FaEvent *e)
{
    switch (e->kind) {
    case DBFA_COMPLETE:
        if (f->command != DBFA_IDLE && e->now >= f->due) {
            if (f->command == DBFA_WRITE_PENDING) { dbfa_complete_write(f); }
            else { dbfa_complete_read(f); }
            f->command = DBFA_IDLE; f->completions++;
        }
        return NULL;
    case DBFA_READ_COMMAND:
        *e->value = f->command;
        if (e->apply) { f->polls++; }
        return NULL;
    case DBFA_READ_DATA:
        if (f->command != DBFA_IDLE || !f->data_known) { return "fa_transfer_data_not_ready"; }
        *e->value = f->data; return NULL;
    case DBFA_READ_ADDRESS: return "fa_transfer_address_read_unmodeled";
    default:
        if (f->command != DBFA_IDLE) { return "fa_transfer_write_while_busy_unmodeled"; }
        return dbfa_idle(f, e);
    }
}
void dbfa_finish(DB2010FaTransfer *f, uint64_t now)
{
    DB2010FaEvent e = {.kind = DBFA_COMPLETE, .now = now, .apply = true};
    dbfa_dispatch(f, &e);
}
static const char *fa_port_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010FaEvent e = {.now = a->now, .value = &a->value, .apply = a->commit};
    e.kind = a->write ? d->id : d->id == DBFA_SUBMIT ? DBFA_READ_COMMAND : DBFA_READ_DATA;
    return dbfa_dispatch(opaque, &e);
}
const DB2010RegMap *dbfa_map(void)
{
#define FA_WRITE .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = fa_port_action}
#define FA_READ .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = fa_port_action}
    static const DB2010RegRule children[] = {
        DB_REG(0, .id = DBFA_SUBMIT, .alignment = 4, FA_READ, FA_WRITE),
        DB_REG(4, .id = DBFA_STAGE_ADDRESS, .alignment = 4, FA_WRITE,
            .read = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32,
                     .fault = "fa_transfer_address_read_unmodeled"}),
        DB_REG(8, .id = DBFA_STAGE_DATA, .alignment = 4, .backing_width = 4, FA_READ, FA_WRITE),
    };
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 12, .children = children, .child_count = ARRAY_SIZE(children)),
    };
#undef FA_WRITE
#undef FA_READ
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xfa040024, rules,
            ARRAY_SIZE(rules), "fa_transfer_access_unmodeled");
        if (fault) { g_error("DB2010 FA transfer layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *dbfa_access(DB2010FaTransfer *f, uint32_t address,
                        unsigned size, uint64_t now, bool write, uint32_t *value, bool apply)
{
    DB2010Access a = {.address = address, .size = size, .now = now, .write = write,
                      .value = *value, .commit = apply};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbfa_map(), &a, &d);
    if (!fault) { fault = db2010_reg_execute(f, &d, &a); }
    *value = a.value;
    return fault;
}

gint dbfa_key_compare(gconstpointer a, gconstpointer b, gpointer opaque)
{
    return (int)GPOINTER_TO_UINT(a) - (int)GPOINTER_TO_UINT(b);
}

QDict *dbfa_snapshot(DB2010FaTransfer *f)
{
    QDict *result = qdict_new();
    QList *spans = qlist_new();
    GList *keys = g_list_sort_with_data(g_hash_table_get_keys(f->memory),
                                       dbfa_key_compare, NULL);
    GString *hex = g_string_new(NULL);
    uint32_t start = 0, previous = 0, count = 0;
    for (GList *it = keys; it; it = it->next) {
        uint32_t address = GPOINTER_TO_UINT(it->data) - 1;
        uint16_t value;
        if (count && address != previous + 1) {
            QDict *span = qdict_new();
            qdict_put_int(span, "start_halfword", start);
            qdict_put_int(span, "halfwords", count);
            qdict_put_str(span, "little_endian_hex", hex->str);
            qlist_append(spans, span);
            g_string_truncate(hex, 0); count = 0;
        }
        if (!count) { start = address; }
        g_assert(dbfa_word(f, address, &value));
        g_string_append_printf(hex, "%02x%02x", value & 255, value >> 8);
        previous = address; count++;
    }
    if (count) {
        QDict *span = qdict_new();
        qdict_put_int(span, "start_halfword", start);
        qdict_put_int(span, "halfwords", count);
        qdict_put_str(span, "little_endian_hex", hex->str);
        qlist_append(spans, span);
    }
    g_list_free(keys); g_string_free(hex, true);
    qdict_put_str(result, "profile", "word-pairs-latency2");
    qdict_put_bool(result, "DSP_execution_modeled", false);
    qdict_put_bool(result, "reset_state_measured", false);
    qdict_put_int(result, "completion_ns", 128);
    qdict_put_int(result, "command", f->command);
    qdict_put_int(result, "deadline_ns", f->due);
    if (f->address_known) { qdict_put_int(result, "address", f->address); }
    else { qdict_put_null(result, "address"); }
    if (f->data_known) { qdict_put_int(result, "data", f->data); }
    else { qdict_put_null(result, "data"); }
    qdict_put_int(result, "write_commands", f->writes);
    qdict_put_int(result, "read_commands", f->reads);
    qdict_put_int(result, "completions", f->completions);
    qdict_put_int(result, "command_reads", f->polls);
    qdict_put_int(result, "initial_halfwords", g_hash_table_size(f->initial));
    qdict_put_int(result, "known_halfwords", g_hash_table_size(f->memory));
    qdict_put(result, "memory", spans);
    return result;
}

const char *dbfa_bus_access(void *opaque, DB2010Access *a)
{
    return dbfa_access(opaque, a->address, a->size, a->now, a->write, &a->value, a->commit);
}
