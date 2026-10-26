/* PL080 cold initialization, display stream and opt-in FA request-waiting arm. */
#ifndef HW_ARM_DB2010_DMA_H
#define HW_ARM_DB2010_DMA_H
#include "db2010-bus.h"
const DB2010RegMap *dbdma_map(void);
typedef struct DB2010DmaConfig {
    bool display_first_burst;
    bool display_frame;
    bool display_stream;
    bool fa_tx;
    bool fa_rx;
    bool display_partial;
} DB2010DmaConfig;
typedef struct DB2010DmaRuntime {
    uint32_t global[6];
    uint32_t channel[8][5];
    uint32_t raw_tc;
    uint32_t raw_error;
} DB2010DmaRuntime;
typedef struct DB2010Dma {
    DB2010DmaConfig config;
    DB2010DmaRuntime state;
} DB2010Dma;

const char *dbdma_access(DB2010Dma *d, uint32_t address,
                                       unsigned size, bool write, uint32_t *value);

void dbdma_source_advance(DB2010Dma *dma, unsigned channel, unsigned bytes);
void dbdma_word_transferred(DB2010Dma *dma, unsigned channel);
void dbdma_complete(DB2010Dma *dma, unsigned channel);
void dbdma_reset(DB2010Dma *d);
typedef struct QDict QDict;
typedef struct QList QList;
void dbdma_export(DB2010Dma *model, QDict *snapshot);
#endif
