/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-rtc-retained.h"
#include "db2010-bus.h"

static const char *retained_field(void *opaque, const DB2010DecodedAccess *d,
                                  const DB2010Access *a, DB2010FieldRef *field)
{
    DB2010RtcRetained *r = opaque;
    *field = (DB2010FieldRef){.value = &r->bytes[d->element], .mask = 255};
    return NULL;
}

const DB2010RegMap *dbrtr_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_ARRAY(0, 25, .stride = 1, .alignment = 1, .backing_width = 1,
            .read = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH16,
                     .field = retained_field, .fault = "rtc_retained_access_unmodeled"},
            .write = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH16,
                .field = retained_field, .fault = "rtc_retained_access_unmodeled",
                .constraint = {.kind = DB_VALUE_RANGE, .maximum = 255,
                               .fault = "rtc_retained_upper_byte_unmodeled"}}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf905002a, rules,
            ARRAY_SIZE(rules), "rtc_retained_access_unmodeled");
        if (fault) { g_error("DB2010 retained RTC layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}

bool dbrtr_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = dbrtr_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}

const char *dbrtr_access(DB2010RtcRetained *r, uint32_t address,
                         unsigned size, bool write, uint32_t *value)
{
    DB2010Access a = {.address = address, .size = size, .write = write,
                      .value = *value, .commit = true};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbrtr_map(), &a, &d);
    if (!fault) { fault = db2010_reg_execute(r, &d, &a); }
    *value = a.value;
    return fault;
}

void dbrtr_export(DB2010RtcRetained *model, QDict *snapshot)
{

        GString *hex = g_string_sized_new(50);
        for (unsigned i = 0; i < sizeof(model->bytes); i++) {
            g_string_append_printf(hex, "%02x", model->bytes[i]);
        }
        QDict *retained = qdict_new();
        qdict_put_str(retained, "profile", "cold-zero-lowbyte-bank");
        qdict_put_str(retained, "bytes_hex", hex->str);g_string_free(hex, true);
        qdict_put(snapshot, "rtc_retained", retained);
    }
