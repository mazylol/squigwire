#include "include/util.h"

#include <ctype.h>
#include <string.h>

char *trim(char *s) {
    char *end;
    while (*s != '\0' && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}
