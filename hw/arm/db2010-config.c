#include "qemu/osdep.h"
#include "db2010-internal.h"

uint64_t input_uint(QDict *dict, const char *key, uint64_t max)
{
    uint64_t value;
    QNum *number = qobject_to(QNum, qdict_get(dict, key));
    if (!number || !qnum_get_try_uint(number, &value) || value > max) {
        error_report("db2010 prepared state: invalid/missing unsigned %s", key);
        exit(EXIT_FAILURE);
    }
    return value;
}

uint32_t input_nullable_word(QDict *dict, const char *key, uint32_t max,
                                    bool *known)
{
    QObject *object = qdict_get(dict, key);
    if (object && qobject_type(object) == QTYPE_QNULL) {
        *known = false;
        return 0;
    }
    *known = true;
    return input_uint(dict, key, max);
}

bool only_keys(QDict *dict, const char *const *keys)
{
    const QDictEntry *entry;
    for (entry = qdict_first(dict); entry; entry = qdict_next(dict, entry)) {
        unsigned i;
        for (i = 0; keys[i] && strcmp(keys[i], qdict_entry_key(entry)); i++) {
        }
        if (!keys[i]) {
            return false;
        }
    }
    return true;
}

DB2010MmioPage *find_mmio_page(DB2010MachineState *s, uint32_t address)
{
    return g_hash_table_lookup(s->mmio_index, GUINT_TO_POINTER(address >> 12));
}

void declare_mmio_page(DB2010MachineState *s, uint32_t address)
{
    DB2010MmioPage *p;
    if (find_mmio_page(s, address)) {
        return;
    }
    p = g_new0(DB2010MmioPage, 1);
    p->base = address & ~0xfff;
    g_ptr_array_add(s->mmio_pages, p);
    g_hash_table_insert(s->mmio_index, GUINT_TO_POINTER(address >> 12), p);
}

void load_register_contracts(DB2010MachineState *s, QDict *state)
{
    static const char *const keys[] = { "address", "width", "name", "initial_value",
        "read_or_mask", "access", "allowed_writes", "read_or_when", "rtc_halfword_alias", "f500_lowbyte_alias", NULL };
    QListEntry *entry;
    unsigned i;
    if (qdict_haskey(state, "power_hold")) {
        QDict *spec = qobject_to(QDict, qdict_get(state, "power_hold"));
        static const char *const power_keys[] = { "address", "initial_value", "rtc_init_mask", "startup_release", NULL };
        if (!spec || !only_keys(spec, power_keys) ||
            input_uint(spec, "address", UINT32_MAX) != 0xf9050058) {
            error_report("db2010: unsupported power-hold contract");
            exit(EXIT_FAILURE);
        }
        s->power->initial_power = input_nullable_word(spec, "initial_value", 0xffff,
                                               &s->power->initial_power_known);
        s->power->power_mask = qdict_haskey(spec, "rtc_init_mask") ?
                        input_uint(spec, "rtc_init_mask", 0xffff) : 0;
        if (s->power->power_mask != 0 && s->power->power_mask != 0x8c) {
            error_report("db2010: unsupported power-hold RTC mask");
            exit(EXIT_FAILURE);
        }
        if (qdict_haskey(spec, "startup_release")) {
            const char *profile = qdict_get_try_str(spec, "startup_release");
            if (!profile || strcmp(profile, "capture-once-without-supply-effect")) {
                error_report("db2010: unsupported startup hold-release hypothesis"); exit(EXIT_FAILURE);
            }
            s->power->allow_startup_release = true;
        }
        s->power->power_mask |= 1;
        s->has_power_hold = true;
        declare_mmio_page(s, 0xf9050058);
    }
    if (qdict_haskey(state, "device_registers")) {
        QList *specs = qobject_to(QList, qdict_get(state, "device_registers"));
        if (!specs || qlist_size(specs) > 512) {
            error_report("db2010: expected bounded exact register specifications");
            exit(EXIT_FAILURE);
        }
        QLIST_FOREACH_ENTRY(specs, entry) {
            QDict *spec = qobject_to(QDict, qlist_entry_obj(entry));
            DB2010Register *r = g_new0(DB2010Register, 1);
            uint32_t max;
            const char *name, *access;
            if (!spec || !only_keys(spec, keys) || !(name = qdict_get_try_str(spec, "name"))) {
                error_report("db2010: invalid exact register specification");
                exit(EXIT_FAILURE);
            }
            r->name = g_strdup(name);
            r->address = input_uint(spec, "address", UINT32_MAX);
            r->width = input_uint(spec, "width", 4);
            r->rtc_halfword_alias = qdict_get_try_bool(spec, "rtc_halfword_alias", false);
            r->f500_lowbyte_alias = qdict_get_try_bool(spec, "f500_lowbyte_alias", false);
            if (r->f500_lowbyte_alias &&
                ((r->address != 0xf5000200 && r->address != 0xf5000000) || r->width != 2)) {
                error_report("db2010: F500 low-byte alias requires its exact halfword owner");
                exit(EXIT_FAILURE);
            }
            if (r->rtc_halfword_alias && (r->address != 0xf9050054 || r->width != 1)) {
                error_report("db2010: RTC halfword alias is scoped to the byte marker");
                exit(EXIT_FAILURE);
            }
            if ((r->width != 1 && r->width != 2 && r->width != 4) || r->address % r->width) {
                error_report("db2010: unsupported register width/alignment");
                exit(EXIT_FAILURE);
            }
            for (i = 0; i < s->bank->registers->len; i++) {
                DB2010Register *other = g_ptr_array_index(s->bank->registers, i);
                if (r->address < (uint64_t)other->address + other->width &&
                    other->address < (uint64_t)r->address + r->width) {
                    error_report("db2010: overlapping exact register specifications");
                    exit(EXIT_FAILURE);
                }
            }
            if (s->has_simif && (r->address & ~0xfff)==0xf9060000) {
                error_report("db2010: generic register overlaps SIMIF ownership"); exit(EXIT_FAILURE);
            }
            if (s->has_power_hold && r->address < 0xf905005a &&
                (uint64_t)r->address + r->width > 0xf9050058) {
                error_report("db2010: generic register overlaps power-hold ownership");
                exit(EXIT_FAILURE);
            }
            if (s->has_timer && dbtimer_contains(r->address, r->width)) {
                error_report("db2010: generic register overlaps timer ownership");
                exit(EXIT_FAILURE);
            }
            if (s->has_rtc && (dbrtc_contains(r->address, r->width) ||
                (s->rtc->config.minute_irq && dbrtc_irq_contains(r->address, r->width)))) {
                error_report("db2010: generic register overlaps RTC ownership");
                exit(EXIT_FAILURE);
            }
            if (s->has_startup_status && startup_status_contains(r->address, r->width)) {
                error_report("db2010: generic register overlaps startup status ownership"); exit(EXIT_FAILURE);
            }
            if (s->has_rtc_retained && dbrtr_contains(r->address, r->width)) {
                error_report("db2010: generic register overlaps RTC retained ownership");
                exit(EXIT_FAILURE);
            }
            if (s->display->has_pdi && dbpdi_contains(r->address, r->width)) {
                error_report("db2010: generic register overlaps PDI ownership"); exit(EXIT_FAILURE);
            }
            if (s->has_f500_port && dbfp_contains(r->address, r->width, &dbfp_primary)) {
                error_report("db2010: generic register overlaps F500 command port"); exit(EXIT_FAILURE);
            }
            if (s->has_f500_aux_port && dbfp_contains(r->address, r->width, &dbfp_auxiliary)) {
                error_report("db2010: generic register overlaps F500 command port"); exit(EXIT_FAILURE);
            }
            if (s->fa->has_dsp_peer && (dbdsp_contains(r->address, r->width) ||
                (s->fa->dsp_peer->config.modexp && db2010_claims(dbdsp_rx_map()->claims,
                  dbdsp_rx_map()->claim_count, r->address, r->width)))) {
                error_report("db2010: generic register overlaps DSP peer event ownership"); exit(EXIT_FAILURE);
            }
            if (s->fa->has_fa_mailbox && dbfm_contains(r->address, r->width)) {
                error_report("db2010: generic register overlaps FA mailbox ownership"); exit(EXIT_FAILURE);
            }
            if (s->has_fa_transfer && dbfa_contains(r->address, r->width)) {
                error_report("db2010: generic register overlaps FA transfer ownership"); exit(EXIT_FAILURE);
            }
            if (s->has_dma && (r->address & ~0xfff) == 0xf2000000) {
                error_report("db2010: generic register overlaps PL080 ownership"); exit(EXIT_FAILURE);
            }
            if (s->has_i2c && (r->address & ~0xfff) == 0xf9040000) {
                error_report("db2010: generic register overlaps I2C ownership"); exit(EXIT_FAILURE);
            }
            if (s->has_nand && (r->address & ~0xfff) == 0x50000000) {
                error_report("db2010: generic register overlaps NAND ownership"); exit(EXIT_FAILURE);
            }
            max = UINT32_MAX >> (8 * (4 - r->width));
            r->initial = input_nullable_word(spec, "initial_value", max, &r->initial_known);
            r->read_mask = input_uint(spec, "read_or_mask", max);
            access = qdict_get_try_str(spec, "access");
            if (qdict_haskey(spec, "access") &&
                (!access || (strcmp(access, "rw") && strcmp(access, "write-only") && strcmp(access, "read-only")))) {
                error_report("db2010: unsupported register access profile");
                exit(EXIT_FAILURE);
            }
            r->write_only = access && !strcmp(access, "write-only");
            r->read_only = access && !strcmp(access, "read-only");
            if (qdict_haskey(spec, "allowed_writes")) {
                QList *allowed = qobject_to(QList, qdict_get(spec, "allowed_writes"));
                QListEntry *word_entry;
                if (!allowed || !qlist_size(allowed) || qlist_size(allowed) > 256) {
                    error_report("db2010: invalid allowed command set");
                    exit(EXIT_FAILURE);
                }
                r->allowed_writes = g_array_new(false, false, sizeof(uint32_t));
                QLIST_FOREACH_ENTRY(allowed, word_entry) {
                    uint64_t word;
                    QNum *n = qobject_to(QNum, qlist_entry_obj(word_entry));
                    if (!n || !qnum_get_try_uint(n, &word) || word > max) {
                        error_report("db2010: command exceeds register width");
                        exit(EXIT_FAILURE);
                    }
                    uint32_t value = word;
                    g_array_append_val(r->allowed_writes, value);
                }
            }
            if (qdict_haskey(spec, "read_or_when")) {
                QDict *condition = qobject_to(QDict, qdict_get(spec, "read_or_when"));
                if (!condition || qdict_size(condition) != 2 || !r->read_mask) {
                    error_report("db2010: invalid conditional mask");
                    exit(EXIT_FAILURE);
                }
                r->condition_address = input_uint(condition, "address", UINT32_MAX);
                r->condition_value = input_uint(condition, "value", UINT32_MAX);
                r->conditional = true;
            }
            g_ptr_array_add(s->bank->registers, r);
            declare_mmio_page(s, r->address);
        }
    }
    for (i = 0; i < s->bank->registers->len; i++) {
        DB2010Register *r = g_ptr_array_index(s->bank->registers, i);
        if (r->conditional) {
            DB2010Register *condition = find_register(s, r->condition_address);
            if (!condition || r->condition_value > (UINT32_MAX >> (8 * (4 - condition->width)))) {
                error_report("db2010: conditional mask references unsupported register/value");
                exit(EXIT_FAILURE);
            }
        }
    }
    for (i = 0; i < s->mmio_pages->len; i++) {
        DB2010MmioPage *p = g_ptr_array_index(s->mmio_pages, i);
        if (find_region(s, p->base, 1) ||
            (s->has_controller && p->base == 0x14000000) ||
            (s->has_region_controls && p->base == 0xfe004000)) {
            error_report("db2010: configured MMIO page overlaps other owner");
            exit(EXIT_FAILURE);
        }
    }
}

/* One peripheral-controlled PL080 burst, after native R61500 GRAM selection.
 * Known black source data avoids assuming the unproven nonzero color-bit map.
 * The next burst/completion/IRQ remains an explicit model boundary. */
void load_state(DB2010MachineState *s)
{
    g_autofree char *text = NULL;
    g_autoptr(QDict) state = NULL;
    QObject *object;
    GError *error = NULL;
    QList *regions, *ranges;
    QListEntry *entry;
    unsigned i;
    if (!s->state_file || !g_file_get_contents(s->state_file, &text, NULL, &error)) {
        error_report("db2010 requires state=<host-prepared JSON file>: %s",
                     error ? error->message : "no file supplied");
        exit(EXIT_FAILURE);
    }
    object = qobject_from_json(text, &error_fatal);
    state = qobject_to(QDict, object);
    if (!state || input_uint(state, "format_version", 1) != 1) {
        error_report("db2010: unsupported prepared state format");
        exit(EXIT_FAILURE);
    }
    if (qdict_haskey(state, "research_guards") &&
        qdict_get_bool(state, "research_guards") != s->research_guards) {
        error_report("db2010: prepared research guard mode differs from machine property"); exit(EXIT_FAILURE);
    }
    s->entry = input_uint(state, "entry", UINT32_MAX);
    s->cpsr = input_uint(state, "cpsr", UINT32_MAX);
    s->sctlr = input_uint(state, "sctlr", UINT32_MAX);
    QDict *timebase = qobject_to(QDict, qdict_get(state, "timebase"));
    if (!timebase || qdict_size(timebase) != 2 ||
        g_strcmp0(qdict_get_try_str(timebase, "profile"), "qemu-virtual-v1") ||
        input_uint(timebase, "icount_shift", 10) != 6) {
        error_report("db2010: requires qemu-virtual-v1 / icount shift6; archived instruction clocks require their frozen runner and QEMU source bundle");
        exit(EXIT_FAILURE);
    }
    s->instruction_limited = qdict_haskey(state, "instruction_limit");
    s->limit = s->instruction_limited ? input_uint(state, "instruction_limit", UINT64_MAX) : 0;
    if (s->instruction_limited && !s->research_guards) {
        error_report("db2010: instruction_limit requires research-guards=on"); exit(EXIT_FAILURE);
    }
    if (qdict_haskey(state, "recording_history")) {
        QDict *history = qobject_to(QDict, qdict_get(state, "recording_history"));
        const char *profile = history ? qdict_get_try_str(history, "profile") : NULL;
        if (!history || qdict_size(history) != 1 || !profile ||
            strcmp(profile, "startup64-recent64-irq512")) {
            error_report("db2010: unsupported recording history policy"); exit(EXIT_FAILURE);
        }
        s->bounded_history = true;
    }
    if (qdict_haskey(state, "dtcm_reset_record")) {
        QDict *record = qobject_to(QDict, qdict_get(state, "dtcm_reset_record"));
        const char *profile = record ? qdict_get_try_str(record, "profile") : NULL;
        if (!record || qdict_size(record) != 4 || !profile ||
            strcmp(profile, "cold-zero-reset-record") ||
            input_uint(record, "address", UINT32_MAX) != 0x42000604 ||
            input_uint(record, "size", UINT32_MAX) != 28 ||
            input_uint(record, "fill", 255) != 0) {
            error_report("db2010: unsupported DTCM reset-record cold input");
            exit(EXIT_FAILURE);
        }
        s->cold_zero_reset_record = true;
    }
    if (qdict_haskey(state, "f500_control")) {
        QDict *fc = qobject_to(QDict, qdict_get(state, "f500_control"));
        const char *profile = fc ? qdict_get_try_str(fc, "profile") : NULL;
        if (!fc || qdict_size(fc) != 2 || !profile ||
            strcmp(profile, "bit10-self-clear-latency")) {
            error_report("db2010: unsupported F500 control profile"); exit(EXIT_FAILURE);
        }
        unsigned latency = input_uint(fc, "latency_ns", 65536);
        if (!latency) {
            error_report("db2010: F500 control latency must be positive"); exit(EXIT_FAILURE);
        }
        s->has_f500_control = true;
        s->f500_control->config.latency = latency;
    }
    if (qdict_haskey(state, "f500_command_port")) {
        QDict *port = qobject_to(QDict, qdict_get(state, "f500_command_port"));
        const char *profile = port ? qdict_get_try_str(port, "profile") : NULL;
        if (!port || qdict_size(port) != 2 || !profile ||
            strcmp(profile, "word-ready-strobe-latency")) {
            error_report("db2010: unsupported F500 command-port profile"); exit(EXIT_FAILURE);
        }
        unsigned latency = input_uint(port, "latency_ns", 65536);
        if (!latency) { error_report("db2010: F500 port latency must be positive"); exit(EXIT_FAILURE); }
        s->has_f500_port = true;
        s->f500_port->config = (DB2010F500PortConfig){ &dbfp_primary, latency };
        declare_mmio_page(s, 0xf5000000);
    }
    if (qdict_haskey(state, "f500_aux_port")) {
        QDict *port = qobject_to(QDict, qdict_get(state, "f500_aux_port"));
        const char *profile = port ? qdict_get_try_str(port, "profile") : NULL;
        if (!port || qdict_size(port) != 2 || !profile ||
            strcmp(profile, "lowtag-ready-strobe-latency")) {
            error_report("db2010: unsupported F500 command-port profile"); exit(EXIT_FAILURE);
        }
        unsigned latency = input_uint(port, "latency_ns", 65536);
        if (!latency) { error_report("db2010: F500 port latency must be positive"); exit(EXIT_FAILURE); }
        s->has_f500_aux_port = true;
        s->f500_aux_port->config = (DB2010F500PortConfig){ &dbfp_auxiliary, latency };
        declare_mmio_page(s, 0xf5000000);
    }
    if (qdict_haskey(state, "f500_command_capture")) {
        QDict *f500 = qobject_to(QDict, qdict_get(state, "f500_command_capture"));
        const char *profile = f500 ? qdict_get_try_str(f500, "profile") : NULL;
        if (!f500 || qdict_size(f500) != 1 || !profile || strcmp(profile, "081c-halfword-zero-command-capture")) {
            error_report("db2010: unsupported F500 profile"); exit(EXIT_FAILURE);
        }
        s->bank->has_f500 = true;
    }
    if (qdict_haskey(state, "fa_mailbox_tx")) {
        QDict *m = qobject_to(QDict, qdict_get(state, "fa_mailbox_tx"));
        const char *profile = m ? qdict_get_try_str(m, "profile") : NULL;
        if (!m || qdict_size(m) != 1 || !profile || strcmp(profile, "outbound-single-slot")) {
            error_report("db2010: unsupported FA mailbox profile"); exit(EXIT_FAILURE);
        }
        s->fa->has_fa_mailbox = true; declare_mmio_page(s, 0xfa040000);
    }
    if (qdict_haskey(state, "fa_transfer")) {
        QDict *fa = qobject_to(QDict, qdict_get(state, "fa_transfer"));
        const char *profile = fa ? qdict_get_try_str(fa, "profile") : NULL;
        QList *seeds = fa ? qobject_to(QList, qdict_get(fa, "initial_halfwords")) : NULL;
        if (!fa || qdict_size(fa) != 2 || !profile ||
            strcmp(profile, "word-pairs-128ns") || !seeds) {
            error_report("db2010: unsupported FA transfer profile"); exit(EXIT_FAILURE);
        }
        s->has_fa_transfer = true;
        s->fa_transfer->memory = g_hash_table_new(g_direct_hash, g_direct_equal);
        s->fa_transfer->initial = g_hash_table_new(g_direct_hash, g_direct_equal);
        QLIST_FOREACH_ENTRY(seeds, entry) {
            QDict *seed = qobject_to(QDict, qlist_entry_obj(entry));
            if (!seed || qdict_size(seed) != 2) {
                error_report("db2010: invalid FA initial halfword"); exit(EXIT_FAILURE);
            }
            uint32_t address = input_uint(seed, "address", 0x7fffff);
            uint32_t value = input_uint(seed, "value", 65535);
            gpointer key = GUINT_TO_POINTER(address + 1);
            if (g_hash_table_contains(s->fa_transfer->initial, key)) {
                error_report("db2010: duplicate FA initial halfword"); exit(EXIT_FAILURE);
            }
            g_hash_table_insert(s->fa_transfer->initial, key, GUINT_TO_POINTER(value + 1));
        }
        declare_mmio_page(s, 0xfa040000);
    }
    if (qdict_haskey(state, "synthetic_etx")) {
        QDict *etx = qobject_to(QDict, qdict_get(state, "synthetic_etx"));
        const char *profile = etx ? qdict_get_try_str(etx, "profile") : NULL;
        const char *hex = etx ? qdict_get_try_str(etx, "expected_request_hex") : NULL;
        if (!etx || qdict_size(etx) != 2 || !profile ||
            strcmp(profile, "gdfs-static-8040-status-success") || !hex || strlen(hex) != 164) {
            error_report("db2010: unsupported synthetic ETX profile"); exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < 164; i++) {
            if (!strchr("0123456789abcdef", hex[i])) {
                error_report("db2010: invalid synthetic ETX request"); exit(EXIT_FAILURE);
            }
            s->etx_request[i / 2] = (s->etx_request[i / 2] << 4) | g_ascii_xdigit_value(hex[i]);
        }
        if (lduw_le_p(s->etx_request + 20) != 60) {
            error_report("db2010: unsupported synthetic ETX payload length"); exit(EXIT_FAILURE);
        }
        s->synthetic_etx = true;
    }
    if (qdict_haskey(state, "synthetic_sha")) {
        QDict *sha = qobject_to(QDict, qdict_get(state, "synthetic_sha"));
        if (!sha || qdict_size(sha) != 1 || !qdict_get_try_str(sha,"profile") ||
            strcmp(qdict_get_try_str(sha,"profile"), "sha1-le92-init-update-final-8040")) {
            error_report("db2010: unsupported synthetic SHA profile"); exit(EXIT_FAILURE);
        }
        s->synthetic_sha = true;
    }
    if (qdict_haskey(state, "synthetic_kgen")) {
        QDict *kgen = qobject_to(QDict, qdict_get(state, "synthetic_kgen"));
        const char *profile = kgen ? qdict_get_try_str(kgen, "profile") : NULL;
        if (!kgen || qdict_size(kgen) != 1 || !profile ||
            (strcmp(profile, "sha1-le92-init-update-8040") &&
             strcmp(profile, "sha1-le92-init-update-final-8040"))) {
            error_report("db2010: unsupported synthetic KGEN profile"); exit(EXIT_FAILURE);
        }
        s->synthetic_kgen = true;
        s->kgen_final = !strcmp(profile, "sha1-le92-init-update-final-8040");
    }
    if (qdict_haskey(state, "nand")) {
        QDict *nand = qobject_to(QDict, qdict_get(state, "nand"));
        const char *profile = nand ? qdict_get_try_str(nand, "profile") : NULL;
        const char *completion = nand ? qdict_get_try_str(nand, "completion") : NULL;
        const char *seed = nand ? qdict_get_try_str(nand, "prepared_spare") : NULL;
        const char *keys[] = {"spare_read", "spare_program", "synthetic_spare"};
        const char *profiles[] = {"samsung-x8-read2-unknown-contents", "samsung-x8-known-spare-byte-program", "first-erase-block-ff-valid-footer"};
        bool erase = nand && qdict_haskey(nand, "block_erase");
        bool main = nand && qdict_haskey(nand, "main_array");
        bool copy = nand && qdict_haskey(nand, "copy_back");
        bool main_image = nand && qdict_haskey(nand, "nand_main_image");
        bool spare_image = nand && qdict_haskey(nand, "nand_spare_image");
        if (!nand || qdict_size(nand) != 10 + erase + main + copy + main_image + spare_image || !profile || strcmp(profile, "samsung-256m-x8-reset-id-status") ||
            !completion || strcmp(completion, "immediate-ready") || !seed ||
            input_uint(nand, "address", UINT32_MAX) != 0x50000000 ||
            input_uint(nand, "manufacturer_id", 255) != 0xec || input_uint(nand, "device_id", 255) != 0x35 ||
            qdict_get_bool(nand, "write_protected")) {
            error_report("db2010: unsupported NAND prepared profile"); exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < 3; i++) {
            QDict *sub = qobject_to(QDict, qdict_get(nand, keys[i]));
            const char *p = sub ? qdict_get_try_str(sub, "profile") : NULL;
            bool supported = p && (!strcmp(p, profiles[i]) ||
                (i == 1 && !strcmp(p, "samsung-x8-known-spare-four-pulse-startup")) ||
                (i == 1 && !strcmp(p, "samsung-x8-known-spare-lifecycle-no-wear")) ||
                (i == 2 && (!strcmp(p, "all-spare-ff-first-valid-footer") ||
                            !strcmp(p, "all-spare-ff-valid-footers"))));
            if (!sub || qdict_size(sub) != 1 || !supported) {
                error_report("db2010: unsupported NAND protocol"); exit(EXIT_FAILURE);
            }
        }
        s->has_nand = true; declare_mmio_page(s, 0x50000000);
        DB2010Nand *t = s->nand;
        QDict *spare_program = qobject_to(QDict, qdict_get(nand, "spare_program"));
        const char *spare_profile = qdict_get_str(spare_program, "profile");
        t->spare_program_limit = !strcmp(spare_profile, "samsung-x8-known-spare-lifecycle-no-wear") ? 0 :
                                !strcmp(spare_profile, "samsung-x8-known-spare-four-pulse-startup") ? 4 : 3;
        if (erase) {
            QDict *sub = qobject_to(QDict, qdict_get(nand, "block_erase"));
            const char *p = sub ? qdict_get_try_str(sub, "profile") : NULL;
            if (!sub || qdict_size(sub) != 1 || !p || strcmp(p, "samsung-x8-block-erase-immediate")) {
                error_report("db2010: unsupported NAND erase protocol"); exit(EXIT_FAILURE);
            }
            t->erase_enabled = true; t->erased_blocks = g_malloc0(2048);
        }
        if (main) {
            QDict *sub = qobject_to(QDict, qdict_get(nand, "main_array"));
            const char *p = sub ? qdict_get_try_str(sub, "profile") : NULL;
            if (!erase || !sub || qdict_size(sub) != 1 || !p || strcmp(p, "samsung-x8-known-main-read-program")) {
                error_report("db2010: unsupported NAND main-array protocol"); exit(EXIT_FAILURE);
            }
            t->main_enabled = true;
            t->main_bytes = g_malloc0(65536 * 512);
            t->main_known_blocks = g_malloc0(2048);
            t->main_program_counts = g_malloc0(65536);
        }
        if (copy) {
            QDict *sub = qobject_to(QDict, qdict_get(nand, "copy_back"));
            const char *p = sub ? qdict_get_try_str(sub, "profile") : NULL;
            if (!main || !sub || qdict_size(sub) != 1 || !p ||
                (strcmp(p, "samsung-x8-known-page-copyback") && strcmp(p, "samsung-x8-known-page-copyback-no-wear"))) {
                error_report("db2010: unsupported NAND copy-back profile"); exit(EXIT_FAILURE);
            }
            t->copy_enabled = true;
            t->copy_no_wear = !strcmp(p, "samsung-x8-known-page-copyback-no-wear");
            if (t->copy_no_wear && t->spare_program_limit) {
                error_report("db2010: no-wear copy-back requires the explicit spare no-wear profile"); exit(EXIT_FAILURE);
            }
            t->copied_pages = g_malloc0(65536);
        }
        t->bytes = g_malloc0(65536 * 16); t->known = g_malloc0(65536 * 16);
        t->initial_bytes = g_malloc0(65536 * 16); t->initial_known = g_malloc0(65536 * 16); t->program_counts = g_new0(uint32_t, 65536);
        g_autofree char *raw = NULL; gsize size;
        if (!g_file_get_contents(seed, &raw, &size, NULL) || size % 4 || size > 65536 * 16 * 4) {
            error_report("db2010: invalid prepared NAND spare seeds"); exit(EXIT_FAILURE);
        }
        for (size_t i = 0; i < size; i += 4) {
            const uint8_t *record = (uint8_t *)raw + i;
            unsigned page = record[0] | (record[1] << 8), column = record[2];
            unsigned index = page * 16 + column;
            if (column >= 16 || t->initial_known[index]) {
                error_report("db2010: invalid/duplicate NAND spare seed"); exit(EXIT_FAILURE);
            }
            t->initial_known[index] = 1; t->initial_bytes[index] = record[3];
        }
        if (main_image) {
            g_autofree char *image = NULL;
            if (!main || !g_file_get_contents(qdict_get_str(nand, "nand_main_image"), &image, &size, NULL) || size != 65536 * 512) {
                error_report("db2010: invalid raw NAND main image"); exit(EXIT_FAILURE);
            }
            t->initial_main_bytes = (uint8_t *)g_steal_pointer(&image);
        }
        if (spare_image) {
            g_autofree char *image = NULL;
            if (!g_file_get_contents(qdict_get_str(nand, "nand_spare_image"), &image, &size, NULL) || size != 65536 * 16) {
                error_report("db2010: invalid raw NAND spare image"); exit(EXIT_FAILURE);
            }
            memcpy(t->initial_bytes, image, size);
            memset(t->initial_known, 1, size);
        }
    }
    if (qdict_haskey(state, "i2c_controller")) {
        QDict *i2c = qobject_to(QDict, qdict_get(state, "i2c_controller"));
        const char *profile = i2c ? qdict_get_try_str(i2c, "profile") : NULL;
        bool register_reads = profile && !strcmp(profile, "vincenne2-startup-register-reads");
        if (!i2c || qdict_size(i2c) != (register_reads ? 7 + qdict_haskey(i2c, "post_startup") + qdict_haskey(i2c, "post_startup_repeat_from") + qdict_haskey(i2c, "post_startup_writes") + qdict_haskey(i2c, "post_startup_reads") + qdict_haskey(i2c, "polled_startup_adc") + qdict_haskey(i2c, "sampled_battery_adc") + qdict_haskey(i2c, "audio_write_latches") + qdict_haskey(i2c, "control45_write_latch") + qdict_haskey(i2c, "control_b1_write_latch") + qdict_haskey(i2c, "control36_37_write_latches") + qdict_haskey(i2c, "vibrator_write_latch") : profile && !strcmp(profile, "marita-polled-byte-power-status-inputs") ? 6 : 5) || !profile ||
            (strcmp(profile, "marita-polled-byte") &&
             strcmp(profile, "marita-polled-byte-async-init-capture") &&
             strcmp(profile, "marita-polled-byte-async-init-completion") &&
             strcmp(profile, "marita-polled-byte-async-power-startup") &&
             strcmp(profile, "marita-polled-byte-power-init-sequence") &&
             strcmp(profile, "marita-polled-byte-power-status-inputs") && !register_reads) ||
            input_uint(i2c, "address", UINT32_MAX) != 0xf9040000 ||
            input_uint(i2c, "completion_ns", 65536) != 128 ||
            !qdict_get_try_bool(i2c, "capture_boot_writes", false)) {
            error_report("db2010: unsupported I2C profile"); exit(EXIT_FAILURE);
        }
        if (register_reads && qdict_haskey(i2c, "polled_startup_adc")) {
            s->i2c->pmic->config.polled_startup_adc = true;
            if (qobject_type(qdict_get(i2c, "polled_startup_adc")) != QTYPE_QNULL) {
                s->i2c->pmic->config.polled_adc_value = input_uint(i2c, "polled_startup_adc", 255);
                s->i2c->pmic->config.polled_adc_known = true;
            }
        }
        if (register_reads && qdict_haskey(i2c, "sampled_battery_adc")) {
            s->i2c->pmic->config.sampled_battery_adc = true;
            s->i2c->pmic->config.sampled_battery_value = input_nullable_word(i2c,
                "sampled_battery_adc", 255, &s->i2c->pmic->config.sampled_battery_known);
        }
        if (register_reads && qdict_haskey(i2c, "vibrator_write_latch")) {
            if (!qdict_get_try_bool(i2c, "vibrator_write_latch", false)) {
                error_report("db2010: invalid PMIC vibrator latch profile"); exit(EXIT_FAILURE);
            }
            s->i2c->pmic->config.vibrator_write_latch = true;
        }
        if (register_reads && qdict_haskey(i2c, "control36_37_write_latches")) {
            if (!qdict_get_try_bool(i2c,"control36_37_write_latches",false)) {
                error_report("db2010: invalid PMIC36/37 control latch profile"); exit(EXIT_FAILURE);
            }
            s->i2c->pmic->config.control36_37_write_latches = true;
        }
        if (register_reads && qdict_haskey(i2c, "control_b1_write_latch")) {
            if (!qdict_get_try_bool(i2c,"control_b1_write_latch",false)) {
                error_report("db2010: invalid PMICB1 latch profile"); exit(EXIT_FAILURE);
            }
            s->i2c->pmic->config.control_b1_write_latch = true;
        }
        if (register_reads && qdict_haskey(i2c, "control45_write_latch")) {
            if (!qdict_get_try_bool(i2c,"control45_write_latch",false)) {
                error_report("db2010: invalid PMIC45 latch profile"); exit(EXIT_FAILURE);
            }
            s->i2c->pmic->config.control45_write_latch = true;
        }
        if (register_reads && qdict_haskey(i2c, "audio_write_latches")) {
            QList *registers = qobject_to(QList, qdict_get(i2c, "audio_write_latches"));
            if (!registers || qlist_empty(registers) || qlist_size(registers) > 16) {
                error_report("db2010: invalid PMIC audio write latches"); exit(EXIT_FAILURE);
            }
            const QListEntry *entry;
            QLIST_FOREACH_ENTRY(registers, entry) {
                QNum *num = qobject_to(QNum, qlist_entry_obj(entry)); uint64_t reg;
                if (!num || !qnum_get_try_uint(num, &reg) || reg < 0xc1 || reg > 0xd0 ||
                    s->i2c->pmic->config.audio_write_latch[reg - 0xc1]) {
                    error_report("db2010: invalid PMIC audio latch register"); exit(EXIT_FAILURE);
                }
                s->i2c->pmic->config.audio_write_latch[reg - 0xc1] = true;
            }
        }
        QDict *inputs = qobject_to(QDict, qdict_get(i2c, "power_inputs"));
        if (inputs) {
            if (qdict_size(inputs) != 2 || !qdict_haskey(inputs, "usb_connected") || !qdict_haskey(inputs, "charger_connected")) {
                error_report("db2010: invalid I2C power inputs"); exit(EXIT_FAILURE);
            }
            s->i2c->pmic->config.power_known = true;
            s->i2c->pmic->config.power_value = (qdict_get_bool(inputs, "usb_connected") ? 0x80 : 0) |
                                (qdict_get_bool(inputs, "charger_connected") ? 8 : 0);
        } else if (qobject_type(qdict_get(i2c, "power_inputs")) != QTYPE_QNULL) {
            error_report("db2010: I2C power inputs must be explicit or unknown"); exit(EXIT_FAILURE);
        }
        s->i2c->pmic->config.read_power_status = register_reads || !strcmp(profile, "marita-polled-byte-power-status-inputs");
        if (s->i2c->pmic->config.read_power_status) {
            QDict *status = qobject_to(QDict, qdict_get(i2c, "status_inputs"));
            const char *names[] = {"register_21", "register_22"};
            if (!status || qdict_size(status) != 2) {
                error_report("db2010: explicit power status inputs required"); exit(EXIT_FAILURE);
            }
            for (unsigned n = 0; n < 2; n++) {
                if (!qdict_haskey(status, names[n])) { error_report("db2010: missing power status input"); exit(EXIT_FAILURE); }
                if (qobject_type(qdict_get(status, names[n])) != QTYPE_QNULL) {
                    s->i2c->pmic->config.status_input[n] = input_uint(status, names[n], 255);
                    s->i2c->pmic->config.status_input_known[n] = true;
                }
            }
        }
        if (register_reads) {
            QList *reads = qobject_to(QList, qdict_get(i2c, "register_reads"));
            if (!reads || !qlist_size(reads) || qlist_size(reads) > ARRAY_SIZE(s->i2c->pmic->config.read_reg)) {
                error_report("db2010: bounded PMIC register reads required"); exit(EXIT_FAILURE);
            }
            s->i2c->pmic->config.startup_register_reads = true;
            const QListEntry *entry;
            QLIST_FOREACH_ENTRY(reads, entry) {
                QDict *read = qobject_to(QDict, qlist_entry_obj(entry));
                if (!read || qdict_size(read) != 2 || !qdict_haskey(read, "value")) {
                    error_report("db2010: invalid PMIC read entry"); exit(EXIT_FAILURE);
                }
                unsigned n = s->i2c->pmic->config.register_read_count++;
                s->i2c->pmic->config.read_reg[n] = input_uint(read, "register", 255);
                const char *kind = qdict_get_try_str(read, "value");
                if (kind && !strcmp(kind, "last-write")) { s->i2c->pmic->config.read_kind[n] = 2; }
                else if (qobject_type(qdict_get(read, "value")) != QTYPE_QNULL) {
                    s->i2c->pmic->config.read_value[n] = input_uint(read, "value", 255);
                    s->i2c->pmic->config.read_kind[n] = 1;
                }
            }
        }
        s->i2c->pmic->config.complete_power_init = s->i2c->pmic->config.read_power_status ||
            !strcmp(profile, "marita-polled-byte-power-init-sequence");
        s->i2c->pmic->config.capture_async_power = s->i2c->pmic->config.complete_power_init ||
            !strcmp(profile, "marita-polled-byte-async-power-startup");
        s->i2c->config.complete_async_init = s->i2c->pmic->config.capture_async_power ||
            !strcmp(profile, "marita-polled-byte-async-init-completion");
        if (register_reads && qdict_haskey(i2c, "post_startup")) {
            QList *ops = qobject_to(QList, qdict_get(i2c, "post_startup"));
            if (!ops || !qlist_size(ops) || qlist_size(ops) > 32) {
                error_report("db2010: expected bounded post-startup I2C operations"); exit(EXIT_FAILURE);
            }
            QLIST_FOREACH_ENTRY(ops, entry) {
                QDict *op = qobject_to(QDict, qlist_entry_obj(entry));
                const char *kind = op ? qdict_get_try_str(op, "operation") : NULL;
                if (!op || qdict_size(op) != 3 || !kind || (strcmp(kind,"read") && strcmp(kind,"write"))) {
                    error_report("db2010: invalid post-startup I2C operation"); exit(EXIT_FAILURE);
                }
                unsigned n = s->i2c->pmic->config.followup_count++;
                s->i2c->pmic->config.followup_reg[n] = input_uint(op, "register", 255);
                s->i2c->pmic->config.followup_read[n] = !strcmp(kind, "read");
                s->i2c->pmic->config.followup_value[n] = input_nullable_word(op, "value", 255, &s->i2c->pmic->config.followup_known[n]);
                if (!s->i2c->pmic->config.followup_read[n] && !s->i2c->pmic->config.followup_known[n]) {
                    error_report("db2010: post-startup write requires exact value"); exit(EXIT_FAILURE);
                }
            }
        }
        if (register_reads && qdict_haskey(i2c, "post_startup_reads")) {
            QList *reads = qobject_to(QList, qdict_get(i2c, "post_startup_reads"));
            if (!s->i2c->pmic->config.followup_count || !reads || !qlist_size(reads) || qlist_size(reads) > 32) {
                error_report("db2010: bounded extra PMIC reads require post-startup operations"); exit(EXIT_FAILURE);
            }
            QLIST_FOREACH_ENTRY(reads, entry) {
                QDict *read = qobject_to(QDict, qlist_entry_obj(entry));
                static const char *const keys[] = { "register", "value", NULL };
                if (!read || qdict_size(read) != 2 || !only_keys(read, keys)) {
                    error_report("db2010: invalid extra PMIC read"); exit(EXIT_FAILURE);
                }
                unsigned reg = input_uint(read, "register", 255);
                if (dbi_extra_read_index(s->i2c, reg) >= 0) {
                    error_report("db2010: duplicate extra PMIC read"); exit(EXIT_FAILURE);
                }
                unsigned n = s->i2c->pmic->config.extra_read_count++;
                s->i2c->pmic->config.extra_read_reg[n] = reg;
                const char *value = qdict_get_try_str(read, "value");
                if (value) {
                    if (strcmp(value, "last-write")) { error_report("db2010: invalid extra PMIC read value"); exit(EXIT_FAILURE); }
                    s->i2c->pmic->config.extra_read_kind[n] = 2;
                } else {
                    bool known;
                    s->i2c->pmic->config.extra_read_value[n] = input_nullable_word(read, "value", 255, &known);
                    s->i2c->pmic->config.extra_read_kind[n] = known ? 1 : 0;
                }
            }
        }
        if (register_reads && qdict_haskey(i2c, "post_startup_writes")) {
            QList *writes = qobject_to(QList, qdict_get(i2c, "post_startup_writes"));
            if (!s->i2c->pmic->config.followup_count || !writes || !qlist_size(writes) || qlist_size(writes) > ARRAY_SIZE(s->i2c->pmic->config.extra_write_reg)) {
                error_report("db2010: bounded extra PMIC writes require post-startup operations"); exit(EXIT_FAILURE);
            }
            QLIST_FOREACH_ENTRY(writes, entry) {
                QDict *write = qobject_to(QDict, qlist_entry_obj(entry));
                if (!write || qdict_size(write) != 2) {
                    error_report("db2010: invalid extra PMIC write"); exit(EXIT_FAILURE);
                }
                unsigned n = s->i2c->pmic->config.extra_write_count++;
                s->i2c->pmic->config.extra_write_reg[n] = input_uint(write, "register", 255);
                s->i2c->pmic->config.extra_write_value[n] = input_uint(write, "value", 255);
            }
        }
        if (qdict_haskey(i2c, "post_startup_repeat_from")) {
            static const uint8_t regs[] = {0x70, 0x71, 0x70, 0x71, 0x7a, 0x77};
            static const uint8_t values[] = {0xa8, 64, 0xac, 64, 64, 64};
            static const bool reads[] = {false, true, false, true, true, true};
            if (input_uint(i2c, "post_startup_repeat_from", 7) != 7 ||
                s->i2c->pmic->config.followup_count != 13) {
                error_report("db2010: unsupported recurring ADC tail"); exit(EXIT_FAILURE);
            }
            for (unsigned n = 0; n < ARRAY_SIZE(regs); n++) {
                unsigned k = n + 7;
                if (s->i2c->pmic->config.followup_reg[k] != regs[n] ||
                    /* Wire77 is the firmware's battery-voltage sample.
                     * Keep the transport/order fixed, but accept its declared
                     * known byte rather than hard-coding the old low input. */
                    (n != 5 && s->i2c->pmic->config.followup_value[k] != values[n]) ||
                    s->i2c->pmic->config.followup_read[k] != reads[n] ||
                    !s->i2c->pmic->config.followup_known[k]) {
                    error_report("db2010: unsupported recurring ADC input"); exit(EXIT_FAILURE);
                }
            }
            s->i2c->pmic->config.repeat_followups = true;
            s->i2c->pmic->config.followup_repeat_from = 7;
        }
        s->i2c->config.capture_async_init = strcmp(profile, "marita-polled-byte") != 0;
        s->i2c->pmic->config.capture_writes = true; s->has_i2c = true; declare_mmio_page(s, 0xf9040000);
    }
    if (qdict_haskey(state, "pdi")) {
        QDict *pdi = qobject_to(QDict, qdict_get(state, "pdi"));
        const char *profile = pdi ? qdict_get_try_str(pdi, "profile") : NULL;
        if (!pdi || qdict_size(pdi) != 1 || !profile || (strcmp(profile, "r61500-write-packets") && strcmp(profile, "r61500-write-packets-retained-latches"))) {
            error_report("db2010: unsupported PDI profile"); exit(EXIT_FAILURE);
        }
        s->display->pdi.config.retained_latches = !strcmp(profile, "r61500-write-packets-retained-latches");
        s->display->has_pdi = true; declare_mmio_page(s, 0xf7000100);
    }
    if (qdict_haskey(state, "fm_radio")) {
        QDict *fm = qobject_to(QDict, qdict_get(state, "fm_radio"));
        const char *profile = fm ? qdict_get_try_str(fm, "profile") : NULL;
        QList *frames = fm ? qobject_to(QList, qdict_get(fm, "allowed_writes_hex")) : NULL;
        if (!s->has_i2c || !s->i2c->pmic->config.complete_power_init || !fm ||
            qdict_size(fm) != 3 || !profile || strcmp(profile, "i2c-frame-sink") ||
            input_uint(fm, "wire_write_address", 255) != 0x20 || !frames ||
            qlist_empty(frames) || qlist_size(frames) > DBFR_MAX_FRAMES) {
            error_report("db2010: invalid FM I2C frame profile"); exit(EXIT_FAILURE);
        }
        const QListEntry *entry;
        QLIST_FOREACH_ENTRY(frames, entry) {
            QString *hex = qobject_to(QString, qlist_entry_obj(entry));
            const char *raw = hex ? qstring_get_str(hex) : NULL;
            unsigned len = raw ? strlen(raw) : 0;
            if (!len || len % 2 || len > 2 * DBFR_MAX_BYTES) {
                error_report("db2010: invalid FM frame length"); exit(EXIT_FAILURE);
            }
            unsigned index = s->i2c->fm->config.count++;
            s->i2c->fm->config.size[index] = len / 2;
            for (unsigned k = 0; k < len; k++) {
                if (!strchr("0123456789abcdef", raw[k])) {
                    error_report("db2010: invalid FM frame hex"); exit(EXIT_FAILURE);
                }
            }
            for (unsigned k = 0; k < len / 2; k++) {
                s->i2c->fm->config.allowed[index][k] = (g_ascii_xdigit_value(raw[k*2]) << 4) |
                                              g_ascii_xdigit_value(raw[k*2+1]);
            }
        }
        s->i2c->fm->config.enabled = true;
    }
    if (qdict_haskey(state, "pl080_dma")) {
        QDict *dma = qobject_to(QDict, qdict_get(state, "pl080_dma"));
        const char *profile = dma ? qdict_get_try_str(dma, "profile") : NULL;
        if (!dma || qdict_size(dma) != 3 || !profile || (strcmp(profile, "cold-disabled-init") && strcmp(profile, "cold-disabled-display-first-burst") && strcmp(profile, "cold-disabled-display-frame") && strcmp(profile, "cold-disabled-display-stream") && strcmp(profile, "cold-disabled-display-stream-fa-tx") && strcmp(profile, "cold-disabled-display-partial-fa-tx")) ||
            input_uint(dma, "address", UINT32_MAX) != 0xf2000000 || input_uint(dma, "channels", 8) != 8) {
            error_report("db2010: unsupported PL080 profile"); exit(EXIT_FAILURE);
        }
        s->dma->config.display_partial = !strcmp(profile, "cold-disabled-display-partial-fa-tx");
        s->dma->config.fa_tx = s->dma->config.display_partial || !strcmp(profile, "cold-disabled-display-stream-fa-tx");
        s->dma->config.display_stream = s->dma->config.fa_tx || !strcmp(profile, "cold-disabled-display-stream");
        s->dma->config.display_frame = s->dma->config.display_stream || !strcmp(profile, "cold-disabled-display-frame");
        s->dma->config.display_first_burst = s->dma->config.display_frame || !strcmp(profile, "cold-disabled-display-first-burst");
        if (s->dma->config.display_first_burst && !s->display->has_pdi) {
            error_report("db2010: display DMA profile requires PDI owner"); exit(EXIT_FAILURE);
        }
        s->has_dma = true; declare_mmio_page(s, 0xf2000000);
    }
    if (qdict_haskey(state, "rtc_counter")) {
        QDict *rtc = qobject_to(QDict, qdict_get(state, "rtc_counter"));
        const char *profile = rtc ? qdict_get_try_str(rtc, "profile") : NULL;
        const char *epoch = rtc ? qdict_get_try_str(rtc, "calendar_epoch") : NULL;
        if (!rtc || qdict_size(rtc) != 8 + qdict_haskey(rtc, "calendar_writable") + qdict_haskey(rtc, "minute_irq") || !profile || strcmp(profile, "virtual-clock-low8") ||
            !epoch || strcmp(epoch, "2000-01-01T00:00:00") ||
            input_uint(rtc, "address", UINT32_MAX) != 0xf9050000 ||
            input_uint(rtc, "frequency_hz", UINT32_MAX) != 32768 ||
            !qdict_get_try_bool(rtc, "control_writable", false)) {
            error_report("db2010: unsupported RTC/calendar profile");
            exit(EXIT_FAILURE);
        }
        s->has_rtc = true;
        s->rtc->config.initial_ticks = input_uint(rtc, "initial_ticks", 255);
        s->rtc->config.initial_control = input_uint(rtc, "control_initial_value", 65535);
        s->rtc->config.status = input_uint(rtc, "status_initial_value", 65535);
        s->rtc->config.calendar_writable = qdict_get_try_bool(rtc, "calendar_writable", false);
        if (qdict_haskey(rtc, "minute_irq")) {
            if (!qdict_get_try_bool(rtc, "minute_irq", false) || !qdict_haskey(state, "timer_irq")) {
                error_report("db2010: RTC minute IRQ requires true and native interrupt controller"); exit(EXIT_FAILURE);
            }
            s->rtc->config.minute_irq = true;
        }
        declare_mmio_page(s, 0xf9050000);
    }
    if (qdict_haskey(state, "rtc_retained_state")) {
        QDict *rtc = qobject_to(QDict, qdict_get(state, "rtc_retained_state"));
        const char *profile = rtc ? qdict_get_try_str(rtc, "profile") : NULL;
        if (!rtc || qdict_size(rtc) != 1 || !profile || strcmp(profile, "cold-zero-lowbyte-bank")) {
            error_report("db2010: unsupported RTC retained-state profile");
            exit(EXIT_FAILURE);
        }
        s->has_rtc_retained = true;
        declare_mmio_page(s, 0xf9050000);
    }
    if (qdict_haskey(state, "serial_lines")) {
        QDict *serial = qobject_to(QDict, qdict_get(state, "serial_lines"));
        const char *profile = serial ? qdict_get_try_str(serial, "profile") : NULL;
        if (!serial || qdict_size(serial) != 3 || !profile || strcmp(profile, "port1-rx-levels")) {
            error_report("db2010: unsupported serial input profile"); exit(EXIT_FAILURE);
        }
        s->bank->has_serial = true;
        s->bank->serial_levels = input_uint(serial, "uart0_rx_level", 1) | (input_uint(serial, "uart1_rx_level", 1) << 4);
    }
    if (qdict_haskey(state, "memorystick_slot")) {
        QDict *slot = qobject_to(QDict, qdict_get(state, "memorystick_slot"));
        const char *profile = slot ? qdict_get_try_str(slot, "profile") : NULL;
        if (!slot || qdict_size(slot) != 1 || !profile || strcmp(profile, "gpio42-empty-slot")) {
            error_report("db2010: unsupported MemoryStick slot profile"); exit(EXIT_FAILURE);
        }
        s->bank->has_memorystick_slot = true;
    }
    if (qdict_haskey(state, "uart_transmit")) {
        QDict *uart = qobject_to(QDict, qdict_get(state, "uart_transmit"));
        const char *profile = uart ? qdict_get_try_str(uart, "profile") : NULL;
        if (!uart || qdict_size(uart) != 2 || !profile || strcmp(profile, "polled-tx-immediate-drain") ||
            input_uint(uart, "free_slots", UINT32_MAX) != 1) {
            error_report("db2010: unsupported UART transmit profile"); exit(EXIT_FAILURE);
        }
        s->has_uart = true;
        static const uint32_t bases[] = {0xfb060000, 0xfc040000, 0xfc030000, 0xfb040000};
        for (unsigned port = 0; port < 4; port++) { declare_mmio_page(s, bases[port]); }
    }
    if (qdict_haskey(state, "serial_transport")) {
        QDict *serial = qobject_to(QDict, qdict_get(state, "serial_transport"));
        if (!s->has_uart || !serial || qdict_size(serial) != 1 ||
            g_strcmp0(qdict_get_try_str(serial, "profile"), "uart0-9600-fifo-assumption")) {
            error_report("db2010: unsupported serial transport profile"); exit(EXIT_FAILURE);
        }
        s->uart_port->data_enabled = true;
        s->bank->data_uart_flow = true;
        s->timer->config.data_uart_route = true;
    }
    if (qdict_haskey(state, "bluetooth_transport")) {
        QDict *bt = qobject_to(QDict, qdict_get(state, "bluetooth_transport"));
        const char *peer = bt ? qdict_get_try_str(bt, "peer") : NULL;
        if (!s->has_uart || !bt || qdict_size(bt) != 2 ||
            g_strcmp0(qdict_get_try_str(bt, "profile"), "uart3-h4-init-failure") ||
            (!peer || (strcmp(peer, "silent") && strcmp(peer, "command-error-03") &&
                       strcmp(peer, "baud-ack-errors") && strcmp(peer, "setup-ack-errors") &&
                       strcmp(peer, "init-ack-errors") && strcmp(peer, "zero-capacity-errors")))) {
            error_report("db2010: unsupported Bluetooth transport profile"); exit(EXIT_FAILURE);
        }
        s->uart_port->bluetooth_enabled = true;
        s->uart_port->bluetooth_error_peer = strcmp(peer, "silent") != 0;
        s->uart_port->bluetooth_ack_level = !strcmp(peer, "zero-capacity-errors") ? 4 :
            !strcmp(peer, "init-ack-errors") ? 3 :
            !strcmp(peer, "setup-ack-errors") ? 2 : !strcmp(peer, "baud-ack-errors") ? 1 : 0;
        s->timer->config.bluetooth_route = true;
    }
    if (qdict_haskey(state, "simif")) {
        QDict *sim=qobject_to(QDict,qdict_get(state,"simif"));
        if (!sim || qdict_size(sim)!=2 ||
            g_strcmp0(qdict_get_try_str(sim,"profile"),"pmb7850-gsm-stub-native-simif")) {
            error_report("db2010: unsupported SIMIF profile"); exit(EXIT_FAILURE);
        }
        s->simif->byte_ns=input_uint(sim,"byte_ns",1000000000);
        if (!s->simif->byte_ns) { error_report("db2010: SIMIF byte latency required"); exit(EXIT_FAILURE); }
        s->has_simif=true; declare_mmio_page(s,0xf9060000);
    }
    if (qdict_haskey(state, "keypad")) {
        QDict *k = qobject_to(QDict, qdict_get(state, "keypad"));
        if (!k || qdict_size(k) != 1 ||
            g_strcmp0(qdict_get_try_str(k, "profile"), "w800-front-ideal-contacts")) {
            error_report("db2010: unsupported keypad profile"); exit(EXIT_FAILURE);
        }
        s->has_keypad = true;
        declare_mmio_page(s, 0xfb010000);
    }
    if (qdict_haskey(state, "timer_irq")) {
        QDict *timer = qobject_to(QDict, qdict_get(state, "timer_irq"));
        const char *profile = timer ? qdict_get_try_str(timer, "profile") : NULL;
        if (!timer || qdict_size(timer) != 3 || !profile ||
            (strcmp(profile, "tick1-upcounter-irq-assumption") && strcmp(profile, "tick1-paired-upcounter-irq-assumption")) ||
            (input_uint(timer, "frequency_hz", UINT32_MAX) != 131072 &&
             input_uint(timer, "frequency_hz", UINT32_MAX) != 32768)) {
            error_report("db2010: unsupported TIMER1 clock/profile");
            exit(EXIT_FAILURE);
        }
        s->timer->config.frequency_hz = input_uint(timer, "frequency_hz", UINT32_MAX);
        s->timer->config.paired_channel = !strcmp(profile, "tick1-paired-upcounter-irq-assumption");
        s->has_timer = true;
        s->timer->config.max_irqs = input_uint(timer, "max_irqs", 1000000);
        if (!s->timer->config.max_irqs) {
            error_report("db2010: IRQ budget must be positive");
            exit(EXIT_FAILURE);
        }
        declare_mmio_page(s, 0xf9010000);
        declare_mmio_page(s, 0xfb020000);
    }
    if (qdict_haskey(state, "execution_proofs")) {
        QList *proofs = qobject_to(QList, qdict_get(state, "execution_proofs"));
        if (!proofs || qlist_size(proofs) > 16) {
            error_report("db2010: invalid execution proofs");
            exit(EXIT_FAILURE);
        }
        QLIST_FOREACH_ENTRY(proofs, entry) {
            QDict *spec = qobject_to(QDict, qlist_entry_obj(entry));
            const char *file = spec ? qdict_get_try_str(spec, "file") : NULL;
            DB2010ExecutionProof *proof = g_new0(DB2010ExecutionProof, 1);
            if (!file || qdict_size(spec) != 2 ||
                !g_file_get_contents(file, (char **)&proof->bytes, &proof->size, NULL) ||
                !proof->size || proof->size > 0x2000000) {
                error_report("db2010: invalid execution proof bytes");
                exit(EXIT_FAILURE);
            }
            proof->base = input_uint(spec, "address", UINT32_MAX);
            if ((uint64_t)proof->base + proof->size > (1ULL << 32)) {
                error_report("db2010: execution proof overflows address space");
                exit(EXIT_FAILURE);
            }
            for (unsigned p = 0; p < s->execution_proofs->len; p++) {
                DB2010ExecutionProof *other = g_ptr_array_index(s->execution_proofs, p);
                if ((uint64_t)proof->base < (uint64_t)other->base + other->size &&
                    (uint64_t)other->base < (uint64_t)proof->base + proof->size) {
                    error_report("db2010: overlapping execution proofs");
                    exit(EXIT_FAILURE);
                }
            }
            g_ptr_array_add(s->execution_proofs, proof);
        }
    }
    if (qdict_haskey(state, "nor_model")) {
        db2010_nor_configure(s->nor, state);
        s->has_nor = true;
    }
    if (qdict_haskey(state, "absent_bank")) {
        QDict *a = qobject_to(QDict, qdict_get(state, "absent_bank"));
        const char *profile = a ? qdict_get_try_str(a, "profile") : NULL;
        if (!a || qdict_size(a) != 3 || !profile || strcmp(profile, "all-ones-ignore-writes") ||
            input_uint(a, "address", UINT32_MAX) != 0x46000000 ||
            input_uint(a, "size", UINT32_MAX) != 8192) {
            error_report("db2010: unsupported absent-bank hypothesis");
            exit(EXIT_FAILURE);
        }
        s->has_absent_bank = true;
    }
    s->i2c->pmic->config.bounded_history = s->bounded_history;
    /* Missing policy preserves archived prepared-state fixtures. The normal
     * host always declares whether an output directory enabled diagnostics. */
    if (qdict_haskey(state, "record_diagnostics")) {
        if (qobject_type(qdict_get(state, "record_diagnostics")) != QTYPE_QBOOL) {
            error_report("db2010: record_diagnostics must be boolean");
            exit(EXIT_FAILURE);
        }
        s->omit_diagnostics = !qdict_get_bool(state, "record_diagnostics");
    }
    s->nand->omit_diagnostics = s->omit_diagnostics;
    s->display->omit_diagnostics = s->omit_diagnostics;
    s->i2c->pmic->config.omit_diagnostics = s->omit_diagnostics;
    s->simif->omit_diagnostics = s->omit_diagnostics;
    s->fa->dsp_peer->omit_diagnostics = s->omit_diagnostics;
    regions = qobject_to(QList, qdict_get(state, "regions"));
    ranges = qobject_to(QList, qdict_get(state, "executable_ranges"));
    if (!regions || !ranges || qlist_size(regions) > 16 || qlist_size(ranges) > 16 ||
        (s->instruction_limited && !s->limit) || s->sctlr & (1 | (1 << 7) | (1 << 13)) ||
        s->cpsr & 0x01000000 || (s->cpsr & 0x1f) != 0x13) {
        error_report("db2010: unsupported prepared CPU/region state");
        exit(EXIT_FAILURE);
    }
    QLIST_FOREACH_ENTRY(ranges, entry) {
        QDict *range = qobject_to(QDict, qlist_entry_obj(entry));
        DB2010Interval value;
        if (!range) {
            error_report("db2010: invalid executable range");
            exit(EXIT_FAILURE);
        }
        value.start = input_uint(range, "start", UINT32_MAX);
        value.end = input_uint(range, "end", 1ULL << 32);
        if (value.end <= value.start) {
            error_report("db2010: empty/reversed executable range");
            exit(EXIT_FAILURE);
        }
        g_array_append_val(s->execution, value);
    }
    QLIST_FOREACH_ENTRY(regions, entry) {
        QDict *spec = qobject_to(QDict, qlist_entry_obj(entry));
        DB2010Region *r;
        QList *seeds;
        QListEntry *seed_entry;
        const char *name;
        if (!spec || !(name = qdict_get_try_str(spec, "name"))) {
            error_report("db2010: invalid prepared region");
            exit(EXIT_FAILURE);
        }
        r = add_region(s, name, input_uint(spec, "address", UINT32_MAX),
                       input_uint(spec, "size", UINT32_MAX),
                       qdict_get_try_bool(spec, "readonly", false));
        if (qdict_haskey(spec, "assumed_fill")) {
            unsigned fill = input_uint(spec, "assumed_fill", 255);
            memset(memory_region_get_ram_ptr(r->mr), fill, r->size);
            memset(r->known, 2, r->size);
        }
        seeds = qobject_to(QList, qdict_get(spec, "seeds"));
        if (!seeds) {
            error_report("db2010: missing prepared seeds");
            exit(EXIT_FAILURE);
        }
        QLIST_FOREACH_ENTRY(seeds, seed_entry) {
            QDict *seed = qobject_to(QDict, qlist_entry_obj(seed_entry));
            const char *filename;
            g_autofree char *data = NULL;
            gsize length;
            uint32_t address;
            if (!seed || !(filename = qdict_get_try_str(seed, "file"))) {
                error_report("db2010: invalid prepared seed");
                exit(EXIT_FAILURE);
            }
            address = input_uint(seed, "address", UINT32_MAX);
            if (!g_file_get_contents(filename, &data, &length, &error) ||
                !length || address < r->base ||
                (uint64_t)address + length > (uint64_t)r->base + r->size) {
                error_report("db2010: missing/out-of-range prepared seed");
                exit(EXIT_FAILURE);
            }
            for (i = 0; i < length; i++) {
                if (r->known[address - r->base + i] == 1) {
                    error_report("db2010: overlapping prepared seeds");
                    exit(EXIT_FAILURE);
                }
            }
            memcpy((uint8_t *)memory_region_get_ram_ptr(r->mr) + address - r->base,
                   data, length);
            memset(r->known + address - r->base, 1, length);
        }
        r->initial = g_memdup2(memory_region_get_ram_ptr(r->mr), r->size);
        r->initial_known = g_memdup2(r->known, r->size);
    }
    if (qdict_haskey(state, "registers")) {
        QList *registers = qobject_to(QList, qdict_get(state, "registers"));
        QListEntry *reg;
        if (!registers || qlist_size(registers) != 16) {
            error_report("db2010: registers must have 16 unsigned words");
            exit(EXIT_FAILURE);
        }
        i = 0;
        QLIST_FOREACH_ENTRY(registers, reg) {
            uint64_t value;
            QNum *n = qobject_to(QNum, qlist_entry_obj(reg));
            if (!n || !qnum_get_try_uint(n, &value) || value > UINT32_MAX) {
                error_report("db2010: invalid initial register");
                exit(EXIT_FAILURE);
            }
            s->initial_registers[i++] = value;
        }
    }
    if (qdict_haskey(state, "memory_controller")) {
        QDict *controller = qobject_to(QDict, qdict_get(state, "memory_controller"));
        const char *profile = controller ? qdict_get_try_str(controller, "profile") : NULL;
        if (!profile || strcmp(profile, "word-latches-fixed-bank") ||
            qdict_size(controller) != 2) {
            error_report("db2010: unsupported controller profile");
            exit(EXIT_FAILURE);
        }
        s->memory_controller->initial_bank = input_uint(controller, "bank_word", UINT32_MAX);
        if (s->memory_controller->initial_bank != 0x04018060) {
            error_report("db2010: unsupported initial bank decoder hypothesis");
            exit(EXIT_FAILURE);
        }
        s->has_controller = true;
    }
    if (qdict_haskey(state, "region_registers")) {
        QDict *spec = qobject_to(QDict, qdict_get(state, "region_registers"));
        QList *values = spec ? qobject_to(QList, qdict_get(spec, "initial_words")) : NULL;
        if (!spec || qdict_size(spec) != 2 ||
            input_uint(spec, "address", UINT32_MAX) != 0xfe004000 ||
            !values || qlist_size(values) != 24) {
            error_report("db2010: region controls require 24 explicit words/unknowns");
            exit(EXIT_FAILURE);
        }
        i = 0;
        QLIST_FOREACH_ENTRY(values, entry) {
            QObject *value = qlist_entry_obj(entry);
            uint64_t word;
            QNum *n = qobject_to(QNum, value);
            if (n && qnum_get_try_uint(n, &word) && word <= UINT32_MAX) {
                s->region_controller->initial_region_words[i] = word;
                s->region_controller->initial_region_known[i] = true;
            } else if (qobject_type(value) != QTYPE_QNULL) {
                error_report("db2010: invalid region word");
                exit(EXIT_FAILURE);
            }
            i++;
        }
        s->has_region_controls = true;
        memcpy(s->region_controller->region_words, s->region_controller->initial_region_words, sizeof(s->region_controller->region_words));
        memcpy(s->region_controller->region_known, s->region_controller->initial_region_known, sizeof(s->region_controller->region_known));
    }
    if (qdict_haskey(state, "section_table")) {
        QDict *spec = qobject_to(QDict, qdict_get(state, "section_table"));
        const char *profile = spec ? qdict_get_try_str(spec, "profile") : NULL;
        uint32_t table[4096];
        unsigned slot;
        const char *fault;
        if (!s->has_region_controls || !profile ||
            strcmp(profile, "identity-disjoint-sections") || qdict_size(spec) != 2 ||
            input_uint(spec, "address", UINT32_MAX) != 0xfe000000 ||
            !qdict_get_try_str(state, "section_table_update_policy") ||
            strcmp(qdict_get_str(state, "section_table_update_policy"), "tlb-boundary")) {
            error_report("db2010: unsupported section-table profile/policy");
            exit(EXIT_FAILURE);
        }
        fault = generate_section_table(s, table, &slot);
        if (fault) {
            error_report("db2010: initial table rejected: %s slot %u", fault, slot);
            exit(EXIT_FAILURE);
        }
        s->section_table = add_region(s, "db2010.section-table", 0xfe000000, 16384, true);
        memcpy(memory_region_get_ram_ptr(s->section_table->mr), table, sizeof(table));
        memset(s->section_table->known, 1, 16384);
        s->section_table->initial = g_memdup2(table, sizeof(table));
        s->section_table->initial_known = g_memdup2(s->section_table->known, 16384);
        memory_region_set_readonly(s->section_table->mr, true);
        s->region_controller->has_section_table = true;
    }
    if ((s->has_region_controls && find_region(s, 0xfe004000, 1)) ||
        (s->has_controller && find_region(s, 0x14000000, 1))) {
        error_report("db2010: prepared RAM overlaps a selected MMIO device");
        exit(EXIT_FAILURE);
    }
    if (qdict_haskey(state, "dsp_peer")) {
        QDict *p = qobject_to(QDict, qdict_get(state, "dsp_peer"));
        const char *profile = p ? qdict_get_try_str(p, "profile") : NULL;
        if (!p || qdict_size(p) != 2 || !profile || (strcmp(profile, "wire20-op2-logical2-sink") && strcmp(profile, "wire10-0300-wire20-dma-sink") && strcmp(profile, "wire10-message-wire20-dma-sink") && strcmp(profile, "wire10-message-wire20-variable-dma-sink") && strcmp(profile, "wire20-modexp1024-peer") && strcmp(profile, "wire20-modexp1024-mma-mode-peer") && strcmp(profile, "wire20-modexp1024-mma-ring-peer") && strcmp(profile, "wire20-modexp1024-mma-setup-peer") && strcmp(profile, "wire20-modexp1024-mma-buffer-peer") && strcmp(profile, "wire20-modexp1024-mma-playback-peer") && strcmp(profile, "wire20-modexp1024-mma-drain-peer") && strcmp(profile, "wire20-modexp1024-mma-stream-peer") && strcmp(profile, "wire20-modexp1024-mma-midi-setup-peer") && strcmp(profile, "wire20-modexp1024-mma-midi-peer")) ||
            !s->fa->has_fa_mailbox || !s->dma->config.fa_tx || !s->has_timer || !s->dma->config.display_frame) {
            error_report("db2010: invalid DSP peer profile/dependencies"); exit(EXIT_FAILURE);
        }
        s->fa->dsp_peer->config.latency = input_uint(p, "latency_ns", 65536);
        if (!s->fa->dsp_peer->config.latency) { error_report("db2010: DSP peer latency must be positive"); exit(EXIT_FAILURE); }
        s->fa->dsp_peer->config.midi_profile = !strcmp(profile, "wire20-modexp1024-mma-midi-peer");
        s->fa->dsp_peer->config.midi_setup = s->fa->dsp_peer->config.midi_profile || !strcmp(profile, "wire20-modexp1024-mma-midi-setup-peer");
        s->fa->dsp_peer->config.mma_stream = s->fa->dsp_peer->config.midi_setup || !strcmp(profile, "wire20-modexp1024-mma-stream-peer");
        s->fa->dsp_peer->config.mma_drain = s->fa->dsp_peer->config.mma_stream || !strcmp(profile, "wire20-modexp1024-mma-drain-peer");
        s->fa->dsp_peer->config.mma_playback = s->fa->dsp_peer->config.mma_drain || !strcmp(profile, "wire20-modexp1024-mma-playback-peer");
        s->fa->dsp_peer->config.mma_buffer = s->fa->dsp_peer->config.mma_playback || !strcmp(profile, "wire20-modexp1024-mma-buffer-peer");
        s->fa->dsp_peer->config.mma_setup = s->fa->dsp_peer->config.mma_buffer || !strcmp(profile, "wire20-modexp1024-mma-setup-peer");
        s->fa->dsp_peer->config.mma_ring = s->fa->dsp_peer->config.mma_setup || !strcmp(profile, "wire20-modexp1024-mma-ring-peer");
        s->fa->dsp_peer->config.mma_mode = s->fa->dsp_peer->config.mma_ring || !strcmp(profile, "wire20-modexp1024-mma-mode-peer");
        s->fa->dsp_peer->config.modexp = s->fa->dsp_peer->config.mma_mode || !strcmp(profile, "wire20-modexp1024-peer");
        s->dma->config.fa_rx = s->fa->dsp_peer->config.modexp;
        s->fa->dsp_peer->config.variable_dma = s->fa->dsp_peer->config.modexp || !strcmp(profile, "wire10-message-wire20-variable-dma-sink");
        s->fa->dsp_peer->config.wire10 = (s->fa->dsp_peer->config.variable_dma || !strcmp(profile, "wire10-message-wire20-dma-sink")) ? 2 :
                              !strcmp(profile, "wire10-0300-wire20-dma-sink");
        s->fa->has_dsp_peer = true;
    }
    if (qdict_haskey(state, "startup_status")) {
        QDict *p = qobject_to(QDict, qdict_get(state, "startup_status"));
        const char *profile = p ? qdict_get_try_str(p, "profile") : NULL;
        if (!p || qdict_size(p) != 2 || !profile || strcmp(profile, "mask05-lowbyte-clear")) {
            error_report("db2010: invalid startup status profile"); exit(EXIT_FAILURE);
        }
        unsigned initial = input_uint(p, "initial_value", 5);
        if (initial & ~5u) { error_report("db2010: unsupported startup status initial bits"); exit(EXIT_FAILURE); }
        s->startup->startup_status_initial = initial; s->has_startup_status = true;
        declare_mmio_page(s, 0xf905005c);
    }
    load_register_contracts(s, state);
    if (s->has_nor && !s->nor->mr) {
        error_report("db2010: NOR configuration lacks its prepared array region");
        exit(EXIT_FAILURE);
    }
    if (s->has_absent_bank && (find_region(s, 0x46000000, 1) ||
        find_region(s, 0x46001000, 1) || find_mmio_page(s, 0x46000000) ||
        find_mmio_page(s, 0x46001000))) {
        error_report("db2010: absent bank overlaps another prepared owner");
        exit(EXIT_FAILURE);
    }
    db2010_checkpoint_prepare(s, state);
}

char *get_state(Object *obj, Error **errp)
{
    return g_strdup(DB2010_MACHINE(obj)->state_file);
}

void set_state(Object *obj, const char *value, Error **errp)
{
    DB2010MachineState *s = DB2010_MACHINE(obj);
    if (s->cpu) {
        error_setg(errp, "db2010 prepared state is fixed before machine initialization");
        return;
    }
    g_free(s->state_file);
    s->state_file = g_strdup(value);
}

/* Read telemetry on the CPU thread: QMP must not race the instruction hook or
 * walk growing device histories. This host work changes no guest state. */

void db2010_validate_connections(DB2010MachineState *s)
{
    if (s->has_f500_control) {
        DB2010Register *r = find_register(s, 0xf5000800);
        if (!r || r->width != 2 || r->write_only || r->read_only ||
            r->read_mask || r->conditional || r->rtc_halfword_alias ||
            !r->allowed_writes || r->allowed_writes->len != 2 ||
            g_array_index(r->allowed_writes, uint32_t, 0) != 0 ||
            g_array_index(r->allowed_writes, uint32_t, 1) != 0x4000) {
            error_report("db2010: F500 control requires original exact halfword latch owner"); exit(EXIT_FAILURE);
        }
    }
    if (s->bank->has_f500) {
        DB2010Register *r = find_register(s, 0xf500081c);
        if (!r || r->width != 2 || !r->write_only || !r->allowed_writes || r->allowed_writes->len != 1 ||
            g_array_index(r->allowed_writes, uint32_t, 0) != 0) {
            error_report("db2010: F500 requires exact write-only zero-command owner"); exit(EXIT_FAILURE);
        }
    }
    if (s->bank->has_serial && (!find_register(s, 0xf900000a) || !find_register(s, 0xf900000c))) {
        error_report("db2010: serial input requires exact GPIO data/direction owners"); exit(EXIT_FAILURE);
    }
    if (s->has_timer && (!find_register(s, 0xfb020104) || !find_register(s, 0xfb020204) ||
        !find_register(s, 0xfb020118) || !find_register(s, 0xfb020218))) {
        error_report("db2010: timer requires exact mask/ACK owners");
        exit(EXIT_FAILURE);
    }
}

DB2010Register *find_register(DB2010MachineState *s, uint32_t address)
{
    return dbreg_find(s->bank, address);
}
