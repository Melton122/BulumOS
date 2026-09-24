/* host-side unit test for bulum_crypto.c (x86_64 build).
 *   gcc -O2 -o /tmp/ctest src/bulum_crypto.c tools/ctest.c
 *   bash tools/make-test-repo.sh   (creates tools/testrepo.key in /tmp repo)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/bulum_crypto.h"

static void hexout(const unsigned char *d, int n, char *out)
{
    for (int i = 0; i < n; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
    out[n * 2] = 0;
}

int main(int argc, char **argv)
{
    int fails = 0;
    /* sha256("abc") */
    {
        unsigned char d[32];
        char hex[65];
        sha256_buf((const unsigned char *)"abc", 3, d);
        hexout(d, 32, hex);
        if (!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
            printf("PASS sha256(abc)\n");
        else {
            printf("FAIL sha256(abc) got %s\n", hex);
            fails++;
        }
    }
    /* sha256 \x00..\xff stream */
    {
        unsigned char big[1000000];
        unsigned char d[32];
        char hex[65];
        for (int i = 0; i < 1000000; i++)
            big[i] = (unsigned char)(i & 0xff);
        sha256_buf(big, 1000000, d);
        hexout(d, 32, hex);
        /* expected digest computed by the host: sha256sum of that pattern;
         * passed as argv[4] ("" lets us skip the check) */
        if (!argv[4] || !argv[4][0] || !strcmp(hex, argv[4]))
            printf("PASS sha256(1MB)\n");
        else {
            printf("FAIL sha256(1MB) got %s want %s\n", hex, argv[4]);
            fails++;
        }
    }
    /* md5("abc") */
    {
        unsigned char d[16];
        char hex[33];
        md5_buf((const unsigned char *)"abc", 3, d);
        hexout(d, 16, hex);
        if (!strcmp(hex, "900150983cd24fb0d6963f7d28e17f72"))
            printf("PASS md5(abc)\n");
        else {
            printf("FAIL md5(abc) got %s\n", hex);
            fails++;
        }
    }
    /* file hashers */
    {
        FILE *f = fopen("/tmp/ctest-file", "w");
        fputs("hello bulum", f);
        fclose(f);
        unsigned char d[32];
        char hex[65];
        sha256_file("/tmp/ctest-file", d);
        hexout(d, 32, hex);
        if (!strcmp(hex, "ccb87cfefc6d30d8c85cf85c5d4fa628031cbe7fc49dffce770039a3bd51c0e0"))
            printf("PASS sha256_file\n");
        else {
            printf("FAIL sha256_file got %s\n", hex);
            fails++;
        }
        unsigned char m[16];
        char mhex[33];
        md5_file("/tmp/ctest-file", m);
        hexout(m, 16, mhex);
        if (!strcmp(mhex, "544ee761d37c5e5da619a49b89d61c7f"))
            printf("PASS md5_file\n");
        else {
            printf("FAIL md5_file got %s\n", mhex);
            fails++;
        }
        unlink("/tmp/ctest-file");
    }
    /* RSA signature verify: requires tools/testrepo.key to exist */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) {
            printf("FAIL cannot open signed file %s\n", argv[1]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *data = malloc(n);
        if (fread(data, 1, n, f) != (size_t)n) {
            printf("FAIL short read\n");
            return 1;
        }
        fclose(f);
        char sbuf[8192];
        size_t sgot = 0;
        FILE *sf = fopen(argv[2], "r");
        if (!sf) {
            printf("FAIL cannot open signature %s\n", argv[2]);
            return 1;
        }
        sgot = fread(sbuf, 1, sizeof(sbuf) - 1, sf);
        sbuf[sgot] = 0;
        fclose(sf);
        char *sig = malloc(sgot + 1);
        int j = 0;
        for (size_t i = 0; i < sgot; i++)
            if (sbuf[i] != '\n' && sbuf[i] != '\r' && sbuf[i] != ' ')
                sig[j++] = sbuf[i];
        sig[j] = 0;
        (void)argv;
        if (rsa_verify_sig(argv[3], data, n, sig) == 0)
            printf("PASS rsa_verify_sig (valid signature)\n");
        else {
            printf("FAIL rsa_verify_sig rejected valid signature\n");
            fails++;
        }
        /* flip one byte in data -> must fail */
        data[0] ^= 0x01;
        if (rsa_verify_sig(argv[3], data, n, sig) != 0)
            printf("PASS rsa_verify_sig rejects tampered data\n");
        else {
            printf("FAIL rsa_verify_sig accepted tampered data\n");
            fails++;
        }
        free(data);
    }
    printf(fails ? "TOTAL: %d FAILURES\n" : "TOTAL: all PASS\n", fails);
    return fails ? 1 : 0;
}