/* Native DB2010 execution migration. SPDX-License-Identifier: GPL-2.0-or-later
 * CPU and RAM bytes use QEMU's native serializers. This early section creates
 * dynamic RAMBlocks before RAM loading and explicitly serializes model fields.
 * Host connections, pointers, configuration and diagnostic histories are absent.
 */
#include "qemu/osdep.h"
#include "db2010-internal.h"
#include "migration/vmstate.h"
#include "migration/qemu-file.h"
#include "hw/core/irq.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "system/runstate.h"

#define CHECKPOINT_VERSION 24

typedef struct Checkpoint {
    QEMUFile *file;
    bool load;
    bool invalid;
    uint32_t version;
} Checkpoint;

#define CP_SCALAR(x, get, put) do { \
    if (c->load) { (x) = get(c->file); } \
    else { put(c->file, (x)); } \
} while (0)
#define CP_U8(x) CP_SCALAR(x, qemu_get_byte, qemu_put_byte)
#define CP_U16(x) CP_SCALAR(x, qemu_get_be16, qemu_put_be16)
#define CP_U32(x) CP_SCALAR(x, qemu_get_be32, qemu_put_be32)
#define CP_U64(x) CP_SCALAR(x, qemu_get_be64, qemu_put_be64)
#define CP_FLOAT(x) do { \
    uint32_t bits; memcpy(&bits, &(x), 4); CP_U32(bits); \
    if (c->load) { memcpy(&(x), &bits, 4); } \
} while (0)
#define CP_BOOL(x) do { \
    uint8_t bit = (x); CP_U8(bit); \
    if (bit > 1) { c->invalid = true; } \
    if (c->load) { (x) = bit; } \
} while (0)

static void cp_buffer(Checkpoint *c, void *bytes, size_t size)
{
    if (c->load) {
        if (qemu_get_buffer(c->file, bytes, size) != size) { c->invalid = true; }
    } else { qemu_put_buffer(c->file, bytes, size); }
}

/* Byte-addressable maps remain in memory. The wire format is a width byte
 * followed by low-bit-first packed entries; unused high bits must be zero. */
static void cp_validity(Checkpoint *c, uint8_t *map, size_t size, uint8_t maximum)
{
    uint8_t width = 1;
    if (!c->load) {
        for (size_t i = 0; i < size; i++) {
            if (map[i] > maximum) { c->invalid = true; return; }
            if (map[i] == 2) { width = 2; }
        }
    }
    CP_U8(width);
    if ((width != 1 && width != 2) || (maximum == 1 && width != 1)) {
        c->invalid = true; return;
    }
    unsigned per_byte = 8 / width;
    bool assumed = false;
    uint8_t buffer[4096];
    for (size_t start = 0; start < size;) {
        size_t entries = MIN(size - start, sizeof(buffer) * per_byte);
        size_t bytes = DIV_ROUND_UP(entries, per_byte);
        if (!c->load) {
            memset(buffer, 0, bytes);
            for (size_t i = 0; i < entries; i++) {
                buffer[i / per_byte] |= map[start + i] << ((i % per_byte) * width);
            }
        }
        cp_buffer(c, buffer, bytes);
        if (c->invalid) { return; }
        if (c->load) {
            for (size_t i = 0; i < entries; i++) {
                uint8_t value = (buffer[i / per_byte] >> ((i % per_byte) * width)) & ((1 << width) - 1);
                if (value > maximum) { c->invalid = true; return; }
                map[start + i] = value;
                assumed |= value == 2;
            }
            unsigned used = entries % per_byte;
            if (used && buffer[bytes - 1] >> (used * width)) { c->invalid = true; return; }
        }
        start += entries;
    }
    if (c->load && width == 2 && !assumed) { c->invalid = true; }
}

#include "db2010-checkpoint-fields.inc"

static bool cp_equal32(Checkpoint *c, uint32_t expected)
{
    uint32_t value = expected;
    CP_U32(value);
    if (value != expected) { c->invalid = true; }
    return !c->invalid;
}

static bool cp_json(Checkpoint *c, QDict **value, unsigned max_size)
{
    g_autoptr(GString) text = !c->load && *value ? qobject_to_json(QOBJECT(*value)) : NULL;
    uint32_t length = text ? text->len : 0;
    CP_U32(length);
    if (length > max_size) { c->invalid = true; return false; }
    if (c->load) {
        qobject_unref(*value); *value = NULL;
        if (length) {
            g_autofree char *data = g_malloc0(length + 1);
            cp_buffer(c, data, length);
            *value = qobject_to(QDict, qobject_from_json(data, NULL));
            if (!*value) { c->invalid = true; }
        }
    } else if (length) { cp_buffer(c, text->str, length); }
    return !c->invalid;
}

static bool cp_regions(Checkpoint *c, DB2010MachineState *s)
{
    uint32_t count = s->regions->len;
    CP_U32(count);
    if (count < s->regions->len || count != s->regions->len) { return false; }
    for (unsigned i = 0; i < count; i++) {
        DB2010Region *r = i < s->regions->len ? g_ptr_array_index(s->regions, i) : NULL;
        uint32_t base = r ? r->base : 0, size = r ? r->size : 0;
        bool readonly = r && r->readonly, itcm = r && r->itcm, active = r && r->active;
        CP_U32(base); CP_U32(size); CP_BOOL(readonly); CP_BOOL(itcm); CP_BOOL(active);
        if (c->invalid) { return false; }
        /* RAM receive bitmaps are allocated before migration sections load.
         * Dynamic banks must already exist from the validated host layout. */
        if (!r) { return false; }
        if (r->base != base || r->size != size || r->readonly != readonly || r->itcm != itcm) {
            return false;
        }
        if (c->load) { r->active = active; memory_region_set_enabled(r->mr, active); }
        cp_validity(c, r->known, size, 2);
    }
    for (unsigned i = 0; i < 2; i++) {
        DB2010Tcm *t = &s->tcm[i];
        CP_U32(t->base); CP_BOOL(t->enabled);
        if (t->enabled && (!t->region || t->region->base != t->base || !t->region->active)) { return false; }
    }
    return !c->invalid;
}

static bool cp_nand(Checkpoint *c, DB2010Nand *n)
{
    CP_U32(n->mode); CP_U32(n->id_cursor); CP_U32(n->resets); CP_U32(n->cycles);
    CP_U32(n->program_size); CP_BOOL(n->read_open); CP_BOOL(n->main_selected);
    CP_U32(n->id_address); CP_U32(n->page); CP_U32(n->column); CP_U32(n->main_start_column);
    cp_buffer(c, n->addresses, sizeof(n->addresses)); cp_buffer(c, n->program, sizeof(n->program));
    cp_buffer(c, n->bytes, 65536 * 16); cp_validity(c, n->known, 65536 * 16, 1);
    for (unsigned i = 0; i < 65536; i++) { CP_U32(n->program_counts[i]); }
    if (n->erase_enabled) { cp_validity(c, n->erased_blocks, 2048, 1); }
    if (n->main_enabled) {
        cp_buffer(c, n->main_bytes, 65536 * 512); cp_validity(c, n->main_known_blocks, 2048, 1);
        cp_buffer(c, n->main_program_counts, 65536);
    }
    CP_U64(n->main_reads); CP_U32(n->copy_source); CP_U64(n->partial_after_copy);
    if (n->copy_enabled) { cp_validity(c, n->copied_pages, 65536, 1); }
    return n->mode <= DBN_COPY_COMPLETE && n->program_size <= 528 && n->cycles <= 3 &&
           n->page >= -1 && n->page < 65536 && n->column >= -1 && n->column <= 528;
}

static bool cp_fa(Checkpoint *c, DB2010FaTransfer *f)
{
    CP_U32(f->address); CP_U32(f->data); CP_U32(f->command);
    CP_BOOL(f->address_known); CP_BOOL(f->data_known);
    CP_U64(f->due); CP_U64(f->writes); CP_U64(f->reads); CP_U64(f->completions); CP_U64(f->polls);
    uint32_t count = g_hash_table_size(f->memory);
    CP_U32(count);
    if (count > 0x800000) { return false; }
    if (c->load) {
        g_hash_table_remove_all(f->memory);
        for (unsigned i = 0; i < count; i++) {
            uint32_t address = 0; uint16_t value = 0;
            CP_U32(address); CP_U16(value);
            if (address > 0x7fffff || g_hash_table_contains(f->memory, GUINT_TO_POINTER(address + 1))) { return false; }
            dbfa_set_word(f, address, value);
        }
    } else {
        /* Canonical ordering independent of GLib hash seed and insertion order. */
        for (uint32_t address = 0; address < 0x800000; address++) {
            uint16_t value;
            if (dbfa_word(f, address, &value)) { CP_U32(address); CP_U16(value); }
        }
    }
    return true;
}

static bool cp_machine(Checkpoint *c, DB2010MachineState *s)
{
    if (!cp_equal32(c, 0x44424350)) { return false; }
    c->version = CHECKPOINT_VERSION;
    CP_U32(c->version);
    if (c->version < 21 || c->version > CHECKPOINT_VERSION) { return false; }
    if (c->version >= 24) {
        bool guarded = s->research_guards;
        CP_BOOL(guarded);
        if (guarded != s->research_guards) {
            error_report("DB2010 checkpoint research guard mode differs"); return false;
        }
        CP_U64(s->native_count_base);
    } else if (!s->research_guards) {
        return false; /* historical state has guarded validity semantics */
    }
    char model[65];
    memcpy(model, s->checkpoint_model_identity, sizeof(model));
    cp_buffer(c, model, sizeof(model));
    if (memcmp(model, s->checkpoint_model_identity, sizeof(model))) { return false; }
    uint32_t features = s->has_nor | s->has_nand << 1 | s->nand->erase_enabled << 2 |
                        s->nand->main_enabled << 3 | s->nand->copy_enabled << 4;
    if (!cp_equal32(c, features) || !cp_regions(c, s)) { return false; }
    CP_U64(s->count); CP_U32(s->instruction_pc); CP_U64(s->wfi_waits);
    CP_U64(s->etx_calls); CP_U32(s->etx_last_request);
    CP_U64(s->kgen_inits); CP_U64(s->kgen_updates); CP_U64(s->kgen_finals); CP_U64(s->kgen_bytes);
    CP_U32(s->kgen_last_context);
    CP_U64(s->sha_inits); CP_U64(s->sha_updates); CP_U64(s->sha_finals); CP_U64(s->sha_bytes);
    CP_U32(s->sha_last_context);
    if (!cp_json(c, &s->stop, 65536)) { return false; }
    if (!cp_equal32(c, s->bank->registers->len)) { return false; }
    for (unsigned i = 0; i < s->bank->registers->len; i++) {
        DB2010Register *r = g_ptr_array_index(s->bank->registers, i);
        if (!cp_equal32(c, r->address)) { return false; }
        CP_U32(r->value); CP_U32(r->known_mask);
    }
    CP_U64(s->bank->f500_commands);
    cp_DB2010Power(c, s->power); cp_DB2010Startup(c, s->startup);
    cp_DB2010MemoryController(c, s->memory_controller);
    cp_DB2010RegionController(c, s->region_controller); cp_DB2010Absent(c, s->absent);
    cp_DB2010TimerRuntime(c, &s->timer->state); cp_DB2010RtcRuntime(c, &s->rtc->state);
    cp_DB2010RtcRetained(c, s->rtc_retained); cp_DB2010DmaRuntime(c, &s->dma->state);
    cp_DB2010I2cRuntime(c, &s->i2c->state); cp_DB2010PmicRuntime(c, &s->i2c->pmic->state);
    cp_DB2010FmRadioRuntime(c, &s->i2c->fm->state);
    cp_DB2010F500ControlRuntime(c, &s->f500_control->state);
    cp_DB2010F500PortRuntime(c, &s->f500_port->state); cp_DB2010F500PortRuntime(c, &s->f500_aux_port->state);
    cp_DB2010FaMailbox(c, &s->fa->fa_mailbox); cp_DB2010DspPeerRuntime(c, &s->fa->dsp_peer->state);
    if (s->has_fa_transfer && !cp_fa(c, s->fa_transfer)) { return false; }
    for (unsigned i = 0; i < 4; i++) { CP_U64(s->uart_port->uart_bytes[i]); }
    /* Optional transport state exists only for the configured profile. */
    if (s->uart_port->bluetooth_enabled) {
        CP_U16(s->uart_port->bluetooth_mask); CP_BOOL(s->uart_port->bluetooth_rx_timeout);
        CP_U8(s->uart_port->bluetooth_tx_pos); CP_U8(s->uart_port->bluetooth_rx_pos);
        CP_U32(s->uart_port->bluetooth_candidates); CP_U16(s->uart_port->bluetooth_opcode);
        CP_U8(s->uart_port->bluetooth_command);
        CP_U64(s->uart_port->bluetooth_commands); CP_U64(s->uart_port->bluetooth_resets); CP_U64(s->uart_port->bluetooth_rx_bytes);
        CP_BOOL(s->timer->state.bluetooth_pending); CP_BOOL(s->timer->state.bluetooth_selected);
        CP_U64(s->timer->state.bluetooth_irqs);
        if (!db2010_bluetooth_state_valid(s->uart_port)) { return false; }
    }
    if (s->uart_port->data_enabled) {
        DB2010Uart *u = s->uart_port;
        CP_BOOL(u->data_mode_known); CP_BOOL(u->data_timeout); CP_BOOL(u->data_autobaud);
        CP_U16(u->data_mode); CP_U16(u->data_mask); CP_U16(u->data_head); CP_U16(u->data_count);
        cp_buffer(c, u->data_rx, sizeof(u->data_rx)); CP_U8(u->data_training);
        CP_U64(u->data_rx_bytes); CP_U64(u->data_received); CP_U64(u->data_autobauds);
        {
            CP_BOOL(u->data_present); CP_U16(u->data_presence_events); CP_U64(u->data_cable_transitions);
            if (u->data_presence_events & ~0x180) { return false; }
            s->bank->serial_levels = (s->bank->serial_levels & ~1) | u->data_present;
        }
        if (u->data_head >= sizeof(u->data_rx) || u->data_count > sizeof(u->data_rx) ||
            u->data_training > 2 || (u->data_mask & ~0x1fff)) { return false; }
    }
    DB2010Keypad *k = s->keypad;
    CP_U32(k->pressed); CP_U16(k->control); CP_U16(k->rows); CP_BOOL(k->known); CP_BOOL(k->pending);
    CP_U64(k->transitions); CP_U64(k->applied_at); CP_U64(k->scans); CP_U64(k->acknowledgements);
    DB2010Simif *sim = s->simif;
    cp_buffer(c, sim->regs, sizeof(sim->regs));
    CP_BOOL(sim->active); CP_BOOL(sim->rx_ready); CP_BOOL(sim->tx_ready); CP_BOOL(sim->tx_event); CP_BOOL(sim->setup_known);
    CP_U64(sim->rx_due); CP_U64(sim->tx_due); CP_U64(sim->activations);
    CP_U64(sim->tx_bytes); CP_U64(sim->rx_bytes); CP_U64(sim->events);
    cp_DB2010SimCard(c, &sim->card);
    if (sim->card.input_len > sizeof(sim->card.input) || sim->card.output_len > DBSIM_QUEUE ||
        sim->card.output_head >= DBSIM_QUEUE || sim->card.response_len > sizeof(sim->card.response)) { return false; }
    DB2010Display *d = s->display;
    cp_DB2010PdiRuntime(c, &d->pdi.state); cp_DB2010Panel(c, d->panel);
    CP_BOOL(d->display_first_burst_done); CP_U64(d->display_frame_count); CP_U64(d->pdi_capture_count);
    CP_U64(d->display_due); CP_U64(d->display_started); CP_U64(d->display_completed);
    CP_U32(d->display_source); CP_BOOL(d->display_dma_irq_level);
    uint32_t pixels = d->display_pixels ? d->display_pixels->len : 0;
    CP_U32(pixels);
    if (pixels > DBPANEL_PIXELS * 3) { return false; }
    if (c->load) {
        if (!d->display_pixels) { d->display_pixels = g_byte_array_new(); }
        g_byte_array_set_size(d->display_pixels, pixels);
    }
    if (pixels) { cp_buffer(c, d->display_pixels->data, pixels); }
    QDict *latest = NULL;
    const QListEntry *entry;
    if (!c->load && d->display_frames) {
        QLIST_FOREACH_ENTRY(d->display_frames, entry) {
            qobject_unref(latest);
            latest = qdict_clone_shallow(qobject_to(QDict, qlist_entry_obj(entry)));
        }
        if (latest) { qdict_del(latest, "update_hex"); }
    }
    bool valid = cp_json(c, &latest, 256 * 1024);
    if (c->load && valid) {
        qobject_unref(d->display_frames); d->display_frames = qlist_new();
        if (latest) { qlist_append(d->display_frames, qobject_ref(latest)); }
    }
    qobject_unref(latest);
    if (!valid) { return false; }
    if (s->has_nor) {
        DB2010Nor *n = s->nor;
        bool romd = memory_region_is_romd(n->mr);
        CP_BOOL(romd);
        if (c->load) { memory_region_rom_device_set_romd(n->mr, romd); }
        for (unsigned i = 0; i < 259; i++) { CP_BOOL(n->locked[i]); CP_BOOL(n->down[i]); }
        CP_U16(n->rcr); CP_U64(n->commands); CP_U64(n->operations);
        CP_U64(n->overlay_reads); CP_U64(n->overlay_bytes);
        for (unsigned i = 0; i < 16; i++) { cp_DB2010NorPart(c, &n->part[i]); }
    }
    if (s->has_nand && !cp_nand(c, s->nand)) { return false; }
    return !c->invalid && !qemu_file_get_error(c->file);
}

static bool checkpoint_load(QEMUFile *f, void *pv, size_t size,
                            const VMStateField *field, Error **errp)
{
    DB2010MachineState *s = pv;
    Checkpoint context = {.file = f, .load = true};
    if (!cp_machine(&context, s)) { error_setg(errp, "invalid DB2010 checkpoint state"); return false; }
    /* Clock outputs are derived from the existing serialized register latches.
     * No extra checkpoint state or layout change is required. */
    dbsys_update(s->syscon);
    /* Limits belong to this invocation. Guarded dependency stops remain sticky. */
    if (s->stop && !strcmp(qdict_get_str(s->stop, "reason"), "instruction_limit")) {
        qobject_unref(s->stop); s->stop = NULL;
    }
    if (s->instruction_limited) {
        if (UINT64_MAX - s->count < s->limit) { error_setg(errp, "resumed limit overflows"); return false; }
        s->limit += s->count;
    }
    s->i2c->pmic->history_baseline = s->i2c->pmic->state.write_count;
    s->simif->history_baseline = s->simif->events;
    db2010_audio_output_reset(s->fa->dsp_peer->output);
    s->checkpoint_loaded = true;
    return true;
}

static bool checkpoint_save(QEMUFile *f, void *pv, size_t size,
                            const VMStateField *field, JSONWriter *vmdesc, Error **errp)
{
    Checkpoint context = {.file = f};
    if (runstate_is_running() || !cp_machine(&context, pv)) {
        error_setg(errp, "DB2010 checkpoint requires valid stopped state"); return false;
    }
    return true;
}

static const VMStateInfo checkpoint_info = {
    .name = "db2010-execution-v2", .load = checkpoint_load, .save = checkpoint_save,
};
static const VMStateDescription checkpoint_vmstate = {
    .name = "db2010-machine", .version_id = CHECKPOINT_VERSION,
    .minimum_version_id = 21, .early_setup = true,
    .fields = (const VMStateField[]) {
        {.name = "execution", .info = &checkpoint_info, .flags = VMS_SINGLE},
        VMSTATE_END_OF_LIST()
    },
};

static void checkpoint_running(void *opaque, bool running, RunState state)
{
    DB2010MachineState *s = opaque;
    if (!running || !s->checkpoint_loaded) { return; }
    s->checkpoint_loaded = false;
    /* CPU/RAM/virtual clock and connections are now all restored. Derive native
     * deadlines and CPU IRQ from saved latches without consuming device events. */
    dbtimer_update_irq(s->timer);
    for (unsigned i = 0; i < DBDEV_COUNT; i++) { db2010_device_reschedule(s->devices[i]); }
}

void db2010_checkpoint_register(DB2010MachineState *s)
{
    vmstate_register(NULL, 0, &checkpoint_vmstate, s);
    qemu_add_vm_change_state_handler(checkpoint_running, s);
}

char *get_checkpoint_layout(Object *obj, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    g_autoptr(QList) banks = qlist_new();
    for (unsigned i = 0; i < 2; i++) {
        if (!s->tcm[i].region) { continue; }
        QDict *item = qdict_new();
        qdict_put_int(item, "bank", i);
        qdict_put_int(item, "address", s->tcm[i].region->base);
        qlist_append(banks, item);
    }
    g_autoptr(GString) json = qobject_to_json(QOBJECT(banks));
    return g_strdup(json->str);
}

void db2010_checkpoint_prepare(DB2010MachineState *s, QDict *state)
{
    if (qdict_haskey(state, "checkpoint_model_identity")) {
        const char *identity = qdict_get_try_str(state, "checkpoint_model_identity");
        if (!identity || strlen(identity) != 64 || strspn(identity, "0123456789abcdef") != 64) {
            error_report("db2010: invalid checkpoint model identity"); exit(EXIT_FAILURE);
        }
        memcpy(s->checkpoint_model_identity, identity, 65);
    }
    if (!qdict_haskey(state, "checkpoint_tcm")) { return; }
    QList *banks = qobject_to(QList, qdict_get(state, "checkpoint_tcm"));
    const QListEntry *entry;
    if (!banks || qlist_size(banks) > 2) {
        error_report("db2010: invalid checkpoint TCM layout"); exit(EXIT_FAILURE);
    }
    QLIST_FOREACH_ENTRY(banks, entry) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));
        if (!item || qdict_size(item) != 2) {
            error_report("db2010: invalid checkpoint TCM bank"); exit(EXIT_FAILURE);
        }
        unsigned bank = input_uint(item, "bank", 1);
        uint32_t base = input_uint(item, "address", UINT32_MAX);
        if (s->tcm[bank].region || base % 0x10000 ||
            (uint64_t)base + 0x10000 > (1ULL << 32)) {
            error_report("db2010: invalid checkpoint TCM mapping"); exit(EXIT_FAILURE);
        }
        DB2010Region *r = add_region(s, bank ? "db2010.itcm" : "db2010.dtcm", base, 0x10000, false);
        r->itcm = bank;
        s->tcm[bank].region = r;
    }
}
