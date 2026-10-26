#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "db2010-internal.h"

void collect_progress(CPUState *cpu, run_on_cpu_data data)
{
    DB2010Progress *p = data.host_ptr;
    DB2010MachineState *s = p->machine;
    db2010_runtime_observe(s);
    p->observations = s->count;
    p->virtual_ns = db2010_virtual_ns();
    p->cpu_hz = s->syscon->cpu_hz;
    p->pc = s->instruction_pc;
    memcpy(p->uart_bytes, s->uart_port->uart_bytes, sizeof(p->uart_bytes));
    p->lcd_frames = s->display->display_frame_count;
    p->timer_ticks = s->has_timer ? s->timer->state.last_cycles : 0;
    p->timer_irqs = s->has_timer ? s->timer->state.irqs : 0;
    p->wfi_waits = s->wfi_waits; p->idle_ticks = s->timer->state.idle_cycles;
    p->pmic_transactions = s->i2c->state.transactions; p->fa_requests = s->fa->dsp_peer->state.requests;
    p->nor_commands = s->nor->commands;
    p->serial_present = s->uart_port->data_present;
    p->serial_ready = s->uart_port->data_mode_known && (s->uart_port->data_mode & 7) == 2 &&
                      (s->uart_port->data_mask & 0x41) == 0x41;
}

char *get_progress(Object *obj, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    DB2010Progress p = { .machine = s };
    g_autoptr(QDict) progress = qdict_new();
    g_autoptr(GString) text = NULL;
    if (s->cpu) {
        run_on_cpu(CPU(s->cpu), collect_progress, RUN_ON_CPU_HOST_PTR(&p));
    }
    qdict_put_bool(progress, "research_guards", s->research_guards);
    qdict_put_str(progress, "counter_kind", s->research_guards ? "research-observations" : "qemu-icount");
    qdict_put_int(progress, "observations", p.observations);
    qdict_put_int(progress, "virtual_ns", p.virtual_ns);
    if (p.cpu_hz) { qdict_put_int(progress, "cpu_hz", p.cpu_hz); }
    else { qdict_put_null(progress, "cpu_hz"); }
    qdict_put_str(progress, "timing_profile", "qemu-virtual-v1");
    qdict_put_int(progress, "pc", p.pc);
    qdict_put_bool(progress, "running", runstate_is_running());
    qdict_put_int(progress,"wfi_waits",p.wfi_waits);
    qdict_put_int(progress,"idle_timer_ticks",p.idle_ticks);
    qdict_put_int(progress,"pmic_transactions",p.pmic_transactions);
    qdict_put_int(progress,"fa_requests",p.fa_requests);
    qdict_put_int(progress,"nor_commands",p.nor_commands);
    QList *uart = qlist_new();
    for (unsigned i = 0; i < 4; i++) {
        qlist_append_int(uart, p.uart_bytes[i]);
    }
    qdict_put(progress, "uart_bytes", uart);
    if (s->uart_port->data_enabled) {
        qdict_put_bool(progress, "serial_input_ready", p.serial_ready);
        qdict_put_bool(progress, "serial_cable_present", p.serial_present);
    }
    qdict_put_int(progress, "lcd_frames", p.lcd_frames);
    if (s->has_timer) {
        qdict_put_int(progress, "timer_ticks", p.timer_ticks);
        qdict_put_int(progress, "timer_hz", clock_get_hz(s->timer->clock));
        qdict_put_int(progress, "timer_irqs", p.timer_irqs);
    }
    text = qobject_to_json(QOBJECT(progress));
    return g_strdup(text->str);
}

char *get_summary(Object *obj, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    if (runstate_is_running()) {
        error_setg(errp, "db2010 summary requires a stopped VM");
        return NULL;
    }
    g_autofree char *progress = get_progress(obj, errp);
    g_autoptr(QDict) summary = qobject_to(QDict, qobject_from_json(progress, errp));
    g_autoptr(GString) text = NULL;
    if (!summary) { return NULL; }
    if (s->stop) { qdict_put(summary, "stop", qobject_ref(s->stop)); }
    text = qobject_to_json(QOBJECT(summary));
    return g_strdup(text->str);
}

char *get_snapshot(Object *obj, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    g_autoptr(QDict) snapshot = qdict_new();
    g_autoptr(GString) text = NULL;
    QList *registers = qlist_new();
    QList *tcm = qlist_new();
    unsigned i;
    if (!s->cpu) {
        return g_strdup("{}");
    }
    if (runstate_is_running()) {
        error_setg(errp, "db2010 snapshot requires a stopped VM");
        return NULL;
    }
    for (i = 0; i < 16; i++) {
        qlist_append_int(registers, s->cpu->env.regs[i]);
    }
    for (i = 0; i < 2; i++) {
        qlist_append_int(tcm, s->tcm[i].base | 28 | s->tcm[i].enabled);
    }
    qdict_put(snapshot, "registers", registers);
    qdict_put(snapshot, "tcm", tcm);
    qdict_put_int(snapshot, "cpsr", cpsr_read(&s->cpu->env));
    qdict_put_int(snapshot, "sctlr", s->cpu->env.cp15.sctlr_el[1]);
    db2010_runtime_observe(s);
    qdict_put_bool(snapshot, "research_guards", s->research_guards);
    qdict_put_str(snapshot, "counter_kind", s->research_guards ? "research-observations" : "qemu-icount");
    qdict_put_int(snapshot, "observations", s->count);
    qdict_put_int(snapshot, "virtual_ns", db2010_virtual_ns());
    if (s->syscon->cpu_hz) { qdict_put_int(snapshot, "cpu_hz", s->syscon->cpu_hz); }
    else { qdict_put_null(snapshot, "cpu_hz"); }
    dbsys_export(s->syscon, snapshot);
    qdict_put_str(snapshot, "timing_profile", "qemu-virtual-v1");
    qdict_put_int(snapshot, "icount_shift", 6);
    qdict_put_int(snapshot, "timer1_hz", clock_get_hz(s->timer->clock));
    qdict_put_int(snapshot, "rtc_hz", clock_get_hz(s->rtc->clock));
    if (s->has_f500_port) { qdict_put(snapshot, "f500_command_port", dbfp_snapshot(s->f500_port)); }
    if (s->has_f500_aux_port) { qdict_put(snapshot, "f500_aux_port", dbfp_snapshot(s->f500_aux_port)); }
    if (s->has_f500_control) { dbfc_export(s->f500_control, snapshot); }
    if (s->has_startup_status) { db2010_startup_export(s->startup, snapshot); }
    if (s->fa->has_dsp_peer) { qdict_put(snapshot, "dsp_peer", dbdsp_snapshot(s->fa->dsp_peer)); }
    if (s->fa->has_fa_mailbox) {
        QDict *mailbox = dbfm_snapshot(&s->fa->fa_mailbox);
        if (s->fa->has_dsp_peer) { qdict_put_bool(mailbox, "peer_consumption_modeled", true); }
        qdict_put(snapshot, "fa_mailbox_tx", mailbox);
    }
    if (s->dma->config.fa_tx) { db2010_fa_dma_export(s->fa, snapshot); }
    if (s->has_fa_transfer) {
        qdict_put(snapshot, "fa_transfer", dbfa_snapshot(s->fa_transfer));
    }
    if (s->synthetic_etx) {
        QDict *etx = qdict_new();
        qdict_put_str(etx, "profile", "gdfs-static-8040-status-success");
        qdict_put_bool(etx, "synthetic", true);
        qdict_put_bool(etx, "crypto_verification_established", false);
        qdict_put_int(etx, "completed_calls", s->etx_calls);
        qdict_put_int(etx, "last_request", s->etx_last_request);
        qdict_put(snapshot, "synthetic_etx", etx);
    }
    if (s->synthetic_sha) {
        QDict *sha = qdict_new();
        qdict_put_str(sha, "profile", "sha1-le92-init-update-final-8040");
        qdict_put_bool(sha, "synthetic", true);
        qdict_put_bool(sha, "ROM_equivalence_established", false);
        qdict_put_int(sha, "init_calls", s->sha_inits);
        qdict_put_int(sha, "update_calls", s->sha_updates);
        qdict_put_int(sha, "final_calls", s->sha_finals);
        qdict_put_int(sha, "input_bytes", s->sha_bytes);
        qdict_put_int(sha, "last_context", s->sha_last_context);
        qdict_put(snapshot, "synthetic_sha", sha);
    }
    if (s->synthetic_kgen) {
        QDict *kgen = qdict_new();
        qdict_put_str(kgen, "profile", s->kgen_final ? "sha1-le92-init-update-final-8040" : "sha1-le92-init-update-8040");
        qdict_put_bool(kgen, "synthetic", true);
        qdict_put_bool(kgen, "ROM_equivalence_established", false);
        qdict_put_bool(kgen, "final_supported", s->kgen_final);
        if (s->kgen_final) { qdict_put_int(kgen, "final_calls", s->kgen_finals); }
        qdict_put_int(kgen, "init_calls", s->kgen_inits);
        qdict_put_int(kgen, "update_calls", s->kgen_updates);
        qdict_put_int(kgen, "input_bytes", s->kgen_bytes);
        qdict_put_int(kgen, "last_context", s->kgen_last_context);
        qdict_put(snapshot, "synthetic_kgen", kgen);
    }
    if (s->display->has_pdi) { db2010_display_export(s->display, snapshot); }
    if (s->bank->has_f500) { qdict_put_int(snapshot, "f500_zero_commands", s->bank->f500_commands); }
    if (s->has_nand) { dbn_export(s->nand, snapshot); }
    if (s->has_i2c) { dbi_export(s->i2c, snapshot); }
    if (s->has_dma) { dbdma_export(s->dma, snapshot); }
    if (s->has_rtc) { dbrtc_export(s->rtc, snapshot, db2010_virtual_ns(), 0); }
    if (s->has_rtc_retained) { dbrtr_export(s->rtc_retained, snapshot); }
    if (s->has_uart) { db2010_uart_export(s->uart_port, snapshot); }
    if (s->has_timer) { dbtimer_export(s->timer, snapshot, s->irq_events); }
    if (s->has_nor) {
        qdict_put(snapshot, "nor", db2010_nor_snapshot(s->nor));
    }
    if (s->has_absent_bank) { db2010_absent_export(s->absent, snapshot); }
    if (s->has_power_hold) { db2010_power_export(s->power, snapshot); }
    if (s->has_simif) { dbsim_export(s->simif, snapshot); }
    if (s->has_keypad) { dbkey_export(s->keypad, snapshot); }
    if (s->bank->registers->len) { dbreg_export(s->bank, snapshot); }
    if (s->bank->has_memorystick_slot) {
        QDict *slot = qdict_new();
        qdict_put_str(slot, "profile", "gpio42-empty-slot");
        qdict_put_bool(slot, "card_present", false);
        qdict_put_int(slot, "detect_port", 4);
        qdict_put_int(slot, "detect_mask", 4);
        qdict_put_bool(slot, "direction_sensitive", true);
        qdict_put(snapshot, "memorystick_slot", slot);
    }
    if (s->has_region_controls) { db2010_region_export(s->region_controller, snapshot); qdict_put_int(snapshot, "ttbr", s->cpu->env.cp15.ttbr0_ns); }
    if (s->has_controller) { db2010_controller_export(s->memory_controller, snapshot); }
    if (s->stop) {
        qdict_put(snapshot, "stop", qobject_ref(s->stop));
    }
    text = qobject_to_json(QOBJECT(snapshot));
    return g_strdup(text->str);
}

/* Freeze one stopped snapshot, then stream it in bounded replies. Base64 keeps
 * byte offsets valid even when a chunk boundary splits a UTF-8 character. The
 * cursor is diagnostic state only; it never changes CPU/peripheral state. */
void set_snapshot_chunk(Object *obj, const char *value, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    if (strcmp(value, "start") && strcmp(value, "release")) {
        error_setg(errp, "snapshot-chunk expects start or release");
        return;
    }
    g_clear_pointer(&s->snapshot_json, g_free);
    s->snapshot_size = s->snapshot_offset = 0;
    if (!strcmp(value, "start")) {
        s->snapshot_json = get_snapshot(obj, errp);
        if (s->snapshot_json) { s->snapshot_size = strlen(s->snapshot_json); }
    }
}

char *get_snapshot_chunk(Object *obj, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    g_autoptr(QDict) chunk = qdict_new();
    g_autoptr(GString) text = NULL;
    if (!s->snapshot_json) {
        error_setg(errp, "snapshot-chunk requires start");
        return NULL;
    }
    size_t length = MIN(s->snapshot_size - s->snapshot_offset, 256 * 1024);
    g_autofree char *data = g_base64_encode(
        (const guchar *)s->snapshot_json + s->snapshot_offset, length);
    qdict_put_int(chunk, "offset", s->snapshot_offset);
    qdict_put_int(chunk, "total", s->snapshot_size);
    qdict_put_str(chunk, "data_base64", data);
    s->snapshot_offset += length;
    if (s->snapshot_offset == s->snapshot_size) {
        g_clear_pointer(&s->snapshot_json, g_free);
        s->snapshot_size = s->snapshot_offset = 0;
    }
    text = qobject_to_json(QOBJECT(chunk));
    return g_strdup(text->str);
}

/* Translation may inspect bytes that never execute. The runtime fetch/data
 * guards reject this placeholder aperture before any guest-visible effect. */

/* Copy only the latest completed transfer's panel on the CPU thread. In-flight
 * DMA can already have changed GRAM: exporting it would expose a torn panel. */
typedef struct DB2010DisplayObservation {
    DB2010MachineState *machine;
    QDict *panel;
} DB2010DisplayObservation;

static void collect_display(CPUState *cpu, run_on_cpu_data data)
{
    DB2010DisplayObservation *observation = data.host_ptr;
    DB2010Display *display = observation->machine->display;
    QDict *panel = observation->panel;
    const QListEntry *entry = display->display_frames ? QTAILQ_LAST(&display->display_frames->head) : NULL;
    const QDict *frame = entry ? qobject_to(QDict, qlist_entry_obj(entry)) : NULL;
    g_autoptr(GString) single = NULL;
    const char *pixels = frame ? qdict_get_str(frame, "pixels_hex") : "";
    if (!frame && display->display_completed &&
        display->display_pixels->len == DBPANEL_PIXELS * 2) {
        single = g_string_sized_new(DBPANEL_PIXELS * 4);
        for (unsigned i = 0; i < display->display_pixels->len; i++) {
            g_string_append_printf(single, "%02x", display->display_pixels->data[i]);
        }
        pixels = single->str;
    }
    unsigned known = frame && qdict_haskey(frame, "panel_known_pixels") ?
        qdict_get_int(frame, "panel_known_pixels") :
        (strlen(pixels) == DBPANEL_PIXELS * 4 ? DBPANEL_PIXELS : 0);
    qdict_put_int(panel, "width", 176);
    qdict_put_int(panel, "height", 220);
    qdict_put_str(panel, "pixel_format", "rgb565-be");
    qdict_put_int(panel, "known_pixels", known);
    qdict_put_int(panel, "frames", frame ? display->display_frame_count : single ? 1 : 0);
    qdict_put_int(panel, "completed", frame ? qdict_get_int(frame, "completed") : single ? display->display_completed : 0);
    qdict_put_int(panel, "started", frame ? qdict_get_int(frame, "started") : single ? display->display_started : 0);
    qdict_put_str(panel, "pixels_hex", known == DBPANEL_PIXELS ? pixels : "");
}

char *get_display(Object *obj, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    g_autoptr(QDict) panel = qdict_new();
    g_autoptr(GString) text = NULL;
    DB2010DisplayObservation observation = { .machine = s, .panel = panel };
    if (s->cpu) {
        run_on_cpu(CPU(s->cpu), collect_display, RUN_ON_CPU_HOST_PTR(&observation));
    }
    text = qobject_to_json(QOBJECT(panel));
    return g_strdup(text->str);
}

/* Export storage bytes only. Never expose command-mode views, validity maps,
 * pending buffers or the internal NOR prefix. Unknown cells are filled in a
 * temporary buffer; exporting cannot establish contents in the running model. */
static bool export_flash_bytes(const char *directory, const char *name,
                               const uint8_t *bytes, const uint8_t *known,
                               size_t size, size_t known_stride, Error **errp)
{
    g_autofree char *path = g_build_filename(directory, name, NULL);
    g_autofree uint8_t *image = g_malloc(size);
    g_autoptr(GError) error = NULL;
    for (size_t i = 0; i < size; i++) {
        image[i] = known[i / known_stride] ? bytes[i] : 255;
    }
    if (!g_file_set_contents(path, (const char *)image, size, &error)) {
        error_setg(errp, "db2010 flash export %s: %s", name, error->message);
        return false;
    }
    return true;
}

void set_flash_byte_export(Object *obj, const char *value, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    if (runstate_is_running()) {
        error_setg(errp, "db2010 flash export requires a stopped VM");
        return;
    }
    if (!s->has_nor || !s->has_nand || !s->nand->main_enabled) {
        error_setg(errp, "db2010 flash export requires NOR and NAND main storage");
        return;
    }
    static const struct { const char *name; uint32_t start, end; } nor[] = {
        { "main.bin", 0x44020000, 0x45400000 },
        { "fs.bin", 0x45400000, 0x45f00000 },
        { "gdfs.bin", 0x45f00000, 0x46000000 },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(nor); i++) {
        unsigned offset = nor[i].start - DBNOR_BASE;
        if (!export_flash_bytes(value, nor[i].name, s->nor->array + offset,
                                s->nor->known + offset,
                                nor[i].end - nor[i].start, 1, errp)) {
            return;
        }
    }
    if (!export_flash_bytes(value, "nand-main.bin", s->nand->main_bytes,
                            s->nand->main_known_blocks, 65536 * 512,
                            32 * 512, errp)) {
        return;
    }
    export_flash_bytes(value, "nand-spare.bin", s->nand->bytes,
                       s->nand->known, 65536 * 16, 1, errp);
}
