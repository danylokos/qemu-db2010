/* Native ROM command/array separation; contracts inherited from Unicorn0073.
 * Prepared bytes and probe inputs remain conditional research assumptions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "qobject/qbool.h"
#include "db2010-nor.h"

#define BASE DBNOR_BASE
#define SIZE DBNOR_SIZE
#define PART_SIZE 0x200000u

static uint32_t number(QDict *d, const char *key, uint32_t max)
{
    uint64_t v;
    QNum *n = qobject_to(QNum, qdict_get(d, key));
    if (!n || !qnum_get_try_uint(n, &v) || v > max) {
        error_report("db2010 NOR: invalid/missing %s", key);
        exit(EXIT_FAILURE);
    }
    return v;
}

static bool named(QDict *d, const char *key, const char *value)
{
    const char *text = qdict_get_try_str(d, key);
    return text && !strcmp(text, value);
}

static bool valid_rcr(uint32_t v)
{
    unsigned latency = (v >> 11) & 7, burst = v & 7;
    return v <= 0xffff && !(v & 0x4030) && (v & 0x80) && latency >= 2 &&
           (burst == 1 || burst == 2 || burst == 3 || burst == 7) &&
           !(latency == 2 && (v & 0x200) && (v & 0x100));
}

void db2010_nor_configure(DB2010Nor *n, QDict *state)
{
    QDict *d = qobject_to(QDict, qdict_get(state, "nor_model"));
    static const uint16_t root[] = {
        0x51, 0x52, 0x59, 1, 0, 0x0a, 1, 0, 0, 0, 0, 0x17, 0x20,
        0x85, 0x95, 8, 9, 10, 0, 1, 1, 2, 0, 25, 1, 0, 6, 0, 2,
        0xfe, 0, 0, 2, 3, 0, 0x80, 0,
    };
    static const uint8_t extended[] = {
        0x50,0x52,0x49,0x31,0x33,0xe6,3,0,0,1,3,0,0x18,0x90,2,0x80,0,3,3,
        0x89,0,0,0,0,0,0,0x10,0,4,3,4,1,2,3,7,
        2,0x0f,0,0x11,0,0,1,0x0f,0,0,2,0x64,0,2,3,
        1,0,0x11,0,0,2,0x0e,0,0,2,0x64,0,2,3,
        3,0,0x80,0,0x64,0,2,3,
    };
    unsigned i;
    if (!d || !named(d,"profile","intel-l18-256m-top") ||
        number(d,"address",UINT32_MAX) != BASE || number(d,"size",UINT32_MAX) != SIZE ||
        number(d,"partition_size",UINT32_MAX) != PART_SIZE ||
        !named(d,"completion","synchronous") || !named(d,"initial_array","unknown") ||
        !qobject_to(QBool,qdict_get(d,"initial_locked")) || !qobject_to(QBool,qdict_get(d,"wp_high"))) {
        error_report("db2010: unsupported conditional NOR profile");
        exit(EXIT_FAILURE);
    }
    if (qdict_haskey(d, "unaligned_array_reads")) {
        if (!named(d, "unaligned_array_reads", "halfword-byte-assembly")) {
            error_report("db2010: unsupported unaligned NOR read profile"); exit(EXIT_FAILURE);
        }
        /* Explicit ARMv5 odd-LDRH hypothesis, not measured handset behavior. */
        n->unaligned_array_halfwords = true;
    }
    if (qdict_haskey(d, "diagnostic_read_overlay")) {
        QDict *overlay = qobject_to(QDict, qdict_get(d, "diagnostic_read_overlay"));
        QList *entries = overlay ? qobject_to(QList, qdict_get(overlay, "entries")) : NULL;
        if (!overlay || qdict_size(overlay) != 2 ||
            !named(overlay, "profile", "gdfs-digest-bytes") || !entries ||
            qlist_empty(entries) || qlist_size(entries) > 2) {
            error_report("db2010: invalid diagnostic GDFS read overlay"); exit(EXIT_FAILURE);
        }
        const QListEntry *entry;
        QLIST_FOREACH_ENTRY(entries, entry) {
            QDict *e = qobject_to(QDict, qlist_entry_obj(entry));
            const char *expected = e ? qdict_get_try_str(e, "expected_hex") : NULL;
            const char *value = e ? qdict_get_try_str(e, "value_hex") : NULL;
            if (!e || qdict_size(e) != 3 || !expected || !value ||
                strlen(expected) != 40 || strlen(value) != 40) {
                error_report("db2010: GDFS overlay requires exact20-byte digests"); exit(EXIT_FAILURE);
            }
            DB2010NorOverlay *o = &n->overlay[n->overlay_count];
            o->address = number(e, "address", UINT32_MAX);
            if (o->address < 0x45f00000u || (uint64_t)o->address + 20 > 0x46000000u) {
                error_report("db2010: GDFS overlay outside record bank"); exit(EXIT_FAILURE);
            }
            for (unsigned k = 0; k < n->overlay_count; k++) {
                if ((uint64_t)o->address < (uint64_t)n->overlay[k].address + 20 &&
                    (uint64_t)o->address + 20 > n->overlay[k].address) {
                    error_report("db2010: overlapping GDFS overlays"); exit(EXIT_FAILURE);
                }
            }
            for (unsigned k = 0; k < 40; k++) {
                if (!strchr("0123456789abcdef", expected[k]) ||
                    !strchr("0123456789abcdef", value[k])) {
                    error_report("db2010: invalid GDFS overlay digest hex"); exit(EXIT_FAILURE);
                }
            }
            for (unsigned k = 0; k < 20; k++) {
                o->expected[k] = (g_ascii_xdigit_value(expected[k*2]) << 4) |
                                 g_ascii_xdigit_value(expected[k*2+1]);
                o->value[k] = (g_ascii_xdigit_value(value[k*2]) << 4) |
                              g_ascii_xdigit_value(value[k*2+1]);
            }
            n->overlay_count++;
        }
    }
    n->initial_locked = qdict_get_bool(d,"initial_locked");
    n->wp_high = qdict_get_bool(d,"wp_high");
    n->manufacturer = number(d,"manufacturer_id",0xffff);
    n->device = number(d,"device_id",0xffff);
    if (qdict_haskey(d,"mirrored_command_bytes") &&
        !qobject_to(QBool,qdict_get(d,"mirrored_command_bytes"))) {
        error_report("db2010: mirrored NOR commands require an explicit boolean");
        exit(EXIT_FAILURE);
    }
    n->mirrored = qdict_get_try_bool(d,"mirrored_command_bytes",false);
    if (qdict_haskey(d,"identifier_cfi")) {
        if (!named(d,"identifier_cfi","root-10-34-compatibility-assumption")) {
            error_report("db2010: unsupported NOR ID/CFI alias"); exit(EXIT_FAILURE);
        }
        n->id_cfi = true;
    }
    if (qdict_haskey(d,"factory_unique_id")) {
        const char *id = qdict_get_try_str(d,"factory_unique_id");
        if (!id || strlen(id) != 16) {
            error_report("db2010: NOR identity requires 16 lower-case hex digits"); exit(EXIT_FAILURE);
        }
        for (i=0;i<16;i++) {
            if (!strchr("0123456789abcdef",id[i])) {
                error_report("db2010: invalid NOR identity"); exit(EXIT_FAILURE);
            }
            n->unique = (n->unique << 4) | g_ascii_xdigit_value(id[i]);
        }
        n->has_unique = true;
    }
    if (qdict_haskey(d,"protection_lock0")) {
        n->has_protection_lock0 = true;
        if (qobject_type(qdict_get(d,"protection_lock0")) != QTYPE_QNULL) {
            unsigned value = number(d,"protection_lock0",0xffff);
            /* Factory segment locked; unused bits read as explicit ones.
             * User64 lock bit1 is an input, never inferred from main-array locks. */
            if (value != 0xfffe && value != 0xfffc) {
                error_report("db2010: unsupported NOR protection-lock0 input"); exit(EXIT_FAILURE);
            }
            n->protection_lock0 = value; n->protection_lock0_known = true;
        }
    }
    if (qdict_haskey(d,"user_protection64")) {
        const char *id = qdict_get_try_str(d,"user_protection64");
        if (!id || strlen(id) != 16) {
            error_report("db2010: user protection requires 16 lower-case hex digits"); exit(EXIT_FAILURE);
        }
        for (i=0;i<16;i++) {
            if (!strchr("0123456789abcdef",id[i])) {
                error_report("db2010: invalid user protection"); exit(EXIT_FAILURE);
            }
            n->user_protection = (n->user_protection << 4) | g_ascii_xdigit_value(id[i]);
        }
        n->has_user_protection = true;
    }
    if (qdict_haskey(d,"read_configuration_register")) {
        QDict *r = qobject_to(QDict,qdict_get(d,"read_configuration_register"));
        if (!r || qdict_size(r)!=2 || !named(r,"profile","l18-x16-address") ||
            !valid_rcr(number(r,"initial_value",0xffff))) {
            error_report("db2010: invalid NOR read configuration"); exit(EXIT_FAILURE);
        }
        n->has_rcr = true;
        n->initial_rcr = number(r,"initial_value",0xffff);
    }
    if (qdict_haskey(d,"reserved_cfi_words")) {
        QDict *reserved = qobject_to(QDict,qdict_get(d,"reserved_cfi_words"));
        const QDictEntry *entry;
        if (!reserved) { error_report("db2010: invalid reserved CFI words"); exit(EXIT_FAILURE); }
        for (entry=qdict_first(reserved);entry;entry=qdict_next(reserved,entry)) {
            const char *key = qdict_entry_key(entry);
            char *end;
            unsigned long offset = strtoul(key,&end,10);
            if (!*key || *end || offset >= 512) {
                error_report("db2010: CFI offset outside supported table"); exit(EXIT_FAILURE);
            }
            n->cfi[offset] = number(reserved,key,0xffff);
            n->cfi_known[offset] = true;
        }
    }
    for (i=0;i<G_N_ELEMENTS(root);i++) {
        n->cfi[0x10+i]=root[i]; n->cfi_known[0x10+i]=true;
    }
    if (qdict_haskey(d,"extended_cfi")) {
        if (!named(d,"extended_cfi","intel-l18-256m-top-v1.3")) {
            error_report("db2010: unsupported extended CFI profile"); exit(EXIT_FAILURE);
        }
        for (i=0;i<G_N_ELEMENTS(extended);i++) {
            n->cfi[0x10a+i]=extended[i]; n->cfi_known[0x10a+i]=true;
        }
    }
    if (qdict_haskey(state,"nor_byte_bridge")) {
        QDict *b = qobject_to(QDict,qdict_get(state,"nor_byte_bridge"));
        if (!b || qdict_size(b)!=1 || !named(b,"profile","steered-query-reset-zero-high")) {
            error_report("db2010: unsupported NOR byte bridge"); exit(EXIT_FAILURE);
        }
        n->bridge=true;
    }
    if (qdict_haskey(state,"nor_probe_placeholders")) {
        QDict *p = qobject_to(QDict,qdict_get(state,"nor_probe_placeholders"));
        if (!p || (qdict_size(p)!=1 && qdict_size(p)!=2) || number(p,"value",255)!=255 ||
            (qdict_haskey(p,"x16_retry") && (!n->bridge || !qdict_get_try_bool(p,"x16_retry",false)))) {
            error_report("db2010: unsupported scoped NOR probes"); exit(EXIT_FAILURE);
        }
        n->probes=true; n->retry=qdict_get_try_bool(p,"x16_retry",false);
    }
}

static unsigned block_at(uint32_t off)
{
    return off < 0x1fe0000 ? off/0x20000 : 255+(off-0x1fe0000)/0x8000;
}

static uint32_t block_start(unsigned block)
{
    return block<255 ? block*0x20000 : 0x1fe0000+(block-255)*0x8000;
}

static bool protected(DB2010Nor *n, unsigned b)
{
    return n->locked[b] || (n->down[b] && !n->wp_high);
}

bool db2010_nor_probe_view(DB2010Nor *n, uint32_t addr, unsigned size)
{
    uint32_t pc = n->env->regs[15];
    return n->probes && size==1 && n->part[(addr-BASE)/PART_SIZE].mode==DBNOR_ARRAY &&
           ((pc==0x4c002c32 && addr==0x44000055) || (pc==0x4c002c3a && addr==0x44000555));
}

static bool overlay_overlap(DB2010Nor *n, uint32_t address, unsigned size)
{
    for (unsigned k = 0; k < n->overlay_count; k++) {
        if ((uint64_t)address < (uint64_t)n->overlay[k].address + 20 &&
            (uint64_t)address + size > n->overlay[k].address) { return true; }
    }
    return false;
}

bool db2010_nor_overlay_view(DB2010Nor *n, uint32_t address, unsigned size)
{
    return n->overlay_count && address >= BASE && (uint64_t)address + size <= BASE + SIZE &&
           n->part[(address-BASE)/PART_SIZE].mode == DBNOR_ARRAY &&
           overlay_overlap(n, address, size);
}

typedef enum DB2010NorEventKind { DBNOR_READ_BYTE, DBNOR_WRITE } DB2010NorEventKind;
typedef struct DB2010NorEvent {
    DB2010NorEventKind kind;
    uint32_t address, off, value, pc;
    unsigned size, position, block;
    uint32_t *output;
    bool commit;
} DB2010NorEvent;
static const char *dbnor_dispatch(DB2010Nor *n, const DB2010NorEvent *e);

const char *db2010_nor_read(DB2010Nor *n, uint32_t address, unsigned size,
                           uint32_t *value, uint32_t *fault_address)
{
    uint32_t off=address-BASE, pc=n->env->regs[15];
    unsigned i;
    *value=0; *fault_address=address;
    if (address<BASE || (uint64_t)off+size>SIZE) { return "nor_out_of_range"; }
    if ((size!=1 && size!=2 && size!=4) ||
        (address%size && !(n->unaligned_array_halfwords && size==2))) {
        return "nor_unsupported_read_width_or_alignment";
    }
    if (address%size) {
        for (i=0; i<size; i++) {
            if (n->part[(off+i)/PART_SIZE].mode != DBNOR_ARRAY) {
                return "nor_unaligned_nonarray_read_unmodeled";
            }
        }
    }
    if (db2010_nor_overlay_view(n, address, size)) {
        for (unsigned k = 0; k < n->overlay_count; k++) {
            DB2010NorOverlay *o = &n->overlay[k];
            if ((uint64_t)address >= (uint64_t)o->address + 20 ||
                (uint64_t)address + size <= o->address) { continue; }
            for (unsigned j = 0; j < 20; j++) {
                unsigned at = o->address - BASE + j;
                if (!n->known[at] || n->array[at] != o->expected[j]) {
                    *fault_address = BASE + at; return "nor_overlay_input_mismatch";
                }
            }
        }
    }
    for (i=0;i<size;i++) {
        uint32_t byte;
        *fault_address=BASE+off+i;
        DB2010NorEvent e = {.kind=DBNOR_READ_BYTE, .address=address, .size=size,
                            .pc=pc, .position=off+i, .output=&byte};
        const char *fault = dbnor_dispatch(n, &e);
        if (fault) { return fault; }
        *value |= byte<<(8*i);
    }
    return NULL;
}

static const char *program(DB2010Nor *n, DB2010NorPart *p, bool commit)
{
    unsigned i;
    if (protected(n,p->block)) { p->status|=0x12; }
    else {
        for (i=0;i<p->used;i++) {
            uint32_t off=p->word_address[i]-BASE;
            if (overlay_overlap(n, p->word_address[i], 2)) { return "nor_overlay_program_unmodeled"; }
            uint16_t old;
            if (!n->known[off] || !n->known[off+1]) { return "nor_unknown_array_before_program"; }
            old=n->array[off] | (n->array[off+1]<<8);
            if ((old & p->word_value[i]) != p->word_value[i]) {
                p->status|=0x10; p->mode=DBNOR_STATUS; n->operations++; return NULL;
            }
        }
        if (commit) {
            for (i=0;i<p->used;i++) {
                uint32_t off=p->word_address[i]-BASE;
                n->array[off]=p->word_value[i]; n->array[off+1]=p->word_value[i]>>8;
            }
        }
    }
    p->mode=DBNOR_STATUS; n->operations++;
    return NULL;
}

static const char *dbnor_read_array(DB2010Nor *n, const DB2010NorEvent *e)
{
    unsigned pos=e->position;
    uint32_t address=e->address;
    unsigned size=e->size;
    uint32_t word=0;
    if (!n->known[pos]) {
        if (db2010_nor_probe_view(n,address,size)) { word=255; }
        else { return "nor_read_unknown_array"; }
    } else {
        word=n->array[pos];
        for (unsigned k = 0; k < n->overlay_count; k++) {
            DB2010NorOverlay *o = &n->overlay[k];
            if (BASE + pos >= o->address && BASE + pos < o->address + 20) {
                word = o->value[BASE + pos - o->address]; break;
            }
        }
    }
    *e->output=word; return NULL;
}
static const char *dbnor_read_identifier(DB2010Nor *n, const DB2010NorEvent *e)
{
    unsigned block=block_at(e->position);
    unsigned rel=(e->position%PART_SIZE)/2;
    unsigned lock_rel=(e->position-block_start(block))/2;
    uint32_t word=0;
    if (lock_rel==2) { word=n->locked[block] | (n->down[block]<<1); }
    else if (rel==0) { word=n->manufacturer; }
    else if (rel==1) { word=n->device; }
    else if (rel==5 && n->has_rcr) { word=n->rcr; }
    else if (n->id_cfi && rel>=0x10 && rel<=0x34) { word=n->cfi[rel]; }
    else if (n->has_user_protection && rel>=0x85 && rel<=0x88) {
        word=n->user_protection >> (16*(rel-0x85));
    }
    else if (n->has_protection_lock0 && rel==0x80) {
        if (!n->protection_lock0_known) { return "nor_protection_lock0_unknown"; }
        word=n->protection_lock0;
    }
    else if (n->has_unique && rel>=0x81 && rel<=0x84) { word=n->unique>>(16*(rel-0x81)); }
    else { return "nor_unmodeled_id_offset"; }
    *e->output=word; return NULL;
}
static const char *dbnor_read_query(DB2010Nor *n, const DB2010NorEvent *e)
{
    unsigned pos=e->position;
    unsigned rel=(e->position%PART_SIZE)/2;
    uint32_t address=e->address;
    uint32_t pc=e->pc;
    unsigned size=e->size;
    uint32_t word=0;
    if (pos&1) { word=0; }
    else if (rel<512 && n->cfi_known[rel]) { word=n->cfi[rel]; }
    else if (n->retry && size==1 &&
        ((pc==0x4c002c44 && address==0x44000010 && n->env->regs[0]==1 && n->env->regs[3]==1) ||
         (pc==0x4c002c32 && address==0x440000aa && n->env->regs[0]==2 && n->env->regs[3]==2) ||
         (pc==0x4c002c3a && address==0x44000aaa && n->env->regs[0]==2 && n->env->regs[3]==2))) {
        word=address==0x44000010 ? 0 : 255;
    } else { return "nor_unmodeled_cfi_offset"; }
    *e->output=word; return NULL;
}
static const char *dbnor_read_partition(DB2010Nor *n, const DB2010NorEvent *e)
{
    DB2010NorPart *p=&n->part[e->position/PART_SIZE];
    const char *fault;
    switch (p->mode) {
    case DBNOR_ARRAY: return dbnor_read_array(n, e);
    case DBNOR_STATUS: *e->output=p->status; break;
    case DBNOR_ID:
        fault=dbnor_read_identifier(n, e); if (fault) { return fault; } break;
    case DBNOR_CFI:
    default:
        fault=dbnor_read_query(n, e); if (fault) { return fault; } break;
    }
    *e->output=(*e->output>>(8*(e->position&1)))&255;
    return NULL;
}

static const char *dbnor_pending_lock(DB2010Nor *n, DB2010NorPart *p, const DB2010NorEvent *e)
{
    uint32_t address=e->address;
    uint32_t off=e->off;
    uint32_t value=e->value;
    unsigned block=e->block;
    if (value==3 && n->has_rcr) {
        if (address!=p->address) { return "nor_rcr_address_sequence_unmodeled"; }
        if (!valid_rcr((off>>1)&0xffff)) { return "nor_rcr_encoding_unmodeled"; }
        n->rcr=(off>>1)&0xffff; p->mode=DBNOR_ARRAY;
    } else if (value==1) { n->locked[block]=true; }
    else if (value==0x2f) { n->locked[block]=n->down[block]=true; }
    else if (value==0xd0) { if (!n->down[block] || n->wp_high) { n->locked[block]=false; } }
    else { return "nor_unsupported_lock_confirm"; }
    n->operations++; p->pending=DBNOR_NONE; return NULL;
}

static const char *dbnor_pending_erase(DB2010Nor *n, DB2010NorPart *p, const DB2010NorEvent *e)
{
    uint32_t value=e->value;
    unsigned block=e->block;
    bool commit=e->commit;
    if (value!=0xd0) { return "nor_unsupported_erase_confirm"; }
    if (protected(n,block)) { p->status|=0x22; }
    else if (overlay_overlap(n, BASE + block_start(block), block<255 ? 0x20000 : 0x8000)) {
        return "nor_overlay_erase_unmodeled";
    } else if (commit) {
        uint32_t start=block_start(block), length=block<255 ? 0x20000 : 0x8000;
        memset(n->array+start,255,length); memset(n->known+start,1,length);
    }
    n->operations++; p->mode=DBNOR_STATUS; p->pending=DBNOR_NONE; return NULL;
}

static const char *dbnor_pending_word(DB2010Nor *n, DB2010NorPart *p, const DB2010NorEvent *e)
{
    uint32_t address=e->address;
    uint32_t value=e->value;
    bool commit=e->commit;
    const char *fault;
    if (address!=p->address) { return "nor_word_program_address_mismatch"; }
    p->used=1; p->word_address[0]=address; p->word_value[0]=value;
    fault=program(n,p,commit); if (fault) { return fault; }
    p->pending=DBNOR_NONE; return NULL;
}

static const char *dbnor_pending_buffer_count(DB2010Nor *n, DB2010NorPart *p, const DB2010NorEvent *e)
{
    uint32_t address=e->address;
    uint32_t value=e->value;
    if (address!=p->address || value>=32) { return "nor_invalid_buffer_count"; }
    p->count=value+1; p->used=0; p->pending=DBNOR_BUFFER_DATA; return NULL;
}

static const char *dbnor_pending_buffer_data(DB2010Nor *n, DB2010NorPart *p, const DB2010NorEvent *e)
{
    uint32_t address=e->address;
    uint32_t value=e->value;
    if (p->used && address!=p->word_address[p->used-1]+2) { return "nor_noncontiguous_buffer_data"; }
    if (p->used && address/64!=p->word_address[0]/64) { return "nor_buffer_boundary_crossed"; }
    p->word_address[p->used]=address; p->word_value[p->used++]=value;
    if (p->used==p->count) { p->pending=DBNOR_BUFFER_CONFIRM; } return NULL;
}

static const char *dbnor_pending_buffer_confirm(DB2010Nor *n, DB2010NorPart *p, const DB2010NorEvent *e)
{
    uint32_t value=e->value;
    bool commit=e->commit;
    const char *fault;
    if (value!=0xd0) { return "nor_unsupported_buffer_confirm"; }
    fault=program(n,p,commit); if (fault) { return fault; }
    p->pending=DBNOR_NONE; return NULL;
}

static const char *dbnor_command(DB2010Nor *n, DB2010NorPart *p, const DB2010NorEvent *e)
{
    uint32_t address=e->address;
    uint32_t value=e->value;
    unsigned block=e->block;
    switch (value) {
    case 0xff: p->mode=DBNOR_ARRAY; break;
    case 0x70: p->mode=DBNOR_STATUS; break;
    case 0x90: p->mode=DBNOR_ID; break;
    case 0x98: p->mode=DBNOR_CFI; break;
    case 0x50: p->status=0x80; break;
    case 0x60: p->pending=DBNOR_LOCK; break;
    case 0x20: p->pending=DBNOR_ERASE; p->mode=DBNOR_STATUS; break;
    case 0x40: case 0x10: p->pending=DBNOR_WORD; p->mode=DBNOR_STATUS; break;
    case 0xe8: p->pending=DBNOR_BUFFER_COUNT; p->mode=DBNOR_STATUS; break;
    default: return "nor_unsupported_command";
    }
    if (p->pending!=DBNOR_NONE) { p->block=block; p->address=address; }
    return NULL;
}
static const char *dbnor_dispatch(DB2010Nor *n, const DB2010NorEvent *e)
{
    if (e->kind == DBNOR_READ_BYTE) { return dbnor_read_partition(n, e); }
    DB2010NorPart *p=&n->part[e->off/PART_SIZE];
    if (p->pending!=DBNOR_NONE) {
        if (e->block!=p->block) { return "nor_sequence_crosses_block"; }
        switch (p->pending) {
        case DBNOR_LOCK: return dbnor_pending_lock(n, p, e);
        case DBNOR_ERASE: return dbnor_pending_erase(n, p, e);
        case DBNOR_WORD: return dbnor_pending_word(n, p, e);
        case DBNOR_BUFFER_COUNT: return dbnor_pending_buffer_count(n, p, e);
        case DBNOR_BUFFER_DATA: return dbnor_pending_buffer_data(n, p, e);
        case DBNOR_BUFFER_CONFIRM: return dbnor_pending_buffer_confirm(n, p, e);
        default: g_assert_not_reached();
        }
    }
    return dbnor_command(n, p, e);
}

const char *db2010_nor_write(DB2010Nor *n, uint32_t address, unsigned size,
                            uint32_t value, bool commit)
{
    uint32_t off=address-BASE;
    DB2010NorPart *p;
    unsigned block;
    if (address<BASE || (uint64_t)off+size>SIZE) { return "nor_out_of_range"; }
    p=&n->part[off/PART_SIZE]; block=block_at(off);
    if (size==1 && n->bridge) {
        if (p->pending!=DBNOR_NONE) { return "nor_bridge_byte_pending_sequence"; }
        value&=255;
        if (value!=0x98 && value!=0xff) { return "nor_bridge_byte_command_unmodeled"; }
        address&=~1; off=address-BASE;
    } else if (size!=2 || (off&1)) { return "nor_unsupported_write_width_or_alignment"; }
    value&=0xffff;
    if (n->mirrored && (p->pending==DBNOR_NONE || p->pending==DBNOR_LOCK ||
        p->pending==DBNOR_ERASE || p->pending==DBNOR_BUFFER_CONFIRM) && value>>8==(value&255)) { value&=255; }
    DB2010NorEvent e = {.kind=DBNOR_WRITE, .address=address, .off=off,
                        .value=value, .block=block, .commit=commit};
    const char *fault = dbnor_dispatch(n, &e);
    if (fault) { return fault; }
    n->commands++;
    return NULL;
}

static uint64_t native_read(void *opaque, hwaddr offset, unsigned size)
{
    DB2010Nor *n=opaque;
    uint32_t value, fault_address;
    const char *fault = db2010_nor_read(n,BASE+offset,size,&value,&fault_address);
    if (fault) {
        if (!n->research_guards) {
            n->env->board_hooks->io_access(n->env->board_hooks->opaque, n->env,
                BASE+offset, size, false, 0, 0);
        }
        return 0;
    }
    return value;
}

void db2010_nor_overlay_observe(DB2010Nor *n, uint32_t address, unsigned size)
{
    if (!db2010_nor_overlay_view(n, address, size)) { return; }
    n->overlay_reads++;
    for (unsigned i = 0; i < size; i++) {
        n->overlay_bytes += overlay_overlap(n, address+i, 1);
    }
}

static uint64_t overlay_read(void *opaque, hwaddr offset, unsigned size)
{
    DB2010Nor *n = opaque; uint32_t value = 0, byte, fault;
    /* Byte assembly also handles debugger/bus splits. Original guest
     * width/alignment is checked before the transaction by the guard. */
    for (unsigned i = 0; i < size; i++) {
        if (db2010_nor_read(n, 0x45f00000u+offset+i, 1, &byte, &fault)) { return 0; }
        value |= byte << (8*i);
    }
    return value;
}

static void native_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    DB2010Nor *n=opaque;
    unsigned i;
    bool array=true;
    const char *fault=db2010_nor_write(n,BASE+offset,size,value,true);
    /* All guest stores were previewed before reaching the native bus. */
    g_assert(!fault);
    for (i=0;i<16;i++) { array &= n->part[i].mode==DBNOR_ARRAY; }
    memory_region_rom_device_set_romd(n->mr,array);
    memory_region_set_dirty(n->mr,0,SIZE);
}

static void overlay_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    DB2010Nor *n = opaque;
    native_write(n, 0x1f00000u+offset, value, size);
}

static const MemoryRegionOps overlay_ops = {
    .read=overlay_read, .write=overlay_write, .endianness=DEVICE_LITTLE_ENDIAN,
    .valid={ .min_access_size=1, .max_access_size=4, .unaligned=true },
    .impl={ .min_access_size=1, .max_access_size=4, .unaligned=true },
};

static const MemoryRegionOps ops = {
    .read=native_read, .write=native_write, .endianness=DEVICE_LITTLE_ENDIAN,
    .valid={ .min_access_size=1, .max_access_size=4, .unaligned=true },
    .impl={ .min_access_size=1, .max_access_size=4, .unaligned=true },
};

void db2010_nor_init(DB2010Nor *n, MemoryRegion *mr, uint8_t *known, const char *name, Object *owner)
{
    n->mr=mr; n->known=known;
    memory_region_init_rom_device(mr,owner,&ops,n,name,SIZE,&error_fatal);
    n->array=memory_region_get_ram_ptr(mr);
    if (n->overlay_count) {
        /* A page-aligned bank view avoids a ROMD fast-path read beginning
         * just outside a tiny overlay and spanning its first bytes. The
         * read helper substitutes only the declared20-byte windows. */
        memory_region_init_io(&n->overlay_mr, owner, &overlay_ops, n,
                              "db2010.gdfs-digest-overlay", 0x100000);
        memory_region_add_subregion_overlap(mr, 0x1f00000, &n->overlay_mr, 1);
    }
}

typedef struct DBNorReadWindow {
    MemoryRegion mr;
    DB2010Nor *nor;
    uint32_t offset;
} DBNorReadWindow;
static uint64_t unknown_read(void *opaque, hwaddr offset, unsigned size)
{
    DBNorReadWindow *w = opaque;
    return native_read(w->nor, w->offset + offset, size);
}
static void unknown_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    DBNorReadWindow *w = opaque;
    native_write(w->nor, w->offset + offset, value, size);
}
static const MemoryRegionOps unknown_ops = {
    .read = unknown_read, .write = unknown_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 1, .max_access_size = 4, .unaligned = true},
    .impl = {.min_access_size = 1, .max_access_size = 4, .unaligned = true},
};
void db2010_nor_map_unknown(DB2010Nor *n, Object *owner)
{
    /* Fast CPU RAM guards are absent. Keep device-level storage coverage checks
     * on pages with any unsupplied flash bytes; fully supplied pages retain ROMD.
     * These immutable views may remain after erase/program establishes bytes. */
    unsigned runs = 0;
    bool previous = false;
    for (unsigned off = 0; off < SIZE; off += 4096) {
        bool unknown = memchr(n->known + off, 0, 4096) != NULL;
        runs += unknown && !previous;
        previous = unknown;
    }
    n->unknown_windows = g_array_sized_new(false, true, sizeof(DBNorReadWindow), runs);
    g_array_set_size(n->unknown_windows, runs);
    unsigned index = 0;
    for (unsigned off = 0; off < SIZE;) {
        if (!memchr(n->known + off, 0, 4096)) { off += 4096; continue; }
        unsigned start = off;
        do { off += 4096; } while (off < SIZE && memchr(n->known + off, 0, 4096));
        DBNorReadWindow *w = &g_array_index(n->unknown_windows, DBNorReadWindow, index++);
        w->nor = n; w->offset = start;
        g_autofree char *name = g_strdup_printf("db2010.nor-unsupplied-%x", start);
        memory_region_init_io(&w->mr, owner, &unknown_ops, w, name, off-start);
        memory_region_add_subregion_overlap(n->mr, start, &w->mr, 0);
    }
}

void db2010_nor_reset(DB2010Nor *n)
{
    unsigned i;
    memset(n->part,0,sizeof(n->part));
    memset(n->locked,n->initial_locked,sizeof(n->locked));
    memset(n->down,0,sizeof(n->down));
    for (i=0;i<16;i++) { n->part[i].status=0x80; }
    n->rcr=n->initial_rcr; n->commands=n->operations=0;
    n->overlay_reads=n->overlay_bytes=0;
    memory_region_rom_device_set_romd(n->mr,true);
}

QDict *db2010_nor_snapshot(DB2010Nor *n)
{
    QDict *result=qdict_new();
    QList *parts=qlist_new(), *locks=qlist_new();
    unsigned i;
    for (i=0;i<16;i++) {
        QDict *p=qdict_new();
        qdict_put_int(p,"mode",n->part[i].mode);
        qdict_put_int(p,"status",n->part[i].status);
        qdict_put_int(p,"pending",n->part[i].pending);
        qdict_put_int(p,"address",n->part[i].address);
        qdict_put_int(p,"buffer_words",n->part[i].used);
        qlist_append(parts,p);
    }
    for (i=0;i<259;i++) {
        qlist_append_int(locks,n->locked[i] | (n->down[i]<<1));
    }
    if (n->unaligned_array_halfwords) {
        qdict_put_str(result,"unaligned_array_reads","halfword-byte-assembly");
    }
    if (n->has_user_protection) {
        char value[17]; snprintf(value,sizeof(value),"%016" PRIx64,n->user_protection);
        qdict_put_str(result,"user_protection64",value);
    }
    if (n->has_protection_lock0) {
        if (n->protection_lock0_known) { qdict_put_int(result,"protection_lock0",n->protection_lock0); }
        else { qdict_put_null(result,"protection_lock0"); }
    }
    if (n->overlay_count) {
        QDict *overlay = qdict_new(); QList *entries = qlist_new();
        qdict_put_str(overlay, "profile", "gdfs-digest-bytes");
        qdict_put_bool(overlay, "diagnostic_substitution", true);
        qdict_put_int(overlay, "guest_reads", n->overlay_reads);
        qdict_put_int(overlay, "substituted_bytes", n->overlay_bytes);
        for (unsigned k = 0; k < n->overlay_count; k++) {
            DB2010NorOverlay *o = &n->overlay[k]; QDict *entry = qdict_new();
            char expected[41], value[41], backing[41];
            for (unsigned j = 0; j < 20; j++) {
                snprintf(expected+j*2, 3, "%02x", o->expected[j]);
                snprintf(value+j*2, 3, "%02x", o->value[j]);
                snprintf(backing+j*2, 3, "%02x", n->array[o->address-BASE+j]);
            }
            qdict_put_int(entry, "address", o->address);
            qdict_put_str(entry, "expected_hex", expected); qdict_put_str(entry, "value_hex", value);
            qdict_put_str(entry, "backing_hex", backing);
            qlist_append(entries, entry);
        }
        qdict_put(overlay, "entries", entries); qdict_put(result, "diagnostic_read_overlay", overlay);
    }
    qdict_put(result,"partitions",parts); qdict_put(result,"locks",locks);
    qdict_put_int(result,"rcr",n->rcr);
    qdict_put_int(result,"commands",n->commands); qdict_put_int(result,"operations",n->operations);
    return result;
}
