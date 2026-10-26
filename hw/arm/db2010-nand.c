/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-nand.h"

void dbn_reset(DB2010Nand *t)
{
    t->mode = DBN_UNKNOWN; t->read_open = false;
    t->id_address = t->page = t->column = -1; t->id_cursor = t->resets = t->cycles = t->program_size = 0;
    memcpy(t->bytes, t->initial_bytes, 65536 * 16); memcpy(t->known, t->initial_known, 65536 * 16);
    memset(t->program_counts, 0, 65536 * sizeof(*t->program_counts));
    qobject_unref(t->operations); t->operations = qlist_new();
    if (t->erase_enabled) {
        memset(t->erased_blocks, 0, 2048);
        qobject_unref(t->erase_operations); t->erase_operations = qlist_new();
    }
    if (t->main_enabled) {
        memset(t->main_known_blocks, t->initial_main_bytes ? 1 : 0, 2048);
        if (t->initial_main_bytes) {
            memcpy(t->main_bytes, t->initial_main_bytes, 65536 * 512);
        }
        memset(t->main_program_counts, 0, 65536);
        qobject_unref(t->main_operations); t->main_operations = qlist_new();
        t->main_selected = false; t->main_start_column = -1; t->main_reads = 0;
    }
    if (t->copy_enabled) {
        t->copy_source = -1; t->partial_after_copy = 0;
        memset(t->copied_pages, 0, 65536);
        qobject_unref(t->copy_operations); t->copy_operations = qlist_new();
    }
}

typedef enum DB2010NandEventKind { DBN_COMMAND, DBN_ADDRESS, DBN_STAGE_DATA, DBN_READ_DATA, DBN_GATE } DB2010NandEventKind;
typedef struct DB2010NandEvent {
    DB2010NandEventKind kind;
    uint32_t *value;
    bool commit, gate_open;
} DB2010NandEvent;

static const char *dbn_program_main(DB2010Nand *t, const DB2010NandEvent *e)
{
    bool commit = e->commit;
    if (t->copy_enabled && t->copied_pages[t->page] && !t->copy_no_wear) {
        return "nand_copyback_partial_program_unmodeled";
    }
    unsigned start = t->column;
    bool main = start < 512;
    /* Main-area transactions have their own bounded history.
     * The legacy spare-only guard is unchanged; combined-area
     * endurance/failure behavior is not inferred here. */
    if (main && !t->main_known_blocks[t->page / 32]) { return "nand_program_unknown_main"; }
    if (main && t->main_program_counts[t->page] >= 2) { return "nand_main_partial_program_limit"; }
    bool record = commit && !t->omit_diagnostics;
    GString *old = record ? g_string_new(NULL) : NULL;
    GString *new = record ? g_string_new(NULL) : NULL;
    for (unsigned i = 0; i < t->program_size; i++) {
        unsigned col = start + i;
        if (col >= 512 && !t->known[t->page * 16 + col - 512]) { return "nand_program_unknown_spare"; }
        uint8_t byte = col < 512 ? t->main_bytes[t->page * 512 + col] : t->bytes[t->page * 16 + col - 512];
        if (record) {
            g_string_append_printf(old, "%02x", byte);
            g_string_append_printf(new, "%02x", byte & t->program[i]);
        }
    }
    if (commit) {
        if (t->copy_enabled && t->copied_pages[t->page]) { t->partial_after_copy++; }
        for (unsigned i = 0; i < t->program_size; i++) {
            unsigned col = start + i;
            if (col < 512) {
                t->main_bytes[t->page * 512 + col] &= t->program[i];
                if (t->program[i] != 255) { t->erased_blocks[t->page / 32] = 0; }
            } else { t->bytes[t->page * 16 + col - 512] &= t->program[i]; }
        }
        if (record) {
            QDict *operation = qdict_new();
            qdict_put_int(operation, "page", t->page); qdict_put_int(operation, "column", start);
            qdict_put_str(operation, "old_hex", old->str); qdict_put_str(operation, "new_hex", new->str);
            GString *input = g_string_new(NULL);
            for (unsigned i = 0; i < t->program_size; i++) { g_string_append_printf(input, "%02x", t->program[i]); }
            qdict_put_str(operation, "input_hex", input->str); g_string_free(input, true);
            qlist_append(t->main_operations, operation);
            g_string_free(old, true); g_string_free(new, true);
        }
        if (main) { t->main_program_counts[t->page]++; }
    }
    t->program_size = 0; t->mode = DBN_PROGRAM_COMPLETE; t->cycles = 0;
    t->main_selected = false; t->page = t->column = -1;
    return NULL;
}

static const char *dbn_program_confirm(DB2010Nand *t, const DB2010NandEvent *e)
{
    bool commit = e->commit;
    if (t->main_enabled && t->mode == DBN_MAIN_PROGRAM_DATA && t->program_size) { return dbn_program_main(t, e); }
                    if (t->mode != DBN_PROGRAM_DATA || !t->program_size) { return "nand_program_confirm_unmodeled"; }
                    if (t->copy_enabled && t->copied_pages[t->page] && !t->copy_no_wear) {
                        return "nand_copyback_partial_program_unmodeled";
                    }
                    if (t->spare_program_limit && t->program_counts[t->page] >= t->spare_program_limit) {
                        return "nand_spare_partial_program_limit";
                    }
                    if (t->program_counts[t->page] == UINT32_MAX) { return "nand_program_history_overflow"; }
                    unsigned start = t->page * 16 + t->column;
                    for (unsigned i = 0; i < t->program_size; i++) {
                        if (!t->known[start + i]) { return "nand_program_unknown_spare"; }
                        if ((t->bytes[start + i] & t->program[i]) != t->program[i]) { return "nand_program_requires_erase"; }
                    }
                    if (commit) {
                        if (t->copy_enabled && t->copied_pages[t->page]) { t->partial_after_copy++; }
                        if (!t->omit_diagnostics) {
                            QDict *operation = qdict_new();
                            GString *old = g_string_new(NULL), *new = g_string_new(NULL);
                            for (unsigned i = 0; i < t->program_size; i++) {
                                g_string_append_printf(old, "%02x", t->bytes[start + i]); g_string_append_printf(new, "%02x", t->program[i]);
                            }
                            qdict_put_int(operation, "page", t->page); qdict_put_int(operation, "column", t->column);
                            qdict_put_str(operation, "old_hex", old->str); qdict_put_str(operation, "new_hex", new->str);
                            g_string_free(old, true); g_string_free(new, true); qlist_append(t->operations, operation);
                        }
                        memcpy(t->bytes + start, t->program, t->program_size); t->program_counts[t->page]++;
                    }
                    t->program_size = 0; t->mode = DBN_PROGRAM_COMPLETE; t->cycles = 0; t->page = t->column = -1;

    return NULL;
}

static const char *dbn_reset_command(DB2010Nand *t, const DB2010NandEvent *e)
{
    t->resets++; t->mode = DBN_IDLE; t->id_address = -1; t->id_cursor = 0;
                    t->read_open = false; t->cycles = 0; t->page = t->column = -1; t->program_size = 0;
                    t->main_selected = false; t->main_start_column = -1;
                    t->copy_source = -1;

    return NULL;
}

static const char *dbn_identify_status(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (!t->resets) { return "nand_handoff_state_unmodeled"; }
                    if (*value == 0x70 && t->mode == DBN_SPARE_ADDRESS) { return "nand_incomplete_spare_address"; }
                    if (*value == 0x70 && t->mode == DBN_MAIN_ADDRESS &&
                        (t->cycles || t->page < 0)) { return "nand_incomplete_main_address"; }
                    t->mode = *value == 0x90 ? DBN_ID_ADDRESS : DBN_STATUS;
                    if (*value == 0x90) { t->id_address = -1; t->id_cursor = 0; t->cycles = 0; t->page = t->column = -1; }

    return NULL;
}

static const char *dbn_main_select(DB2010Nand *t, const DB2010NandEvent *e)
{
    if (!t->resets) { return "nand_handoff_state_unmodeled"; }
                    /* 00 selects area A before either a three-cycle read or 80.
                     * After status, it restores the selected page register. */
                    if (!t->main_selected) { t->page = t->main_start_column = -1; }
                    t->main_selected = true; t->mode = DBN_MAIN_ADDRESS; t->cycles = 0;
                    t->column = t->main_start_column;

    return NULL;
}

static const char *dbn_copy_begin(DB2010Nand *t, const DB2010NandEvent *e)
{
    if (!t->main_selected || t->mode != DBN_STATUS || t->page < 0 || t->main_start_column != 0) {
                        return "nand_copyback_source_unmodeled";
                    }
                    if (!t->main_known_blocks[t->page / 32]) { return "nand_copyback_unknown_source"; }
                    for (unsigned i = 0; i < 16; i++) {
                        if (!t->known[t->page * 16 + i]) { return "nand_copyback_unknown_source_spare"; }
                    }
                    /* Read00 loaded the complete page register; no host DATA reload. */
                    memcpy(t->program, t->main_bytes + t->page * 512, 512);
                    memcpy(t->program + 512, t->bytes + t->page * 16, 16);
                    t->copy_source = t->page; t->program_size = 528;
                    t->mode = DBN_COPY_ADDRESS; t->cycles = 0;
                    t->main_selected = false; t->page = t->column = -1;

    return NULL;
}

static const char *dbn_spare_select(DB2010Nand *t, const DB2010NandEvent *e)
{
    t->main_selected = false;
                    if (!t->resets) { return "nand_handoff_state_unmodeled"; }
                    if (t->mode == DBN_STATUS && t->page >= 0) { t->mode = DBN_SPARE_READ; }
                    else { t->mode = DBN_SPARE_ADDRESS; t->cycles = 0; t->page = t->column = -1; }

    return NULL;
}

static const char *dbn_program_begin(DB2010Nand *t, const DB2010NandEvent *e)
{
    if (t->main_enabled && t->mode == DBN_MAIN_ADDRESS && !t->cycles) {
                        t->mode = DBN_MAIN_PROGRAM_ADDRESS; t->program_size = 0;
                        t->page = t->column = -1;
                        return NULL;
                    }
                    if (t->mode != DBN_SPARE_ADDRESS || t->cycles) { return "nand_program_pointer_unmodeled"; }
                    t->mode = DBN_PROGRAM_ADDRESS; t->program_size = 0;

    return NULL;
}

static const char *dbn_erase_command(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    bool commit = e->commit;
    if (!t->resets) { return "nand_handoff_state_unmodeled"; }
                    if (*value == 0x60) {
                        t->mode = DBN_ERASE_ADDRESS; t->cycles = 0;
                        t->page = t->column = -1;
                    } else {
                        if (t->mode != DBN_ERASE_READY) { return "nand_erase_confirm_unmodeled"; }
                        unsigned block = t->page / 32, start = block * 32;
                        if (commit) {
                            /* Erase makes all main/spare cells in this block known
                             * FF and starts fresh partial-program history. */
                            memset(t->bytes + start * 16, 0xff, 32 * 16);
                            memset(t->known + start * 16, 1, 32 * 16);
                            memset(t->program_counts + start, 0, 32 * sizeof(*t->program_counts));
                            if (t->copy_enabled) { memset(t->copied_pages + start, 0, 32); }
                            t->erased_blocks[block] = 1;
                            if (t->main_enabled) {
                                memset(t->main_bytes + start * 512, 0xff, 32 * 512);
                                t->main_known_blocks[block] = 1;
                                memset(t->main_program_counts + start, 0, 32);
                            }
                            if (!t->omit_diagnostics) {
                                QDict *operation = qdict_new();
                                qdict_put_int(operation, "page", t->page);
                                qdict_put_int(operation, "block", block);
                                qlist_append(t->erase_operations, operation);
                            }
                        }
                        t->mode = DBN_ERASE_COMPLETE; t->cycles = 0;
                        t->page = t->column = -1;
                    }

    return NULL;
}

static const char *dbn_command(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if ((t->mode == DBN_ERASE_ADDRESS || t->mode == DBN_ERASE_READY) &&
                    *value != 0xff && *value != 0xd0) {
                    return "nand_erase_pending_unmodeled";
                }
                if ((t->mode == DBN_PROGRAM_ADDRESS || t->mode == DBN_PROGRAM_DATA) && *value != 0xff && *value != 0x10) {
                    return "nand_program_pending_unmodeled";
                }
                if ((t->mode == DBN_MAIN_PROGRAM_ADDRESS || t->mode == DBN_MAIN_PROGRAM_DATA) &&
                    *value != 0xff && *value != 0x10) { return "nand_program_pending_unmodeled"; }
                if (t->mode == DBN_COPY_ADDRESS && *value != 0xff) { return "nand_copyback_incomplete_address"; }
                if (*value == 0xff) { return dbn_reset_command(t, e); } else if (*value == 0x90 || *value == 0x70) { return dbn_identify_status(t, e); } else if (*value == 0 && t->main_enabled) { return dbn_main_select(t, e); } else if (*value == 0x8a && t->copy_enabled) { return dbn_copy_begin(t, e); } else if (*value == 0x50) { return dbn_spare_select(t, e); } else if (*value == 0x80) { return dbn_program_begin(t, e); } else if (*value == 0x10) { return dbn_program_confirm(t, e); } else if ((*value == 0x60 || *value == 0xd0) && t->erase_enabled) { return dbn_erase_command(t, e); } else if (*value == 0 || *value == 1 || *value == 0x60 || *value == 0xd0 || *value == 0x8a) {
                    return "nand_array_operation_unmodeled";
                } else { return "nand_command_unmodeled"; }

    return NULL;
}

static const char *dbn_copy_address(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    bool commit = e->commit;
    t->addresses[t->cycles++] = *value;
                    if (t->cycles == 3) {
                        unsigned page = t->addresses[1] | (t->addresses[2] << 8);
                        if (t->addresses[0]) { return "nand_copyback_column_unmodeled"; }
                        if (((unsigned)t->copy_source ^ page) & 0x20) { return "nand_copyback_cross_plane"; }
                        if (!t->main_known_blocks[page / 32]) { return "nand_copyback_unknown_destination"; }
                        if (t->main_program_counts[page]) { return "nand_copyback_requires_erased_main"; }
                        for (unsigned i = 0; i < 528; i++) {
                            if (i < 512) {
                                if (t->main_bytes[page * 512 + i] != 255) { return "nand_copyback_requires_erased_main"; }
                            } else if (!t->known[page * 16 + i - 512]) { return "nand_copyback_unknown_destination_spare"; }
                        }
                        if (commit) {
                            GString *old = t->omit_diagnostics ? NULL : g_string_new(NULL);
                            GString *data = t->omit_diagnostics ? NULL : g_string_new(NULL);
                            GString *copied = t->omit_diagnostics ? NULL : g_string_new(NULL);
                            for (unsigned i = 0; i < 528; i++) {
                                uint8_t *cell = i < 512 ? &t->main_bytes[page * 512 + i] : &t->bytes[page * 16 + i - 512];
                                if (!t->omit_diagnostics) {
                                    g_string_append_printf(old, "%02x", *cell);
                                    g_string_append_printf(data, "%02x", t->program[i]);
                                }
                                *cell &= t->program[i];
                                if (!t->omit_diagnostics) { g_string_append_printf(copied, "%02x", *cell); }
                                if (i < 512 && t->program[i] != 255) { t->erased_blocks[page / 32] = 0; }
                            }
                            if (!t->omit_diagnostics) {
                                QDict *operation = qdict_new();
                                qdict_put_int(operation, "source_page", t->copy_source); qdict_put_int(operation, "page", page);
                                qdict_put_str(operation, "old_hex", old->str); qdict_put_str(operation, "input_hex", data->str);
                                qdict_put_str(operation, "new_hex", copied->str); qlist_append(t->copy_operations, operation);
                                g_string_free(old, true); g_string_free(data, true); g_string_free(copied, true);
                            }
                            t->copied_pages[page] = 1; t->main_program_counts[page]++;
                        }
                        t->mode = DBN_COPY_COMPLETE; t->cycles = t->program_size = 0;
                        t->page = t->column = -1;
                    }

    return NULL;
}

static const char *dbn_main_address(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (!t->cycles) { t->page = -1; }
                    t->addresses[t->cycles++] = *value;
                    if (t->cycles == 3) {
                        t->page = t->addresses[1] | (t->addresses[2] << 8);
                        t->column = t->main_start_column = t->addresses[0];
                        t->mode = t->mode == DBN_MAIN_PROGRAM_ADDRESS ? DBN_MAIN_PROGRAM_DATA : DBN_MAIN_READ;
                    }

    return NULL;
}

static const char *dbn_erase_address(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    t->addresses[t->cycles++] = *value;
                    if (t->cycles == 2) {
                        t->page = t->addresses[0] | (t->addresses[1] << 8);
                        t->mode = DBN_ERASE_READY;
                    }

    return NULL;
}

static const char *dbn_spare_address(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    t->addresses[t->cycles++] = *value;
                    if (t->cycles == 3) {
                        t->page = t->addresses[1] | (t->addresses[2] << 8); t->column = t->addresses[0] & 15;
                        t->mode = t->mode == DBN_PROGRAM_ADDRESS ? DBN_PROGRAM_DATA : DBN_SPARE_READ;
                    }

    return NULL;
}

static const char *dbn_address(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (t->mode == DBN_COPY_ADDRESS) { return dbn_copy_address(t, e); } else if (t->mode == DBN_MAIN_ADDRESS || t->mode == DBN_MAIN_PROGRAM_ADDRESS) { return dbn_main_address(t, e); } else if (t->mode == DBN_ERASE_ADDRESS) { return dbn_erase_address(t, e); } else if (t->mode == DBN_SPARE_ADDRESS || t->mode == DBN_PROGRAM_ADDRESS) { return dbn_spare_address(t, e); } else if (t->mode != DBN_ID_ADDRESS || *value) { return "nand_address_sequence_unmodeled"; }
                else { t->id_address = 0; t->id_cursor = 0; t->mode = DBN_ID; }

    return NULL;
}

static const char *dbn_stage_main(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (t->column + t->program_size >= 528) { return "nand_program_page_bounds"; }
                t->program[t->program_size++] = *value;

    return NULL;
}

static const char *dbn_stage_spare(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (t->column + t->program_size >= 16) { return "nand_program_spare_bounds"; }
                t->program[t->program_size++] = *value;

    return NULL;
}

static const char *dbn_read_id(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (t->id_cursor >= 2) { return "nand_id_read_unmodeled"; }
    *value = t->id_cursor++ ? 0x35 : 0xec; return NULL;
}

static const char *dbn_read_main(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (t->column >= 528) { return "nand_main_sequential_page_unmodeled"; }
    if (t->column < 512) {
        if (!t->main_known_blocks[t->page / 32]) { return "nand_read_unknown_main"; }
        *value = t->main_bytes[t->page * 512 + t->column];
        t->main_reads++;
    } else {
        unsigned index = t->page * 16 + t->column - 512;
        if (!t->known[index]) { return "nand_read_unknown_spare"; }
        *value = t->bytes[index];
    }
    t->column++; return NULL;
}

static const char *dbn_read_spare(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (t->column >= 16) { return "nand_spare_sequential_page_unmodeled"; }
    unsigned index = t->page * 16 + t->column;
    if (!t->known[index]) { return "nand_read_unknown_spare"; }
    *value = t->bytes[index]; t->column++; return NULL;
}

static const char *dbn_read(DB2010Nand *t, const DB2010NandEvent *e)
{
    uint32_t *value = e->value;
    if (!t->read_open) { return "nand_data_gate_closed"; }
    if (t->mode == DBN_ID) { return dbn_read_id(t, e); }
    if (t->mode == DBN_STATUS) { *value = 0xc0; return NULL; }
    if (t->mode == DBN_MAIN_ADDRESS) {
        if (t->cycles || t->page < 0) { return "nand_incomplete_main_address"; }
        t->mode = DBN_MAIN_READ;
    }
    if (t->mode == DBN_MAIN_READ) { return dbn_read_main(t, e); }
    if (t->mode == DBN_SPARE_ADDRESS) { return "nand_incomplete_spare_address"; }
    if (t->mode == DBN_SPARE_READ) { return dbn_read_spare(t, e); }
    return "nand_data_read_unmodeled";
}
static const char *dbn_dispatch(DB2010Nand *t, const DB2010NandEvent *e)
{
    switch (e->kind) {
    case DBN_COMMAND: return dbn_command(t, e);
    case DBN_ADDRESS: return dbn_address(t, e);
    case DBN_READ_DATA: return dbn_read(t, e);
    case DBN_GATE:
        if (*e->value) { return "nand_gate_value_unmodeled"; }
        t->read_open = e->gate_open; return NULL;
    case DBN_STAGE_DATA:
        switch (t->mode) {
        case DBN_MAIN_PROGRAM_DATA: return dbn_stage_main(t, e);
        case DBN_PROGRAM_DATA: return dbn_stage_spare(t, e);
        default: return "nand_data_write_unmodeled";
        }
    default: g_assert_not_reached();
    }
}
static const char *nand_port_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    DB2010NandEvent e = {.kind = a->write ? d->id : DBN_READ_DATA,
                         .value = &a->value, .commit = a->commit};
    if (e.kind == DBN_GATE) { e.gate_open = *(const bool *)d->selector; }
    return dbn_dispatch(opaque, &e);
}
const DB2010RegMap *dbn_map(void)
{
    static const bool gate[] = {false, true};
#define NAND_WRITE .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH8, .callback = nand_port_action, \
    .constraint = {.kind = DB_VALUE_RANGE, .maximum = 255, .fault = "nand_write_value_unmodeled"}}
#define NAND_NO_READ .read = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH8, .fault = "nand_register_read_unmodeled"}
    static const DB2010RegRule children[] = {
        DB_REG(0, .id = DBN_STAGE_DATA, .alignment = 1, .backing_width = 1, NAND_WRITE,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH8, .callback = nand_port_action}),
        DB_REG(8, .id = DBN_ADDRESS, .alignment = 1, NAND_NO_READ, NAND_WRITE),
        DB_REG(0x10, .id = DBN_COMMAND, .alignment = 1, NAND_NO_READ, NAND_WRITE),
        DB_REG(0x20, .id = DBN_GATE, .alignment = 1, .selector = &gate[1], NAND_NO_READ, NAND_WRITE),
        DB_REG(0x24, .id = DBN_GATE, .alignment = 1, .selector = &gate[0], NAND_NO_READ, NAND_WRITE),
    };
    static const DB2010RegRule rules[] = {
        DB_WINDOW(0, 4096, .children = children, .child_count = ARRAY_SIZE(children)),
    };
#undef NAND_WRITE
#undef NAND_NO_READ
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0x50000000, rules,
            ARRAY_SIZE(rules), "nand_access_unmodeled");
        if (fault) { g_error("DB2010 NAND port layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *dbn_access(DB2010Nand *t, uint32_t address, unsigned size,
                       bool write, uint32_t *value, bool commit)
{
    DB2010Access a = {.address = address, .size = size, .write = write,
                      .value = *value, .commit = commit};
    DB2010DecodedAccess d;
    const char *fault = db2010_reg_decode(dbn_map(), &a, &d);
    if (!fault) { fault = db2010_reg_execute(t, &d, &a); }
    *value = a.value;
    return fault;
}

const char *dbn_bus_access(void *opaque, DB2010Access *a)
{
    DB2010Nand *nand = opaque, preview = *nand;
    if (a->write && a->size == 1) { a->value &= 255; }
    return dbn_access(a->commit ? nand : &preview, a->address, a->size,
                      a->write, &a->value, a->commit);
}

void dbn_export(DB2010Nand *model, QDict *snapshot)
{

        DB2010Nand *t = model;
        QDict *nand = qdict_new(), *spare = qdict_new(), *program = qdict_new();
        static const char *modes[] = {"unknown", "idle", "id-address", "id", "status", "spare-address", "spare-read", "spare-program-address", "spare-program-data", "program-complete", "erase-address", "erase-ready", "erase-complete", "main-address", "main-read", "main-program-address", "main-program-data", "copyback-address", "copyback-complete"};
        qdict_put_str(nand, "mode", modes[t->mode]); qdict_put_bool(nand, "read_open", t->read_open);
        if (t->id_address < 0) { qdict_put_null(nand, "id_address"); } else { qdict_put_int(nand, "id_address", t->id_address); }
        qdict_put_int(nand, "id_cursor", t->id_cursor); qdict_put_int(nand, "reset_count", t->resets);
        QList *cycles = qlist_new(); for (unsigned i = 0; i < t->cycles; i++) { qlist_append_int(cycles, t->addresses[i]); }
        qdict_put(spare, "address_cycles", cycles);
        if (t->page < 0) { qdict_put_null(spare, "page"); } else { qdict_put_int(spare, "page", t->page); }
        if (t->column < 0) { qdict_put_null(spare, "column"); } else { qdict_put_int(spare, "column", t->column); }
        GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256); unsigned known = 0;
        for (unsigned p = 0; p < 65536; p++) {
            for (unsigned c = 0; c < 16; c++) {
                unsigned i = p * 16 + c;
                if (t->known[i]) { uint8_t raw[] = {p & 255, p >> 8, c, t->bytes[i]}; g_checksum_update(hash, raw, 4); known++; }
            }
        }
        qdict_put_int(spare, "known_bytes", known); qdict_put_str(spare, "known_contents_sha256", g_checksum_get_string(hash)); g_checksum_free(hash);
        qdict_put(nand, "spare_read", spare);
        QList *buffer = qlist_new(); for (unsigned i = 0; i < t->program_size; i++) { qlist_append_int(buffer, t->program[i]); }
        qdict_put(program, "buffer", buffer); qdict_put(program, "operations", qobject_ref(t->operations));
        QDict *counts = qdict_new();
        for (unsigned p = 0; p < 65536; p++) {
            if (t->program_counts[p]) { g_autofree char *key = g_strdup_printf("%u", p); qdict_put_int(counts, key, t->program_counts[p]); }
        }
        qdict_put(program, "page_program_counts", counts); qdict_put_str(program, "completion", "immediate-ready");
        qdict_put_int(program, "initial_partial_program_counts", 0); qdict_put(nand, "spare_program", program);
        if (t->spare_program_limit == 4) {
            qdict_put_str(program, "profile", "samsung-x8-known-spare-four-pulse-startup");
            qdict_put_int(program, "partial_program_limit", 4);
            qdict_put_int(program, "manufacturer_reference_limit", 3);
            qdict_put_bool(program, "physical_reliability_established", false);
        }
        if (!t->spare_program_limit) {
            QList *over_reference = qlist_new();
            for (unsigned p = 0; p < 65536; p++) {
                if (t->program_counts[p] > 3) { qlist_append_int(over_reference, p); }
            }
            qdict_put_str(program, "profile", "samsung-x8-known-spare-lifecycle-no-wear");
            qdict_put_null(program, "partial_program_limit");
            qdict_put_int(program, "manufacturer_reference_limit", 3);
            qdict_put(program, "pages_over_reference", over_reference);
            qdict_put_bool(program, "physical_reliability_established", false);
        }
        qdict_put(snapshot, "nand", nand);
        if (t->erase_enabled) {
            QDict *erase = qdict_new(); QList *blocks = qlist_new(); unsigned count = 0;
            for (unsigned b = 0; b < 2048; b++) {
                if (t->erased_blocks[b]) { qlist_append_int(blocks, b); count++; }
            }
            qdict_put(erase, "operations", qobject_ref(t->erase_operations));
            qdict_put(erase, "known_main_ff_blocks", blocks);
            qdict_put_int(erase, "known_main_bytes", count * 32 * 512);
            qdict_put_str(erase, "completion", "immediate-ready");
            qdict_put(nand, "block_erase", erase);
        }
        if (t->main_enabled) {
            QDict *main = qdict_new(), *counts = qdict_new(); QList *blocks = qlist_new();
            unsigned known_blocks = 0;
            GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
            for (unsigned b = 0; b < 2048; b++) {
                if (t->main_known_blocks[b]) {
                    uint8_t key[] = {b & 255, b >> 8};
                    g_checksum_update(hash, key, 2);
                    g_checksum_update(hash, t->main_bytes + b * 16384, 16384);
                    qlist_append_int(blocks, b); known_blocks++;
                }
            }
            for (unsigned p = 0; p < 65536; p++) {
                if (t->main_program_counts[p]) {
                    g_autofree char *key = g_strdup_printf("%u", p);
                    qdict_put_int(counts, key, t->main_program_counts[p]);
                }
            }
            qdict_put_str(main, "profile", "samsung-x8-known-main-read-program");
            qdict_put_int(main, "known_bytes", known_blocks * 16384);
            qdict_put(main, "known_blocks", blocks);
            qdict_put_str(main, "known_contents_sha256", g_checksum_get_string(hash));
            g_checksum_free(hash);
            qdict_put_int(main, "data_reads", t->main_reads);
            qdict_put(main, "program_operations", qobject_ref(t->main_operations));
            qdict_put(main, "page_program_counts", counts);
            qdict_put(nand, "main_array", main);
        }
        if (t->copy_enabled) {
            QDict *copy = qdict_new(); QList *pages = qlist_new();
            for (unsigned page = 0; page < 65536; page++) {
                if (t->copied_pages[page]) { qlist_append_int(pages, page); }
            }
            qdict_put_str(copy, "profile", t->copy_no_wear ? "samsung-x8-known-page-copyback-no-wear" : "samsung-x8-known-page-copyback");
            qdict_put(copy, "operations", qobject_ref(t->copy_operations));
            qdict_put(copy, "copied_pages", pages);
            qdict_put_int(copy, "source_page", t->copy_source);
            qdict_put_int(copy, "partial_programs_after_copy", t->partial_after_copy);
            qdict_put_bool(copy, "physical_reliability_established", false);
            qdict_put(nand, "copy_back", copy);
        }
    }
