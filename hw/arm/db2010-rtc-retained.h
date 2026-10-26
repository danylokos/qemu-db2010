/* Native RTC backup loop reads/restores 25 low-byte fields at002A..0042.
 * Cold zero and byte-latch behavior are explicit hypotheses, not RTC events. */
#ifndef HW_ARM_DB2010_RTC_RETAINED_H
#define HW_ARM_DB2010_RTC_RETAINED_H
#include "db2010-contract.h"
const DB2010RegMap *dbrtr_map(void);

typedef struct DB2010RtcRetained {
    uint8_t bytes[25];
} DB2010RtcRetained;

bool dbrtr_contains(uint32_t address, unsigned size);

const char *dbrtr_access(DB2010RtcRetained *r, uint32_t address,
                                     unsigned size, bool write, uint32_t *value);

typedef struct QDict QDict;
typedef struct QList QList;
void dbrtr_export(DB2010RtcRetained *model, QDict *snapshot);
#endif
