/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNICORN_TEXT_WRAP_H
#define UNICORN_TEXT_WRAP_H
/* Unicorn Overlord: story lines that the PS4 speech bubble cannot hold (#73).
 *
 * The bubble is two lines tall. It grows sideways to the longest line, up to about 57 of the
 * dialogue font's fixed-width Latin letters, but a third line falls out of it. The Japanese story
 * text keeps to two lines (12 of its 489,999 entries have three). The fan English patches break
 * their lines at about 47 letters and so use three lines in 5,813 entries. Two fixes, in order:
 *
 * 1. Fewer lines. FUN_0002b590 (the text lookup, replaced by native_settings_text) passes each
 *    story entry (the sheet with over 400,000 entries) through text_wrap_story. An entry of three
 *    or more lines that has spaces and fits two lines of TEXT_WRAP_WIDTH visible letters gets
 *    one break, at the space that best balances the two lines (about 4,000 of the 5,813).
 *    Japanese and Chinese text has no spaces and is never changed.
 * 2. Smaller text. The bubble's set-up (FUN_00a27990) stores the text scale in its text object
 *    (+0x164 x, +0x168 y; 1.1 from the game's tuning values 0xcee/0xcef) and then sets the text
 *    with the call at TEXT_BUBBLE_CALL to FUN_0002c590, which lays it out at that scale. The probe
 *    sends that call through a hook that counts the message's lines (its text is inline, at most
 *    0x100 bytes) and, for three or more, lays it out at text_bubble_scale and then puts the
 *    game's scale back. Three lines at 0.85 fit the bubble.
 *
 * VW_TEXT_FIT=0 turns both off. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_WRAP_WIDTH 56
#define TEXT_WRAP_STORY_MIN 400000u
/* The bubble's call to FUN_0002c590 (v01.04 offsets) and the bytes before it: lea rsi,
 * [rsp+0x80]; mov rdi, r14; call. 0xa27ad2 loads the scale's tuning id: mov esi, 0xcee. */
#define TEXT_BUBBLE_CALL 0xa27bf9u
#define TEXT_BUBBLE_SET_TEXT 0x2c590u
#define TEXT_BUBBLE_SCALE_ID 0xa27ad2u
static const unsigned char text_bubble_call_signature[] = {0x48, 0x8d, 0xb4, 0x24, 0x80, 0x00, 0x00,
                                                           0x00, 0x4c, 0x89, 0xf7, 0xe8};
static const unsigned char text_bubble_scale_signature[] = {0xbe, 0xee, 0x0c, 0x00, 0x00};

/* Whether the fixes are on (VW_TEXT_FIT, read once). */
static int text_fit_state; /* 0 unknown, 1 on, -1 off */
static inline int text_fit_enabled(void) {
    int state = __atomic_load_n(&text_fit_state, __ATOMIC_RELAXED);
    if (!state) {
        const char *env = getenv("VW_TEXT_FIT");
        state = env && env[0] == '0' ? -1 : 1;
        __atomic_store_n(&text_fit_state, state, __ATOMIC_RELAXED);
    }
    return state > 0;
}

/* Off for the game's Western text languages (1-5): the game then wraps lines and sizes the
 * bubbles itself, as in its Western releases. */
static inline void text_fit_disable(void) {
    __atomic_store_n(&text_fit_state, -1, __ATOMIC_RELAXED);
}

/* Lines of the text in `buffer` (at most `size` bytes, ending at the first zero). */
static inline int text_bubble_lines(const unsigned char *buffer, size_t size) {
    int lines = 1;
    for (size_t i = 0; i < size && buffer[i]; ++i) lines += buffer[i] == '\n';
    return lines;
}

/* The scale to lay out `lines` lines at, for the game's `scale`: its own up to two lines; for
 * more, small enough that they take the height two take at 1.1 (0.85 for three). */
static inline float text_bubble_scale(float scale, int lines) {
    return lines >= 3 ? scale * 2.32f / (float)lines : scale;
}

/* The letters `s[0..n)` shows: control codes (fms.py CODE) take no room, UTF-8 continuation
 * bytes belong to the letter before. */
static inline int text_wrap_visible(const char *s, size_t n) {
    int count = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '#' && i + 1 < n) {
            size_t j = i + 1;
            if (s[j] == '/') { /* #/c */
                ++j;
                while (j < n && ((s[j] | 0x20) >= 'a' && (s[j] | 0x20) <= 'z')) ++j;
                if (j > i + 2) { i = j; continue; }
            } else {
                size_t k = j;
                while (k < n && k < j + 2 && ((s[k] | 0x20) >= 'a' && (s[k] | 0x20) <= 'z')) ++k;
                if (k > j && k < n && s[k] == '(') { /* #c(12), #(2) has no letter: counted */
                    const char *close = (const char *)memchr(s + k, ')', n - k);
                    if (close) { i = (size_t)(close - s) + 1; continue; }
                }
                if (s[j] == 'i' && j + 1 < n && s[j + 1] != ' ' && s[j + 1] != '\n') { i = j + 1; continue; }
            }
        }
        if (((unsigned char)s[i] & 0xc0) != 0x80) ++count;
        ++i;
    }
    return count;
}

/* `text` in two lines of at most `width` visible letters, or NULL (fewer than three lines, no
 * space to break at, or too long). The result is malloc'd. */
static inline char *text_wrap_two_lines(const char *text, int width) {
    size_t n = strlen(text), breaks = 0;
    for (size_t i = 0; i < n; ++i) breaks += text[i] == '\n';
    /* Text without spaces (Japanese, Chinese) is not broken at the spaces joining adds. */
    if (breaks < 2 || !memchr(text, ' ', n)) return NULL;
    char *joined = (char *)malloc(n + 1);
    if (!joined) return NULL;
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] != '\n') { joined[m++] = text[i]; continue; }
        if (m && joined[m - 1] != ' ' && i + 1 < n && text[i + 1] != ' ') joined[m++] = ' ';
    }
    joined[m] = 0;
    size_t best = 0;
    int best_long = width + 1;
    for (size_t i = 1; i + 1 < m; ++i) {
        if (joined[i] != ' ') continue;
        const int left = text_wrap_visible(joined, i), right = text_wrap_visible(joined + i + 1, m - i - 1);
        const int longer = left > right ? left : right;
        if (longer < best_long) { best_long = longer; best = i; }
    }
    if (!best) { free(joined); return NULL; }
    joined[best] = '\n';
    return joined;
}

typedef struct { const char *source, *text; } TextWrapEntry;
typedef struct { const unsigned char *table; TextWrapEntry **entries; uint32_t count; } TextWrapSheet;
static TextWrapSheet text_wrap_sheets[4];

/* The entry of the story sheet `table` (`count` entries) to show for `text`. */
static inline const char *text_wrap_story(const unsigned char *table, uint32_t count, int id, const char *text) {
    if (!text_fit_enabled() || !text || count < TEXT_WRAP_STORY_MIN || id < 0 || (uint32_t)id >= count) return text;
    TextWrapEntry **entries = NULL;
    for (unsigned i = 0; i < sizeof(text_wrap_sheets) / sizeof(*text_wrap_sheets) && !entries;) {
        TextWrapSheet *s = &text_wrap_sheets[i];
        const unsigned char *owner = __atomic_load_n(&s->table, __ATOMIC_ACQUIRE);
        if (owner == table) {
            entries = __atomic_load_n(&s->entries, __ATOMIC_ACQUIRE);
            /* Not made yet (another thread), or another sheet now at that address. */
            if (!entries || s->count != count) return text;
        } else if (!owner) {
            const unsigned char *expected = NULL;
            if (!__atomic_compare_exchange_n(&s->table, &expected, table, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue; /* taken meanwhile: look at this slot again */
            s->count = count;
            TextWrapEntry **made = (TextWrapEntry **)calloc(count, sizeof(*made));
            if (!made) return text;
            __atomic_store_n(&s->entries, made, __ATOMIC_RELEASE);
            entries = made;
        } else {
            ++i;
        }
    }
    if (!entries) return text; /* more sheets than slots */
    TextWrapEntry *entry = __atomic_load_n(&entries[id], __ATOMIC_ACQUIRE);
    if (entry && entry->source == text) return entry->text;
    TextWrapEntry *made = (TextWrapEntry *)malloc(sizeof(*made));
    if (!made) return text;
    char *wrapped = text_wrap_two_lines(text, TEXT_WRAP_WIDTH);
    made->source = text;
    made->text = wrapped ? wrapped : text;
    /* An older entry (the sheet was loaded again) stays allocated: the game may still hold it. */
    __atomic_store_n(&entries[id], made, __ATOMIC_RELEASE);
    return made->text;
}

#endif
