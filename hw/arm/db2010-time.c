/* Board timing connections. Timers belong to their QOM devices, never to
 * copied access-preview state. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "db2010-internal.h"
#include "hw/core/irq.h"

static int64_t timer_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    return !s->has_timer || s->stop ? -1 : dbtimer_deadline(s->timer);
}
static void timer_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (s->stop) { return; }
    dbtimer_advance(s->timer, db2010_virtual_ns());
    dbtimer_update_irq(s->timer);
}
static int64_t rtc_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    return !s->has_rtc || s->stop ? -1 : dbrtc_deadline(s->rtc);
}
static void rtc_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (s->stop) { return; }
    const char *fault = dbrtc_advance(s->rtc, db2010_virtual_ns());
    if (fault) { db2010_async_stop(s, fault, 0xf9050018, 2); return; }
    dbrtc_update_irq(s->rtc);
}
static int64_t display_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    DB2010Display *d = s->display;
    if (s->stop) { return -1; }
    int64_t due = -1;
    if (s->dma->config.display_frame && d->display_first_burst_done &&
        d->pdi.state.dma_armed && !d->display_completed) { due = d->display_due; }
    if (d->pdi.state.busy_until > db2010_virtual_ns()) {
        due = due < 0 ? d->pdi.state.busy_until : MIN(due, d->pdi.state.busy_until);
    }
    return due;
}
static void display_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (s->stop) { return; }
    db2010_runtime_observe(s);
    display_dma_advance(s->display);
    display_irq_update(s->display);
}
static int64_t i2c_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    return !s->stop && s->has_i2c && s->i2c->state.pending ? s->i2c->state.due : -1;
}
static void i2c_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (s->stop) { return; }
    dbi_finish(s->i2c, db2010_virtual_ns());
    qemu_set_irq(s->i2c_irq, s->i2c->state.irq_pending);
}
static int64_t control_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    return !s->stop && s->has_f500_control && s->f500_control->state.pending ?
        s->f500_control->state.due : -1;
}
static void control_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (!s->stop) { dbfc_advance(s->f500_control, s->f500_control_link.latch, db2010_virtual_ns()); }
}
static int64_t primary_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    return !s->stop && s->has_f500_port && s->f500_port->state.pending ? s->f500_port->state.due : -1;
}
static void primary_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (!s->stop) { dbfp_finish(s->f500_port, db2010_virtual_ns()); }
}
static int64_t auxiliary_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    return !s->stop && s->has_f500_aux_port && s->f500_aux_port->state.pending ? s->f500_aux_port->state.due : -1;
}
static void auxiliary_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (!s->stop) { dbfp_finish(s->f500_aux_port, db2010_virtual_ns()); }
}
static int64_t fa_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    return !s->stop && s->has_fa_transfer && s->fa_transfer->command ? s->fa_transfer->due : -1;
}
static void fa_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (!s->stop) { dbfa_finish(s->fa_transfer, db2010_virtual_ns()); }
}
static int64_t dsp_due(void *opaque)
{
    DB2010MachineState *s = opaque;
    DB2010DspPeer *p = s->fa->dsp_peer;
    return !s->stop && s->fa->has_dsp_peer ? dbdsp_deadline(p) : -1;
}
static void dsp_service(void *opaque)
{
    DB2010MachineState *s = opaque;
    if (s->stop) { return; }
    dsp_peer_advance(s->fa);
    if (!s->stop) { display_irq_update(s->display); }
}
static int64_t sim_due(void *opaque)
{
    DB2010MachineState *s=opaque;
    return !s->stop && s->has_simif ? dbsim_deadline(s->simif) : -1;
}
static void sim_service(void *opaque)
{
    DB2010MachineState *s=opaque;
    if (!s->stop) { dbsim_advance(s->simif,db2010_virtual_ns()); }
}
void db2010_time_connect(DB2010MachineState *s)
{
#define TIMED(id, name) db2010_device_timing(s->devices[id], name##_due, name##_service, s)
    TIMED(DBDEV_TIMER, timer); TIMED(DBDEV_RTC, rtc); TIMED(DBDEV_DISPLAY, display);
    TIMED(DBDEV_I2C, i2c); TIMED(DBDEV_F500_CONTROL, control);
    TIMED(DBDEV_F500_PRIMARY, primary); TIMED(DBDEV_F500_AUXILIARY, auxiliary);
    TIMED(DBDEV_SIMIF, sim);
    TIMED(DBDEV_FA_TRANSFER, fa); TIMED(DBDEV_DSP, dsp);
#undef TIMED
    db2010_device_dependency(s->devices[DBDEV_DMA], s->devices[DBDEV_DISPLAY]);
    db2010_device_dependency(s->devices[DBDEV_FA], s->devices[DBDEV_DSP]);
}
