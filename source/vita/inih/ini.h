#ifndef RV_INI_LITE_H
#define RV_INI_LITE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*ini_handler)(void *user, const char *section, const char *name, const char *value);

int ini_parse(const char *filename, ini_handler handler, void *user);

#ifdef __cplusplus
}
#endif

#endif
