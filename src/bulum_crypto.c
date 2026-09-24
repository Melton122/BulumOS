/*
 * bulum_crypto.c - SHA-256, MD5, 2048-bit big numbers, RSA PKCS#1 v1.5
 * verification for the bulum package manager (Phase 3).
 *
 * Combined into one translation unit with bulum.c at build time:
 *   aarch64-linux-gnu-gcc -static -O2 -s -o rootfs/sbin/bulum \
 *       src/bulum.c src/bulum_crypto.c
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================= SHA-256 ================= */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    int used;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t sha_ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

static void sha256_compress(sha256_ctx *c, const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha_ror(w[i - 15], 7) ^ sha_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha_ror(w[i - 2], 17) ^ sha_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], dd = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha_ror(e, 6) ^ sha_ror(e, 11) ^ sha_ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha_ror(a, 2) ^ sha_ror(a, 13) ^ sha_ror(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = dd + t1;
        dd = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += dd;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372;
    c->h[3] = 0xa54ff53a; c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0; c->used = 0;
}

static void sha256_update(sha256_ctx *c, const unsigned char *p, size_t n)
{
    c->len += n;
    while (n > 0) {
        int room = 64 - c->used;
        size_t take = n < (size_t)room ? n : room;
        memcpy(c->buf + c->used, p, take);
        c->used += (int)take;
        p += take;
        n -= take;
        if (c->used == 64) {
            sha256_compress(c, c->buf);
            c->used = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    unsigned char z = 0;
    while (c->used != 56)
        sha256_update(c, &z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++)
        lb[i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = c->h[i] >> 24;
        out[i * 4 + 1] = c->h[i] >> 16;
        out[i * 4 + 2] = c->h[i] >> 8;
        out[i * 4 + 3] = c->h[i];
    }
}

void sha256_buf(const unsigned char *p, size_t n, unsigned char out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, p, n);
    sha256_final(&c, out);
}

/* hash a file into 32 bytes; all zeros if unreadable */
void sha256_file(const char *path, unsigned char out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    FILE *f = fopen(path, "rb");
    if (!f) {
        memset(out, 0, 32);
        return;
    }
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(&c, buf, n);
    fclose(f);
    sha256_final(&c, out);
}

void sha256_hex(const unsigned char d[32], char out[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
    out[64] = 0;
}

/* ================= MD5 ================= */

typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;
    unsigned char buf[64];
    int used;
} md5_ctx;

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const int md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t md5_rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void md5_compress(md5_ctx *c, const unsigned char *p)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    uint32_t a = c->a, b = c->b, cc = c->c, d = c->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) & 15; }
        else { f = cc ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d; d = cc; cc = b;
        b = b + md5_rol(a + f + md5_k[i] + m[g], md5_s[i]);
        a = t;
    }
    c->a += a; c->b += b; c->c += cc; c->d += d;
}

static void md5_init(md5_ctx *c)
{
    c->a = 0x67452301; c->b = 0xefcdab89;
    c->c = 0x98badcfe; c->d = 0x10325476;
    c->len = 0; c->used = 0;
}

static void md5_update(md5_ctx *c, const unsigned char *p, size_t n)
{
    c->len += n;
    while (n > 0) {
        int room = 64 - c->used;
        size_t take = n < (size_t)room ? n : room;
        memcpy(c->buf + c->used, p, take);
        c->used += (int)take;
        p += take;
        n -= take;
        if (c->used == 64) {
            md5_compress(c, c->buf);
            c->used = 0;
        }
    }
}

static void md5_final(md5_ctx *c, unsigned char out[16])
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80;
    md5_update(c, &pad, 1);
    unsigned char z = 0;
    while (c->used != 56)
        md5_update(c, &z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++)
        lb[i] = (unsigned char)(bits >> (8 * i));
    md5_update(c, lb, 8);
    uint32_t v[4] = { c->a, c->b, c->c, c->d };
    for (int i = 0; i < 4; i++) {
        out[i * 4] = v[i];
        out[i * 4 + 1] = v[i] >> 8;
        out[i * 4 + 2] = v[i] >> 16;
        out[i * 4 + 3] = v[i] >> 24;
    }
}

void md5_buf(const unsigned char *p, size_t n, unsigned char out[16])
{
    md5_ctx c;
    md5_init(&c);
    md5_update(&c, p, n);
    md5_final(&c, out);
}

void md5_file(const char *path, unsigned char out[16])
{
    md5_ctx c;
    md5_init(&c);
    FILE *f = fopen(path, "rb");
    if (!f) {
        memset(out, 0, 16);
        return;
    }
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        md5_update(&c, buf, n);
    fclose(f);
    md5_final(&c, out);
}

/* ================= big numbers (2048-bit RSA) ================= */

#define BILIMBS 64

typedef struct { uint32_t l[BILIMBS]; } bi_t;

static void bi_zero(bi_t *r) { memset(r, 0, sizeof(*r)); }

static int bi_cmp(const bi_t *a, const bi_t *b)
{
    for (int i = BILIMBS - 1; i >= 0; i--) {
        if (a->l[i] != b->l[i])
            return a->l[i] < b->l[i] ? -1 : 1;
    }
    return 0;
}

static int bi_from_hex(bi_t *r, const char *hex)
{
    bi_zero(r);
    int seen = 0;
    for (int i = 0; hex[i]; i++) {
        char ch = hex[i];
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == ':')
            continue;
        else
            return -1;
        seen = 1;
        for (int j = BILIMBS - 1; j >= 0; j--) {
            r->l[j] = (r->l[j] << 4);
            if (j > 0)
                r->l[j] |= r->l[j - 1] >> 28;
        }
        r->l[0] |= v;
    }
    return seen ? 0 : -1;
}

static void bi_to_bytes_be(const bi_t *r, unsigned char *out, int n)
{
    memset(out, 0, n);
    for (int i = 0; i < n; i++) {
        int limb = (n - 1 - i) / 4;
        int sh = (n - 1 - i) % 4 * 8;
        out[i] = (unsigned char)(r->l[limb] >> sh);
    }
}

static int bi_topbit(const bi_t *r)
{
    for (int i = BILIMBS - 1; i >= 0; i--)
        if (r->l[i]) {
            int b = 31;
            while (b > 0 && !(r->l[i] & (1u << b)))
                b--;
            return i * 32 + b;
        }
    return -1;
}

/* w = a*b mod m: schoolbook wide product then binary long division */
static void bi_mulmod(const bi_t *a, const bi_t *b, const bi_t *m, bi_t *r)
{
    uint32_t w[2 * BILIMBS];
    memset(w, 0, sizeof(w));
    for (int i = 0; i < BILIMBS; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < BILIMBS; j++) {
            uint64_t t = (uint64_t)a->l[i] * b->l[j] + w[i + j] + carry;
            w[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        int k = i + BILIMBS;
        while (carry && k < 2 * BILIMBS) {
            uint64_t t = (uint64_t)w[k] + carry;
            w[k] = (uint32_t)t;
            carry = t >> 32;
            k++;
        }
    }
    int topw = -1;
    for (int i = 2 * BILIMBS - 1; i >= 0; i--)
        if (w[i]) {
            int b2 = 31;
            while (b2 > 0 && !(w[i] & (1u << b2)))
                b2--;
            topw = i * 32 + b2;
            break;
        }
    int topm = bi_topbit(m);
    if (topm < 0 || topw < topm) {
        for (int i = 0; i < BILIMBS; i++)
            r->l[i] = w[i];
        return;
    }
    for (int s = topw - topm; s >= 0; s--) {
        int shb = s % 32, shl = s / 32;
        int ge = 1;
        for (int i = 2 * BILIMBS - 1; i >= 0; i--) {
            int idx = i - shl;
            uint32_t mv = 0;
            if (idx >= 0 && idx <= BILIMBS) {
                mv = (idx < BILIMBS ? m->l[idx] : 0) << shb;
                if (shb && idx >= 1)
                    mv |= (idx - 1 < BILIMBS ? m->l[idx - 1] : 0)
                          >> (32 - shb);
            }
            if (w[i] > mv)
                break;
            if (w[i] < mv) {
                ge = 0;
                break;
            }
        }
        if (ge) {
            uint64_t borrow = 0;
            for (int i = 0; i < 2 * BILIMBS; i++) {
                int idx = i - shl;
                uint32_t mv = 0;
                if (idx >= 0 && idx <= BILIMBS) {
                    mv = (idx < BILIMBS ? m->l[idx] : 0) << shb;
                    if (shb && idx >= 1)
                        mv |= (idx - 1 < BILIMBS ? m->l[idx - 1] : 0)
                              >> (32 - shb);
                }
                uint64_t sub = (uint64_t)mv + borrow;
                uint32_t wi = w[i];
                w[i] = wi - (uint32_t)sub;
                borrow = (sub > wi) ? 1 : 0;
            }
        }
    }
    for (int i = 0; i < BILIMBS; i++)
        r->l[i] = w[i];
}



/* r = b^e mod m, public exponent fits in 64 bits */
static void bi_expmod(bi_t *r, const bi_t *b, uint64_t e, const bi_t *m)
{
    unsigned top = 63;
    while (top > 0 && !(e & (1ULL << top)))
        top--;
    bi_zero(r);
    r->l[0] = 1;
    for (int i = top; i >= 0; i--) {
        bi_mulmod(r, r, m, r);
        if (e & (1ULL << i))
            bi_mulmod(r, b, m, r);
    }
}

/*
 * Verify a PKCS#1 v1.5 SHA-256 signature over data[len].
 *
 * key file format (one field per line):
 *   name=<name>
 *   e=<public exponent decimal>
 *   n=<hex modulus>
 *
 * sig_hex: hex string of the signature (as placed in Release.sig).
 * Returns 0 if the signature is valid, -1 on any failure.
 */
int rsa_verify_sig(const char *key_path, const unsigned char *data,
                          long len, const char *sig_hex)
{
    FILE *f = fopen(key_path, "r");
    if (!f)
        return -1;
    uint64_t e = 65537;
    bi_t n;
    int have_n = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        if (!strcmp(line, "e"))
            e = strtoull(eq + 1, NULL, 10);
        else if (!strcmp(line, "n") && bi_from_hex(&n, eq + 1) == 0)
            have_n = 1;
    }
    fclose(f);
    /* only 2048-bit keys are supported (BILIMBS = 64 limbs); a leading
     * zero byte (257-byte dump) is fine and means n < 2^2048 */
    if (!have_n || bi_topbit(&n) < 2047 || bi_topbit(&n) >= 2048)
        return -1;

    bi_t s, em;
    if (bi_from_hex(&s, sig_hex) != 0)
        return -1;
    if (bi_cmp(&s, &n) >= 0)
        return -1;
    bi_expmod(&em, &s, e, &n);

    unsigned char digest[32];
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, digest);

    /* EM for 2048-bit RSA = 00 01 FF..FF 00 DER(DigestInfo SHA-256) */
    static const unsigned char der[] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    unsigned char expect[256];
    memset(expect, 0, 256);
    expect[1] = 0x01;
    memset(expect + 2, 0xff, 256 - 3 - 19 - 32);
    expect[256 - 19 - 32 - 1] = 0x00;
    memcpy(expect + 256 - 19 - 32, der, 19);
    memcpy(expect + 256 - 32, digest, 32);

    unsigned char got[256];
    bi_to_bytes_be(&em, got, 256);
    return memcmp(got, expect, 256) == 0 ? 0 : -1;
}
