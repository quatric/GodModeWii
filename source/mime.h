#ifndef MIME_H
#define MIME_H

#include <stdbool.h>
#include <stddef.h>

/* A mail message (headers with MIME-Version / Content-Type and From, Subject...): Wii Message Board mail, .eml. */
bool mime_is(const unsigned char *buf, size_t len);
/* Its main headers and its text, decoded, as UTF-8 (malloc'd, NUL-terminated); NULL when out of memory. */
char *mime_render(const unsigned char *buf, size_t len, size_t *outlen);

#endif
