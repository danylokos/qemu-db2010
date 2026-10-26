/* Disposable nonblocking presentation channel. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_AUDIO_OUTPUT_H
#define DB2010_AUDIO_OUTPUT_H
#include "chardev/char-fe.h"
#include "db2010-audio-samples.h"
typedef struct DB2010AudioOutput DB2010AudioOutput;
DB2010AudioOutput *db2010_audio_output_new(DB2010AudioSink *sink);
void db2010_audio_output_reset(DB2010AudioOutput *out);
void db2010_audio_output_new_stream(DB2010AudioOutput *out);
char *db2010_audio_output_status(DB2010AudioOutput *out);
void db2010_audio_output_free(DB2010AudioOutput *out);
#endif
