#ifndef AES_H
#define AES_H

#include <stdint.h>
#include <stddef.h>

/* AES-128 decryption (CBC), enough to read WADs, content.bin and data.bin.
 * Table driven; the tables are built on first use. */

typedef struct {
    uint32_t dk[44];    /* decryption round keys */
} aes128_t;

void aes128_init_dec(aes128_t *ctx, const uint8_t key[16]);

/* CBC decrypt len bytes (multiple of 16). in and out may be the same buffer.
 * iv is not modified. */
void aes128_cbc_decrypt(const aes128_t *ctx, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);

/* One block, ECB, encrypt (key derivation only; not table based, not fast). */
void aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#endif
