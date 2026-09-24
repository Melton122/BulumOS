/*
 * bulum_crypto.h - shared interface between bulum.c and bulum_crypto.c
 */
#ifndef BULUM_CRYPTO_H
#define BULUM_CRYPTO_H

#include <stdint.h>

void sha256_buf(const unsigned char *p, size_t n, unsigned char out[32]);
void sha256_file(const char *path, unsigned char out[32]);
void sha256_hex(const unsigned char d[32], char out[65]);
void md5_buf(const unsigned char *p, size_t n, unsigned char out[16]);
void md5_file(const char *path, unsigned char out[16]);
int rsa_verify_sig(const char *key_path, const unsigned char *data,
                   long len, const char *sig_hex);

#endif