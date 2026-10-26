#include "qemu/osdep.h"
#include "db2010-internal.h"
#include "qapi/visitor.h"

static void db2010_init(MachineState *machine)
{
    DB2010MachineState *s = DB2010_MACHINE(machine);
    unsigned i;
    ARMCPRegInfo status = {
        .name = "DB2010_TCMTR", .cp = 15, .crn = 0, .crm = 0, .opc1 = 0, .opc2 = 2,
        .access = PL1_R, .type = ARM_CP_OVERRIDE | ARM_CP_CONST, .resetvalue = 0x10001,
    };
    db2010_qom_create(s);
    s->regions = g_ptr_array_new();
    s->bank->registers = g_ptr_array_new();
    s->mmio_pages = g_ptr_array_new();
    s->mmio_index = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->execution = g_array_new(false, false, sizeof(DB2010Interval));
    s->execution_proofs = g_ptr_array_new();
    memory_region_init_io(&s->untranslated_backing, OBJECT(s), &untranslated_ops,
                         s, "db2010.untranslated-only", 1ULL << 32);
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &s->untranslated_backing, -100);
    load_state(s);
    if (s->has_nor) {
        s->nor->research_guards = s->research_guards;
        if (!s->research_guards) {
            db2010_nor_map_unknown(s->nor, OBJECT(s->devices[DBDEV_NOR]));
        }
    }
    db2010_register_owners(s);
    db2010_qom_realize(s);
    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));
    db2010_validate_connections(s);
    db2010_qom_connect(s);
    s->nor->env = &s->cpu->env;
    s->cpu_hooks = (ARMBoardHooks){.opaque = s, .research_guards = s->research_guards,
        .instruction_needed = db2010_cpu_instruction_needed,
        .instruction = db2010_cpu_instruction, .io_access = db2010_cpu_io,
        .exception = db2010_cpu_exception,
                                   .wfi = s->has_timer ? db2010_cpu_wfi : NULL};
    s->cpu->env.board_hooks = &s->cpu_hooks;
    s->hooks = (ARMResearchHooks){ .opaque = s, .instruction = guard_instruction,
                                  .memory = guard_memory, .wfi = s->has_timer ? timer_wfi : NULL,
                                  .exception = guard_exception };
    s->cpu->env.research_hooks = &s->hooks;
    define_one_arm_cp_reg(s->cpu, &status);
    for (i = 0; i < 2; i++) {
        ARMCPRegInfo reg = {
            .name = i ? "DB2010_ITCM" : "DB2010_DTCM", .cp = 15, .crn = 9,
            .crm = 1, .opc1 = 0, .opc2 = i, .access = PL1_RW,
            .type = ARM_CP_OVERRIDE | ARM_CP_NO_RAW, .readfn = tcm_read,
            .writefn = tcm_write,
        };
        s->tcm[i].machine = s;
        s->tcm[i].bank = i;
        define_one_arm_cp_reg(s->cpu, &reg);
    }
    qemu_register_reset(db2010_reset, s);
    db2010_qom_reset(s);
    db2010_reset(s);
    db2010_checkpoint_register(s);
}

static bool get_research_guards(Object *obj, Error **errp)
{
    return DB2010_MACHINE(obj)->research_guards;
}
static void set_research_guards(Object *obj, bool enabled, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    if (s->cpu) { error_setg(errp, "research-guards is immutable after initialization"); return; }
    s->research_guards = enabled;
}

static void db2010_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const cpus[] = { ARM_CPU_TYPE_NAME("arm926"), NULL };
    mc->desc = "Sony Ericsson DB2010 (bounded W800i research contracts)";
    mc->init = db2010_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->valid_cpu_types = cpus;
    mc->max_cpus = 1;
    mc->default_ram_size = 0;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    object_class_property_add_bool(oc, "research-guards", get_research_guards,
                                    set_research_guards);
    object_class_property_add_str(oc, "state", get_state, set_state);
    object_class_property_add_str(oc, "snapshot", get_snapshot, NULL);
    object_class_property_add_str(oc, "summary", get_summary, NULL);
    object_class_property_add_str(oc, "checkpoint-layout", get_checkpoint_layout, NULL);
    object_class_property_add_str(oc, "flash-byte-export", NULL, set_flash_byte_export);
    object_class_property_add_str(oc, "snapshot-chunk", get_snapshot_chunk,
                                  set_snapshot_chunk);
    object_class_property_add_str(oc, "progress", get_progress, NULL);
    object_class_property_add_str(oc, "display", get_display, NULL);
}

static void db2010_finalize(Object *obj)
{
    g_free(DB2010_MACHINE(obj)->snapshot_json);
}

static const TypeInfo db2010_info = {
    .name = TYPE_DB2010_MACHINE, .parent = TYPE_MACHINE,
    .instance_size = sizeof(DB2010MachineState), .class_init = db2010_class_init,
    .instance_finalize = db2010_finalize,
    .interfaces = arm_machine_interfaces,
};

static void db2010_register_types(void)
{
    type_register_static(&db2010_info);
}

type_init(db2010_register_types);
