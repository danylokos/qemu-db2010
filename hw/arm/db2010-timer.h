/* Bounded recovered TIMER1/cascade contract; clocks remain model assumptions. */
#ifndef HW_ARM_DB2010_TIMER_H
#define HW_ARM_DB2010_TIMER_H

#include "db2010-contract.h"
#include "db2010-time.h"
#include "db2010-links.h"
const DB2010RegMap *dbtimer_map(void);

typedef struct DB2010TimerConfig {
    uint32_t frequency_hz;
    uint32_t initial_masks[3];
    bool initial_masks_known[3];
    bool i2c_route;
    bool display_route;
    bool dsp_route;
    bool keypad_route;
    bool rtc_route;
    bool simif_route;
    bool bluetooth_route;
    bool paired_channel;
    bool data_uart_route;
    uint64_t max_irqs;
    bool unlimited_irqs;
} DB2010TimerConfig;
typedef enum DB2010IrqSelection { DBIRQ_UNSELECTED = 0, DBIRQ_SELECTED = 1 } DB2010IrqSelection;
typedef enum DB2010CascadeAck { DBIRQ_WAIT_SECONDARY = 0, DBIRQ_SECONDARY_ACKED = 1 } DB2010CascadeAck;
typedef struct DB2010TimerRuntime {
    DB2010ClockPhase phase;
    uint32_t control;
    uint32_t load;
    uint32_t counter;
    uint32_t masks[3];
    bool load_known;
    bool masks_known[3];
    bool pending;
    DB2010IrqSelection selected;
    DB2010CascadeAck secondary_acked;
    bool i2c_pending;
    bool i2c_selected;
    uint64_t i2c_irqs;
    bool pdi_pending;
    bool dma_pending;
    bool pdi_selected;
    bool dma_selected;
    uint64_t pdi_irqs;
    uint64_t dma_irqs;
    bool keypad_pending, keypad_selected;
    uint64_t keypad_irqs;
    bool dsp_pending;
    bool dsp_selected;
    uint64_t dsp_irqs;
    bool rtc_pending, rtc_selected, rtc_input_level;
    uint64_t rtc_irqs;
    bool sim_rx_pending, sim_rx_selected;
    bool sim_tx_pending, sim_tx_selected, sim_tx_input_level;
    uint64_t sim_rx_irqs, sim_tx_irqs;
    bool bluetooth_pending, bluetooth_selected;
    uint64_t bluetooth_irqs;
    uint64_t last_cycles;
    uint64_t idle_cycles;
    uint64_t irqs;
    uint32_t paired_load, paired_counter;
    bool paired_load_known, paired_pending, paired_selected;
    uint64_t paired_irqs;
    bool data_uart_pending, data_uart_selected;
    uint64_t data_uart_irqs;
} DB2010TimerRuntime;
typedef struct DB2010Timer {
    Clock *clock;
    DB2010Signal irq;
    DB2010TimerConfig config;
    DB2010TimerRuntime state;
} DB2010Timer;

bool dbtimer_routed(const DB2010Timer *t);

/* Actual bound I2C vector/compact line70: bank2 IRQ bit10 / cascade30.
 * Timer routing and legacy snapshots retain their original contract. */
bool dbirq_i2c_routed(const DB2010Timer *t);

/* Observed PDI vector41 = secondary13 / cascade28; PL080 vector128 maps
 * compact77 = tertiary17 / cascade30. Native ILRs: PDI edge/IRQ, DMA level/IRQ. Compactbank offsets are28/60, not32/64. */
bool dbirq_pdi_routed(const DB2010Timer *t);

bool dbirq_dma_routed(const DB2010Timer *t);

/* Firmware DSPIF_INT primary IRQ9, live vector9 -> native Thumb0688. */
bool dbirq_dsp_routed(const DB2010Timer *t);

bool dbirq_any_routed(const DB2010Timer *t);

const char *dbtimer_advance(DB2010Timer *t, uint64_t now);

bool dbtimer_contains(uint32_t address, unsigned size);

const char *dbtimer_access(DB2010Timer *t, uint32_t address,
                                         unsigned size, bool write, uint32_t *value);

const char *dbtimer_observe(DB2010Timer *t, uint32_t address, uint32_t value);

const char *dbtimer_select(DB2010Timer *t);

int64_t dbtimer_deadline(DB2010Timer *t);
void dbtimer_update_irq(DB2010Timer *t);

void dbtimer_reset(DB2010Timer *d);
typedef struct QDict QDict;
typedef struct QList QList;
void dbtimer_export(DB2010Timer *model, QDict *snapshot, QList * irq_events);
typedef enum DB2010IrqLine {
    DBIRQ_I2C, DBIRQ_PDI, DBIRQ_DMA, DBIRQ_DSP, DBIRQ_KEYPAD, DBIRQ_RTC, DBIRQ_SIM_RX, DBIRQ_SIM_TX, DBIRQ_BLUETOOTH, DBIRQ_DATA_UART,
} DB2010IrqLine;
typedef struct DB2010IrqInput {
    DB2010Timer *timer;
    DB2010IrqLine line;
} DB2010IrqInput;
void dbtimer_set_line(DB2010Timer *timer, DB2010IrqLine line, bool level);
void dbtimer_input(void *opaque, bool level);
const bool *dbtimer_input_latch(const DB2010Timer *timer, DB2010IrqLine line);
#endif
