/* src/unicorn_exit_game.h on a made-up game image: a game that differs is left alone; on v01.04's
 * instructions every site jumps to the port's code, the System menus get one "Exit Game" entry
 * after Load, the title list gets the port's button after the game's, and the texts are served. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unicorn_exit_game.h"

int vwgpu_setting_rows(const VwgpuSettingRow **out) {
    if (out) *out = NULL;
    return 0;
}
float vwgpu_setting_value(int row) { (void)row; return 0; }
void vwgpu_setting_set(int row, float value) { (void)row, (void)value; }
static int quits;
void vwgpu_quit(void) { ++quits; }

enum { IMAGE = 0x2200000, PAGE = 4096 };

static void put(unsigned char *image, uint64_t at, const char *hex) {
    for (size_t i = 0; hex[2 * i]; ++i) {
        unsigned value;
        sscanf(hex + 2 * i, "%2x", &value);
        image[at + i] = (unsigned char)value;
    }
}
static void call(unsigned char *image, uint32_t at, uint32_t to) {
    const int32_t rel = (int32_t)(to - (at + 5));
    image[at] = 0xe8;
    memcpy(image + at + 1, &rel, 4);
}
static const unsigned char *jump_target(const unsigned char *image, uint64_t at) {
    int32_t rel;
    memcpy(&rel, image + at + 1, 4);
    return image + at + 5 + rel;
}

/* The game's functions, as the port calls them. */
static unsigned char heap[2][EXIT_BUTTON_SIZE];
static int news, inits, adds, texts, sounds;
static void *added[8];
static ABI void *game_new(uint64_t size) { assert(size == EXIT_BUTTON_SIZE && news < 2); return heap[news++]; }
static ABI void button_init(void *b) { ((unsigned char *)b)[0x540] = 0; memset((unsigned char *)b + 0x528, 0xff, 4); ++inits; }
static ABI void nothing(void *object) { (void)object; }
static ABI void list_add(void *list, void *item) {
    int32_t count;
    memcpy(&count, (unsigned char *)list + EXIT_LIST_ITEMS, 4);
    added[count++] = item;
    memcpy((unsigned char *)list + EXIT_LIST_ITEMS, &count, 4);
    ++adds;
}
static ABI void fade(void *object, int32_t frames, const void *c) { (void)object, (void)frames, (void)c; }
static const char *last_text;
static ABI void set_string(void *text, const char *string) { (void)text; last_text = string; ++texts; }
static ABI void sound(void *s, int32_t id, int32_t flags) { (void)s, (void)flags; assert(id == 0x3f3); ++sounds; }
static ABI void game_title_build(void *title) { /* the game's five buttons */
    for (int i = 0; i < 5; ++i) list_add((unsigned char *)title + EXIT_TITLE_LIST, (unsigned char *)title + 0x4560 + i * 0x570);
}
static ABI void game_build(void *menu) { (void)menu; }

int main(void) {
    (void)native_settings_install; /* the Options rows: tests/test_native_settings.c */
    unsigned char *image = calloc(1, IMAGE + 2 * PAGE), *code = image + IMAGE;
    assert(image);
    for (size_t i = 0; i < NATIVE_COUNT(exit_calls); ++i) put(image, exit_calls[i].at, exit_calls[i].bytes);
    put(image, EXIT_TITLE_BUILD, NATIVE_PROLOGUE);
    put(image, EXIT_TITLE_FREE, NATIVE_PROLOGUE);
    put(image, EXIT_FIELD_BUILD, NATIVE_PROLOGUE);
    put(image, EXIT_STAGE_BUILD, NATIVE_PROLOGUE);
    put(image, EXIT_TITLE_COMPARE, EXIT_TITLE_COMPARE_BYTES);
    for (size_t i = 0; i < NATIVE_COUNT(exit_menus); ++i) {
        const ExitSystemMenu *m = &exit_menus[i];
        put(image, m->chosen, EXIT_CHOSEN_BYTES);
        put(image, m->help, EXIT_HELP_BYTES);
        put(image, m->ask, EXIT_ASK_BYTES);
        put(image, m->help + 16, "4903be");
        call(image, m->help + 23, EXIT_TEXT_ID);
        call(image, m->ask_call, EXIT_TEXT_ID);
        put(image, m->title, "488b05");
        const int32_t disp = (int32_t)(EXIT_TITLE_FLAG - (m->title + 7));
        memcpy(image + m->title + 3, &disp, 4);
        put(image, m->title + 7, EXIT_TITLE_FLAG_BYTES);
    }

    /* One instruction differs: nothing changes. */
    unsigned char *copy = malloc(IMAGE);
    assert(copy);
    image[exit_menus[1].ask + 1] = 0xdd;
    memcpy(copy, image, IMAGE);
    assert(!exit_game_install(image, code));
    assert(!memcmp(copy, image, IMAGE));
    image[exit_menus[1].ask + 1] = 0xdc;

    assert(exit_game_install(image, code));
    for (size_t i = 0; i < NATIVE_COUNT(exit_menus); ++i) {
        const ExitSystemMenu *m = &exit_menus[i];
        const uint64_t sites[4] = {m->chosen, m->help, m->ask, m->title};
        for (int k = 0; k < 4; ++k) {
            assert(image[sites[k]] == 0xe9);
            const unsigned char *to = jump_target(image, sites[k]);
            assert(to >= code && to < code + PAGE);
        }
        assert(image[m->chosen + 5] == 0x90 && image[m->help + 27] == 0x90 && image[m->ask_call + 4] == 0x90 &&
               image[m->title + 13] == 0x90);
    }
    int32_t rel;
    memcpy(&rel, image + EXIT_TITLE_COMPARE + 12, 4);
    assert(image + EXIT_TITLE_COMPARE + 16 + rel == code); /* the title stub is the first */
    assert(image[EXIT_TITLE_BUILD] == 0xe9 && image[EXIT_TITLE_FREE] == 0xe9 && image[EXIT_FIELD_BUILD] == 0xe9 &&
           image[EXIT_STAGE_BUILD] == 0xe9);

    exit_game.game_new = game_new, exit_game.button_init = button_init, exit_game.button_reset = nothing;
    exit_game.text_stop = exit_game.text_restart = exit_game.text_show = nothing;
    exit_game.list_add = list_add, exit_game.fade = fade, exit_game.set_string = set_string, exit_game.sound = sound;
    exit_game.title_build = game_title_build, exit_game.title_free = game_build;
    exit_game.field_build = exit_game.stage_build = game_build;

    /* System menu: Options, Text Log, Return to Title, Load, then the port's entry, once. */
    unsigned char *list = calloc(1, EXIT_LIST_COUNT + 0x20);
    assert(list);
    for (int32_t i = 0; i < 4; ++i) {
        const uint32_t action = 0xc + (uint32_t)i;
        memcpy(list + (uint64_t)i * EXIT_ENTRY + EXIT_ENTRY_ACTION, &action, 4);
        memcpy(list + (uint64_t)i * EXIT_ENTRY + EXIT_ENTRY_ID, &action, 4);
    }
    const int32_t four = 4;
    memcpy(list + EXIT_LIST_COUNT, &four, 4);
    unsigned char field[0x600] = {0};
    memcpy(field + 0x598, &list, 8);
    exit_field_build(field);
    exit_field_build(field);
    assert(exit_i32(list + EXIT_LIST_COUNT) == 5 && exit_i32(list + 0x7a0) == 5 && exit_i32(list + 0x7a4) == 5);
    assert((uint32_t)exit_i32(list + 4 * EXIT_ENTRY + EXIT_ENTRY_ID) == EXIT_MARK);
    assert(exit_i32(list + 4 * EXIT_ENTRY + EXIT_ENTRY_ACTION) == EXIT_RETURN_TO_TITLE);
    assert(texts == 1 && !strcmp(last_text, "Exit Game"));
    unsigned char stage[0x600] = {0}; /* a menu without its list yet */
    exit_stage_build(stage);

    /* Title: the port's button last, one per title object, text set once. */
    unsigned char *title = calloc(1, 0xc6d0);
    assert(title);
    exit_title_build(title);
    assert(adds == 6 && added[5] == heap[0] && inits == 1 && texts == 2 && last_text == exit_label);
    assert(exit_i32(title + EXIT_TITLE_LIST + 0x370) == 6 && exit_i32(title + EXIT_TITLE_LIST + 0x374) == 6);
    memset(title + EXIT_TITLE_LIST + EXIT_LIST_ITEMS, 0, 4);
    exit_title_build(title); /* a submenu closed: the list is filled again */
    assert(news == 1 && added[5] == heap[0] && texts == 2);
    exit_title_free(title);
    memset(title + EXIT_TITLE_LIST + EXIT_LIST_ITEMS, 0, 4);
    exit_title_build(title); /* a new title object (here at the same address) */
    assert(news == 2 && added[5] == heap[1] && texts == 3);
    exit_title_chosen(NULL);
    assert(sounds == 1 && quits == 1);

    free(title);
    free(list);
    free(copy);
    free(image);
    printf("exit game: ok\n");
    return 0;
}
