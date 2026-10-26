/* Narrow research memory/time/fault connections. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_LINKS_H
#define DB2010_LINKS_H
#include "db2010-contract.h"

typedef struct DB2010MemoryView {
    uint32_t base;
    const uint8_t *bytes, *known;
    bool unchecked; /* research RAM validity is unavailable in fast mode */
} DB2010MemoryView;
typedef struct DB2010MemoryPort {
    void *opaque;
    bool (*view)(void *opaque, uint32_t address, unsigned size, DB2010MemoryView *view);
    bool (*overlay)(void *opaque, uint32_t address, unsigned size);
    /* NULL data validates a writable DMA destination without changing it. */
    bool (*write)(void *opaque, uint32_t address, const uint8_t *data, unsigned size);
} DB2010MemoryPort;
typedef struct DB2010TimePort {
    const uint64_t *count, *idle;
    const uint32_t *pc;
} DB2010TimePort;
typedef struct DB2010FaultPort {
    void *opaque;
    void (*stop)(void *opaque, const char *reason, uint32_t address, unsigned size);
} DB2010FaultPort;
static inline DB2010MemoryView *db2010_memory_view(DB2010MemoryPort *port,
    uint32_t address, unsigned size, DB2010MemoryView *view)
{
    return port->view(port->opaque, address, size, view) ? view : NULL;
}
static inline void db2010_link_stop(DB2010FaultPort *port, const char *reason,
                                    uint32_t address, unsigned size)
{
    port->stop(port->opaque, reason, address, size);
}
typedef struct DB2010Signal {
    void *opaque;
    void (*set)(void *opaque, bool level);
} DB2010Signal;
static inline void db2010_signal_set(DB2010Signal *signal, bool level)
{
    if (signal->set) { signal->set(signal->opaque, level); }
}
#endif
