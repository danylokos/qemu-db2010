/* Bounded timed MIDI service; host synthesis policies are explicit.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_MIDI_STREAM_H
#define DB2010_MIDI_STREAM_H
#include "db2010-midi-synth.h"
#include "db2010-audio-samples.h"
#define DB2010_MIDI_RING_WORDS 1000
typedef struct DB2010MidiStream {
    DB2010MidiSynth synth;
    DB2010AudioSamples samples;
    uint16_t ring[DB2010_MIDI_RING_WORDS];
    unsigned used, credit, rx_kind;
    bool active, paused, eof, completed, completion_requested, completion_sent;
    bool credit_notify, flush_pending;
    uint64_t frames, origin_ns, origin_frame, last_time, end_frame;
    uint64_t pcm_hash, nonzero_samples, events, words_received, words_credited;
} DB2010MidiStream;
void db2010_midi_stream_init(DB2010MidiStream *m);
const char *db2010_midi_stream_check_append(const DB2010MidiStream *m,
                                           const uint16_t *words, unsigned count);
const char *db2010_midi_stream_append(DB2010MidiStream *m, const uint16_t *words,
                                      unsigned count);
const char *db2010_midi_stream_advance(DB2010MidiStream *m, DB2010AudioSink *sink,
                                       uint64_t now);
int64_t db2010_midi_stream_deadline(DB2010MidiStream *m);
void db2010_midi_stream_run(DB2010MidiStream *m, uint64_t now);
void db2010_midi_stream_pause(DB2010MidiStream *m, DB2010AudioSink *sink);
void db2010_midi_stream_stop(DB2010MidiStream *m, DB2010AudioSink *sink);
#endif
