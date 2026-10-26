/* R61500_B aligned high-speed writes, backed by observed R03/44/45 values.
 * Three-byte D230 input is projected to RGB565 for the retained display view.
 * No cold GRAM contents, readback, scan timing or power effects are assumed.
 * References: retained R61500 Rev1.0a pp38,61,76,119 and native PDI packets.
 */
#ifndef HW_ARM_DB2010_PANEL_H
#define HW_ARM_DB2010_PANEL_H
#include "db2010-pdi.h"
#define DBPANEL_PIXELS (176 * 220)
typedef enum DB2010PanelIndexState { DBPANEL_INDEX_UNKNOWN = 0, DBPANEL_INDEX_KNOWN = 1 } DB2010PanelIndexState;
typedef struct DB2010Panel {
    uint16_t regs[0x46], index, cursor, started_cursor;
    bool known[0x46], cursor_known;
    DB2010PanelIndexState index_known;
    uint8_t pixels[DBPANEL_PIXELS * 2], pixel_known[DBPANEL_PIXELS];
    unsigned known_pixels;
} DB2010Panel;

const char *dbpanel_packet(DB2010Panel *p, DB2010Pdi *bus);

const char *dbpanel_dma_fault(DB2010Panel *p, unsigned bytes);

void dbpanel_pixels(DB2010Panel *p, const uint8_t *data, unsigned bytes);

void dbpanel_begin_stream(DB2010Panel *p);
void dbpanel_reset(DB2010Panel *p);
#endif
