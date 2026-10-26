/* Guarded synthetic FA transport peer, optionally computing positive1024-bit
 * modular exponentiation through native RX mailbox/DMA/IRQ, plus observed
 * silent audio-control acknowledgements and zeroed unused-slot query mocks.
 * Optional scoped host startup processing; no DSP ISA execution. Timing and
 * reset state are hypotheses.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_DB2010_DSP_PEER_H
#define HW_ARM_DB2010_DSP_PEER_H

#include "db2010-bus.h"
#include "db2010-audio-codec.h"
#include "db2010-audio-output.h"
#include "db2010-midi-stream.h"
const DB2010RegMap *dbdsp_map(void);
const DB2010RegMap *dbdsp_rx_map(void);
const char *db2010_peer_rx_access(void *opaque, DB2010Access *a);
const char *dsp_peer_rx_arm_fault(void *opaque);
typedef struct QDict QDict;
typedef struct DB2010DspPeerConfig {
    unsigned latency;
    unsigned wire10;
    bool variable_dma;
    bool modexp;
    bool mma_mode;
    bool mma_ring;
    bool mma_setup;
    bool mma_buffer;
    bool mma_playback;
    bool mma_drain;
    bool mma_stream;
    bool midi_setup;
    bool midi_profile;
} DB2010DspPeerConfig;
typedef enum DB2010DspPhase {
    DBDSP_IDLE = 0, DBDSP_MAILBOX_PENDING = 1, DBDSP_DMA = 2,
    DBDSP_TRANSFER_PENDING = 3, DBDSP_DONE = 4,
    DBDSP_RX_OFFER = 5, DBDSP_RX_WAIT = 6, DBDSP_RX_DMA = 7,
    DBDSP_RX_COMPLETE = 8,
    DBDSP_MMA_MODE_OFFER = 9,
    DBDSP_MIDI_PREPARE_OFFER = 10,
} DB2010DspPhase;
typedef struct DB2010DspPeerRuntime {
    uint32_t mask;
    uint32_t status;
    uint32_t source;
    DB2010DspPhase phase;
    unsigned bytes;
    bool mailbox_only;
    unsigned length;
    unsigned logical;
    uint64_t dma_messages;
    uint64_t total_bytes;
    uint64_t message_requests;
    uint64_t due;
    uint64_t requests;
    uint64_t mailbox_events;
    uint64_t transfer_events;
    uint64_t status_reads;
    uint8_t payload[4094 * 4];
    uint16_t rx_mailbox[32];
    bool rx_ready;
    uint32_t selector, rx_address, rx_count;
    unsigned rx_bytes, replies;
    uint8_t response[136];
    bool rx_inline;
    uint32_t audio_opcode;
    uint64_t audio_requests, audio_replies;
    uint16_t audio_iir_active[5];
    bool rx_mma;
    uint16_t mma_mode;
    uint64_t mma_requests, mma_replies;
    bool rx_ring4, ring4_announced, ring4_registered;
    uint16_t ring4_threshold;
    uint64_t ring4_announcements, ring4_registrations;
    bool mma_configured;
    uint16_t ring4_buffer[800];
    unsigned ring4_buffered_words;
    uint64_t ring4_packets;
    DB2010AudioRuntime audio;
    bool midi_initialized, midi_prepared, midi_ring_announced;
    bool midi_ring_registered, midi_rx_ring;
    uint16_t midi_ring_threshold;
    uint64_t midi_requests, midi_replies;
    DB2010MidiStream midi;
} DB2010DspPeerRuntime;
typedef struct DB2010DspPeer {
    bool omit_diagnostics;
    QList *encoded_frames;
    uint64_t encoded_frame_count;
    DB2010AudioSink sink;
    DB2010AudioOutput *output;
    DB2010DspPeerConfig config;
    DB2010DspPeerRuntime state;
} DB2010DspPeer;

bool dbdsp_contains(uint32_t address, unsigned size);

const char *dbdsp_access(DB2010DspPeer *p, uint32_t address,
                                 unsigned size, bool write, uint32_t *value,
                                 bool apply);

QDict *dbdsp_snapshot(DB2010DspPeer *p);

void dbdsp_reset(DB2010DspPeer *d);
void dbdsp_request(DB2010DspPeer *p, bool mailbox_only, unsigned length,
                   unsigned logical, uint64_t now);
int64_t dbdsp_deadline(DB2010DspPeer *p);
#endif
