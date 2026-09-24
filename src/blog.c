/*
 * BulumOS log viewer - tail/view/filter/rotate /var/log/messages.
 * usage: blog [opts] [file]
 *   -f              follow appended data
 *   -n LINES        last N lines
 *   -g PATTERN      filter lines matching PATTERN (grep)
 *   -v PATTERN      filter OUT lines matching PATTERN
 *   -r              rotate the log (move to .1, .2, ...), then reopen
 *   -a              show all levels (default)
 *   -h              help
 *
 * Exit codes: 0 ok, 1 error, 2 usage.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#define C_RESET  "\033[0m"
#define C_RED    "\033[31m"
#define C_YELLOW "\033[33m"
#define C_GRAY   "\033[90m"
#define C_GREEN  "\033[32m"

static const char *DEF_LOG = "/var/log/messages";

static int follow = 0;
static int lines = 100;
static const char *grep_pat = NULL;   /* keep matching */
static const char *vgrep_pat = NULL;  /* drop matching */
static int rotate = 0;

static void colorize(const char *line)
{
    if (strstr(line, "err") || strstr(line, "fail") || strstr(line, "panic"))
        printf(C_RED "%s" C_RESET, line);
    else if (strstr(line, "warn") || strstr(line, "crit"))
        printf(C_YELLOW "%s" C_RESET, line);
    else
        printf(C_GRAY "%s" C_RESET, line);
}

static int keep(const char *line)
{
    if (grep_pat && !strstr(line, grep_pat))
        return 0;
    if (vgrep_pat && strstr(line, vgrep_pat))
        return 0;
    return 1;
}

static int rotate_log(const char *path)
{
    /* keep up to 4 rotated copies: path.1 .. path.4 */
    char src[PATH_MAX], dst[PATH_MAX];
    int i;
    for (i = 4; i >= 1; i--) {
        if (i == 1)
            snprintf(src, sizeof(src), "%s", path);
        else
            snprintf(src, sizeof(src), "%s.%d", path, i - 1);
        if (access(src, F_OK) != 0)
            continue;
        snprintf(dst, sizeof(dst), "%s.%d", path, i);
        rename(src, dst);
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-f] [-n LINES] [-g PAT] [-v PAT] [-r] [logfile]\n", prog);
}

int main(int argc, char **argv)
{
    const char *path = DEF_LOG;
    int c;

    while ((c = getopt(argc, argv, "fn:g:v:rah")) != -1) {
        switch (c) {
        case 'f': follow = 1; break;
        case 'n': lines = atoi(optarg); if (lines < 1) lines = 1; break;
        case 'g': grep_pat = optarg; break;
        case 'v': vgrep_pat = optarg; break;
        case 'r': rotate = 1; break;
        case 'a': break; /* all levels default */
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc)
        path = argv[optind];

    if (rotate) {
        if (rotate_log(path) != 0) {
            fprintf(stderr, "blog: rotate failed\n");
            return 1;
        }
        printf(C_GREEN "[ OK ]" C_RESET " rotated %s -> .1 (kept .1..%d)\n", path, 4);
        return 0;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "blog: cannot open %s\n", path);
        return 1;
    }

    /* skip to last N lines */
    if (lines > 0) {
        fseek(f, 0, SEEK_END);
        long pos = ftell(f);
        int count = 0;
        while (pos > 0) {
            pos--;
            fseek(f, pos, SEEK_SET);
            if (fgetc(f) == '\n') {
                count++;
                if (count >= lines + 1) {
                    pos++;
                    break;
                }
            }
        }
        fseek(f, pos, SEEK_SET);
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (keep(line)) {
            colorize(line);
            fflush(stdout);
        }
    }
    free(line);

    if (follow) {
        for (;;) {
            fseek(f, 0, SEEK_END);
            while ((n = getline(&line, &cap, f)) != -1) {
                if (keep(line)) {
                    colorize(line);
                    fflush(stdout);
                }
            }
            usleep(200000);
        }
    }
    fclose(f);
    return 0;
}
