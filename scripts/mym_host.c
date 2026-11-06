/* Host build of the mym -> csm converter (source/mym.c), for checking it on a PC:
 *   mym_host <unzipped mym folder> <stock menu .app> <out.csm>
 * See mym_host_test.sh. */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <ftw.h>

#include "mym.h"

static char root[4096];
static char **names, **files;
static int nfiles;

static int walk(const char *path, const struct stat *st, int type, struct FTW *f)
{
    (void)st; (void)f;
    if (type != FTW_F) return 0;
    const char *rel = path + strlen(root);
    while (*rel == '/') rel++;
    char *n = strdup(rel);
    for (char *c = n; *c; c++) *c = *c == '\\' ? '/' : tolower((unsigned char)*c);
    names = realloc(names, sizeof(char *) * (nfiles + 1));
    files = realloc(files, sizeof(char *) * (nfiles + 1));
    names[nfiles] = n;
    files[nfiles++] = strdup(path);
    return 0;
}

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

static bool fetch(void *ctx, const char *member, uint8_t **data, uint32_t *len)
{
    (void)ctx;
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(names[i], member)) return (*data = slurp(files[i], len)) != NULL;
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: mym_host <unzipped mym> <menu.app> <out.csm>\n");
        return 2;
    }
    snprintf(root, sizeof(root), "%s", argv[1]);
    nftw(root, walk, 16, FTW_PHYS);
    uint32_t blen, olen;
    uint8_t *base = slurp(argv[2], &blen);
    if (!base) { perror(argv[2]); return 1; }
    mym_result res;
    char err[256];
    uint8_t *out = mym_build(base, blen, fetch, NULL, NULL, NULL, &olen, &res, err, sizeof(err));
    if (!out) { fprintf(stderr, "error: %s\n", err); return 1; }
    fputs(res.report, stderr);
    FILE *f = fopen(argv[3], "wb");
    fwrite(out, 1, olen, f);
    fclose(f);
    printf("wrote %s (%u bytes, %d applied, %d skipped)\n", argv[3], olen, res.applied, res.skipped);
    return 0;
}
