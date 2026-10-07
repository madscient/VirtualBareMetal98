#ifndef LOG_H
#define LOG_H

/*
 * 診断ログ (spec.md の -log、design.md §19)。ホスト世界で書く。
 * 実機ではゲストが画面を使うので標準出力の診断が読めず、止まったときに何も残らない。そこで本体の表示を
 * ファイルにも書き、1 行ごとに DOS に書き戻させる (ハングしてもそこまでの行は残る)。
 */

/* ログファイルを開く (既存なら末尾に追記)。0 で成功 */
int log_open(const char *path);
void log_close(void);
int log_is_open(void);
/* 標準出力と (開いていれば) ログの両方に書く。printf と同じ書式。1 行は 255 文字まで */
void say(const char *fmt, ...);
/* ログだけに書く (開いていなければ捨てる)。ゲストの画面を汚したくないもの (心拍) に使う */
void log_line(const char *fmt, ...);

#endif
