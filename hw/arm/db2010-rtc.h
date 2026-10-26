/* Retained calendar/RTC clock hypothesis; no host wall-time dependency. */
#ifndef HW_ARM_DB2010_RTC_H
#define HW_ARM_DB2010_RTC_H

#include "db2010-contract.h"
#include "db2010-time.h"
#include "db2010-links.h"
const DB2010RegMap *dbrtc_map(void);
const DB2010RegMap *dbrtc_irq_map(void);

typedef struct DB2010RtcConfig {
    uint32_t initial_control;
    uint32_t status;
    uint32_t initial_ticks;
    bool calendar_writable;
    bool minute_irq;
} DB2010RtcConfig;
typedef struct DB2010RtcRuntime {
    DB2010ClockPhase phase;
    uint32_t control;
    bool control_written;
    int64_t calendar_adjust_seconds;
    int64_t counter_adjust_ticks;
    uint16_t minute_control, minute_status;
    uint64_t minute_next_ticks, minute_events;
} DB2010RtcRuntime;
typedef struct DB2010Rtc {
    Clock *clock;
    DB2010Signal irq;
    DB2010RtcConfig config;
    DB2010RtcRuntime state;
} DB2010Rtc;

bool dbrtc_contains(uint32_t address, unsigned size);
bool dbrtc_irq_contains(uint32_t address, unsigned size);
const char *dbrtc_advance(DB2010Rtc *t, uint64_t now);
int64_t dbrtc_deadline(DB2010Rtc *t);
void dbrtc_update_irq(DB2010Rtc *t);

const char *dbrtc_access(DB2010Rtc *t, uint64_t now,
                    uint64_t idle, uint32_t address, unsigned size,
                    bool write, uint32_t *value);

void dbrtc_reset(DB2010Rtc *d);
typedef struct QDict QDict;
typedef struct QList QList;
void dbrtc_export(DB2010Rtc *model, QDict *snapshot, uint64_t now, uint64_t idle);
#endif
