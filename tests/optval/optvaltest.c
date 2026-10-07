/*
 * optval.c の試験: -dipsw / -memsw の '*' 付きの 16 進と、-iotrap の一覧・定義ファイルの行の読み取り。
 * 期待値は spec.md の書式から。
 */
#include <stdio.h>
#include <string.h>
#include "optval.h"

static int fails, count;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        fails++;
    count++;
}

static void hexmask(void)
{
    u8 v[8], m[8];

    memset(v, 0xAA, sizeof v);
    memset(m, 0xAA, sizeof m);
    check("hexmask: plain hex", optval_hexmask("c1F4fb", v, m, 3) == 0 && v[0] == 0xC1 && v[1] == 0xF4 && v[2] == 0xFB &&
          m[0] == 0 && m[1] == 0 && m[2] == 0);
    check("hexmask: '*' marks the nibble (value 0, mask F)", optval_hexmask("*1F*", v, m, 2) == 0 &&
          v[0] == 0x01 && m[0] == 0xF0 && v[1] == 0xF0 && m[1] == 0x0F);
    check("hexmask: all '*' for 8 bytes", optval_hexmask("****************", v, m, 8) == 0 && v[7] == 0 && m[7] == 0xFF && m[0] == 0xFF);
    check("hexmask: too short", optval_hexmask("C1F4F", v, m, 3) != 0);
    check("hexmask: too long", optval_hexmask("C1F4FB0", v, m, 3) != 0);
    check("hexmask: bad character", optval_hexmask("C1G4FB", v, m, 3) != 0);
    check("merge: host nibbles replace the masked ones", optval_merge(0x01, 0x48, 0xF0) == 0x41 &&
          optval_merge(0x08, 0x6E, 0x00) == 0x08 && optval_merge(0x00, 0x6E, 0xFF) == 0x6E);
}

static void iotrap(void)
{
    u16 g[4], h[4];
    u8 n;

    n = 0;
    check("list: one pair", optval_iotrap_list("188=288", g, h, &n, 4) == 0 && n == 1 && g[0] == 0x188 && h[0] == 0x288);
    check("list: several pairs, 1 to 4 hex digits, lower case", optval_iotrap_list("18a=28A,f31=31,1=FFFF", g, h, &n, 4) == 0 &&
          n == 4 && g[1] == 0x18A && h[1] == 0x28A && g[2] == 0xF31 && h[2] == 0x31 && g[3] == 1 && h[3] == 0xFFFF);
    check("list: same guest port again replaces the host port", optval_iotrap_list("188=388", g, h, &n, 4) == 0 && n == 4 && h[0] == 0x388);
    check("list: table full", optval_iotrap_list("200=300", g, h, &n, 4) != 0 && n == 4);
    n = 0;
    check("list: empty", optval_iotrap_list("", g, h, &n, 4) != 0);
    check("list: missing '='", optval_iotrap_list("188", g, h, &n, 4) != 0);
    check("list: missing host port", optval_iotrap_list("188=", g, h, &n, 4) != 0);
    check("list: trailing comma", optval_iotrap_list("188=288,", g, h, &n, 4) != 0);
    check("list: five hex digits", optval_iotrap_list("10188=288", g, h, &n, 4) != 0);
    check("list: non-hex", optval_iotrap_list("18G=288", g, h, &n, 4) != 0);
    n = 0;
    check("line: space separated with CRLF", optval_iotrap_line("188 288\r\n", g, h, &n, 4) == 0 && n == 1 && g[0] == 0x188 && h[0] == 0x288);
    check("line: tabs and leading blanks, LF only", optval_iotrap_line("\t 18A\t28A \n", g, h, &n, 4) == 0 && n == 2 && g[1] == 0x18A && h[1] == 0x28A);
    check("line: no line end", optval_iotrap_line("F31 31", g, h, &n, 4) == 0 && n == 3 && g[2] == 0xF31);
    check("line: empty and blank lines add nothing", optval_iotrap_line("", g, h, &n, 4) == 0 && optval_iotrap_line("\r\n", g, h, &n, 4) == 0 &&
          optval_iotrap_line("   \n", g, h, &n, 4) == 0 && n == 3);
    check("line: only one port", optval_iotrap_line("188\r\n", g, h, &n, 4) != 0);
    check("line: third field", optval_iotrap_line("188 288 388\r\n", g, h, &n, 4) != 0);
    check("line: '=' is not a separator here", optval_iotrap_line("188=288\r\n", g, h, &n, 4) != 0);
    check("line: table full", optval_iotrap_line("1 2", g, h, &n, 4) == 0 && n == 4 && optval_iotrap_line("3 4", g, h, &n, 4) != 0);
}

int main(void)
{
    hexmask();
    iotrap();
    printf("END %d %d\n", fails, count);
    return fails != 0;
}
