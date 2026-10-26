/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-dsp-peer.h"
#include "db2010-components.h"

typedef enum DB2010DspEventKind { DBDSP_REQUEST, DBDSP_TICK, DBDSP_ACK_STATUS } DB2010DspEventKind;
typedef struct DB2010DspEvent {
    DB2010DspEventKind kind;
    DB2010FaLink *link;
    DB2010Access *access;
    bool mailbox_only;
    unsigned length, logical;
    uint64_t now;
} DB2010DspEvent;
static void dbdsp_dispatch(DB2010DspPeer *p, const DB2010DspEvent *e);
static const char *dbdsp_modexp(DB2010DspPeer *p);
static void dbdsp_rx_word(DB2010DspPeer *p, const DB2010DspEvent *e);
static void dbdsp_audio_tick(DB2010DspPeer *p, DB2010FaLink *s, uint64_t now);
static void dbdsp_midi_tick(DB2010DspPeer *p, DB2010FaLink *s, uint64_t now);

static const char *dsp_mask_field(void *opaque, const DB2010DecodedAccess *d,
                                 const DB2010Access *a, DB2010FieldRef *field)
{
    DB2010DspPeer *p = opaque;
    *field = (DB2010FieldRef){.value = &p->state.mask, .mask = UINT32_MAX};
    return NULL;
}
static const char *dsp_status(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010DspPeer *p = opaque;
    DB2010DspEvent e = {.kind = DBDSP_ACK_STATUS, .access = a};
    dbdsp_dispatch(p, &e);
    return NULL;
}
const DB2010RegMap *dbdsp_map(void)
{
    static const uint32_t masks[] = {0, 3, 0x1b, 0xdf};
    static const DB2010RegRule children[] = {
        DB_REG(0, .alignment = 4, .backing_width = 4,
            .read = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH32,
                     .field = dsp_mask_field, .fault = "dsp_peer_access_unmodeled"},
            .write = {.kind = DB_ACTION_FIELD, .widths = DB_WIDTH32,
                .field = dsp_mask_field, .fault = "dsp_peer_access_unmodeled",
                .constraint = {.kind = DB_VALUE_SET, .values = masks,
                    .count = ARRAY_SIZE(masks), .fault = "dsp_peer_event_mask_unmodeled"}}),
        DB_REG(4, .alignment = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32,
                     .callback = dsp_status},
            .write = {.kind = DB_ACTION_IGNORE, .widths = DB_WIDTH32,
                .constraint = {.kind = DB_VALUE_RANGE, .maximum = 0,
                               .fault = "dsp_peer_status_write_unmodeled"}}),
    };
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 8, .children = children, .child_count = ARRAY_SIZE(children)),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xfa04000c, rules,
            ARRAY_SIZE(rules), "dsp_peer_access_unmodeled");
        if (fault) { g_error("DB2010 DSP layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
bool dbdsp_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = dbdsp_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}
const char *dbdsp_access(DB2010DspPeer *p, uint32_t address,
                         unsigned size, bool write, uint32_t *value, bool apply)
{
    DB2010Access a = {.address = address, .size = size, .write = write,
                      .value = *value, .commit = apply};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbdsp_map(), &a, &d);
    if (!fault) { fault = db2010_reg_execute(p, &d, &a); }
    *value = a.value;
    return fault;
}

QDict *dbdsp_snapshot(DB2010DspPeer *p)
{
    QDict *r = qdict_new();
    qdict_put_bool(r, "midi_setup_reply_modeled", p->config.midi_setup);
    qdict_put_bool(r, "midi_playback_modeled", p->config.midi_setup);
    qdict_put_bool(r, "midi_initialized", p->state.midi_initialized);
    qdict_put_bool(r, "midi_prepared", p->state.midi_prepared);
    qdict_put_bool(r, "midi_ring_announced", p->state.midi_ring_announced);
    qdict_put_bool(r, "midi_ring_registered", p->state.midi_ring_registered);
    qdict_put_int(r, "midi_ring_threshold", p->state.midi_ring_threshold);
    qdict_put_int(r, "midi_requests", p->state.midi_requests);
    qdict_put_int(r, "midi_replies", p->state.midi_replies);
    if (p->config.midi_setup) {
        DB2010MidiStream *m = &p->state.midi;
        qdict_put_int(r, "midi_buffered_words", m->used);
        qdict_put_int(r, "midi_events", m->events);
        qdict_put_int(r, "midi_frames", m->frames);
        qdict_put_int(r, "midi_nonzero_samples", m->nonzero_samples);
        qdict_put_int(r, "midi_words_received", m->words_received);
        qdict_put_int(r, "midi_words_credited", m->words_credited);
        qdict_put_int(r, "midi_credit", m->credit);
        qdict_put_int(r, "midi_last_time", m->last_time);
        qdict_put_bool(r, "midi_active", m->active);
        qdict_put_bool(r, "midi_paused", m->paused);
        qdict_put_bool(r, "midi_eof", m->eof);
        qdict_put_bool(r, "midi_completed", m->completed);
        qdict_put_bool(r, "midi_completion_sent", m->completion_sent);
        qdict_put_str(r, "midi_processing", "host-40-triangle-48000-mono-duplicated");
        char hash[17]; snprintf(hash, sizeof(hash), "%016" PRIx64, m->pcm_hash);
        qdict_put_str(r, "midi_pcm_fnv1a64", hash);
    }
    qdict_put_str(r, "profile", p->config.midi_profile ? "wire20-modexp1024-mma-midi-peer" : p->config.midi_setup ? "wire20-modexp1024-mma-midi-setup-peer" : p->config.mma_stream ? "wire20-modexp1024-mma-stream-peer" : p->config.mma_drain ? "wire20-modexp1024-mma-drain-peer" : p->config.mma_playback ? "wire20-modexp1024-mma-playback-peer" : p->config.mma_buffer ? "wire20-modexp1024-mma-buffer-peer" : p->config.mma_setup ? "wire20-modexp1024-mma-setup-peer" : p->config.mma_ring ? "wire20-modexp1024-mma-ring-peer" : p->config.mma_mode ? "wire20-modexp1024-mma-mode-peer" : p->config.modexp ? "wire20-modexp1024-peer" : p->config.variable_dma ? "wire10-message-wire20-variable-dma-sink" : p->config.wire10 == 2 ? "wire10-message-wire20-dma-sink" :
                  p->config.wire10 ? "wire10-0300-wire20-dma-sink" : "wire20-op2-logical2-sink");
    if (p->config.variable_dma) {
        qdict_put_int(r,"dma_messages",p->state.dma_messages);
        qdict_put_int(r,"total_received_bytes",p->state.total_bytes);
        qdict_put_int(r,"expected_bytes",p->state.length);
        qdict_put_int(r,"logical_channel",p->state.logical);
    }
    if (p->config.wire10) {
        qdict_put_int(r, "mailbox_only_messages", p->state.message_requests);
        qdict_put_bool(r, "mailbox_only_current", p->state.mailbox_only);
    }
    qdict_put_bool(r, "synthetic", true);
    qdict_put_bool(r, "DSP_execution_modeled", false);
    qdict_put_bool(r, "application_reply_modeled", p->config.modexp);
    if (p->config.modexp) {
        qdict_put_bool(r, "audio_playback_modeled", false);
        qdict_put_str(r, "audio_equalizer", "control-acceptance-only-no-pcm-filtering");
        if (p->config.mma_playback) {
            DB2010AudioRuntime *a = &p->state.audio;
            qdict_put_bool(r, "audio_playback_modeled", true);
            qdict_put_str(r, "audio_processing", a->source_codec == 1 ?
                          "host-fixed-faad2-aac-lc-mode0-virtual-clock" :
                          "host-scalar-minimp3-layer3-mode0-virtual-clock");
            qdict_put_str(r, "audio_source_codec", a->source_codec == 1 ? "aac-lc" : "layer3");
            qdict_put_bool(r, "audio_aac_primed", a->aac.primed);
            qdict_put_bool(r, "audio_underrun", a->underrun);
            qdict_put_bool(r, "audio_underrun_notified", a->underrun_notified);
            qdict_put_int(r, "audio_underruns_offered", a->underruns_offered);
            qdict_put_int(r, "audio_underruns_acked", a->underruns_acked);
            if (!p->omit_diagnostics && p->encoded_frame_count) {
                qdict_put(r, "audio_encoded_frames", qobject_ref(p->encoded_frames));
                qdict_put_int(r, "audio_encoded_frames_total", p->encoded_frame_count);
                qdict_put_int(r, "audio_encoded_frames_dropped", p->encoded_frame_count - qlist_size(p->encoded_frames));
            }
            if (p->config.mma_stream) {
                qdict_put_int(r, "audio_sample_packets", a->samples.packets);
                qdict_put_int(r, "audio_sample_partial_frames", a->samples.frames);
                qdict_put_int(r, "audio_sample_emitted", a->samples.emitted);
                qdict_put_bool(r, "audio_sample_ended", a->samples.ended);
            }
            qdict_put_int(r, "audio_source_rate", a->source_rate ? a->source_rate : 48000);
            qdict_put_int(r, "audio_source_channels", a->source_channels ? a->source_channels : 2);
            qdict_put_bool(r, "audio_coefficient_ready", a->coefficient_ready);
            qdict_put_bool(r, "audio_interrupted", a->interrupted);
            qdict_put_bool(r, "audio_paused", a->paused);
            qdict_put_int(r, "audio_resampler_phase", a->resampler.phase);
            bool converted = a->source_rate && a->source_rate != 48000;
            qdict_put_int(r, "audio_source_frames", converted ? a->resampler.input_frames : a->frames_decoded * 1152);
            qdict_put_int(r, "audio_resampled_frames", converted ? a->resampler.output_frames : a->frames_decoded * 1152);
            qdict_put_str(r, "audio_resampling", a->source_rate == 22050 ? "integer-linear-320-over-147" : a->source_rate == 44100 ? "integer-linear-160-over-147" : converted ? "integer-linear-rational" : "identity-48000");
            qdict_put_bool(r, "audio_active", a->active);
            qdict_put_bool(r, "audio_eof", a->eof);
            qdict_put_bool(r, "audio_completed", a->completed);
            qdict_put_int(r, "audio_start_ns", a->start_ns);
            qdict_put_int(r, "audio_due", a->due);
            qdict_put_int(r, "audio_input_bytes", a->input_bytes);
            qdict_put_int(r, "audio_pending_pcm_frames", a->pcm_frames);
            qdict_put_int(r, "audio_frames_decoded", a->frames_decoded);
            qdict_put_int(r, "audio_frames_played", a->frames_played);
            qdict_put_int(r, "audio_nonzero_samples", a->nonzero_samples);
            g_autofree char *hash = g_strdup_printf("%016" PRIx64, a->pcm_hash);
            qdict_put_str(r, "audio_pcm_fnv1a64", hash);
            qdict_put_int(r, "audio_pending_credit", a->credit);
            qdict_put_int(r, "audio_rx_kind", a->rx_kind);
            qdict_put_int(r, "audio_credits_offered", a->credits_offered);
            qdict_put_int(r, "audio_credits_acked", a->credits_acked);
            qdict_put_int(r, "audio_words_credited", a->words_credited);
            qdict_put_int(r, "audio_completions_acked", a->completions_acked);
        }
        qdict_put_str(r, "audio_control", "observed-opcode-acks-zero-coefficients-slot-occupancy");
        qdict_put_int(r, "audio_requests", p->state.audio_requests);
        qdict_put_int(r, "audio_replies", p->state.audio_replies);
        qdict_put_int(r, "audio_opcode", p->state.audio_opcode);
        qdict_put_bool(r, "rx_inline", p->state.rx_inline);
        QList *slots = qlist_new();
        for (unsigned i = 0; i < ARRAY_SIZE(p->state.audio_iir_active); i++) {
            qlist_append_int(slots, p->state.audio_iir_active[i]);
        }
        qdict_put(r, "audio_iir_active", slots);
        if (p->config.mma_mode) {
            qdict_put_str(r, "mma_control", p->config.mma_playback ? "host-recovered-setup-mode0-processing" : p->config.mma_setup ? "observed-0e00-startup-setup-acceptance-no-playback" : "synthetic-0e04-mode-acceptance-no-playback");
            if (p->config.mma_setup) {
                qdict_put_str(r, "mma_setup_clock", "qemu-virtual-microseconds-modulo-u32");
            }
            qdict_put_int(r, "mma_mode", p->state.mma_mode);
            qdict_put_int(r, "mma_requests", p->state.mma_requests);
            qdict_put_int(r, "mma_replies", p->state.mma_replies);
            qdict_put_bool(r, "rx_mma", p->state.rx_mma);
            if (p->config.mma_ring) {
                qdict_put_str(r, "ring4_contract", p->config.mma_playback ? "bounded-800-halfword-buffer-consumed-word-credits" : p->config.mma_buffer ? "bounded-800-halfword-buffer-no-consumption-or-credit" : "synthetic-801-halfword-announcement-registration-only");
                qdict_put_int(r, "ring4_capacity", 801);
                qdict_put_bool(r, "rx_ring4", p->state.rx_ring4);
                qdict_put_bool(r, "ring4_announced", p->state.ring4_announced);
                qdict_put_bool(r, "ring4_registered", p->state.ring4_registered);
                qdict_put_int(r, "ring4_threshold", p->state.ring4_threshold);
                qdict_put_int(r, "ring4_announcements", p->state.ring4_announcements);
                qdict_put_int(r, "ring4_registrations", p->state.ring4_registrations);
                if (p->config.mma_buffer) {
                    qdict_put_bool(r, "mma_configured", p->state.mma_configured);
                    qdict_put_int(r, "ring4_buffered_words", p->state.ring4_buffered_words);
                    qdict_put_int(r, "ring4_free_words", 800 - p->state.ring4_buffered_words);
                    qdict_put_int(r, "ring4_packets", p->state.ring4_packets);
                    GString *buffer = g_string_new(NULL);
                    for (unsigned i = 0; i < p->state.ring4_buffered_words; i++) {
                        g_string_append_printf(buffer, "%02x%02x", p->state.ring4_buffer[i] & 255,
                                               p->state.ring4_buffer[i] >> 8);
                    }
                    qdict_put_str(r, "ring4_buffer_hex", buffer->str);
                    g_string_free(buffer, true);
                }
            }
        }
        GString *mailbox = g_string_new(NULL);
        for (unsigned i = 0; i < ARRAY_SIZE(p->state.rx_mailbox); i++) {
            g_string_append_printf(mailbox, "%02x%02x", p->state.rx_mailbox[i] & 255,
                                   p->state.rx_mailbox[i] >> 8);
        }
        qdict_put_str(r, "rx_mailbox_hex", mailbox->str); g_string_free(mailbox, true);
    }
    if (p->config.modexp) {
        qdict_put_int(r, "replies", p->state.replies);
        qdict_put_int(r, "rx_bytes", p->state.rx_bytes);
        qdict_put_int(r, "rx_selector", p->state.selector);
        qdict_put_bool(r, "rx_mailbox_ready", p->state.rx_ready);
        GString *result = g_string_new(NULL);
        for (unsigned i = 0; i < sizeof(p->state.response); i++) {
            g_string_append_printf(result, "%02x", p->state.response[i]);
        }
        qdict_put_str(r, "response_hex", result->str); g_string_free(result, true);
    }
    qdict_put_str(r, "status_ack_hypothesis", "clear-on-read");
    qdict_put_int(r, "latency_ns", p->config.latency);
    qdict_put_int(r, "phase", p->state.phase);
    qdict_put_int(r, "deadline_ns", p->state.due);
    qdict_put_int(r, "event_mask", p->state.mask);
    qdict_put_int(r, "event_status", p->state.status);
    qdict_put_int(r, "initial_source", p->state.source);
    qdict_put_int(r, "bytes_received", p->state.bytes);
    qdict_put_int(r, "requests", p->state.requests);
    qdict_put_int(r, "mailbox_events", p->state.mailbox_events);
    qdict_put_int(r, "transfer_events", p->state.transfer_events);
    qdict_put_int(r, "status_reads", p->state.status_reads);
    GString *hex = g_string_new(NULL);
    for (unsigned i = 0; i < p->state.bytes; i++) { g_string_append_printf(hex, "%02x", p->state.payload[i]); }
    qdict_put_str(r, "payload_hex", hex->str); g_string_free(hex, true);
    return r;
}

void dbdsp_reset(DB2010DspPeer *d)
{
    db2010_audio_output_reset(d->output);
    qobject_unref(d->encoded_frames); d->encoded_frames = qlist_new();
    d->encoded_frame_count = 0;
    d->state = (DB2010DspPeerRuntime){0};
}

void dbdsp_request(DB2010DspPeer *p, bool mailbox_only, unsigned length,
                   unsigned logical, uint64_t now)
{
    DB2010DspEvent e = {.kind = DBDSP_REQUEST, .mailbox_only = mailbox_only,
                        .length = length, .logical = logical, .now = now};
    dbdsp_dispatch(p, &e);
}

const char *db2010_peer_access(void *opaque, DB2010Access *a)
{
    DB2010FaLink *s = opaque;
    const char *fault = dbdsp_access(s->dsp_peer, a->address, a->size,
                                     a->write, &a->value, a->commit);
    if (!fault && a->commit && !a->write) {
        db2010_signal_set(&s->dsp_irq, (s->dsp_peer->state.mask & s->dsp_peer->state.status) != 0);
    }
    return fault;
}

const char *dsp_peer_submit_fault(DB2010FaLink *s)
{
    DB2010FaMailbox *m = &s->fa_mailbox;
    DB2010DspPeer *p = s->dsp_peer;
    /* The separate wire10 path transmits framed application bytes in the
     * mailbox itself. dbfm_access already checked count and all known words.
     * Completion acknowledges transport only, never an application result. */
    if (p->config.wire10 && p->state.phase == DBDSP_DONE &&
        ((p->config.wire10 == 2 && (m->words[0] & 0x7ff) == 10) ||
         (p->config.wire10 == 1 && (m->known & 15) == 15 &&
          m->words[0] == 0x180a && m->words[1] == 0x0300 &&
          m->words[2] == 0x0065 && m->words[3] == 0x0014))) {
        if (s->dma->state.channel[2][4] & 1) { return "dsp_peer_message_dma_active"; }
        unsigned count = m->words[0] >> 11; /* Staged header, before commit. */
        if (p->config.midi_setup && m->words[count] == 9) {
            DB2010MidiStream *midi = &p->state.midi;
            static const uint16_t setup[] = {0x600, 2, 0, 40, 16, 0x7fff, 0, 1};
            if (count == 2 && m->words[1] == 0x604) {
                return p->state.midi_initialized && !p->state.midi_prepared &&
                    !p->state.audio.active && !p->state.audio.paused ?
                    NULL : "dsp_peer_midi_prepare_lifecycle_unmodeled";
            }
            if (count == 3 && m->words[1] == 0x608 && m->words[2] <= 2) {
                return p->state.midi_prepared && p->state.midi_ring_registered &&
                    (m->words[2] != 0 || ((!midi->active && !midi->completed) &&
                     (midi->paused || midi->used))) ? NULL : "dsp_peer_midi_mode_lifecycle_unmodeled";
            }
            if (count == 4 && m->words[1] == 0x60a) {
                return midi->active ? NULL : "dsp_peer_midi_completion_lifecycle_unmodeled";
            }
            if (count == 4 && m->words[1] == 0x61a && p->state.midi_prepared) { return NULL; }
            if (count == 2 && m->words[1] == 0x606 && p->state.midi_prepared &&
                !p->state.midi_ring_registered && !midi->active && !midi->used && !midi->credit) { return NULL; }
            if (count == 2 && m->words[1] == 0x602 && p->state.midi_initialized &&
                !p->state.midi_prepared) { return NULL; }
            if (count != ARRAY_SIZE(setup) + 1 ||
                memcmp(m->words + 1, setup, sizeof(setup))) {
                return "dsp_peer_midi_command_unmodeled";
            }
            if (p->state.midi_initialized) { return "dsp_peer_midi_setup_lifecycle_unmodeled"; }
        }
        if (p->config.mma_mode && m->words[count] == 18) {
            bool start = p->config.mma_playback && count == 3 && m->words[1] == 0x0e04 && m->words[2] == 0;
            if (start) {
                if (p->state.midi.active || p->state.midi.paused) {
                    return "dsp_peer_concurrent_audio_unmodeled";
                }
                DB2010AudioRuntime *a = &p->state.audio;
                if (a->underrun && a->underrun_notified &&
                    p->state.mma_mode == 0 && p->state.mma_configured &&
                    p->state.ring4_registered && p->state.ring4_buffered_words &&
                    !a->pcm_frames && !a->input_bytes) {
                    return NULL;
                }
                if (p->state.audio.paused && p->state.mma_mode == 2 &&
                    p->state.mma_configured && p->state.ring4_registered) {
                    return NULL;
                }
                if (!p->state.mma_configured || p->state.mma_mode != 3 ||
                    ((p->state.audio.source_rate == 22050 || p->state.audio.source_rate == 16000) && !p->state.audio.coefficient_ready) ||
                    !p->state.ring4_buffered_words || p->state.audio.active || p->state.audio.completed) {
                    return "dsp_peer_audio_start_lifecycle_unmodeled";
                }
                return NULL;
            }
            if (p->config.mma_drain && count == 3 && m->words[1] == 0x0e04 &&
                p->state.ring4_registered &&
                (((p->state.audio.active || p->state.audio.underrun) && p->state.mma_mode == 0 &&
                  (m->words[2] == 1 || m->words[2] == 2)) ||
                 (p->state.audio.paused && p->state.mma_mode == 2 && m->words[2] == 1))) {
                return NULL;
            }
            if (p->state.audio.paused) {
                return "dsp_peer_audio_paused_control_unmodeled";
            }
            if (p->config.mma_drain && p->state.audio.active) {
                return "dsp_peer_audio_active_control_unmodeled";
            }
            if (p->config.mma_buffer && p->state.ring4_buffered_words) {
                return "dsp_peer_mma_buffer_lifecycle_unmodeled";
            }
            if (p->config.mma_setup && m->words[1] == 0x0e00) {
                /* Original startup request: format4/rate48000/stereo with the
                 * inherited synthetic zero-stage coefficient query. This is
                 * configuration acceptance, not codec or data consumption. */
                uint16_t setup[] = {
                    0x0e00, 8, 1, 2, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0xffff, 0xffff, 0xffff, 0, 0, 1, 0, 0xcc02, 18
                };
                /* Original0243 ringtone serializer declares44100/stereo. */
                if (m->words[2] == 7 && m->words[6] == 2) {
                    setup[1] = 7; setup[5] = 2;
                    /* Native4488D6E4 maps mono to0 and
                     * sign-extends the observed option byte FF toFFFF. */
                    if (m->words[3] == 0) {
                        setup[2] = 0; setup[21] = 0xffff;
                    }
                }
                if (m->words[2] == 4) {
                    const uint16_t opera[] = {
                        0x0e00, 4, 1, 2, 1, 2, 1, 2, 0, 0, 1, 0, 0, 32, 0, 0,
                        0, 0xffff, 0xffff, 1, 0, 1, 0, 0xcc02, 18
                    };
                    memcpy(setup, opera, sizeof(setup));
                }
                if (m->words[2] == 3) {
                    /* Native Demo Tour setup, format1/16000Hz/stereo.
                     * Only accept the captured serializer contract here;
                     * coefficient upload and encoded data remain guarded. */
                    const uint16_t video[] = {
                        0x0e00, 3, 1, 1, 0, 3, 1, 3, 0, 0, 1, 0, 0, 32, 0, 0,
                        0, 0xffff, 0xffff, 1, 0, 1, 0, 0xcc01, 18
                    };
                    memcpy(setup, video, sizeof(setup));
                }
                if (count != ARRAY_SIZE(setup) ||
                    memcmp(&m->words[1], setup, sizeof(setup))) {
                    return "dsp_peer_mma_setup_unmodeled";
                }
                if (p->state.mma_mode != 3 || !p->state.ring4_registered) {
                    return "dsp_peer_mma_setup_lifecycle_unmodeled";
                }
            } else {
                if (count != 3 || m->words[1] != 0x0e04) {
                    return "dsp_peer_mma_packet_unmodeled";
                }
                if (m->words[2] != 1 && m->words[2] != 3 && m->words[2] != 4) {
                    return "dsp_peer_mma_mode_unmodeled";
                }
                if (p->config.mma_ring && m->words[2] == 3 && p->state.ring4_announced) {
                    DB2010AudioRuntime *a = &p->state.audio;
                    /* The ringtone-menu request reopens after native mode4,
                     * drained completion and type1 writer unregistration.
                     * Both completion and interruption must finish native cleanup first. */
                    if (!p->config.mma_drain || p->state.mma_mode != 4 ||
                        p->state.ring4_registered || (!a->interrupted &&
                        (!a->completed || a->completions_acked != 1)) || a->active || a->pcm_frames ||
                        a->input_bytes || a->credit || a->rx_kind ||
                        p->state.ring4_buffered_words || a->samples.frames ||
                        a->credits_offered != a->credits_acked) {
                        return "dsp_peer_ring_lifecycle_unmodeled";
                    }
                }
            }
        }
        if (p->config.mma_ring && m->words[count] == 20) {
            if (p->config.midi_setup && (m->words[1] & 255) == 1) {
                DB2010MidiStream *midi = &p->state.midi;
                if (count == 3 && m->words[1] == 0x401 &&
                    m->words[2] == 750 && p->state.midi_prepared &&
                    p->state.midi_ring_announced && !p->state.midi_ring_registered) {
                    return NULL;
                }
                if (p->state.midi_ring_registered) {
                    if (count == 2 && (m->words[1] == 0x701 ||
                        (m->words[1] == 0x101 && !midi->active && !midi->credit && !midi->used))) { return NULL; }
                    if (m->words[1] == 0x801 && count >= 4 && m->words[2] == count - 3 &&
                        m->words[2] < 28) {
                        return db2010_midi_stream_check_append(midi, m->words + 3, m->words[2]);
                    }
                }
                return "dsp_peer_midi_ring_packet_unmodeled";
            }
            if (p->config.mma_playback && count >= 9 && m->words[1] == 0x0804) {
                /* Original447367A8 uses inline transport below28 ring words.
                 * A short final source read carries the same counted header
                 * and bytes as DMA; zero-source type2 remains repeated EOF. */
                unsigned words = m->words[2];
                bool eof = m->words[3] == 2;
                if (words != count - 3 || words >= 28 ||
                    (m->words[3] != 1 && !eof) ||
                    m->words[4] > 1 || m->words[5] != 1 ||
                    m->words[8] != words - 6 || (eof && words != 6) ||
                    (!eof && words == 6)) { return "dsp_peer_ring_data_header_unmodeled"; }
                if (p->state.audio.eof && !eof) { return "dsp_peer_audio_data_after_eof_unmodeled"; }
                /* Short AAC prefill is inline before mode0, just like the
                 * already-supported bulk ring prefill. It supplies source
                 * bytes only; consumption still waits for native mode0. */
                if (!p->state.mma_configured ||
                    ((p->state.audio.source_rate == 22050 || p->state.audio.source_rate == 16000) && !p->state.audio.coefficient_ready) ||
                    (p->state.mma_mode != 3 && p->state.mma_mode != 0 &&
                     !(p->state.mma_mode == 2 && p->state.audio.paused)) || !p->state.ring4_registered ||
                    (p->state.audio.eof && !p->config.mma_drain) ||
                    words > 800 - p->state.ring4_buffered_words) {
                    return "dsp_peer_audio_eof_lifecycle_unmodeled";
                }
                return NULL;
            }
            if (p->config.mma_drain && count == 2 &&
                (m->words[1] == 0x0704 || m->words[1] == 0x0104)) {
                DB2010AudioRuntime *a = &p->state.audio;
                bool interrupted = a->interrupted && p->state.mma_mode == 1;
                if (!p->state.ring4_registered ||
                    (!interrupted && (!a->completed || !a->completions_acked)) ||
                    a->active || a->pcm_frames || a->input_bytes ||
                    ((!interrupted || m->words[1] == 0x0104) &&
                     (a->credit || p->state.ring4_buffered_words)) ||
                    a->credit + p->state.ring4_buffered_words > 800 ||
                    (m->words[1] == 0x0104 && p->state.mma_mode != 1)) {
                    return "dsp_peer_audio_drain_lifecycle_unmodeled";
                }
                return NULL;
            }
            /* Inherited native bootstrap ring0 declaration remains transport-only. */
            if (count == 3 && m->words[1] == 0x0300 && m->words[2] == 0x65) {
                return NULL;
            }
            /* Original448D5514 closes ring0 with type6 and no payload or
             * application wait. The opposite ARM receiver's type6 only clears
             * its state byte. This peer owns no ring0 data: acknowledge the
             * transport only, without a reply, callback, or ring4 mutation. */
            if (count == 2 && m->words[1] == 0x0600) {
                return NULL;
            }
            if (count != 3 || m->words[1] != 0x0404) {
                return "dsp_peer_ring_packet_unmodeled";
            }
            if (!p->state.ring4_announced || p->state.ring4_registered || m->words[2] != 240) {
                return "dsp_peer_ring_registration_unmodeled";
            }
        }
        return NULL;
    }
    bool framed_dma = p->config.variable_dma && (p->state.phase == DBDSP_IDLE || p->state.phase == DBDSP_DONE) &&
        (m->known & 15) == 15 && m->words[0] == 0x1814 && m->words[1] == 2 &&
        (s->dma->state.channel[2][3] & 0xfffff000) == 0xa4492000 &&
        (s->dma->state.channel[2][3] & 0xfff) && (s->dma->state.channel[2][3] & 0xfff) <= 4094 &&
        m->words[3] == (s->dma->state.channel[2][3] & 0xfff) * 2;
    if (framed_dma && p->config.midi_setup && m->words[2] == 9) {
        return "dsp_peer_midi_bank_unmodeled";
    }
    if (framed_dma && p->config.mma_mode && m->words[2] == 18) {
        if (!p->config.mma_buffer || (p->state.audio.source_rate != 22050 && p->state.audio.source_rate != 16000)) {
            return "dsp_peer_mma_packet_unmodeled";
        }
        if (m->words[3] != (p->state.audio.source_rate == 16000 ? 64 : 48) ||
            !p->state.mma_configured || p->state.mma_mode != 3 ||
            !p->state.ring4_registered ||
            p->state.audio.coefficient_ready || p->state.audio.active ||
            p->state.ring4_buffered_words) {
            return "dsp_peer_mma_coefficients_lifecycle_unmodeled";
        }
    }
    if (framed_dma && p->config.mma_ring && m->words[2] == 20 &&
        !(p->config.midi_setup && p->state.midi_ring_registered)) {
        if (!p->config.mma_buffer) { return "dsp_peer_ring_packet_unmodeled"; }
        if (p->config.mma_drain && p->state.audio.eof) {
            return "dsp_peer_audio_data_after_eof_unmodeled";
        }
        /* Original4488BF58 refills before sending mode0 on resume. A paused
         * ring can accept bounded input without consuming or playing it. */
        if (!p->state.mma_configured || (p->state.mma_mode != 3 &&
            !(p->config.mma_playback && (p->state.mma_mode == 0 ||
              (p->state.mma_mode == 2 && p->state.audio.paused)))) || !p->state.ring4_registered) {
            return "dsp_peer_ring_data_lifecycle_unmodeled";
        }
        if ((p->state.audio.source_rate == 22050 || p->state.audio.source_rate == 16000) && !p->state.audio.coefficient_ready) {
            return "dsp_peer_mma_coefficients_lifecycle_unmodeled";
        }
    }
    if (!framed_dma && (p->state.phase || m->words[0] != 0x1814 || m->words[1] != 2 ||
        m->words[2] != 2 || m->words[3] != 112 || (m->known & 15) != 15)) {
        return "dsp_peer_packet_contract_unmodeled";
    }
    if (s->dma->state.channel[2][4] != 0xc8c7 || s->dma->state.channel[2][1] != 0xfa040020 ||
        (!framed_dma && s->dma->state.channel[2][3] != 0xa4492038)) {
        return "dsp_peer_pending_dma_unmodeled";
    }
    const DB2010Register *mode = s->mode;
    if (p->config.modexp ? p->state.selector != 0x18 : (!mode || !dbreg_known(mode) || mode->value != 0x18)) { return "dsp_peer_mode_unmodeled"; }
    return NULL;
}

/* Original MAIN 44A872D4 /446A26E8 accept these reply opcodes. The two
 * query callers copy complete records and inspect their final busy halfword.
 * This silent bring-up peer declares zeroed unused slots, not handset reset
 * coefficients. Unknown programming commands must not silently succeed.
 * Payload lengths include the native trailing logical-channel halfword.
 */
static const char *dbdsp_audio(DB2010DspPeer *p, unsigned logical,
                              const uint8_t *data, unsigned length, bool inlined)
{
    unsigned opcode = lduw_le_p(data), expected = 0, count = 2;
    if (logical == 11) {
        if (opcode != 0x0800) { return "dsp_peer_audio_opcode_unmodeled"; }
        expected = 46;
    } else {
        switch (opcode) {
        case 0x0900: expected = 6; break;
        case 0x0904: case 0x090a: expected = 8; break;
        case 0x0906: expected = 12; break;
        case 0x0908: expected = 10; break;
        case 0x090c:
            /* 446A22BC: the observed enable1 payload has a32-bit tap count
             * and128/256 coefficients. Native FA adds a trailer and padding.
             * Enable0 with count0 allocates eight bytes plus the trailer. */
            if (length < 6 || lduw_le_p(data + 4) > 1) {
                return "dsp_peer_audio_filter_unmodeled";
            }
            expected = 10;
            if (lduw_le_p(data + 4)) {
                if (length < 10) { return "dsp_peer_audio_filter_unmodeled"; }
                unsigned taps = ldl_le_p(data + 6);
                if (taps != 128 && taps != 256) { return "dsp_peer_audio_taps_unmodeled"; }
                expected = taps == 128 ? 288 : 544;
            }
            break;
        case 0x090e:
            /* MAIN446A24E8 /4469FBE4 programs five coefficient halfwords
             * per equalizer section;4469FD64 disables the bank. Native
             * allocation includes a channel trailer:24/16 bytes. Accept
             * control only, like090C: no physical EQ filtering is claimed. */
            if (length < 6 || lduw_le_p(data + 4) > 1) {
                return "dsp_peer_audio_filter_unmodeled";
            }
            expected = lduw_le_p(data + 4) ? 24 : 16;
            break;
        case 0x0910:
            if (length < 6 || lduw_le_p(data + 4) > 1) {
                return "dsp_peer_audio_filter_unmodeled";
            }
            expected = lduw_le_p(data + 4) ? 58 : 12;
            break;
        case 0x0912: expected = 36; count = 18; break;
        case 0x0914: expected = 46; count = 23; break;
        default: return "dsp_peer_audio_opcode_unmodeled";
        }
    }
    if (length != expected || inlined != (expected < 62) ||
        lduw_le_p(data + length - 2) != logical) {
        return "dsp_peer_audio_packet_unmodeled";
    }
    unsigned selector = lduw_le_p(data + 2);
    if (opcode == 0x090e && lduw_le_p(data + 4)) {
        unsigned sections = lduw_le_p(data + 6);
        unsigned section = lduw_le_p(data + 8);
        if (!sections || sections > 9 || section >= sections) {
            return "dsp_peer_audio_equalizer_section_unmodeled";
        }
    }
    if ((opcode == 0x0912 && selector >= 7) ||
        ((opcode == 0x0914 || opcode == 0x0910) && selector >= 5)) {
        return "dsp_peer_audio_slot_unmodeled";
    }
    if (opcode == 0x0910 && lduw_le_p(data + 4) && lduw_le_p(data + 6) > 3) {
        return "dsp_peer_audio_route_unmodeled";
    }
    if (p->state.rx_ready) { return "dsp_peer_audio_reply_pending"; }
    memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
    p->state.rx_mailbox[0] = (count << 11) | 10;
    p->state.rx_mailbox[1] = opcode + 1;
    if (count > 2) { p->state.rx_mailbox[2] = selector; }
    if (opcode == 0x0910) { p->state.audio_iir_active[selector] = lduw_le_p(data + 4); }
    if (opcode == 0x0914) { p->state.rx_mailbox[count - 1] = p->state.audio_iir_active[selector]; }
    p->state.rx_mailbox[count] = logical;
    p->state.audio_opcode = opcode; p->state.audio_requests++;
    p->state.rx_mma = false;
    p->state.rx_ring4 = false;
    p->state.rx_inline = true; p->state.rx_ready = true;
    p->state.status |= 2; p->state.phase = DBDSP_RX_WAIT;
    return NULL;
}

/* Native4488D5D8 maps0E05 to017D;4488D660 requires halfword1==1.
 * This opt-in synthetic mode acceptance supplies no decoder or completion. */
static void dbdsp_mma_reply(DB2010DspPeer *p)
{
    memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
    p->state.rx_mailbox[0] = 0x180a;
    p->state.rx_mailbox[1] = 0x0e05;
    p->state.rx_mailbox[2] = 1;
    p->state.rx_mailbox[3] = 18;
    p->state.rx_mma = true;
    p->state.rx_ring4 = false;
    p->state.rx_inline = true;
    p->state.rx_ready = true;
    p->state.status |= 2;
    p->state.phase = DBDSP_RX_WAIT;
}

static void dbdsp_audio_retire(DB2010AudioRuntime *a, unsigned frames)
{
    for (unsigned i = 0; i < frames * 2; i++) {
        uint16_t value = a->pcm[i];
        a->nonzero_samples += value != 0;
        a->pcm_hash = (a->pcm_hash ^ (value & 255)) * UINT64_C(1099511628211);
        a->pcm_hash = (a->pcm_hash ^ (value >> 8)) * UINT64_C(1099511628211);
    }
    a->frames_played += frames;
    a->pcm_frames -= frames;
    memmove(a->pcm, a->pcm + frames * 2, a->pcm_frames * 2 * sizeof(*a->pcm));
    a->samples.emitted = 0;
}

static void dbdsp_mma_mode(DB2010DspPeer *p, unsigned mode, uint64_t now)
{
    if (mode == 3 && p->state.ring4_announced) {
        /* New logical stream: discard decoder history and prior EOS state,
         * then reannounce capacity through the original ARM ring receiver.
         * Transport epoch changes prevent post-EOS samples sharing an epoch. */
        p->state.audio = (DB2010AudioRuntime){0};
        p->state.mma_configured = false;
        p->state.ring4_threshold = 0;
        db2010_audio_output_new_stream(p->output);
    }
    p->state.mma_mode = mode;
    p->state.mma_requests++;
    if (mode == 2) {
        /* Original4488C1F8 waits for0E05/status1 (native017D). Preserve
         * decoder, encoded input, unplayed PCM and ring ownership. Elapsed
         * PCM is retired once; no EOF, flush or completion is invented. */
        DB2010AudioRuntime *a = &p->state.audio;
        unsigned elapsed = db2010_audio_sample_pause(a, &p->sink, now);
        dbdsp_audio_retire(a, elapsed);
        a->active = false; a->paused = true; a->due = UINT64_MAX;
        dbdsp_mma_reply(p);
        return;
    }
    if (mode == 1) {
        DB2010AudioRuntime *a = &p->state.audio;
        if (a->active || a->paused || a->underrun) {
            unsigned elapsed = a->paused || a->underrun ? 0 : db2010_audio_sample_stop(a, &p->sink, now);
            dbdsp_audio_retire(a, elapsed);
            a->pcm_frames = 0; a->active = false; a->paused = false; a->interrupted = true;
            a->input_bytes = 0; a->due = UINT64_MAX;
            a->underrun = a->underrun_notified = false;
            db2010_audio_sample_end(a, &p->sink);
            /* Active4488C39C waits for017D after mode1. Unlike idle close,
             * this acceptance must reach the original0E05/FA/IRQ consumer.
             * No natural0E06 completion is synthesized for interruption. */
            dbdsp_mma_reply(p);
        }
        return; /* Idle close has no application wait; active stop is above. */
    }
    if (p->config.mma_ring && mode == 3) {
        /* Original448D526C type3 installs peer capacity/state4. 801 is this
         * host service's bounded buffer policy, not a measured DSP capacity.
         * Announce before accepting mode3, so native registration sees it. */
        memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
        p->state.rx_mailbox[0] = 0x180a;
        p->state.rx_mailbox[1] = 0x0304;
        p->state.rx_mailbox[2] = 801;
        p->state.rx_mailbox[3] = 20;
        p->state.rx_mma = false;
        p->state.rx_ring4 = true;
        p->state.rx_inline = true;
        p->state.rx_ready = true;
        p->state.status |= 2;
        p->state.phase = DBDSP_RX_WAIT;
        return;
    }
    dbdsp_mma_reply(p);
}

static void dbdsp_mailbox_complete(DB2010DspPeer *p, const DB2010DspEvent *e)
{
    DB2010FaLink *s = e->link;
    dbfm_consumed(&s->fa_mailbox);
    p->state.status |= 1; p->state.mailbox_events++;
    if (p->state.mailbox_only) {
        p->state.phase = DBDSP_DONE;
        unsigned count = s->fa_mailbox.payload_words;
        unsigned logical = s->fa_mailbox.words[count];
        if (p->config.midi_setup && logical == 9) {
            /* Uploaded DSP010EF0 rewrites0600 to0601 through the request
             * packet register. Preserve the qualified eight-word request;
             * ARM448825F0 checks only a nonnull reply pointer, no MMA status.
             * This host setup mock stops at the next unrecovered command. */
            memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
            p->state.rx_mailbox[0] = (count << 11) | 10;
            memcpy(p->state.rx_mailbox + 1, s->fa_mailbox.words + 1,
                   count * sizeof(uint16_t));
            p->state.midi_requests++;
            unsigned opcode = s->fa_mailbox.words[1];
            DB2010MidiStream *midi = &p->state.midi;
            if (opcode == 0x604) {
                /* Original448827CC registers ring1 only after0605. Provide
                 * capacity first through its native type3 receiver. 1001 is
                 * a bounded host policy (threshold750 requires >=752). */
                p->state.midi_prepared = true;
                p->state.midi_rx_ring = true;
                p->state.rx_mailbox[0] = 0x180a;
                p->state.rx_mailbox[1] = 0x301;
                p->state.rx_mailbox[2] = 1001;
                p->state.rx_mailbox[3] = 20;
                db2010_midi_stream_init(midi);
                db2010_audio_output_new_stream(p->output);
            } else if (opcode == 0x600) {
                p->state.midi_initialized = true;
                p->state.rx_mailbox[1] = 0x601;
            } else if (opcode == 0x60a) {
                midi->completion_requested = true;
                return;
            } else {
                memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
                p->state.rx_mailbox[0] = 0x100a;
                p->state.rx_mailbox[1] = opcode + 1;
                p->state.rx_mailbox[2] = 9;
                if (opcode == 0x608) {
                    const char *fault = db2010_midi_stream_advance(midi, &p->sink, e->now);
                    if (fault) { db2010_link_stop(&s->faults, fault, 0xfa040020, 4); return; }
                    unsigned mode = s->fa_mailbox.words[2];
                    if (!mode) { db2010_midi_stream_run(midi, e->now); }
                    else if (mode == 1) { db2010_midi_stream_pause(midi, &p->sink); }
                    else { db2010_midi_stream_stop(midi, &p->sink); }
                } else if (opcode == 0x61a) {
                    const char *fault = db2010_midi_stream_advance(midi, &p->sink, e->now);
                    if (fault) { db2010_link_stop(&s->faults, fault, 0xfa040020, 4); return; }
                    uint32_t time = midi->eof ? midi->last_time : midi->frames;
                    p->state.rx_mailbox[0] = 0x200a;
                    p->state.rx_mailbox[2] = time;
                    p->state.rx_mailbox[3] = time >> 16;
                    p->state.rx_mailbox[4] = 9;
                } else if (opcode == 0x606) {
                    p->state.midi_prepared = p->state.midi_ring_announced = false;
                } else if (opcode == 0x602) {
                    p->state.midi_initialized = false;
                }
            }
            p->state.rx_inline = true;
            p->state.rx_mma = p->state.rx_ring4 = false;
            p->state.rx_ready = true;
            p->state.status |= 2;
            p->state.phase = DBDSP_RX_WAIT;
        }
        if (p->config.mma_mode && logical == 18) {
            if (p->config.mma_setup && s->fa_mailbox.words[1] == 0x0e00) {
                /* 4488D5D8 maps0E01 to017E. 4488D6E4 checks status1 and
                 * converts high/low microseconds to milliseconds for its
                 * OSE offset. The virtual service clock is a host policy. */
                uint32_t clock_us = e->now / 1000;
                memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
                p->state.rx_mailbox[0] = 0x280a;
                p->state.rx_mailbox[1] = 0x0e01;
                p->state.rx_mailbox[2] = 1;
                p->state.rx_mailbox[3] = clock_us >> 16;
                p->state.rx_mailbox[4] = clock_us & 0xffff;
                p->state.rx_mailbox[5] = 18;
                p->state.rx_mma = true;
                p->state.rx_ring4 = false;
                p->state.rx_inline = true;
                p->state.rx_ready = true;
                p->state.status |= 2;
                p->state.phase = DBDSP_RX_WAIT;
                p->state.mma_requests++;
                if (p->config.mma_buffer) { p->state.mma_configured = true; }
                p->state.audio.source_rate = s->fa_mailbox.words[2] == 3 ? 16000 :
                    s->fa_mailbox.words[2] == 4 ? 22050 :
                    s->fa_mailbox.words[2] == 7 ? 44100 : 48000;
                p->state.audio.source_channels = s->fa_mailbox.words[3] + 1;
                p->state.audio.source_codec = s->fa_mailbox.words[2] == 3 ? 1 : 0;
                p->state.audio.coefficient_ready = false;
            } else {
                dbdsp_mma_mode(p, s->fa_mailbox.words[2], e->now);
                if (p->config.mma_playback && p->state.mma_mode == 0) {
                    DB2010AudioRuntime *a = &p->state.audio;
                    a->active = true;
                    if (a->underrun) {
                        /* Native status1 rebuffering restarts mode0, preserving
                         * AAC overlap/PNS, source cursor, PCM totals and hash. */
                        a->underrun = a->underrun_notified = false;
                        a->paused = false;
                        a->due = e->now;
                    } else if (a->paused) {
                        a->paused = false;
                        a->due = e->now + db2010_audio_duration(a->pcm_frames);
                    } else {
                        a->start_ns = e->now; a->due = e->now;
                        a->pcm_hash = UINT64_C(14695981039346656037);
                    }
                }
            }
        }
        if (p->config.mma_drain && logical == 20 && s->fa_mailbox.words[1] == 0x0704) {
            /* Original448D526C type7 ->448D5240 clears read/write indexes
             * and returns the occupied word count, including zero. An active
             * stop also returns copied-but-uncredited words, without refill. */
            memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
            p->state.rx_mailbox[0] = 0x180a;
            p->state.rx_mailbox[1] = 0x0204;
            unsigned released = p->state.ring4_buffered_words + p->state.audio.credit;
            p->state.rx_mailbox[2] = released;
            p->state.rx_mailbox[3] = 20;
            memset(p->state.ring4_buffer, 0, sizeof(p->state.ring4_buffer));
            p->state.ring4_buffered_words = 0;
            p->state.audio.credit = 0;
            p->state.audio.credit_notify = false;
            p->state.audio.words_credited += released;
            p->state.audio.rx_kind = 1;
            p->state.audio.credits_offered++;
            p->state.rx_inline = true; p->state.rx_mma = false; p->state.rx_ring4 = false;
            p->state.rx_ready = true; p->state.status |= 2; p->state.phase = DBDSP_RX_WAIT;
        }
        if (p->config.mma_drain && logical == 20 && s->fa_mailbox.words[1] == 0x0104) {
            /* Original448D555C unregisters the writer through type1. The
             * opposite receiver enters state1 without an application reply. */
            p->state.ring4_registered = false;
        }
        if (p->config.midi_setup && logical == 20 && s->fa_mailbox.words[1] == 0x401) {
            p->state.midi_ring_registered = true;
            p->state.midi_ring_threshold = s->fa_mailbox.words[2];
        }
        if (p->config.midi_setup && logical == 20 && (s->fa_mailbox.words[1] & 255) == 1) {
            DB2010MidiStream *midi = &p->state.midi;
            unsigned opcode = s->fa_mailbox.words[1];
            if (opcode == 0x801) {
                const char *fault = db2010_midi_stream_append(midi, s->fa_mailbox.words + 3,
                                                            s->fa_mailbox.words[2]);
                if (fault) { db2010_link_stop(&s->faults, fault, 0xfa040020, 4); return; }
            } else if (opcode == 0x101) { p->state.midi_ring_registered = false; }
            else if (opcode == 0x701) {
                midi->credit += midi->used; midi->used = 0;
                memset(midi->ring, 0, sizeof(midi->ring));
                midi->credit_notify = false;
                midi->flush_pending = true;
            }
        }
        if (p->config.mma_playback && logical == 20 && s->fa_mailbox.words[1] == 0x0804) {
            for (unsigned i = 0; i < s->fa_mailbox.words[2]; i++) {
                p->state.ring4_buffer[p->state.ring4_buffered_words++] = s->fa_mailbox.words[i + 3];
            }
            p->state.ring4_packets++;
            if (!p->state.audio.pcm_frames) { p->state.audio.due = e->now; }
        }
        if (p->config.mma_ring && logical == 20 && s->fa_mailbox.words[1] == 0x0404) {
            p->state.ring4_registered = true;
            p->state.ring4_threshold = s->fa_mailbox.words[2];
            p->state.ring4_registrations++;
        }
        if (p->config.modexp && (logical == 11 || logical == 12)) {
            uint8_t data[62];
            for (unsigned i = 0; i < count; i++) {
                stw_le_p(data + 2 * i, s->fa_mailbox.words[i + 1]);
            }
            const char *fault = dbdsp_audio(p, logical, data, count * 2, true);
            if (fault) { db2010_link_stop(&s->faults, fault, 0xfa040040, count * 2); }
        }
    }
    else {
        p->state.source = s->dma->state.channel[2][0];
        p->state.phase = DBDSP_DMA; p->state.due = e->now + p->config.latency;
    }
}
static void dbdsp_dma_word(DB2010DspPeer *p, const DB2010DspEvent *e)
{
    DB2010FaLink *s = e->link;
    uint32_t *ch = s->dma->state.channel[2];
    unsigned length = p->config.variable_dma ? p->state.length : 224;
    if (ch[4] != 0xc8c7 || ch[1] != 0xfa040020 || ch[2] ||
        ch[0] != p->state.source + p->state.bytes ||
        ch[3] != (0xa4492000 | ((length - p->state.bytes) / 4))) {
        db2010_link_stop(&s->faults, "dsp_peer_dma_progress_unmodeled", 0xfa040020, 4); return;
    }
    DB2010MemoryView view;
    DB2010MemoryView *r = db2010_memory_view(&s->memory, ch[0], 4, &view);
    if (!r) { db2010_link_stop(&s->faults, "dsp_peer_dma_source_unmapped", ch[0], 4); return; }
    for (unsigned i = 0; i < 4; i++) {
        if (!r->unchecked && !r->known[ch[0] - r->base + i]) { db2010_link_stop(&s->faults, "dsp_peer_dma_source_unknown", ch[0] + i, 1); return; }
    }
    memcpy(p->state.payload + p->state.bytes,
           (uint8_t *)r->bytes + ch[0] - r->base, 4);
    p->state.bytes += 4; dbdma_word_transferred(s->dma, 2);
    if (p->config.variable_dma) { p->state.total_bytes += 4; }
    p->state.due = e->now + 64;
    if (p->state.bytes == length) {
        dbdma_complete(s->dma, 2);
        p->state.phase = DBDSP_TRANSFER_PENDING; p->state.due = e->now + p->config.latency;
    }

}
static void dbdsp_transfer_complete(DB2010DspPeer *p, const DB2010DspEvent *e)
{
    if (p->config.midi_setup && p->state.logical == 20 &&
        lduw_le_p(p->state.payload) == 0x801) {
        unsigned bytes = p->state.bytes, count = lduw_le_p(p->state.payload + 2);
        const char *fault = NULL;
        if (!p->state.midi_ring_registered || count < 28 || count > DB2010_MIDI_RING_WORDS ||
            bytes != ((count * 2 + 6 + 31) & ~31u) ||
            lduw_le_p(p->state.payload + bytes - 2) != 20) {
            fault = "dsp_peer_midi_ring_data_packet_unmodeled";
        } else {
            uint16_t words[DB2010_MIDI_RING_WORDS];
            for (unsigned i = 0; i < count; i++) { words[i] = lduw_le_p(p->state.payload + 4 + i * 2); }
            fault = db2010_midi_stream_append(&p->state.midi, words, count);
        }
        if (fault) { db2010_link_stop(&e->link->faults, fault, p->state.source, bytes); return; }
        p->state.status |= 8; p->state.transfer_events++; p->state.phase = DBDSP_DONE;
        return;
    }
    if (p->config.mma_buffer && p->state.logical == 18) {
        /* Original MAIN443811AE, filter ID2; authenticated by the Opera
         * source/descriptor audit. Only this known configuration is accepted.
         * Host decoding/conversion replaces the DSP filter service; no claim
         * of executing these coefficients or original DSP instructions. */
        static const int16_t coefficients[32] = {
            -140, -350, -290, 37, 269, 22, -303, -131,
            340, 274, -332, -458, 264, 657, -111, -856,
            -139, 1022, 506, -1119, -999, 1100, 1641, -898,
            -2476, 400, 3637, 675, -5612, -3470, 11642, 27340
        };
        /* Video16000Hz filter3, original MAIN table authenticated from the
         * native48-tap upload. The host conversion is still a substitute,
         * not an implementation of this DSP FIR arithmetic. */
        static const int16_t video_coefficients[48] = {
            -102, -273, -342, -339, -141, 98, 260, 196,
            -40, -276, -293, -51, 278, 408, 192, -233,
            -517, -379, 123, 595, 604, 68, -613, -848,
            -352, 541, 1086, 733, -343, -1285, -1213, -19,
            1404, 1798, 607, -1383, -2506, -1531, 1128, 3414,
            3068, -410, -4818, -6262, -1716, 8576, 20625, 28706
        };
        bool video = p->state.audio.source_rate == 16000;
        unsigned taps = video ? ARRAY_SIZE(video_coefficients) : ARRAY_SIZE(coefficients);
        unsigned bytes = video ? 128 : 96;
        const uint8_t *data = p->state.payload;
        bool valid = p->state.bytes == bytes && lduw_le_p(data) == 0x0e02 &&
            !lduw_le_p(data + 2) && !lduw_le_p(data + 4) &&
            lduw_le_p(data + 6) == taps && lduw_le_p(data + bytes - 2) == 18;
        for (unsigned i = 0; i < taps && valid; i++) {
            valid = lduw_le_p(data + 8 + i * 2) == (uint16_t)(video ? video_coefficients[i] : coefficients[i]);
        }
        if (!valid) {
            db2010_link_stop(&e->link->faults, "dsp_peer_mma_coefficients_unmodeled",
                            p->state.source, p->state.bytes);
            return;
        }
        p->state.audio.coefficient_ready = true;
        p->state.status |= 8; p->state.transfer_events++; p->state.mma_requests++;
        /* Original4488D5D8 routes0E03 to017F; the waiter only frees its
         * payload. Minimal opcode-only acknowledgement, no invented status. */
        memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
        p->state.rx_mailbox[0] = 0x100a;
        p->state.rx_mailbox[1] = 0x0e03;
        p->state.rx_mailbox[2] = 18;
        p->state.rx_mma = true; p->state.rx_ring4 = false;
        p->state.rx_inline = true; p->state.rx_ready = true;
        p->state.status |= 2; p->state.phase = DBDSP_RX_WAIT;
        return;
    }
    if (p->config.mma_buffer && p->state.logical == 20) {
        /* Original448D51CC sends opcode/count plus counted ring halfwords;
         * 44736114 rounds DMA allocations to32 bytes, 447367A8 installs the
         * final logical trailer. Padding is opaque and never audio data.
         * Retain all metadata/source words; no consumer means no credits. */
        const uint8_t *data = p->state.payload;
        unsigned length = p->state.bytes;
        unsigned count = length >= 4 ? lduw_le_p(data + 2) : 0;
        const char *fault = NULL;
        if (length < 64 || lduw_le_p(data) != 0x0804 || count < 28 || count > 800 ||
            length != ((count * 2 + 6 + 31) & ~31u) || lduw_le_p(data + length - 2) != 20) {
            fault = "dsp_peer_ring_data_packet_unmodeled";
        } else if (lduw_le_p(data + 4) > 2 || lduw_le_p(data + 6) > 1 ||
                   lduw_le_p(data + 8) != 1 || lduw_le_p(data + 14) != count - 6) {
            fault = "dsp_peer_ring_data_header_unmodeled";
        } else if (count > 800 - p->state.ring4_buffered_words) {
            fault = "dsp_peer_ring_data_overflow";
        }
        if (fault) {
            db2010_link_stop(&e->link->faults, fault, p->state.source, length);
            return;
        }
        for (unsigned i = 0; i < count; i++) {
            p->state.ring4_buffer[p->state.ring4_buffered_words + i] = lduw_le_p(data + 4 + i * 2);
        }
        p->state.ring4_buffered_words += count;
        p->state.ring4_packets++;
        if (p->config.mma_playback && p->state.audio.active && !p->state.audio.pcm_frames) {
            p->state.audio.due = e->now;
        }
    }
    p->state.status |= 8; p->state.transfer_events++; p->state.phase = DBDSP_DONE;
    if (p->config.modexp && (p->state.logical == 11 || p->state.logical == 12)) {
        const char *fault = dbdsp_audio(p, p->state.logical, p->state.payload,
                                      p->state.bytes, false);
        if (fault) { db2010_link_stop(&e->link->faults, fault, p->state.source, p->state.bytes); }
        return;
    }
    if (p->config.modexp && p->state.logical == 15) {
        const char *fault = dbdsp_modexp(p);
        if (fault) { db2010_link_stop(&e->link->faults, fault, p->state.source, p->state.bytes); return; }
        p->state.phase = DBDSP_RX_OFFER; p->state.due = e->now + p->config.latency;
    }
}
static void dbdsp_dispatch(DB2010DspPeer *p, const DB2010DspEvent *e)
{
    switch (e->kind) {
    case DBDSP_REQUEST:
        p->state.mailbox_only = e->mailbox_only;
        if (e->mailbox_only) { p->state.message_requests++; }
        else if (p->config.variable_dma) {
            p->state.length = e->length; p->state.bytes = 0; p->state.logical = e->logical;
            p->state.dma_messages++;
        }
        p->state.phase = DBDSP_MAILBOX_PENDING;
        p->state.due = e->now + p->config.latency; p->state.requests++;
        return;
    case DBDSP_ACK_STATUS:
        e->access->value = p->state.status;
        if (e->access->commit) { p->state.status = 0; p->state.status_reads++; }
        return;
    case DBDSP_TICK:
        if (e->now < p->state.due) { return; }
        switch (p->state.phase) {
        case DBDSP_MAILBOX_PENDING: dbdsp_mailbox_complete(p, e); return;
        case DBDSP_DMA: dbdsp_dma_word(p, e); return;
        case DBDSP_TRANSFER_PENDING: dbdsp_transfer_complete(p, e); return;
        case DBDSP_MMA_MODE_OFFER: dbdsp_mma_reply(p); return;
        case DBDSP_MIDI_PREPARE_OFFER:
            memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
            p->state.rx_mailbox[0] = 0x100a;
            p->state.rx_mailbox[1] = 0x605;
            p->state.rx_mailbox[2] = 9;
            p->state.rx_inline = true; p->state.rx_ready = true;
            p->state.status |= 2; p->state.phase = DBDSP_RX_WAIT;
            return;
        case DBDSP_RX_OFFER:
            p->state.rx_mma = false;
            p->state.rx_ring4 = false;
            p->state.rx_inline = false;
            memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
            p->state.rx_mailbox[0] = 0x4814; /* nine control halfwords */
            p->state.rx_mailbox[1] = 2; p->state.rx_mailbox[2] = 15;
            p->state.rx_mailbox[3] = sizeof(p->state.response) / 2;
            p->state.rx_mailbox[5] = 0x1000; /* declared peer-memory address */
            p->state.rx_mailbox[6] = 8;
            p->state.rx_ready = true; p->state.status |= 2;
            p->state.phase = DBDSP_RX_WAIT; return;
        case DBDSP_RX_DMA: dbdsp_rx_word(p, e); return;
        case DBDSP_RX_COMPLETE:
            p->state.selector &= ~1u; p->state.status |= 0x10;
            p->state.replies++; p->state.phase = DBDSP_DONE; return;
        case DBDSP_IDLE: case DBDSP_DONE: return;
        default: return;
        }
    default: g_assert_not_reached();
    }
}
/* Host service processing is separate from FA transport progress. Only copied
 * ring words return credits; only elapsed PCM completes output frames. */
static void dbdsp_audio_tick(DB2010DspPeer *p, DB2010FaLink *s, uint64_t now)
{
    if (!p->config.mma_playback) { return; }
    DB2010AudioRuntime *a = &p->state.audio;
    if (a->interrupted || a->paused) { return; }
    if (p->config.mma_stream) { db2010_audio_sample_advance(a, &p->sink, now); }
    if ((a->active || (p->config.mma_drain && a->completed && p->state.ring4_buffered_words)) && now >= a->due) {
        if (a->pcm_frames) {
            dbdsp_audio_retire(a, a->pcm_frames);
        }
        while (a->input_bytes < db2010_audio_frame_bytes(a) && p->state.ring4_buffered_words) {
            unsigned before = p->state.ring4_buffered_words;
            uint16_t *b = p->state.ring4_buffer;
            unsigned words = b[5], count = words + 6;
            if (count > before || a->input_bytes + words * 2 > sizeof(a->input) ||
                (a->eof && !(p->config.mma_drain && b[0] == 2 && !words)) ||
                b[1] > 1 || b[2] != 1 ||
                (b[0] != 1 && b[0] != 2) || (b[0] == 2 && words)) {
                db2010_link_stop(&s->faults, "dsp_peer_audio_stream_header_unmodeled", 0xfa040020, 4);
                return;
            }
            for (unsigned i = 0; i < words; i++) {
                a->input[a->input_bytes++] = b[6 + i] >> 8;
                a->input[a->input_bytes++] = b[6 + i] & 255;
            }
            a->eof = b[0] == 2;
            p->state.ring4_buffered_words -= count;
            memmove(b, b + count, p->state.ring4_buffered_words * sizeof(*b));
            memset(b + p->state.ring4_buffered_words, 0, count * sizeof(*b));
            a->credit += count;
            a->credit_notify |= before > p->state.ring4_threshold &&
                p->state.ring4_buffered_words <= p->state.ring4_threshold;
        }
        QDict *encoded = NULL;
        uint64_t prior_frames = a->frames_decoded;
        if (!p->omit_diagnostics && a->source_codec == 1 && a->input_bytes >= 8) {
            unsigned bytes = (a->input[4] << 5) | (a->input[5] >> 3);
            if (bytes > 8 && bytes <= a->input_bytes) {
                encoded = qdict_new();
                GString *hex = g_string_sized_new(bytes * 2);
                for (unsigned i = 0; i < bytes; i++) { g_string_append_printf(hex, "%02x", a->input[i]); }
                qdict_put_str(encoded, "bytes_hex", hex->str); g_string_free(hex, true);
                qdict_put_int(encoded, "decoded_index", prior_frames);
                qdict_put_int(encoded, "virtual_ns", now);
                qdict_put_int(encoded, "stream_start_ns", a->start_ns);
            }
        }
        const char *fault = db2010_audio_decode(a);
        if (encoded) {
            if (!fault && a->frames_decoded > prior_frames) {
                p->encoded_frame_count++;
                qlist_append(p->encoded_frames, encoded);
                if (qlist_size(p->encoded_frames) > 1024) { qobject_unref(qlist_pop(p->encoded_frames)); }
            } else { qobject_unref(encoded); }
        }
        if (fault || (a->eof && a->input_bytes && !a->pcm_frames)) {
            db2010_link_stop(&s->faults, fault ? fault : "dsp_peer_audio_truncated_source", 0xfa040020, 4);
            return;
        }
        if (a->pcm_frames) {
            a->due = MAX(a->start_ns + (a->frames_played + a->pcm_frames) * UINT64_C(1000000000) / 48000,
                         now + db2010_audio_duration(a->pcm_frames));
        } else {
            a->due = UINT64_MAX;
            if (a->eof) { a->active = false; a->completed = true; }
            else if (a->source_codec == 1 && a->frames_decoded &&
                     !a->input_bytes && !p->state.ring4_buffered_words) {
                /* Actual starvation: all decoded PCM elapsed, no encoded
                 * input and no source EOF. Native4488CB64 status1 changes
                 * playing state3 to buffering4 and requests native refill. */
                a->active = false; a->underrun = true;
                a->underrun_notified = false;
            }
        }
    }
    /* A checkpoint16 continuation may already be waiting in this exact
     * starvation state, before its underrun notification was implemented. */
    if (a->source_codec == 1 && a->active && a->frames_decoded &&
        a->due == UINT64_MAX && !a->pcm_frames && !a->input_bytes &&
        !p->state.ring4_buffered_words && !a->eof) {
        a->active = false; a->underrun = true; a->underrun_notified = false;
    }
    if (p->config.mma_stream && a->completed) { db2010_audio_sample_end(a, &p->sink); }
    if (p->state.phase != DBDSP_DONE || p->state.rx_ready || a->rx_kind) { return; }
    bool underrun = a->underrun && !a->underrun_notified;
    if (underrun || a->credit || (a->completed && !a->completions_acked)) {
        memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
        p->state.rx_mailbox[0] = 0x180a;
        if (a->credit) {
            p->state.rx_mailbox[1] = a->credit_notify ? 0x0504 : 0x0204;
            p->state.rx_mailbox[2] = a->credit;
            p->state.rx_mailbox[3] = 20;
            a->credit_notify = false;
            a->words_credited += a->credit;
            a->credit = 0; a->rx_kind = 1; a->credits_offered++;
        } else if (underrun) {
            p->state.rx_mailbox[1] = 0x0e06;
            p->state.rx_mailbox[2] = 1;
            p->state.rx_mailbox[3] = 18;
            a->rx_kind = 3; a->underrun_notified = true;
            a->underruns_offered++;
        } else {
            /* Original0E06 -> OS0180 ->4488CB64 status2 performs native
             * drain/end cleanup. Send only after an actual EOF and PCM end. */
            p->state.rx_mailbox[1] = 0x0e06;
            p->state.rx_mailbox[2] = 2;
            p->state.rx_mailbox[3] = 18;
            a->rx_kind = 2;
        }
        p->state.rx_inline = true; p->state.rx_mma = false; p->state.rx_ring4 = false;
        p->state.rx_ready = true; p->state.status |= 2; p->state.phase = DBDSP_RX_WAIT;
    }
}

static void dbdsp_midi_tick(DB2010DspPeer *p, DB2010FaLink *s, uint64_t now)
{
    if (!p->config.midi_setup) { return; }
    DB2010MidiStream *m = &p->state.midi;
    const char *fault = db2010_midi_stream_advance(m, &p->sink, now);
    if (fault) { db2010_link_stop(&s->faults, fault, 0xfa040020, 4); return; }
    if (p->state.phase != DBDSP_DONE || p->state.rx_ready || m->rx_kind) { return; }
    bool complete = m->completed && m->completion_requested && !m->completion_sent;
    if (!m->credit && !m->flush_pending && !complete) { return; }
    memset(p->state.rx_mailbox, 0, sizeof(p->state.rx_mailbox));
    if (m->credit || m->flush_pending) {
        p->state.rx_mailbox[0] = 0x180a;
        p->state.rx_mailbox[1] = m->credit_notify ? 0x501 : 0x201;
        p->state.rx_mailbox[2] = m->credit;
        p->state.rx_mailbox[3] = 20;
        m->words_credited += m->credit; m->credit = 0;
        m->credit_notify = false; m->rx_kind = 1;
        m->flush_pending = false;
    } else {
        p->state.rx_mailbox[0] = 0x200a;
        p->state.rx_mailbox[1] = 0x60b;
        p->state.rx_mailbox[2] = m->last_time;
        p->state.rx_mailbox[3] = m->last_time >> 16;
        p->state.rx_mailbox[4] = 9;
        m->completion_sent = true; m->rx_kind = 2;
    }
    p->state.rx_inline = p->state.rx_ready = true;
    p->state.rx_mma = p->state.rx_ring4 = false;
    p->state.status |= 2; p->state.phase = DBDSP_RX_WAIT;
}

int64_t dbdsp_deadline(DB2010DspPeer *p)
{
    int64_t due = p->state.phase != DBDSP_IDLE && p->state.phase != DBDSP_DONE &&
        p->state.phase != DBDSP_RX_WAIT ? p->state.due : -1;
    if (p->config.midi_setup) {
        DB2010MidiStream *m = &p->state.midi;
        int64_t midi_due = db2010_midi_stream_deadline(m);
        if (midi_due >= 0) { due = due < 0 ? midi_due : MIN(due, midi_due); }
        if (p->state.phase == DBDSP_DONE && (m->credit || m->flush_pending ||
            (m->completed && m->completion_requested && !m->completion_sent))) { due = 0; }
    }
    if (p->config.mma_playback) {
        DB2010AudioRuntime *a = &p->state.audio;
        int64_t sample_due = p->config.mma_stream ? db2010_audio_sample_deadline(a) : -1;
        if (sample_due >= 0) { due = due < 0 ? sample_due : MIN(due, sample_due); }
        if (a->active && a->due != UINT64_MAX) {
            due = due < 0 ? a->due : MIN((uint64_t)due, a->due);
        }
        if (!a->interrupted && !a->paused && p->state.phase == DBDSP_DONE &&
            (a->credit || (a->completed && !a->completions_acked) ||
             (a->underrun && !a->underrun_notified))) {
            due = 0;
        }
        if (a->source_codec == 1 && a->active && a->frames_decoded &&
            a->due == UINT64_MAX && !a->pcm_frames && !a->input_bytes &&
            !p->state.ring4_buffered_words && !a->eof) { due = 0; }
    }
    return due;
}

void dsp_peer_advance(DB2010FaLink *s)
{
    if (!s->has_dsp_peer) { return; }
    DB2010DspPeer *p = s->dsp_peer;
    DB2010DspEvent e = {.kind = DBDSP_TICK, .link = s, .now = db2010_virtual_ns()};
    dbdsp_dispatch(p, &e);
    dbdsp_audio_tick(p, s, e.now);
    dbdsp_midi_tick(p, s, e.now);
    db2010_signal_set(&s->dsp_irq, (p->state.mask & p->state.status) != 0);
}

/* Positive 1024-bit arithmetic. Add/reduce avoids overflow at the limb boundary;
 * division by R uses modular halving for an odd modulus. No canned RSA bytes. */
#define RSA_WORDS 32
typedef uint32_t DBDspInteger[RSA_WORDS];
static int rsa_cmp(const uint32_t *a, const uint32_t *b)
{
    for (int i = RSA_WORDS - 1; i >= 0; i--) {
        if (a[i] != b[i]) { return a[i] > b[i] ? 1 : -1; }
    }
    return 0;
}
static void rsa_sub(uint32_t *out, const uint32_t *a, const uint32_t *b)
{
    uint64_t borrow = 0;
    for (unsigned i = 0; i < RSA_WORDS; i++) {
        uint64_t sub = (uint64_t)b[i] + borrow;
        uint32_t old = a[i]; out[i] = old - sub; borrow = old < sub;
    }
}
static void rsa_addmod(uint32_t *out, const uint32_t *a, const uint32_t *b,
                       const uint32_t *n)
{
    DBDspInteger gap;
    rsa_sub(gap, n, a);
    if (rsa_cmp(b, gap) >= 0) { rsa_sub(out, b, gap); return; }
    uint64_t carry = 0;
    for (unsigned i = 0; i < RSA_WORDS; i++) {
        carry += (uint64_t)a[i] + b[i]; out[i] = carry; carry >>= 32;
    }
}
static void rsa_mulmod(uint32_t *out, const uint32_t *a, const uint32_t *b,
                       const uint32_t *n)
{
    DBDspInteger product = {0}, power;
    memcpy(power, a, sizeof(power));
    for (unsigned i = 0; i < RSA_WORDS * 32; i++) {
        if ((b[i / 32] >> (i % 32)) & 1) { rsa_addmod(product, product, power, n); }
        rsa_addmod(power, power, power, n);
    }
    memcpy(out, product, sizeof(product));
}
static const char *dbdsp_modexp(DB2010DspPeer *p)
{
    DBDspInteger fields[4] = {{0}}, radix = {1}, base, result = {1};
    const uint8_t *packet = p->state.payload;
    if (p->state.bytes != 1088 || lduw_le_p(packet) != 0x0b01) {
        return "dsp_peer_modexp_request_unmodeled";
    }
    for (unsigned f = 0; f < 4; f++) {
        const uint8_t *s = packet + 2 + f * 266;
        unsigned count = lduw_le_p(s);
        if (count != (f == 2 ? 5 : 67) || lduw_le_p(s + 2)) {
            return "dsp_peer_modexp_field_unmodeled";
        }
        for (unsigned i = 0; i < (count - 3) / 2; i++) {
            fields[f][i] = ldl_le_p(s + 6 + 4 * i);
        }
        uint32_t high = fields[f][(count - 3) / 2 - 1];
        /* Serializer copies cached metadata; the observed Montgomery base
         * has top28 although its current high limb has bit31 set. */
        if (!high || lduw_le_p(s + 4) > 31) {
            return "dsp_peer_modexp_top_unmodeled";
        }
    }
    uint32_t *n = fields[3];
    if (!(n[0] & 1) || !(n[31] & 0x80000000) || fields[2][0] != 65537 ||
        rsa_cmp(fields[0], n) >= 0 || rsa_cmp(fields[1], n) >= 0) {
        return "dsp_peer_modexp_operands_unmodeled";
    }
    for (unsigned i = 0; i < RSA_WORDS * 32; i++) { rsa_addmod(radix, radix, radix, n); }
    if (rsa_cmp(radix, fields[0])) { return "dsp_peer_modexp_radix_mismatch"; }
    memcpy(base, fields[1], sizeof(base));
    for (unsigned bit = 0; bit < RSA_WORDS * 32; bit++) {
        uint64_t carry = 0;
        if (base[0] & 1) {
            for (unsigned i = 0; i < RSA_WORDS; i++) {
                carry += (uint64_t)base[i] + n[i]; base[i] = carry; carry >>= 32;
            }
        }
        for (int i = RSA_WORDS - 1; i >= 0; i--) {
            uint32_t next = base[i] & 1; base[i] = (base[i] >> 1) | (carry << 31); carry = next;
        }
    }
    for (uint32_t exponent = fields[2][0]; exponent; exponent >>= 1) {
        if (exponent & 1) { rsa_mulmod(result, result, base, n); }
        rsa_mulmod(base, base, base, n);
    }
    unsigned used = RSA_WORDS;
    while (used > 1 && !result[used - 1]) { used--; }
    memset(p->state.response, 0, sizeof(p->state.response));
    stw_le_p(p->state.response, 0x0b01); /* ignored by consumer; echo hypothesis */
    stw_le_p(p->state.response + 2, used * 2 + 3);
    stw_le_p(p->state.response + 6, result[used - 1] ? 31 - clz32(result[used - 1]) : 0xffff);
    for (unsigned i = 0; i < used; i++) { stl_le_p(p->state.response + 8 + 4 * i, result[i]); }
    return NULL;
}

const char *dsp_peer_rx_arm_fault(void *opaque)
{
    DB2010FaLink *s = opaque;
    DB2010DspPeer *p = s->dsp_peer;
    uint32_t *ch = s->dma->state.channel[2];
    if (!p->config.modexp || p->state.phase != DBDSP_RX_WAIT || p->state.rx_ready ||
        ch[0] != 0xfa040020 || ch[2] || ch[3] != 0xa8492022 || ch[4] != 0xf0c6 ||
        s->dma->state.global[4] != 1 || (s->dma->state.raw_tc & 4)) {
        return "dsp_peer_rx_dma_configuration_unmodeled";
    }
    if (ch[1] % 32 || !s->memory.write(s->memory.opaque, ch[1], NULL, 136)) {
        return "dsp_peer_rx_destination_unmodeled";
    }
    return NULL;
}
static const char *dsp_rx_register(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010FaLink *s = opaque;
    DB2010DspPeer *p = s->dsp_peer;
    switch (d->id) {
    case 4:
        if (!a->write) { a->value = p->state.rx_ready; return NULL; }
        if (!p->state.rx_ready || a->value != 0xfffffffe) { return "dsp_peer_rx_ack_unmodeled"; }
        if (a->commit) {
            p->state.rx_ready = false;
            if (p->state.rx_inline) {
                if (p->config.midi_setup && p->state.midi_rx_ring) {
                    p->state.midi_rx_ring = false;
                    p->state.midi_ring_announced = true;
                    p->state.phase = DBDSP_MIDI_PREPARE_OFFER;
                    p->state.due = a->now + p->config.latency;
                    return NULL;
                }
                if (p->config.midi_setup &&
                    ((p->state.rx_mailbox[p->state.rx_mailbox[0] >> 11] == 9 &&
                     p->state.rx_mailbox[1] >= 0x601 && p->state.rx_mailbox[1] <= 0x621) ||
                     p->state.midi.rx_kind)) {
                    p->state.midi.rx_kind = 0;
                    p->state.midi_replies++;
                    p->state.phase = DBDSP_DONE;
                    return NULL;
                }
                if (p->config.mma_playback && p->state.audio.rx_kind) {
                    if (p->state.audio.rx_kind == 1) { p->state.audio.credits_acked++; }
                    else if (p->state.audio.rx_kind == 2) { p->state.audio.completions_acked++; }
                    else { p->state.audio.underruns_acked++; }
                    p->state.audio.rx_kind = 0;
                    p->state.phase = DBDSP_DONE;
                    return NULL;
                }
                if (p->state.rx_ring4) {
                    p->state.rx_ring4 = false;
                    p->state.ring4_announced = true;
                    p->state.ring4_announcements++;
                    p->state.phase = DBDSP_MMA_MODE_OFFER;
                    p->state.due = a->now + p->config.latency;
                    return NULL;
                }
                if (p->state.rx_mma) {
                    p->state.mma_replies++;
                } else {
                    p->state.audio_replies++;
                }
                p->state.phase = DBDSP_DONE;
            }
        }
        return NULL;
    case 0x80:
        if (!p->state.rx_ready) { return "dsp_peer_rx_empty"; }
        a->value = p->state.rx_mailbox[d->element]; return NULL;
    case 0x14:
        if (!a->write) { a->value = p->state.selector; return NULL; }
        if (a->value == 0 || a->value == 0x10 || a->value == 0x18) {
            if (a->commit) { p->state.selector = a->value; } return NULL;
        }
        if (a->value != 0x19 || p->state.phase != DBDSP_RX_WAIT || p->state.rx_ready ||
            p->state.rx_address != 0x1000 || p->state.rx_count != 34 ||
            s->dma->state.channel[2][4] != 0xf0c7) {
            return "dsp_peer_rx_start_unmodeled";
        }
        if (a->commit) {
            p->state.selector = a->value; p->state.rx_bytes = 0;
            p->state.phase = DBDSP_RX_DMA; p->state.due = a->now + p->config.latency;
        }
        return NULL;
    case 0x18: case 0x1c:
        if (p->state.phase != DBDSP_RX_WAIT || a->value != (d->id == 0x18 ? 0x1000 : 34)) {
            return "dsp_peer_rx_request_unmodeled";
        }
        if (a->commit) {
            if (d->id == 0x18) { p->state.rx_address = a->value; }
            else { p->state.rx_count = a->value; }
        }
        return NULL;
    default: g_assert_not_reached();
    }
}
const DB2010RegMap *dbdsp_rx_map(void)
{
#define RX_RW .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = dsp_rx_register}, \
              .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = dsp_rx_register}
#define RX_W .read = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH32, .fault = "dsp_peer_rx_request_read_unmodeled"}, \
             .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32, .callback = dsp_rx_register}
    static const DB2010RegRule rules[] = {
        DB_REG(4, .id = 4, .alignment = 4, RX_RW),
        DB_REG(0x14, .id = 0x14, .alignment = 4, RX_RW),
        DB_REG(0x18, .id = 0x18, .alignment = 4, RX_W),
        DB_REG(0x1c, .id = 0x1c, .alignment = 4, RX_W),
        DB_ARRAY(0x80, 32, .id = 0x80, .stride = 2, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = dsp_rx_register},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH16, .fault = "dsp_peer_rx_window_write_unmodeled"}),
    };
#undef RX_RW
#undef RX_W
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xfa040000, rules,
            ARRAY_SIZE(rules), "dsp_peer_rx_access_unmodeled");
        if (fault) { g_error("DB2010 DSP RX layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *db2010_peer_rx_access(void *opaque, DB2010Access *a)
{
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbdsp_rx_map(), a, &d);
    return fault ? fault : db2010_reg_execute(opaque, &d, a);
}
static void dbdsp_rx_word(DB2010DspPeer *p, const DB2010DspEvent *e)
{
    DB2010FaLink *s = e->link;
    uint32_t *ch = s->dma->state.channel[2];
    if (ch[0] != 0xfa040020 || ch[2] || ch[4] != 0xf0c7 ||
        ch[3] != (0xa8492000 | ((136 - p->state.rx_bytes) / 4)) ||
        !s->memory.write(s->memory.opaque, ch[1], p->state.response + p->state.rx_bytes, 4)) {
        db2010_link_stop(&s->faults, "dsp_peer_rx_dma_progress_unmodeled", ch[1], 4); return;
    }
    ch[1] += 4; ch[3]--; p->state.rx_bytes += 4; p->state.due = e->now + 64;
    if (p->state.rx_bytes == 136) {
        dbdma_complete(s->dma, 2); p->state.phase = DBDSP_RX_COMPLETE;
        p->state.due = e->now + p->config.latency;
    }
}
