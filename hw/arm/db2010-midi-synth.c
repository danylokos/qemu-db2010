/* 40 global voices, 16 MIDI channels; all programs (including channel 10)
 * use the same pitched triangle. FIFO note release, then deterministic voice
 * stealing: first free, oldest releasing, oldest held. Integer arithmetic
 * keeps phase/envelope/PCM identical across checkpoint and packet boundaries.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-midi-synth.h"

/* round(440 * 2^((note - 69)/12) * 2^32 / 48000), A4 = 440 Hz. */
static const uint32_t increments[128] = {
    731558u, 775059u, 821146u, 869974u, 921705u, 976513u, 1034579u, 1096099u,
    1161276u, 1230329u, 1303488u, 1380998u, 1463116u, 1550118u, 1642292u, 1739948u,
    1843411u, 1953026u, 2069159u, 2192197u, 2322552u, 2460658u, 2606977u, 2761996u,
    2926232u, 3100235u, 3284585u, 3479896u, 3686822u, 3906052u, 4138318u, 4384395u,
    4645104u, 4921317u, 5213953u, 5523991u, 5852465u, 6200470u, 6569170u, 6959793u,
    7373644u, 7812103u, 8276635u, 8768789u, 9290209u, 9842633u, 10427907u, 11047982u,
    11704930u, 12400941u, 13138339u, 13919586u, 14747287u, 15624207u, 16553270u, 17537579u,
    18580418u, 19685267u, 20855814u, 22095965u, 23409859u, 24801882u, 26276679u, 27839171u,
    29494575u, 31248413u, 33106541u, 35075158u, 37160835u, 39370534u, 41711627u, 44191930u,
    46819719u, 49603764u, 52553357u, 55678342u, 58989149u, 62496826u, 66213081u, 70150316u,
    74321671u, 78741067u, 83423255u, 88383859u, 93639437u, 99207528u, 105106715u, 111356685u,
    117978298u, 124993653u, 132426162u, 140300631u, 148643341u, 157482134u, 166846509u, 176767719u,
    187278874u, 198415056u, 210213429u, 222713370u, 235956596u, 249987305u, 264852324u, 280601263u,
    297286682u, 314964268u, 333693018u, 353535438u, 374557749u, 396830112u, 420426858u, 445426740u,
    471913192u, 499974611u, 529704648u, 561202526u, 594573365u, 629928537u, 667386037u, 707070876u,
    749115498u, 793660223u, 840853716u, 890853480u, 943826385u, 999949222u, 1059409297u, 1122405052u,
};

/* Q24 ratios across +/-2 semitones; 64 bend units between entries. Pitch
 * bend range is a declared host default; RPN range changes remain guarded. */
static const uint32_t bend_ratios[257] = {
    14946800u, 14960296u, 14973805u, 14987325u, 15000858u, 15014403u, 15027960u, 15041529u,
    15055111u, 15068705u, 15082311u, 15095929u, 15109560u, 15123203u, 15136859u, 15150526u,
    15164206u, 15177899u, 15191604u, 15205321u, 15219050u, 15232792u, 15246547u, 15260313u,
    15274093u, 15287884u, 15301688u, 15315505u, 15329334u, 15343175u, 15357029u, 15370896u,
    15384775u, 15398666u, 15412571u, 15426487u, 15440416u, 15454358u, 15468313u, 15482280u,
    15496259u, 15510252u, 15524256u, 15538274u, 15552304u, 15566347u, 15580402u, 15594471u,
    15608552u, 15622645u, 15636752u, 15650871u, 15665003u, 15679147u, 15693305u, 15707475u,
    15721658u, 15735853u, 15750062u, 15764283u, 15778518u, 15792765u, 15807025u, 15821297u,
    15835583u, 15849882u, 15864193u, 15878518u, 15892855u, 15907206u, 15921569u, 15935945u,
    15950334u, 15964737u, 15979152u, 15993580u, 16008021u, 16022476u, 16036943u, 16051424u,
    16065917u, 16080424u, 16094943u, 16109476u, 16124022u, 16138581u, 16153153u, 16167739u,
    16182337u, 16196949u, 16211574u, 16226212u, 16240863u, 16255528u, 16270206u, 16284897u,
    16299601u, 16314319u, 16329050u, 16343794u, 16358551u, 16373322u, 16388106u, 16402904u,
    16417715u, 16432539u, 16447377u, 16462228u, 16477092u, 16491970u, 16506861u, 16521766u,
    16536684u, 16551616u, 16566561u, 16581520u, 16596492u, 16611478u, 16626477u, 16641490u,
    16656516u, 16671556u, 16686609u, 16701676u, 16716757u, 16731851u, 16746959u, 16762081u,
    16777216u, 16792365u, 16807527u, 16822704u, 16837894u, 16853097u, 16868315u, 16883546u,
    16898791u, 16914049u, 16929322u, 16944608u, 16959908u, 16975222u, 16990549u, 17005891u,
    17021246u, 17036615u, 17051999u, 17067396u, 17082806u, 17098231u, 17113670u, 17129123u,
    17144589u, 17160070u, 17175564u, 17191073u, 17206595u, 17222132u, 17237683u, 17253247u,
    17268826u, 17284419u, 17300026u, 17315646u, 17331282u, 17346931u, 17362594u, 17378271u,
    17393963u, 17409669u, 17425389u, 17441123u, 17456871u, 17472634u, 17488410u, 17504202u,
    17520007u, 17535826u, 17551660u, 17567508u, 17583371u, 17599248u, 17615139u, 17631044u,
    17646964u, 17662898u, 17678847u, 17694810u, 17710787u, 17726779u, 17742785u, 17758806u,
    17774841u, 17790891u, 17806955u, 17823034u, 17839127u, 17855235u, 17871357u, 17887494u,
    17903645u, 17919811u, 17935992u, 17952187u, 17968397u, 17984621u, 18000860u, 18017114u,
    18033382u, 18049665u, 18065963u, 18082276u, 18098603u, 18114945u, 18131302u, 18147673u,
    18164060u, 18180461u, 18196877u, 18213307u, 18229753u, 18246213u, 18262689u, 18279179u,
    18295684u, 18312204u, 18328739u, 18345288u, 18361853u, 18378433u, 18395028u, 18411637u,
    18428262u, 18444902u, 18461556u, 18478226u, 18494911u, 18511611u, 18528325u, 18545056u,
    18561801u, 18578561u, 18595336u, 18612127u, 18628932u, 18645753u, 18662589u, 18679441u,
    18696307u, 18713189u, 18730086u, 18746998u, 18763925u, 18780868u, 18797826u, 18814800u,
    18831788u,
};

void db2010_midi_synth_init(DB2010MidiSynth *s)
{
    *s = (DB2010MidiSynth){0};
    for (unsigned i = 0; i < 16; i++) { s->volume[i] = 127; s->bend[i] = 8192; }
}

static uint32_t pitch(DB2010MidiSynth *s, unsigned channel, unsigned note)
{
    unsigned bend = s->bend[channel], index = bend / 64, part = bend % 64;
    uint64_t ratio = bend_ratios[index] +
        (uint64_t)(bend_ratios[index + 1] - bend_ratios[index]) * part / 64;
    return ((uint64_t)increments[note] * ratio + (1u << 23)) >> 24;
}

const char *db2010_midi_event(DB2010MidiSynth *s, unsigned status,
                             unsigned data1, unsigned data2)
{
    if (status < 0x80 || status > 0xef || data1 > 127 || data2 > 127) {
        return "dsp_peer_midi_event_unmodeled";
    }
    unsigned channel = status & 15, kind = status >> 4;
    if (kind == 12) { s->program[channel] = data1; return NULL; }
    if (kind == 11 && data1 == 7) { s->volume[channel] = data2; return NULL; }
    if (kind == 14) {
        s->bend[channel] = data1 | (data2 << 7);
        for (unsigned i = 0; i < DB2010_MIDI_VOICES; i++) {
            DB2010MidiVoice *v = &s->voices[i];
            if (v->velocity && v->channel == channel) { v->step = pitch(s, channel, v->note); }
        }
        return NULL;
    }
    if (kind != 8 && kind != 9) { return "dsp_peer_midi_event_unmodeled"; }
    if (kind == 8 || !data2) {
        DB2010MidiVoice *oldest = NULL;
        for (unsigned i = 0; i < DB2010_MIDI_VOICES; i++) {
            DB2010MidiVoice *v = &s->voices[i];
            if (v->held && v->channel == channel && v->note == data1 &&
                (!oldest || v->age < oldest->age)) { oldest = v; }
        }
        if (oldest) { oldest->held = false; oldest->release = DB2010_MIDI_RELEASE; }
        return NULL;
    }
    DB2010MidiVoice *slot = NULL;
    for (unsigned i = 0; i < DB2010_MIDI_VOICES; i++) {
        DB2010MidiVoice *v = &s->voices[i];
        if (!v->velocity) { slot = v; break; }
        if (!slot || (!v->held && slot->held) ||
            (v->held == slot->held && v->age < slot->age)) { slot = v; }
    }
    s->stolen += slot->velocity != 0;
    *slot = (DB2010MidiVoice){.step = pitch(s, channel, data1), .age = ++s->notes,
        .channel = channel, .note = data1, .velocity = data2, .held = true};
    return NULL;
}

void db2010_midi_render(DB2010MidiSynth *s, int16_t *pcm, unsigned frames)
{
    for (unsigned f = 0; f < frames; f++) {
        int32_t sum = 0;
        for (unsigned i = 0; i < DB2010_MIDI_VOICES; i++) {
            DB2010MidiVoice *v = &s->voices[i];
            if (!v->velocity) { continue; }
            /* Starts at zero and rises; signed amplitude [-32768,32767]. */
            uint32_t phase = v->phase + UINT32_C(0x40000000);
            uint32_t ramp = phase >> 15;
            int32_t value = (ramp < 65536 ? ramp : 131071 - ramp) - 32768;
            value = value * v->velocity / 127;
            value = value * s->volume[v->channel] / 127;
            if (!v->held) { value = value * v->release / DB2010_MIDI_RELEASE; }
            /* Fixed 40-voice headroom: no note-count-dependent pumping. */
            sum += value / DB2010_MIDI_VOICES;
            v->phase += v->step;
            if (!v->held && !--v->release) { v->velocity = 0; }
        }
        pcm[f * 2] = pcm[f * 2 + 1] = sum;
    }
}
