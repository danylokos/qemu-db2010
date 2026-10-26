/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-fa-mailbox.h"

typedef enum DB2010MailboxEventKind {
    DBFM_STATUS, DBFM_STAGE, DBFM_SUBMIT, DBFM_CONSUMED,
} DB2010MailboxEventKind;
typedef struct DB2010MailboxEvent {
    DB2010MailboxEventKind kind;
    unsigned element;
    DB2010Access *access;
} DB2010MailboxEvent;
static const char *dbfm_submit_empty(DB2010FaMailbox *m, const DB2010MailboxEvent *e)
{
    DB2010Access *a = e->access;
    if (!(m->known & 1)) { return "fa_mailbox_header_unknown"; }
    unsigned count = m->words[0] >> 11;
    if (!count || count > 31 || (m->words[0] & 0x7e0)) {
        return "fa_mailbox_header_unmodeled";
    }
    uint32_t required = count == 31 ? UINT32_MAX : (1u << (count + 1)) - 1;
    if ((m->known & required) != required) { return "fa_mailbox_payload_unknown"; }
    if (a->commit) {
        m->busy = DBFM_OCCUPIED; m->payload_words = count; m->submissions++;
    }
    return NULL;
}
static const char *dbfm_stage_empty(DB2010FaMailbox *m, const DB2010MailboxEvent *e)
{
    DB2010Access *a = e->access;
    if (a->commit) {
        m->words[e->element] = a->value;
        m->known |= 1u << e->element;
    }
    return NULL;
}
static const char *dbfm_dispatch(DB2010FaMailbox *m, const DB2010MailboxEvent *e)
{
    if (e->kind == DBFM_STATUS) { e->access->value = m->busy; return NULL; }
    if (e->kind == DBFM_CONSUMED) { m->busy = DBFM_EMPTY; return NULL; }
    if (e->kind == DBFM_SUBMIT && e->access->value != 1) { return "fa_mailbox_command_unmodeled"; }
    switch (m->busy) {
    case DBFM_OCCUPIED: return "fa_mailbox_peer_consumption_unmodeled";
    case DBFM_EMPTY:
        switch (e->kind) {
        case DBFM_STAGE: return dbfm_stage_empty(m, e);
        case DBFM_SUBMIT: return dbfm_submit_empty(m, e);
        default: g_assert_not_reached();
        }
    default: g_assert_not_reached();
    }
}
#define MAILBOX_ADAPTER(name, event) \
static const char *name(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a) \
{ \
    DB2010MailboxEvent e = {.kind = event, .element = d->element, .access = a}; \
    return dbfm_dispatch(opaque, &e); \
}
MAILBOX_ADAPTER(dbfm_status, DBFM_STATUS)
MAILBOX_ADAPTER(dbfm_stage, DBFM_STAGE)
MAILBOX_ADAPTER(dbfm_submit, DBFM_SUBMIT)
#undef MAILBOX_ADAPTER

const DB2010RegMap *dbfm_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_REG(0, .id = DBFM_COMMAND_STATUS, .alignment = 4,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32,
                     .callback = dbfm_status},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH32,
                      .callback = dbfm_submit}),
        DB_ARRAY(0x40, 32, .id = DBFM_TX_WORDS, .stride = 2, .alignment = 2,
            .backing_width = 2,
            .read = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH16,
                     .fault = "fa_mailbox_tx_buffer_read_unmodeled"},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16,
                      .callback = dbfm_stage}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xfa040000, rules,
            ARRAY_SIZE(rules), "fa_mailbox_access_unmodeled");
        if (fault) { g_error("DB2010 mailbox layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}

bool dbfm_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = dbfm_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}

const char *dbfm_access(DB2010FaMailbox *m, uint32_t address,
                        unsigned size, bool write, uint32_t *value, bool apply)
{
    DB2010Access a = {.address = address, .size = size, .write = write,
                      .value = *value, .commit = apply};
    DB2010DecodedAccess decoded;
    const char *fault = db2010_reg_decode(dbfm_map(), &a, &decoded);
    if (!fault) { fault = db2010_reg_execute(m, &decoded, &a); }
    *value = a.value;
    return fault;
}

QDict *dbfm_snapshot(DB2010FaMailbox *m)
{
    QDict *r = qdict_new();
    qdict_put_str(r, "profile", "outbound-single-slot");
    qdict_put_bool(r, "peer_consumption_modeled", false);
    qdict_put_bool(r, "busy", m->busy);
    qdict_put_int(r, "known_halfword_mask", m->known);
    qdict_put_int(r, "submissions", m->submissions);
    qdict_put_int(r, "payload_halfwords", m->payload_words);
    GString *hex = g_string_new(NULL);
    if (m->submissions) {
        for (unsigned i = 0; i <= m->payload_words; i++) {
            g_string_append_printf(hex, "%02x%02x", m->words[i] & 255, m->words[i] >> 8);
        }
    }
    qdict_put_str(r, "submitted_packet_hex", hex->str);
    g_string_free(hex, true);
    return r;
}

void dbfm_consumed(DB2010FaMailbox *mailbox)
{
    DB2010MailboxEvent e = {.kind = DBFM_CONSUMED};
    dbfm_dispatch(mailbox, &e);
}

void dbfm_reset(DB2010FaMailbox *mailbox) { memset(mailbox, 0, sizeof(*mailbox)); }
