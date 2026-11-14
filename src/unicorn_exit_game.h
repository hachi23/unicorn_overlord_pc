/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNICORN_EXIT_GAME_H
#define UNICORN_EXIT_GAME_H
/* Unicorn Overlord: "Exit Game" in the game's own menus (#91). The game has no way out; the
 * Insert menu's Quit game is the port's. This adds one to the title menu (after System) and to
 * the System menu of the world map and of the battle map (after Load).
 *
 * Title (UcUI_Title, vtable 0x15c5f78, one at a time in the global 0x234eee8). Its menu is a
 * list widget at +0x9c60 holding button widgets (0x570 bytes each, built by FUN_009ed290) that
 * live inside the title object: Continue +0x4560, New Game +0x4ad0, Load Game +0x5040, Library
 * +0x55b0, System +0x5b20. FUN_00a455b0 fills the list (on opening and when a submenu closes).
 * It is wrapped: afterwards the port's button (allocated with the game's operator new, one per
 * title object; the title's destructor FUN_00a432f0 is wrapped to forget it) is added the same
 * way and the list's count mirrors are updated. On Cross the update (FUN_00a3e910, state 4)
 * walks the list to the chosen button and compares it with the five; the last test
 * (`cmp rcx, System; jne 0xa4201b`, the "no" sound) goes to a stub that plays the confirm sound
 * and quits when it is the port's button.
 *
 * World map (vtable 0x159b4d0, builder FUN_007827d0) and battle map (builder FUN_007de9e0):
 * their System submenu is a list object (+0x598 and +0x4d8) of up to 20 entries of 0x2510 bytes
 * from +0x1440: action at +0x3600 (the menu's next state), an id at +0x3604 nothing reads, the
 * label text at +0x33d0, the count at +0x2f650. Options 0xc, Text Log 0xd, Return to Title 0xe,
 * Load 0xf. The builders are wrapped: an entry with action 0xe and id EXIT_MARK is appended.
 * State 0xe asks UcSysMsg 732 ("Return to the title screen?") in the game's Yes/No dialog,
 * fades out on Yes, then sets the go-to-title flag (byte +0x142 of *0x21d7500). Four stubs per
 * menu: where the chosen entry's action is read, remember whether it was the port's; the help
 * line on opening the submenu (886 + cursor) and the dialog's question become the port's for
 * it; and after the fade, quit instead of setting the go-to-title flag.
 *
 * Texts: FUN_00280db0 sets a text object from a UcSysMsg id and keeps -1 for an id past the
 * sheet, so the port's English texts go in with FUN_0002c590 (set the string), as the title
 * does for its literal texts. All addresses are CUSA36381 v01.04 image offsets; install checks
 * each patched instruction and the start of each game function it calls. */
#include <stdint.h>
#include <string.h>
#include "unicorn_build.h"
#include "unicorn_native_settings.h"

static const char exit_label[] = "Exit Game";
static const char exit_help[] = "Close the game and return to the desktop.";
static const char exit_question[] = "Exit the game?\nAny unsaved progress will be lost.";

#define EXIT_MARK 0x45584954u /* "EXIT": the entry id of the port's System menu entry */
#define EXIT_RETURN_TO_TITLE 0xe
#define EXIT_LIST_MAX 20
#define EXIT_ENTRY 0x2510
#define EXIT_ENTRY_ACTION 0x3600
#define EXIT_ENTRY_ID 0x3604
#define EXIT_LIST_COUNT 0x2f650
#define EXIT_BUTTON_SIZE 0x570
#define EXIT_TITLE_LIST 0x9c60
#define EXIT_LIST_ITEMS 0x148 /* the list's item count; +0x370 and +0x374 mirror it */
#define EXIT_COLOR_ON 0xa4a720

/* The game's functions it calls. */
#define EXIT_NEW 0x9760
#define EXIT_BUTTON_INIT 0x9ed290
#define EXIT_BUTTON_RESET 0x9e9900
#define EXIT_LIST_ADD 0x9ebcb0
#define EXIT_FADE 0x4c0
#define EXIT_TEXT_STOP 0x33030
#define EXIT_TEXT_RESTART 0x280a50
#define EXIT_TEXT_SHOW 0x32fc0
#define EXIT_TEXT_ID 0x280db0
#define EXIT_TEXT_STRING 0x2c590
#define EXIT_SOUND 0x5c9d00
static const NativeCode exit_calls[] = {
    {EXIT_NEW, "554889e5535048"},
    {EXIT_BUTTON_INIT, "554889e54157415653"},
    {EXIT_BUTTON_RESET, "4156534c8bb7b0000000"},
    {EXIT_LIST_ADD, "554889e541565348"},
    {EXIT_FADE, "c5f96e02897724"},
    {EXIT_TEXT_STOP, "554889e5535048"},
    {EXIT_TEXT_RESTART, "554889e541565348"},
    {EXIT_TEXT_SHOW, "808fd800000040c3"},
    {EXIT_TEXT_ID, "488b4f40"},
    {EXIT_TEXT_STRING, "554889e54157415641545348"},
    {EXIT_SOUND, "554889e541574156"},
};

/* The title's sites. */
#define EXIT_TITLE_BUILD 0xa455b0
#define EXIT_TITLE_FREE 0xa432f0
#define EXIT_TITLE_COMPARE 0xa4265d /* lea rax, [r13 + 0x5b20]; cmp rcx, rax; jne 0xa4201b */
#define EXIT_TITLE_COMPARE_BYTES "498d85205b00004839c10f85aef9ffff"
#define EXIT_TITLE_NO 0xa4201b
#define EXIT_TITLE_DONE 0xa420d8
#define EXIT_FIELD_BUILD 0x7827d0
#define EXIT_STAGE_BUILD 0x7de9e0

/* The System menu's sites, per menu. */
typedef struct {
    uint32_t chosen;   /* movzx esi, word [rax + rcx + 0x3600]: the chosen entry's action */
    uint32_t help;     /* mov esi, 886; mov edi, 0x10c0; add esi, [rbx + 0x788]; add rdi, [r14 + x];
                        * call FUN_00280db0 (28 bytes) */
    uint32_t ask, ask_call; /* mov esi, 732 ... call FUN_00280db0: the Yes/No dialog's question */
    uint32_t title;    /* mov rax, [rip -> 0x21d7500]; mov byte [rax + 0x142], 1 */
} ExitSystemMenu;
static const ExitSystemMenu exit_menus[] = {
    {0x77f44f, 0x77ec06, 0x7806bc, 0x7806c4, 0x7806ed}, /* world map */
    {0x7dccd4, 0x7dc30d, 0x7dc905, 0x7dc90a, 0x7dceb4}, /* battle map */
};
#define EXIT_CHOSEN_BYTES "0fb7b40800360000"
#define EXIT_HELP_BYTES "be76030000bfc010000003b388070000"
#define EXIT_ASK_BYTES "bedc020000"
#define EXIT_TITLE_FLAG_BYTES "c6804201000001"
#define EXIT_TITLE_FLAG 0x21d7500

typedef void *(ABI *ExitNew)(uint64_t size);
typedef void (ABI *ExitObject)(void *object);
typedef void (ABI *ExitAdd)(void *list, void *item);
typedef void (ABI *ExitFade)(void *object, int32_t frames, const void *color);
typedef void (ABI *ExitString)(void *text, const char *string);
typedef void (ABI *ExitSound)(void *sounds, int32_t id, int32_t flags);

static struct {
    unsigned char *image;
    ExitNew game_new;
    ExitObject button_init, button_reset, text_stop, text_restart, text_show;
    ExitAdd list_add;
    ExitFade fade;
    ExitString set_string;
    ExitSound sound;
    ExitObject title_build, title_free, field_build, stage_build; /* the game's, via trampolines */
    void *title, *button; /* the title object the button belongs to */
    uint8_t chosen;       /* the System menu's last chosen entry was the port's */
} exit_game;

static unsigned char *exit_at(uint32_t v0104) { return exit_game.image + unicorn_address(v0104); }
static int32_t exit_i32(const unsigned char *p) { int32_t v; memcpy(&v, p, 4); return v; }
static void exit_put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }

/* Title: the port's button after the game's, as FUN_00a455b0 adds New Game. */
static ABI void exit_title_build(void *title) {
    exit_game.title_build(title);
    unsigned char *list = (unsigned char *)title + EXIT_TITLE_LIST;
    int fresh = 0;
    if (exit_game.title != title || !exit_game.button) {
        unsigned char *b = exit_game.game_new(EXIT_BUTTON_SIZE);
        if (!b) return;
        exit_game.button_init(b);
        exit_game.title = title, exit_game.button = b, fresh = 1;
    }
    unsigned char *b = exit_game.button, *text = b + 0x340;
    exit_game.button_reset(b);
    exit_game.list_add(list, b);
    exit_game.fade(b + 0x1f0, 0, exit_at(EXIT_COLOR_ON));
    b[0x41a] |= 1;
    exit_put32(b + 0x494, 0xff050a19u);
    if (exit_i32(b + 0x528) >= 0) exit_game.text_stop(text);
    if (exit_i32(b + 0x540) != 2) {
        const int32_t keep = exit_i32(b + 0x4b4);
        exit_put32(b + 0x540, 2);
        exit_game.text_restart(text);
        exit_put32(b + 0x4b4, (uint32_t)keep);
        exit_game.text_show(text);
    }
    if (fresh) exit_game.set_string(text, exit_label);
    const int32_t count = exit_i32(list + EXIT_LIST_ITEMS);
    exit_put32(list + 0x370, (uint32_t)count);
    exit_put32(list + 0x374, (uint32_t)count);
}

/* Title: the object goes away; the button (memory of the game's heap) is left to it. */
static ABI void exit_title_free(void *title) {
    if (exit_game.title == title) exit_game.title = NULL, exit_game.button = NULL;
    exit_game.title_free(title);
}

/* Title: Cross on the port's button. sounds: the game's sound object (rdi at the stub). */
static ABI void exit_title_chosen(void *sounds) {
    exit_game.sound(sounds, 0x3f3, 0);
    vwgpu_quit();
}

/* System menu: the port's entry after Load, as the builders add theirs. */
static void exit_system_entry(unsigned char *list) {
    if (!list) return;
    const int32_t count = exit_i32(list + EXIT_LIST_COUNT);
    if (count <= 0 || count >= EXIT_LIST_MAX) return;
    for (int32_t i = 0; i < count; ++i)
        if ((uint32_t)exit_i32(list + (uint64_t)i * EXIT_ENTRY + EXIT_ENTRY_ID) == EXIT_MARK) return;
    unsigned char *e = list + (uint64_t)count * EXIT_ENTRY;
    exit_put32(e + EXIT_ENTRY_ID, EXIT_MARK);
    exit_game.set_string(e + 0x33d0, exit_label);
    exit_put32(e + EXIT_ENTRY_ACTION, EXIT_RETURN_TO_TITLE);
    exit_game.fade(e + 0x3280, 0, exit_at(EXIT_COLOR_ON));
    e[0x3611] = 0;
    e[0x1440] &= 0xc3;
    exit_put32(list + EXIT_LIST_COUNT, (uint32_t)(count + 1));
    exit_put32(list + 0x7a0, (uint32_t)(count + 1));
    exit_put32(list + 0x7a4, (uint32_t)(count + 1));
}

static ABI void exit_field_build(void *menu) {
    exit_game.field_build(menu);
    unsigned char *list;
    memcpy(&list, (unsigned char *)menu + 0x598, sizeof(list));
    exit_system_entry(list);
}

static ABI void exit_stage_build(void *menu) {
    exit_game.stage_build(menu);
    unsigned char *list;
    memcpy(&list, (unsigned char *)menu + 0x4d8, sizeof(list));
    exit_system_entry(list);
}

static ABI void exit_system_quit(void) { vwgpu_quit(); }

/* Code writers for the stubs (r11 and rax are free at every site). */
static unsigned char *exit_r11(unsigned char *p, uint64_t value) {
    *p++ = 0x49; *p++ = 0xbb;
    memcpy(p, &value, 8);
    return p + 8;
}
static unsigned char *exit_call(unsigned char *p, const void *target) { /* movabs rax; call rax */
    p = native_movabs(p, 0, (uintptr_t)target);
    *p++ = 0xff; *p++ = 0xd0;
    return p;
}
static unsigned char *exit_back(unsigned char *p, uint32_t v0104) { return native_jump(p, exit_at(v0104)); }
static unsigned char *exit_set_string(unsigned char *p, const char *string) { /* rdi = the text object */
    p = native_movabs(p, 6, (uintptr_t)string);  /* movabs rsi */
    return exit_call(p, exit_at(EXIT_TEXT_STRING));
}
/* cmp byte [r11], 0 with r11 = &exit_game.chosen */
static unsigned char *exit_test_chosen(unsigned char *p) {
    p = exit_r11(p, (uintptr_t)&exit_game.chosen);
    *p++ = 0x41; *p++ = 0x80; *p++ = 0x3b; *p++ = 0x00;
    return p;
}
static void exit_patch_jump(uint32_t at, unsigned length, const void *stub) {
    unsigned char *site = exit_at(at);
    site[0] = 0xe9;
    native_rel32(exit_game.image, at + 1, stub);
    memset(site + 5, 0x90, length - 5);
}
/* call rel32 at `at` goes to v0104 */
static int exit_calls_to(const unsigned char *image, uint32_t at, uint32_t v0104) {
    at = unicorn_address(at);
    return image[at] == 0xe8 && (int64_t)at + 5 + exit_i32(image + at + 1) == (int64_t)unicorn_address(v0104);
}

/* Installs it. image: the relocated game, writable; code: a page within 2 GiB of it (made
 * executable by the caller). 0 when the game differs from v01.04. */
static int exit_game_install(unsigned char *image, unsigned char *code) {
    exit_game.image = image;
    int ok = native_reaches(image, code);
    for (size_t i = 0; ok && i < NATIVE_COUNT(exit_calls); ++i) ok = native_bytes(image, exit_calls[i].at, exit_calls[i].bytes);
    ok = ok && native_bytes(image, EXIT_TITLE_BUILD, NATIVE_PROLOGUE) && native_bytes(image, EXIT_TITLE_FREE, NATIVE_PROLOGUE) &&
         native_bytes(image, EXIT_FIELD_BUILD, NATIVE_PROLOGUE) && native_bytes(image, EXIT_STAGE_BUILD, NATIVE_PROLOGUE) &&
         native_bytes(image, EXIT_TITLE_COMPARE, EXIT_TITLE_COMPARE_BYTES);
    for (size_t i = 0; ok && i < NATIVE_COUNT(exit_menus); ++i) {
        const ExitSystemMenu *m = &exit_menus[i];
        ok = native_bytes(image, m->chosen, EXIT_CHOSEN_BYTES) && native_bytes(image, m->help, EXIT_HELP_BYTES) &&
             native_bytes(image, m->help + 16, "4903be") && exit_calls_to(image, m->help + 23, EXIT_TEXT_ID) &&
             native_bytes(image, m->ask, EXIT_ASK_BYTES) && exit_calls_to(image, m->ask_call, EXIT_TEXT_ID) &&
             native_bytes(image, m->title, "488b05") && native_bytes(image, m->title + 7, EXIT_TITLE_FLAG_BYTES) &&
             (int64_t)unicorn_address(m->title) + 7 + exit_i32(image + unicorn_address(m->title) + 3) ==
                 (int64_t)unicorn_address(EXIT_TITLE_FLAG);
    }
    if (!ok) return 0;

    exit_game.game_new = (ExitNew)(uintptr_t)exit_at(EXIT_NEW);
    exit_game.button_init = (ExitObject)(uintptr_t)exit_at(EXIT_BUTTON_INIT);
    exit_game.button_reset = (ExitObject)(uintptr_t)exit_at(EXIT_BUTTON_RESET);
    exit_game.list_add = (ExitAdd)(uintptr_t)exit_at(EXIT_LIST_ADD);
    exit_game.fade = (ExitFade)(uintptr_t)exit_at(EXIT_FADE);
    exit_game.text_stop = (ExitObject)(uintptr_t)exit_at(EXIT_TEXT_STOP);
    exit_game.text_restart = (ExitObject)(uintptr_t)exit_at(EXIT_TEXT_RESTART);
    exit_game.text_show = (ExitObject)(uintptr_t)exit_at(EXIT_TEXT_SHOW);
    exit_game.set_string = (ExitString)(uintptr_t)exit_at(EXIT_TEXT_STRING);
    exit_game.sound = (ExitSound)(uintptr_t)exit_at(EXIT_SOUND);

    unsigned char *p = code;
    /* Title: rcx = the chosen button, rdi = the sound object. */
    unsigned char *title = p;
    p = native_movabs(p, 0, (uintptr_t)&exit_game.button);
    *p++ = 0x48; *p++ = 0x3b; *p++ = 0x08;                 /* cmp rcx, [rax] */
    *p++ = 0x75; *p++ = 12 + 14;                           /* jne no */
    p = exit_call(p, exit_title_chosen);
    p = exit_back(p, EXIT_TITLE_DONE);
    p = exit_back(p, EXIT_TITLE_NO);                       /* no: */

    unsigned char *stubs[NATIVE_COUNT(exit_menus)][4];
    for (size_t i = 0; i < NATIVE_COUNT(exit_menus); ++i) {
        const ExitSystemMenu *m = &exit_menus[i];
        /* chosen = the entry's id is the port's; then the game's read of its action */
        stubs[i][0] = p;
        p = exit_r11(p, (uintptr_t)&exit_game.chosen);
        *p++ = 0x41; *p++ = 0xc6; *p++ = 0x03; *p++ = 0x00;  /* mov byte [r11], 0 */
        *p++ = 0x81; *p++ = 0xbc; *p++ = 0x08;                /* cmp dword [rax + rcx + 0x3604], EXIT_MARK */
        exit_put32(p, EXIT_ENTRY_ID); p += 4;
        exit_put32(p, EXIT_MARK); p += 4;
        *p++ = 0x75; *p++ = 4;                                 /* jne +4 */
        *p++ = 0x41; *p++ = 0xc6; *p++ = 0x03; *p++ = 0x01;  /* mov byte [r11], 1 */
        memcpy(p, exit_at(m->chosen), 8); p += 8;
        p = exit_back(p, m->chosen + 8);
        /* help: the game's id 886 + cursor, or the port's line on its entry (rbx = the list) */
        stubs[i][1] = p;
        memcpy(p, exit_at(m->help), 23); p += 23;             /* esi = id, rdi = the help text */
        *p++ = 0x48; *p++ = 0x63; *p++ = 0x83;                /* movsxd rax, [rbx + 0x788] */
        exit_put32(p, 0x788); p += 4;
        *p++ = 0x48; *p++ = 0x69; *p++ = 0xc0;                /* imul rax, rax, 0x2510 */
        exit_put32(p, EXIT_ENTRY); p += 4;
        *p++ = 0x81; *p++ = 0xbc; *p++ = 0x03;                /* cmp dword [rbx + rax + 0x3604], EXIT_MARK */
        exit_put32(p, EXIT_ENTRY_ID); p += 4;
        exit_put32(p, EXIT_MARK); p += 4;
        *p++ = 0x74; *p++ = 12 + 14;                           /* je port */
        p = exit_call(p, exit_at(EXIT_TEXT_ID));
        p = exit_back(p, m->help + 28);
        p = exit_set_string(p, exit_help);                     /* port: */
        p = exit_back(p, m->help + 28);
        /* the question: the game's 732, or the port's */
        stubs[i][2] = p;
        const unsigned between = m->ask_call - (m->ask + 5);
        memcpy(p, exit_at(m->ask), 5 + between); p += 5 + between;
        p = exit_test_chosen(p);
        *p++ = 0x75; *p++ = 12 + 14;                           /* jne port */
        p = exit_call(p, exit_at(EXIT_TEXT_ID));
        p = exit_back(p, m->ask_call + 5);
        p = exit_set_string(p, exit_question);                 /* port: */
        p = exit_back(p, m->ask_call + 5);
        /* after the fade: quit, or go to the title */
        stubs[i][3] = p;
        p = exit_test_chosen(p);
        *p++ = 0x75; *p++ = 10 + 3 + 7 + 14;                   /* jne quit */
        p = native_movabs(p, 0, (uintptr_t)exit_at(EXIT_TITLE_FLAG));
        *p++ = 0x48; *p++ = 0x8b; *p++ = 0x00;                /* mov rax, [rax] */
        memcpy(p, exit_at(m->title + 7), 7); p += 7;           /* mov byte [rax + 0x142], 1 */
        p = exit_back(p, m->title + 14);
        p = exit_call(p, exit_system_quit);                    /* quit: */
        p = exit_back(p, m->title + 14);
    }

    exit_game.title_build = (ExitObject)(uintptr_t)native_wrap(image, EXIT_TITLE_BUILD, &p, exit_title_build);
    exit_game.title_free = (ExitObject)(uintptr_t)native_wrap(image, EXIT_TITLE_FREE, &p, exit_title_free);
    exit_game.field_build = (ExitObject)(uintptr_t)native_wrap(image, EXIT_FIELD_BUILD, &p, exit_field_build);
    exit_game.stage_build = (ExitObject)(uintptr_t)native_wrap(image, EXIT_STAGE_BUILD, &p, exit_stage_build);
    native_rel32(image, EXIT_TITLE_COMPARE + 12, title);
    for (size_t i = 0; i < NATIVE_COUNT(exit_menus); ++i) {
        const ExitSystemMenu *m = &exit_menus[i];
        exit_patch_jump(m->chosen, 8, stubs[i][0]);
        exit_patch_jump(m->help, 28, stubs[i][1]);
        exit_patch_jump(m->ask, m->ask_call + 5 - m->ask, stubs[i][2]);
        exit_patch_jump(m->title, 14, stubs[i][3]);
    }
    return 1;
}

#endif
