/* This consumes guest-produced events, never SMF bytes. Timing uses 48kHz
 * units observed in the native serializer. An all-zero short record is the
 * original ARM staging/end sentinel. Drain scheduled events, then a 64ms host
 * release tail; report the last source time separately from this PCM tail.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-midi-stream.h"

void db2010_midi_stream_init(DB2010MidiStream *m)
{
    *m = (DB2010MidiStream){.pcm_hash = UINT64_C(14695981039346656037)};
    db2010_midi_synth_init(&m->synth);
}
const char *db2010_midi_stream_check_append(const DB2010MidiStream *m,
                                           const uint16_t *words, unsigned count)
{
    if (!count || count > DB2010_MIDI_RING_WORDS - m->used) {
        return "dsp_peer_midi_ring_overflow";
    }
    if (m->eof) {
        if (count % 5) { return "dsp_peer_midi_data_after_end_unmodeled"; }
        for (unsigned i = 0; i < count; i++) {
            if (words[i]) { return "dsp_peer_midi_data_after_end_unmodeled"; }
        }
    }
    return NULL;
}
const char *db2010_midi_stream_append(DB2010MidiStream *m, const uint16_t *words,
                                      unsigned count)
{
    const char *fault = db2010_midi_stream_check_append(m, words, count);
    if (fault) { return fault; }
    if (m->eof && !m->active) {
        m->credit += count; m->words_received += count;
        return NULL;
    }
    memcpy(m->ring + m->used, words, count * sizeof(*words));
    m->used += count; m->words_received += count;
    return NULL;
}
static void packet(DB2010MidiStream *m, DB2010AudioSink *sink, bool eos)
{
    DB2010AudioSamples *s = &m->samples;
    if (sink->packet) {
        sink->packet(sink->opaque, s->pcm, s->frames, s->packets,
                     s->first_frame, s->start_ns, s->discontinuity, eos);
    }
    s->frames = 0; s->packets++; s->discontinuity = false;
    s->ended = eos;
}
void db2010_midi_stream_run(DB2010MidiStream *m, uint64_t now)
{
    m->origin_ns = now; m->origin_frame = m->frames;
    m->active = true; m->paused = false;
    m->samples.discontinuity = true;
}
void db2010_midi_stream_pause(DB2010MidiStream *m, DB2010AudioSink *sink)
{
    if (m->samples.frames) { packet(m, sink, false); }
    m->active = false; m->paused = true;
}
void db2010_midi_stream_stop(DB2010MidiStream *m, DB2010AudioSink *sink)
{
    m->active = m->paused = false;
    m->completion_requested = false;
    memset(&m->synth.voices, 0, sizeof(m->synth.voices));
    if (!m->samples.frames) {
        m->samples.first_frame = m->frames;
        m->samples.start_ns = m->origin_ns + (m->frames - m->origin_frame) * UINT64_C(1000000000) / 48000;
    }
    if (!m->samples.ended) { packet(m, sink, true); }
}
int64_t db2010_midi_stream_deadline(DB2010MidiStream *m)
{
    if (!m->active) { return -1; }
    uint64_t next = m->frames + DB2010_AUDIO_PACKET_FRAMES - m->samples.frames;
    if (m->eof) { next = MIN(next, m->end_frame); }
    return m->origin_ns + ((next - m->origin_frame) * UINT64_C(1000000000) + 47999) / 48000;
}
const char *db2010_midi_stream_advance(DB2010MidiStream *m, DB2010AudioSink *sink,
                                       uint64_t now)
{
    if (!m->active) { return NULL; }
    uint64_t target = m->origin_frame + (now - m->origin_ns) * 48000 / UINT64_C(1000000000);
    while (m->active && m->frames <= target) {
        uint64_t until = target;
        if (m->used) {
            if (m->used < 5) { return "dsp_peer_midi_partial_record_unmodeled"; }
            uint16_t *b = m->ring;
            uint64_t time = b[3] | (uint32_t)b[4] << 16;
            bool end = !(b[0] | b[1] | b[2] | b[3] | b[4]);
            if (m->eof && !end) { return "dsp_peer_midi_data_after_end_unmodeled"; }
            if (end || time <= m->frames) {
                if (!end && time < m->last_time) { return "dsp_peer_midi_time_order_unmodeled"; }
                if (!b[0] && !end) { return "dsp_peer_midi_long_event_unmodeled"; }
                const char *fault = end ? NULL : db2010_midi_event(&m->synth, b[0], b[1], b[2]);
                if (fault) { return fault; }
                if (end) {
                    /* Original4488468C flushes the zero sentinel on every
                     * pending refill after the parser reaches EOT. Native
                     * gameplay exposes duplicate sentinels behind the first.
                     * They return ownership once, without restarting tails. */
                    if (m->used % 5) { return "dsp_peer_midi_data_after_end_unmodeled"; }
                    for (unsigned i = 5; i < m->used; i++) {
                        if (b[i]) { return "dsp_peer_midi_data_after_end_unmodeled"; }
                    }
                    if (!m->eof) {
                        m->eof = true;
                        m->end_frame = m->frames + DB2010_MIDI_RELEASE;
                        for (unsigned i = 0; i < DB2010_MIDI_VOICES; i++) {
                            DB2010MidiVoice *v = &m->synth.voices[i];
                            if (v->held) { v->held = false; v->release = DB2010_MIDI_RELEASE; }
                        }
                    }
                } else { m->last_time = time; m->events++; }
                unsigned before = m->used;
                m->used -= 5; m->credit += 5;
                m->credit_notify |= before > 750 && m->used <= 750;
                memmove(b, b + 5, m->used * sizeof(*b));
                memset(b + m->used, 0, 5 * sizeof(*b));
                continue;
            }
            until = MIN(until, time);
        }
        if (m->eof) { until = MIN(until, m->end_frame); }
        if (until > m->frames) {
            DB2010AudioSamples *s = &m->samples;
            unsigned n = MIN(until - m->frames, DB2010_AUDIO_PACKET_FRAMES - s->frames);
            if (!s->frames) {
                s->first_frame = m->frames;
                s->start_ns = m->origin_ns + (m->frames - m->origin_frame) * UINT64_C(1000000000) / 48000;
            }
            int16_t *pcm = s->pcm + s->frames * 2;
            db2010_midi_render(&m->synth, pcm, n);
            for (unsigned i = 0; i < n * 2; i++) {
                uint16_t value = pcm[i];
                m->nonzero_samples += value != 0;
                m->pcm_hash = (m->pcm_hash ^ (value & 255)) * UINT64_C(1099511628211);
                m->pcm_hash = (m->pcm_hash ^ (value >> 8)) * UINT64_C(1099511628211);
            }
            s->frames += n; m->frames += n;
            if (s->frames == DB2010_AUDIO_PACKET_FRAMES) { packet(m, sink, false); }
        } else if (m->eof && m->frames == m->end_frame) {
            m->active = false; m->completed = true;
            if (!m->samples.frames) {
                m->samples.first_frame = m->frames;
                m->samples.start_ns = m->origin_ns + (m->frames - m->origin_frame) * UINT64_C(1000000000) / 48000;
            }
            packet(m, sink, true);
        } else { break; }
    }
    return NULL;
}
