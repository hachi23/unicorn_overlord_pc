/* src/unicorn_text_wrap.h: three-line story entries that fit two bubble lines get one break. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unicorn_text_wrap.h"

static void expect(const char *in, const char *out) {
    char *got = text_wrap_two_lines(in, TEXT_WRAP_WIDTH);
    if (!out) {
        assert(!got);
        return;
    }
    if (!got || strcmp(got, out)) {
        fprintf(stderr, "in:   %s\nwant: %s\ngot:  %s\n", in, out, got ? got : "(null)");
        abort();
    }
    free(got);
}

int main(void) {
    /* The line from #73: 118 letters, balanced into 61 + 56 would not fit; 56 + 61 neither:
     * the best break leaves both at most 56 only when one exists. */
    expect("To begin with, Your Highness, I suggest we make\nfor the fort we just liberated and\n"
           "increase the maximum size of a unit.",
           NULL);
    expect("I suggest we make\nfor the fort we just liberated and\nincrease the size of a unit.",
           "I suggest we make for the fort we just\nliberated and increase the size of a unit.");
    /* Two lines already, one line, or no space: as they are. */
    expect("Two lines\nonly.", NULL);
    expect("One line.", NULL);
    expect("まずは先ほど解放した砦で\n編成人数の上限を拡張しましょう\n三行目", NULL);
    /* Codes take no room; a break inside a colour span is fine. */
    assert(text_wrap_visible("#c(12)Lord#/c", 13) == 4);
    assert(text_wrap_visible("Press #(2) now", 14) == 14); /* an icon code with no letter: counted */
    assert(text_wrap_visible("#iFoo", 5) == 3);
    assert(text_wrap_visible("ãç", strlen("ãç")) == 2);
    expect("#c(12)Alain#/c and #c(13)Scarlett#/c\nride\nnorth today.",
           "#c(12)Alain#/c and #c(13)Scarlett#/c\nride north today.");
    /* A line break with spaces around it does not make a double space. */
    expect("Alpha beta \ngamma\n delta.", "Alpha beta\ngamma delta.");

    /* Through the lookup: only a sheet of the story's size, cached per entry, and the cache
     * follows the source text. */
    static const char *source = "We ride\nat dawn,\nmy friends.";
    unsigned char story[1] = {0}, small[1] = {0};
    const char *a = text_wrap_story(story, TEXT_WRAP_STORY_MIN, 7, source);
    assert(!strcmp(a, "We ride at dawn,\nmy friends."));
    assert(text_wrap_story(story, TEXT_WRAP_STORY_MIN, 7, source) == a);
    const char *plain = "No change.";
    assert(text_wrap_story(story, TEXT_WRAP_STORY_MIN, 8, plain) == plain);
    assert(text_wrap_story(small, 10, 1, source) == source);
    static const char *reloaded = "Another\ntext\nhere now.";
    assert(!strcmp(text_wrap_story(story, TEXT_WRAP_STORY_MIN, 7, reloaded), "Another text\nhere now."));
    /* The bubble: the game's scale up to two lines, 0.85 for three at 1.1. */
    assert(text_bubble_lines((const unsigned char *)"One", 0x100) == 1);
    assert(text_bubble_lines((const unsigned char *)"a\nb\nc", 0x100) == 3);
    assert(text_bubble_lines((const unsigned char *)"a\nb\nc", 3) == 2); /* within the buffer only */
    assert(text_bubble_scale(1.1f, 2) == 1.1f && text_bubble_scale(1.0f, 1) == 1.0f);
    const float three = text_bubble_scale(1.1f, 3), four = text_bubble_scale(1.1f, 4);
    assert(three > 0.845f && three < 0.855f && four > 0.63f && four < 0.64f);
    puts("text wrap: ok");
    return 0;
}
