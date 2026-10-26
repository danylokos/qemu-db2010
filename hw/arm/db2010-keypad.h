/* W800 handset contacts. Electrical timing is an explicit model assumption.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_KEYPAD_H
#define DB2010_KEYPAD_H
#include "db2010-bus.h"
#include "db2010-links.h"
#include "hw/core/cpu.h"
#include "qapi/error.h"
typedef struct DB2010Keypad {
    CPUState *cpu;
    const uint64_t *count;
    DB2010Signal irq;
    uint32_t pressed;
    uint16_t control, rows;
    bool known, pending;
    uint64_t transitions, applied_at, scans, acknowledgements;
} DB2010Keypad;
const DB2010RegMap *dbkey_map(void);
const char *dbkey_access(void *opaque, DB2010Access *a);
void dbkey_reset(DB2010Keypad *s);
char *dbkey_get_input_state(Object *obj, Error **errp);
char *dbkey_get_pressed(Object *obj, Error **errp);
void dbkey_set_pressed(Object *obj, const char *value, Error **errp);
void dbkey_export(DB2010Keypad *s, QDict *snapshot);
#endif
