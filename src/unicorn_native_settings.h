/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNICORN_NATIVE_SETTINGS_H
#define UNICORN_NATIVE_SETTINGS_H
/* Unicorn Overlord: the port's settings as a "PC" section of the game's own Options screen
 * (System > Options), after its 21 rows. The rows are vwgpu_setting_rows() (gpu/vwgpu.h).
 *
 * The screen (UcUI_Option, built by FUN_00735490) reads three static tables: the rows
 * (0xd7c9f0: 21 x {float default, int label, int help, int kind, int flag}) and two orders of
 * them (0xd7c940, 21 rows; 0xd7c9a0, 20 rows without Difficulty; the global at 0x234eee8 picks
 * one). Copies with the added rows (ids 21 and up) replace them: every lea that loads a table
 * is repointed and every bound (0x13, 0x14, 0x15) is raised. The game keeps a row's value in a
 * float array of its system data indexed by row id (21 floats); three branches that would index
 * past it read and write `values` here instead. Choice rows take {id, count, first text} from a
 * table the builder picks by id, sliders {id, format text, step, min, max, decimals} from a
 * table indexed by id - 4: both are copied and extended. Texts are ids of MSGSHEET/UcOption.FMS
 * looked up by FUN_0002b590, replaced here so ids from NATIVE_TEXT return the port's strings.
 * Two bit masks of row ids name the section headings (0x1441: the cursor skips them) and the
 * on/off rows (0x1d680c: redrawn once the screen is shown); the added ids join them.
 * The screen's update (FUN_00738f70) is wrapped: after it runs, changed rows are applied
 * through vwgpu_setting_set and their "value on open" is updated, so closing the screen never
 * writes them to the game's data. Opening the screen (FUN_00735490) loads `values` first.
 * All addresses are CUSA36381 v01.04 image offsets; install checks each patched instruction. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "unicorn_build.h"
#include "unicorn_text_wrap.h"
#include "../gpu/vwgpu.h"

#ifndef ABI /* src/runtime.h: the game's calling convention */
#define ABI __attribute__((sysv_abi))
#endif

#define NATIVE_GAME_ROWS 21u
#define NATIVE_MAX_ROWS 11u   /* ids 21..31: within the 32-bit masks; (id & 0x1b) == 1 is a game row */
/* text id = NATIVE_TEXT + row * 64 + slot. FUN_0002b590 serves every sheet: the story text
 * (UcScriptMsgConv.fms) has 489,999 ids, so the port's ids start past the largest sheet. */
#define NATIVE_TEXT 0x100000
enum { NATIVE_LABEL = 0, NATIVE_HELP = 1, NATIVE_FORMAT = 31, NATIVE_CHOICE = 32 };

/* The screen object (UcUI_Option) and its rows. */
#define NATIVE_UI_ROWS 0x6298   /* std::vector<Row *> in screen order */
#define NATIVE_UI_SAVED 0x62b8  /* float * : each row's value when the screen opened */
#define NATIVE_UI_COUNT 0x62d8  /* int: rows on the screen */
#define NATIVE_ROW_VALUE 0xe8   /* vtable: float value() */

typedef struct { float fallback; int32_t label, help, kind, flag; } NativeRow;
typedef struct { int32_t id, count, first_text; } NativeChoice;
typedef struct { int32_t id, format_text; float step, min, max; int32_t decimals; } NativeSlider;

typedef struct {
    NativeRow rows[NATIVE_GAME_ROWS + NATIVE_MAX_ROWS];
    uint32_t order[NATIVE_GAME_ROWS + NATIVE_MAX_ROWS];         /* with Difficulty */
    uint32_t order_short[NATIVE_GAME_ROWS - 1 + NATIVE_MAX_ROWS]; /* without */
    uint64_t sliders[NATIVE_GAME_ROWS + NATIVE_MAX_ROWS - 4];     /* NativeSlider *, by id - 4 */
    uint64_t choices[NATIVE_GAME_ROWS + NATIVE_MAX_ROWS];         /* NativeChoice *, by id */
    NativeSlider slider[NATIVE_MAX_ROWS];
    NativeChoice choice[NATIVE_MAX_ROWS];
    float values[NATIVE_MAX_ROWS];
} NativeTables;

typedef struct {
    const VwgpuSettingRow *rows;
    unsigned count;
    NativeTables *tables;
    void (ABI *open)(void *ui);
    void (ABI *update)(void *ui);
} NativeSettings;

static NativeSettings native_settings;

static ABI const char *native_settings_text(const unsigned char *sheet, int id) {
    /* FUN_0002b590: the sheet's string table (count at 0x14, pointers from 0x20). */
    const unsigned char *table;
    uint32_t count;
    memcpy(&table, sheet + 0x128, sizeof(table));
    memcpy(&count, table + 0x14, sizeof(count));
    if (id >= 0 && (uint32_t)id < count) {
        const char *text;
        memcpy(&text, table + 0x20 + (uint64_t)id * 8, sizeof(text));
        return count >= TEXT_WRAP_STORY_MIN ? text_wrap_story(table, count, id, text) : text;
    }
    if (id >= NATIVE_TEXT) {
        const unsigned row = (unsigned)(id - NATIVE_TEXT) / 64, slot = (unsigned)(id - NATIVE_TEXT) % 64;
        if (row >= native_settings.count) return NULL;
        const VwgpuSettingRow *r = &native_settings.rows[row];
        if (slot == NATIVE_LABEL) return r->label;
        if (slot == NATIVE_FORMAT) return r->format;
        if (slot >= NATIVE_CHOICE) return slot - NATIVE_CHOICE < (unsigned)r->count ? r->choices[slot - NATIVE_CHOICE] : NULL;
        return r->help; /* NATIVE_HELP + the row's value */
    }
    return NULL;
}

static ABI void native_settings_open(void *ui) {
    for (unsigned i = 0; i < native_settings.count; ++i) native_settings.tables->values[i] = vwgpu_setting_value((int)i);
    native_settings.open(ui);
}

/* After the screen's update: the added rows are the last ones on it. */
static ABI void native_settings_update(void *ui) {
    native_settings.update(ui);
    unsigned char *base = ui;
    void **rows, **rows_end;
    float *saved;
    int32_t count;
    memcpy(&rows, base + NATIVE_UI_ROWS, sizeof(rows));
    memcpy(&rows_end, base + NATIVE_UI_ROWS + 8, sizeof(rows_end));
    memcpy(&saved, base + NATIVE_UI_SAVED, sizeof(saved));
    memcpy(&count, base + NATIVE_UI_COUNT, sizeof(count));
    if (!rows || !saved || count < (int32_t)native_settings.count || rows_end - rows < count) return;
    for (unsigned i = 0; i < native_settings.count; ++i) {
        if (native_settings.rows[i].kind == VWGPU_ROW_SECTION) continue;
        const int32_t at = count - (int32_t)native_settings.count + (int32_t)i;
        unsigned char *row = rows[at];
        const unsigned char *vtable;
        memcpy(&vtable, row, sizeof(vtable));
        float (ABI *value_of)(void *);
        memcpy(&value_of, vtable + NATIVE_ROW_VALUE, sizeof(value_of));
        const float value = value_of(row);
        if (value != native_settings.tables->values[i]) {
            native_settings.tables->values[i] = value;
            vwgpu_setting_set((int)i, value);
        }
        saved[at] = value;
    }
}

/* lea reg, [rip + disp32] at `at` that loads `from`; with `to`, it loads `to` instead. */
static int native_lea(unsigned char *image, uint64_t at, uint64_t from, const void *to) {
    at = unicorn_address((uint32_t)at), from = unicorn_address((uint32_t)from);
    int32_t disp;
    memcpy(&disp, image + at + 3, 4);
    if ((image[at] != 0x48 && image[at] != 0x4c) || image[at + 1] != 0x8d || at + 7 + (int64_t)disp != from) return 0;
    if (to) {
        disp = (int32_t)((int64_t)(uintptr_t)to - (int64_t)(uintptr_t)(image + at + 7));
        memcpy(image + at + 3, &disp, 4);
    }
    return 1;
}

/* A page every rel32 and disp32 of the image's first 64 MiB can reach. */
static int native_reaches(const unsigned char *image, const void *page) {
    const int64_t delta = (int64_t)(uintptr_t)page - (int64_t)(uintptr_t)image;
    return delta > INT32_MIN + (64 << 20) && delta < INT32_MAX - (64 << 20);
}

static int native_bytes(const unsigned char *image, uint64_t at, const char *hex) {
    at = unicorn_address((uint32_t)at);
    for (size_t i = 0; hex[2 * i]; ++i) {
        unsigned value;
        if (sscanf(hex + 2 * i, "%2x", &value) != 1 || image[at + i] != value) return 0;
    }
    return 1;
}

static void native_rel32(unsigned char *image, uint64_t field, const void *target) {
    field = unicorn_address((uint32_t)field);
    const int32_t rel = (int32_t)((int64_t)(uintptr_t)target - (int64_t)(uintptr_t)(image + field + 4));
    memcpy(image + field, &rel, 4);
}

static void native_or32(unsigned char *image, uint64_t at, uint32_t bits) {
    at = unicorn_address((uint32_t)at);
    uint32_t value;
    memcpy(&value, image + at, 4);
    value |= bits;
    memcpy(image + at, &value, 4);
}

/* Code writers for the stubs. */
static unsigned char *native_movabs(unsigned char *p, unsigned char reg, uint64_t value) {
    *p++ = 0x48; *p++ = (unsigned char)(0xb8 + reg);
    memcpy(p, &value, 8);
    return p + 8;
}
static unsigned char *native_jump(unsigned char *p, const void *target) { /* jmp [rip]; dq target */
    const uint64_t address = (uintptr_t)target;
    *p++ = 0xff; *p++ = 0x25; memset(p, 0, 4); p += 4;
    memcpy(p, &address, 8);
    return p + 8;
}
/* A function entry whose first 6 bytes are push rbp; mov rbp, rsp; push r15 (moved to the
 * trampoline) jumps to `hook`; returns the trampoline that runs the original. */
#define NATIVE_PROLOGUE "554889e54157"
static void *native_wrap(unsigned char *image, uint64_t at, unsigned char **code, const void *hook) {
    unsigned char *stub = *code, *trampoline = native_movabs(stub, 0, (uintptr_t)hook);
    *trampoline++ = 0xff; *trampoline++ = 0xe0; /* jmp rax */
    memcpy(trampoline, image + unicorn_address((uint32_t)at), 6);
    *code = native_jump(trampoline + 6, image + unicorn_address((uint32_t)at) + 6);
    image[unicorn_address((uint32_t)at)] = 0xe9;
    native_rel32(image, at + 1, stub);
    image[unicorn_address((uint32_t)at) + 5] = 0x90;
    return trampoline;
}

/* The game's tables. */
#define NATIVE_ORDER 0xd7c940
#define NATIVE_ORDER_SHORT 0xd7c9a0
#define NATIVE_ROWS 0xd7c9f0
#define NATIVE_SLIDERS 0x14fcdb0
#define NATIVE_COUNT(a) (sizeof(a) / sizeof(*(a)))

typedef struct { uint64_t at; const char *bytes; } NativeCode;
/* What is patched, as v01.04 has it. */
static const NativeCode native_code[] = {
    {0x735719, "83c814"},               /* open: rows = (order == long) | 0x14 */
    {0x738900, "771e4183fd01"},         /* open: id > 20 -> system data */
    {0x738916, "662e0f1f840000000000"}, /* (padding) */
    {0x738a31, "0f8709cdffff"},         /* open: the "value on open" of id > 20 */
    {0x73896f, "4183fd05b900000000"},   /* open: choice table by id */
    {0x739901, "771d4183fd01"},         /* close: write a changed value of id > 20 */
    {0x7398b6, "662e0f1f840000000000"}, /* (padding) */
    {0x738e88, "b941140000"},           /* section headings */
    {0x739f82, "b941140000"},
    {0x738ed1, "41bc0c681d00"},         /* on/off rows */
    {0x2b590, "85f67814488b8728010000"}, /* FUN_0002b590, the text lookup */
    {0x735490, NATIVE_PROLOGUE},        /* open */
    {0x738f70, NATIVE_PROLOGUE},        /* update */
};
/* cmp against the last index or the count; the immediate is the last byte. */
static const NativeCode native_bounds[] = {
    {0x735790, "4983ff13"}, {0x7357b5, "4983ff14"}, {0x7357c0, "83fa15"}, /* open */
    {0x739120, "4183fc13"}, {0x739135, "4183fc14"},                       /* update */
    {0x7398aa, "4883fb13"}, {0x7398c6, "4883fb14"},                       /* close */
};
static const struct { uint64_t at, table; } native_leas[] = {
    {0x7353b9, NATIVE_ORDER}, {0x7356ff, NATIVE_ORDER}, {0x738e7d, NATIVE_ORDER}, {0x738eca, NATIVE_ORDER},
    {0x739049, NATIVE_ORDER}, {0x73913b, NATIVE_ORDER}, {0x739265, NATIVE_ORDER}, {0x739857, NATIVE_ORDER},
    {0x739f77, NATIVE_ORDER}, {0x739fde, NATIVE_ORDER},
    {0x7353b0, NATIVE_ORDER_SHORT}, {0x735796, NATIVE_ORDER_SHORT}, {0x738e74, NATIVE_ORDER_SHORT},
    {0x738ed7, NATIVE_ORDER_SHORT}, {0x73903e, NATIVE_ORDER_SHORT}, {0x739126, NATIVE_ORDER_SHORT},
    {0x7392a5, NATIVE_ORDER_SHORT}, {0x73985e, NATIVE_ORDER_SHORT}, {0x739f6c, NATIVE_ORDER_SHORT},
    {0x739fd2, NATIVE_ORDER_SHORT},
    {0x7353c4, NATIVE_ROWS}, {0x7357d2, NATIVE_ROWS}, {0x7392c3, NATIVE_ROWS},
    {0x7389d8, NATIVE_SLIDERS},
};

/* Installs the rows. image: the relocated game, writable; code, data: a page each within
 * 2 GiB of it (code is made executable by the caller). 0 when the game differs from v01.04. */
static int native_settings_install(unsigned char *image, unsigned char *code, unsigned char *data) {
    NativeSettings *s = &native_settings;
    s->count = (unsigned)vwgpu_setting_rows(&s->rows);
    if (s->count > NATIVE_MAX_ROWS) s->count = NATIVE_MAX_ROWS;
    NativeTables *t = s->tables = (NativeTables *)data;
    const unsigned rows = NATIVE_GAME_ROWS + s->count;

    /* Check every instruction first: nothing is written to a game that differs. */
    int ok = native_reaches(image, code) && native_reaches(image, data);
    for (size_t i = 0; ok && i < NATIVE_COUNT(native_code); ++i) ok = native_bytes(image, native_code[i].at, native_code[i].bytes);
    for (size_t i = 0; ok && i < NATIVE_COUNT(native_bounds); ++i) ok = native_bytes(image, native_bounds[i].at, native_bounds[i].bytes);
    for (size_t i = 0; ok && i < NATIVE_COUNT(native_leas); ++i) ok = native_lea(image, native_leas[i].at, native_leas[i].table, NULL);
    if (!ok) return 0;

    /* Tables: the game's rows, then the port's. */
    memcpy(t->rows, image + unicorn_address(NATIVE_ROWS), sizeof(NativeRow) * NATIVE_GAME_ROWS);
    memcpy(t->order, image + unicorn_address(NATIVE_ORDER), 4 * NATIVE_GAME_ROWS);
    memcpy(t->order_short, image + unicorn_address(NATIVE_ORDER_SHORT), 4 * (NATIVE_GAME_ROWS - 1));
    memcpy(t->sliders, image + unicorn_address(NATIVE_SLIDERS), 8 * 14); /* ids 4..17; the table ends there */
    t->choices[1] = (uintptr_t)(image + unicorn_address(0xd7cba0)); /* Difficulty */
    t->choices[5] = (uintptr_t)(image + unicorn_address(0xd7cbac)); /* Voice Language */
    for (unsigned i = 0; i < s->count; ++i) {
        const VwgpuSettingRow *r = &s->rows[i];
        const int32_t id = (int32_t)(NATIVE_GAME_ROWS + i), text = NATIVE_TEXT + (int32_t)i * 64;
        t->rows[id] = (NativeRow){r->fallback, text + NATIVE_LABEL, r->kind == VWGPU_ROW_SECTION ? -1 : text + NATIVE_HELP,
                                  r->kind, 0};
        t->order[id] = (uint32_t)id;
        t->order_short[id - 1] = (uint32_t)id;
        t->slider[i] = (NativeSlider){id, r->format ? text + NATIVE_FORMAT : -1, r->step, r->min, r->max, 0};
        t->sliders[id - 4] = (uintptr_t)&t->slider[i];
        t->choice[i] = (NativeChoice){id, r->count, text + NATIVE_CHOICE};
        t->choices[id] = (uintptr_t)&t->choice[i];
    }

    for (size_t i = 0; i < NATIVE_COUNT(native_leas); ++i) {
        const uint64_t table = native_leas[i].table;
        native_lea(image, native_leas[i].at, table,
                   table == NATIVE_ORDER ? (void *)t->order : table == NATIVE_ORDER_SHORT ? (void *)t->order_short
                   : table == NATIVE_ROWS ? (void *)t->rows : (void *)t->sliders);
    }
    uint32_t headings = 0, switches = 0;
    for (unsigned i = 0; i < s->count; ++i) {
        if (s->rows[i].kind == VWGPU_ROW_SECTION) headings |= 1u << (NATIVE_GAME_ROWS + i);
        if (s->rows[i].kind == VWGPU_ROW_SWITCH) switches |= 1u << (NATIVE_GAME_ROWS + i);
    }
    native_or32(image, 0x738e88 + 1, headings);
    native_or32(image, 0x739f82 + 1, headings);
    native_or32(image, 0x738ed1 + 2, switches);
    image[unicorn_address(0x735719) + 1] = 0xc0; /* or eax, 0x14 -> add eax, 0x14 + added rows */
    image[unicorn_address(0x735719) + 2] = (unsigned char)(rows - 1);
    for (size_t i = 0; i < NATIVE_COUNT(native_bounds); ++i) {
        const uint64_t last = unicorn_address((uint32_t)native_bounds[i].at) + strlen(native_bounds[i].bytes) / 2 - 1;
        image[last] = (unsigned char)(image[last] + s->count);
    }

    /* Values of ids past the system data's 21: values[id - 21]. rax = values - 21 floats, then
     * back to the instruction that indexes it by id. */
    const uint64_t values = (uintptr_t)t->values - 4 * NATIVE_GAME_ROWS;
    unsigned char *p = code;
    unsigned char *open_value = p;                     /* open: the row's value */
    p = native_jump(native_movabs(p, 0, values), image + unicorn_address(0x738940));
    unsigned char *open_saved = p;                     /* open: the value on open */
    p = native_jump(native_movabs(p, 0, values), image + unicorn_address(0x735756));
    unsigned char *close_value = p;                    /* close: write the changed value */
    p = native_jump(native_movabs(p, 0, values), image + unicorn_address(0x739871));
    unsigned char *choice = p;                         /* open: rcx = choices[id] */
    memcpy(p, image + unicorn_address(0x73897f), 8); p += 8;            /* mov rax, [r12 + 0x6298] */
    *p++ = 0x49; *p++ = 0x63; *p++ = 0xcd;             /* movsxd rcx, r13d */
    p = native_movabs(p, 2, (uintptr_t)t->choices);    /* movabs rdx, choices */
    *p++ = 0x48; *p++ = 0x8b; *p++ = 0x0c; *p++ = 0xca; /* mov rcx, [rdx + rcx * 8] */
    p = native_jump(p, image + unicorn_address(0x73899a));
    unsigned char *text = p;                           /* FUN_0002b590 */
    p = native_movabs(p, 0, (uintptr_t)native_settings_text);
    *p++ = 0xff; *p++ = 0xe0;

    image[unicorn_address(0x738901)] = 0x14;                            /* ja -> 0x738916 */
    image[unicorn_address(0x738916)] = 0xe9;
    image[unicorn_address(0x7398b6)] = 0xe9;
    image[unicorn_address(0x739902)] = (unsigned char)(0x7398b6 - 0x739903);
    image[unicorn_address(0x73896f)] = 0xe9;
    image[unicorn_address(0x2b590)] = 0xe9;
    native_rel32(image, 0x738917, open_value);
    native_rel32(image, 0x738a33, open_saved);
    native_rel32(image, 0x7398b7, close_value);
    native_rel32(image, 0x738970, choice);
    native_rel32(image, 0x2b591, text);
    s->open = native_wrap(image, 0x735490, &p, native_settings_open);
    s->update = native_wrap(image, 0x738f70, &p, native_settings_update);
    return 1;
}

#endif
