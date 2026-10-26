/* Recovered byte protocol and two-instruction completion assumption. */
#ifndef HW_ARM_DB2010_I2C_H
#define HW_ARM_DB2010_I2C_H
#include <stdbool.h>
#include <stdint.h>
#include "db2010-fm-radio.h"
#include "db2010-pmic.h"
#include "db2010-bus.h"
const DB2010RegMap *dbi_map(void);
const char *dbi_bus_access(void *opaque, DB2010Access *a);
/* Explicit values preserve snapshot phases and pending-operation numbers. */
typedef enum DB2010I2cPhase {
    DBI_UNINITIALIZED = 0, DBI_CONFIGURED = 1, DBI_IDLE = 2,
    DBI_WRITE_ADDRESS = 3, DBI_REGISTER = 4, DBI_SELECTED = 5,
    DBI_READ_ADDRESS = 6, DBI_RECEIVE_READY = 7, DBI_RECEIVED = 8,
    DBI_READ_COMPLETE = 9, DBI_WRITE_COMPLETE = 10,
} DB2010I2cPhase;
typedef enum DB2010I2cOperation {
    DBI_NONE = 0, DBI_START = 1, DBI_TRANSMIT = 2,
    DBI_RECEIVE = 3, DBI_STOP = 4,
} DB2010I2cOperation;
typedef enum DB2010I2cInitStep {
    DBI_INIT_POLLED = 0, DBI_INIT_CONTROL = 1,
    DBI_INIT_CLOCK = 2, DBI_INIT_READY = 3,
} DB2010I2cInitStep;
typedef enum DB2010I2cAsyncPhase {
    DBI_ASYNC_START_READY = 0, DBI_ASYNC_START_PENDING = 1,
    DBI_ASYNC_BYTE_READY = 2, DBI_ASYNC_BYTE_PENDING = 3,
    DBI_ASYNC_STOP_READY = 4, DBI_ASYNC_STOP_PENDING = 5,
    DBI_ASYNC_PEER_READY = 6,
} DB2010I2cAsyncPhase;
/* Shared wire progress; peer-specific transaction cursors live in children. */
typedef enum DB2010I2cPeerPhase {
    DBI_PEER_CONTROL_READY = 0, DBI_PEER_CLOCK_READY = 1,
    DBI_PEER_START_READY = 2, DBI_PEER_START_PENDING = 3,
    DBI_PEER_ADDRESS_READY = 4, DBI_PEER_ADDRESS_PENDING = 5,
    DBI_PEER_FIRST_BYTE_READY = 6, DBI_PEER_BYTE_PENDING = 7,
    DBI_PEER_DATA_READY = 8, DBI_PEER_DATA_PENDING = 9,
    DBI_PEER_STOP_READY = 10, DBI_PEER_STOP_PENDING = 11,
    DBI_PEER_RESTART_PENDING = 12, DBI_PEER_READ_ADDRESS_READY = 13,
    DBI_PEER_READ_ADDRESS_PENDING = 14, DBI_PEER_RECEIVE_READY = 15,
    DBI_PEER_RECEIVE_PENDING = 16, DBI_PEER_RECEIVED = 17,
    DBI_PEER_READ_COMPLETE = 18, DBI_PEER_READ_STOP_PENDING = 19,
} DB2010I2cPeerPhase;
typedef struct DB2010I2cConfig {
    bool capture_async_init;
    bool complete_async_init;
} DB2010I2cConfig;
typedef struct DB2010I2cRuntime {
    DB2010I2cPeerPhase peer_step;
    uint32_t control;
    uint32_t clock;
    DB2010I2cPhase phase;
    DB2010I2cOperation pending;
    uint32_t mask;
    bool control_known;
    bool clock_known;
    bool irq_pending;
    bool status_known;
    DB2010I2cInitStep async_init_step;
    DB2010I2cAsyncPhase async_phase;
    uint32_t status;
    uint64_t async_completions;
    int staged;
    int received;
    int slave;
    int reg;
    uint64_t due;
    uint64_t transactions;
    uint8_t wire[16];
    unsigned wire_size;
} DB2010I2cRuntime;
typedef struct DB2010I2c {
    DB2010I2cConfig config;
    DB2010I2cRuntime state;
    DB2010FmRadio *fm;
    DB2010Pmic *pmic;
} DB2010I2c;

/* Preserve the first64 startup records and the most recent64 thereafter.
 * Register state is independent of recording retention. */
unsigned dbi_write_slot(const DB2010I2c *t, uint64_t index);

unsigned dbi_retained_writes(const DB2010I2c *t);

uint64_t dbi_write_index(const DB2010I2c *t, unsigned ordinal);

void dbi_capture_write(DB2010I2c *t, uint8_t reg, uint8_t value);

void dbi_transport_reset(DB2010I2c *t);
void dbi_reset(DB2010I2c *t);

/* Exact opt-in register writes from independent native clients may interleave
 * with the ordered ADC sequence. They never consume its next operation. */
bool dbi_extra_write_known(const DB2010I2c *t, unsigned reg, int value);

/* Independent explicitly configured reads do not advance the ordered tail. */
int dbi_extra_read_index(const DB2010I2c *t, unsigned reg);

/* Original independent clients use only92..94 and B3..B4 bursts.
 * Each byte still needs a declared input or known last-write value. */
bool dbi_burst_has_next(unsigned reg);

unsigned dbi_followup_index(const DB2010I2c *t);

void dbi_finish(DB2010I2c *t, uint64_t now);

/* Transport requests and completions do not stage data or acknowledge IRQs. */
void dbi_request(DB2010I2c *t, DB2010I2cOperation operation,
                 unsigned mask, uint64_t now);
void dbi_complete_async_request(DB2010I2c *t);

/* Compatibility entrypoint for existing peers. */
void dbi_begin(DB2010I2c *t, unsigned operation, unsigned mask, uint64_t now);

const char *dbi_access(DB2010I2c *t, uint32_t address, unsigned size,
                                    uint64_t now, bool write, uint32_t *value);

typedef struct QDict QDict;
typedef struct QList QList;
void dbi_export(DB2010I2c *model, QDict *snapshot);
void dbi_irq_consumed(DB2010I2c *i2c);
#endif
