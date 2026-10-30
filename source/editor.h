#ifndef EDITOR_H
#define EDITOR_H

/* In-app editors for files on writable drives. Both load the file into memory,
 * edit it there and only write it back when the user saves. */

/* Change bytes in place (the size stays the same), up to 4 MiB. */
void editor_hex(const char *path);

/* Edit text a line at a time, up to 1 MiB. CRLF files stay CRLF. */
void editor_text(const char *path);

#endif
