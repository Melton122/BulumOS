#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/bulum_crypto.h"

int main(void)
{
    FILE *f = fopen("/tmp/head.bin", "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(n);
    fread(d, 1, n, f);
    fclose(f);
    f = fopen("/tmp/sig.bin", "rb");
    long sn = 0;
    char *s = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        sn = ftell(f);
        fseek(f, 0, SEEK_SET);
        s = malloc(sn * 2 + 1);
        char *h = malloc(sn * 2 + 1);
        fread(h, 1, sn, f);
        fclose(f);
        for (long i = 0; i < sn; i++)
            sprintf(s + i * 2, "%02x", (unsigned char)h[i]);
        s[sn * 2] = 0;
    }
    printf("head=%ld sigbin=%ld\n", n, sn);
    int rc = rsa_verify_sig("/tmp/bpkkeys/main.pub", (unsigned char *)d, n, s);
    printf("rsa_verify_sig = %d\n", rc);
    return rc;
}