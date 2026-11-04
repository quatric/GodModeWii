#ifndef ECC233_H
#define ECC233_H

#include <stdbool.h>
#include <stdint.h>

/* ECDSA on sect233r1 (B-233), as the Wii signs with its console (NG) and AP keys. Keys, coordinates
 * and r / s are 30-byte big-endian strings; the public key is x then y. The hash is a SHA-1. */
void ecc233_public_key(const uint8_t priv[30], uint8_t pub[60]);
void ecc233_sign(const uint8_t priv[30], const uint8_t hash[20], uint8_t r[30], uint8_t s[30]);
bool ecc233_verify(const uint8_t pub[60], const uint8_t hash[20], const uint8_t r[30], const uint8_t s[30]);

#endif
