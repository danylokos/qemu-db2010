/* Unidentified exact register bank; identities are deliberately not inferred.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-components.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

static const char *bank_action(void *opaque, const DB2010DecodedAccess *d,
                               DB2010Access *a)
{
    DB2010RegisterBank *s = opaque;
    DB2010Register *r = (DB2010Register *)d->selector;
    if (s->timer != NULL && a->write) {
        DB2010Timer preview = *s->timer;
        const char *fault = dbtimer_observe(&preview, a->address, a->value);
        if (fault) { return fault; }
    }
    if (!a->write) {
        if (s->has_serial && a->address == 0xf900000a) {
            DB2010Register *direction = dbreg_find(s, 0xf900000c);
            if (!direction || !dbreg_known(direction)) {
                a->fault_address = 0xf900000c; a->fault_size = 2;
                return "serial_gpio_direction_unknown";
            }
        }
        if (s->has_memorystick_slot && a->address == 0xf9000028) {
            DB2010Register *direction = dbreg_find(s, 0xf900002a);
            if (!direction || direction->width != 2 || !dbreg_known(direction)) {
                a->fault_address = 0xf900002a; a->fault_size = 2;
                return "memorystick_detect_direction_unknown";
            }
        }
        if (r->write_only) { return "device_register_read_unmodeled"; }
        uint32_t required = UINT32_MAX >> (8 * (4 - (r->f500_lowbyte_alias ? a->size : r->width)));
        if ((r->known_mask & required) != required) {
            return "device_register_initial_state_unknown";
        }
        uint32_t mask = r->read_mask;
        if (r->conditional) {
            DB2010Register *condition = dbreg_find(s, r->condition_address);
            if (!dbreg_known(condition) || condition->value != r->condition_value) { mask = 0; }
        }
        a->value = r->value | mask;
        if (s->has_serial && a->address == 0xf900000a) {
            DB2010Register *direction = dbreg_find(s, 0xf900000c);
            uint32_t inputs = 0x11 & ~direction->value;
            a->value = (a->value & ~inputs) | (s->serial_levels & inputs);
        }
        if (s->has_memorystick_slot && a->address == 0xf9000028) {
            DB2010Register *direction = dbreg_find(s, 0xf900002a);
            if (!(direction->value & 4)) { a->value |= 4; }
        }
        if (r->f500_lowbyte_alias && a->size == 1) { a->value &= 255; }
        return NULL;
    }
    if (r->read_only) { return "device_register_write_unmodeled"; }
    if (r->rtc_halfword_alias && a->size == 2 && a->value > 255) {
        return "rtc_marker_upper_byte_unmodeled";
    }
    uint32_t value = a->value & (UINT32_MAX >> (8 * (4 - a->size)));
    /* Native44A97C38 changes only UART0 flow bit20 after cable edges.
     * Retain exact setup constraints and reject changes to every other bit. */
    bool uart_flow_change = s->data_uart_flow && a->address == 0xfb06000c &&
                            a->size == 2 && dbreg_known(r) && (value ^ r->value) == 0x20;
    if (r->allowed_writes && !uart_flow_change) {
        DB2010ValueConstraint constraint = {.kind = DB_VALUE_SET,
            .values = (const uint32_t *)r->allowed_writes->data,
            .count = r->allowed_writes->len,
            .fault = "device_register_command_unmodeled"};
        const char *fault = db2010_reg_value_check(&constraint, value);
        if (fault) { return fault; }
    }
    if (!a->commit) { return NULL; }
    if (r->f500_lowbyte_alias && a->size == 1) {
        r->value = (r->value & 0xff00) | (value & 0xff);
        r->known_mask |= 255;
    } else {
        r->value = value & (UINT32_MAX >> (8 * (4 - r->width)));
        r->known_mask = UINT32_MAX >> (8 * (4 - r->width));
    }
    if (s->has_f500 && a->address == 0xf500081c) { s->f500_commands++; }
    if (s->timer != NULL) {
        dbtimer_observe(s->timer, a->address, r->value);
        dbtimer_update_irq(s->timer);
    }
    return NULL;
}

const DB2010RegMap *dbreg_map(DB2010Register *r)
{
    if (!r->map.count) {
        unsigned widths = (1u << r->width) |
            (r->rtc_halfword_alias ? DB_WIDTH16 : 0) |
            (r->f500_lowbyte_alias ? DB_WIDTH8 : 0);
        r->rule = (DB2010RegRule)DB_VIEW(0, .alignment = 1,
            .backing_width = r->width, .selector = r,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = widths,
                     .callback = bank_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = widths,
                      .callback = bank_action});
        const char *fault = db2010_regmap_init(&r->map, r->address, &r->rule, 1,
                                             "device_register_access_unmodeled");
        if (fault) { g_error("DB2010 configured register layout: %s", fault); }
    }
    return &r->map;
}

const char *db2010_bank_access(void *opaque, DB2010Access *a)
{
    DB2010Register *r = dbreg_find(opaque, a->address);
    if (!r) { return "device_register_access_unmodeled"; }
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbreg_map(r), a, &d);
    if (!fault) { fault = db2010_reg_execute(opaque, &d, a); }
    return fault;
}

const char *db2010_timer_access(void *opaque, DB2010Access *a)
{
    DB2010TimerLink *s = opaque;
    DB2010Timer preview = *s->timer, *timer = a->commit ? s->timer : &preview;
    const char *fault = dbtimer_advance(timer, a->now);
    if (!fault) { fault = dbtimer_access(timer, a->address, a->size, a->write, &a->value); }
    if (!fault && a->commit && !a->write) {
        if (a->address == 0xfb020310 && timer->state.i2c_selected) {
            dbi_irq_consumed(s->i2c); dbtimer_set_line(timer, DBIRQ_I2C, false);
        }
        if (*s->display_frame && a->address == 0xfb020210 && timer->state.pdi_selected) {
            dbtimer_set_line(timer, DBIRQ_PDI, false);
        }
    }
    if (!fault && a->commit) { dbtimer_update_irq(timer); }
    return fault;
}

DB2010Register *dbreg_find(DB2010RegisterBank *bank, uint32_t address)
{
    for (unsigned i = 0; i < bank->registers->len; i++) {
        DB2010Register *r = g_ptr_array_index(bank->registers, i);
        if (r->address == address) { return r; }
    }
    return NULL;
}

void dbreg_reset(DB2010RegisterBank *s)
{
    s->f500_commands = 0;
    for (unsigned i = 0; i < s->registers->len; i++) {
        DB2010Register *r = g_ptr_array_index(s->registers, i);
        r->value = r->initial;
        r->known_mask = r->initial_known ? UINT32_MAX >> (8 * (4 - r->width)) : 0;
    }
}

void dbreg_export(DB2010RegisterBank *model, QDict *snapshot)
{
    unsigned i;

        QDict *values = qdict_new();
        QDict *aliases = qdict_new();
        for (i = 0; i < model->registers->len; i++) {
            DB2010Register *r = g_ptr_array_index(model->registers, i);
            g_autofree char *key = g_strdup_printf("%u", r->address);
            if (dbreg_known(r)) {
                qdict_put_int(values, key, r->value);
            } else {
                qdict_put_null(values, key);
            }
            if (r->f500_lowbyte_alias) {
                QDict *alias = qdict_new();
                uint32_t mask = r->known_mask;
                qdict_put_int(alias, "known_mask", mask);
                qdict_put_int(alias, "known_value", r->value & mask);
                qdict_put(aliases, key, alias);
            }
        }
        qdict_put(snapshot, "device_registers", values);
        if (qdict_size(aliases)) {
            qdict_put(snapshot, "low_byte_aliases", aliases);
        } else {
            qobject_unref(aliases);
        }
    }
