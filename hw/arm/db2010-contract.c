/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-bus.h"

bool db2010_claims(const DB2010RegisterSpec *specs, unsigned count,
                   uint32_t address, unsigned size)
{
    for (unsigned i = 0; i < count; i++) {
        if (address < (uint64_t)specs[i].address + specs[i].span &&
            (uint64_t)address + size > specs[i].address) {
            return true;
        }
    }
    return false;
}

const DB2010RegisterSpec *db2010_register_spec(const DB2010RegisterSpec *specs,
                                             unsigned count, uint32_t address,
                                             unsigned size)
{
    for (unsigned i = 0; i < count; i++) {
        const DB2010RegisterSpec *r = &specs[i];
        if (address == r->address && size < 32 && (r->widths & (1u << size)) &&
            (!r->alignment || !(address % r->alignment)) && size <= r->span) {
            return r;
        }
    }
    return NULL;
}

/* Map compilation is structural only: never evaluate guards or read state. */

static unsigned reg_width(unsigned widths)
{
    return widths & DB_WIDTH32 ? 4 : widths & DB_WIDTH16 ? 2 :
           widths & DB_WIDTH8 ? 1 : 0;
}

static bool valid_constraint(const DB2010ValueConstraint *v)
{
    switch (v->kind) {
    case DB_VALUE_ANY:
        return true;
    case DB_VALUE_SET:
        return v->fault && v->values && v->count;
    case DB_VALUE_MASKED:
        return v->fault && !(v->value & ~v->mask);
    case DB_VALUE_RANGE:
        return v->fault && v->minimum <= v->maximum;
    default:
        return false;
    }
}

static bool valid_action(const DB2010RegAction *a, bool write,
                         unsigned backing_width)
{
    if (a->kind == DB_ACTION_UNSET) {
        return !a->widths;
    }
    if (!a->widths || (a->widths & ~(DB_WIDTH8 | DB_WIDTH16 | DB_WIDTH32)) ||
        !valid_constraint(&a->constraint) ||
        (!write && a->constraint.kind != DB_VALUE_ANY)) {
        return false;
    }
    switch (a->kind) {
    case DB_ACTION_CALLBACK:
        return a->callback != NULL;
    case DB_ACTION_REJECT:
        return a->fault && !a->guard && a->constraint.kind == DB_VALUE_ANY;
    case DB_ACTION_CONSTANT:
        return !write;
    case DB_ACTION_FIELD:
    case DB_ACTION_READ_CLEAR:
    case DB_ACTION_W1C:
        return a->field && a->fault &&
               (backing_width == 1 || backing_width == 2 || backing_width == 4) &&
               (a->kind == DB_ACTION_FIELD ||
                (write ? a->kind == DB_ACTION_W1C :
                         a->kind == DB_ACTION_READ_CLEAR));
    case DB_ACTION_IGNORE:
        return write;
    default:
        return false;
    }
}

static const char *map_claim(GArray *claims, uint32_t address, unsigned span,
                             const DB2010RegRule *rule)
{
    if (db2010_claims((DB2010RegisterSpec *)claims->data, claims->len,
                      address, span)) {
        return "conflicting register map claims";
    }
    DB2010RegisterSpec claim = {
        .address = address, .span = span,
        .widths = rule->read.widths | rule->write.widths,
        .alignment = rule->alignment,
        .readable = rule->read.kind != DB_ACTION_UNSET &&
                    rule->read.kind != DB_ACTION_REJECT,
        .writable = rule->write.kind != DB_ACTION_UNSET &&
                    rule->write.kind != DB_ACTION_REJECT,
    };
    g_array_append_val(claims, claim);
    return NULL;
}

static const char *map_flatten(GArray *flat, GArray *claims, GHashTable *path,
    const DB2010RegRule *rules, unsigned count, uint64_t base,
    unsigned bank, bool claimed, uint64_t limit)
{
    if (!rules || !count) {
        return "empty register map rules";
    }
    for (unsigned i = 0; i < count; i++) {
        const DB2010RegRule *r = &rules[i];
        uint64_t address = base + r->offset;
        const char *fault;

        if (address >= limit || address >= (1ULL << 32) ||
            r->kind > DB_RULE_VIEW ||
            (r->alignment && (r->alignment & (r->alignment - 1)))) {
            return "invalid register map layout";
        }
        if (g_hash_table_contains(path, r)) {
            return "cyclic register map rules";
        }
        if (r->kind == DB_RULE_REPEAT) {
            if (!r->count || !r->children || !r->child_count || r->span ||
                r->read.kind || r->write.kind ||
                r->read.widths || r->write.widths ||
                (!r->bases && r->count > 1 && !r->stride) ||
                r->count > G_MAXUINT / sizeof(DB2010FlatRule) / r->child_count ||
                (uint64_t)bank * r->count + r->count - 1 > G_MAXUINT ||
                (!r->bases && address + (uint64_t)(r->count - 1) * r->stride >= limit)) {
                return "invalid repeated register group";
            }
            g_hash_table_add(path, (void *)r);
            for (unsigned j = 0; j < r->count; j++) {
                uint64_t offset = r->bases ? r->bases[j] : (uint64_t)j * r->stride;
                fault = map_flatten(flat, claims, path, r->children,
                    r->child_count, address + offset, bank * r->count + j,
                    claimed, limit);
                if (fault) {
                    return fault;
                }
            }
            g_hash_table_remove(path, r);
            continue;
        }
        if (r->kind != DB_RULE_WINDOW &&
            (r->children || r->child_count || r->bases || r->span)) {
            return "invalid register map children";
        }
        if (!valid_action(&r->read, false, r->backing_width) ||
            !valid_action(&r->write, true, r->backing_width)) {
            return "invalid register map action";
        }
        unsigned width = reg_width(r->read.widths | r->write.widths);
        uint64_t span;
        if (r->kind == DB_RULE_WINDOW) {
            span = r->span;
            if (!span || (r->children && !r->child_count) || (r->child_count &&
                (!r->children || r->read.kind || r->write.kind))) {
                return "invalid register map window";
            }
        } else {
            if (!width || !r->count ||
                (r->kind != DB_RULE_ARRAY && r->count != 1) ||
                (r->kind == DB_RULE_ARRAY && !r->stride)) {
                return "invalid register map elements";
            }
            span = (uint64_t)(r->count - 1) * r->stride + width;
        }
        if (!span || span > G_MAXUINT || address + span > limit) {
            return "invalid register map boundary";
        }
        if (!claimed) {
            fault = map_claim(claims, address, span, r);
            if (fault) {
                return fault;
            }
        }
        if (r->kind == DB_RULE_WINDOW && r->child_count) {
            g_hash_table_add(path, (void *)r);
            fault = map_flatten(flat, claims, path, r->children, r->child_count,
                                address, bank, true, address + span);
            g_hash_table_remove(path, r);
            if (fault) {
                return fault;
            }
            continue;
        }
        if (!width) {
            continue; /* Explicit guarded window, without valid accesses. */
        }
        if (width > span) {
            return "register map transfer exceeds window";
        }
        for (unsigned j = 0; j < flat->len; j++) {
            DB2010FlatRule *other = &g_array_index(flat, DB2010FlatRule, j);
            if (address < (uint64_t)other->address + other->span &&
                address + span > other->address) {
                return "conflicting register map rules";
            }
        }
        DB2010FlatRule entry = {r, address, span, bank};
        g_array_append_val(flat, entry);
    }
    return NULL;
}

const char *db2010_regmap_init(DB2010RegMap *map, uint32_t base,
    const DB2010RegRule *rules, unsigned count, const char *fault)
{
    if (map->rules || map->claims || !fault) {
        return "invalid register map initialization";
    }
    GArray *flat = g_array_new(false, false, sizeof(DB2010FlatRule));
    GArray *claims = g_array_new(false, false, sizeof(DB2010RegisterSpec));
    GHashTable *path = g_hash_table_new(g_direct_hash, g_direct_equal);
    const char *error = map_flatten(flat, claims, path, rules, count, base,
                                   0, false, 1ULL << 32);
    g_hash_table_unref(path);
    if (error) {
        g_array_unref(flat);
        g_array_unref(claims);
        return error;
    }
    *map = (DB2010RegMap){.count = flat->len, .claim_count = claims->len,
                         .fault = fault};
    map->rules = (DB2010FlatRule *)g_array_free(flat, false);
    map->claims = (DB2010RegisterSpec *)g_array_free(claims, false);
    return NULL;
}

void db2010_regmap_destroy(DB2010RegMap *map)
{
    g_free(map->rules);
    g_free(map->claims);
    *map = (DB2010RegMap){0};
}

const char *db2010_reg_decode(const DB2010RegMap *map,
    const DB2010Access *a, DB2010DecodedAccess *decoded)
{
    if (!map->fault) {
        return "uninitialized register map";
    }
    if (!a->size || a->size >= 32 ||
        (uint64_t)a->address + a->size > (1ULL << 32)) {
        return map->fault;
    }
    for (unsigned i = 0; i < map->count; i++) {
        const DB2010FlatRule *f = &map->rules[i];
        const DB2010RegRule *r = f->rule;
        const DB2010RegAction *action = a->write ? &r->write : &r->read;
        if (a->address < f->address ||
            (uint64_t)a->address + a->size > (uint64_t)f->address + f->span ||
            !(action->widths & (1u << a->size))) {
            continue;
        }
        unsigned offset = a->address - f->address;
        unsigned element = 0;
        if (r->kind != DB_RULE_WINDOW) {
            if (r->kind == DB_RULE_ARRAY) {
                if (offset % r->stride || offset / r->stride >= r->count) {
                    continue;
                }
                element = offset / r->stride;
            } else if (offset) {
                continue;
            }
        }
        unsigned alignment = r->alignment ? r->alignment : a->size;
        if (a->address % alignment) {
            continue;
        }
        *decoded = (DB2010DecodedAccess){.rule = r, .action = action,
            .rule_index = i, .id = r->id, .bank = f->bank, .element = element,
            .selector = r->selector, .address = a->address, .offset = offset,
            .size = a->size, .write = a->write};
        return NULL;
    }
    return map->fault;
}

const char *db2010_reg_value_check(const DB2010ValueConstraint *v, uint32_t value)
{
    switch (v->kind) {
    case DB_VALUE_ANY:
        return NULL;
    case DB_VALUE_SET:
        for (unsigned i = 0; i < v->count; i++) {
            if (value == v->values[i]) {
                return NULL;
            }
        }
        return v->fault;
    case DB_VALUE_MASKED:
        return (value & v->mask) == v->value ? NULL : v->fault;
    case DB_VALUE_RANGE:
        return value >= v->minimum && value <= v->maximum ? NULL : v->fault;
    default:
        g_assert_not_reached();
    }
}

static uint32_t field_load(void *value, unsigned width)
{
    uint8_t v8;
    uint16_t v16;
    uint32_t v32;
    switch (width) {
    case 1: memcpy(&v8, value, 1); return v8;
    case 2: memcpy(&v16, value, 2); return v16;
    case 4: memcpy(&v32, value, 4); return v32;
    default: g_assert_not_reached();
    }
}

static void field_store(void *value, unsigned width, uint32_t data)
{
    uint8_t v8 = data;
    uint16_t v16 = data;
    switch (width) {
    case 1: memcpy(value, &v8, 1); break;
    case 2: memcpy(value, &v16, 2); break;
    case 4: memcpy(value, &data, 4); break;
    default: g_assert_not_reached();
    }
}

static uint32_t size_mask(unsigned size)
{
    return size == 4 ? UINT32_MAX : (1u << (8 * size)) - 1;
}

static const char *reg_field_action(void *opaque, const DB2010DecodedAccess *d,
                                    DB2010Access *a)
{
    const DB2010RegAction *action = d->action;
    DB2010FieldRef field = {0};
    const char *fault = action->field(opaque, d, a, &field);
    if (fault) {
        return fault;
    }
    unsigned width = d->rule->backing_width;
    if (!field.value || (field.mask & ~size_mask(width))) {
        return "invalid register field reference";
    }
    uint32_t mask = field.mask & size_mask(a->size);
    bool reads = !a->write || action->kind == DB_ACTION_W1C;
    if (reads && field.known && (*field.known & mask) != mask) {
        return action->fault;
    }
    uint32_t value = field_load(field.value, width);
    uint32_t updated;
    if (!a->write) {
        a->value = value & mask;
        updated = action->kind == DB_ACTION_READ_CLEAR ? value & ~mask : value;
    } else if (action->kind == DB_ACTION_W1C) {
        updated = value & ~(a->value & mask);
    } else {
        updated = (value & ~mask) | (a->value & mask);
    }
    if (a->commit && (a->write || action->kind == DB_ACTION_READ_CLEAR)) {
        field_store(field.value, width, updated);
        if (field.known && a->write) {
            *field.known |= mask;
        }
    }
    return NULL;
}

const char *db2010_reg_execute(void *opaque, const DB2010DecodedAccess *d,
                               DB2010Access *a)
{
    if (a->address != d->address || a->size != d->size || a->write != d->write) {
        return "decoded register access mismatch";
    }
    const DB2010RegAction *action = d->action;
    const char *fault;
    if (action->kind == DB_ACTION_REJECT) {
        return action->fault;
    }
    if (a->write) {
        fault = db2010_reg_value_check(&action->constraint, a->value);
        if (fault) {
            return fault;
        }
    }
    if (action->guard) {
        fault = action->guard(opaque, d, a);
        if (fault) {
            return fault;
        }
    }
    switch (action->kind) {
    case DB_ACTION_CALLBACK:
        return action->callback(opaque, d, a);
    case DB_ACTION_CONSTANT:
        a->value = action->constant_by_width ?
                   action->constant & size_mask(a->size) : action->constant;
        return NULL;
    case DB_ACTION_IGNORE:
        return NULL;
    case DB_ACTION_FIELD:
    case DB_ACTION_READ_CLEAR:
    case DB_ACTION_W1C:
        return reg_field_action(opaque, d, a);
    default:
        g_assert_not_reached();
    }
}
