/* Synthetic GSM card profile/APDUs adapted from pmb7850's QEMU stub.
 * Controller transport is independent. Unsupported commands stay guarded.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-sim-card.h"

static void queue(DB2010SimCard *c, uint8_t v)
{
    g_assert(c->output_len < DBSIM_QUEUE);
    c->output[(c->output_head + c->output_len++) % DBSIM_QUEUE] = v;
}
uint8_t dbsim_card_pop(DB2010SimCard *c)
{
    g_assert(c->output_len);
    uint8_t v = c->output[c->output_head];
    c->output_head = (c->output_head + 1) % DBSIM_QUEUE;
    c->output_len--;
    return v;
}
void dbsim_card_activate(DB2010SimCard *c)
{
    c->df = 0x3f00; c->ef = 0;
    c->input_len = 0; c->input_need = 5;
    c->input_data = c->pps = false;
    c->output_head = c->output_len = c->response_len = 0;
    static const uint8_t atr[] = {0x3b, 0x90, 0x11, 0x00};
    for (unsigned i = 0; i < sizeof(atr); i++) { queue(c, atr[i]); }
}
void dbsim_card_init(DB2010SimCard *c)
{
    memset(c, 0, sizeof(*c));
    static const struct { uint16_t parent, id; int structure; unsigned size; } profile[] = {
        {0,0x3f00,-1,0}, {0x3f00,0x7f10,-1,0}, {0x3f00,0x7f20,-1,0},
        {0x3f00,0x7f21,-1,0}, {0x3f00,0x2fe2,0,10}, {0x7f20,0x6f07,0,9},
        {0x7f20,0x6f05,0,1}, {0x7f20,0x6fae,0,1}, {0x7f20,0x6f38,0,2},
        {0x7f20,0x6fad,0,4}, {0x7f20,0x6f20,0,9}, {0x7f20,0x6f31,0,1},
        {0x7f20,0x6f30,0,24}, {0x7f20,0x6f7e,0,11}, {0x7f20,0x6f74,0,16},
        {0x7f20,0x6f78,0,2}, {0x7f20,0x6f7b,0,12},
        {0x7f20,0x6f16,0,3}, {0x7f20,0x6f14,0,16},
    };
    G_STATIC_ASSERT(ARRAY_SIZE(profile) == DBSIM_FILES);
    for (unsigned i = 0; i < DBSIM_FILES; i++) {
        c->files[i] = (DB2010SimFile){.parent=profile[i].parent,.id=profile[i].id,
            .structure=profile[i].structure,.size=profile[i].size};
    }
    static const uint8_t iccid[] = {0x98,0x94,0x10,0,0,0,0,0,0,0xf0};
    static const uint8_t imsi[] = {8,0x10,0x10,0x10,0x32,0x54,0x76,0x98,0xf0};
    memcpy(c->files[4].data,iccid,sizeof(iccid));
    memcpy(c->files[5].data,imsi,sizeof(imsi));
    c->files[6].data[0]=0xff; c->files[7].data[0]=2;
    c->files[8].data[0]=3; c->files[9].data[3]=2;
    memset(c->files[10].data,0xff,8); c->files[10].data[8]=7;
    for (unsigned i=12;i<=14;i++) { memset(c->files[i].data,0xff,c->files[i].size); }
    c->files[13].data[10]=1; c->files[15].data[1]=1;
    memset(c->files[16].data,0xff,12);
    c->files[17].data[0]=1; c->files[17].data[1]=2;
    memset(c->files[18].data,0xff,16); memcpy(c->files[18].data,"UA-KYIVSTAR",11);
    c->df=0x3f00; c->input_need=5;
}
static DB2010SimFile *find(DB2010SimCard *c, uint16_t id)
{
    for (unsigned i=0;i<DBSIM_FILES;i++) {
        DB2010SimFile *f=&c->files[i];
        if (f->id==id && (f->structure<0 || f->parent==c->df)) { return f; }
    }
    return NULL;
}
static unsigned response(DB2010SimCard *c, DB2010SimFile *f, uint8_t *out)
{
    /* TS11.11 9.2.1: all22 mandatory DF bytes;15-byte EF header.
     * PMB's15-byte DF prefix is insufficient for a general GSM terminal. */
    unsigned n=f->structure<0 ? 22 : 15;
    memset(out,0,n); out[2]=f->size>>8; out[3]=f->size;
    out[4]=f->id>>8; out[5]=f->id;
    if (f->structure<0) {
        out[6]=f->id==0x3f00 ? 1 : 2; out[12]=n-13; out[13]=0x93;
        for (unsigned i=0;i<DBSIM_FILES;i++) {
            if (c->files[i].parent==f->id) { out[c->files[i].structure<0 ? 14 : 15]++; }
        }
        out[16]=2; out[18]=out[20]=0x83; out[19]=out[21]=0x8a;
    } else {
        out[6]=4; out[11]=1; /* file valid, readable while not invalidated */
        out[12]=2; out[13]=f->structure; out[14]=f->record_size;
    }
    return n;
}
static const char *execute(DB2010SimCard *c)
{
    uint8_t *a=c->input, data[256];
    unsigned n=0, le=a[4] ? a[4] : 256, off=((a[2]&127)<<8)|a[3];
    uint16_t sw=0x9000;
    DB2010SimFile *f=find(c,c->ef ? c->ef : c->df);
    switch(a[1]) {
    case 0xa4:
        f=find(c,((unsigned)a[5]<<8)|a[6]); c->response_len=0;
        if (!f) { sw=0x9404; break; }
        if (f->structure<0) { c->df=f->id; c->ef=0; } else { c->ef=f->id; }
        c->response_len=response(c,f,c->response); sw=0x9f00|c->response_len;
        break;
    case 0xc0:
        n=MIN(le,c->response_len); memcpy(data,c->response,n); break;
    case 0xf2:
        f=find(c,c->df); if (!f) { return "sim_card_directory_unmodeled"; }
        n=MIN(le,response(c,f,data)); break;
    case 0xb0: case 0xd6:
        if (!f || f->structure!=0 || !c->ef || off+(a[1]==0xb0 ? le:a[4])>f->size) {
            sw=0x9402; break;
        }
        if (a[1]==0xb0) { n=le; memcpy(data,f->data+off,n); }
        else { memcpy(f->data+off,a+5,a[4]); }
        break;
    case 0xb2: case 0xdc:
        /* Profile has no record EFs: report the source card's address error. */
        sw=0x9402; break;
    case 0x20:
        /* Ported dummy PIN acceptance; profile declares CHV1 disabled. */
        break;
    default: return "sim_card_instruction_unmodeled";
    }
    for (unsigned i=0;i<n;i++) { queue(c,data[i]); }
    queue(c,sw>>8); queue(c,sw);
    c->apdus++; c->last_sw=sw; memcpy(c->last_header,a,5);
    c->input_len=0; c->input_need=5; c->input_data=false;
    return NULL;
}
const char *dbsim_card_input(DB2010SimCard *c, uint8_t v)
{
    if (c->output_len) { return "sim_card_input_before_response_drained"; }
    if (c->input_len>=sizeof(c->input)) { return "sim_card_command_overflow"; }
    if (!c->input_len) {
        c->pps=v==0xff; c->input_need=c->pps ? 3 : 5;
        if (!c->pps && v!=0xa0 && v!=0x00) { return "sim_card_class_unmodeled"; }
    }
    c->input[c->input_len++]=v;
    if (c->pps && c->input_len==2) {
        c->input_need=3+!!(v&16)+!!(v&32)+!!(v&64);
    }
    if (c->input_len<c->input_need) { return NULL; }
    if (c->pps) {
        uint8_t check=0;
        for (unsigned i=0;i<c->input_len;i++) { check^=c->input[i]; }
        if (check || (c->input[1]&15)) { return "sim_card_pps_unmodeled"; }
        for (unsigned i=0;i<c->input_len;i++) { queue(c,c->input[i]); }
        c->input_len=0; c->pps=false; return NULL;
    }
    if (!c->pps && c->input[0]==0) {
        /* Known ISO-class probe against a GSM-only card: TS11.11 9.4. */
        queue(c,0x6e); queue(c,0); c->apdus++; c->last_sw=0x6e00;
        memcpy(c->last_header,c->input,5); c->input_len=0; c->input_need=5;
        return NULL;
    }
    if (!c->input_data) {
        uint8_t ins=c->input[1];
        if (ins!=0xa4 && ins!=0xc0 && ins!=0xf2 && ins!=0xb0 && ins!=0xd6 &&
            ins!=0xb2 && ins!=0xdc && ins!=0x20) { return "sim_card_instruction_unmodeled"; }
        if (ins==0xa4 && (c->input[2] || c->input[3] || c->input[4]!=2)) {
            return "sim_card_select_parameters_unmodeled";
        }
        if ((ins==0xb0 || ins==0xd6) && (c->input[2]&128)) {
            return "sim_card_binary_mode_unmodeled";
        }
        queue(c,ins); /* T=0 acknowledgement: transfer all remaining bytes. */
        if ((ins==0xa4 || ins==0xd6 || ins==0xdc || ins==0x20) && c->input[4]) {
            c->input_need=5+c->input[4]; c->input_data=true; return NULL;
        }
    }
    return execute(c);
}
