/* Declared FM-radio transmit frames; no tuner, read data or RF simulation. */
#ifndef HW_ARM_DB2010_FM_RADIO_H
#define HW_ARM_DB2010_FM_RADIO_H
#include <stdbool.h>
#include <stdint.h>
#define DBFR_MAX_FRAMES 16
#define DBFR_MAX_BYTES 32
typedef struct DB2010FmRadioConfig {
    bool enabled;
    unsigned count;
    unsigned size[DBFR_MAX_FRAMES];
    uint8_t allowed[DBFR_MAX_FRAMES][DBFR_MAX_BYTES];
} DB2010FmRadioConfig;
typedef enum DB2010FmRadioPhase {
    DBFR_IDLE = 0,
    DBFR_SELECTED = 1,
} DB2010FmRadioPhase;
typedef struct DB2010FmRadioRuntime {
    DB2010FmRadioPhase active; /* Existing snapshot exports this as a boolean. */
    unsigned used;
    unsigned latest_size;
    uint8_t pending[DBFR_MAX_BYTES];
    uint8_t latest[DBFR_MAX_BYTES];
    uint64_t frames;
    uint64_t bytes;
} DB2010FmRadioRuntime;
typedef struct DB2010FmRadio {
    DB2010FmRadioConfig config;
    DB2010FmRadioRuntime state;
} DB2010FmRadio;

bool dbfr_byte_known(const DB2010FmRadio *f, uint8_t value);

bool dbfr_frame_known(const DB2010FmRadio *f);

void dbfr_complete(DB2010FmRadio *f);

void dbfr_reset(DB2010FmRadio *d);
#endif
