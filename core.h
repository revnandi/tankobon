#ifndef TANKOBON_CORE_H
#define TANKOBON_CORE_H

int natural_compare(const char *a, const char *b);
int parse_page_bookmark(char *line, const char **path, int *page);

#endif
