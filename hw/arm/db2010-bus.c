/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-bus.h"

void db2010_bus_init(DB2010Bus *bus)
{
    bus->cached_owner = NULL; bus->cached_claim = NULL;
    bus->pages = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                     (GDestroyNotify)g_ptr_array_unref);
    bus->owners = g_ptr_array_new_with_free_func(g_free);
}

void db2010_bus_destroy(DB2010Bus *bus)
{
    g_hash_table_unref(bus->pages);
    g_ptr_array_unref(bus->owners);
}

const char *db2010_bus_add(DB2010Bus *bus, const char *name,
                          const DB2010RegisterSpec *claims, unsigned count,
                          DB2010AccessFn access, void *opaque, unsigned priority)
{
    for (unsigned i = 0; i < count; i++) {
        const DB2010RegisterSpec *r = &claims[i];
        if (!r->span || (uint64_t)r->address + r->span > (1ULL << 32)) {
            return "invalid MMIO ownership boundary";
        }
        for (unsigned j = 0; j < bus->owners->len; j++) {
            DB2010Owner *other = g_ptr_array_index(bus->owners, j);
            if (db2010_claims(other->claims, other->count, r->address, r->span)) {
                return "conflicting MMIO ownership";
            }
        }
    }
    DB2010Owner *owner = g_new0(DB2010Owner, 1);
    *owner = (DB2010Owner){.name = name, .claims = claims, .count = count,
        .priority = priority, .opaque = opaque, .access = access};
    g_ptr_array_add(bus->owners, owner);
    for (unsigned i = 0; i < count; i++) {
        uint32_t first = claims[i].address >> 12;
        uint32_t last = ((uint64_t)claims[i].address + claims[i].span - 1) >> 12;
        for (uint32_t page = first; page <= last; page++) {
            gpointer key = GUINT_TO_POINTER(page);
            GPtrArray *entries = g_hash_table_lookup(bus->pages, key);
            if (!entries) {
                entries = g_ptr_array_new();
                g_hash_table_insert(bus->pages, key, entries);
            }
            if (!g_ptr_array_find(entries, owner, NULL)) {
                g_ptr_array_add(entries, owner);
            }
        }
    }
    return NULL;
}

const char *db2010_bus_add_map(DB2010Bus *bus, const char *name,
    const DB2010RegMap *map, DB2010AccessFn access, void *opaque,
    unsigned priority)
{
    if (!map->claim_count || !map->claims) {
        return "uninitialized register map";
    }
    return db2010_bus_add(bus, name, map->claims, map->claim_count,
                         access, opaque, priority);
}

DB2010Owner *db2010_bus_owner(DB2010Bus *bus, uint32_t address, unsigned size)
{
    const DB2010RegisterSpec *cached = bus->cached_claim;
    if (size && cached && address >= cached->address &&
        (uint64_t)address + size <= (uint64_t)cached->address + cached->span) {
        return bus->cached_owner;
    }
    DB2010Owner *best = NULL;
    uint64_t end = MIN((uint64_t)address + size, 1ULL << 32);
    if (!size) { return NULL; }
    for (uint32_t page = address >> 12; page <= (end - 1) >> 12; page++) {
        GPtrArray *entries = g_hash_table_lookup(bus->pages, GUINT_TO_POINTER(page));
        if (!entries) { continue; }
        for (unsigned i = 0; i < entries->len; i++) {
            DB2010Owner *owner = g_ptr_array_index(entries, i);
            if (owner->start_within && !db2010_claims(owner->claims, owner->count, address, 1)) { continue; }
            if ((!best || owner->priority < best->priority) &&
                db2010_claims(owner->claims, owner->count, address, size)) {
                best = owner;
            }
        }
    }
    if (best) {
        for (unsigned i = 0; i < best->count; i++) {
            const DB2010RegisterSpec *r = &best->claims[i];
            if (address >= r->address && (uint64_t)address + size <= (uint64_t)r->address + r->span) {
                bus->cached_owner = best; bus->cached_claim = r; break;
            }
        }
    }
    return best;
}
