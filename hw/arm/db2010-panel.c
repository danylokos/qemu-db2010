/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-panel.h"

typedef enum DB2010PanelEventKind {
    DBPANEL_SELECT, DBPANEL_DATA, DBPANEL_VALIDATE_DMA, DBPANEL_BEGIN_STREAM, DBPANEL_PIXEL_DATA,
} DB2010PanelEventKind;
typedef struct DB2010PanelEvent {
    DB2010PanelEventKind kind;
    uint16_t word;
    const uint8_t *data;
    unsigned bytes;
} DB2010PanelEvent;
static const char *dbpanel_dispatch(DB2010Panel *p, const DB2010PanelEvent *e);
const char *dbpanel_packet(DB2010Panel *p, DB2010Pdi *bus)
{
    if (bus->state.packet_size % 2) { return "display_panel_packet_unmodeled"; }
    for (unsigned i = 0; i < bus->state.packet_size; i += 2) {
        if (bus->state.rs[i] != bus->state.rs[i + 1]) { return "display_panel_packet_unmodeled"; }
        DB2010PanelEvent e = {.kind = bus->state.rs[i] ? DBPANEL_DATA : DBPANEL_SELECT,
            .word = (bus->state.packet[i] << 8) | bus->state.packet[i + 1]};
        const char *fault = dbpanel_dispatch(p, &e);
        if (fault) { return fault; }
    }
    return NULL;
}

static const char *dbpanel_validate_dma(DB2010Panel *p, unsigned bytes)
{
    /* Only the selected native profile is supported. Other entry modes,
     * windows and pixel depths need separate evidence before accepting DMA. */
    if (!p->known[3] || !p->known[0x44] || !p->known[0x45] ||
        (p->regs[3] != 0x7230 && p->regs[3] != 0xd230) ||
        p->regs[0x44] != 0xaf00 || p->regs[0x45] != 0xdb00) {
        return "display_panel_geometry_unmodeled";
    }
    if (!p->index_known || p->index != 0x22 || !p->cursor_known ||
        (p->cursor & 3) || /* R03.HWM=1: four-word groups, R61500 pp38/116. */
        (p->cursor & 255) >= 176 || (p->cursor >> 8) >= 220) {
        return "display_panel_cursor_unmodeled";
    }
    if (bytes < 16 || bytes > DBPANEL_PIXELS * 2 || bytes % 16) {
        return "display_panel_dma_count_unmodeled";
    }
    return NULL;
}

static void dbpanel_stream_pixels(DB2010Panel *p, const uint8_t *data, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i += 2) {
        unsigned x = p->cursor & 255, y = p->cursor >> 8, offset = y * 176 + x;
        memcpy(p->pixels + offset * 2, data + i, 2);
        if (!p->pixel_known[offset]) { p->pixel_known[offset] = 1; p->known_pixels++; }
        if (++x == 176) { x = 0; if (++y == 220) { y = 0; } }
        p->cursor = (y << 8) | x;
    }
}

static const char *dbpanel_selected_data(DB2010Panel *p, uint16_t word)
{
    if (p->index == 0x22) { return "display_panel_data_unmodeled"; }
    p->regs[p->index] = word; p->known[p->index] = true;
    if (p->index == 0x21) { p->cursor = word; p->cursor_known = true; }
    return NULL;
}
static const char *dbpanel_dispatch(DB2010Panel *p, const DB2010PanelEvent *e)
{
    switch (e->kind) {
    case DBPANEL_SELECT:
        if (e->word >= G_N_ELEMENTS(p->regs)) { return "display_panel_index_unmodeled"; }
        p->index = e->word; p->index_known = DBPANEL_INDEX_KNOWN; return NULL;
    case DBPANEL_DATA:
        switch (p->index_known) {
        case DBPANEL_INDEX_UNKNOWN: return "display_panel_data_unmodeled";
        case DBPANEL_INDEX_KNOWN: return dbpanel_selected_data(p, e->word);
        default: g_assert_not_reached();
        }
    case DBPANEL_VALIDATE_DMA: return dbpanel_validate_dma(p, e->bytes);
    case DBPANEL_BEGIN_STREAM: p->started_cursor = p->cursor; return NULL;
    case DBPANEL_PIXEL_DATA: dbpanel_stream_pixels(p, e->data, e->bytes); return NULL;
    default: g_assert_not_reached();
    }
}
const char *dbpanel_dma_fault(DB2010Panel *p, unsigned bytes)
{
    DB2010PanelEvent e = {.kind = DBPANEL_VALIDATE_DMA, .bytes = bytes};
    return dbpanel_dispatch(p, &e);
}
void dbpanel_begin_stream(DB2010Panel *p)
{
    DB2010PanelEvent e = {.kind = DBPANEL_BEGIN_STREAM}; dbpanel_dispatch(p, &e);
}
void dbpanel_pixels(DB2010Panel *p, const uint8_t *data, unsigned bytes)
{
    DB2010PanelEvent e = {.kind = DBPANEL_PIXEL_DATA, .data = data, .bytes = bytes};
    dbpanel_dispatch(p, &e);
}
void dbpanel_reset(DB2010Panel *p) { memset(p, 0, sizeof(*p)); }
