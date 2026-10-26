/* Lifecycle and wiring only; protocol behavior remains in compiled backends.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "db2010-time.h"
#include "db2010-device.h"
#include "db2010-components.h"
#include "db2010-nand.h"
#include "db2010-f500-control.h"
#include "db2010-f500-port.h"
#include "db2010-fa-transfer.h"
#include "db2010-nor.h"
#include "db2010-simif.h"
#include "db2010-syscon.h"

#define TYPE_DB2010_CONTROLLER "db2010-controller"
#define TYPE_DB2010_PEER "db2010-peer"
#define TYPE_DB2010_PEER_BUS "db2010-peer-bus"

typedef struct DB2010DeviceSpec {
    DB2010DeviceId id;
    const char *name, *type;
    size_t size;
    bool peer;
} DB2010DeviceSpec;
#define DEVICE(id, name, type, model) {id, name, type, sizeof(model), false}
#define PEER(id, name, type, model) {id, name, type, sizeof(model), true}
static const DB2010DeviceSpec devices[DBDEV_COUNT] = {
    DEVICE(DBDEV_BANK, "register-bank", "db2010-register-bank", DB2010RegisterBank),
    DEVICE(DBDEV_POWER, "power-hold", "db2010-power-hold", DB2010Power),
    DEVICE(DBDEV_STARTUP, "startup-status", "db2010-startup-status", DB2010Startup),
    DEVICE(DBDEV_UART, "uart", "db2010-uart", DB2010Uart),
    DEVICE(DBDEV_MEMORY, "memory-controller", "db2010-memory-controller", DB2010MemoryController),
    DEVICE(DBDEV_REGIONS, "region-controls", "db2010-region-controls", DB2010RegionController),
    DEVICE(DBDEV_ABSENT, "absent-bank", "db2010-absent-bank", DB2010Absent),
    DEVICE(DBDEV_TIMER, "timer-irq", "db2010-timer-irq", DB2010Timer),
    DEVICE(DBDEV_RTC, "rtc", "db2010-rtc", DB2010Rtc),
    DEVICE(DBDEV_RETAINED, "rtc-retained", "db2010-rtc-retained", DB2010RtcRetained),
    DEVICE(DBDEV_DISPLAY, "pdi", "db2010-pdi", DB2010Display),
    DEVICE(DBDEV_DMA, "dma", "db2010-dma", DB2010Dma),
    DEVICE(DBDEV_I2C, "i2c", "db2010-i2c", DB2010I2c),
    DEVICE(DBDEV_NAND, "nand", "db2010-nand", DB2010Nand),
    DEVICE(DBDEV_NOR, "nor", "db2010-nor", DB2010Nor),
    DEVICE(DBDEV_F500_CONTROL, "f500-control", "db2010-f500-control", DB2010F500Control),
    DEVICE(DBDEV_F500_PRIMARY, "f500-primary", "db2010-f500-port", DB2010F500Port),
    DEVICE(DBDEV_F500_AUXILIARY, "f500-auxiliary", "db2010-f500-port", DB2010F500Port),
    DEVICE(DBDEV_FA, "fa-mailbox", "db2010-fa-mailbox", DB2010FaLink),
    DEVICE(DBDEV_FA_TRANSFER, "fa-transfer", "db2010-fa-transfer", DB2010FaTransfer),
    DEVICE(DBDEV_KEYPAD, "keypad", "db2010-keypad", DB2010Keypad),
    DEVICE(DBDEV_SIMIF, "simif", "db2010-simif", DB2010Simif),
    DEVICE(DBDEV_SYSCON, "syscon", "db2010-syscon", DB2010Syscon),
    PEER(DBDEV_PMIC, "pmic", "db2010-pmic", DB2010Pmic),
    PEER(DBDEV_FM, "fm", "db2010-fm", DB2010FmRadio),
    PEER(DBDEV_PANEL, "panel", "db2010-panel", DB2010Panel),
    PEER(DBDEV_DSP, "dsp", "db2010-dsp-peer", DB2010DspPeer),
};
#undef DEVICE
#undef PEER

typedef struct DB2010Window {
    MemoryRegion mr;
    uint32_t base, span;
    DeviceState *device;
    DB2010Owner *owner;
    DB2010AccessFn access;
    void *opaque;
    DB2010TimePort time;
    int priority;
} DB2010Window;

typedef struct DB2010LevelOutput {
    qemu_irq *irq;
    const bool *input_latch;
} DB2010LevelOutput;

typedef struct DB2010DeviceBody {
    const DB2010DeviceSpec *spec;
    bool active;
    void *model;
    MemoryRegion container, nor_region;
    MemoryRegion *parent_container;
    GPtrArray *windows, *children;
    BusState *peer_bus;
    qemu_irq irq[2];
    DB2010LevelOutput level_output[2];
    QEMUTimer *timer;
    DB2010DeadlineFn deadline;
    DB2010ServiceFn service;
    void *timing_opaque;
    DeviceState *dependencies[2];
    unsigned dependency_count;
} DB2010DeviceBody;

typedef struct DB2010Controller {
    SysBusDevice parent_obj;
    DB2010DeviceBody body;
} DB2010Controller;
typedef struct DB2010ControllerClass {
    SysBusDeviceClass parent_class;
    const DB2010DeviceSpec *spec;
} DB2010ControllerClass;
OBJECT_DECLARE_TYPE(DB2010Controller, DB2010ControllerClass, DB2010_CONTROLLER)

typedef struct DB2010Peer {
    DeviceState parent_obj;
    DB2010DeviceBody body;
} DB2010Peer;
typedef struct DB2010PeerClass {
    DeviceClass parent_class;
    const DB2010DeviceSpec *spec;
} DB2010PeerClass;
OBJECT_DECLARE_TYPE(DB2010Peer, DB2010PeerClass, DB2010_PEER)

static DB2010DeviceBody *body_of(Object *obj)
{
    if (object_dynamic_cast(obj, TYPE_DB2010_CONTROLLER)) {
        return &DB2010_CONTROLLER(obj)->body;
    }
    return &DB2010_PEER(obj)->body;
}

static uint64_t window_read(void *opaque, hwaddr offset, unsigned size)
{
    DB2010Window *w = opaque;
    DB2010Access a = {.address = w->base + offset, .size = size,
        .now = db2010_virtual_ns(), .pc = *w->time.pc,
        .commit = true};
    /* Architectural requests were previewed by the CPU guard. Retain the
     * original zero/no-effect result for rejected native split fragments. */
    const char *fault = w->access(w->opaque, &a);
    db2010_device_reschedule(w->device);
    return fault ? 0 : a.value;
}
static void window_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    DB2010Window *w = opaque;
    DB2010Access a = {.address = w->base + offset, .size = size, .value = value,
        .now = db2010_virtual_ns(), .pc = *w->time.pc,
        .write = true, .commit = true};
    w->access(w->opaque, &a);
    db2010_device_reschedule(w->device);
}
static const MemoryRegionOps window_ops = {
    .read = window_read, .write = window_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 1, .max_access_size = 4, .unaligned = true},
    .impl = {.min_access_size = 1, .max_access_size = 4, .unaligned = true},
};

static void reset_model(DeviceState *dev)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    if (b->timer) { timer_del(b->timer); }
    if (!b->active) { return; }
    switch (b->spec->id) {
    case DBDEV_BANK: dbreg_reset(b->model); break;
    case DBDEV_SYSCON: dbsys_reset(b->model); break;
    case DBDEV_POWER: db2010_power_reset(b->model); break;
    case DBDEV_STARTUP: db2010_startup_reset(b->model); break;
    case DBDEV_UART: db2010_uart_reset(b->model); break;
    case DBDEV_MEMORY: db2010_memory_controller_reset(b->model, true); break;
    case DBDEV_REGIONS: db2010_region_controller_reset(b->model); break;
    case DBDEV_ABSENT: db2010_absent_reset(b->model); break;
    case DBDEV_KEYPAD: dbkey_reset(b->model); break;
    case DBDEV_SIMIF: dbsim_reset(b->model); break;
    case DBDEV_TIMER: dbtimer_reset(b->model); break;
    case DBDEV_RTC: dbrtc_reset(b->model); break;
    case DBDEV_RETAINED: memset(b->model, 0, sizeof(DB2010RtcRetained)); break;
    case DBDEV_DISPLAY: {
        DB2010Display *d = b->model;
        dbpdi_reset(&d->pdi); db2010_display_capture_reset(d); break;
    }
    case DBDEV_DMA: dbdma_reset(b->model); break;
    case DBDEV_I2C: dbi_transport_reset(b->model); break;
    case DBDEV_NAND: dbn_reset(b->model); break;
    case DBDEV_NOR: db2010_nor_reset(b->model); break;
    case DBDEV_F500_CONTROL: dbfc_reset(b->model); break;
    case DBDEV_F500_PRIMARY: dbfp_reset(b->model); break;
    case DBDEV_FA: dbfm_reset(&((DB2010FaLink *)b->model)->fa_mailbox); break;
    case DBDEV_FA_TRANSFER: dbfa_reset(b->model); break;
    case DBDEV_PMIC: dbpmic_reset(b->model); break;
    case DBDEV_FM: dbfr_reset(b->model); break;
    case DBDEV_PANEL: dbpanel_reset(b->model); break;
    case DBDEV_DSP: dbdsp_reset(b->model); break;
    default: g_assert_not_reached();
    }
}

typedef struct CableChange { DB2010Uart *uart; bool present; } CableChange;
static void apply_cable(CPUState *cpu, run_on_cpu_data data)
{
    CableChange *c = data.host_ptr;
    db2010_data_uart_cable(c->uart, c->present);
}
static void collect_cable(CPUState *cpu, run_on_cpu_data data)
{
    CableChange *c = data.host_ptr;
    c->present = c->uart->data_present;
}
static bool get_cable(Object *obj, Error **errp)
{
    DB2010Uart *u = db2010_device_model(DEVICE(obj));
    CableChange c = {.uart=u};
    if (u->data_cpu) { run_on_cpu(u->data_cpu, collect_cable, RUN_ON_CPU_HOST_PTR(&c)); }
    return c.present;
}
static void set_cable(Object *obj, bool present, Error **errp)
{
    DB2010Uart *u = db2010_device_model(DEVICE(obj));
    if (!u->data_enabled || !u->data_bank || !u->data_bank->has_serial || !u->data_cpu) {
        error_setg(errp, "cable input requires the explicit UART0 serial and pad profiles"); return;
    }
    CableChange c = {.uart=u,.present=present};
    run_on_cpu(u->data_cpu, apply_cable, RUN_ON_CPU_HOST_PTR(&c));
}

static void timer_input(void *opaque, int input, int level)
{
    dbtimer_set_line(opaque, input, level);
}
static bool get_active(Object *obj, Error **errp)
{
    return body_of(obj)->active;
}
static char *get_contract(Object *obj, Error **errp)
{
    return g_strdup(body_of(obj)->spec->name);
}
static char *get_audio_output(Object *obj, Error **errp)
{
    return db2010_audio_output_status(((DB2010DspPeer *)body_of(obj)->model)->output);
}
static char *get_variant(Object *obj, Error **errp)
{
    DB2010F500Port *p = body_of(obj)->model;
    return g_strdup(p->config.variant ? p->config.variant->profile : "unconfigured");
}
static void input_clock_changed(void *opaque, ClockEvent event)
{
    DB2010DeviceBody *b = opaque;
    if (!b->active) { return; }
    uint64_t now = db2010_virtual_ns();
    if (event == ClockPreUpdate) {
        if (b->spec->id == DBDEV_TIMER) { dbtimer_advance(b->model, now); }
        else {
            DB2010Rtc *rtc = b->model;
            dbrtc_advance(rtc, now);
            dbrtc_update_irq(rtc);
        }
    } else if (b->timer && b->deadline) {
        int64_t due = b->deadline(b->timing_opaque);
        if (due < 0) { timer_del(b->timer); }
        else { timer_mod_ns(b->timer, due); }
    }
}
static void initialize_body(Object *obj, DB2010DeviceBody *b,
                            const DB2010DeviceSpec *spec)
{
    b->spec = spec; b->model = g_malloc0(spec->size);
    if (spec->id == DBDEV_SYSCON) {
        DB2010Syscon *s = b->model;
        s->cpu_clock = qdev_init_clock_out(DEVICE(obj), "cpu");
        s->bus_clock = qdev_init_clock_out(DEVICE(obj), "memory-bus");
        object_property_add_uint64_ptr(obj, "nominal-cpu-hz", &s->cpu_hz, OBJ_PROP_FLAG_READ);
    }
    if (spec->id == DBDEV_TIMER || spec->id == DBDEV_RTC) {
        Clock *clock = qdev_init_clock_in(DEVICE(obj), "clk", input_clock_changed,
                                         b, ClockPreUpdate | ClockUpdate);
        if (spec->id == DBDEV_TIMER) { ((DB2010Timer *)b->model)->clock = clock; }
        else { ((DB2010Rtc *)b->model)->clock = clock; }
    }
    if (spec->id == DBDEV_DSP) { object_property_add_str(obj, "audio-output", get_audio_output, NULL); }
    b->windows = g_ptr_array_new_with_free_func(g_free);
    b->children = g_ptr_array_new();
    object_property_add_str(obj, "contract", get_contract, NULL);
    object_property_add_bool(obj, "active", get_active, NULL);
    if (!spec->peer) {
        memory_region_init(&b->container, obj, spec->type, 1ULL << 32);
    }
    if (spec->id == DBDEV_F500_PRIMARY || spec->id == DBDEV_F500_CONTROL) {
        unsigned *latency = spec->id == DBDEV_F500_PRIMARY ?
            &((DB2010F500Port *)b->model)->config.latency :
            &((DB2010F500Control *)b->model)->config.latency;
        object_property_add_uint32_ptr(obj, "latency-ns", latency, OBJ_PROP_FLAG_READ);
        if (spec->id == DBDEV_F500_PRIMARY) { object_property_add_str(obj, "variant", get_variant, NULL); }
    }
    if (spec->id == DBDEV_UART) { object_property_add_bool(obj, "cable-present", get_cable, set_cable); }
    if (spec->id == DBDEV_KEYPAD) {
        object_property_add_str(obj, "input-state", dbkey_get_input_state, NULL);
        object_property_add_str(obj, "pressed", dbkey_get_pressed, dbkey_set_pressed);
    }
    if (spec->id == DBDEV_TIMER) {
        qdev_init_gpio_in_named_with_opaque(DEVICE(obj), timer_input, b->model, "sources", 10);
    }
    if (!spec->peer) {
        for (unsigned i = 0; i < 2; i++) { sysbus_init_irq(SYS_BUS_DEVICE(obj), &b->irq[i]); }
    } else {
        qdev_init_gpio_out_named(DEVICE(obj), b->irq, "irq", 2);
    }
}
static void controller_init(Object *obj)
{
    initialize_body(obj, &DB2010_CONTROLLER(obj)->body, DB2010_CONTROLLER_GET_CLASS(obj)->spec);
}
static void peer_init(Object *obj)
{
    initialize_body(obj, &DB2010_PEER(obj)->body, DB2010_PEER_GET_CLASS(obj)->spec);
}
static void finalize_body(DB2010DeviceBody *b)
{
    if (b->timer) { timer_free(b->timer); }
    switch (b->spec->id) {
    case DBDEV_BANK: {
        DB2010RegisterBank *s = b->model;
        if (s->registers) {
            for (unsigned i = 0; i < s->registers->len; i++) {
                DB2010Register *r = g_ptr_array_index(s->registers, i);
                if (r->allowed_writes) { g_array_unref(r->allowed_writes); }
                db2010_regmap_destroy(&r->map);
                g_free(r->name); g_free(r);
            }
            g_ptr_array_unref(s->registers);
        }
        break;
    }
    case DBDEV_DSP:
        db2010_audio_output_free(((DB2010DspPeer *)b->model)->output);
        qobject_unref(((DB2010DspPeer *)b->model)->encoded_frames);
        break;
    case DBDEV_UART: {
        DB2010Uart *s = b->model;
        for (unsigned i = 0; i < 4; i++) { qemu_chr_fe_deinit(&s->uart[i], false); }
        break;
    }
    case DBDEV_DISPLAY: {
        DB2010Display *s = b->model;
        if (s->display_pixels) { g_byte_array_unref(s->display_pixels); }
        qobject_unref(s->pdi_transfers); qobject_unref(s->display_bursts); qobject_unref(s->display_frames);
        break;
    }
    case DBDEV_FA_TRANSFER: {
        DB2010FaTransfer *s = b->model;
        if (s->memory) { g_hash_table_unref(s->memory); }
        if (s->initial) { g_hash_table_unref(s->initial); }
        break;
    }
    case DBDEV_NOR: {
        DB2010Nor *s = b->model;
        if (s->unknown_windows) { g_array_unref(s->unknown_windows); }
        break;
    }
    case DBDEV_NAND: {
        DB2010Nand *s = b->model;
        g_free(s->bytes); g_free(s->known); g_free(s->initial_bytes); g_free(s->initial_known);
        g_free(s->program_counts); g_free(s->erased_blocks); g_free(s->main_bytes);
        g_free(s->main_known_blocks); g_free(s->main_program_counts); g_free(s->copied_pages);
        qobject_unref(s->operations); qobject_unref(s->erase_operations);
        qobject_unref(s->main_operations); qobject_unref(s->copy_operations);
        break;
    }
    default: break;
    }
    /* QOM releases owned MemoryRegion children before instance finalization. */
    g_ptr_array_unref(b->windows); g_ptr_array_unref(b->children);
    g_free(b->model);
}
static void controller_finalize(Object *obj) { finalize_body(&DB2010_CONTROLLER(obj)->body); }
static void peer_finalize(Object *obj) { finalize_body(&DB2010_PEER(obj)->body); }

static void realize_model(DeviceState *dev, Error **errp)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    if (b->active && b->spec->id == DBDEV_F500_PRIMARY) {
        DB2010F500Port *p = b->model;
        if (!p->config.variant || !p->config.latency) {
            error_setg(errp, "DB2010 F500 port requires an immutable variant and latency"); return;
        }
    }
    if (b->active && b->spec->id == DBDEV_DSP) {
        DB2010DspPeer *p = b->model;
        if (p->config.mma_stream) { p->output = db2010_audio_output_new(&p->sink); }
    }
    if (b->active && b->spec->id == DBDEV_UART) { db2010_uart_connect(b->model); }
    for (unsigned i = 0; i < b->children->len; i++) {
        DeviceState *child = g_ptr_array_index(b->children, i);
        DB2010DeviceBody *peer = body_of(OBJECT(child));
        if (peer->spec->id != DBDEV_DSP) { peer->active = b->active; }
        if (!qdev_realize(child, b->peer_bus, errp)) { return; }
    }
    for (unsigned i = 0; i < b->windows->len; i++) {
        DB2010Window *w = g_ptr_array_index(b->windows, i);
        g_autofree char *name = g_strdup_printf("%s.%08x", b->spec->type, w->base);
        memory_region_init_io(&w->mr, OBJECT(dev), &window_ops, w, name, w->span);
        MemoryRegion *container = b->spec->peer ? b->parent_container : &b->container;
        memory_region_add_subregion_overlap(container, w->base, &w->mr, w->priority);
    }
    if (!b->spec->peer) {
        sysbus_init_mmio(SYS_BUS_DEVICE(dev), b->active && b->spec->id == DBDEV_NOR ? &b->nor_region : &b->container);
    }
}
static void unrealize_model(DeviceState *dev)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    if (b->timer) { timer_del(b->timer); }
}
static void controller_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->unrealize = unrealize_model;
    dc->realize = realize_model; dc->user_creatable = false; dc->hotpluggable = false;
    device_class_set_legacy_reset(dc, reset_model);
}
static void peer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->unrealize = unrealize_model;
    dc->realize = realize_model; dc->user_creatable = false; dc->hotpluggable = false;
    dc->bus_type = TYPE_DB2010_PEER_BUS;
    device_class_set_legacy_reset(dc, reset_model);
}
static void variant_class_init(ObjectClass *oc, const void *data)
{
    const DB2010DeviceSpec *spec = data;
    if (spec->peer) { DB2010_PEER_CLASS(oc)->spec = spec; }
    else { DB2010_CONTROLLER_CLASS(oc)->spec = spec; }
    DEVICE_CLASS(oc)->desc = spec->name;
}

DeviceState *db2010_device_new(Object *parent, DB2010DeviceId id)
{
    DeviceState *dev = qdev_new(devices[id].type);
    object_property_add_child(parent, devices[id].name, OBJECT(dev));
    object_unref(dev);
    return dev;
}
DeviceState *db2010_peer_new(DeviceState *parent, DB2010DeviceId id)
{
    DB2010DeviceBody *body = body_of(OBJECT(parent));
    if (!body->peer_bus) { body->peer_bus = qbus_new(TYPE_DB2010_PEER_BUS, parent, "peers"); }
    DeviceState *dev = db2010_device_new(OBJECT(parent), id);
    body_of(OBJECT(dev))->parent_container = &body->container;
    g_ptr_array_add(body->children, dev);
    return dev;
}
void *db2010_device_model(DeviceState *dev) { return body_of(OBJECT(dev))->model; }
void db2010_device_owner(DeviceState *dev, DB2010Owner *owner, DB2010TimePort time)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    b->active = true;
    if (b->spec->id == DBDEV_NOR) { return; } /* Retain native ROMD array/overlay views. */
    for (unsigned i = 0; i < owner->count; i++) {
        DB2010Window *w = g_new0(DB2010Window, 1);
        *w = (DB2010Window){.device = dev, .base = owner->claims[i].address, .span = owner->claims[i].span,
            .owner = owner, .access = owner->access, .opaque = owner->opaque, .time = time};
        g_ptr_array_add(b->windows, w);
    }
}
void db2010_device_fallback(DeviceState *dev, uint32_t page, DB2010TimePort time)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    b->active = true;
    DB2010Window *w = g_new0(DB2010Window, 1);
    *w = (DB2010Window){.device = dev, .base = page, .span = 4096, .access = db2010_bank_access,
        .opaque = b->model, .time = time, .priority = -1};
    g_ptr_array_add(b->windows, w);
}
void db2010_device_realize(DeviceState *dev)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    g_assert(!b->spec->peer);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    if (b->timer) { timer_del(b->timer); }
    if (!b->active) { return; }
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, b->spec->id == DBDEV_NOR ? 0x44000000 : 0,
                           b->spec->id == DBDEV_BANK ? -1 : 0);
}
static void set_signal(void *opaque, bool level) { qemu_set_irq(opaque, level); }
DB2010Signal db2010_device_signal(DeviceState *dev, unsigned output)
{
    return (DB2010Signal){body_of(OBJECT(dev))->irq[output], set_signal};
}
qemu_irq db2010_device_irq_line(DeviceState *dev, unsigned output)
{
    return body_of(OBJECT(dev))->irq[output];
}
/* DMA and DSP inputs retain their level until their source changes. Their
 * repeated unchanged updates are no-ops. PDI remains uncached: selecting its
 * vector consumes the edge latch independently of the next completion. */
static void set_level_signal(void *opaque, bool level)
{
    DB2010LevelOutput *output = opaque;
    /* Read the actual input latch, so reset or acknowledgement cannot leave
     * a stale output cache. Every update that can change state still uses GPIO. */
    if (output->input_latch && *output->input_latch == level) { return; }
    qemu_set_irq(*output->irq, level);
}
DB2010Signal db2010_device_level_signal(DeviceState *dev, unsigned output)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    b->level_output[output].irq = &b->irq[output];
    return (DB2010Signal){&b->level_output[output], set_level_signal};
}

void db2010_device_connect_irq(DeviceState *source, unsigned output, DeviceState *timer, unsigned input)
{
    DB2010DeviceBody *b = body_of(OBJECT(source));
    qemu_irq irq = qdev_get_gpio_in_named(timer, "sources", input);
    b->level_output[output].input_latch = dbtimer_input_latch(body_of(OBJECT(timer))->model, input);
    if (b->spec->peer) { qdev_connect_gpio_out_named(source, "irq", output, irq); }
    else { sysbus_connect_irq(SYS_BUS_DEVICE(source), output, irq); }
}
void db2010_device_cpu_irq(DeviceState *timer, qemu_irq cpu_irq)
{
    sysbus_connect_irq(SYS_BUS_DEVICE(timer), 0, cpu_irq);
}
MemoryRegion *db2010_device_nor_region(DeviceState *dev) { return &body_of(OBJECT(dev))->nor_region; }

void db2010_device_cancel_timer(DeviceState *dev)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    if (b->timer) { timer_del(b->timer); }
}
void db2010_device_reschedule(DeviceState *dev)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    if (b->timer) {
        int64_t due = b->deadline(b->timing_opaque);
        if (due < 0) { timer_del(b->timer); }
        else { timer_mod_ns(b->timer, due); }
    }
    for (unsigned i = 0; i < b->dependency_count; i++) {
        db2010_device_reschedule(b->dependencies[i]);
    }
}
static void service_timer(void *opaque)
{
    DeviceState *dev = opaque;
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    b->service(b->timing_opaque);
    db2010_device_reschedule(dev);
}
void db2010_device_timing(DeviceState *dev, DB2010DeadlineFn deadline,
                          DB2010ServiceFn service, void *opaque)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    b->deadline = deadline; b->service = service; b->timing_opaque = opaque;
    b->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, service_timer, dev);
}
void db2010_device_dependency(DeviceState *dev, DeviceState *dependent)
{
    DB2010DeviceBody *b = body_of(OBJECT(dev));
    assert(b->dependency_count < ARRAY_SIZE(b->dependencies));
    b->dependencies[b->dependency_count++] = dependent;
}

static void register_types(void)
{
    static const TypeInfo controller = {
        .name = TYPE_DB2010_CONTROLLER, .parent = TYPE_SYS_BUS_DEVICE, .abstract = true,
        .instance_size = sizeof(DB2010Controller), .class_size = sizeof(DB2010ControllerClass),
        .instance_init = controller_init, .instance_finalize = controller_finalize, .class_init = controller_class_init,
    };
    static const TypeInfo peer = {
        .name = TYPE_DB2010_PEER, .parent = TYPE_DEVICE, .abstract = true,
        .instance_size = sizeof(DB2010Peer), .class_size = sizeof(DB2010PeerClass),
        .instance_init = peer_init, .instance_finalize = peer_finalize, .class_init = peer_class_init,
    };
    static const TypeInfo bus = {.name = TYPE_DB2010_PEER_BUS, .parent = TYPE_BUS, .instance_size = sizeof(BusState)};
    type_register_static(&controller); type_register_static(&peer); type_register_static(&bus);
    for (unsigned i = 0; i < DBDEV_COUNT; i++) {
        if (i == DBDEV_F500_AUXILIARY) { continue; }
        TypeInfo info = {.name = devices[i].type, .parent = devices[i].peer ? TYPE_DB2010_PEER : TYPE_DB2010_CONTROLLER,
            .class_init = variant_class_init, .class_data = &devices[i]};
        type_register_static(&info);
    }
}
type_init(register_types);
