/* Guarded DB2010 ownership registry. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_BUS_H
#define DB2010_BUS_H
typedef struct _GHashTable GHashTable;
typedef struct _GPtrArray GPtrArray;
#include "db2010-contract.h"

typedef struct DB2010Access {
    uint32_t address, value, pc, fault_address;
    unsigned fault_size;
    unsigned size;
    uint64_t now, idle;
    bool write, commit;
} DB2010Access;

typedef const char *(*DB2010AccessFn)(void *opaque, DB2010Access *access);
typedef struct DB2010Owner {
    const char *name;
    const DB2010RegisterSpec *claims;
    unsigned count, priority;
    bool start_within;
    void *opaque;
    DB2010AccessFn access;
    void (*prepare_native)(void *opaque, DB2010Access *access);
} DB2010Owner;

typedef struct DB2010Bus {
    GHashTable *pages;
    GPtrArray *owners;
    DB2010Owner *cached_owner;
    const DB2010RegisterSpec *cached_claim;
} DB2010Bus;

void db2010_bus_init(DB2010Bus *bus);
void db2010_bus_destroy(DB2010Bus *bus);
/* Conflicts include guarded holes. Aliases belong to one owner's contract. */
const char *db2010_bus_add(DB2010Bus *bus, const char *name,
                          const DB2010RegisterSpec *claims, unsigned count,
                          DB2010AccessFn access, void *opaque, unsigned priority);
/* Map and legacy owners share conflict/priority handling. Map storage must
 * outlive the registry; the owner still supplies its access wrapper. */
const char *db2010_bus_add_map(DB2010Bus *bus, const char *name,
    const DB2010RegMap *map, DB2010AccessFn access, void *opaque,
    unsigned priority);
DB2010Owner *db2010_bus_owner(DB2010Bus *bus, uint32_t address, unsigned size);
#endif
