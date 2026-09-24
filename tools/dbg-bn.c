/* debug: compute s^e mod n with bulum's bigint and print as hex */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/bulum_crypto.c" /* static funcs visible in same TU */

static char *strip(const char *p)
{
    char *out = malloc(strlen(p) + 1);
    int j = 0;
    for (int i = 0; p[i]; i++)
        if (p[i] != '\n' && p[i] != '\r' && p[i] != ' ' && p[i] != '\t')
            out[j++] = p[i];
    out[j] = 0;
    return out;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "mulhex")) {
        bi_t a, b, m, r;
        bi_from_hex(&a, argv[2]);
        bi_from_hex(&b, argv[3]);
        bi_from_hex(&m, argv[4]);
        bi_mulmod(&a, &b, &m, &r);
        char buf[2048];
        bi_to_bytes_be(&r, (unsigned char *)buf, 256);
        for (int i = 0; i < 256; i++)
            printf("%02x", (unsigned char)buf[i]);
        printf("\n");
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "muldbg")) {
        printf("debug copy removed\n");
        return 0;
    }
    if (argc < 3) {
        printf("usage: dbg-bn <keyfile> <sighexfile>\n");
        return 1;
    }
    FILE *kf = fopen(argv[1], "r");
    bi_t n;
    uint64_t e = 65537;
    char line[4096];
    while (fgets(line, sizeof(line), kf)) {
        if (!strncmp(line, "e=", 2))
            e = strtoull(line + 2, NULL, 10);
        else if (!strncmp(line, "n=", 2))
            bi_from_hex(&n, line + 2);
    }
    fclose(kf);
    FILE *sf = fopen(argv[2], "r");
    char sigbuf[8192] = "";
    if (sf) {
        size_t got = fread(sigbuf, 1, sizeof(sigbuf) - 1, sf);
        sigbuf[got] = 0;
        fclose(sf);
    }
    char *sig = strip(sigbuf);
    bi_t s, em;
    bi_from_hex(&s, sig);
    printf("topbit n = %d\n", bi_topbit(&n));
    printf("n = ");
    bi_to_bytes_be(&n, (unsigned char *)line, 256);
    for (int i = 0; i < 256; i++) printf("%02x", (unsigned char)line[i]);
    printf("\n");
    printf("s = ");
    bi_to_bytes_be(&s, (unsigned char *)line, 256);
    for (int i = 0; i < 256; i++) printf("%02x", (unsigned char)line[i]);
    printf("\n");
    printf("s>=n? %d\n", bi_cmp(&s, &n) >= 0);
    if (argc > 3 && !strcmp(argv[3], "pow1")) {
        bi_expmod(&em, &s, 1, &n);
        printf("pow1(s) = ");
        bi_to_bytes_be(&em, (unsigned char *)line, 256);
        for (int i = 0; i < 256; i++) printf("%02x", (unsigned char)line[i]);
        printf("\n");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "pow2")) {
        bi_expmod(&em, &s, 2, &n);
        printf("pow2(s) = ");
        bi_to_bytes_be(&em, (unsigned char *)line, 256);
        for (int i = 0; i < 256; i++) printf("%02x", (unsigned char)line[i]);
        printf("\n");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "mul2")) {
        bi_mulmod(&s, &s, &n, &em);
        printf("mul2(s) = ");
        bi_to_bytes_be(&em, (unsigned char *)line, 256);
        for (int i = 0; i < 256; i++) printf("%02x", (unsigned char)line[i]);
        printf("\n");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "cube")) {
        bi_expmod(&em, &s, 3, &n);
        printf("cube(s) = ");
        bi_to_bytes_be(&em, (unsigned char *)line, 256);
        for (int i = 0; i < 256; i++) printf("%02x", (unsigned char)line[i]);
        printf("\n");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "pow5")) {
        bi_expmod(&em, &s, 5, &n);
        printf("pow5(s) = ");
        bi_to_bytes_be(&em, (unsigned char *)line, 256);
        for (int i = 0; i < 256; i++) printf("%02x", (unsigned char)line[i]);
        printf("\n");
        return 0;
    }
    bi_expmod(&em, &s, e, &n);
    printf("pow(s,e,n) [e=%llu] = ", (unsigned long long)e);
    bi_to_bytes_be(&em, (unsigned char *)line, 256);
    for (int i = 0; i < 256; i++) printf("%02x", (unsigned char)line[i]);
    printf("\n");
    return 0;
}