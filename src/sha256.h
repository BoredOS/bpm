#ifndef BPM_SHA256_H
#define BPM_SHA256_H
#include <stddef.h>
int sha256_file(const char *path, char *hex_out, size_t hex_len);
#endif
