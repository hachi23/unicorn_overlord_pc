/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Default keyboard layout (VW_KEY_MAP changes the buttons, see apply_button_map):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad,
 *   IJKL d-pad (I up, K down, J left, L right). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/vwgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
/* A stick through a round dead zone of zone percent (vw.ini stick_dead_zone): many sticks rest
 * off centre (issue #28), and the game took that for a held direction. Past the zone the travel
 * is rescaled, so the stick still moves smoothly from the centre to the edge. */
static void stick(int16_t raw_x, int16_t raw_y, int zone, uint8_t *out_x, uint8_t *out_y) {
    float x=raw_x<-32767 ? -1.0f : raw_x/32767.0f, y=raw_y<-32767 ? -1.0f : raw_y/32767.0f;
    const float length=SDL_sqrtf(x*x+y*y), dead=(zone<0 ? 0 : zone>90 ? 90 : zone)/100.0f;
    if (length<=dead) { *out_x=*out_y=128; return; }
    const float scale=((length>1.0f ? 1.0f : length)-dead)/(1.0f-dead)/length;
    x*=scale; y*=scale;
    const int ix=(int)SDL_floorf(128.0f+x*127.5f), iy=(int)SDL_floorf(128.0f+y*127.5f);
    *out_x=(uint8_t)(ix<0 ? 0 : ix>255 ? 255 : ix);
    *out_y=(uint8_t)(iy<0 ? 0 : iy>255 ? 255 : iy);
}
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with VW_PAD_MAP (gamepad) and VW_KEY_MAP (keyboard), e.g.
 * VW_KEY_MAP="cross=Q,square=Left Shift" or VW_PAD_MAP="cross=b,circle=a": comma-separated
 * <ps4 button>=<SDL key or gamepad button name>. A mapped PS4 button loses its default sources.
 * Sticks, triggers and the overlay shortcuts are not remapped. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static ButtonMap key_map={{
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_LSHIFT,BTN_CIRCLE}, {SDL_SCANCODE_E,BTN_SQUARE},
    {SDL_SCANCODE_Q,BTN_TRIANGLE}, {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
    {SDL_SCANCODE_R,BTN_L2}, {SDL_SCANCODE_F,BTN_R2}, {SDL_SCANCODE_Z,BTN_L3}, {SDL_SCANCODE_C,BTN_R3},
    {SDL_SCANCODE_RETURN,BTN_OPTIONS},
    {SDL_SCANCODE_I,BTN_UP}, {SDL_SCANCODE_K,BTN_DOWN}, {SDL_SCANCODE_J,BTN_LEFT}, {SDL_SCANCODE_L,BTN_RIGHT},
},15};

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

/* Applies a VW_PAD_MAP/VW_KEY_MAP value; returns how many entries were used. Bad entries are
 * logged and skipped, so their PS4 button keeps its default. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        /* name: one source, several joined with '|', or "none" (the button is not pressed by
         * anything on this device). */
        int sources[8], count=0;
        bool valid=ps && *name;
        const bool none=!SDL_strcasecmp(name,"none");
        if (valid && !none) {
            char names[256];
            snprintf(names,sizeof(names),"%s",name);
            for (char *part=names, *bar; part && valid; part=bar) {
                if ((bar=strchr(part,'|'))) *bar++=0;
                const char *one=trim(part);
                const int source=keyboard ? (int)SDL_GetScancodeFromName(one) : (int)SDL_GetGamepadButtonFromString(one);
                valid=source>(keyboard ? (int)SDL_SCANCODE_UNKNOWN : (int)SDL_GAMEPAD_BUTTON_INVALID);
                if (valid && count<8) sources[count++]=source;
            }
        }
        if (!valid) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        size_t kept=0;
        for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
        map->count=kept;
        for (int s=0;s<count;++s)
            if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){sources[s],ps};
        ++used;
    }
    return used;
}

/* The defaults above, kept for "Reset" and to tell remapped buttons apart (runtime_pad_spec). */
static ButtonMap pad_defaults, key_defaults;

/* Order: the defaults, then the in-game Controls tab (vw.ini pad_map/key_map), then the
 * environment. */
static void load_button_maps(void) {
    static bool loaded;
    if (loaded) return;
    loaded=true;
    pad_defaults=pad_map;
    key_defaults=key_map;
    char saved[1024];
    int pad=0, keys=0;
    if (vwgpu_setting_text("pad_map",saved,sizeof(saved))) pad+=apply_button_map(&pad_map,saved,false);
    if (vwgpu_setting_text("key_map",saved,sizeof(saved))) keys+=apply_button_map(&key_map,saved,true);
    pad+=apply_button_map(&pad_map,getenv("VW_PAD_MAP"),false);
    keys+=apply_button_map(&key_map,getenv("VW_KEY_MAP"),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard buttons remapped\n",pad,keys);
}

/* The in-game Controls tab (gpu/shim/vw_overlay.cpp): PS4 button i of ps_names, its host
 * sources and a new one, applied at once. */
int runtime_pad_button_count(void) { return (int)(sizeof(ps_names)/sizeof(*ps_names)); }
const char *runtime_pad_button_name(int i) {
    return i>=0 && i<runtime_pad_button_count() ? ps_names[i].name : "";
}
static const char *source_name(int source, bool keyboard) {
    const char *name=keyboard ? SDL_GetScancodeName((SDL_Scancode)source)
                              : SDL_GetGamepadStringForButton((SDL_GamepadButton)source);
    return name && *name ? name : "?";
}
/* A gamepad button as the menu shows it: Xbox and PlayStation names. */
static const char *pad_label(int source) {
    switch (source) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "A / Cross";
    case SDL_GAMEPAD_BUTTON_EAST: return "B / Circle";
    case SDL_GAMEPAD_BUTTON_WEST: return "X / Square";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Y / Triangle";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB / L1";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB / R1";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LS / L3";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RS / R3";
    case SDL_GAMEPAD_BUTTON_START: return "Start / Options";
    case SDL_GAMEPAD_BUTTON_BACK: return "Back / Share";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
    default: return source_name(source,false);
    }
}
/* The sources of PS4 button i, joined with ", " ("" when none). */
void runtime_pad_describe(int keyboard, int i, char *out, size_t size) {
    if (!size) return;
    out[0]=0;
    if (i<0 || i>=runtime_pad_button_count()) return;
    pthread_mutex_lock(&lock);
    load_button_maps();
    const ButtonMap *map=keyboard ? &key_map : &pad_map;
    size_t used=0;
    for (size_t e=0;e<map->count;++e) {
        if (map->entries[e].ps!=ps_names[i].ps) continue;
        const int source=map->entries[e].source;
        const int n=snprintf(out+used,size-used,"%s%s",used ? ", " : "",keyboard ? source_name(source,true) : pad_label(source));
        if (n<0 || (size_t)n>=size-used) break;
        used+=(size_t)n;
    }
    pthread_mutex_unlock(&lock);
}
/* PS4 button i is pressed by `source` only (an SDL scancode or gamepad button); -1: by nothing. */
void runtime_pad_bind(int keyboard, int i, int source) {
    if (i<0 || i>=runtime_pad_button_count()) return;
    pthread_mutex_lock(&lock);
    load_button_maps();
    ButtonMap *map=keyboard ? &key_map : &pad_map;
    size_t kept=0;
    for (size_t e=0;e<map->count;++e) if (map->entries[e].ps!=ps_names[i].ps) map->entries[kept++]=map->entries[e];
    map->count=kept;
    if (source>=0 && map->count<sizeof(map->entries)/sizeof(*map->entries))
        map->entries[map->count++]=(ButtonSource){source,ps_names[i].ps};
    pthread_mutex_unlock(&lock);
}
void runtime_pad_reset(int keyboard) {
    pthread_mutex_lock(&lock);
    load_button_maps();
    if (keyboard) key_map=key_defaults; else pad_map=pad_defaults;
    pthread_mutex_unlock(&lock);
}
/* The buttons that differ from the defaults, in apply_button_map's format (vw.ini):
 * "cross=Q,circle=none"; several sources of one button are joined with '|'. */
void runtime_pad_spec(int keyboard, char *out, size_t size) {
    if (!size) return;
    out[0]=0;
    pthread_mutex_lock(&lock);
    load_button_maps();
    const ButtonMap *map=keyboard ? &key_map : &pad_map, *base=keyboard ? &key_defaults : &pad_defaults;
    size_t used=0;
    bool full=false;
    for (int i=0;i<runtime_pad_button_count() && !full;++i) {
        const uint32_t ps=ps_names[i].ps;
        size_t now=0, before=0, matching=0;
        for (size_t e=0;e<map->count;++e) {
            if (map->entries[e].ps!=ps) continue;
            ++now;
            for (size_t b=0;b<base->count;++b)
                if (base->entries[b].ps==ps && base->entries[b].source==map->entries[e].source) { ++matching; break; }
        }
        for (size_t b=0;b<base->count;++b) before+=base->entries[b].ps==ps;
        if (now==before && matching==now) continue;
        char value[256]="none";
        size_t length=0;
        for (size_t e=0;e<map->count;++e) {
            if (map->entries[e].ps!=ps) continue;
            const int n=snprintf(value+length,sizeof(value)-length,"%s%s",length ? "|" : "",source_name(map->entries[e].source,keyboard));
            if (n>0 && (size_t)n<sizeof(value)-length) length+=(size_t)n;
        }
        const int n=snprintf(out+used,size-used,"%s%s=%s",used ? "," : "",ps_names[i].name,value);
        if (n<0 || (size_t)n>=size-used) { out[used]=0; full=true; }
        else used+=(size_t)n;
    }
    pthread_mutex_unlock(&lock);
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
/* Mouse buttons over the game window: left Cross, right Circle. The click that focuses the window
 * is not passed on (held buttons wait for release after the window gains focus). */
static uint32_t mouse_buttons(void) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    static int wait_release;
    SDL_Window *w=SDL_GetMouseFocus();
    const SDL_MouseButtonFlags held=SDL_GetMouseState(NULL,NULL);
    if (!w || !(SDL_GetWindowFlags(w)&SDL_WINDOW_INPUT_FOCUS)) { wait_release=1; return 0; }
    if (wait_release) { if (!held) wait_release=0; return 0; }
    uint32_t b=0;
    if (held&SDL_BUTTON_LMASK) b|=BTN_CROSS;
    if (held&SDL_BUTTON_RMASK) b|=BTN_CIRCLE;
    return b;
}
static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    if (vwgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    load_button_maps();
    d->buttons|=mouse_buttons();
    if (g) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        const int zone=vwgpu_stick_dead_zone();
        stick(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX),SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY),zone,&d->left_x,&d->left_y);
        stick(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX),SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY),zone,&d->right_x,&d->right_y);
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (!k) return;
    for (size_t i=0;i<key_map.count;++i) if (k[key_map.entries[i].source]) d->buttons|=key_map.entries[i].ps;
    if (k[SDL_SCANCODE_TAB]) touch_click(d,0);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0); /* a remapped touchpad key */
    if (k[SDL_SCANCODE_BACKSPACE]) touch_click(d,1);
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,k[SDL_SCANCODE_A],k[SDL_SCANCODE_D]);
    key_axis(&d->left_y,k[SDL_SCANCODE_W],k[SDL_SCANCODE_S]);
    key_axis(&d->right_x,k[SDL_SCANCODE_LEFT],k[SDL_SCANCODE_RIGHT]);
    key_axis(&d->right_y,k[SDL_SCANCODE_UP],k[SDL_SCANCODE_DOWN]);
}

/* VW_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks; mouse=dx,dy moves the mouse by that many pixels once. An empty file releases
 * everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static float injected_mouse[2];   /* mouse=dx,dy, pixels, taken once by runtime_mouse_take */
static int nudge_x, nudge_y, nudge_reads;   /* runtime_mouse_nudge: left stick for a few reads */
static int replay_armed;      /* 1 while a VW_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("VW_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* VW_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
        float mx, my;
        if (sscanf(token,"mouse=%f,%f",&mx,&my)==2) { injected_mouse[0]+=mx; injected_mouse[1]+=my; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* VW_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; VW_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in VW_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("VW_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("VW_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* Button icons (src/unicorn_button_icons.h): the setting, else the connected controller's
 * family. Keyboard only: Xbox letters, the most common prompts on PC. Checked twice a second. */
static void (*button_icons_listener)(int);
void runtime_pad_on_button_icons(void (*listener)(int style)) { button_icons_listener=listener; }
static int button_icons_style(SDL_Gamepad *g) {
    char text[32]="";
    const char *env=getenv("VW_BUTTON_ICONS");
    if (env && *env) snprintf(text,sizeof(text),"%s",env);
    else vwgpu_setting_text("button_icons",text,sizeof(text));
    if (!strcmp(text,"playstation")) return 0;
    if (!strcmp(text,"xbox")) return 1;
    if (!strcmp(text,"nintendo")) return 2;
    if (!g) return 1;
    switch (SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return 0;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return 2;
    default: return 1;
    }
}
static void update_button_icons(void) {
    static int current=-1;
    static uint64_t last;
    const uint64_t now=now_us();
    if (!button_icons_listener || (current>=0 && now-last<500000)) return;
    last=now;
    const int style=button_icons_style(current_gamepad());
    if (style==current) return;
    static const char *const names[]={"PlayStation","Xbox","Nintendo"};
    printf("Runtime: button icons: %s\n",names[style]);
    current=style;
    button_icons_listener(style);
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    update_button_icons();
    if (vwgpu_overlay_captures_input()) { hold_after_capture=1; return; }
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    if (nudge_reads>0) {
        --nudge_reads;
        d->left_x=(uint8_t)(128+nudge_x); d->left_y=(uint8_t)(128+nudge_y);
    }
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }

/* Mouse motion over the window since the last call, in window points, and the window's size
 * (the battle map cursor, src/unicorn_mouse.h); with VW_PAD_FILE, its mouse=dx,dy tokens too. */
void runtime_mouse_take(float *dx, float *dy, int *window_w, int *window_h) {
    float x=0, y=0;
    int w_px=0, h=0;
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        SDL_GetRelativeMouseState(&x,&y);
        SDL_Window *w=SDL_GetMouseFocus();
        if (!w || !(SDL_GetWindowFlags(w)&SDL_WINDOW_INPUT_FOCUS) || vwgpu_overlay_captures_input()) x=y=0;
        else SDL_GetWindowSize(w,&w_px,&h);
    }
    pthread_mutex_lock(&lock);
    read_inject();
    x+=injected_mouse[0]; y+=injected_mouse[1];
    injected_mouse[0]=injected_mouse[1]=0;
    pthread_mutex_unlock(&lock);
    if (w_px<=0 || h<=0) w_px=1381, h=776;   /* the window of scripted runs */
    *dx=x; *dy=y; *window_w=w_px; *window_h=h;
}

/* Tilts the left stick toward (dx, dy) for two pad reads: after a facility menu the game keeps
 * the map cursor on the facility until the stick moves it, which the mouse alone does not. */
void runtime_mouse_nudge(float dx, float dy) {
    const float length=SDL_sqrtf(dx*dx+dy*dy);
    if (length<=0) return;
    pthread_mutex_lock(&lock);
    nudge_x=(int)(dx/length*60.0f); nudge_y=(int)(dy/length*60.0f);
    nudge_reads=2;
    pthread_mutex_unlock(&lock);
}
