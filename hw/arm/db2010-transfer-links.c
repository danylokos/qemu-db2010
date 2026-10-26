/* Transfer controller connections. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-components.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

typedef enum DB2010DmaRequestKind { DBDMA_NO_REQUEST, DBDMA_DISPLAY_REQUEST, DBDMA_FA_REQUEST } DB2010DmaRequestKind;
static const char *dma_request_preflight(DB2010DmaLink *s, DB2010DmaRequestKind request)
{
    switch (request) {
    case DBDMA_DISPLAY_REQUEST: return display_dma_fault(s->display);
    case DBDMA_FA_REQUEST: return fa_dma_arm_fault(s->fa);
    case DBDMA_NO_REQUEST: return NULL;
    default: g_assert_not_reached();
    }
}
const char *db2010_dma_access(void *opaque, DB2010Access *a)
{
    DB2010DmaLink *s = opaque;
    DB2010DmaRequestKind request = DBDMA_NO_REQUEST;
    if (s->dma->config.display_first_burst && a->write && a->address == 0xf20001b0 &&
        a->size == 4 && a->value == 0xea53) { request = DBDMA_DISPLAY_REQUEST; }
    if (s->dma->config.fa_tx && a->write && a->address == 0xf2000150 &&
        a->size == 4 && (a->value & 1)) { request = DBDMA_FA_REQUEST; }
    bool rx = request == DBDMA_FA_REQUEST && s->dma->config.fa_rx &&
              s->dma->state.channel[2][0] == 0xfa040020;
    const char *fault = rx ? dsp_peer_rx_arm_fault(s->fa) : dma_request_preflight(s, request);
    if (fault) { return fault; }
    DB2010Dma preview = *s->dma;
    fault = dbdma_access(a->commit ? s->dma : &preview, a->address,
                         a->size, a->write, &a->value);
    if (!fault && a->commit && request == DBDMA_DISPLAY_REQUEST) {
        display_dma_first_burst(s->display);
    }
    if (!fault && a->commit) { display_irq_update(s->display); }
    return fault;
}

const char *db2010_mailbox_access(void *opaque, DB2010Access *a)
{
    DB2010FaLink *s = opaque;
    DB2010DecodedAccess decoded;
    const char *fault = db2010_reg_decode(dbfm_map(), a, &decoded);
    if (fault) { return fault; }
    DB2010Access preview = *a;
    preview.commit = false;
    fault = db2010_reg_execute(&s->fa_mailbox, &decoded, &preview);
    a->value = preview.value;
    if (fault) { return fault; }
    bool submit = a->write && decoded.id == DBFM_COMMAND_STATUS;
    if (s->has_dsp_peer && submit) {
        fault = dsp_peer_submit_fault(s);
        if (fault) { return fault; }
    }
    if (!a->commit) { return NULL; }
    fault = db2010_reg_execute(&s->fa_mailbox, &decoded, a);
    if (!fault && s->has_dsp_peer && submit) {
        dbdsp_request(s->dsp_peer,
            s->dsp_peer->config.wire10 && (s->fa_mailbox.words[0] & 0x7ff) == 10,
            (s->dma->state.channel[2][3] & 0xfff) * 4, s->fa_mailbox.words[2], a->now);
    }
    return fault;
}

const char *fa_dma_arm_fault(DB2010FaLink *s)
{
    uint32_t *ch = s->dma->state.channel[2];
    unsigned bytes = (ch[3] & 0xfff) * 4;
    if (s->dma->state.global[4] != 1 || ch[1] != 0xfa040020 || ch[2] ||
        (s->dsp_peer->config.variable_dma ?
          ((ch[3] & 0xfffff000) != 0xa4492000 || !(ch[3] & 0xfff) || (ch[3] & 0xfff) > 4094 ||
           (s->dsp_peer->state.phase != DBDSP_IDLE && s->dsp_peer->state.phase != DBDSP_DONE) || (s->dma->state.raw_tc & 4) || s->fa_mailbox.busy) :
          ch[3] != 0xa4492038) || ch[4] != 0xc8c6) {
        return "fa_dma_configuration_unmodeled";
    }
    DB2010MemoryView view;
    DB2010MemoryView *r = db2010_memory_view(&s->memory, ch[0], bytes, &view);
    if (!r || ch[0] % 4 || !r->known) { return "fa_dma_source_unmodeled"; }
    if (s->memory.overlay(s->memory.opaque, ch[0], bytes)) {
        return "fa_dma_overlay_source_unmodeled";
    }
    for (unsigned i = 0; i < bytes; i++) {
        if (!r->unchecked && !r->known[ch[0] - r->base + i]) { return "fa_dma_source_unknown"; }
    }
    return NULL;
}





void db2010_fa_reset(DB2010FaLink *s)
{
    dbfm_reset(&s->fa_mailbox);
    dbdsp_reset(s->dsp_peer);
}

void db2010_fa_dma_export(DB2010FaLink *model, QDict *snapshot)
{

        QDict *pending = qdict_new();
        qdict_put_bool(pending, "armed", model->dma->state.channel[2][4] & 1);
        qdict_put_str(pending, "waiting_for", model->has_dsp_peer ? "synthetic DSP contract phase" : "FA peripheral request; not supplied");
        qdict_put_int(pending, "transferred_bytes", model->has_dsp_peer ? model->dsp_peer->state.bytes : 0);
        qdict_put_bool(pending, "completion_supplied", model->has_dsp_peer && model->dsp_peer->state.phase >= 3);
        qdict_put(snapshot, "fa_dma", pending);
    }
