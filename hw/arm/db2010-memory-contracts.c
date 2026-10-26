/* Guarded memory-controller/storage adapters. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "db2010-components.h"
#include "db2010-nor.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"


static const char *controller_action(void *opaque, const DB2010DecodedAccess *d,
                                     DB2010Access *a)
{
    DB2010MemoryController *s = opaque;
    unsigned index = d->id + d->element;
    if (!a->write) {
        if (!s->controller_known[index]) { return "controller_latch_unknown_word"; }
        a->value = s->controller_words[index];
    } else {
        if (index == 10 && a->value != s->initial_bank) { return "bank_decoder_change_unmodeled"; }
        if (a->commit) { s->controller_words[index] = a->value; s->controller_known[index] = true; }
    }
    return NULL;
}

const DB2010RegMap *db2010_controller_map(void)
{
#define MC_ACTION .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = controller_action}, \
                  .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = controller_action}
    static const DB2010RegRule children[] = {
        DB_REG(0, .id = 0, .alignment = 4, .backing_width = 4, MC_ACTION),
        DB_ARRAY(8, 9, .id = 2, .stride = 4, .alignment = 4, .backing_width = 4, MC_ACTION),
    };
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 4096, .children = children, .child_count = ARRAY_SIZE(children)),
    };
#undef MC_ACTION
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0x14000000, rules,
            ARRAY_SIZE(rules), "controller_latch_access_unmodeled");
        if (fault) { g_error("DB2010 controller layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *db2010_controller_access(void *opaque, DB2010Access *a)
{
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(db2010_controller_map(), a, &d);
    if (!fault) { fault = db2010_reg_execute(opaque, &d, a); }
    return fault;
}

static const char *region_action(void *opaque, const DB2010DecodedAccess *d,
                                 DB2010Access *a)
{
    DB2010RegionController *s = opaque;
    if (!a->write) {
        if (!s->region_known[d->element]) { return "region_register_initial_state_unknown"; }
        a->value = s->region_words[d->element];
    } else if (a->commit) {
        s->region_words[d->element] = a->value; s->region_known[d->element] = true;
        s->table_dirty = s->has_section_table;
    }
    return NULL;
}

const DB2010RegMap *db2010_region_map(void)
{
    static const DB2010RegRule children[] = {
        DB_ARRAY(0, 24, .stride = 4, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32,
                     .callback = region_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32,
                      .callback = region_action}),
    };
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 4096, .children = children, .child_count = ARRAY_SIZE(children)),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xfe004000, rules,
            ARRAY_SIZE(rules), "region_register_access_unmodeled");
        if (fault) { g_error("DB2010 region layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *db2010_region_access(void *opaque, DB2010Access *a)
{
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(db2010_region_map(), a, &d);
    if (!fault) { fault = db2010_reg_execute(opaque, &d, a); }
    return fault;
}

const DB2010RegMap *db2010_absent_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 8192, .alignment = DB_ALIGN_NATURAL,
            .read = {.kind = DB_ACTION_CONSTANT,
                .widths = DB_WIDTH8 | DB_WIDTH16 | DB_WIDTH32,
                .constant = UINT32_MAX, .constant_by_width = true},
            .write = {.kind = DB_ACTION_IGNORE,
                      .widths = DB_WIDTH8 | DB_WIDTH16 | DB_WIDTH32}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0x46000000, rules,
            ARRAY_SIZE(rules), "absent_bank_access_unmodeled");
        if (fault) { g_error("DB2010 absent-bank layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *db2010_absent_access(void *opaque, DB2010Access *a)
{
    DB2010Absent *s = opaque;
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(db2010_absent_map(), a, &d);
    if (!fault) { fault = db2010_reg_execute(s, &d, a); }
    if (!fault && a->commit) {
        if (a->write) { s->absent_writes++; } else { s->absent_reads++; }
    }
    return fault;
}

static const char *nor_read_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    return db2010_nor_read(opaque, a->address, a->size, &a->value, &a->fault_address);
}
static const char *nor_write_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Nor *nor = opaque, preview = *nor;
    return db2010_nor_write(a->commit ? nor : &preview, a->address, a->size, a->value, a->commit);
}
const DB2010RegMap *db2010_nor_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, DBNOR_SIZE, .alignment = 1,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH8 | DB_WIDTH16 | DB_WIDTH32,
                     .callback = nor_read_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH8 | DB_WIDTH16 | DB_WIDTH32,
                      .callback = nor_write_action}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, DBNOR_BASE, rules,
            ARRAY_SIZE(rules), "nor_access_unmodeled");
        if (fault) { g_error("DB2010 NOR window layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *db2010_nor_bus_access(void *opaque, DB2010Access *a)
{
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(db2010_nor_map(), a, &d);
    if (!fault) { return db2010_reg_execute(opaque, &d, a); }
    /* Preserve custom range/width fault priority and read fault addresses for
     * rejected window shapes. No successful native shape may bypass the map. */
    fault = a->write ? nor_write_action(opaque, NULL, a) : nor_read_action(opaque, NULL, a);
    g_assert(fault);
    return fault;
}

/* Native bus preparation is separate from the pure access preview: only the
 * CPU research integration calls this after a successful read validation.
 * It retains the existing ROMD probe-view transition and guest overlay ledger. */
void db2010_nor_prepare(void *opaque, DB2010Access *a)
{
    DB2010Nor *nor = opaque;
    db2010_nor_overlay_observe(nor, a->address, a->size);
    if (db2010_nor_probe_view(nor, a->address, a->size)) {
        BQL_LOCK_GUARD();
        memory_region_rom_device_set_romd(nor->mr, false);
    }
}

void db2010_memory_controller_reset(DB2010MemoryController *s, bool enabled)
{
    memset(s->controller_words, 0, sizeof(s->controller_words));
    memset(s->controller_known, 0, sizeof(s->controller_known));
    s->controller_words[10] = s->initial_bank; s->controller_known[10] = enabled;
}
void db2010_region_controller_reset(DB2010RegionController *s)
{
    memcpy(s->region_words, s->initial_region_words, sizeof(s->region_words));
    memcpy(s->region_known, s->initial_region_known, sizeof(s->region_known));
    s->table_dirty = false; s->table_commits = 0;
}
void db2010_absent_reset(DB2010Absent *s)
{
    s->absent_reads = s->absent_writes = 0;
}

void db2010_absent_export(DB2010Absent *model, QDict *snapshot)
{

        qdict_put_int(snapshot, "absent_bank_reads", model->absent_reads);
        qdict_put_int(snapshot, "absent_bank_ignored_writes", model->absent_writes);
    }

void db2010_region_export(DB2010RegionController *model, QDict *snapshot)
{
    unsigned i;

        QList *values = qlist_new();
        for (i = 0; i < 24; i++) {
            if (model->region_known[i]) {
                qlist_append_int(values, model->region_words[i]);
            } else {
                qlist_append_null(values);
            }
        }
        qdict_put(snapshot, "region_words", values);
        qdict_put_bool(snapshot, "section_table_dirty", model->table_dirty);
        qdict_put_int(snapshot, "section_table_commits", model->table_commits);
    }

void db2010_controller_export(DB2010MemoryController *model, QDict *snapshot)
{
    unsigned i;

        QList *values = qlist_new();
        for (i = 0; i < 11; i++) {
            if (model->controller_known[i]) {
                qlist_append_int(values, model->controller_words[i]);
            } else {
                qlist_append_null(values);
            }
        }
        qdict_put(snapshot, "controller_words", values);
    }

void db2010_region_table_committed(DB2010RegionController *s)
{
    s->table_dirty = false; s->table_commits++;
}
