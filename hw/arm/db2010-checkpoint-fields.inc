/* Explicit field serializers; no structure padding or host pointers. */
static void cp_DB2010ClockPhase(Checkpoint *c, DB2010ClockPhase *v)
{
    CP_U64(v->ns);
    CP_U64(v->ticks);
    CP_U64(v->fraction);
}

static void cp_DB2010TimerRuntime(Checkpoint *c, DB2010TimerRuntime *v)
{
    cp_DB2010ClockPhase(c, &v->phase);
    CP_U32(v->control);
    CP_U32(v->load);
    CP_U32(v->counter);
    for (unsigned i0 = 0; i0 < 3; i0++) {
        CP_U32(v->masks[i0]);
    }
    CP_BOOL(v->load_known);
    for (unsigned i0 = 0; i0 < 3; i0++) {
        CP_BOOL(v->masks_known[i0]);
    }
    CP_BOOL(v->pending);
    CP_U32(v->selected);
    CP_U32(v->secondary_acked);
    CP_BOOL(v->i2c_pending);
    CP_BOOL(v->i2c_selected);
    CP_U64(v->i2c_irqs);
    CP_BOOL(v->pdi_pending);
    CP_BOOL(v->dma_pending);
    CP_BOOL(v->pdi_selected);
    CP_BOOL(v->dma_selected);
    CP_U64(v->pdi_irqs);
    CP_U64(v->dma_irqs);
    CP_BOOL(v->keypad_pending);
    CP_BOOL(v->keypad_selected);
    CP_U64(v->keypad_irqs);
    CP_BOOL(v->dsp_pending);
    CP_BOOL(v->dsp_selected);
    CP_U64(v->dsp_irqs);
    CP_BOOL(v->rtc_pending);
    CP_BOOL(v->rtc_selected);
    CP_BOOL(v->rtc_input_level);
    CP_U64(v->rtc_irqs);
    CP_BOOL(v->sim_rx_pending);
    CP_BOOL(v->sim_rx_selected);
    CP_BOOL(v->sim_tx_pending);
    CP_BOOL(v->sim_tx_selected);
    CP_BOOL(v->sim_tx_input_level);
    CP_U64(v->sim_rx_irqs);
    CP_U64(v->sim_tx_irqs);
    CP_U64(v->last_cycles);
    CP_U64(v->idle_cycles);
    CP_U64(v->irqs);
    CP_U32(v->paired_load); CP_U32(v->paired_counter);
    CP_BOOL(v->paired_load_known); CP_BOOL(v->paired_pending);
    CP_BOOL(v->paired_selected); CP_U64(v->paired_irqs);
    CP_BOOL(v->data_uart_pending); CP_BOOL(v->data_uart_selected); CP_U64(v->data_uart_irqs);
}

static void cp_DB2010RtcRuntime(Checkpoint *c, DB2010RtcRuntime *v)
{
    cp_DB2010ClockPhase(c, &v->phase);
    CP_U32(v->control);
    CP_BOOL(v->control_written);
    CP_U64(v->calendar_adjust_seconds);
    CP_U64(v->counter_adjust_ticks);
    CP_U16(v->minute_control);
    CP_U16(v->minute_status);
    CP_U64(v->minute_next_ticks);
    CP_U64(v->minute_events);
}

static void cp_DB2010RtcRetained(Checkpoint *c, DB2010RtcRetained *v)
{
    for (unsigned i0 = 0; i0 < 25; i0++) {
        CP_U8(v->bytes[i0]);
    }
}

static void cp_DB2010DmaRuntime(Checkpoint *c, DB2010DmaRuntime *v)
{
    for (unsigned i0 = 0; i0 < 6; i0++) {
        CP_U32(v->global[i0]);
    }
    for (unsigned i0 = 0; i0 < 8; i0++) {
        for (unsigned i1 = 0; i1 < 5; i1++) {
            CP_U32(v->channel[i0][i1]);
        }
    }
    CP_U32(v->raw_tc);
    CP_U32(v->raw_error);
}

static void cp_DB2010I2cRuntime(Checkpoint *c, DB2010I2cRuntime *v)
{
    CP_U32(v->peer_step);
    CP_U32(v->control);
    CP_U32(v->clock);
    CP_U32(v->phase);
    CP_U32(v->pending);
    CP_U32(v->mask);
    CP_BOOL(v->control_known);
    CP_BOOL(v->clock_known);
    CP_BOOL(v->irq_pending);
    CP_BOOL(v->status_known);
    CP_U32(v->async_init_step);
    CP_U32(v->async_phase);
    CP_U32(v->status);
    CP_U64(v->async_completions);
    CP_U32(v->staged);
    CP_U32(v->received);
    CP_U32(v->slave);
    CP_U32(v->reg);
    CP_U64(v->due);
    CP_U64(v->transactions);
    for (unsigned i0 = 0; i0 < 16; i0++) {
        CP_U8(v->wire[i0]);
    }
    CP_U32(v->wire_size);
}

static void cp_DB2010PdiRuntime(Checkpoint *c, DB2010PdiRuntime *v)
{
    for (unsigned i0 = 0; i0 < 26; i0++) {
        CP_U32(v->program[i0]);
    }
    for (unsigned i0 = 0; i0 < 6; i0++) {
        CP_U32(v->data[i0]);
    }
    CP_U32(v->mask);
    CP_U32(v->config);
    CP_U32(v->control);
    CP_U32(v->selector);
    CP_U32(v->event_mask);
    CP_U32(v->data_known);
    CP_U32(v->data_dirty);
    CP_BOOL(v->mask_known);
    CP_BOOL(v->mask_dirty);
    CP_U64(v->busy_until);
    CP_U64(v->transfers);
    CP_U64(v->busy_reads);
    CP_U32(v->dma_armed);
    CP_BOOL(v->dma_complete);
    CP_U64(v->stop_commands);
    CP_U32(v->dma_bytes);
    CP_U32(v->packet_size);
    for (unsigned i0 = 0; i0 < 24; i0++) {
        CP_U8(v->packet[i0]);
    }
    for (unsigned i0 = 0; i0 < 24; i0++) {
        CP_U8(v->rs[i0]);
    }
}

static void cp_DB2010Panel(Checkpoint *c, DB2010Panel *v)
{
    for (unsigned i0 = 0; i0 < 0x46; i0++) {
        CP_U16(v->regs[i0]);
    }
    CP_U16(v->index);
    CP_U16(v->cursor);
    CP_U16(v->started_cursor);
    for (unsigned i0 = 0; i0 < 0x46; i0++) {
        CP_BOOL(v->known[i0]);
    }
    CP_BOOL(v->cursor_known);
    CP_U32(v->index_known);
    for (unsigned i0 = 0; i0 < DBPANEL_PIXELS * 2; i0++) {
        CP_U8(v->pixels[i0]);
    }
    for (unsigned i0 = 0; i0 < DBPANEL_PIXELS; i0++) {
        CP_U8(v->pixel_known[i0]);
    }
    CP_U32(v->known_pixels);
}

static void cp_DB2010FaMailbox(Checkpoint *c, DB2010FaMailbox *v)
{
    for (unsigned i0 = 0; i0 < 32; i0++) {
        CP_U16(v->words[i0]);
    }
    CP_U32(v->known);
    CP_U32(v->busy);
    CP_U32(v->payload_words);
    CP_U64(v->submissions);
}

static void cp_DB2010DspPeerRuntime(Checkpoint *c, DB2010DspPeerRuntime *v)
{
    CP_U32(v->mask);
    CP_U32(v->status);
    CP_U32(v->source);
    CP_U32(v->phase);
    CP_U32(v->bytes);
    CP_BOOL(v->mailbox_only);
    CP_U32(v->length);
    CP_U32(v->logical);
    CP_U64(v->dma_messages);
    CP_U64(v->total_bytes);
    CP_U64(v->message_requests);
    CP_U64(v->due);
    CP_U64(v->requests);
    CP_U64(v->mailbox_events);
    CP_U64(v->transfer_events);
    CP_U64(v->status_reads);
    for (unsigned i0 = 0; i0 < 4094 * 4; i0++) {
        CP_U8(v->payload[i0]);
    }
    for (unsigned i0 = 0; i0 < 32; i0++) {
        CP_U16(v->rx_mailbox[i0]);
    }
    CP_BOOL(v->rx_ready);
    CP_U32(v->selector);
    CP_U32(v->rx_address);
    CP_U32(v->rx_count);
    CP_U32(v->rx_bytes);
    CP_U32(v->replies);
    for (unsigned i0 = 0; i0 < 136; i0++) {
        CP_U8(v->response[i0]);
    }
    CP_BOOL(v->rx_inline);
    CP_U32(v->audio_opcode);
    CP_U64(v->audio_requests);
    CP_U64(v->audio_replies);
    for (unsigned i0 = 0; i0 < 5; i0++) {
        CP_U16(v->audio_iir_active[i0]);
    }
    CP_BOOL(v->rx_mma);
    CP_U16(v->mma_mode);
    CP_U64(v->mma_requests);
    CP_U64(v->mma_replies);
    CP_BOOL(v->rx_ring4);
    CP_BOOL(v->ring4_announced);
    CP_BOOL(v->ring4_registered);
    CP_U16(v->ring4_threshold);
    CP_U64(v->ring4_announcements);
    CP_U64(v->ring4_registrations);
    CP_BOOL(v->mma_configured);
    for (unsigned i0 = 0; i0 < 800; i0++) {
        CP_U16(v->ring4_buffer[i0]);
    }
    CP_U32(v->ring4_buffered_words);
    CP_U64(v->ring4_packets);
    if (v->ring4_buffered_words > 800) { c->invalid = true; }
    DB2010AudioRuntime *a = &v->audio;
    DB2010AudioSamples *o = &a->samples;
    for (unsigned i = 0; i < 1920; i++) { CP_U16(o->pcm[i]); }
    CP_U32(o->frames); CP_U32(o->emitted);
    CP_U64(o->first_frame); CP_U64(o->start_ns); CP_U64(o->previous_end_ns); CP_U64(o->packets);
    CP_BOOL(o->discontinuity); CP_BOOL(o->ended);
    if (o->frames >= 960 || o->emitted > DB2010_AUDIO_MAX_PCM_FRAMES) { c->invalid = true; }
    for (unsigned ch = 0; ch < 2; ch++) {
        for (unsigned i = 0; i < 288; i++) { CP_FLOAT(a->decoder.mdct_overlap[ch][i]); }
    }
    for (unsigned i = 0; i < 960; i++) { CP_FLOAT(a->decoder.qmf_state[i]); }
    CP_U32(a->decoder.reserv); CP_U32(a->decoder.free_format_bytes);
    for (unsigned i = 0; i < 4; i++) { CP_U8(a->decoder.header[i]); }
    for (unsigned i = 0; i < 511; i++) { CP_U8(a->decoder.reserv_buf[i]); }
    for (unsigned i = 0; i < ARRAY_SIZE(a->input); i++) { CP_U8(a->input[i]); }
    CP_U32(a->input_bytes);
    for (unsigned i = 0; i < ARRAY_SIZE(a->pcm); i++) { CP_U16(a->pcm[i]); }
    CP_U32(a->pcm_frames);
    CP_BOOL(a->active); CP_BOOL(a->eof); CP_BOOL(a->completed); CP_BOOL(a->credit_notify);
    CP_U32(a->credit); CP_U32(a->rx_kind);
    CP_U64(a->due); CP_U64(a->start_ns); CP_U64(a->frames_decoded); CP_U64(a->frames_played);
    CP_U64(a->pcm_hash); CP_U64(a->nonzero_samples);
    CP_U64(a->credits_offered); CP_U64(a->credits_acked); CP_U64(a->words_credited); CP_U64(a->completions_acked);
    CP_U32(a->source_rate);
    DB2010AudioResampler *r = &a->resampler;
    CP_U16(r->previous[0]); CP_U16(r->previous[1]); CP_U32(r->phase);
    CP_BOOL(r->primed); CP_BOOL(r->ended);
    CP_U64(r->input_frames); CP_U64(r->output_frames);
    unsigned divisor = db2010_audio_rate_gcd(a->source_rate);
    unsigned scale = 48000 / divisor;
    unsigned step = a->source_rate / divisor;
    if ((a->source_rate && !db2010_audio_rate_supported(a->source_rate)) ||
        (r->primed && (!r->input_frames || !r->phase ||
            r->phase > (r->ended ? scale + step - 1 : step) || (r->ended && r->phase < scale))) ||
        (!r->primed && (r->phase || r->input_frames || r->output_frames))) { c->invalid = true; }
    CP_U32(a->source_channels);
    if (a->source_channels > 2) { c->invalid = true; }
    CP_BOOL(a->coefficient_ready);
    if (a->coefficient_ready && a->source_rate != 22050 && a->source_rate != 16000) { c->invalid = true; }
    if (o->emitted > a->pcm_frames) { c->invalid = true; }
    CP_BOOL(a->interrupted);
    if (a->interrupted && (a->active || a->completed || a->completions_acked || a->pcm_frames || a->input_bytes)) {
        c->invalid = true;
    }
    CP_BOOL(a->paused);
    if (a->paused && (a->active || a->interrupted || a->completed ||
        a->samples.ended || a->due != UINT64_MAX)) { c->invalid = true; }
    CP_U32(a->source_codec);
    CP_BOOL(a->aac.primed);
    for (unsigned ch = 0; ch < 2; ch++) {
        CP_U8(a->aac.window_shape[ch]);
        CP_U32(a->aac.rng[ch]);
        for (unsigned i = 0; i < 1024; i++) { CP_U32(a->aac.overlap[ch][i]); }
        if (a->aac.window_shape[ch] > 1) { c->invalid = true; }
    }
    if (a->source_codec > 1 || (a->source_codec == 1 &&
        (a->source_rate != 16000 || a->source_channels != 2)) ||
        (a->aac.primed && (a->source_codec != 1 || !a->frames_decoded))) {
        c->invalid = true;
    }
    CP_BOOL(a->underrun); CP_BOOL(a->underrun_notified);
    CP_U64(a->underruns_offered); CP_U64(a->underruns_acked);
    if (a->underruns_acked > a->underruns_offered ||
        (a->underrun && (a->source_codec != 1 || a->active || a->eof || a->completed || a->pcm_frames || a->input_bytes)) ||
        (a->rx_kind == 3 && (!a->underrun || !a->underrun_notified ||
         a->underruns_offered != a->underruns_acked + 1))) { c->invalid = true; }
    if (a->input_bytes > sizeof(a->input) ||
        a->pcm_frames > DB2010_AUDIO_MAX_PCM_FRAMES || a->credit > 800 || a->rx_kind > 3 ||
        a->decoder.reserv < 0 || a->decoder.reserv > 511 || a->decoder.free_format_bytes != 0) { c->invalid = true; }
    if (c->version >= 22) {
        CP_BOOL(v->midi_initialized); CP_BOOL(v->midi_prepared);
        CP_BOOL(v->midi_ring_announced); CP_BOOL(v->midi_ring_registered);
        CP_BOOL(v->midi_rx_ring); CP_U16(v->midi_ring_threshold);
        CP_U64(v->midi_requests); CP_U64(v->midi_replies);
        if ((v->midi_prepared && !v->midi_initialized) ||
            (v->midi_ring_registered && (!v->midi_ring_announced ||
             v->midi_ring_threshold != 750))) { c->invalid = true; }
    }
    if (c->version >= 23) {
        DB2010MidiStream *m = &v->midi;
        for (unsigned i = 0; i < DB2010_MIDI_VOICES; i++) {
            DB2010MidiVoice *voice = &m->synth.voices[i];
            CP_U32(voice->phase); CP_U32(voice->step); CP_U64(voice->age);
            CP_U16(voice->release); CP_U8(voice->channel); CP_U8(voice->note);
            CP_U8(voice->velocity); CP_BOOL(voice->held);
            if (voice->release > DB2010_MIDI_RELEASE || voice->channel > 15 ||
                voice->note > 127 || voice->velocity > 127 ||
                (voice->velocity && !voice->held && !voice->release)) { c->invalid = true; }
        }
        for (unsigned i = 0; i < 16; i++) {
            CP_U8(m->synth.program[i]); CP_U8(m->synth.volume[i]); CP_U16(m->synth.bend[i]);
            if (m->synth.program[i] > 127 || m->synth.volume[i] > 127 ||
                m->synth.bend[i] > 16383) { c->invalid = true; }
        }
        CP_U64(m->synth.notes); CP_U64(m->synth.stolen);
        DB2010AudioSamples *ms = &m->samples;
        for (unsigned i = 0; i < ARRAY_SIZE(ms->pcm); i++) { CP_U16(ms->pcm[i]); }
        CP_U32(ms->frames); CP_U32(ms->emitted);
        CP_U64(ms->first_frame); CP_U64(ms->start_ns); CP_U64(ms->previous_end_ns); CP_U64(ms->packets);
        CP_BOOL(ms->discontinuity); CP_BOOL(ms->ended);
        for (unsigned i = 0; i < DB2010_MIDI_RING_WORDS; i++) { CP_U16(m->ring[i]); }
        CP_U32(m->used); CP_U32(m->credit); CP_U32(m->rx_kind);
        CP_BOOL(m->active); CP_BOOL(m->paused); CP_BOOL(m->eof); CP_BOOL(m->completed);
        CP_BOOL(m->completion_requested); CP_BOOL(m->completion_sent); CP_BOOL(m->credit_notify);
        CP_BOOL(m->flush_pending);
        CP_U64(m->frames); CP_U64(m->origin_ns); CP_U64(m->origin_frame);
        CP_U64(m->last_time); CP_U64(m->end_frame);
        CP_U64(m->pcm_hash); CP_U64(m->nonzero_samples); CP_U64(m->events);
        CP_U64(m->words_received); CP_U64(m->words_credited);
        if (ms->frames >= DB2010_AUDIO_PACKET_FRAMES || ms->emitted ||
            m->used > DB2010_MIDI_RING_WORDS || m->credit > DB2010_MIDI_RING_WORDS ||
            m->used + m->credit > DB2010_MIDI_RING_WORDS || m->rx_kind > 2 ||
            (m->active && (m->paused || m->completed || ms->ended)) ||
            (m->paused && (m->active || m->completed || ms->ended)) ||
            m->origin_frame > m->frames) { c->invalid = true; }
    } else if (c->load && v->midi_prepared) {
        /* Checkpoint22 could prepare/announce/register MIDI but guarded all
         * event data. Its newly added renderer therefore starts empty, with
         * the same channel defaults as a fresh native prepare. */
        db2010_midi_stream_init(&v->midi);
    }
}

static void cp_DB2010F500ControlRuntime(Checkpoint *c, DB2010F500ControlRuntime *v)
{
    CP_U32(v->pending);
    CP_U64(v->due);
    CP_U64(v->commands);
    CP_U64(v->completions);
    CP_U64(v->busy_reads);
    CP_U64(v->ready_reads);
    CP_U64(v->config_writes);
}

static void cp_DB2010F500PortRuntime(Checkpoint *c, DB2010F500PortRuntime *v)
{
    CP_BOOL(v->setup_known);
    CP_BOOL(v->status_known);
    CP_BOOL(v->strobe_known);
    CP_U32(v->pending);
    CP_U32(v->strobe_high);
    CP_U8(v->setup_value);
    CP_U16(v->word);
    for (unsigned i0 = 0; i0 < 16; i0++) {
        CP_U16(v->last_by_tag[i0]);
    }
    for (unsigned i0 = 0; i0 < 16; i0++) {
        CP_BOOL(v->tag_known[i0]);
    }
    CP_U64(v->due);
    CP_U64(v->commands);
    CP_U64(v->completions);
    CP_U64(v->reads);
    CP_U64(v->busy_reads);
    CP_U64(v->pulses);
}

static void cp_DB2010SimFile(Checkpoint *c, DB2010SimFile *v)
{
    CP_U16(v->parent);
    CP_U16(v->id);
    CP_U16(v->size);
    CP_U32(v->structure);
    CP_U8(v->record_size);
    for (unsigned i0 = 0; i0 < 32; i0++) {
        CP_U8(v->data[i0]);
    }
}

static void cp_DB2010FmRadioRuntime(Checkpoint *c, DB2010FmRadioRuntime *v)
{
    CP_U32(v->active);
    CP_U32(v->used);
    CP_U32(v->latest_size);
    for (unsigned i0 = 0; i0 < DBFR_MAX_BYTES; i0++) {
        CP_U8(v->pending[i0]);
    }
    for (unsigned i0 = 0; i0 < DBFR_MAX_BYTES; i0++) {
        CP_U8(v->latest[i0]);
    }
    CP_U64(v->frames);
    CP_U64(v->bytes);
}

static void cp_DB2010PmicRuntime(Checkpoint *c, DB2010PmicRuntime *v)
{
    CP_U32(v->status_reads);
    CP_U64(v->register_reads);
    CP_U64(v->followup_cursor);
    CP_U8(v->extra_reg);
    CP_U32(v->extra_active);
    CP_U32(v->extra_read);
    CP_U32(v->extra_receive_more);
    CP_U64(v->extra_reads);
    CP_U64(v->extra_writes);
    CP_U64(v->power_writes);
    CP_U64(v->write_count);
    for (unsigned i0 = 0; i0 < 256; i0++) {
        CP_U8(v->last_value[i0]);
    }
    for (unsigned i0 = 0; i0 < 256; i0++) {
        CP_BOOL(v->last_known[i0]);
    }
}

static void cp_DB2010Power(Checkpoint *c, DB2010Power *v)
{
    CP_BOOL(v->power_known);
    CP_BOOL(v->initial_power_known);
    CP_U32(v->power_value);
    CP_U32(v->initial_power);
    CP_U32(v->power_mask);
    CP_BOOL(v->allow_startup_release);
    CP_U32(v->startup_releases);
}

static void cp_DB2010Startup(Checkpoint *c, DB2010Startup *v)
{
    CP_U8(v->startup_status_initial);
    CP_U8(v->startup_status_value);
    CP_U64(v->startup_status_reads);
    CP_U64(v->startup_status_clears);
}

static void cp_DB2010MemoryController(Checkpoint *c, DB2010MemoryController *v)
{
    for (unsigned i0 = 0; i0 < 11; i0++) {
        CP_U32(v->controller_words[i0]);
    }
    CP_U32(v->initial_bank);
    for (unsigned i0 = 0; i0 < 11; i0++) {
        CP_BOOL(v->controller_known[i0]);
    }
}

static void cp_DB2010RegionController(Checkpoint *c, DB2010RegionController *v)
{
    CP_BOOL(v->has_section_table);
    CP_BOOL(v->table_dirty);
    for (unsigned i0 = 0; i0 < 24; i0++) {
        CP_U32(v->region_words[i0]);
    }
    for (unsigned i0 = 0; i0 < 24; i0++) {
        CP_U32(v->initial_region_words[i0]);
    }
    for (unsigned i0 = 0; i0 < 24; i0++) {
        CP_BOOL(v->region_known[i0]);
    }
    for (unsigned i0 = 0; i0 < 24; i0++) {
        CP_BOOL(v->initial_region_known[i0]);
    }
    CP_U64(v->table_commits);
}

static void cp_DB2010Absent(Checkpoint *c, DB2010Absent *v)
{
    CP_U64(v->absent_reads);
    CP_U64(v->absent_writes);
}

static void cp_DB2010NorPart(Checkpoint *c, DB2010NorPart *v)
{
    CP_U32(v->mode);
    CP_U32(v->pending);
    CP_U16(v->status);
    CP_U32(v->block);
    CP_U32(v->address);
    CP_U32(v->count);
    CP_U32(v->used);
    for (unsigned i0 = 0; i0 < 32; i0++) {
        CP_U32(v->word_address[i0]);
    }
    for (unsigned i0 = 0; i0 < 32; i0++) {
        CP_U16(v->word_value[i0]);
    }
}

static void cp_DB2010SimCard(Checkpoint *c, DB2010SimCard *v)
{
    for (unsigned i0 = 0; i0 < DBSIM_FILES; i0++) {
        cp_DB2010SimFile(c, &v->files[i0]);
    }
    CP_U16(v->df);
    CP_U16(v->ef);
    for (unsigned i0 = 0; i0 < 260; i0++) {
        CP_U8(v->input[i0]);
    }
    for (unsigned i0 = 0; i0 < DBSIM_QUEUE; i0++) {
        CP_U8(v->output[i0]);
    }
    for (unsigned i0 = 0; i0 < 32; i0++) {
        CP_U8(v->response[i0]);
    }
    CP_U32(v->input_len);
    CP_U32(v->input_need);
    CP_U32(v->output_head);
    CP_U32(v->output_len);
    CP_U32(v->response_len);
    CP_BOOL(v->input_data);
    CP_BOOL(v->pps);
    CP_U64(v->apdus);
    CP_U16(v->last_sw);
    for (unsigned i0 = 0; i0 < 5; i0++) {
        CP_U8(v->last_header[i0]);
    }
}

