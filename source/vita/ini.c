#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "inih/ini.h"

#define MAX_LINE 400
#define MAX_SECTION 64
#define MAX_NAME 64

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = 0;
    return s;
}

static void strip_inline_comment(char *s) {
    for (char *p = s; *p; p++) {
        if ((*p == ';' || *p == '#') && p > s && isspace((unsigned char)p[-1])) {
            *p = 0;
            return;
        }
    }
}

int ini_parse(const char *filename, ini_handler handler, void *user) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;

    char line[MAX_LINE];
    char section[MAX_SECTION] = "";
    int lineno = 0;
    int first_error = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *start = line;
        if (lineno == 1 && (unsigned char)start[0] == 0xEF && (unsigned char)start[1] == 0xBB && (unsigned char)start[2] == 0xBF)
            start += 3;
        start = trim(start);
        if (*start == 0 || *start == ';' || *start == '#') continue;

        if (*start == '[') {
            char *end = strchr(start + 1, ']');
            if (!end) {
                if (!first_error) first_error = lineno;
                continue;
            }
            *end = 0;
            strncpy(section, start + 1, sizeof(section) - 1);
            section[sizeof(section) - 1] = 0;
            continue;
        }

        char *eq = strchr(start, '=');
        if (!eq) eq = strchr(start, ':');
        if (!eq) {
            if (!first_error) first_error = lineno;
            continue;
        }
        *eq = 0;
        char *name = trim(start);
        char *value = eq + 1;
        strip_inline_comment(value);
        value = trim(value);
        if (strlen(name) >= MAX_NAME) {
            if (!first_error) first_error = lineno;
            continue;
        }
        if (!handler(user, section, name, value) && !first_error) first_error = lineno;
    }

    fclose(f);
    return first_error;
}
