/* Bounded R61500_B PDI write-program recognition, not a general sequencer ISA.
 * Storage bounds/packing come from original firmware; timing is a declared mock.
 * LCD bytes are captured with guarded native DMA completion; panel reads remain unsupported.
 */
#ifndef HW_ARM_DB2010_PDI_H
#define HW_ARM_DB2010_PDI_H
#include "db2010-bus.h"
const DB2010RegMap *dbpdi_map(void);

typedef struct DB2010PdiConfig {
    bool repeat_frames;
    bool retained_latches;
} DB2010PdiConfig;
typedef enum DB2010PdiDmaState { DBPDI_DMA_IDLE = 0, DBPDI_DMA_ARMED = 1 } DB2010PdiDmaState;
typedef struct DB2010PdiRuntime {
    uint32_t program[26];
    uint32_t data[6];
    uint32_t mask;
    uint32_t config;
    uint32_t control;
    uint32_t selector;
    uint32_t event_mask;
    unsigned data_known;
    unsigned data_dirty;
    bool mask_known;
    bool mask_dirty;
    uint64_t busy_until;
    uint64_t transfers;
    uint64_t busy_reads;
    DB2010PdiDmaState dma_armed;
    bool dma_complete;
    uint64_t stop_commands;
    uint32_t dma_bytes;
    unsigned packet_size;
    uint8_t packet[24];
    uint8_t rs[24];
} DB2010PdiRuntime;
typedef struct DB2010Pdi {
    DB2010PdiConfig config;
    DB2010PdiRuntime state;
} DB2010Pdi;

bool dbpdi_contains(uint32_t address, unsigned size);
bool dbpdi_dma_write_selected(const DB2010Pdi *p);

const char *dbpdi_access(DB2010Pdi *p, uint32_t address,
                                      unsigned size, uint64_t count,
                                      bool write, uint32_t *value);

void dbpdi_dma_complete(DB2010Pdi *p, uint64_t count);
void dbpdi_reset(DB2010Pdi *d);
#endif
