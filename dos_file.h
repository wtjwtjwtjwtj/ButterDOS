/*
 * dos_file.h – File I/O layer for Butterscotch on DOS.
 *
 * DJGPP's libc already routes fopen/fread through INT 21h with LFN
 * support if a LFN TSR is resident (FreeDOS LFN, DOSLFN, or Windows 9x).
 * This module wraps that into two operations Butterscotch actually needs:
 *   - read an entire file into one malloc'd buffer
 *   - enumerate a directory
 */

#ifndef DOS_FILE_H
#define DOS_FILE_H

#include <stddef.h>

/* Reads the entire file at `path` into a freshly malloc'd buffer.
 O n *success, returns the buffer and writes its byte length to *out_size.
 On failure, returns NULL and *out_size is set to 0.
 The caller owns the buffer and must free() it. */
void *dos_file_read_all(const char *path, size_t *out_size);

/* Returns 1 if the path exists and is a regular file. */
int   dos_file_exists(const char *path);

/* Returns the file size in bytes, or -1 on failure. */
long  dos_file_size(const char *path);

/* Directory enumeration. Callback receives each entry name (8.3 or LFN,
 dep*ending on what the filesystem yields). `is_dir` is 1 for directories. */
typedef void (*dos_dir_cb)(const char *name, int is_dir, void *user);

/* Lists all entries in `dir`. Returns 0 on success, -1 on failure. */
int   dos_dir_list(const char *dir, dos_dir_cb cb, void *user);

#endif
