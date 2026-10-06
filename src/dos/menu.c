/*
 * VM メニューとファイル選択 (menu.h、design.md §9)。ホスト世界で動く。
 * 一覧は MS-DOS のファイル検索で作り、ディレクトリを先に名前順に並べる。サブディレクトリへは
 * カレントディレクトリを移して入る (スクリーンショットの保存先も移る。spec.md)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dos.h>
#include <direct.h>
#include "vbm.h"
#include "ui.h"
#include "menu.h"
#include "ui_text.h"

#define MAX_FILES 200
#define LIST_TOP  3
#define LIST_ROWS 19

struct entry {
    char name[13];
    u8 dir;
};

static struct entry entries[MAX_FILES];
static int nentries;
static char msg[128];
static char cwd[80];

/* 一覧に出す拡張子 (spec.md)。MS-DOS のファイル名は大文字 */
static const char *const image_ext[] = { ".FDI", ".NFD", ".FDD", ".HDM", ".IMG" };

static int is_image(const char *name)
{
    const char *dot = strrchr(name, '.');
    unsigned i;

    if (!dot)
        return 0;
    for (i = 0; i < sizeof image_ext / sizeof image_ext[0]; i++)
        if (strcmp(dot, image_ext[i]) == 0)
            return 1;
    return 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;

    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void scan_dir(int all)
{
    struct find_t ft;
    unsigned rc;

    nentries = 0;
    rc = _dos_findfirst("*.*", _A_SUBDIR | _A_RDONLY | _A_ARCH, &ft);
    while (rc == 0 && nentries < MAX_FILES) {
        if (ft.attrib & _A_SUBDIR) {
            if (strcmp(ft.name, ".") != 0) {
                strcpy(entries[nentries].name, ft.name);
                entries[nentries].dir = 1;
                nentries++;
            }
        } else if (all || is_image(ft.name)) {
            strcpy(entries[nentries].name, ft.name);
            entries[nentries].dir = 0;
            nentries++;
        }
        rc = _dos_findnext(&ft);
    }
    _dos_findclose(&ft);
    qsort(entries, (size_t)nentries, sizeof entries[0], cmp_entry);
}

/* 下 2 行に知らせを出してキーを待つ */
static void notice(const char *s)
{
    ui_fill(22, 0, UI_COLS, 2, UI_WHITE);
    ui_puts(22, 2, UI_YELLOW, s);
    ui_puts(23, 2, UI_CYAN, T_MSG_ANYKEY);
    ui_getkey();
    ui_fill(22, 0, UI_COLS, 2, UI_WHITE);
}

/* 一覧の項目 0 は「取り出す」、1 以降が entries */
static void draw_list(int unit, int all, int cur, int top)
{
    const char *name = vm_drive_name(unit);
    int i;
    u8 row, attr;

    ui_fill(0, 0, UI_COLS, UI_ROWS, UI_WHITE);
    sprintf(msg, T_SEL_TITLE, unit, name[0] ? name : T_SEL_NONE);
    ui_puts(0, 2, UI_YELLOW, msg);
    if (getcwd(cwd, sizeof cwd))
        ui_puts(1, 2, UI_CYAN, cwd);
    for (i = top; i < top + LIST_ROWS && i <= nentries; i++) {
        attr = (u8)(i == cur ? (UI_WHITE | UI_REV) : UI_WHITE);
        row = (u8)(LIST_TOP + i - top);
        if (i == 0) {
            ui_puts(row, 4, attr, T_SEL_EJECT);
        } else if (entries[i - 1].dir && strcmp(entries[i - 1].name, "..") == 0) {
            ui_puts(row, 4, attr, T_SEL_UPDIR);
        } else {
            sprintf(msg, "%-12s %s", entries[i - 1].name, entries[i - 1].dir ? T_SEL_DIR : "");
            ui_puts(row, 4, attr, msg);
        }
    }
    if (nentries == 0)
        ui_puts(LIST_TOP + 1, 4, UI_CYAN, T_SEL_EMPTY);
    ui_puts(23, 2, UI_CYAN, all ? T_SEL_HELP_ALL : T_SEL_HELP_IMG);
}

/* ドライブ unit のイメージを選ばせる。入れ替えか取り出しをしたら 1 */
static int disk_select(int unit)
{
    int all = 0, cur = 0, top = 0, rescan = 1;
    u16 k;
    u8 sc, ch;

    for (;;) {
        if (rescan) {
            scan_dir(all);
            rescan = 0;
            if (cur > nentries)
                cur = nentries;
        }
        if (cur < top)
            top = cur;
        if (cur >= top + LIST_ROWS)
            top = cur - LIST_ROWS + 1;
        draw_list(unit, all, cur, top);
        k = ui_getkey();
        sc = UI_KEY_SC(k);
        ch = UI_KEY_CH(k);
        if (sc == UI_SC_ESC || ch == 0x1B)
            return 0;
        if (sc == UI_SC_UP) {
            if (cur > 0)
                cur--;
        } else if (sc == UI_SC_DOWN) {
            if (cur < nentries)
                cur++;
        } else if (sc == UI_SC_ROLLUP) {
            /* ROLL UP は内容を上へ送る = 次のページ (PC-98 の慣例) */
            cur = cur + LIST_ROWS <= nentries ? cur + LIST_ROWS : nentries;
        } else if (sc == UI_SC_ROLLDOWN) {
            cur = cur >= LIST_ROWS ? cur - LIST_ROWS : 0;
        } else if (sc == UI_SC_TAB || ch == 0x09) {
            all = !all;
            cur = top = 0;
            rescan = 1;
        } else if (sc == UI_SC_RETURN || ch == 0x0D) {
            if (cur == 0) {
                vm_eject(unit);
                sprintf(msg, T_MSG_EJECTED, unit);
                notice(msg);
                return 1;
            }
            if (entries[cur - 1].dir) {
                if (chdir(entries[cur - 1].name) == 0) {
                    cur = top = 0;
                    rescan = 1;
                }
            } else if (vm_mount(unit, entries[cur - 1].name, 1) == 0) {
                sprintf(msg, T_MSG_MOUNTED, unit, entries[cur - 1].name);
                notice(msg);
                return 1;
            } else {
                sprintf(msg, T_MSG_MOUNT_FAIL, entries[cur - 1].name);
                notice(msg);
            }
        }
    }
}

int menu_debug;

static void draw_main(void)
{
    ui_fill(0, 0, UI_COLS, UI_ROWS, UI_WHITE);
    ui_box(6, 18, 44, 11, UI_CYAN);
    ui_puts(7, 21, UI_YELLOW, T_MENU_TITLE);
    ui_puts(9, 22, UI_WHITE, T_MENU_FDD0);
    ui_puts(10, 22, UI_WHITE, T_MENU_FDD1);
    ui_puts(11, 22, UI_WHITE, T_MENU_SHOT);
    ui_puts(12, 22, UI_WHITE, T_MENU_EXIT);
    ui_puts(14, 21, UI_CYAN, T_MENU_HELP);
    if (menu_debug) {
        ui_debug_dump(6, 30);
        ui_debug_dump(7, 30);
        ui_debug_dump(9, 48);
    }
}

static int confirm_exit(void)
{
    u16 k;
    u8 sc, ch;

    ui_fill(18, 0, UI_COLS, 1, UI_WHITE);
    ui_puts(18, 2, UI_YELLOW, T_CONFIRM_EXIT);
    for (;;) {
        k = ui_getkey();
        sc = UI_KEY_SC(k);
        ch = UI_KEY_CH(k);
        if (sc == UI_SC_Y || ch == 'y' || ch == 'Y')
            return 1;
        if (sc == UI_SC_N || ch == 'n' || ch == 'N' || sc == UI_SC_ESC || ch == 0x1B)
            return 0;
    }
}

/* 撮るのはゲストの画面なので、いったん戻してから撮る */
static void do_shot(void)
{
    char name[13];
    int rc;

    ui_close();
    rc = vm_shot(name);
    ui_open();
    draw_main();
    if (rc) {
        notice(T_MSG_SHOT_FAIL);
    } else {
        sprintf(msg, T_MSG_SHOT_OK, name);
        notice(msg);
    }
}

int menu_main(void)
{
    int rc = MENU_RESUME;
    u16 k;
    u8 sc, ch;

    ui_open();
    ui_flush_keys();
    for (;;) {
        draw_main();
        k = ui_getkey();
        sc = UI_KEY_SC(k);
        ch = UI_KEY_CH(k);
        if (sc == UI_SC_ESC || ch == 0x1B)
            break;
        if (sc == UI_SC_1 || ch == '1')
            disk_select(0);
        else if (sc == UI_SC_2 || ch == '2')
            disk_select(1);
        else if (sc == UI_SC_3 || ch == '3')
            do_shot();
        else if ((sc == UI_SC_4 || ch == '4') && confirm_exit()) {
            rc = MENU_EXIT;
            break;
        }
    }
    ui_close();
    return rc;
}

void menu_disk(int unit)
{
    ui_open();
    ui_flush_keys();
    disk_select(unit);
    ui_close();
}

int menu_confirm_exit(void)
{
    int r;

    ui_open();
    ui_flush_keys();
    r = confirm_exit();
    ui_close();
    return r;
}

int menu_pick_boot(void)
{
    int r;

    ui_open();
    ui_flush_keys();
    disk_select(0);
    r = vm_drive_name(0)[0] != 0;
    ui_close();
    return r;
}
