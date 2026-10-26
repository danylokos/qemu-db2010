/* Virtual-time PCM packet assembly; transport queues are separate.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_AUDIO_SAMPLES_H
#define DB2010_AUDIO_SAMPLES_H
#define DB2010_AUDIO_PACKET_FRAMES 960

typedef struct DB2010AudioSamples {
    int16_t pcm[DB2010_AUDIO_PACKET_FRAMES * 2];
    unsigned frames, emitted;
    uint64_t first_frame, start_ns, previous_end_ns, packets;
    bool discontinuity, ended;
} DB2010AudioSamples;

typedef struct DB2010AudioSink {
    void *opaque;
    void (*packet)(void *opaque, const int16_t *pcm, unsigned frames,
                   uint64_t sequence, uint64_t first_frame, uint64_t start_ns,
                   bool discontinuity, bool eos);
} DB2010AudioSink;
struct DB2010AudioRuntime;
int64_t db2010_audio_sample_deadline(struct DB2010AudioRuntime *a);
void db2010_audio_sample_advance(struct DB2010AudioRuntime *a,
                                DB2010AudioSink *sink, uint64_t now);
void db2010_audio_sample_end(struct DB2010AudioRuntime *a, DB2010AudioSink *sink);
unsigned db2010_audio_sample_stop(struct DB2010AudioRuntime *a,
                                 DB2010AudioSink *sink, uint64_t now);
unsigned db2010_audio_sample_pause(struct DB2010AudioRuntime *a,
                                  DB2010AudioSink *sink, uint64_t now);
#endif
