/*
 * GodModeWii glue: a read-only stdio stream over vfs_file (funopen).
 * vfs_read wants a 32-byte aligned buffer, so reads bounce through one.
 */
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "mc_io.h"
#include "vfs.h"

#ifdef __LARGE64_FILES
typedef _fpos64_t mc_pos;
#else
typedef fpos_t mc_pos;
#endif

#define BOUNCE (64 * 1024)

typedef struct {
    vfs_file f;
    u64 pos;
    u8 *buf;
} mc_file;

static int mc_read(void *c, char *out, _READ_WRITE_BUFSIZE_TYPE len)
{
    mc_file *m = c;
    int done = 0;
    int n = (int)len;
    if (m->pos >= m->f.size) return 0;
    if ((u64)n > m->f.size - m->pos) n = (int)(m->f.size - m->pos);
    while (done < n) {
        u32 want = n - done > BOUNCE ? BOUNCE : (u32)(n - done);
        s32 r = vfs_read(&m->f, m->buf, want);
        if (r <= 0) break;
        memcpy(out + done, m->buf, r);
        done += r;
        m->pos += r;
    }
    return done ? done : -1;
}

static mc_pos mc_seek(void *c, mc_pos off, int whence)
{
    mc_file *m = c;
    s64 np = whence == SEEK_SET ? (s64)off : whence == SEEK_CUR ? (s64)m->pos + off : (s64)m->f.size + off;
    if (np < 0) return -1;
    if ((u64)np > m->f.size) np = m->f.size;
    if (!vfs_seek(&m->f, (u64)np)) return -1;
    m->pos = (u64)np;
    return (mc_pos)np;
}

static int mc_close(void *c)
{
    mc_file *m = c;
    vfs_close(&m->f);
    free(m->buf);
    free(m);
    return 0;
}

FILE *mc_fopen(const char *path)
{
    /* SD / USB: libfat directly, as wii-mobiclip-player reads (the demuxer sets a 128 KiB
     * buffer). Going through vfs adds a stream, a bounce copy and a second buffer per read,
     * which made playback stutter. Archives, the NAND and discs still go through vfs. */
    int dev = vfs_dev_of(path);
    if (dev >= 0 && !vfs_is_archive(path) && !vfs_is_virtual(dev) && dev != DEV_NAND)
        return fopen(path, "rb");
    mc_file *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->buf = memalign(32, BOUNCE);
    if (!m->buf || !vfs_open_read(&m->f, path)) { free(m->buf); free(m); return NULL; }
    FILE *fp = funopen(m, mc_read, NULL, mc_seek, mc_close);
    if (!fp) { mc_close(m); return NULL; }
    setvbuf(fp, NULL, _IOFBF, 32 * 1024);
    return fp;
}
