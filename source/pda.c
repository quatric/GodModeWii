#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "pda.h"
#include "vfs.h"

bool pda_dir(const char *sub, char *out, size_t n)
{
    static const int order[] = { DEV_SD, DEV_USB0, DEV_USB1, DEV_USB2, DEV_USB3,
                                 DEV_USB4, DEV_USB5, DEV_USB6, DEV_USB7 };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        const vfs_dev *d = &g_devs[order[i]];
        if (!d->mounted || d->ext) continue;
        char base[VFS_PATH_MAX];
        snprintf(base, sizeof(base), "%sGodModeWii", d->root);
        mkdir(base, 0777);
        snprintf(out, n, "%s/%s", base, sub);
        mkdir(out, 0777);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    }
    return false;
}
