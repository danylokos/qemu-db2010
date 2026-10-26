/* Sample deadlines cannot depend on whether an output sink is present.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-audio-codec.h"

static void packet(DB2010AudioRuntime *a, DB2010AudioSink *sink, bool eos)
{
    DB2010AudioSamples *s = &a->samples;
    if (sink->packet) {
        sink->packet(sink->opaque, s->pcm, s->frames, s->packets,
                     s->first_frame, s->start_ns, s->discontinuity, eos);
    }
    s->packets++;
    s->frames = 0;
    s->discontinuity = false;
}
int64_t db2010_audio_sample_deadline(DB2010AudioRuntime *a)
{
    if (!a->active || !a->pcm_frames) { return -1; }
    unsigned next = MIN(a->pcm_frames, a->samples.emitted +
                        DB2010_AUDIO_PACKET_FRAMES - a->samples.frames);
    return a->due - db2010_audio_duration(a->pcm_frames) + db2010_audio_duration(next);
}
void db2010_audio_sample_advance(DB2010AudioRuntime *a, DB2010AudioSink *sink,
                                uint64_t now)
{
    DB2010AudioSamples *s = &a->samples;
    if (!a->active || !a->pcm_frames) { return; }
    uint64_t start = a->due - db2010_audio_duration(a->pcm_frames);
    if (!s->emitted) {
        if (!s->previous_end_ns || start > s->previous_end_ns + db2010_audio_duration(1)) {
            /* A delayed source is a gap, never invented native silence. */
            if (s->frames) { packet(a, sink, false); }
            s->discontinuity = true;
        }
        s->previous_end_ns = start + db2010_audio_duration(a->pcm_frames);
    }
    while (db2010_audio_sample_deadline(a) <= now && s->emitted < a->pcm_frames) {
        unsigned count = MIN(a->pcm_frames - s->emitted,
                             DB2010_AUDIO_PACKET_FRAMES - s->frames);
        if (!s->frames) {
            s->first_frame = a->frames_played + s->emitted;
            s->start_ns = start + db2010_audio_duration(s->emitted);
        }
        memcpy(s->pcm + s->frames * 2, a->pcm + s->emitted * 2,
               count * 2 * sizeof(int16_t));
        s->frames += count; s->emitted += count;
        if (s->frames == DB2010_AUDIO_PACKET_FRAMES) { packet(a, sink, false); }
    }
}
void db2010_audio_sample_end(DB2010AudioRuntime *a, DB2010AudioSink *sink)
{
    DB2010AudioSamples *s = &a->samples;
    if (s->ended) { return; }
    /* An exactly packet-aligned stream needs an explicit zero-frame EOS. */
    if (!s->frames) {
        s->first_frame = a->frames_played;
        s->start_ns = s->previous_end_ns;
    }
    packet(a, sink, true);
    s->ended = true;
}

unsigned db2010_audio_sample_stop(DB2010AudioRuntime *a, DB2010AudioSink *sink,
                                  uint64_t now)
{
    db2010_audio_sample_advance(a, sink, now);
    DB2010AudioSamples *s = &a->samples;
    if (!a->pcm_frames) { return 0; }
    uint64_t start = a->due - db2010_audio_duration(a->pcm_frames);
    unsigned elapsed = now >= a->due ? a->pcm_frames : now > start ?
        MIN(a->pcm_frames, (now - start) * 48000 / UINT64_C(1000000000)) : 0;
    elapsed = MAX(elapsed, s->emitted);
    while (s->emitted < elapsed) {
        unsigned count = MIN(elapsed - s->emitted,
                             DB2010_AUDIO_PACKET_FRAMES - s->frames);
        if (!s->frames) {
            s->first_frame = a->frames_played + s->emitted;
            s->start_ns = start + db2010_audio_duration(s->emitted);
        }
        memcpy(s->pcm + s->frames * 2, a->pcm + s->emitted * 2,
               count * 2 * sizeof(int16_t));
        s->frames += count; s->emitted += count;
        if (s->frames == DB2010_AUDIO_PACKET_FRAMES) { packet(a, sink, false); }
    }
    s->previous_end_ns = start + db2010_audio_duration(elapsed);
    return elapsed;
}

unsigned db2010_audio_sample_pause(DB2010AudioRuntime *a, DB2010AudioSink *sink,
                                   uint64_t now)
{
    unsigned elapsed = db2010_audio_sample_stop(a, sink, now);
    /* Deliver the elapsed prefix; pause does not terminate the source. */
    if (a->samples.frames) { packet(a, sink, false); }
    return elapsed;
}
