/* Display capture owner. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-components.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

static void capture_transfer(DB2010Display *s, QDict *event)
{
    s->pdi_capture_count++;
    qlist_append(s->pdi_transfers, event);
    if (qlist_size(s->pdi_transfers) > 4096) {
        qobject_unref(qlist_pop(s->pdi_transfers));
    }
}

const char *db2010_display_access(void *opaque, DB2010Access *a)
{
    DB2010Display *s = opaque;
    DB2010Pdi preview = s->pdi;
    const char *fault = dbpdi_access(&preview, a->address, a->size, a->now,
                                     a->write, &a->value);
    if (fault) { return fault; }
    if (s->dma->config.display_partial && preview.state.transfers != s->pdi.state.transfers) {
        DB2010Panel panel = *s->panel;
        fault = dbpanel_packet(&panel, &preview);
        if (fault) { return fault; }
    }
    if (!a->commit) { return NULL; }
    uint64_t prior = s->pdi.state.transfers;
    fault = dbpdi_access(&s->pdi, a->address, a->size, a->now, a->write, &a->value);
    if (a->write && a->address == 0xf70001dc && s->pdi.state.transfers != prior) {
        if (s->dma->config.display_partial) { dbpanel_packet(s->panel, &s->pdi); }
        if (s->omit_diagnostics) {
            s->pdi_capture_count++;
            return fault;
        }
        QDict *event = qdict_new(); QList *bytes = qlist_new(), *rs = qlist_new();
        for (unsigned i = 0; i < s->pdi.state.packet_size; i++) {
            qlist_append_int(bytes, s->pdi.state.packet[i]); qlist_append_int(rs, s->pdi.state.rs[i]);
        }
        qdict_put_int(event, "virtual_ns", a->now);
        qdict_put_int(event, "pc", a->pc);
        qdict_put_int(event, "trigger", a->value);
        qdict_put(event, "bytes", bytes); qdict_put(event, "rs", rs);
        capture_transfer(s, event);
    }
    return fault;
}

const char *display_dma_fault(DB2010Display *s)
{
    uint32_t *ch = s->dma->state.channel[5];
    const DB2010Register *stream = s->stream;
    if (!s->has_pdi || !s->pdi.state.dma_armed || (!s->dma->config.display_partial && s->pdi.state.dma_bytes != 77440) ||
        !dbpdi_dma_write_selected(&s->pdi) ||
        (s->pdi.state.config != 0x180200 && !(s->dma->config.display_stream && s->pdi.state.config == 0x180208)) ||
        !stream || !dbreg_known(stream) ||
        (stream->value != 0x120 && !(s->dma->config.display_stream && stream->value == 0x320)) ||
        ch[1] != 0xf7000308 || ch[2] || ch[3] != 0x84489004 ||
        ch[4] != 0xea52 || s->dma->state.global[4] != 1 ||
        (s->display_first_burst_done && !(s->dma->config.display_stream && s->display_completed &&
          s->pdi.state.stop_commands == s->display_frame_count && !s->dma->state.raw_tc))) {
        return "display_dma_configuration_unmodeled";
    }
    bool rgb888 = s->dma->config.display_partial && s->panel->known[3] && s->panel->regs[3] == 0xd230;
    if (rgb888) {
        /* Image viewer full frames and video-control strips use the same
         * packed triplets and retained GRAM geometry. Require whole triplets
         * and sixteen-byte DMA bursts before validating the projected count.
         */
        if (stream->value != 0x320 || s->pdi.state.selector != 0xffff1c ||
            s->pdi.state.dma_bytes % 48 ||
            s->pdi.state.dma_bytes > 116160) {
            return "display_dma_pixel_format_unmodeled";
        }
    }
    if (!rgb888 && s->pdi.state.dma_bytes > 77440) {
        return "display_dma_pixel_format_unmodeled";
    }
    /* Address packet must actually select GRAM before any stream write. */
    static const uint8_t address[] = {0,0,0,0,0,0x21,0,0,0,0x22};
    static const uint8_t rs[] = {0,0,0,0,0,0,1,1,0,0};
    uint8_t selected[10]; memcpy(selected, s->pdi.state.packet, 10);
    if (s->dma->config.display_partial) { selected[6] = selected[7] = 0; }
    if (s->pdi.state.packet_size != 10 || memcmp(selected,address,10) || memcmp(s->pdi.state.rs,rs,10)) {
        return "display_dma_gram_selection_unmodeled";
    }
    if (s->dma->config.display_partial) {
        const char *fault = dbpanel_dma_fault(s->panel, rgb888 ?
            s->pdi.state.dma_bytes / 3 * 2 : s->pdi.state.dma_bytes);
        if (fault) { return fault; }
    }
    if (ch[0] < 0x4c000000 || ch[0] > 0x4c800000 - 16 || ch[0] % 4) {
        return "display_dma_source_address_unmodeled";
    }
    DB2010MemoryView view;
    DB2010MemoryView *r = db2010_memory_view(&s->memory, ch[0], 16, &view);
    if (!r) { return "display_dma_source_address_unmodeled"; }
    const uint8_t *data = r->bytes + ch[0] - r->base;
    for (unsigned i = 0; i < 16; i++) {
        if (!r->unchecked && !r->known[ch[0] - r->base + i]) { return "display_dma_source_unknown"; }
        if (data[i] && !s->dma->config.display_stream) { return "display_dma_nonzero_pixels_unmodeled"; }
    }
    return NULL;
}

static void display_pixels(DB2010Display *s, const uint8_t *data, unsigned bytes)
{
    unsigned before = s->display_pixels->len;
    g_byte_array_append(s->display_pixels, data, bytes);
    if (!s->dma->config.display_partial) { return; }
    if (s->panel->regs[3] != 0xd230) {
        dbpanel_pixels(s->panel, data, bytes);
        return;
    }
    /* Keep incomplete RGB triplets in the existing raw stream buffer. Its
     * length/content already survives native checkpoints; no extra phase state
     * or host-dependent conversion is needed across sixteen-byte DMA bursts.
     */
    uint8_t converted[12];
    unsigned n = 0;
    for (unsigned i = before / 3 * 3; i + 3 <= s->display_pixels->len; i += 3) {
        const uint8_t *rgb = s->display_pixels->data + i;
        uint16_t word = (rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | (rgb[2] >> 3);
        converted[n++] = word >> 8;
        converted[n++] = word;
    }
    dbpanel_pixels(s->panel, converted, n);
}

static void display_first_burst(DB2010Display *s)
{
    if (s->dma->config.display_stream && s->display_first_burst_done) {
        g_byte_array_set_size(s->display_pixels, 0);
        qobject_unref(s->display_bursts);s->display_bursts = qlist_new();
        s->display_completed = 0;
    }
    uint32_t source = s->dma->state.channel[5][0];
    DB2010MemoryView view;
    DB2010MemoryView *r = db2010_memory_view(&s->memory, source, 16, &view);
    const uint8_t *data = r->bytes + source - r->base;
    if (!s->omit_diagnostics) {
        QDict *event = qdict_new(); QList *bytes = qlist_new(), *rs = qlist_new();
        for (unsigned i = 0; i < 16; i++) {
            qlist_append_int(bytes, data[i]); qlist_append_int(rs, 1);
        }
        qdict_put_str(event, "kind", "pl080-first-burst");
        qdict_put_int(event, "instruction_index", (*s->time.count));
        qdict_put_int(event, "pc", (*s->time.pc));
        qdict_put_int(event, "channel", 5); qdict_put_int(event, "source", source);
        qdict_put_int(event, "destination", 0xf7000308);
        qdict_put_bool(event, "source_all_known", !r->unchecked);
        qdict_put(event, "bytes", bytes); qdict_put(event, "rs", rs);
        capture_transfer(s, event);
    } else { s->pdi_capture_count++; }
    if (s->dma->config.display_partial) {
        dbpanel_begin_stream(s->panel);
    }
    if (s->dma->config.display_frame) { display_pixels(s, data, 16); }
    else if (s->dma->config.display_partial) { dbpanel_pixels(s->panel, data, 16); }
    dbdma_source_advance(s->dma, 5, 16); s->pdi.state.dma_bytes -= 16;
    s->display_first_burst_done = true;
    if (s->dma->config.display_frame) {
        s->display_source = source; s->display_started = (*s->time.count);
        s->display_due = db2010_virtual_ns() + 64;
    }
}

uint64_t display_clock(DB2010Display *s)
{
    return db2010_virtual_ns();
}

void display_irq_update(DB2010Display *s)
{
    if (!s->dma->config.display_frame) { return; }
    bool level = (s->dma->state.raw_tc & (1u << 5)) && (s->dma->state.channel[5][4] & 0x8000);
    if (s->has_dsp_peer && (s->dma->state.raw_tc & (1u << 2)) && (s->dma->state.channel[2][4] & 0x8000)) { level = true; }
    db2010_signal_set(&s->dma_irq, level); /* Native ILR7A selects a level-sensitive IRQ. */
    s->display_dma_irq_level = level;
}

typedef enum DB2010DisplayPhase { DBDISPLAY_WAITING, DBDISPLAY_ACTIVE, DBDISPLAY_COMPLETED } DB2010DisplayPhase;
typedef enum DB2010DisplayEvent { DBDISPLAY_FIRST_BURST, DBDISPLAY_TICK } DB2010DisplayEvent;
static bool display_burst(DB2010Display *s)
{
    uint32_t source = s->dma->state.channel[5][0]; unsigned n = MIN(16, s->pdi.state.dma_bytes);
    if (s->dma->state.channel[5][4] != 0xea53 || n != 16 || source % 4 ||
        source < 0x4c000000 || source > 0x4c800000 - n) {
        db2010_link_stop(&s->faults, "display_dma_progress_unmodeled", source, n); return false;
    }
    DB2010MemoryView view;
DB2010MemoryView *r = db2010_memory_view(&s->memory, source, n, &view);
    if (!r) { db2010_link_stop(&s->faults, "display_dma_source_address_unmodeled", source, n); return false; }
    const uint8_t *data = r->bytes + source - r->base;
    for (unsigned i = 0; i < n; i++) {
        if (!r->unchecked && !r->known[source - r->base + i]) { db2010_link_stop(&s->faults, "display_dma_source_unknown", source + i, n); return false; }
        if (data[i] && !s->dma->config.display_stream) { db2010_link_stop(&s->faults, "display_dma_nonzero_pixels_unmodeled", source + i, n); return false; }
    }
    if (!s->omit_diagnostics) {
        QDict *burst = qdict_new();
        qdict_put_int(burst, "virtual_ns", s->display_due);
        qdict_put_int(burst, "source", source); qdict_put_int(burst, "bytes", n);
        qlist_append(s->display_bursts, burst);
    }
    display_pixels(s, data, n);
    dbdma_source_advance(s->dma, 5, n); s->pdi.state.dma_bytes -= n; s->display_due += 64;

    return true;
}
static void display_complete(DB2010Display *s)
{
    dbdma_complete(s->dma, 5); /* Last peripheral request, LLI0. */
    dbpdi_dma_complete(&s->pdi, db2010_virtual_ns());
    s->display_completed = (*s->time.count);
    if (s->dma->config.display_stream) {
        s->display_frame_count++;
        QDict *frame = qdict_new();GString *hex = g_string_sized_new(s->display_pixels->len * 2);
        for (unsigned i = 0; i < s->display_pixels->len; i++) { g_string_append_printf(hex, "%02x", s->display_pixels->data[i]); }
        if (s->dma->config.display_partial) {
            if (!s->omit_diagnostics) { qdict_put_str(frame, "update_hex", hex->str); }
            g_string_truncate(hex, 0);
            if (s->panel->known_pixels == DBPANEL_PIXELS) {
                for (unsigned i = 0; i < sizeof(s->panel->pixels); i++) {
                    g_string_append_printf(hex, "%02x", s->panel->pixels[i]);
                }
            }
            qdict_put_int(frame, "cursor_start", s->panel->started_cursor);
            qdict_put_int(frame, "cursor_end", s->panel->cursor);
            qdict_put_int(frame, "panel_known_pixels", s->panel->known_pixels);
        }
        qdict_put_str(frame, "pixels_hex", hex->str);g_string_free(hex, true);
        qdict_put_int(frame, "source", s->display_source);
        qdict_put_int(frame, "started", s->display_started);
        qdict_put_int(frame, "completed", s->display_completed);
        qdict_put_int(frame, "bytes", s->display_pixels->len);
        qdict_put_str(frame, "input_pixel_format", s->panel->regs[3] == 0xd230 ? "rgb666-in-rgb888" : "rgb565-be");
        DB2010MemoryView view;
        DB2010MemoryView *source = db2010_memory_view(&s->memory, s->display_source, 1, &view);
        bool unchecked = source && source->unchecked;
        qdict_put_str(frame, "transport", unchecked ?
            "native DMA backing bytes; RAM provenance unchecked" :
            "native known DMA bytes; color interpretation in host");
        if (unchecked) { qdict_put_bool(frame, "source_all_known", false); }
        qlist_append(s->display_frames, frame);
        if (qlist_size(s->display_frames) > (s->omit_diagnostics ? 1 : 256)) {
            qobject_unref(qlist_pop(s->display_frames));
        }
    }
    db2010_signal_set(&s->pdi_irq, true);

}
static void display_active(DB2010Display *s)
{
    uint64_t now = display_clock(s);
    while (s->display_due <= now && s->pdi.state.dma_bytes) {
        if (!display_burst(s)) { return; }
    }
    if (!s->pdi.state.dma_bytes) { display_complete(s); }
    display_irq_update(s);
}
static void display_dispatch(DB2010Display *s, DB2010DisplayEvent event)
{
    if (event == DBDISPLAY_FIRST_BURST) { display_first_burst(s); return; }
    DB2010DisplayPhase phase = s->display_completed ? DBDISPLAY_COMPLETED :
        s->display_first_burst_done && s->pdi.state.dma_armed ? DBDISPLAY_ACTIVE : DBDISPLAY_WAITING;
    if (!s->dma->config.display_frame) { return; }
    switch (phase) {
    case DBDISPLAY_ACTIVE: display_active(s); return;
    case DBDISPLAY_WAITING: case DBDISPLAY_COMPLETED: return;
    default: g_assert_not_reached();
    }
}
void display_dma_first_burst(DB2010Display *s) { display_dispatch(s, DBDISPLAY_FIRST_BURST); }
void display_dma_advance(DB2010Display *s) { display_dispatch(s, DBDISPLAY_TICK); }

void db2010_display_capture_reset(DB2010Display *s)
{
    qobject_unref(s->pdi_transfers); s->pdi_transfers = qlist_new();
    if (s->display_pixels) { g_byte_array_unref(s->display_pixels); }
    s->display_pixels = g_byte_array_new();
    qobject_unref(s->display_bursts); s->display_bursts = qlist_new();
    qobject_unref(s->display_frames); s->display_frames = qlist_new();
    s->display_frame_count = s->pdi_capture_count = 0;
    s->display_source = 0; s->display_due = s->display_started = s->display_completed = 0;
    s->display_dma_irq_level = false; s->display_first_burst_done = false;
}

void db2010_display_export(DB2010Display *model, QDict *snapshot)
{

        QDict *pdi = qdict_new();
        qdict_put_str(pdi, "profile", model->pdi.config.retained_latches ? "r61500-write-packets-retained-latches" : "r61500-write-packets");
        qdict_put_str(pdi, "completion", "atomic-write-with-2048ns-busy");
        qdict_put_int(pdi, "busy_deadline_ns", model->pdi.state.busy_until);
        qdict_put_int(pdi, "busy_reads", model->pdi.state.busy_reads);
        qdict_put_bool(pdi, "dma_armed", model->pdi.state.dma_armed);
        qdict_put_int(pdi, "dma_bytes", model->pdi.state.dma_bytes);
        if (model->dma->config.display_frame) {
            qdict_put_bool(pdi, "dma_complete", model->pdi.state.dma_complete);
            qdict_put_int(pdi, "stop_commands", model->pdi.state.stop_commands);
            QDict *frame = qdict_new();
            GString *hex = g_string_sized_new(model->display_pixels->len * 2);
            for (unsigned i = 0; i < model->display_pixels->len; i++) { g_string_append_printf(hex, "%02x", model->display_pixels->data[i]); }
            qdict_put_str(frame, "pixels_hex", hex->str); g_string_free(hex, true);
            qdict_put_str(frame, "model", "synthetic-one-16-byte-burst-per-64ns");
            qdict_put_int(frame, "source", model->display_source);
            qdict_put_int(frame, "started", model->display_started);
            qdict_put_int(frame, "completed", model->display_completed);
            qdict_put_int(frame, "bytes", model->display_pixels->len);
            qdict_put(frame, "remaining_bursts", qobject_ref(model->display_bursts));
            qdict_put(pdi, "frame", frame);
            if (model->dma->config.display_stream) {
                qdict_put(pdi, "frames", qobject_ref(model->display_frames));
                qdict_put_int(pdi, "completed_frames", model->display_frame_count);
                if (model->display_frame_count > qlist_size(model->display_frames)) {
                    qdict_put_int(pdi, "frames_dropped", model->display_frame_count - qlist_size(model->display_frames));
                }
            }
        }
        if (model->pdi.config.retained_latches) {
            QList *data = qlist_new();
            for (unsigned i = 0; i < 6; i++) {
                if (model->pdi.state.data_known & (1u << i)) { qlist_append_int(data, model->pdi.state.data[i]); }
                else { qlist_append_null(data); }
            }
            qdict_put(pdi, "data_latches_0_to_5", data);
            qdict_put_int(pdi, "data_known_mask", model->pdi.state.data_known);
            qdict_put_int(pdi, "data_dirty_mask", model->pdi.state.data_dirty);
            qdict_put_bool(pdi, "mask_dirty", model->pdi.state.mask_dirty);
            if (model->pdi.state.mask_known) { qdict_put_int(pdi, "mask_latch", model->pdi.state.mask); }
            else { qdict_put_null(pdi, "mask_latch"); }
        }
        qdict_put_int(pdi, "control", model->pdi.state.control);
        qdict_put_int(pdi, "config", model->pdi.state.config);
        if (model->dma->config.display_partial) {
            QDict *panel = qdict_new(); QList *regs = qlist_new();
            for (unsigned i = 0; i < G_N_ELEMENTS(model->panel->regs); i++) {
                if (model->panel->known[i]) { qlist_append_int(regs, model->panel->regs[i]); }
                else { qlist_append_null(regs); }
            }
            qdict_put(panel, "registers", regs);
            qdict_put_int(panel, "cursor", model->panel->cursor);
            qdict_put_bool(panel, "cursor_known", model->panel->cursor_known);
            qdict_put_int(panel, "known_pixels", model->panel->known_pixels);
            qdict_put_str(panel, "profile", "r61500-7230-af00-db00-retained-gram");
            qdict_put(pdi, "panel", panel);
        }
        qdict_put(pdi, "transfers", qobject_ref(model->pdi_transfers));
        if (model->pdi_capture_count > qlist_size(model->pdi_transfers)) {
            qdict_put_int(pdi, "transfers_total", model->pdi_capture_count);
            qdict_put_int(pdi, "transfers_dropped", model->pdi_capture_count - qlist_size(model->pdi_transfers));
        }
        qdict_put(snapshot, "pdi", pdi);
    }

void db2010_display_reset(DB2010Display *s)
{
    db2010_display_capture_reset(s); dbpanel_reset(s->panel);
}
