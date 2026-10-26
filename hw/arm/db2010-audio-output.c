/* No write_all, guest callbacks, native state or unbounded presentation queues.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "system/runstate.h"
#include "exec/icount.h"
#include "db2010-audio-output.h"

typedef struct AudioPacket {
    unsigned length, offset, frames;
    uint8_t bytes[4096 + 4 + 3840];
} AudioPacket;
struct DB2010AudioOutput {
    CharFrontend chr;
    GQueue queue;
    guint watch;
    char *stream;
    uint64_t epoch, dropped_frames, sent_packets;
    bool discontinuity;
    VMChangeStateEntry *vmstate;
    int64_t next_host_us;
    uint64_t paced_packets, waited_us;
};
static void clear(DB2010AudioOutput *o)
{
    if (o->watch) { g_source_remove(o->watch); o->watch = 0; }
    for (GList *i = o->queue.head; i; i = i->next) {
        o->dropped_frames += ((AudioPacket *)i->data)->frames;
    }
    g_queue_clear_full(&o->queue, g_free);
    o->epoch++; o->discontinuity = true;
}
void db2010_audio_output_reset(DB2010AudioOutput *o)
{
    if (!o) { return; }
    bool had_data = o->sent_packets || !g_queue_is_empty(&o->queue);
    clear(o);
    o->next_host_us = 0;
    /* Never splice a partially written packet into the next epoch. */
    if (had_data) { qemu_chr_fe_disconnect(&o->chr); }
}
void db2010_audio_output_new_stream(DB2010AudioOutput *o)
{
    if (!o) { return; }
    AudioPacket *head = g_queue_peek_head(&o->queue);
    bool partial = head && head->offset;
    clear(o);
    o->next_host_us = 0;
    /* Complete envelopes can change epochs in band. Only a partial envelope
     * requires reconnecting, otherwise the reconnect gap eats the next intro. */
    if (partial) { qemu_chr_fe_disconnect(&o->chr); }
}
static void flush(DB2010AudioOutput *o);
static gboolean writable(void *unused, GIOCondition condition, void *opaque)
{
    DB2010AudioOutput *o = opaque;
    o->watch = 0;
    if (condition & (G_IO_HUP | G_IO_ERR)) { db2010_audio_output_reset(o); }
    else { flush(o); }
    return G_SOURCE_REMOVE;
}
static void flush(DB2010AudioOutput *o)
{
    while (!g_queue_is_empty(&o->queue)) {
        AudioPacket *p = g_queue_peek_head(&o->queue);
        int n = qemu_chr_fe_write(&o->chr, p->bytes + p->offset, p->length - p->offset);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            db2010_audio_output_reset(o); return;
        }
        if (n <= 0) { break; }
        p->offset += n;
        if (p->offset == p->length) { o->sent_packets++; g_free(g_queue_pop_head(&o->queue)); }
    }
    if (!g_queue_is_empty(&o->queue) && !o->watch) {
        o->watch = qemu_chr_fe_add_watch(&o->chr, G_IO_OUT | G_IO_HUP | G_IO_ERR, writable, o);
        if (!o->watch) { db2010_audio_output_reset(o); }
    }
}
static void output_packet(void *opaque, const int16_t *pcm, unsigned frames,
                          uint64_t sequence, uint64_t first, uint64_t start,
                          bool discontinuity, bool eos)
{
    DB2010AudioOutput *o = opaque;
    /* Aligned icount may repay seconds of old CPU lag through idle warps.
     * Audio must still run at its sample rate. Bound host production to1x,
     * independently of backend presence/readiness; never wait for a consumer.
     * Late production rebases this presentation-only deadline so no catch-up
     * debt is carried into the next block. Guest clocks/timestamps are intact.
     * One wait is at most the preceding packet's20ms. Unpaced research stays
     * unpaced, and this disposable host deadline never enters a checkpoint. */
    if (icount_align_enabled()) {
        int64_t now = g_get_monotonic_time();
        if (!sequence) { o->next_host_us = 0; }
        if (o->next_host_us > now) {
            o->waited_us += o->next_host_us - now;
            g_usleep(o->next_host_us - now);
        }
        o->next_host_us = g_get_monotonic_time() +
            ((uint64_t)frames * 1000000 + 47999) / 48000;
        o->paced_packets++;
    }
    if (!qemu_chr_fe_backend_open(&o->chr)) { return; }
    if (g_queue_get_length(&o->queue) >= 10) {
        AudioPacket *head = g_queue_peek_head(&o->queue);
        if (head->offset) { db2010_audio_output_reset(o); return; }
        clear(o);
    }
    AudioPacket *p = g_new0(AudioPacket, 1);
    int n = snprintf((char *)p->bytes + 4, 4096,
        "{\"type\":\"audio\",\"version\":1,\"stream_id\":\"%s\",\"epoch\":\"%"PRIu64"\","
        "\"sequence\":\"%"PRIu64"\",\"first_frame\":\"%"PRIu64"\",\"virtual_start_ns\":\"%"PRIu64"\","
        "\"sample_rate\":48000,\"channels\":2,\"format\":\"s16le\",\"frame_count\":%u,"
        "\"discontinuity\":%s,\"eos\":%s}", o->stream, o->epoch, sequence, first, start,
        frames, discontinuity || o->discontinuity ? "true" : "false", eos ? "true" : "false");
    g_assert(n > 0 && n < 4096 && frames <= 960);
    stl_be_p(p->bytes, n);
    for (unsigned i = 0; i < frames * 2; i++) { stw_le_p(p->bytes + 4 + n + i * 2, pcm[i]); }
    p->length = 4 + n + frames * 4; p->frames = frames;
    o->discontinuity = false;
    g_queue_push_tail(&o->queue, p);
    flush(o);
}
static void event(void *opaque, QEMUChrEvent event)
{
    if (event == CHR_EVENT_OPENED || event == CHR_EVENT_CLOSED) { clear(opaque); }
}
static void runstate(void *opaque, bool running, RunState state)
{
    if (!running) { db2010_audio_output_reset(opaque); }
}
DB2010AudioOutput *db2010_audio_output_new(DB2010AudioSink *sink)
{
    DB2010AudioOutput *o = g_new0(DB2010AudioOutput, 1);
    o->stream = g_uuid_string_random();
    Chardev *chr = qemu_chr_find("audio");
    if (chr) {
        qemu_chr_fe_init(&o->chr, chr, &error_fatal);
        qemu_chr_fe_set_handlers(&o->chr, NULL, NULL, event, NULL, o, NULL, true);
    }
    o->vmstate = qemu_add_vm_change_state_handler(runstate, o);
    *sink = (DB2010AudioSink){o, output_packet};
    return o;
}
void db2010_audio_output_free(DB2010AudioOutput *o)
{
    if (!o) { return; }
    qemu_del_vm_change_state_handler(o->vmstate);
    clear(o); qemu_chr_fe_deinit(&o->chr, false);
    g_free(o->stream); g_free(o);
}

char *db2010_audio_output_status(DB2010AudioOutput *o)
{
    if (!o) { return g_strdup("{}"); }
    return g_strdup_printf("{\"queued\":%u,\"dropped_frames\":%"PRIu64",\"sent_packets\":%"PRIu64",\"epoch\":%"PRIu64",\"paced_packets\":%"PRIu64",\"waited_us\":%"PRIu64"}",
                           g_queue_get_length(&o->queue), o->dropped_frames, o->sent_packets, o->epoch,
                           o->paced_packets, o->waited_us);
}
