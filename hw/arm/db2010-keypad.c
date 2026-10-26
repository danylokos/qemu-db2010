/* Ideal handset contacts, without host repeat or bounce.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-keypad.h"
#include "db2010-device.h"
#include "db2010-time.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "system/cpus.h"
#include "qemu/main-loop.h"

typedef struct Contact { const char *name; unsigned row, columns; } Contact;
/* Original key table4423FEA0, row*5+column; joystick decoder44E44EFC.
 * Joystick names use the clockwise directional codes37..44 (see report).
 * Multiple joystick switches resolve by wired-AND, not synthesized diagonals. */
static const Contact contacts[] = {
    {"1", 3, 1}, {"2", 2, 1}, {"3", 0, 1},
    {"4", 3, 2}, {"5", 2, 2}, {"6", 0, 2},
    {"7", 3, 4}, {"8", 2, 4}, {"9", 0, 4},
    {"star", 3, 8}, {"0", 2, 8}, {"hash", 0, 8},
    {"soft-left", 1, 1}, {"soft-right", 1, 2},
    {"back", 1, 4}, {"clear", 1, 8}, {"walkman", 5, 2},
    {"up", 4, 0x1a}, {"right", 4, 0x19},
    {"down", 4, 0x0d}, {"left", 4, 0x0e}, {"select", 4, 8},
    {"volume-up", 3, 0x10}, {"volume-down", 2, 0x10},
    {"play-pause", 1, 0x10}, {"camera-focus", 5, 4},
    /* A full shutter press closes both stages. Releasing it leaves a
     * separately held half-press closed through the ordinary wired AND. */
    {"camera", 5, 0x0c},
    /* ON/OFF grounds KEYIN0 independently of KEYOUT. Native44911F84
     * samples it with all six rows high, then masks this column out of
     * the six matrix samples. Native index30 translates to key code100. */
    {"power", 6, 1},
};
static unsigned columns(DB2010Keypad *s)
{
    unsigned result = 255;
    for (unsigned i = 0; i < ARRAY_SIZE(contacts); i++) {
        if ((s->pressed & (1u << i)) &&
            (contacts[i].row == 6 || !(s->rows & (1u << contacts[i].row)))) {
            result &= ~contacts[i].columns;
        }
    }
    return result;
}
static void irq_update(DB2010Keypad *s)
{
    /* Bit7 scanner enable, bit9 opened IRQ; bit8 write acknowledges.
     * Readback preserves the former boot latch contract, including bit8. */
    db2010_signal_set(&s->irq, s->known && (s->control & 0x280) == 0x280 && s->pending);
}
static const char *keypad_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Keypad *s = opaque;
    if (!d->id) {
        if (!a->write) {
            if (!s->known) { return "keypad_control_unknown"; }
            a->value = s->control;
        } else {
            switch (a->value) {
            case 0x121: case 0x1a1: case 0x321: case 0x3a1: break;
            default: return "keypad_control_write_unmodeled";
            }
            if (a->commit) {
                s->known = true; s->control = a->value;
                if (a->value & 0x100) { s->pending = false; s->acknowledgements++; }
                irq_update(s);
            }
        }
    } else if (!a->write) {
        a->value = s->rows | (columns(s) << 6);
        if (a->commit) { s->scans++; }
    } else {
        unsigned rows = a->value & 63;
        if ((a->value & ~0x3fff) ||
            (rows != 63 && rows != 62 && rows != 61 && rows != 59 &&
             rows != 55 && rows != 47 && rows != 31)) {
            return "keypad_rows_write_unmodeled";
        }
        /* Column bits are external inputs; firmware's read/modify/write may
         * copy any sampled combination back. They never become output latches. */
        if (a->commit) { s->rows = rows; }
    }
    return NULL;
}
const DB2010RegMap *dbkey_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_REG(0, .id = 0, .alignment = 2, .backing_width = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = keypad_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = keypad_action}),
        DB_REG(2, .id = 1, .alignment = 2, .backing_width = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = keypad_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = keypad_action}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xfb010000, rules,
            ARRAY_SIZE(rules), "keypad_access_unmodeled");
        if (fault) { g_error("DB2010 keypad map: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *dbkey_access(void *opaque, DB2010Access *a)
{
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbkey_map(), a, &d);
    return fault ? fault : db2010_reg_execute(opaque, &d, a);
}
void dbkey_reset(DB2010Keypad *s)
{
    s->pressed = s->control = s->rows = 0;
    s->known = s->pending = false;
    s->transitions = s->applied_at = s->scans = s->acknowledgements = 0;
    irq_update(s);
}
typedef struct Change { DB2010Keypad *keypad; uint32_t pressed; } Change;
static void apply(CPUState *cpu, run_on_cpu_data data)
{
    Change *c = data.host_ptr;
    DB2010Keypad *s = c->keypad;
    if (s->pressed != c->pressed) {
        s->pressed = c->pressed;
        s->transitions++;
        s->applied_at = db2010_virtual_ns();
        s->pending = true;
        irq_update(s);
    }
}
static QList *pressed_list(DB2010Keypad *s)
{
    QList *list = qlist_new();
    for (unsigned i = 0; i < ARRAY_SIZE(contacts); i++) {
        if (s->pressed & (1u << i)) { qlist_append_str(list, contacts[i].name); }
    }
    return list;
}
static void collect(CPUState *cpu, run_on_cpu_data data)
{
    Change *c = data.host_ptr;
    c->pressed = c->keypad->pressed;
}
char *dbkey_get_pressed(Object *obj, Error **errp)
{
    DB2010Keypad *s = db2010_device_model(DEVICE(obj));
    Change c = {.keypad = s};
    if (s->cpu) { run_on_cpu(s->cpu, collect, RUN_ON_CPU_HOST_PTR(&c)); }
    DB2010Keypad snapshot = {.pressed = c.pressed};
    g_autoptr(QList) list = pressed_list(&snapshot);
    g_autoptr(GString) json = qobject_to_json(QOBJECT(list));
    return g_strdup(json->str);
}
void dbkey_set_pressed(Object *obj, const char *value, Error **errp)
{
    DB2010Keypad *s = db2010_device_model(DEVICE(obj));
    if (strlen(value) > 1024) { error_setg(errp, "keypad button array too large"); return; }
    QObject *parsed = qobject_from_json(value, errp);
    if (!parsed) { return; }
    g_autoptr(QList) list = qobject_to(QList, parsed);
    if (!list) { qobject_unref(parsed); }
    if (!list || qlist_size(list) > ARRAY_SIZE(contacts) || !s->cpu || !s->count) {
        error_setg(errp, "keypad pressed requires an active keypad and JSON button array"); return;
    }
    Change c = {.keypad = s};
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(list, entry) {
        QString *name = qobject_to(QString, qlist_entry_obj(entry));
        unsigned i;
        for (i = 0; name && i < ARRAY_SIZE(contacts); i++) {
            if (!strcmp(qstring_get_str(name), contacts[i].name)) { break; }
        }
        if (!name || i == ARRAY_SIZE(contacts) || (c.pressed & (1u << i))) {
            error_setg(errp, "unknown or duplicate keypad button"); return;
        }
        c.pressed |= 1u << i;
    }
    run_on_cpu(s->cpu, apply, RUN_ON_CPU_HOST_PTR(&c));
}
void dbkey_export(DB2010Keypad *s, QDict *snapshot)
{
    QDict *k = qdict_new();
    qdict_put(k, "pressed", pressed_list(s));
    qdict_put_int(k, "rows", s->rows);
    qdict_put_int(k, "columns", columns(s));
    if (s->known) { qdict_put_int(k, "control", s->control); } else { qdict_put_null(k, "control"); }
    qdict_put_bool(k, "pending", s->pending);
    qdict_put_int(k, "transitions", s->transitions);
    qdict_put_int(k, "applied_at", s->applied_at);
    qdict_put_int(k, "column_reads", s->scans);
    qdict_put_int(k, "acknowledgements", s->acknowledgements);
    qdict_put(snapshot, "keypad", k);
}

typedef struct InputState { DB2010Keypad *keypad; uint64_t count; } InputState;
static void collect_input(CPUState *cpu, run_on_cpu_data data)
{
    InputState *s = data.host_ptr;
    s->count = s->keypad->applied_at;
}
char *dbkey_get_input_state(Object *obj, Error **errp)
{
    DB2010Keypad *s = db2010_device_model(DEVICE(obj));
    InputState state = {.keypad = s};
    if (s->cpu) { run_on_cpu(s->cpu, collect_input, RUN_ON_CPU_HOST_PTR(&state)); }
    return g_strdup_printf("{\"applied_at\":%" PRIu64 "}", state.count);
}
