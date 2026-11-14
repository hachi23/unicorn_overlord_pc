/* VW_PAD_MAP / VW_KEY_MAP parsing (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

int vwgpu_overlay_captures_input(void) { return 0; }
int vwgpu_stick_dead_zone(void) { return 15; }
int vwgpu_setting_text(const char *key, char *out, uint64_t size) { (void)key; (void)out; (void)size; return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    ButtonMap keys=key_map;
    assert(apply_button_map(&keys,NULL,true)==0 && apply_button_map(&keys,"",true)==0);
    assert(keys.count==key_map.count);

    /* Swap: Q presses Cross, Space presses Triangle; Space no longer presses Cross. */
    assert(apply_button_map(&keys," cross = Q , triangle=Space",true)==2);
    assert(pressed_by(&keys,SDL_SCANCODE_Q)==BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    assert(pressed_by(&keys,SDL_SCANCODE_E)==BTN_SQUARE);

    /* Names with spaces, any case; the last entry for a button wins. */
    ButtonMap again=key_map;
    assert(apply_button_map(&again,"CIRCLE=left shift,circle=Left Ctrl",true)==2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    assert(pressed_by(&again,SDL_SCANCODE_LSHIFT)==0);

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap bad=key_map;
    assert(apply_button_map(&bad,"bogus=Q,cross=NotAKey,circle,=Q,square=,,",true)==0);
    assert(bad.count==key_map.count && pressed_by(&bad,SDL_SCANCODE_SPACE)==BTN_CROSS);

    /* Gamepad: Xbox layout swap; both touchpad sources go when touchpad is remapped. */
    ButtonMap pad=pad_map;
    assert(apply_button_map(&pad,"cross=b,circle=a,touchpad=misc1",false)==3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==0);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==BTN_TOUCHPAD);
    assert(apply_button_map(&pad,"cross=Space",false)==0); /* a key name is no gamepad button */

    /* Several sources with '|', "none" unbinds; one bad name skips the whole entry. */
    ButtonMap multi=key_map;
    assert(apply_button_map(&multi,"cross=Q|Space,circle=none,square=E|NotAKey",true)==2);
    assert(pressed_by(&multi,SDL_SCANCODE_Q)==(BTN_CROSS|BTN_TRIANGLE));
    assert(pressed_by(&multi,SDL_SCANCODE_SPACE)==BTN_CROSS);
    assert(pressed_by(&multi,SDL_SCANCODE_LSHIFT)==0 && pressed_by(&multi,SDL_SCANCODE_E)==BTN_SQUARE);

    /* The Controls tab: bind, describe, save (spec), reset; the spec loads back to the same map. */
    char text[256];
    runtime_pad_bind(1,0,SDL_SCANCODE_X);            /* cross: X only */
    runtime_pad_bind(1,1,-1);                        /* circle: nothing */
    runtime_pad_describe(1,0,text,sizeof(text));
    assert(!strcmp(text,"X"));
    runtime_pad_spec(1,text,sizeof(text));
    assert(!strcmp(text,"cross=X,circle=none"));
    ButtonMap loaded=key_defaults;
    assert(apply_button_map(&loaded,text,true)==2);
    assert(pressed_by(&loaded,SDL_SCANCODE_X)==BTN_CROSS && pressed_by(&loaded,SDL_SCANCODE_LSHIFT)==0);
    runtime_pad_reset(1);
    runtime_pad_spec(1,text,sizeof(text));
    assert(!text[0]);
    runtime_pad_describe(0,0,text,sizeof(text));
    assert(!strcmp(text,"A / Cross"));
    puts("PASS: pad and key maps");
}
