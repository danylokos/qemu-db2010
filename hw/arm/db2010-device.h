/* QOM lifecycle for the compiled contracts. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_DEVICE_H
#define DB2010_DEVICE_H
#include "hw/core/qdev.h"
#include "db2010-bus.h"
#include "db2010-links.h"

typedef enum DB2010DeviceId {
    DBDEV_BANK, DBDEV_POWER, DBDEV_STARTUP, DBDEV_UART,
    DBDEV_MEMORY, DBDEV_REGIONS, DBDEV_ABSENT, DBDEV_TIMER,
    DBDEV_RTC, DBDEV_RETAINED, DBDEV_DISPLAY, DBDEV_DMA,
    DBDEV_I2C, DBDEV_NAND, DBDEV_NOR, DBDEV_F500_CONTROL,
    DBDEV_F500_PRIMARY, DBDEV_F500_AUXILIARY, DBDEV_FA, DBDEV_FA_TRANSFER,
    DBDEV_KEYPAD, DBDEV_SIMIF, DBDEV_SYSCON, DBDEV_PMIC, DBDEV_FM, DBDEV_PANEL, DBDEV_DSP, DBDEV_COUNT,
} DB2010DeviceId;

DeviceState *db2010_device_new(Object *parent, DB2010DeviceId id);
DeviceState *db2010_peer_new(DeviceState *parent, DB2010DeviceId id);
void *db2010_device_model(DeviceState *device);
void db2010_device_owner(DeviceState *device, DB2010Owner *owner, DB2010TimePort time);
void db2010_device_fallback(DeviceState *device, uint32_t page, DB2010TimePort time);
void db2010_device_realize(DeviceState *device);
qemu_irq db2010_device_irq_line(DeviceState *device, unsigned output);
DB2010Signal db2010_device_signal(DeviceState *device, unsigned output);
DB2010Signal db2010_device_level_signal(DeviceState *device, unsigned output);
MemoryRegion *db2010_device_nor_region(DeviceState *device);
void db2010_device_connect_irq(DeviceState *source, unsigned output,
                               DeviceState *timer, unsigned input);
void db2010_device_cpu_irq(DeviceState *timer, qemu_irq cpu_irq);
typedef int64_t (*DB2010DeadlineFn)(void *opaque);
typedef void (*DB2010ServiceFn)(void *opaque);
void db2010_device_timing(DeviceState *dev, DB2010DeadlineFn deadline,
                          DB2010ServiceFn service, void *opaque);
void db2010_device_reschedule(DeviceState *dev);
void db2010_device_cancel_timer(DeviceState *dev);
void db2010_device_dependency(DeviceState *dev, DeviceState *dependent);
#endif
