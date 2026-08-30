#include "../core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_natural_compare(void) {
    assert(natural_compare("Page2.jpg", "Page10.jpg") < 0);
    assert(natural_compare("page02.jpg", "Page2.jpg") == 0);
    assert(natural_compare("Vol9.cbz", "Vol10.cbz") < 0);
    assert(natural_compare("Chapter A.cbz", "chapter b.cbz") < 0);
}

static void test_page_bookmark(void) {
    char valid[] = "PAGE\tms0:/PSP/GAME/Tankobon/mangas/Naruto/Vol01.cbz\t42";
    const char *path = NULL;
    int page = -1;
    assert(parse_page_bookmark(valid, &path, &page));
    assert(strcmp(path, "ms0:/PSP/GAME/Tankobon/mangas/Naruto/Vol01.cbz") == 0);
    assert(page == 42);

    char legacy[] = "Vol01.cbz=3";
    assert(!parse_page_bookmark(legacy, &path, &page));
    char invalid[] = "PAGE\t/path/to/book.cbz\tnot-a-number";
    assert(!parse_page_bookmark(invalid, &path, &page));
}

int main(void) {
    test_natural_compare();
    test_page_bookmark();
    puts("core tests passed");
    return 0;
}
