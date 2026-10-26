/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-rtc.h"
#include "db2010-bus.h"


bool dbrtc_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = dbrtc_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}

bool dbrtc_irq_contains(uint32_t address, unsigned size)
{
    const DB2010RegMap *map = dbrtc_irq_map();
    return db2010_claims(map->claims, map->claim_count, address, size);
}

static const char *rtc_ticks(const DB2010Rtc *t, uint64_t *ticks)
{
    __int128 total = (__int128)t->config.initial_ticks + t->state.phase.ticks +
                     t->state.counter_adjust_ticks;
    if (total < 0 || total > UINT64_MAX) { return "rtc_clock_overflow_unmodeled"; }
    *ticks = total;
    return NULL;
}

static void minute_rearm(DB2010Rtc *t, uint64_t ticks)
{
    /* Calendar minute rollover; initial alignment is an explicit model
     * assumption derived from the original driver's 60-second API. */
    int64_t seconds = t->state.calendar_adjust_seconds + (int64_t)(ticks / 32768);
    unsigned second = (seconds % 60 + 60) % 60;
    t->state.minute_next_ticks = ticks + (60 - second) * 32768 - ticks % 32768;
}

const char *dbrtc_advance(DB2010Rtc *t, uint64_t now)
{
    db2010_clock_sync(&t->state.phase, t->clock, now);
    uint64_t ticks;
    const char *fault = rtc_ticks(t, &ticks);
    if (fault) { return fault; }
    if (t->config.minute_irq && (t->state.minute_control & 0x80) &&
        t->state.minute_next_ticks && ticks >= t->state.minute_next_ticks) {
        uint64_t periods = 1 + (ticks - t->state.minute_next_ticks) / (60 * 32768);
        t->state.minute_status |= 0x80;
        t->state.minute_events += periods;
        t->state.minute_next_ticks += periods * (60 * 32768);
    }
    return NULL;
}

int64_t dbrtc_deadline(DB2010Rtc *t)
{
    uint64_t ticks;
    if (!t->config.minute_irq || !(t->state.minute_control & 0x80) ||
        !t->state.minute_next_ticks || rtc_ticks(t, &ticks)) { return -1; }
    return db2010_clock_deadline(&t->state.phase, t->clock,
                                t->state.minute_next_ticks - ticks);
}

void dbrtc_update_irq(DB2010Rtc *t)
{
    db2010_signal_set(&t->irq, t->config.minute_irq &&
                     (t->state.minute_control & t->state.minute_status & 0x80));
}

typedef struct DB2010RtcAccessContext {
    DB2010Rtc *rtc;
    uint64_t now;
} DB2010RtcAccessContext;
/* Field identities equal their canonical offsets; unknown accesses retain the
 * same clock/range preflight before their final guarded-register rejection. */
static const char *dbrtc_field(DB2010RtcAccessContext *ctx, unsigned offset,
                               bool write, uint32_t *value)
{
    DB2010Rtc *t = ctx->rtc;
    uint64_t now_ns = ctx->now;
    if (write && offset == 20) {
        t->state.control = *value & 0xffff;
        t->state.control_written = true;
        return NULL;
    }
    const char *fault = dbrtc_advance(t, now_ns);
    if (fault) { return fault; }
    uint64_t ticks;
    fault = rtc_ticks(t, &ticks);
    if (fault) { return fault; }
    if (t->config.minute_irq && offset == 24) {
        if (write) {
            if (*value & ~0x80u) { return "rtc_alarm_control_unmodeled"; }
            t->state.minute_control = *value;
            minute_rearm(t, ticks);
        } else { *value = t->state.minute_control; }
        return NULL;
    }
    if (t->config.minute_irq && offset == 28) {
        if (write) { return "rtc_status_write_unmodeled"; }
        *value = t->state.minute_status;
        t->state.minute_status = 0; /* Inferred original status-collector ACK. */
        return NULL;
    }
    if (write && offset == 2 && t->config.calendar_writable && *value == 0) {
        /* Original setter clears only high fractional divider bits. */
        t->state.counter_adjust_ticks -= (uint64_t)ticks & 0x7f00;
        minute_rearm(t, ticks - (ticks & 0x7f00));
        return NULL;
    }
    if (write && !t->config.calendar_writable) { return "rtc_counter_write_unmodeled"; }
    if (write && offset != 4 && offset != 6 && offset != 8 && offset != 12 &&
        offset != 14 && offset != 16 && offset != 18) {
        return "rtc_counter_write_unmodeled";
    }
    if (write && (*value > 99 ||
        ((offset == 4 || offset == 6) && *value > 59) ||
        (offset == 8 && *value > 23) ||
        (offset == 12 && (*value < 1 || *value > 31)) ||
        (offset == 14 && (*value < 1 || *value > 12)))) {
        return "rtc_calendar_value_unmodeled";
    }
    if (offset == 0) { *value = (uint64_t)ticks & 255; return NULL; }
    if (offset == 2) { *value = ((uint64_t)ticks >> 8) & 127; return NULL; }
    if (offset == 20) { *value = t->state.control; return NULL; }
    if (offset == 30) { *value = t->config.status; return NULL; }
    g_autoptr(GDateTime) epoch = g_date_time_new_utc(2000, 1, 1, 0, 0, 0);
    g_autoptr(GDateTime) now = g_date_time_add_seconds(epoch,
        t->state.calendar_adjust_seconds + (int64_t)((uint64_t)ticks / 32768));
    if (!now) {
        return "rtc_calendar_range_unmodeled";
    }
    if (write) {
        int year = g_date_time_get_year(now), month = g_date_time_get_month(now);
        int day = g_date_time_get_day_of_month(now), hour = g_date_time_get_hour(now);
        int minute = g_date_time_get_minute(now), second = g_date_time_get_second(now);
        switch (offset) {
        case 4: second = *value; break;
        case 6: minute = *value; break;
        case 8: hour = *value; break;
        case 12: day = *value; break;
        case 14: month = *value; break;
        case 16: year = year / 100 * 100 + *value; break;
        case 18: year = *value * 100 + year % 100; break;
        }
        g_autoptr(GDateTime) next = g_date_time_new_utc(year, month, day, hour, minute, second);
        if (!next) { return "rtc_calendar_value_unmodeled"; }
        t->state.calendar_adjust_seconds = g_date_time_difference(next, epoch) / G_TIME_SPAN_SECOND -
                                    (int64_t)((uint64_t)ticks / 32768);
        minute_rearm(t, ticks);
        return NULL;
    }
    switch (offset) {
    case 4: *value = g_date_time_get_second(now); break;
    case 6: *value = g_date_time_get_minute(now); break;
    case 8: *value = g_date_time_get_hour(now); break;
    case 12: *value = g_date_time_get_day_of_month(now); break;
    case 14: *value = g_date_time_get_month(now); break;
    case 16: *value = g_date_time_get_year(now) % 100; break;
    case 18: *value = g_date_time_get_year(now) / 100; break;
    default: return "rtc_counter_access_unmodeled";
    }
    return NULL;
}
static const char *rtc_action(void *opaque, const DB2010DecodedAccess *d, DB2010Access *a)
{
    return dbrtc_field(opaque, d->id, a->write, &a->value);
}
const DB2010RegMap *dbrtc_irq_map(void)
{
    static const DB2010RegRule rules[] = {
        DB_REG(24, .id = 24, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = rtc_action},
            .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = rtc_action}),
        DB_REG(28, .id = 28, .alignment = 2,
            .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = rtc_action},
            .write = {.kind = DB_ACTION_REJECT, .widths = DB_WIDTH16, .fault = "rtc_status_write_unmodeled"}),
    };
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf9050000, rules,
            ARRAY_SIZE(rules), "rtc_minute_access_unmodeled");
        if (fault) { g_error("DB2010 minute RTC layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const DB2010RegMap *dbrtc_map(void)
{
#define RTC_WORD(off) DB_REG(off, .id = off, .alignment = 2, \
    .read = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = rtc_action}, \
    .write = {.kind = DB_ACTION_CALLBACK, .widths = DB_WIDTH16, .callback = rtc_action})
    static const DB2010RegRule rules[] = {
        RTC_WORD(0), RTC_WORD(2), RTC_WORD(4), RTC_WORD(6), RTC_WORD(8),
        RTC_WORD(12), RTC_WORD(14), RTC_WORD(16), RTC_WORD(18),
        RTC_WORD(20), RTC_WORD(30),
    };
#undef RTC_WORD
    static DB2010RegMap map;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        const char *fault = db2010_regmap_init(&map, 0xf9050000, rules,
            ARRAY_SIZE(rules), "rtc_counter_access_unmodeled");
        if (fault) { g_error("DB2010 calendar RTC layout: %s", fault); }
        g_once_init_leave(&initialized, 1);
    }
    return &map;
}
const char *dbrtc_access(DB2010Rtc *t, uint64_t now,
                         uint64_t idle, uint32_t address, unsigned size,
                         bool write, uint32_t *value)
{
    DB2010Access a = {.address = address, .size = size, .write = write,
                      .value = *value, .commit = true};
    DB2010Rtc preview = *t;
    DB2010RtcAccessContext ctx = {&preview, now};
    DB2010DecodedAccess d;
    const DB2010RegMap *map = t->config.minute_irq &&
        dbrtc_irq_contains(address, size) ? dbrtc_irq_map() : dbrtc_map();
    const char *fault = db2010_reg_decode(map, &a, &d);
    if (fault) {
        if (size != 2 || address % 2) { return fault; }
        /* Unknown aligned fields still undergo clock/range validation. */
        return dbrtc_field(&ctx, UINT_MAX, write, value);
    }
    fault = db2010_reg_execute(&ctx, &d, &a);
    *value = a.value;
    if (!fault) { t->state = preview.state; }
    return fault;
}

void dbrtc_reset(DB2010Rtc *d)
{
    d->state = (DB2010RtcRuntime){.control = d->config.initial_control, .phase.ns = db2010_virtual_ns()};
    dbrtc_update_irq(d);
}

void dbrtc_export(DB2010Rtc *model, QDict *snapshot, uint64_t now, uint64_t idle)
{

        DB2010Rtc preview = *model;
        model = &preview;
        dbrtc_advance(model, now);
        QDict *rtc = qdict_new();
        qdict_put_int(rtc, "control", model->state.control);
        qdict_put_bool(rtc, "control_written", model->state.control_written);
        qdict_put_int(rtc, "status", model->config.status);
        if (model->config.minute_irq) {
            qdict_put_int(rtc, "minute_control", model->state.minute_control);
            qdict_put_int(rtc, "minute_status", model->state.minute_status);
            qdict_put_int(rtc, "minute_events", model->state.minute_events);
            qdict_put_int(rtc, "minute_deadline_ns", dbrtc_deadline(model));
        }
        if (model->config.calendar_writable) {
            qdict_put_int(rtc, "calendar_adjust_seconds", model->state.calendar_adjust_seconds);
            qdict_put_int(rtc, "counter_adjust_ticks", model->state.counter_adjust_ticks);
            static const unsigned offsets[] = {4,6,8,12,14,16,18};
            static const char *names[] = {"second","minute","hour","day","month","year_low","century"};
            QDict *calendar = qdict_new();
            for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
                uint32_t value = 0;
                dbrtc_access(model, now, 0,
                             0xf9050000 + offsets[i], 2, false, &value);
                qdict_put_int(calendar, names[i], value);
            }
            qdict_put(rtc, "calendar", calendar);
        }
        qdict_put(snapshot, "rtc", rtc);
    }
