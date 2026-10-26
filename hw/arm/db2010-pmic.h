/* Explicit configured PMIC peer; no analog effects. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_PMIC_H
#define DB2010_PMIC_H
#include <stdbool.h>
#include <stdint.h>
typedef struct DB2010I2c DB2010I2c;
typedef struct DB2010PmicConfig {
    bool power_known;
    bool capture_writes;
    bool capture_async_power;
    bool complete_power_init;
    bool read_power_status;
    bool status_input_known[2];
    uint8_t status_input[2];
    bool startup_register_reads;
    bool polled_startup_adc;
    bool polled_adc_known;
    uint8_t polled_adc_value;
    bool sampled_battery_adc;
    bool sampled_battery_known;
    uint8_t sampled_battery_value;
    unsigned register_read_count;
    uint8_t read_reg[73];
    uint8_t read_value[73];
    uint8_t read_kind[73];
    unsigned followup_count;
    bool repeat_followups;
    unsigned followup_repeat_from;
    uint8_t followup_reg[32];
    uint8_t followup_value[32];
    bool followup_read[32];
    bool followup_known[32];
    unsigned extra_write_count;
    bool control_b1_write_latch;
    bool control45_write_latch;
    bool control36_37_write_latches;
    bool vibrator_write_latch;
    bool audio_write_latch[16];
    uint8_t extra_write_reg[128];
    uint8_t extra_write_value[128];
    unsigned extra_read_count;
    uint8_t extra_read_reg[32];
    uint8_t extra_read_value[32];
    uint8_t extra_read_kind[32];
    uint32_t power_value;
    bool bounded_history;
    bool omit_diagnostics;
} DB2010PmicConfig;
/* These are independent dimensions; the snapshots still expose booleans. */
typedef enum DB2010PmicSelection {
    DBPMIC_ORDERED = 0, DBPMIC_INDEPENDENT = 1,
} DB2010PmicSelection;
typedef enum DB2010PmicDirection {
    DBPMIC_WRITE = 0, DBPMIC_READ = 1,
} DB2010PmicDirection;
typedef enum DB2010PmicReadAck {
    DBPMIC_FINAL_NACK = 0, DBPMIC_MORE_ACK = 1,
} DB2010PmicReadAck;
typedef struct DB2010PmicRuntime {
    unsigned status_reads;
    uint64_t register_reads;
    uint64_t followup_cursor;
    uint8_t extra_reg;
    DB2010PmicSelection extra_active;
    DB2010PmicDirection extra_read;
    DB2010PmicReadAck extra_receive_more;
    uint64_t extra_reads;
    uint64_t extra_writes;
    uint64_t power_writes;
    uint64_t write_count;
    uint8_t write_reg[128];
    uint8_t write_value[128];
    uint8_t last_value[256];
    bool last_known[256];
} DB2010PmicRuntime;
typedef struct DB2010Pmic {
    DB2010PmicConfig config;
    DB2010PmicRuntime state;
    uint64_t history_baseline; /* Host capture session, excluded from migration. */
} DB2010Pmic;
void dbpmic_reset(DB2010Pmic *pmic);
/* Polled peer decisions leave wire phase, staging and timing to transport. */
const char *dbpmic_polled_read(DB2010I2c *t);
const char *dbpmic_polled_write(DB2010I2c *t, int byte);
const char *dbpmic_async_access(DB2010I2c *transport, unsigned off, uint64_t now, uint32_t *value);
void dbpmic_async_finish(DB2010I2c *transport);
const char *dbfr_transport(DB2010I2c *transport, unsigned off, uint64_t now, uint32_t *value, bool *handled);
#endif
