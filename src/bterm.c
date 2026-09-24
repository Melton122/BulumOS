/*
 * bterm - BulumOS native terminal (console/PTY frontend)
 *
 * Allocates a PTY, runs the child (/bin/sh by default, or the -e command)
 * with the pty slave as its controlling terminal, and forwards raw bytes
 * between the real terminal (stdin/stdout) and the pty master.
 *
 * The live byte stream passes through unmodified, so vi/less/top and full
 * ANSI color work exactly as on the terminal they run on.  In parallel a
 * minimal VT parser tracks the text into a bounded scrollback ring buffer
 * (default 10000 lines) which can be browsed with a Ctrl+A prefix:
 *
 *   Ctrl+A then u / PgUp    scroll back one page
 *   Ctrl+A then d / PgDn    scroll forward one page
 *   Ctrl+A then q           return to live output (also Enter/Ctrl+A x/Ctrl+Z)
 *   Ctrl+A then Ctrl+A      enter scrollback viewer (first scroll also enters)
 *
 * In the viewer: u/PgUp and d/PgDn page, k/j move a line, q/Enter/Ctrl+A
 * return to live output.
 *
 * Signals:
 *   SIGWINCH -> push the real terminal size to the pty (TIOCSWINSZ)
 *   SIGCHLD  -> reap the child, propagate its exit status
 *   SIGTERM/SIGINT/SIGQUIT -> terminate the child, restore the terminal, exit
 *
 * The terminal is always restored to its original mode when bterm exits.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define BT_NAME "Bulum Terminal"
#define BT_VER "0.1"

#define DEF_SCROLLBACK 10000
#define MIN_SCROLLBACK 100
#define MAX_SCROLLBACK 50000

/* line text storage cap (bytes).  a scrollback line wider than this is
 * truncated (viewer-only; the live stream is never affected). */
#define LINE_CAP 1024

/* ANSI */
#define CSI "\033["
#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_REV "\033[7m"
#define C_DIM "\033[2m"

/* ---- config ---------------------------------------------------------- */

struct config {
    int scrollback;    /* ring capacity in lines */
    int theme;         /* 0 default, 1 light, 2 dark */
    int cursor_blink;  /* viewer selection blink */
    int banner;        /* print a startup banner */
};

static void cfg_defaults(struct config *c)
{
    c->scrollback = DEF_SCROLLBACK;
    c->theme = 0;
    c->cursor_blink = 0;
    c->banner = 0;
}

/* load one key=value file into c; later files override earlier ones */
static void cfg_file(struct config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        while (*key && isspace((unsigned char)*key))
            key++;
        char *ke = key + strlen(key);
        while (ke > key && isspace((unsigned char)ke[-1]))
            *--ke = 0;
        while (*val && isspace((unsigned char)*val))
            val++;
        char *ve = val + strlen(val);
        while (ve > val && isspace((unsigned char)ve[-1]))
            *--ve = 0;
        if (!strcmp(key, "scrollback")) {
            int n = atoi(val);
            if (n >= MIN_SCROLLBACK && n <= MAX_SCROLLBACK)
                c->scrollback = n;
        } else if (!strcmp(key, "theme")) {
            if (!strcmp(val, "light"))
                c->theme = 1;
            else if (!strcmp(val, "dark"))
                c->theme = 2;
        } else if (!strcmp(key, "cursor-blink")) {
            c->cursor_blink = (!strcmp(val, "yes") || !strcmp(val, "1") ||
                               !strcmp(val, "on"));
        } else if (!strcmp(key, "banner")) {
            c->banner = (!strcmp(val, "yes") || !strcmp(val, "1") ||
                         !strcmp(val, "on"));
        }
    }
    fclose(f);
}

static void cfg_load(struct config *c, const char *user_cfg)
{
    cfg_defaults(c);
    cfg_file(c, "/etc/bterm.conf");
    if (user_cfg)
        cfg_file(c, user_cfg);
}

/* ---- UTF-8 ----------------------------------------------------------- */

static int utf8_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

/* decode one UTF-8 sequence; returns bytes used or 0 if invalid.
 * *cp receives the code point (0 for invalid). */
static int utf8_decode(const unsigned char *s, int n, uint32_t *cp)
{
    if (n < 1)
        return 0;
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int len = utf8_len(c);
    if (len > n)
        return 0;
    uint32_t v;
    uint32_t min;
    switch (len) {
    case 2:
        v = c & 0x1F;
        min = 0x80;
        break;
    case 3:
        v = c & 0x0F;
        min = 0x800;
        break;
    default:
        v = c & 0x07;
        min = 0x10000;
        break;
    }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        *cp = 0;
        return 1; /* invalid sequence: consume one byte, caller prints ? */
    }
    *cp = v;
    return len;
}

static int cp_width(uint32_t cp)
{
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 0;
    /* combining marks */
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F))
        return 0;
    /* CJK and wide ranges */
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

/* ---- line + scrollback ring ----------------------------------------- */

#define ST_BOLD 1
#define ST_DIM 2
#define ST_UNDER 4
#define ST_REV 8
#define ST_BLINK 16

struct line {
    unsigned char *b;  /* utf-8 bytes, NUL terminated */
    int len;           /* bytes used */
    int wid;           /* display columns */
    uint8_t style;
    int fg;            /* -1 default, 0..255 palette, -2 truecolor */
    int bg;
    uint32_t fg24, bg24;
};

static void line_init(struct line *l)
{
    l->b = malloc(LINE_CAP);
    l->b[0] = 0;
    l->len = 0;
    l->wid = 0;
    l->style = 0;
    l->fg = -1;
    l->bg = -1;
}

static void line_copy(struct line *dst, const struct line *src)
{
    memcpy(dst, src, sizeof(*dst));
    dst->b = malloc((size_t)src->len + 1);
    memcpy(dst->b, src->b, (size_t)src->len + 1);
}

static void line_free(struct line *l)
{
    free(l->b);
    l->b = NULL;
}

static void line_reset(struct line *l)
{
    l->len = 0;
    l->wid = 0;
    l->style = 0;
    l->fg = -1;
    l->bg = -1;
    l->b[0] = 0;
}

/* trim trailing spaces (they are invisible and pollute the viewer) */
static void line_trim(struct line *l)
{
    int w = 0;
    for (int i = 0; i < l->len;) {
        uint32_t cp;
        int k = utf8_decode(l->b + i, l->len - i, &cp);
        if (k <= 0)
            k = 1;
        if (cp == ' ') {
            l->wid -= 1; /* only ASCII spaces stored, width 1 */
            w = i;
        } else {
            w = i + k;
        }
        i += k;
    }
    if (w < l->len) {
        l->len = w;
        l->b[w] = 0;
    }
}

struct ring {
    struct line *v;
    int cap;
    int n;     /* number of lines stored */
    int head;  /* index of oldest */
};

static void ring_init(struct ring *r, int cap)
{
    r->v = calloc((size_t)cap, sizeof(struct line));
    r->cap = cap;
    r->n = 0;
    r->head = 0;
}

static void ring_free(struct ring *r)
{
    for (int i = 0; i < r->n; i++)
        line_free(&r->v[i]);
    free(r->v);
    r->v = NULL;
    r->n = r->cap = 0;
}

static int ring_count(const struct ring *r)
{
    return r->n;
}

/* index n (0 = oldest, count-1 = newest) */
static struct line *ring_at(struct ring *r, int n)
{
    if (n < 0 || n >= r->n)
        return NULL;
    return &r->v[(r->head + n) % r->cap];
}

static void ring_push(struct ring *r, const struct line *src)
{
    if (r->n < r->cap) {
        int i = (r->head + r->n) % r->cap;
        line_init(&r->v[i]);
        line_copy(&r->v[i], src);
        r->n++;
    } else {
        line_free(&r->v[r->head]);
        line_copy(&r->v[r->head], src);
        r->head = (r->head + 1) % r->cap;
    }
}

/* ---- VT screen state -------------------------------------------------- */

struct term {
    struct line cur;       /* line being built */
    int col;               /* cursor column (0-based) */
    int cols;              /* screen width */
    struct ring ring;      /* live scrollback */
    struct ring alt;       /* alternate-screen buffer */
    int in_alt;
    int stopped;           /* not tracking (e.g. alt screen off when frozen) */
};

static struct term T;

/* push the current line into the active ring (or drop it in alt mode) */
static void vt_newline(void)
{
    line_trim(&T.cur);
    if (T.in_alt) {
        if (T.cur.len > 0 || T.cur.wid > 0)
            ring_push(&T.alt, &T.cur);
    } else {
        if (T.cur.len > 0 || T.cur.wid > 0)
            ring_push(&T.ring, &T.cur);
    }
    line_reset(&T.cur);
    T.col = 0;
}

/* append one code point to the current line with wrap handling */
static void vt_char(uint32_t cp)
{
    int w = cp_width(cp);
    if (w <= 0) {
        /* combining or control: append bytes, no column advance */
        if (w == 0 && T.cur.len + 4 < LINE_CAP && cp >= 0x20) {
            int k = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            unsigned char enc[4];
            if (k == 1)
                enc[0] = (unsigned char)cp;
            else if (k == 2) {
                enc[0] = 0xC0 | (cp >> 6);
                enc[1] = 0x80 | (cp & 0x3F);
            } else if (k == 3) {
                enc[0] = 0xE0 | (cp >> 12);
                enc[1] = 0x80 | ((cp >> 6) & 0x3F);
                enc[2] = 0x80 | (cp & 0x3F);
            } else {
                enc[0] = 0xF0 | (cp >> 18);
                enc[1] = 0x80 | ((cp >> 12) & 0x3F);
                enc[2] = 0x80 | ((cp >> 6) & 0x3F);
                enc[3] = 0x80 | (cp & 0x3F);
            }
            memcpy(T.cur.b + T.cur.len, enc, (size_t)k);
            T.cur.len += k;
            T.cur.b[T.cur.len] = 0;
        }
        return;
    }
    /* spaces / tabs only advance the column */
    if (cp == ' ' || cp == '\t') {
        int adv = (cp == '\t') ? 8 - (T.col % 8) : 1;
        for (int i = 0; i < adv; i++) {
            if (T.col >= T.cols && T.cur.len) {
                vt_newline();
                break;
            }
            if (T.cur.len < LINE_CAP - 1) {
                if (T.col < T.cur.len) { /* overwrite in place */
                    int j = T.col;
                    while (j < T.cur.len) {
                        uint32_t cc;
                        int k = utf8_decode(T.cur.b + j, T.cur.len - j, &cc);
                        if (k <= 0)
                            k = 1;
                        int cw = cp_width(cc);
                        if (cw <= 0) {
                            j += k;
                            continue;
                        }
                        if (cw == 1)
                            break;
                        j += k;
                    }
                    T.cur.len = j;
                    T.cur.b[T.cur.len] = 0;
                    int ww = 0;
                    for (int z = 0; z < T.cur.len;) {
                        uint32_t cc;
                        int k = utf8_decode(T.cur.b + z, T.cur.len - z, &cc);
                        if (k <= 0)
                            k = 1;
                        ww += cp_width(cc);
                        z += k;
                    }
                    T.cur.wid = ww;
                }
                if (T.cur.len < LINE_CAP - 1) {
                    T.cur.b[T.cur.len++] = ' ';
                    T.cur.b[T.cur.len] = 0;
                    T.cur.wid++;
                }
            }
            T.col++;
        }
        return;
    }
    if (T.col >= T.cols && T.cur.len)
        vt_newline();
    int k = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (T.cur.len + k >= LINE_CAP) {
        /* line too long: drop the tail from the ring, keep writing */
        T.cur.len = 0;
        T.cur.wid = 0;
    }
    unsigned char enc[4];
    if (k == 1)
        enc[0] = (unsigned char)cp;
    else if (k == 2) {
        enc[0] = 0xC0 | (cp >> 6);
        enc[1] = 0x80 | (cp & 0x3F);
    } else if (k == 3) {
        enc[0] = 0xE0 | (cp >> 12);
        enc[1] = 0x80 | ((cp >> 6) & 0x3F);
        enc[2] = 0x80 | (cp & 0x3F);
    } else {
        enc[0] = 0xF0 | (cp >> 18);
        enc[1] = 0x80 | ((cp >> 12) & 0x3F);
        enc[2] = 0x80 | ((cp >> 6) & 0x3F);
        enc[3] = 0x80 | (cp & 0x3F);
    }
    memcpy(T.cur.b + T.cur.len, enc, (size_t)k);
    T.cur.len += k;
    T.cur.b[T.cur.len] = 0;
    T.cur.wid += w;
    T.col += w;
}

/* erase in line: mode 0 = cursor..EOL, 1 = BOL..cursor, 2 = whole line */
static void vt_erase_line(int mode)
{
    int i = 0;
    if (mode == 0) {
        if (T.col < T.cur.len) {
            T.cur.len = T.col;
            T.cur.b[T.cur.len] = 0;
            T.cur.wid = T.col;
        }
        return;
    }
    if (mode == 1) {
        while (i < T.cur.len && i < T.col)
            i++;
        if (i > 0) {
            memmove(T.cur.b, T.cur.b + i, (size_t)(T.cur.len - i));
            T.cur.len -= i;
            T.cur.b[T.cur.len] = 0;
            T.cur.wid = T.cur.wid > i ? T.cur.wid - i : 0;
        }
        return;
    }
    line_reset(&T.cur);
    T.col = 0;
}

/* erase in display: mode 2 = whole screen (scrollback stays, like a real
 * terminal); other modes have no grid effect. */
static void vt_erase_screen(int mode)
{
    if (mode == 2 || mode == 3)
        line_reset(&T.cur);
}

static void vt_enter_alt(void)
{
    vt_newline();
    T.in_alt = 1;
    T.alt.head = 0;
    T.alt.n = 0;
}

static void vt_exit_alt(void)
{
    vt_newline();
    T.in_alt = 0;
    T.alt.head = 0;
    T.alt.n = 0;
}

/* ---- ANSI parser ------------------------------------------------------ */

struct parser {
    int state;       /* 0 text, 1 esc, 2 csi, 3 osc */
    int params[16];
    int nparams;
    int cur;         /* current param value */
    int collecting;  /* inside param collection */
    int priv;        /* CSI private marker '?' */
    int osclen;
};

static struct parser P;

static void p_reset(struct parser *p)
{
    p->state = 0;
    p->nparams = 0;
    p->cur = 0;
    p->collecting = 0;
    p->priv = 0;
}

static void p_start_csi(struct parser *p)
{
    p->state = 2;
    p->nparams = 0;
    p->cur = 0;
    p->collecting = 0;
    p->priv = 0;
}

static void p_param(struct parser *p, int final)
{
    (void)final;
    if (p->collecting) {
        if (p->nparams < 16)
            p->params[p->nparams++] = p->cur;
        p->cur = 0;
        p->collecting = 0;
    }
}

static void p_digit(struct parser *p, int d)
{
    p->cur = p->cur * 10 + d;
    if (p->cur > 99999)
        p->cur = 99999;
    p->collecting = 1;
}

static int p_get(const struct parser *p, int i, int def)
{
    return i < p->nparams ? p->params[i] : def;
}

static void sgr_apply(struct parser *p)
{
    int n = p->nparams;
    for (int i = 0; i < n; i++) {
        int a = p->params[i];
        switch (a) {
        case 0:
            T.cur.style = 0;
            T.cur.fg = -1;
            T.cur.bg = -1;
            break;
        case 1:
            T.cur.style |= ST_BOLD;
            break;
        case 2:
            T.cur.style |= ST_DIM;
            break;
        case 4:
            T.cur.style |= ST_UNDER;
            break;
        case 5:
            T.cur.style |= ST_BLINK;
            break;
        case 7:
            T.cur.style |= ST_REV;
            break;
        case 22:
            T.cur.style &= ~(ST_BOLD | ST_DIM);
            break;
        case 24:
            T.cur.style &= ~ST_UNDER;
            break;
        case 25:
            T.cur.style &= ~ST_BLINK;
            break;
        case 27:
            T.cur.style &= ~ST_REV;
            break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            T.cur.fg = a - 30;
            break;
        case 39:
            T.cur.fg = -1;
            break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            T.cur.fg = a - 90 + 8;
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            T.cur.bg = a - 40;
            break;
        case 49:
            T.cur.bg = -1;
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            T.cur.bg = a - 100 + 8;
            break;
        case 38: case 48: {
            int isfg = (a == 38);
            int mode = p_get(p, i + 1, 0);
            if (mode == 5 && i + 2 < n) {
                int c = p->params[i + 2];
                if (isfg)
                    T.cur.fg = c;
                else
                    T.cur.bg = c;
                i += 2;
            } else if (mode == 2 && i + 4 < n) {
                uint32_t r = (uint32_t)(p->params[i + 2] & 0xFF);
                uint32_t g = (uint32_t)(p->params[i + 3] & 0xFF);
                uint32_t b = (uint32_t)(p->params[i + 4] & 0xFF);
                if (isfg) {
                    T.cur.fg = -2;
                    T.cur.fg24 = (r << 16) | (g << 8) | b;
                } else {
                    T.cur.bg = -2;
                    T.cur.bg24 = (r << 16) | (g << 8) | b;
                }
                i += 4;
            }
            break;
        }
        default:
            break;
        }
    }
}

static void csi_exec(struct parser *p, int final)
{
    /* ignore private modes except the alternate-screen enter/leave */
    if (p->priv) {
        int mode = p_get(p, 0, 0);
        if ((mode == 1049 || mode == 47 || mode == 1047) &&
            (final == 'h' || final == 'l')) {
            if (final == 'h')
                vt_enter_alt();
            else
                vt_exit_alt();
        }
        return;
    }
    switch (final) {
    case 'm':
        sgr_apply(p);
        break;
    case 'A':
        break;
    case 'B':
        break;
    case 'C': {
        int n = p_get(p, 0, 1);
        T.col += n > 0 ? n : 1;
        if (T.col < 0)
            T.col = 0;
        break;
    }
    case 'D': {
        int n = p_get(p, 0, 1);
        T.col -= n > 0 ? n : 1;
        if (T.col < 0)
            T.col = 0;
        break;
    }
    case 'G':
    case '`': {
        int c = p_get(p, 0, 1);
        T.col = c > 0 ? c - 1 : 0;
        if (T.col > T.cols)
            T.col = T.cols;
        break;
    }
    case 'H':
    case 'f': {
        int c = p_get(p, 1, 1);
        T.col = c > 0 ? c - 1 : 0;
        if (T.col > T.cols)
            T.col = T.cols;
        break;
    }
    case 'J':
        vt_erase_screen(p_get(p, 0, 0));
        break;
    case 'K':
        vt_erase_line(p_get(p, 0, 0));
        break;
    case 'n':
    case 'R':
    case 'd':
    case 'X':
    case 'Z':
    case 'L':
    case 'M':
    case 'P':
    case '@':
    case 'T':
    case 'S':
    case 'c':
    case 'g':
    case 'q':
    case 't':
        break; /* no grid effect */
    default:
        break;
    }
}

/* feed one output byte into the tracker; returns 0 when the byte is fully
 * consumed, 1 when the parser still needs more bytes. */
static int vt_feed(unsigned char c)
{
    if (P.state == 3) { /* OSC: consume until BEL or ST */
        if (c == 0x07) {
            p_reset(&P);
            return 0;
        }
        if (c == 0x1B)
            P.state = 1; /* will be discarded by ESC terminator handling */
        return 0;
    }
    if (P.state == 2) { /* CSI */
        if (c >= 0x40 && c <= 0x7E) {
            p_param(&P, 1);
            if (c == '~')
                P.state = 0;
            csi_exec(&P, c);
            p_reset(&P);
        } else if (c == '?') {
            P.priv = 1;
        } else if (c == ';') {
            p_param(&P, 0);
        } else if (c >= '0' && c <= '9') {
            p_digit(&P, c - '0');
        } else if (c == 0x1B) {
            p_reset(&P);
            P.state = 1;
        } else {
            p_reset(&P);
        }
        return 0;
    }
    if (P.state == 1) { /* ESC */
        if (c == '[') {
            p_start_csi(&P);
            return 0;
        }
        if (c == ']') {
            P.state = 3;
            P.osclen = 0;
            return 0;
        }
        if (c == '7' || c == '8' || c == 'M' || c == 'D' || c == 'E' ||
            c == 'H' || c == 'c') {
            /* DECSC/DECRC/RI/IND/NEL/HTS/RS: no grid effect in v1 */
            p_reset(&P);
            return 0;
        }
        p_reset(&P);
        return 0;
    }
    /* text */
    if (c == 0x1B) {
        P.state = 1;
        return 0;
    }
    if (c == '\n' || c == 0x0B || c == 0x0C) {
        vt_newline();
        if (c == 0x0C)
            T.col = 0;
        return 0;
    }
    if (c == '\r') {
        T.col = 0;
        return 0;
    }
    if (c == '\b') {
        if (T.col > 0)
            T.col--;
        return 0;
    }
    if (c == '\t') {
        vt_char('\t');
        return 0;
    }
    if (c == 0x07 || c == 0x01 || c == 0x02 || c == 0x04) {
        return 0; /* BEL etc: no grid effect */
    }
    if (c < 0x20) {
        return 0;
    }
    if (c < 0x80) {
        vt_char(c);
        return 0;
    }
    uint32_t cp;
    int k = utf8_decode(&c, 1, &cp);
    if (k == 0) {
        vt_char(0xFFFD); /* invalid byte: replacement */
        return 0;
    }
    return 1; /* multi-byte start: caller must buffer */
}

/* feed a full multi-byte sequence; n >= 2 */
static void vt_feed_seq(const unsigned char *s, int n)
{
    uint32_t cp = 0;
    int k = utf8_decode(s, n, &cp);
    if (k <= 0 || k > n) {
        vt_char(0xFFFD);
        return;
    }
    if (k < n) {
        /* sequence was longer than what we have: consume what decodes,
         * re-feed the tail as plain bytes */
        vt_char(cp);
        for (int i = k; i < n; i++)
            vt_feed(s[i]);
        return;
    }
    vt_char(cp);
}

/* ---- viewer ----------------------------------------------------------- */

static int viewer_active;
static int view_top;   /* ring index of the first visible line */
static int view_rows;

static void viewer_render(void)
{
    int rows = view_rows > 1 ? view_rows - 1 : view_rows;
    int total = T.in_alt ? ring_count(&T.alt) : ring_count(&T.ring);
    if (total < rows)
        rows = total;
    int bottom = view_top + rows;
    if (bottom > total) {
        bottom = total;
        view_top = total - rows;
        if (view_top < 0)
            view_top = 0;
    }
    struct ring *r = T.in_alt ? &T.alt : &T.ring;
    /* clear + home */
    write(1, CSI "2J" CSI "H", 8);
    for (int i = 0; i < rows; i++) {
        struct line *l = ring_at(r, view_top + i);
        char pos[32];
        int pl = snprintf(pos, sizeof(pos), CSI "%d;1H", i + 1);
        write(1, pos, (size_t)pl);
        char style[64];
        int sl = 0;
        style[sl++] = 0x1B;
        style[sl++] = '[';
        int sel = (i == rows - 1 && view_top + rows >= total);
        if (l && sel)
            style[sl++] = '7';
        if (l && l->fg == -2) {
            sl += snprintf(style + sl, sizeof(style) - (size_t)sl,
                           "%s38;2;%u;%u;%um",
                           sel ? ";" : "", (l->fg24 >> 16) & 0xFF,
                           (l->fg24 >> 8) & 0xFF, l->fg24 & 0xFF);
        } else if (l && l->fg >= 0 && l->fg <= 15) {
            sl += snprintf(style + sl, sizeof(style) - (size_t)sl, "%s%dm",
                           sel ? ";" : "",
                           l->fg < 8 ? 30 + l->fg : 90 + l->fg - 8);
        } else if (l && l->fg > 0) {
            sl += snprintf(style + sl, sizeof(style) - (size_t)sl, "%s38;5;%dm",
                           sel ? ";" : "", l->fg);
        } else {
            style[sl++] = '0';
        }
        style[sl++] = 'm';
        write(1, style, (size_t)sl);
        if (l)
            write(1, l->b, (size_t)l->len);
        write(1, CSI "0m", 4);
    }
    /* status line */
    {
        char st[96];
        int sl = snprintf(st, sizeof(st),
                          CSI "%d;1H" C_DIM "bterm scrollback %d-%d/%d  "
                          "[u/d or PgUp/PgDn: page, k/j: line, q: live]" C_RESET,
                          view_rows, view_top + (rows ? 1 : 0), total, total);
        write(1, st, (size_t)sl);
    }
    write(1, CSI "?25h", 5);
}

/* redraw the live screen from the tail of the scrollback (after viewer
 * exits), so the user is back where they were. */
static void viewer_exit(void)
{
    int total = T.in_alt ? ring_count(&T.alt) : ring_count(&T.ring);
    int rows = view_rows > 0 ? view_rows : 24;
    view_top = total - rows;
    if (view_top < 0)
        view_top = 0;
    viewer_render();
    write(1, CSI "H", 3);
    viewer_active = 0;
}

/* ---- signals ---------------------------------------------------------- */

static volatile sig_atomic_t g_sigwinch;
static volatile sig_atomic_t g_sigchld;
static volatile sig_atomic_t g_term;   /* signal number to forward+exit */

static pid_t child_pid;

static void on_sigwinch(int sig)
{
    (void)sig;
    g_sigwinch = 1;
}

static void on_sigchld(int sig)
{
    (void)sig;
    g_sigchld = 1;
}

static void on_term(int sig)
{
    g_term = sig;
}

static void install_sig(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
}

/* ---- terminal mode ---------------------------------------------------- */

static struct termios saved_tio;
static int tio_saved;

static void term_restore(void)
{
    if (tio_saved) {
        tcsetattr(0, TCSAFLUSH, &saved_tio);
        tio_saved = 0;
    }
}

static void term_raw(void)
{
    if (tcgetattr(0, &saved_tio) == 0) {
        struct termios t = saved_tio;
        t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                       ICRNL | IXON);
        t.c_oflag &= ~OPOST;
        t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        t.c_cflag &= ~(CSIZE | PARENB);
        t.c_cflag |= CS8;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(0, TCSANOW, &t) == 0)
            tio_saved = 1;
    }
}

/* push our terminal size into the pty */
static void push_size(int master, int init)
{
    struct winsize ws;
    int fd = isatty(1) ? 1 : (isatty(0) ? 0 : -1);
    if (fd >= 0 && ioctl(fd, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_row > 0 && ws.ws_col > 0) {
        /* real terminal size */
    } else if (init) {
        /* serial console or no size: use the conventional 80x24 */
        ws.ws_row = 24;
        ws.ws_col = 80;
    } else {
        return;
    }
    ioctl(master, TIOCSWINSZ, &ws);
    if (init) {
        T.cols = ws.ws_col > 0 ? ws.ws_col : 80;
        T.col = 0;
    }
}

/* ---- child ------------------------------------------------------------ */

static void child_exec(int slave, char *const argv[])
{
    setsid();
    ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
        close(slave);
    execvp(argv[0], argv);
    _exit(127);
}

/* ---- viewer input ----------------------------------------------------- */

/* returns 1 when the byte(s) were consumed by the viewer */
static int viewer_input(unsigned char *buf, int n, int master)
{
    /* peek one byte: 0x01 = Ctrl+A (exit), 0x71 'q', 0x75 'u', 0x64 'd',
     * 0x6B 'k', 0x6A 'j', 0x0D Enter, 0x1A Ctrl+Z, CSI page keys */
    unsigned char c = buf[0];
    if (c == 0x01 || c == 'q' || c == 0x0D || c == 0x1A) {
        viewer_exit();
        return 1;
    }
    if (c == 'u') {
        view_top -= view_rows;
        if (view_top < 0)
            view_top = 0;
        viewer_render();
        return 1;
    }
    if (c == 'd') {
        view_top += view_rows;
        viewer_render();
        return 1;
    }
    if (c == 'k' && n == 1) {
        if (view_top > 0)
            view_top--;
        viewer_render();
        return 1;
    }
    if (c == 'j' && n == 1) {
        view_top++;
        viewer_render();
        return 1;
    }
    if (c == 0x1B && n >= 3 && buf[1] == '[' && buf[2] == '5' && n == 3) {
        view_top -= view_rows;
        if (view_top < 0)
            view_top = 0;
        viewer_render();
        return 1;
    }
    if (c == 0x1B && n >= 3 && buf[1] == '[' && buf[2] == '6' && n == 3) {
        view_top += view_rows;
        viewer_render();
        return 1;
    }
    /* anything else: exit the viewer and forward the input */
    viewer_exit();
    write(master, buf, (size_t)n);
    return 1;
}

/* prefix mode: Ctrl+A is held pending; the next byte decides */
static int prefix_pending;

/* enter the scrollback viewer; at_top=1 starts at the oldest page,
 * at_top=0 starts at the newest page */
static void viewer_enter(int at_top)
{
    struct winsize ws;
    view_rows = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1)
        view_rows = ws.ws_row;
    int total = T.in_alt ? ring_count(&T.alt) : ring_count(&T.ring);
    view_top = at_top ? 0 : total - view_rows;
    if (view_top < 0)
        view_top = 0;
    viewer_active = 1;
    viewer_render();
}

/* returns bytes to forward to the pty (0..n); consumed prefix bytes are
 * dropped.  *viewer_entered set when the viewer is now active. */
static int live_input(unsigned char *buf, int n, int master, int *entered)
{
    *entered = 0;
    if (prefix_pending) {
        prefix_pending = 0;
        unsigned char c = buf[0];
        if (c == 0x01 || c == 'u' || c == 'd') { /* Ctrl+A / u / d */
            viewer_enter(0);
            *entered = 1;
            return 1;
        }
        if (c == 0x1B && n >= 3 && buf[1] == '[') {
            if (buf[2] == '5' || buf[2] == '6') { /* PgUp / PgDn */
                viewer_enter(buf[2] == '5');
                *entered = 1;
                return 1;
            }
            return n >= 3 ? 2 : 0; /* swallow ESC [, forward the rest */
        }
        /* plain Ctrl+A followed by anything else: forward both */
        if (write(master, "\x01", 1) != 1)
            return 1;
        return 0; /* forward the rest unchanged */
    }
    if (buf[0] == 0x01) {
        if (n >= 2) {
            unsigned char c = buf[1];
            if (c == 0x01 || c == 'u' || c == 'd') {
                viewer_enter(0);
                *entered = 1;
                return n;
            }
            if (c == 0x1B && n >= 4 && buf[2] == '[' &&
                (buf[3] == '5' || buf[3] == '6')) {
                viewer_enter(buf[3] == '5');
                *entered = 1;
                return n;
            }
            /* not a viewer key: forward the Ctrl+A, then the rest */
            if (write(master, "\x01", 1) != 1)
                return n;
            return 1;
        }
        prefix_pending = 1;
        return 1; /* hold it for the next byte */
    }
    return 0;
}

/* ---- main ------------------------------------------------------------- */

static void usage(FILE *f)
{
    fprintf(f,
            "usage: bterm [-e <program> [args...]]\n"
            "  -e <program> [args...]   run the given program in the pty\n"
            "                           (default: /bin/bsh)\n"
            "  --help                   show this help\n"
            "  --version                show version\n\n"
            "Ctrl+A viewer: Ctrl+A Ctrl+A (or Ctrl+A u/PgUp) enters the\n"
            "scrollback viewer; u/d/PgUp/PgDn scroll, q/Enter/Ctrl+A exits.\n"
            "Config: /etc/bterm.conf, ~/.config/bterm/config\n");
}

int main(int argc, char **argv)
{
    char *user_cfg = NULL;
    char *prog = NULL;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("%s %s\n", BT_NAME, BT_VER);
            printf("BulumOS\n");
            return 0;
        }
        if (!strcmp(argv[i], "-e")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bterm: -e requires a program\n");
                return 1;
            }
            prog = argv[i + 1];
            i++;
            break;
        }
        if (!strcmp(argv[i], "--config")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "bterm: --config requires a path\n");
                return 1;
            }
            user_cfg = argv[i + 1];
            i++;
            continue;
        }
        fprintf(stderr, "bterm: unknown option '%s' (see --help)\n", argv[i]);
        return 1;
    }
    char **child_argv;
    if (prog) {
        child_argv = argv + i; /* -e <prog> [args...] */
    } else {
        static char *sh[] = { (char *)"/bin/bsh", NULL };
        static char *sh_fallback[] = { (char *)"/bin/sh", NULL };
        child_argv = access("/bin/bsh", X_OK) == 0 ? sh : sh_fallback;
    }

    struct config cfg;
    cfg_load(&cfg, user_cfg);

    line_init(&T.cur);
    ring_init(&T.ring, cfg.scrollback);
    ring_init(&T.alt, 256);
    p_reset(&P);

    int master = -1, slave = -1;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        fprintf(stderr, "bterm: openpty: %s\n", strerror(errno));
        return 1;
    }
    push_size(master, 1);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "bterm: fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0)
        child_exec(slave, child_argv);
    child_pid = pid;
    close(slave);

    if (isatty(0)) {
        term_raw();
    }

    signal(SIGPIPE, SIG_IGN);
    install_sig(SIGWINCH, on_sigwinch);
    install_sig(SIGCHLD, on_sigchld);
    install_sig(SIGTERM, on_term);
    install_sig(SIGINT, on_term);
    install_sig(SIGQUIT, on_term);

    if (cfg.banner)
        write(1, "Bulum Terminal " BT_VER " - BulumOS\n",
              sizeof("Bulum Terminal " BT_VER " - BulumOS\n") - 1);

    int child_status = 0;
    int have_status = 0;

    for (;;) {
        if (g_sigwinch) {
            g_sigwinch = 0;
            push_size(master, 0);
            if (viewer_active) {
                struct winsize ws;
                if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1)
                    view_rows = ws.ws_row;
                viewer_render();
            }
        }
        if (g_sigchld) {
            g_sigchld = 0;
            int st = 0;
            pid_t r = waitpid(child_pid, &st, WNOHANG);
            if (r == child_pid) {
                child_status = st;
                have_status = 1;
                break;
            }
        }
        if (g_term) {
            int sig = (int)g_term;
            kill(child_pid, sig == SIGINT ? SIGINT : SIGTERM);
            /* wait briefly for the child to go away */
            for (int k = 0; k < 50; k++) {
                int st = 0;
                if (waitpid(child_pid, &st, WNOHANG) == child_pid) {
                    have_status = 1;
                    child_status = st;
                    break;
                }
                usleep(20000);
            }
            if (!have_status) {
                kill(child_pid, SIGKILL);
                waitpid(child_pid, NULL, 0);
            }
            term_restore();
            _exit(128 + sig);
        }

        struct pollfd pf[2];
        pf[0].fd = master;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = 0;
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        int nfd = poll(pf, 2, -1);
        if (nfd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pf[0].revents & POLLIN) {
            unsigned char buf[8192];
            ssize_t n = read(master, buf, sizeof(buf));
            if (n <= 0)
                break; /* child closed the pty (EIO after exit) */
            ssize_t w = write(1, buf, (size_t)n);
            if (w < 0)
                break;
            /* track the stream (ASCII fast path) */
            for (ssize_t k = 0; k < n; k++) {
                unsigned char c = buf[k];
                if (c < 0x80) {
                    vt_feed(c);
                } else {
                    /* buffer a utf-8 sequence */
                    unsigned char seq[4];
                    int need = utf8_len(c);
                    if (need > 4)
                        need = 4;
                    seq[0] = c;
                    int have = 1;
                    while (have < need && k + 1 < n) {
                        seq[have++] = buf[++k];
                    }
                    if (have < need)
                        break; /* tail of buffer: next round */
                    vt_feed_seq(seq, have);
                }
            }
        } else if (pf[0].revents & (POLLHUP | POLLERR)) {
            break;
        }

        if (pf[1].revents & POLLIN) {
            unsigned char buf[8192];
            ssize_t n = read(0, buf, sizeof(buf));
            if (n <= 0)
                break; /* EOF on stdin: shut down */
            if (viewer_active) {
                viewer_input(buf, (int)n, master);
                continue;
            }
            int entered = 0;
            int consumed = live_input(buf, (int)n, master, &entered);
            if (entered)
                continue;
            if (consumed < n)
                if (write(master, buf + consumed, (size_t)(n - consumed)) < 0)
                    break;
        }
    }

    /* drain any remaining child */
    if (!have_status) {
        int st = 0;
        if (waitpid(child_pid, &st, 0) == child_pid) {
            child_status = st;
            have_status = 1;
        }
    }
    term_restore();

    if (have_status && WIFEXITED(child_status))
        return WEXITSTATUS(child_status);
    if (have_status && WIFSIGNALED(child_status))
        return 128 + WTERMSIG(child_status);
    return 1;
}
