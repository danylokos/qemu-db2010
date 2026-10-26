/* Board assembly for the QOM components. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "db2010-internal.h"

void db2010_qom_create(DB2010MachineState *s)
{
    for (unsigned id = 0; id < DBDEV_PMIC; id++) {
        s->devices[id] = db2010_device_new(OBJECT(s), id);
    }
#define MODEL(field, id) s->field = db2010_device_model(s->devices[id])
    Clock *rtc_source = clock_new(OBJECT(s), "rtc-source");
    Clock *timer_source = clock_new(OBJECT(s), "timer1-source");
    clock_set_hz(rtc_source, 32768);
    clock_set_hz(timer_source, 131072);
    qdev_connect_clock_in(s->devices[DBDEV_RTC], "clk", rtc_source);
    qdev_connect_clock_in(s->devices[DBDEV_TIMER], "clk", timer_source);
    MODEL(keypad, DBDEV_KEYPAD); MODEL(simif, DBDEV_SIMIF);
    MODEL(syscon, DBDEV_SYSCON);
    s->syscon->bank = db2010_device_model(s->devices[DBDEV_BANK]);
    MODEL(bank, DBDEV_BANK); MODEL(power, DBDEV_POWER); MODEL(startup, DBDEV_STARTUP);
    MODEL(uart_port, DBDEV_UART); MODEL(memory_controller, DBDEV_MEMORY);
    MODEL(region_controller, DBDEV_REGIONS); MODEL(absent, DBDEV_ABSENT);
    MODEL(timer, DBDEV_TIMER); MODEL(rtc, DBDEV_RTC); MODEL(rtc_retained, DBDEV_RETAINED);
    MODEL(display, DBDEV_DISPLAY); MODEL(dma, DBDEV_DMA); MODEL(i2c, DBDEV_I2C);
    MODEL(nand, DBDEV_NAND); MODEL(nor, DBDEV_NOR); MODEL(f500_control, DBDEV_F500_CONTROL);
    MODEL(f500_port, DBDEV_F500_PRIMARY); MODEL(f500_aux_port, DBDEV_F500_AUXILIARY);
    MODEL(fa, DBDEV_FA); MODEL(fa_transfer, DBDEV_FA_TRANSFER);
#undef MODEL
    s->devices[DBDEV_PMIC] = db2010_peer_new(s->devices[DBDEV_I2C], DBDEV_PMIC);
    s->devices[DBDEV_FM] = db2010_peer_new(s->devices[DBDEV_I2C], DBDEV_FM);
    s->devices[DBDEV_PANEL] = db2010_peer_new(s->devices[DBDEV_DISPLAY], DBDEV_PANEL);
    s->devices[DBDEV_DSP] = db2010_peer_new(s->devices[DBDEV_FA], DBDEV_DSP);
    s->i2c->pmic = db2010_device_model(s->devices[DBDEV_PMIC]);
    s->i2c->fm = db2010_device_model(s->devices[DBDEV_FM]);
    s->display->panel = db2010_device_model(s->devices[DBDEV_PANEL]);
    s->fa->dsp_peer = db2010_device_model(s->devices[DBDEV_DSP]);
}

static DB2010DeviceId owner_device(DB2010MachineState *s, DB2010Owner *owner)
{
    /* Registration is by contract handler, never a second address decoder. */
    static const struct { DB2010AccessFn access; DB2010DeviceId id; } bindings[] = {
        {dbkey_access, DBDEV_KEYPAD}, {dbsim_access, DBDEV_SIMIF},
        {dbsys_access, DBDEV_SYSCON},
        {db2010_bank_access, DBDEV_BANK}, {db2010_power_access, DBDEV_POWER},
        {db2010_startup_access, DBDEV_STARTUP}, {db2010_uart_access, DBDEV_UART},
        {db2010_controller_access, DBDEV_MEMORY}, {db2010_region_access, DBDEV_REGIONS},
        {db2010_absent_access, DBDEV_ABSENT}, {db2010_timer_access, DBDEV_TIMER},
        {db2010_rtc_access, DBDEV_RTC}, {db2010_retained_access, DBDEV_RETAINED},
        {db2010_display_access, DBDEV_DISPLAY}, {db2010_dma_access, DBDEV_DMA},
        {dbi_bus_access, DBDEV_I2C}, {dbn_bus_access, DBDEV_NAND},
        {db2010_nor_bus_access, DBDEV_NOR}, {dbfc_bus_access, DBDEV_F500_CONTROL},
        {db2010_mailbox_access, DBDEV_FA}, {dbfa_bus_access, DBDEV_FA_TRANSFER},
        {db2010_peer_access, DBDEV_DSP}, {db2010_peer_rx_access, DBDEV_DSP},
    };
    if (owner->access == dbfp_bus_access) {
        return owner->opaque == s->f500_port ? DBDEV_F500_PRIMARY : DBDEV_F500_AUXILIARY;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(bindings); i++) {
        if (bindings[i].access == owner->access) { return bindings[i].id; }
    }
    g_assert_not_reached();
}

void db2010_qom_realize(DB2010MachineState *s)
{
    if (s->has_timer) {
        Clock *source = CLOCK(object_resolve_path_component(OBJECT(s), "timer1-source"));
        clock_update_hz(source, s->timer->config.frequency_hz);
    }
    DB2010TimePort time = {&s->count, &s->timer->state.idle_cycles, &s->instruction_pc};
    for (unsigned i = 0; i < s->bus.owners->len; i++) {
        DB2010Owner *owner = g_ptr_array_index(s->bus.owners, i);
        DB2010DeviceId id = owner_device(s, owner);
        db2010_device_owner(s->devices[id], owner, time);
    }
    for (unsigned i = 0; i < s->mmio_pages->len; i++) {
        DB2010MmioPage *p = g_ptr_array_index(s->mmio_pages, i);
        db2010_device_fallback(s->devices[DBDEV_BANK], p->base, time);
    }
    for (unsigned id = 0; id < DBDEV_PMIC; id++) {
        db2010_device_realize(s->devices[id]);
    }
}

void db2010_qom_connect(DB2010MachineState *s)
{
    DeviceState *timer = s->devices[DBDEV_TIMER];
    s->uart_port->data_cpu = s->uart_port->data_enabled ? CPU(s->cpu) : NULL;
    s->uart_port->data_bank = s->bank;
    s->keypad->cpu = s->has_keypad ? CPU(s->cpu) : NULL;
    db2010_device_connect_irq(s->devices[DBDEV_KEYPAD], 0, timer, DBIRQ_KEYPAD);
    s->keypad->irq = db2010_device_signal(s->devices[DBDEV_KEYPAD], 0);
    db2010_device_connect_irq(s->devices[DBDEV_I2C], 0, timer, DBIRQ_I2C);
    db2010_device_connect_irq(s->devices[DBDEV_DISPLAY], 0, timer, DBIRQ_PDI);
    db2010_device_connect_irq(s->devices[DBDEV_DMA], 0, timer, DBIRQ_DMA);
    db2010_device_connect_irq(s->devices[DBDEV_DSP], 0, timer, DBIRQ_DSP);
    db2010_device_connect_irq(s->devices[DBDEV_RTC], 0, timer, DBIRQ_RTC);
    db2010_device_connect_irq(s->devices[DBDEV_SIMIF], 0, timer, DBIRQ_SIM_RX);
    db2010_device_connect_irq(s->devices[DBDEV_SIMIF], 1, timer, DBIRQ_SIM_TX);
    db2010_device_connect_irq(s->devices[DBDEV_UART], 0, timer, DBIRQ_BLUETOOTH);
    s->uart_port->bluetooth_irq = db2010_device_level_signal(s->devices[DBDEV_UART], 0);
    db2010_device_connect_irq(s->devices[DBDEV_UART], 1, timer, DBIRQ_DATA_UART);
    s->uart_port->data_irq = db2010_device_level_signal(s->devices[DBDEV_UART], 1);
    s->simif->rx_irq = db2010_device_signal(s->devices[DBDEV_SIMIF], 0);
    s->simif->tx_irq = db2010_device_signal(s->devices[DBDEV_SIMIF], 1);
    s->rtc->irq = db2010_device_signal(s->devices[DBDEV_RTC], 0);
    s->display->pdi_irq = db2010_device_signal(s->devices[DBDEV_DISPLAY], 0);
    s->display->dma_irq = db2010_device_level_signal(s->devices[DBDEV_DMA], 0);
    s->fa->dsp_irq = db2010_device_level_signal(s->devices[DBDEV_DSP], 0);
    db2010_device_cpu_irq(timer, qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    s->timer_irq = db2010_device_irq_line(timer, 0);
    s->timer->irq = db2010_device_signal(timer, 0);
    db2010_time_connect(s);
    s->i2c_irq = db2010_device_irq_line(s->devices[DBDEV_I2C], 0);
}

void db2010_qom_reset(DB2010MachineState *s)
{
    for (unsigned id = 0; id < DBDEV_PMIC; id++) {
        if (s->devices[id]->realized) { device_cold_reset(s->devices[id]); }
    }
}
