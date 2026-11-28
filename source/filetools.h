#ifndef FILETOOLS_H
#define FILETOOLS_H

#include <stdbool.h>

/* Two files side by side in hex, the bytes that differ in red; A / 1 jump to the next / previous difference. */
void hexcompare(const char *a, const char *b);

/* This file's contents replaced by another's (the name and place stay). */
bool file_replace(const char *dst, const char *src);

/* Another file written into this one at an offset (asked for), in place, as GodMode9's "inject data". */
bool file_inject(const char *dst, const char *src);

/* What a WAD, TMD, ticket or installed title says (title ID, version, IOS, region, contents, ticket...),
 * like ShowMiiWads or NUSD show it. false: not one of those. */
bool titleinfo_can(const char *path);
void titleinfo_show(const char *path);

#endif
