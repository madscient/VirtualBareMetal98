#include "optval.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int optval_hexmask(const char *s, u8 *val, u8 *hostmask, int n)
{
    int i, k, d;
    u8 v, m;

    for (i = 0; i < n; i++) {
        v = 0;
        m = 0;
        for (k = 0; k < 2; k++) {
            char c = s[i * 2 + k];

            if (c == '*') {
                m = (u8)(m << 4 | 0x0F);
                v = (u8)(v << 4);
            } else if ((d = hexval(c)) >= 0) {
                m = (u8)(m << 4);
                v = (u8)(v << 4 | d);
            } else {
                return 1;
            }
        }
        val[i] = v;
        hostmask[i] = m;
    }
    return s[n * 2] != 0;
}

u8 optval_merge(u8 val, u8 host, u8 hostmask)
{
    return (u8)((val & ~hostmask) | (host & hostmask));
}

/* 16 進 1〜4 桁を読む。読んだ文字数を返し、0 なら誤り */
static int parse_port(const char *s, u16 *port)
{
    u16 v = 0;
    int i, d;

    for (i = 0; i < 4 && (d = hexval(s[i])) >= 0; i++)
        v = (u16)((v << 4) | (u16)d);
    if (i == 0 || hexval(s[i]) >= 0)
        return 0;
    *port = v;
    return i;
}

static int add(u16 g, u16 h, u16 *guest, u16 *host, u8 *n, u8 max)
{
    u8 i;

    for (i = 0; i < *n; i++)
        if (guest[i] == g) {
            host[i] = h;
            return 0;
        }
    if (*n >= max)
        return 1;
    guest[*n] = g;
    host[*n] = h;
    (*n)++;
    return 0;
}

int optval_iotrap_list(const char *s, u16 *guest, u16 *host, u8 *n, u8 max)
{
    u16 g, h;
    int len;

    if (*s == 0)
        return 1;
    for (;;) {
        if ((len = parse_port(s, &g)) == 0)
            return 1;
        s += len;
        if (*s++ != '=')
            return 1;
        if ((len = parse_port(s, &h)) == 0)
            return 1;
        s += len;
        if (add(g, h, guest, host, n, max))
            return 1;
        if (*s == 0)
            return 0;
        if (*s++ != ',')
            return 1;
    }
}

static int blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int optval_iotrap_line(const char *line, u16 *guest, u16 *host, u8 *n, u8 max)
{
    u16 g, h;
    int len;

    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == 0 || *line == '\r' || *line == '\n')
        return 0;
    if ((len = parse_port(line, &g)) == 0)
        return 1;
    line += len;
    if (!(*line == ' ' || *line == '\t'))
        return 1;
    while (*line == ' ' || *line == '\t')
        line++;
    if ((len = parse_port(line, &h)) == 0)
        return 1;
    line += len;
    while (blank(*line))
        line++;
    if (*line != 0)
        return 1;
    return add(g, h, guest, host, n, max);
}
