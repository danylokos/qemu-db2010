/* DB2010 pre-effect register contracts. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_CONTRACT_H
#define DB2010_CONTRACT_H
#include <stdbool.h>
#include <stdint.h>

/* A claim includes its guarded holes. Width/alignment/direction are validated
 * separately, so an invalid transaction cannot fall through to another owner. */
typedef struct DB2010RegisterSpec {
    uint32_t address;
    unsigned span, widths, alignment;
    bool readable, writable;
} DB2010RegisterSpec;

bool db2010_claims(const DB2010RegisterSpec *specs, unsigned count,
                   uint32_t address, unsigned size);
const DB2010RegisterSpec *db2010_register_spec(const DB2010RegisterSpec *specs,
                                             unsigned count, uint32_t address,
                                             unsigned size);

typedef struct DB2010Access DB2010Access;
typedef struct DB2010DecodedAccess DB2010DecodedAccess;
typedef struct DB2010RegRule DB2010RegRule;

/* Width masks name transfer sizes in bytes, independently of backing width. */
#define DB_WIDTH8  (1u << 1)
#define DB_WIDTH16 (1u << 2)
#define DB_WIDTH32 (1u << 4)
#define DB_ALIGN_NATURAL 0

typedef const char *(*DB2010RegCallback)(void *opaque,
    const DB2010DecodedAccess *decoded, DB2010Access *access);
/* Guards and field resolution must not change owner/peer state or outputs. */
typedef const char *(*DB2010RegGuard)(const void *opaque,
    const DB2010DecodedAccess *decoded, const DB2010Access *access);
typedef struct DB2010FieldRef {
    void *value;
    uint32_t *known; /* NULL explicitly declares all backing bits known. */
    uint32_t mask;
} DB2010FieldRef;
typedef const char *(*DB2010FieldResolve)(void *opaque,
    const DB2010DecodedAccess *decoded, const DB2010Access *access,
    DB2010FieldRef *field);

typedef enum DB2010ValueKind {
    DB_VALUE_ANY, DB_VALUE_SET, DB_VALUE_MASKED, DB_VALUE_RANGE,
} DB2010ValueKind;
typedef struct DB2010ValueConstraint {
    DB2010ValueKind kind;
    const uint32_t *values;
    unsigned count;
    uint32_t mask, value, minimum, maximum;
    const char *fault;
} DB2010ValueConstraint;

typedef enum DB2010ActionKind {
    DB_ACTION_UNSET, DB_ACTION_CALLBACK, DB_ACTION_REJECT, DB_ACTION_CONSTANT,
    DB_ACTION_FIELD, DB_ACTION_READ_CLEAR, DB_ACTION_W1C, DB_ACTION_IGNORE,
} DB2010ActionKind;
typedef struct DB2010RegAction {
    DB2010ActionKind kind;
    unsigned widths;
    DB2010RegCallback callback;
    DB2010RegGuard guard;
    DB2010FieldResolve field;
    DB2010ValueConstraint constraint; /* Raw write values; no normalization. */
    const char *fault;
    uint32_t constant;
    bool constant_by_width;
} DB2010RegAction;

typedef enum DB2010RuleKind {
    DB_RULE_REGISTER, DB_RULE_ARRAY, DB_RULE_REPEAT, DB_RULE_WINDOW, DB_RULE_VIEW,
} DB2010RuleKind;
struct DB2010RegRule {
    DB2010RuleKind kind;
    uint32_t offset, span;
    unsigned id, count, stride, alignment, backing_width;
    const void *selector;
    const uint32_t *bases; /* Optional nonuniform offsets for repeated groups. */
    const DB2010RegRule *children;
    unsigned child_count; /* Never the repetition count. */
    DB2010RegAction read, write;
};

/* Initializer shorthand only; state, validation and timing remain ordinary C. */
#define DB_REG(off, ...) \
    { .kind = DB_RULE_REGISTER, .offset = (off), .count = 1, __VA_ARGS__ }
#define DB_ARRAY(off, n, ...) \
    { .kind = DB_RULE_ARRAY, .offset = (off), .count = (n), __VA_ARGS__ }
#define DB_REPEAT(off, n, ...) \
    { .kind = DB_RULE_REPEAT, .offset = (off), .count = (n), __VA_ARGS__ }
#define DB_WINDOW(off, length, ...) \
    { .kind = DB_RULE_WINDOW, .offset = (off), .span = (length), __VA_ARGS__ }
#define DB_VIEW(off, ...) \
    { .kind = DB_RULE_VIEW, .offset = (off), .count = 1, __VA_ARGS__ }

typedef struct DB2010FlatRule {
    const DB2010RegRule *rule;
    uint32_t address, span;
    unsigned bank;
} DB2010FlatRule;
typedef struct DB2010RegMap {
    DB2010FlatRule *rules;
    unsigned count;
    DB2010RegisterSpec *claims;
    unsigned claim_count;
    const char *fault;
} DB2010RegMap;
struct DB2010DecodedAccess {
    const DB2010RegRule *rule;
    const DB2010RegAction *action;
    unsigned rule_index, id, bank, element;
    const void *selector;
    uint32_t address, offset;
    unsigned size;
    bool write;
};

/* Compile once, then treat as immutable until all owners using it are gone.
 * map must be zero-initialized; failures leave it untouched. Descriptors and
 * selector/constraint data must outlive the compiled map. A window with children
 * claims its guarded envelope; a window without actions can guard empty holes.
 */
const char *db2010_regmap_init(DB2010RegMap *map, uint32_t base,
    const DB2010RegRule *rules, unsigned count, const char *fault);
void db2010_regmap_destroy(DB2010RegMap *map);
const char *db2010_reg_decode(const DB2010RegMap *map,
    const DB2010Access *access, DB2010DecodedAccess *decoded);
/* Constraints then pure guard then action. Owners needing other precedence
 * keep compound validation in a callback or call these helpers explicitly.
 * No implicit time update, peer preflight, preview copy or post-validation work.
 */
const char *db2010_reg_value_check(const DB2010ValueConstraint *constraint,
                                  uint32_t value);
const char *db2010_reg_execute(void *opaque,
    const DB2010DecodedAccess *decoded, DB2010Access *access);
#endif
