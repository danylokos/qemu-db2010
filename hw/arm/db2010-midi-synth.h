/* Deterministic host triangle substitute, not the original DSP synthesizer.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_MIDI_SYNTH_H
#define DB2010_MIDI_SYNTH_H
#define DB2010_MIDI_VOICES 40
#define DB2010_MIDI_RELEASE 3072 /* 64 ms at 48 kHz, an explicit model policy */
typedef struct DB2010MidiVoice {
    uint32_t phase, step;
    uint64_t age;
    uint16_t release;
    uint8_t channel, note, velocity;
    bool held;
} DB2010MidiVoice;
typedef struct DB2010MidiSynth {
    DB2010MidiVoice voices[DB2010_MIDI_VOICES];
    uint8_t program[16];
    uint8_t volume[16];
    uint16_t bend[16];
    uint64_t notes, stolen;
} DB2010MidiSynth;
void db2010_midi_synth_init(DB2010MidiSynth *s);
const char *db2010_midi_event(DB2010MidiSynth *s, unsigned status,
                             unsigned data1, unsigned data2);
void db2010_midi_render(DB2010MidiSynth *s, int16_t *pcm, unsigned frames);
#endif
