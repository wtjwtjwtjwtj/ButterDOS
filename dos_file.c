/*
 * dos_file.c – File I/O layer for Butterscotch on DOS.
 */

#include "dos_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dir.h>        /* DJGPP: findfirst / findnext / struct ffblk */
#include <sys/stat.h>

void *dos_file_read_all(const char *path, size_t *out_size)
{
    FILE  *f;
    long   sz;
    void  *buf;

    if (out_size) *out_size = 0;

    f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    /* Allocate one extra byte so callers can rely on a trailing NUL. */
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    if (sz > 0) {
        size_t got = fread(buf, 1, (size_t)sz, f);
        if (got != (size_t)sz) {
            free(buf);
            fclose(f);
            return NULL;
        }
    }
    ((unsigned char *)buf)[sz] = '\0';

    fclose(f);
    if (out_size) *out_size = (size_t)sz;
    return buf;
}

int dos_file_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

long dos_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

int dos_dir_list(const char *dir, dos_dir_cb cb, void *user)
{
    struct ffblk  ff;
    char          pattern[300];
    int           done;
    size_t        dlen;

    if (!dir || !cb) return -1;

    dlen = strlen(dir);

    /* Refuse paths that would overflow our pattern buffer.
     *      300 - 4 ("*.*") - 2 (separator + NUL) = 294 max usable. */
    if (dlen > 290) return -1;

    if (dlen == 0) {
        strcpy(pattern, "*.*");
    } else if (dir[dlen - 1] == '\\' || dir[dlen - 1] == '/') {
        memcpy(pattern, dir, dlen);
        strcpy(pattern + dlen, "*.*");
    } else {
        memcpy(pattern, dir, dlen);
        pattern[dlen]     = '\\';
        pattern[dlen + 1] = '*';
        pattern[dlen + 2] = '.';
        pattern[dlen + 3] = '*';
        pattern[dlen + 4] = '\0';
    }

    done = findfirst(pattern, &ff, FA_DIREC | FA_ARCH | FA_RDONLY | FA_HIDDEN | FA_SYSTEM);
    while (done == 0) {
        if (strcmp(ff.ff_name, ".") != 0 && strcmp(ff.ff_name, "..") != 0) {
            int is_dir = (ff.ff_attrib & FA_DIREC) ? 1 : 0;
            cb(ff.ff_name, is_dir, user);
        }
        done = findnext(&ff);
    }
    return 0;
}
