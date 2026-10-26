/* Private research integration state. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_INTERNAL_H
#define DB2010_INTERNAL_H
/*
 * Sony Ericsson DB2010 research machine: native execution with strict guards.
 * Prepared inputs are supplied by the host; no firmware-container processing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "hw/core/boards.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"
#include "target/arm/cpregs.h"
#include "accel/tcg/cpu-loop.h"
#include "db2010-nor.h"
#include "db2010-simif.h"
#include "db2010-timer.h"
#include "db2010-rtc.h"
#include "db2010-rtc-retained.h"
#include "db2010-dma.h"
#include "db2010-i2c.h"
#include "db2010-nand.h"
#include "db2010-pdi.h"
#include "db2010-panel.h"
#include "db2010-kgen.h"
#include "db2010-fa-transfer.h"
#include "db2010-fa-mailbox.h"
#include "db2010-dsp-peer.h"
#include "db2010-f500-control.h"
#include "db2010-f500-port.h"
#include "chardev/char-fe.h"
#include "db2010-bus.h"
#include "db2010-components.h"
#include "db2010-device.h"
#include "db2010-syscon.h"

#define TYPE_DB2010_MACHINE MACHINE_TYPE_NAME("db2010")
OBJECT_DECLARE_SIMPLE_TYPE(DB2010MachineState, DB2010_MACHINE)

typedef struct DB2010Region {
    MemoryRegion storage;
    MemoryRegion *mr;
    uint32_t base, size;
    bool readonly, itcm, active;
    uint8_t *known, *initial, *initial_known;
} DB2010Region;

typedef struct DB2010Tcm {
    struct DB2010MachineState *machine;
    unsigned bank;
    uint32_t base;
    bool enabled;
    DB2010Region *region;
} DB2010Tcm;



typedef struct DB2010MmioPage {
    uint32_t base;
} DB2010MmioPage;

typedef struct DB2010ExecutionProof {
    uint32_t base;
    size_t size;
    uint8_t *bytes;
} DB2010ExecutionProof;

struct DB2010MachineState {
    MachineState parent;
    DB2010Bus bus;
    DB2010Keypad *keypad;
    bool has_keypad;
    bool has_simif;
    DB2010Simif *simif;
    DeviceState *devices[DBDEV_COUNT];
    qemu_irq timer_irq, i2c_irq;
    DB2010DmaLink dma_link;
    DB2010TimerLink timer_link;
    ARMCPU *cpu;
    bool research_guards;
    ARMBoardHooks cpu_hooks;
    ARMResearchHooks hooks;
    MemoryRegion untranslated_backing;
    bool has_controller;
    bool has_region_controls;
    DB2010Region *section_table;
    GPtrArray *mmio_pages;
    GHashTable *mmio_index;
    bool has_power_hold;
    uint32_t instruction_pc;
    bool has_nor;
    DB2010Nor *nor;
    bool has_absent_bank;
    bool has_timer;
    DB2010Timer *timer;
    QList *irq_events;
    bool has_rtc, has_uart;
    DB2010Rtc *rtc;
    bool has_startup_status;
    bool has_rtc_retained;
    DB2010RtcRetained *rtc_retained;
    bool has_dma;
    DB2010Dma *dma;
    bool has_i2c;
    DB2010I2c *i2c;
    bool has_nand;
    DB2010Nand *nand;
    bool has_f500_control;
    DB2010F500Control *f500_control;
    DB2010F500ControlLink f500_control_link;
    bool has_f500_port;
    DB2010F500Port *f500_port, *f500_aux_port;
    bool has_f500_aux_port;
    bool synthetic_kgen, kgen_final, synthetic_sha;
    bool synthetic_etx;
    uint8_t etx_request[82];
    uint64_t etx_calls;
    uint32_t etx_last_request;
    uint64_t kgen_inits, kgen_updates, kgen_finals, kgen_bytes;
    uint64_t wfi_waits;
    uint32_t kgen_last_context;
    uint64_t sha_inits, sha_updates, sha_finals, sha_bytes;
    uint32_t sha_last_context;
    bool has_fa_transfer;
    DB2010FaTransfer *fa_transfer;
    GPtrArray *regions;
    GArray *execution;
    GPtrArray *execution_proofs;
    DB2010Tcm tcm[2];
    bool cold_zero_reset_record;
    bool checkpoint_loaded;
    char checkpoint_model_identity[65];
    char *state_file;
    uint32_t entry, cpsr, sctlr;
    uint32_t initial_registers[16];
    uint64_t count, limit, native_count_base;
    bool instruction_limited;
    bool bounded_history;
    bool omit_diagnostics;
    QDict *stop;
    char *snapshot_json;
    size_t snapshot_size, snapshot_offset;
    DB2010Power *power;
    DB2010Startup *startup;
    DB2010Uart *uart_port;
    DB2010MemoryController *memory_controller;
    DB2010RegionController *region_controller;
    DB2010Absent *absent;
    DB2010RegisterBank *bank;
    DB2010Syscon *syscon;
    DB2010Display *display;
    DB2010FaLink *fa;
};

typedef struct DB2010Interval {
    uint32_t start;
    uint64_t end;
} DB2010Interval;

typedef struct DB2010Progress {
    DB2010MachineState *machine;
    uint64_t observations, virtual_ns, uart_bytes[4], timer_ticks, timer_irqs, cpu_hz;
    uint64_t wfi_waits, idle_ticks, pmic_transactions, fa_requests, nor_commands;
    uint32_t pc;
    unsigned lcd_frames;
    bool serial_ready, serial_present;
} DB2010Progress;


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
void db2010_nor_prepare(void *opaque, DB2010Access *a);
bool db2010_validate_bus(DB2010MachineState *s, uint32_t address, unsigned size,
                          unsigned operation, uint32_t value);

char *get_checkpoint_layout(Object *obj, Error **errp);
void set_flash_byte_export(Object *obj, const char *value, Error **errp);
void db2010_checkpoint_prepare(DB2010MachineState *s, QDict *state);
void db2010_checkpoint_register(DB2010MachineState *s);
void db2010_time_connect(DB2010MachineState *s);
void db2010_async_stop(DB2010MachineState *s, const char *reason, uint32_t address, unsigned size);
void db2010_qom_create(DB2010MachineState *s);
void db2010_qom_realize(DB2010MachineState *s);
void db2010_qom_reset(DB2010MachineState *s);
void db2010_qom_connect(DB2010MachineState *s);
void db2010_register_owners(DB2010MachineState *s);
bool db2010_dispatch(DB2010MachineState *s, uint32_t address, unsigned size,
                      bool write, uint32_t *value, bool commit);
const char *db2010_rtc_access(void *opaque, DB2010Access *a);
const char *db2010_retained_access(void *opaque, DB2010Access *a);
const char *db2010_power_access(void *opaque, DB2010Access *a);
const char *db2010_startup_access(void *opaque, DB2010Access *a);
const char *db2010_uart_access(void *opaque, DB2010Access *a);

uint64_t input_uint(QDict *dict, const char *key, uint64_t max);
uint32_t input_nullable_word(QDict *dict, const char *key, uint32_t max,
                                    bool *known);
bool only_keys(QDict *dict, const char *const *keys);
DB2010Region *find_region(DB2010MachineState *s, uint32_t addr,
                                unsigned size);
bool executable(DB2010MachineState *s, uint32_t addr, unsigned size);
G_NORETURN void db2010_stop(DB2010MachineState *s, const char *reason,
                                  uint32_t address, unsigned size);
const char *generate_section_table(DB2010MachineState *s, uint32_t *table,
                                          unsigned *fault_slot);
void commit_section_table(DB2010MachineState *s);
DB2010Register *find_register(DB2010MachineState *s, uint32_t address);
DB2010MmioPage *find_mmio_page(DB2010MachineState *s, uint32_t address);
void declare_mmio_page(DB2010MachineState *s, uint32_t address);
void load_register_contracts(DB2010MachineState *s, QDict *state);
bool startup_status_contains(uint32_t address, unsigned size);
const char *startup_status_access(DB2010Startup *s, uint32_t address,
                                        unsigned size, bool write, uint32_t *value,
                                        bool commit);
void guard_registers(DB2010MachineState *s, uint32_t addr,
                             unsigned size, unsigned op, uint32_t value);
void guard_memory(void *opaque, CPUARMState *env, uint32_t addr,
                          unsigned size, unsigned operation, uint32_t value);
void db2010_runtime_observe(DB2010MachineState *s);
bool db2010_cpu_instruction_needed(void *opaque, uint32_t pc, uint32_t insn, bool thumb);
void db2010_cpu_instruction(void *opaque, CPUARMState *env, unsigned size, uint32_t insn);
void db2010_cpu_io(void *opaque, CPUARMState *env, uint32_t address,
                   unsigned size, bool write, uint64_t value, uintptr_t retaddr);
void db2010_cpu_exception(void *opaque, CPUARMState *env, int exception, uint32_t vector);
void db2010_cpu_wfi(void *opaque, CPUARMState *env, unsigned size);
void db2010_cpu_service(DB2010MachineState *s, CPUARMState *env, uint32_t pc, unsigned size);
void db2010_cpu_control(DB2010MachineState *s, CPUARMState *env, uint32_t pc,
                        unsigned size, uint32_t insn);
void guard_exception(void *opaque, CPUARMState *env, int exception, uint32_t vector);
void select_timer_irq(DB2010MachineState *s, CPUARMState *env, bool candidate);
void timer_wfi(void *opaque, CPUARMState *env, unsigned size);
void synthetic_hash_api(DB2010MachineState *s, CPUARMState *env, uint32_t pc,
                               unsigned size, bool sha);
void synthetic_etx(DB2010MachineState *s, CPUARMState *env, uint32_t pc,
                          unsigned size);
void guard_instruction(void *opaque, CPUARMState *env, uint32_t pc,
                               unsigned size, uint32_t insn);
DB2010Region *add_region(DB2010MachineState *s, const char *name,
                                uint32_t base, uint32_t size, bool readonly);
uint64_t tcm_read(CPUARMState *env, const ARMCPRegInfo *ri);
void tcm_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value);
void db2010_reset(void *opaque);
void db2010_validate_connections(DB2010MachineState *s);
void load_state(DB2010MachineState *s);
char *get_state(Object *obj, Error **errp);
void set_state(Object *obj, const char *value, Error **errp);
void collect_progress(CPUState *cpu, run_on_cpu_data data);
char *get_progress(Object *obj, Error **errp);
char *get_display(Object *obj, Error **errp);
char *get_snapshot(Object *obj, Error **errp);
char *get_summary(Object *obj, Error **errp);
char *get_snapshot_chunk(Object *obj, Error **errp);
void set_snapshot_chunk(Object *obj, const char *value, Error **errp);
uint64_t untranslated_read(void *opaque, hwaddr address, unsigned size);
void untranslated_write(void *opaque, hwaddr address, uint64_t value,
                                unsigned size);
extern const MemoryRegionOps untranslated_ops;
#endif
