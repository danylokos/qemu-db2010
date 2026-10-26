/* Component connections and state. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_COMPONENTS_H
#define DB2010_COMPONENTS_H
#include "db2010-links.h"
#include "db2010-keypad.h"
#include "db2010-bus.h"
#include "db2010-timer.h"
typedef struct DB2010F500Control DB2010F500Control;
typedef struct DB2010Nand DB2010Nand;
#include "db2010-dma.h"
#include "db2010-i2c.h"
#include "db2010-rtc.h"
#include "db2010-rtc-retained.h"
#include "db2010-pdi.h"
#include "db2010-panel.h"
#include "db2010-dsp-peer.h"
#include "db2010-fa-mailbox.h"

#include "chardev/char-fe.h"
#include "hw/core/cpu.h"
typedef struct DB2010Register {
    uint32_t address, width, value, initial, read_mask;
    uint32_t known_mask;
    DB2010RegRule rule;
    DB2010RegMap map;
    bool initial_known, write_only, read_only, conditional, rtc_halfword_alias;
    bool f500_lowbyte_alias;
    uint32_t condition_address, condition_value;
    GArray *allowed_writes;
    char *name;
} DB2010Register;
const DB2010RegMap *dbreg_map(DB2010Register *r);
static inline bool dbreg_known(const DB2010Register *r)
{
    return r->known_mask == UINT32_MAX >> (8 * (4 - r->width));
}
typedef struct DB2010Display DB2010Display;
typedef struct DB2010FaLink DB2010FaLink;
typedef struct DB2010Power {
    bool power_known;
    bool initial_power_known;
    uint32_t power_value;
    uint32_t initial_power;
    uint32_t power_mask;
    bool allow_startup_release;
    unsigned startup_releases;
} DB2010Power;
typedef struct DB2010Startup {
    uint8_t startup_status_initial;
    uint8_t startup_status_value;
    uint64_t startup_status_reads;
    uint64_t startup_status_clears;
} DB2010Startup;
typedef struct DB2010Uart {
    CharFrontend uart[4];
    uint64_t uart_bytes[4];
    bool bluetooth_enabled, bluetooth_error_peer, bluetooth_rx_timeout;
    uint16_t bluetooth_mask;
    uint8_t bluetooth_tx_pos, bluetooth_rx_pos, bluetooth_command;
    uint8_t bluetooth_ack_level; /* Configuration, not runtime state. */
    uint16_t bluetooth_opcode;
    uint32_t bluetooth_candidates;
    uint64_t bluetooth_commands;
    uint64_t bluetooth_resets, bluetooth_rx_bytes;
    DB2010Signal bluetooth_irq;
    CPUState *data_cpu;
    struct DB2010RegisterBank *data_bank;
    bool data_present;
    uint16_t data_presence_events;
    uint64_t data_cable_transitions;
    bool data_enabled, data_mode_known, data_timeout, data_autobaud;
    uint16_t data_mode, data_mask, data_head, data_count;
    uint8_t data_rx[256], data_training;
    uint64_t data_rx_bytes, data_received, data_autobauds;
    DB2010Signal data_irq;
} DB2010Uart;
typedef struct DB2010MemoryController {
    uint32_t controller_words[11];
    uint32_t initial_bank;
    bool controller_known[11];
} DB2010MemoryController;
typedef struct DB2010RegionController {
    bool has_section_table;
    bool table_dirty;
    uint32_t region_words[24];
    uint32_t initial_region_words[24];
    bool region_known[24];
    bool initial_region_known[24];
    uint64_t table_commits;
} DB2010RegionController;
typedef struct DB2010Absent {
    uint64_t absent_reads;
    uint64_t absent_writes;
} DB2010Absent;
typedef struct DB2010RegisterBank {
    GPtrArray *registers;
    bool has_serial, data_uart_flow;
    uint32_t serial_levels;
    bool has_memorystick_slot;
    bool has_f500;
    uint64_t f500_commands;
    DB2010Timer *timer;
} DB2010RegisterBank;
typedef struct DB2010Display {
    bool omit_diagnostics;
    bool has_pdi;
    bool display_first_burst_done;
    GByteArray *display_pixels;
    QList *display_bursts;
    QList *display_frames;
    uint64_t display_frame_count, pdi_capture_count;
    DB2010Panel *panel;
    uint64_t display_due;
    uint64_t display_started;
    uint64_t display_completed;
    uint32_t display_source;
    bool display_dma_irq_level;
    DB2010Pdi pdi;
    QList *pdi_transfers;
    DB2010Dma *dma;
    DB2010Signal pdi_irq, dma_irq, dsp_irq;
    bool has_dsp_peer;
    const DB2010Register *stream;
    DB2010MemoryPort memory;
    DB2010TimePort time;
    DB2010FaultPort faults;
} DB2010Display;
typedef struct DB2010FaLink {
    bool has_dsp_peer;
    DB2010DspPeer *dsp_peer;
    bool has_fa_mailbox;
    DB2010FaMailbox fa_mailbox;
    DB2010Dma *dma;
    DB2010Signal pdi_irq, dma_irq, dsp_irq;
    const DB2010Register *mode;
    DB2010MemoryPort memory;
    DB2010TimePort time;
    DB2010FaultPort faults;
} DB2010FaLink;
typedef struct DB2010DmaLink {
    DB2010Dma *dma;
    DB2010Display *display;
    DB2010FaLink *fa;
} DB2010DmaLink;
typedef struct DB2010TimerLink {
    DB2010Timer *timer;
    DB2010I2c *i2c;
    const bool *display_frame;
} DB2010TimerLink;
DB2010Register *dbreg_find(DB2010RegisterBank *bank, uint32_t address);
void dbdma_source_advance(DB2010Dma *dma, unsigned channel, unsigned bytes);
void dbdma_complete(DB2010Dma *dma, unsigned channel);
void dbfm_consumed(DB2010FaMailbox *mailbox);
const char *db2010_dma_access(void *opaque, DB2010Access *a);
const char *db2010_mailbox_access(void *opaque, DB2010Access *a);
const char *db2010_peer_access(void *opaque, DB2010Access *a);
const char *db2010_display_access(void *opaque, DB2010Access *a);
const char *db2010_bank_access(void *opaque, DB2010Access *a);
const char *db2010_timer_access(void *opaque, DB2010Access *a);
const char *db2010_controller_access(void *opaque, DB2010Access *a);
const char *db2010_region_access(void *opaque, DB2010Access *a);
const char *db2010_absent_access(void *opaque, DB2010Access *a);
const char *db2010_nor_bus_access(void *opaque, DB2010Access *a);
const char *db2010_rtc_access(void *opaque, DB2010Access *a);
const char *db2010_retained_access(void *opaque, DB2010Access *a);
const char *db2010_power_access(void *opaque, DB2010Access *a);
const char *db2010_startup_access(void *opaque, DB2010Access *a);
const char *db2010_uart_access(void *opaque, DB2010Access *a);
const char *display_dma_fault(DB2010Display *s);
const char *fa_dma_arm_fault(DB2010FaLink *s);
const char *dsp_peer_submit_fault(DB2010FaLink *s);
void dsp_peer_advance(DB2010FaLink *s);
void display_dma_first_burst(DB2010Display *s);
uint64_t display_clock(DB2010Display *s);
void display_irq_update(DB2010Display *s);
void display_dma_advance(DB2010Display *s);
const DB2010RegMap *db2010_controller_map(void);
const DB2010RegMap *db2010_region_map(void);
const DB2010RegMap *db2010_absent_map(void);
const DB2010RegMap *db2010_nor_map(void);
const DB2010RegMap *db2010_power_map(void);
const DB2010RegMap *db2010_startup_map(void);
const DB2010RegMap *db2010_uart_map(void);
const DB2010RegMap *db2010_data_uart_map(void);
void db2010_data_uart_cable(DB2010Uart *s, bool present);
void db2010_data_uart_receive(DB2010Uart *s, const uint8_t *bytes, unsigned count);
const DB2010RegMap *db2010_bluetooth_uart_map(void);
bool db2010_bluetooth_state_valid(const DB2010Uart *s);
bool startup_status_contains(uint32_t address, unsigned size);
const char *startup_status_access(DB2010Startup *s, uint32_t address,
    unsigned size, bool write, uint32_t *value, bool commit);
void db2010_power_reset(DB2010Power *s);
void db2010_startup_reset(DB2010Startup *s);
void db2010_uart_reset(DB2010Uart *s);
void db2010_memory_controller_reset(DB2010MemoryController *s, bool enabled);
void db2010_region_controller_reset(DB2010RegionController *s);
void db2010_absent_reset(DB2010Absent *s);
void db2010_display_capture_reset(DB2010Display *s);
void db2010_display_reset(DB2010Display *s);
void db2010_fa_reset(DB2010FaLink *s);
void dbreg_reset(DB2010RegisterBank *s);
void dbfc_export(DB2010F500Control *model, QDict *snapshot);
void db2010_startup_export(DB2010Startup *model, QDict *snapshot);
void db2010_fa_dma_export(DB2010FaLink *model, QDict *snapshot);
void db2010_display_export(DB2010Display *model, QDict *snapshot);
void dbn_export(DB2010Nand *model, QDict *snapshot);
void dbi_export(DB2010I2c *model, QDict *snapshot);
void dbdma_export(DB2010Dma *model, QDict *snapshot);
void dbrtc_export(DB2010Rtc *model, QDict *snapshot, uint64_t count, uint64_t idle);
void dbrtr_export(DB2010RtcRetained *model, QDict *snapshot);
void db2010_uart_export(DB2010Uart *model, QDict *snapshot);
void dbtimer_export(DB2010Timer *model, QDict *snapshot, QList * irq_events);
void db2010_absent_export(DB2010Absent *model, QDict *snapshot);
void db2010_power_export(DB2010Power *model, QDict *snapshot);
void dbreg_export(DB2010RegisterBank *model, QDict *snapshot);
void db2010_region_export(DB2010RegionController *model, QDict *snapshot);
void db2010_controller_export(DB2010MemoryController *model, QDict *snapshot);
void db2010_uart_connect(DB2010Uart *s);
void db2010_region_table_committed(DB2010RegionController *s);
void db2010_nor_prepare(void *opaque, DB2010Access *a);
#endif
