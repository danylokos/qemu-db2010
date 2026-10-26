/* Bounded conditional Intel L18 model for the DB2010 research machine. */
#ifndef HW_ARM_DB2010_NOR_H
#define HW_ARM_DB2010_NOR_H

#define DBNOR_BASE 0x44000000u
#define DBNOR_SIZE 0x02000000u

#include "system/memory.h"
#include "qobject/qdict.h"
#include "target/arm/cpu.h"

typedef enum DB2010NorMode {
    DBNOR_ARRAY = 0, DBNOR_STATUS = 1, DBNOR_ID = 2, DBNOR_CFI = 3,
} DB2010NorMode;

typedef enum DB2010NorPending {
    DBNOR_NONE = 0, DBNOR_LOCK = 1, DBNOR_ERASE = 2, DBNOR_WORD = 3,
    DBNOR_BUFFER_COUNT = 4, DBNOR_BUFFER_DATA = 5, DBNOR_BUFFER_CONFIRM = 6,
} DB2010NorPending;

typedef struct DB2010NorPart {
    DB2010NorMode mode;
    DB2010NorPending pending;
    uint16_t status;
    uint32_t block, address, count, used;
    uint32_t word_address[32];
    uint16_t word_value[32];
} DB2010NorPart;

typedef struct DB2010NorOverlay {
    uint32_t address;
    uint8_t expected[20], value[20];
} DB2010NorOverlay;

typedef struct DB2010Nor {
    MemoryRegion *mr;
    CPUARMState *env;
    uint8_t *array, *known;
    bool locked[259], down[259];
    bool initial_locked, wp_high, mirrored, bridge, probes, retry;
    bool has_rcr, id_cfi, has_unique, unaligned_array_halfwords;
    bool has_protection_lock0, protection_lock0_known;
    uint16_t protection_lock0;
    bool has_user_protection;
    uint64_t user_protection;
    uint16_t manufacturer, device, rcr, initial_rcr, cfi[512];
    bool cfi_known[512];
    uint64_t unique, commands, operations;
    DB2010NorPart part[16];
    MemoryRegion overlay_mr;
    bool research_guards;
    GArray *unknown_windows;
    unsigned overlay_count;
    DB2010NorOverlay overlay[2];
    uint64_t overlay_reads, overlay_bytes;
} DB2010Nor;

void db2010_nor_configure(DB2010Nor *n, QDict *state);
void db2010_nor_init(DB2010Nor *n, MemoryRegion *mr, uint8_t *known, const char *name, Object *owner);
void db2010_nor_reset(DB2010Nor *n);
void db2010_nor_map_unknown(DB2010Nor *n, Object *owner);
const char *db2010_nor_read(DB2010Nor *n, uint32_t address, unsigned size,
                           uint32_t *value, uint32_t *fault_address);
const char *db2010_nor_write(DB2010Nor *n, uint32_t address, unsigned size,
                            uint32_t value, bool commit);
bool db2010_nor_probe_view(DB2010Nor *n, uint32_t address, unsigned size);
bool db2010_nor_overlay_view(DB2010Nor *n, uint32_t address, unsigned size);
void db2010_nor_overlay_observe(DB2010Nor *n, uint32_t address, unsigned size);
QDict *db2010_nor_snapshot(DB2010Nor *n);

#endif
