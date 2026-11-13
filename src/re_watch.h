/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Research aid, Linux, off unless VW_RE_WATCH is set: watch one of the game's objects change
 * while you play, to find which field holds what (the battle cursor's position, the camera...).
 *
 *   VW_RE_WATCH=<vtable>:<bytes>   e.g. 0x15012d0:0xd90 (UcStCursor in v01.04)
 *
 * <vtable> is the class's vtable as an eboot address (Ghidra's), <bytes> how much of the object
 * to watch. Every 250 ms a thread finds the live objects whose first word is that vtable
 * (scanning the process's writable memory, again when the object goes away), and prints each
 * 4-byte field of the first one that changed since the last look, as hex and as float:
 *   RE watch 0x7f12345000+0x0c4: 00000000 -> 43a00000 (0 -> 320)
 * Fields that change on every look (timers) are muted after 8 changes. Process memory only:
 * the game is not written to. */
#ifdef __linux__
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

static uint64_t re_watch_vtable;
/* Copies memory that may be unmapped meanwhile (the game frees and maps while we look):
 * a failed copy returns 0 instead of a fault. */
static int re_watch_copy(void *to, const void *from, size_t bytes) {
    const struct iovec local = {to, bytes}, remote = {(void *)from, bytes};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)bytes;
}
static uintptr_t re_watch_image;   /* skipped: the eboot's own tables hold vtable addresses */
static size_t re_watch_bytes;

/* The objects whose first 8 bytes are the vtable, in the writable mappings. */
static size_t re_watch_find(uint64_t **found, size_t max) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512];
    size_t n = 0;
    while (n < max && fgets(line, sizeof(line), maps)) {
        unsigned long start, end;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (strstr(line, "[stack") || strstr(line, "[vvar")) continue;
        /* Only pages in memory: reading a never-touched page of the shared guest memory would
         * allocate it (gigabytes). */
        const size_t pages = (end - start) / 4096;
        unsigned char *resident = malloc(pages ? pages : 1);
        if (!resident || mincore((void *)start, end - start, resident) != 0) { free(resident); continue; }
        for (size_t page = 0; page < pages && n < max; ++page) {
            if (!(resident[page] & 1)) continue;
            uint64_t words[512];
            uint64_t *at = (uint64_t *)(start + page * 4096);
            if (!re_watch_copy(words, at, sizeof(words))) continue;
            for (int i = 0; i < 512 && n < max; ++i)
                if (words[i] == re_watch_vtable && (uintptr_t)(at + i) - re_watch_image >= 0x2000000 && at + i != &re_watch_vtable)
                    found[n++] = at + i;
        }
        free(resident);
    }
    fclose(maps);
    return n;
}

static void *re_watch_thread(void *unused) {
    (void)unused;
    const size_t words = re_watch_bytes / 4;
    uint32_t *last = calloc(words, 4), *now = calloc(words, 4);
    uint8_t *changes = calloc(words, 1);
    uint64_t *object = NULL;
    if (!last || !now || !changes) return NULL;
    const struct timespec pause = {0, 250 * 1000 * 1000};
    for (;;) {
        nanosleep(&pause, NULL);
        uint64_t head = 0;
        if (!object || !re_watch_copy(&head, object, 8) || head != re_watch_vtable) {
            uint64_t *found[8];
            const size_t n = re_watch_find(found, 8);
            object = n ? found[0] : NULL;
            if (!object) continue;
            printf("RE watch: %zu object(s), watching %p\n", n, (void *)object);
            for (size_t i = 1; i < n; ++i) printf("RE watch:   also %p\n", (void *)found[i]);
            if (!re_watch_copy(last, object, words * 4)) object = NULL;
            memset(changes, 0, words);
            continue;
        }
        if (!re_watch_copy(now, object, words * 4)) { object = NULL; continue; }
        for (size_t i = 0; i < words; ++i) {
            const uint32_t v = now[i];
            if (v == last[i]) continue;
            if (changes[i] < 8) {
                float before, after;
                memcpy(&before, &last[i], 4);
                memcpy(&after, &v, 4);
                printf("RE watch %p+0x%03zx: %08x -> %08x (%g -> %g)%s\n", (void *)object, i * 4, last[i], v,
                       before, after, ++changes[i] == 8 ? " [muted]" : "");
            }
            last[i] = v;
        }
        fflush(stdout);
    }
    return NULL;
}

/* image: where the eboot is loaded (its address 0). */
static void re_watch_start(const unsigned char *image) {
    const char *spec = getenv("VW_RE_WATCH");
    if (!spec || !*spec) return;
    char *end;
    const uint64_t vtable = strtoull(spec, &end, 0);
    const size_t bytes = *end == ':' ? strtoull(end + 1, NULL, 0) : 0x100;
    if (!vtable || bytes < 8 || bytes > 0x10000) { printf("RE watch: VW_RE_WATCH=<vtable>:<bytes>\n"); return; }
    re_watch_image = (uintptr_t)image;
    re_watch_vtable = (uint64_t)(uintptr_t)image + vtable;
    re_watch_bytes = bytes & ~(size_t)3;
    pthread_t thread;
    if (pthread_create(&thread, NULL, re_watch_thread, NULL) == 0) pthread_detach(thread);
    printf("RE watch: objects with vtable %#" PRIx64 " (eboot %#" PRIx64 "), %zu bytes\n", re_watch_vtable, vtable, re_watch_bytes);
}
#else
static void re_watch_start(const unsigned char *image) { (void)image; }
#endif
