/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-pdi.h"


bool dbpdi_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = dbpdi_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}

typedef enum DB2010PdiEventKind { DBPDI_TRIGGER_PACKET, DBPDI_TRIGGER_DMA, DBPDI_STATUS, DBPDI_DMA_COMPLETE } DB2010PdiEventKind;
typedef struct DB2010PdiEvent {
    DB2010PdiEventKind kind;
    uint64_t count;
    uint32_t *value;
} DB2010PdiEvent;
static const char *dbpdi_packet_program(DB2010Pdi *p, unsigned *packet_size)
{
    unsigned n;
    static const uint8_t original[] = {
        0x1f,0x1d,0x09,0x00,0x1f,0x20,0x90,0x99,0x1f,0x28,0x9c,0x99,
        0x17,0x98,0,0,0x17,0x05,0x15,0x05,0x17,0xf8,0x1f,0,0x1f,0x08
    };
    /* Kind1 has no selector MMIO: observed slot0 write program only. */
    for (unsigned i = 0; i < sizeof(original); i++) {
        unsigned byte = (p->state.program[i / 4] >> (8 * (i % 4))) & 255;
        if (i != 14 && i != 15 && byte != original[i]) {
            return "pdi_program_execution_unmodeled";
        }
    }
    n = p->state.program[3] >> 16;
    if (!n || n > 24) { return "pdi_transfer_count_unmodeled"; }
    /* Original preparation preserves completion-control bit3 after
     * the first DMA stop. Accept it only after that native lifecycle. */
    if (p->state.control || (p->state.config != 0x200 &&
        !(p->state.config == 0x208 && (p->state.stop_commands == 1 ||
           (p->config.repeat_frames && p->state.stop_commands > 1))))) {
        return "pdi_transfer_config_unmodeled";
    }
    *packet_size = n; return NULL;
}
bool dbpdi_dma_write_selected(const DB2010Pdi *p)
{
    /* The image viewer disables the unused selector fields with FF while
     * retaining the same slot28 DMA write program. Other combinations need
     * their own program/transfer evidence.
     */
    return p->state.selector == 0x1c1c1c || p->state.selector == 0xffff1c;
}

static const char *dbpdi_dma_program(DB2010Pdi *p, unsigned *packet_size, unsigned *bytes)
{
    unsigned n, dma_bytes;
    static const uint8_t dma[] = {
        0x1f,0x1d,9,0,0x1f,0x20,0x90,0x99,0x1f,0x28,0x9c,0x99,
        0x17,0x98,10,0,0x17,5,0x15,5,0x17,0xf8,0x13,0x80,0,0,
        0x17,0x88,0,0,0x17,0x60,0x15,1,0x17,0xe0,0x17,0xe8,0x1f,0,0x1f,0x0d
    };
    unsigned slot = p->state.selector & 255;
    if (!dbpdi_dma_write_selected(p)) { return "pdi_dma_selector_unmodeled"; }
    for (unsigned i = 0; i < sizeof(dma); i++) {
        unsigned j = slot + i, byte = (p->state.program[j / 4] >> (8 * (j % 4))) & 255;
        if (i != 24 && i != 25 && i != 28 && i != 29 && byte != dma[i]) {
            return "pdi_dma_program_unmodeled";
        }
    }
    unsigned blocks = p->state.program[(slot + 24) / 4] & 0xffff;
    unsigned repeat = p->state.program[(slot + 28) / 4] & 0xffff;
    unsigned maximum = p->state.selector == 0xffff1c ? 116160 : 77440;
    if (!blocks || !repeat || (uint64_t)blocks * repeat > maximum) { return "pdi_dma_count_unmodeled"; }
    if (p->state.control || (p->state.config != 0x180200 &&
        !(p->config.repeat_frames && p->state.stop_commands && p->state.config == 0x180208))) {
        return "pdi_dma_config_unmodeled";
    }
    dma_bytes = blocks * repeat;
    n = 10;
    *packet_size = n; *bytes = dma_bytes; return NULL;
}
static const char *dbpdi_trigger_ready(DB2010Pdi *p, const DB2010PdiEvent *e)
{
    uint64_t count = e->count;
    unsigned n, dma_bytes = 0;
    const char *fault = e->kind == DBPDI_TRIGGER_PACKET ? dbpdi_packet_program(p, &n) :
        dbpdi_dma_program(p, &n, &dma_bytes);
    if (fault) { return fault; }
    unsigned required = (1u << ((n + 3) / 4)) - 1;
    /* Packet registers are hardware latches. The newer profile permits
     * original software to omit unchanged writes, but never unknown data.
     * Dirty bits remain for the separate completed-stream STOP command. */
    if (p->config.retained_latches ? (!p->state.mask_known || (p->state.data_known & required) != required) :
        (!p->state.mask_dirty || (p->state.data_dirty & required) != required)) {
        return "pdi_transfer_data_unknown";
    }
    p->state.packet_size = n;
    for (unsigned i = 0; i < n; i++) {
        unsigned reverse = n - 1 - i;
        p->state.packet[i] = p->state.data[reverse / 4] >> (8 * (3 - reverse % 4));
        p->state.rs[i] = (p->state.mask >> (32 - n + i)) & 1;
    }
    p->state.transfers++;
    p->state.dma_bytes = dma_bytes;
    p->state.dma_armed = e->kind == DBPDI_TRIGGER_DMA ? DBPDI_DMA_ARMED : DBPDI_DMA_IDLE; p->state.dma_complete = false;
    p->state.busy_until = count + 2048; /* Synthetic 32-observation delay converted at 64ns. */
    p->state.data_dirty = 0; p->state.mask_dirty = false;
    return NULL;
}
static const char *dbpdi_dispatch(DB2010Pdi *p, const DB2010PdiEvent *e)
{
    uint64_t count = e->count;
    if (e->kind == DBPDI_DMA_COMPLETE) {
        p->state.dma_armed = DBPDI_DMA_IDLE; p->state.dma_complete = true;
        p->state.busy_until = count; return NULL;
    }
    if (e->kind == DBPDI_STATUS) {
        *e->value = p->state.dma_armed || count < p->state.busy_until;
        if (*e->value) { p->state.busy_reads++; }
        return NULL;
    }
    if (e->kind == DBPDI_TRIGGER_DMA && p->state.dma_complete && !p->state.dma_armed && !p->state.dma_bytes &&
        p->state.config == 0x180208 && !p->state.mask_dirty && !p->state.data_dirty) {
        p->state.dma_complete = false; p->state.stop_commands++; return NULL;
    }
    if (p->state.dma_armed || count < p->state.busy_until) { return "pdi_trigger_while_busy"; }
    return dbpdi_trigger_ready(p, e);
}
void dbpdi_dma_complete(DB2010Pdi *p, uint64_t count)
{
    DB2010PdiEvent e = {.kind = DBPDI_DMA_COMPLETE, .count = count};
    dbpdi_dispatch(p, &e);
}

typedef enum DB2010PdiRegister { DBPDI_CONTROL_FIELD, DBPDI_CONFIG_FIELD, DBPDI_SELECTOR_FIELD, DBPDI_EVENT_MASK_FIELD } DB2010PdiRegister;
static const char *pdi_program_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Pdi *p = opaque;
    if (a->write) { p->state.program[d->element] = a->value; }
    else { a->value = p->state.program[d->element]; }
    return NULL;
}
static const char *pdi_data_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Pdi *p = opaque;
    unsigned field = ((const unsigned *)d->selector)[d->element];
    if (!a->write) {
        if (!(p->state.data_known & (1u << field))) { return "pdi_data_state_unknown"; }
        a->value = p->state.data[field];
    } else {
        p->state.data[field] = a->value;
        p->state.data_known |= 1u << field; p->state.data_dirty |= 1u << field;
    }
    return NULL;
}
static const char *pdi_mask_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010Pdi *p = opaque;
    if (!a->write) {
        if (!p->state.mask_known) { return "pdi_mask_state_unknown"; }
        a->value = p->state.mask;
    } else {
        p->state.mask = a->value; p->state.mask_known = true; p->state.mask_dirty = true;
    }
    return NULL;
}
static const char *pdi_field(void *opaque, const DB2010DecodedAccess *d,
                            const DB2010Access *a, DB2010FieldRef *field)
{
    DB2010Pdi *p = opaque;
    uint32_t *value;
    switch (d->id) {
    case DBPDI_CONTROL_FIELD: value = &p->state.control; break;
    case DBPDI_CONFIG_FIELD: value = &p->state.config; break;
    case DBPDI_SELECTOR_FIELD: value = &p->state.selector; break;
    case DBPDI_EVENT_MASK_FIELD: value = &p->state.event_mask; break;
    default: g_assert_not_reached();
    }
    *field = (DB2010FieldRef){.value = value, .mask = UINT32_MAX}; return NULL;
}
static const char *pdi_selector_guard(const void *opaque,
    const DB2010DecodedAccess *d, const DB2010Access *a)
{
    for (unsigned i = 0; i < 3; i++) {
        unsigned slot = (a->value >> (8 * i)) & 255;
        if (slot != 255 && (slot >= 104 || slot % 4)) { return "pdi_selector_unmodeled"; }
    }
    return NULL;
}
static const char *pdi_status_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010PdiEvent e = {.kind = DBPDI_STATUS, .count = a->now, .value = &a->value};
    return dbpdi_dispatch(opaque, &e);
}
static const char *pdi_trigger_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010PdiEvent e = {.kind = a->value == 1 ? DBPDI_TRIGGER_PACKET : DBPDI_TRIGGER_DMA,
                        .count = a->now, .value = &a->value};
    return dbpdi_dispatch(opaque, &e);
}
const DB2010RegMap *dbpdi_map(void)
{
    static const unsigned reverse_data[] = {5, 4, 3, 2, 1, 0};
    static const uint32_t controls[] = {0, 0x20, 0x120}, events[] = {0, 255, 65535}, triggers[] = {1, 5};
#define PDI_RW(fn) .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = fn}, \
                   .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = fn}
#define PDI_FIELD_READ(reason) .read = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH32, .field = pdi_field, .fault = reason}
    static const DB2010RegRule children[] = {
        DB_ARRAY(0, 26, .stride = 4, .alignment = 4, .backing_width = 4, PDI_RW(pdi_program_action)),
        DB_ARRAY(0x68, 6, .stride = 4, .alignment = 4, .backing_width = 4,
                 .selector = reverse_data, PDI_RW(pdi_data_action)),
        DB_REG(0x8c, .alignment = 4, .backing_width = 4, PDI_RW(pdi_mask_action)),
        DB_REG(0x90, .id = DBPDI_CONTROL_FIELD, .alignment = 4, .backing_width = 4,
            PDI_FIELD_READ("pdi_control_unmodeled"),
            .write = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH32, .field = pdi_field,
                .fault = "pdi_control_unmodeled", .constraint = {.kind = DB_VALUE_SET,
                .values = controls, .count = ARRAY_SIZE(controls), .fault = "pdi_control_unmodeled"}}),
        DB_REG(0xf8, .id = DBPDI_CONFIG_FIELD, .alignment = 4, .backing_width = 4,
            PDI_FIELD_READ("pdi_config_unmodeled"),
            .write = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH32, .field = pdi_field,
                .fault = "pdi_config_unmodeled", .constraint = {.kind = DB_VALUE_MASKED,
                .mask = ~0x0018ff08u, .value = 0, .fault = "pdi_config_unmodeled"}}),
        DB_REG(0xfc, .id = DBPDI_SELECTOR_FIELD, .alignment = 4, .backing_width = 4,
            PDI_FIELD_READ("pdi_selector_unmodeled"),
            .write = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH32, .field = pdi_field,
                .fault = "pdi_selector_unmodeled", .guard = pdi_selector_guard,
                .constraint = {.kind = DB_VALUE_MASKED, .mask = 0xff000000u, .value = 0,
                               .fault = "pdi_selector_unmodeled"}}),
        DB_REG(0xe0, .id = DBPDI_EVENT_MASK_FIELD, .alignment = 4, .backing_width = 4,
            PDI_FIELD_READ("pdi_event_mask_unmodeled"),
            .write = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH32, .field = pdi_field,
                .fault = "pdi_event_mask_unmodeled", .constraint = {.kind = DB_VALUE_SET,
                .values = events, .count = ARRAY_SIZE(events), .fault = "pdi_event_mask_unmodeled"}}),
        DB_REG(0xf4, .alignment = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = pdi_status_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "pdi_status_write_unmodeled"}),
        DB_REG(0xdc, .alignment = 4,
            .read = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "pdi_trigger_unmodeled"},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = pdi_trigger_action,
                .constraint = {.kind = DB_VALUE_SET, .values = triggers,
                    .count = ARRAY_SIZE(triggers), .fault = "pdi_trigger_unmodeled"}}),
    };
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 256, .children = children, .child_count = ARRAY_SIZE(children)),
    };
#undef PDI_RW
#undef PDI_FIELD_READ
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf7000100, rules,
            ARRAY_SIZE(rules), "pdi_register_unmodeled");
        if (fault) { g_error("DB2010 PDI layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *dbpdi_access(DB2010Pdi *p, uint32_t address,
                        unsigned size, uint64_t count, bool write, uint32_t *value)
{
    DB2010Access a = {.address = address, .size = size, .now = count, .write = write,
                      .value = *value, .commit = true};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbpdi_map(), &a, &d);
    if (fault && (size != 4 || address % 4)) { return "pdi_access_width_unmodeled"; }
    if (!fault) { fault = db2010_reg_execute(p, &d, &a); }
    *value = a.value;
    return fault;
}

void dbpdi_reset(DB2010Pdi *d)
{
    d->state = (DB2010PdiRuntime){0};
}
