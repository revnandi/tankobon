#include "core.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int natural_compare(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *a_end = a;
            const char *b_end = b;
            while (isdigit((unsigned char)*a_end)) a_end++;
            while (isdigit((unsigned char)*b_end)) b_end++;
            int a_len = (int)(a_end - a);
            int b_len = (int)(b_end - b);
            if (a_len != b_len) return a_len < b_len ? -1 : 1;
            int number_cmp = strncmp(a, b, (size_t)a_len);
            if (number_cmp != 0) return number_cmp;
            a = a_end;
            b = b_end;
        } else {
            int ca = tolower((unsigned char)*a);
            int cb = tolower((unsigned char)*b);
            if (ca != cb) return ca < cb ? -1 : 1;
            a++;
            b++;
        }
    }
    return *a ? 1 : (*b ? -1 : 0);
}

int parse_page_bookmark(char *line, const char **path, int *page) {
    if (strncmp(line, "PAGE\t", 5) != 0) return 0;

    char *page_text = strrchr(line + 5, '\t');
    if (!page_text) return 0;

    char *end = NULL;
    long parsed_page = strtol(page_text + 1, &end, 10);
    if (!end || *end != '\0' || parsed_page < 0 || parsed_page > 0x7FFFFFFF) return 0;

    *page_text = '\0';
    *path = line + 5;
    *page = (int)parsed_page;
    return 1;
}
