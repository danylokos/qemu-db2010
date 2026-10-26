/* Synthetic GSM card adapted from PMB7850 stub; see port provenance.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_SIM_CARD_H
#define DB2010_SIM_CARD_H
#include <stdint.h>
#include <stdbool.h>
#define DBSIM_FILES 19
#define DBSIM_QUEUE 512
typedef struct DB2010SimFile {
    uint16_t parent, id, size;
    int structure;
    uint8_t record_size, data[32];
} DB2010SimFile;
typedef struct DB2010SimCard {
    DB2010SimFile files[DBSIM_FILES];
    uint16_t df, ef;
    uint8_t input[260], output[DBSIM_QUEUE], response[32];
    unsigned input_len, input_need, output_head, output_len, response_len;
    bool input_data, pps;
    uint64_t apdus;
    uint16_t last_sw;
    uint8_t last_header[5];
} DB2010SimCard;
void dbsim_card_init(DB2010SimCard *c);
void dbsim_card_activate(DB2010SimCard *c);
const char *dbsim_card_input(DB2010SimCard *c, uint8_t value);
uint8_t dbsim_card_pop(DB2010SimCard *c);
#endif
