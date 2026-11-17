#ifndef S2_ZIP_FILES_H
#define S2_ZIP_FILES_H
#include "../launcher/native/zipmount.h"
#include <sys/stat.h>

int s2_zip_init(const char *root);
int s2_zip_path(const char *host);
const zm_entry *s2_zip_find(const char *host);
int s2_zip_stat(const char *host, struct stat *st);
int s2_zip_list(const char *host, void (*each)(void *, const char *, unsigned char), void *arg);
long long s2_zip_read(const zm_entry *entry, void *buffer, size_t size, uint64_t offset);
int s2_zip_copy(const zm_entry *entry, const char *host);
uint8_t *s2_read_file(const char *host, size_t *size);
int s2_zip_memory_fd(const zm_entry *entry);
#endif
