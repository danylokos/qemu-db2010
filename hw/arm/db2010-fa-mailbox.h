/* Outbound single-slot FA mailbox. Empty reset is an explicit hypothesis.
 * Submission occupies the slot; no DSP consumption, reply or IRQ is invented.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_DB2010_FA_MAILBOX_H
#define HW_ARM_DB2010_FA_MAILBOX_H

#include "db2010-bus.h"
typedef enum DB2010FaMailboxRule {
    DBFM_COMMAND_STATUS = 1,
    DBFM_TX_WORDS = 2,
} DB2010FaMailboxRule;
/* Immutable shared layout; runtime slot/staging state remains per instance. */
const DB2010RegMap *dbfm_map(void);
typedef struct QDict QDict;
typedef enum DB2010MailboxSlot { DBFM_EMPTY = 0, DBFM_OCCUPIED = 1 } DB2010MailboxSlot;
typedef struct DB2010FaMailbox {
    uint16_t words[32];
    uint32_t known;
    DB2010MailboxSlot busy;
    unsigned payload_words;
    uint64_t submissions;
} DB2010FaMailbox;

bool dbfm_contains(uint32_t address, unsigned size);

const char *dbfm_access(DB2010FaMailbox *m, uint32_t address,
                                unsigned size, bool write, uint32_t *value,
                                bool apply);

QDict *dbfm_snapshot(DB2010FaMailbox *m);

void dbfm_consumed(DB2010FaMailbox *mailbox);
void dbfm_reset(DB2010FaMailbox *mailbox);
#endif
