/* EC/35 x8 command/gate, Read2, known-cell programming and bounded main reads. */
#ifndef HW_ARM_DB2010_NAND_H
#define HW_ARM_DB2010_NAND_H
#include "db2010-bus.h"
const DB2010RegMap *dbn_map(void);
const char *dbn_bus_access(void *opaque, DB2010Access *a);
typedef enum DB2010NandMode {
    DBN_UNKNOWN = 0,
    DBN_IDLE = 1,
    DBN_ID_ADDRESS = 2,
    DBN_ID = 3,
    DBN_STATUS = 4,
    DBN_SPARE_ADDRESS = 5,
    DBN_SPARE_READ = 6,
    DBN_PROGRAM_ADDRESS = 7,
    DBN_PROGRAM_DATA = 8,
    DBN_PROGRAM_COMPLETE = 9,
    DBN_ERASE_ADDRESS = 10,
    DBN_ERASE_READY = 11,
    DBN_ERASE_COMPLETE = 12,
    DBN_MAIN_ADDRESS = 13,
    DBN_MAIN_READ = 14,
    DBN_MAIN_PROGRAM_ADDRESS = 15,
    DBN_MAIN_PROGRAM_DATA = 16,
    DBN_COPY_ADDRESS = 17,
    DBN_COPY_COMPLETE = 18,
} DB2010NandMode;

typedef struct DB2010Nand {
    bool omit_diagnostics;
    DB2010NandMode mode;
    unsigned id_cursor, resets, cycles, program_size, spare_program_limit;
    bool read_open, erase_enabled, main_enabled, main_selected;
    int id_address, page, column;
    int main_start_column;
    uint8_t addresses[3], program[528];
    uint8_t *bytes, *known, *initial_bytes, *initial_known;
    uint32_t *program_counts;
    uint8_t *erased_blocks;
    QList *operations, *erase_operations;
    uint8_t *main_bytes, *main_known_blocks, *main_program_counts;
    uint8_t *initial_main_bytes;
    QList *main_operations;
    uint64_t main_reads;
    bool copy_enabled, copy_no_wear;
    int copy_source;
    uint8_t *copied_pages;
    QList *copy_operations;
    uint64_t partial_after_copy;
} DB2010Nand;

void dbn_reset(DB2010Nand *t);

const char *dbn_access(DB2010Nand *t, uint32_t address, unsigned size,
                                    bool write, uint32_t *value, bool commit);

typedef struct QDict QDict;
typedef struct QList QList;
void dbn_export(DB2010Nand *model, QDict *snapshot);
#endif
