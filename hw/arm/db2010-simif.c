/* Native SIMIF5/34 transport. Electrical/timing behavior is synthetic.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-simif.h"

static void update_irq(DB2010Simif *s)
{
    bool enabled=s->active && (s->regs[2]&0x84)==0x84;
    db2010_signal_set(&s->rx_irq,enabled && !(s->regs[3]&0x20) && s->rx_ready);
    /* Native ILR58 is an edge: clock resume must not repeat an old empty
     * indication. One event per completed shift, retained while masked. */
    if (enabled && !(s->regs[3]&0x80) && s->tx_event) {
        s->tx_event=false;
        db2010_signal_set(&s->tx_irq,true);
        db2010_signal_set(&s->tx_irq,false);
    }
}
void dbsim_reset(DB2010Simif *s)
{
    uint32_t ns=s->byte_ns;
    DB2010Signal rx=s->rx_irq, tx=s->tx_irq;
    bool omit=s->omit_diagnostics;
    *s=(DB2010Simif){.omit_diagnostics=omit,.byte_ns=ns,.rx_irq=rx,.tx_irq=tx,.rx_due=-1,.tx_due=-1};
    dbsim_card_init(&s->card); update_irq(s);
}
void dbsim_advance(DB2010Simif *s, uint64_t now)
{
    if (s->rx_due>=0 && now>=(uint64_t)s->rx_due) {
        s->rx_due=-1;
        if (s->active && s->card.output_len) { s->rx_ready=true; }
    }
    if (s->tx_due>=0 && now>=(uint64_t)s->tx_due) {
        s->tx_due=-1;
        if (s->active) { s->tx_ready=s->tx_event=true; }
    }
    update_irq(s);
}
int64_t dbsim_deadline(DB2010Simif *s)
{
    return s->rx_due<0 ? s->tx_due : s->tx_due<0 ? s->rx_due : MIN(s->rx_due,s->tx_due);
}
static const char *access_inner(DB2010Simif *s, DB2010Access *a)
{
    unsigned off=a->address-0xf9060000;
    if (a->size!=1 || off>12 || (off&1)) { return "simif_access_unmodeled"; }
    if (off==0) {
        if (!s->active) { return "simif_card_not_active"; }
        if (a->write) {
            if (s->tx_due>=0) { return "simif_transmit_busy"; }
            const char *fault=dbsim_card_input(&s->card,a->value);
            if (fault) { return fault; }
            s->tx_bytes++; s->tx_ready=s->tx_event=false; s->tx_due=a->now+s->byte_ns;
            if (s->card.output_len && !s->rx_ready && s->rx_due<0) {
                s->rx_due=a->now+s->byte_ns;
            }
        } else {
            if (!s->rx_ready) { return "simif_receive_empty"; }
            a->value=dbsim_card_pop(&s->card); s->rx_bytes++; s->rx_ready=false;
            if (s->card.output_len) { s->rx_due=a->now+s->byte_ns; }
        }
    } else if (off==8) {
        if (a->write) { return "simif_status_write_unmodeled"; }
        /* Original4490E758 drains RX while status&F8; bit1/2 report errors. */
        a->value=s->rx_ready ? 8 : 0;
    } else if (off==10) {
        return "simif_register_unmodeled";
    } else if (a->write) {
        unsigned v=a->value;
        if (v>255 || (off==2 && v!=0x40 && v!=0xc0) ||
            (off==4 && (v&8))) { return "simif_control_unmodeled"; }
        unsigned old=s->regs[off/2];
        s->regs[off/2]=v;
        if (off==2) { s->setup_known=true; }
        if (off==4) {
            if ((old&1) && !(v&1)) {
                s->active=s->rx_ready=s->tx_ready=s->tx_event=false;
                s->rx_due=s->tx_due=-1;
                s->card.output_len=0;
            }
            if (!(old&1) && (v&1) && (v&4)) {
                s->active=true; s->rx_ready=s->tx_ready=s->tx_event=false;
                s->activations++; dbsim_card_activate(&s->card);
                s->rx_due=a->now+s->byte_ns; s->tx_due=-1;
            }
        }
    } else {
        if (off==12 || (off==2 && !s->setup_known)) { return "simif_register_read_unknown"; }
        a->value=s->regs[off/2];
    }
    return NULL;
}
const char *dbsim_access(void *opaque, DB2010Access *a)
{
    if (a->write && a->size==1) { a->value&=255; }
    DB2010Simif *s=opaque, preview=*s;
    /* Validate byte parser/queue/file effects atomically before a CPU store. */
    DB2010Simif *target=&preview;
    const char *fault=access_inner(target,a);
    if (fault || !a->commit) { return fault; }
    *s=preview;
    if (!s->omit_diagnostics) {
        s->history[s->events%ARRAY_SIZE(s->history)]=(DB2010SimEvent){
            .ns=a->now,.pc=a->pc,.offset=a->address-0xf9060000,.value=a->value,.write=a->write};
    }
    s->events++;
    update_irq(s);
    return NULL;
}
static const char *simif_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    return dbsim_access(opaque,a);
}
const DB2010RegMap *dbsim_map(void)
{
    static const DB2010RegRule rules[]={DB_WINDOW(0,0x1000,
        .read={.kind=DB_ACTION_CALLBACK,.widths=DB_WIDTH8,.callback=simif_action},
        .write={.kind=DB_ACTION_CALLBACK,.widths=DB_WIDTH8,.callback=simif_action})};
    static DB2010RegMap map;
    static gsize once;
    if (g_once_init_enter(&once)) {
        const char *fault=db2010_regmap_init(&map,0xf9060000,rules,ARRAY_SIZE(rules),"simif_access_unmodeled");
        if (fault) { g_error("SIMIF map: %s",fault); }
        g_once_init_leave(&once,1);
    }
    return &map;
}
void dbsim_export(DB2010Simif *s, QDict *snapshot)
{
    QDict *out=qdict_new(); QList *history=qlist_new();
    qdict_put_str(out,"profile","pmb7850-gsm-stub-native-simif");
    qdict_put_bool(out,"synthetic",true); qdict_put_int(out,"byte_ns",s->byte_ns);
    qdict_put_bool(out,"active",s->active); qdict_put_int(out,"activations",s->activations);
    qdict_put_bool(out,"rx_ready",s->rx_ready); qdict_put_bool(out,"tx_ready",s->tx_ready);
    qdict_put_int(out,"tx_bytes",s->tx_bytes); qdict_put_int(out,"rx_bytes",s->rx_bytes);
    qdict_put_int(out,"apdus",s->card.apdus); qdict_put_int(out,"selected_df",s->card.df);
    qdict_put_int(out,"selected_ef",s->card.ef); qdict_put_int(out,"last_sw",s->card.last_sw);
    qdict_put_int(out,"queued_bytes",s->card.output_len); qdict_put_int(out,"input_bytes",s->card.input_len);
    qdict_put_int(out,"rx_due",s->rx_due); qdict_put_int(out,"tx_due",s->tx_due);
    GString *hex=g_string_new(NULL);
    for (unsigned i=0;i<s->card.input_len;i++) { g_string_append_printf(hex,"%02x",s->card.input[i]); }
    qdict_put_str(out,"input_hex",hex->str); g_string_truncate(hex,0);
    for (unsigned i=0;i<5;i++) { g_string_append_printf(hex,"%02x",s->card.last_header[i]); }
    qdict_put_str(out,"last_header_hex",hex->str); g_string_free(hex,true);
    qdict_put_int(out,"events_total",s->events);
    uint64_t first = MAX(s->history_baseline, s->events > 256 ? s->events - 256 : 0);
    qdict_put_int(out,"events_dropped",s->omit_diagnostics ? s->events : first);
    for (uint64_t i=s->omit_diagnostics ? s->events : first;i<s->events;i++) {
        DB2010SimEvent *e=&s->history[i%256]; QDict *row=qdict_new();
        qdict_put_int(row,"sequence",i+1); qdict_put_int(row,"ns",e->ns);
        qdict_put_int(row,"pc",e->pc); qdict_put_int(row,"offset",e->offset);
        qdict_put_int(row,"value",e->value); qdict_put_bool(row,"write",e->write);
        qlist_append(history,row);
    }
    qdict_put(out,"events",history); qdict_put(snapshot,"simif",out);
}
