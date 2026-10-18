/* Now Playing text: romanization to printable ASCII and the "..." cut. */
#include "spotify_bridge.c"
#include <stdio.h>
#include <string.h>

void log_append(const char *message) { (void)message; }
void log_append_value(const char *label, int value) { (void)label; (void)value; }

static int fails;
static void check(const char *in, const char *want, size_t cap) {
    char out[256];
    romanize_text(in, out, cap);
    if (strcmp(out, want)) {
        printf("FAIL [%s] -> [%s], want [%s]\n", in, out, want);
        fails++;
    }
}
static void check_ell(const char *in, size_t max, const char *want) {
    char buf[256];
    strcpy(buf, in);
    ellipsize(buf, max);
    if (strcmp(buf, want)) {
        printf("FAIL ellipsize [%s,%zu] -> [%s], want [%s]\n", in, max, buf, want);
        fails++;
    }
}

int main(void) {
    check("Кино", "Kino", 256);
    check("Виктор Цой", "Viktor Tsoy", 256);
    check("Щука ЖУК", "Shchuka ZhUK", 256);
    check("Ёлка, Їжак, Ґанок, Ўсё, Єва, Ірина", "Yolka, Yizhak, Ganok, Usyo, Yeva, Irina", 256);
    check("Ђорђе Јовановић, Љубав, Њујорк", "Djordje Jovanovic, Ljubav, Njujork", 256);
    check("AC/DC & Björk", "AC/DC & Bjork", 256);
    check("방탄소년단", "Bangtansonyeondan", 256);
    check("아이유 (IU)", "Aiyu (IU)", 256);
    check("ヨアソビ", "Yoasobi", 256);
    check("ラーメン", "Ramen", 256);
    check("しゃしん ちゃ じゃ", "Shashin Cha Ja", 256);
    check("がっこう マッチ", "Gakkou Matchi", 256);
    check("ファン ティー", "Fan Ti", 256);
    check("ヨル・シカ", "Yoru Shika", 256);
    check("夜に駆ける", "Ye Ni Qu Keru", 256);
    check("周杰伦 - 晴天", "Zhou Jie Lun - Qing Tian", 256);
    check("Jay Chou周杰伦", "Jay Chou Zhou Jie Lun", 256);
    check("五月天 (Mayday)", "Wu Yue Tian (Mayday)", 256);
    check("ワールドイズマイン-初音ミク「マジカルミライ 2021」Live- (feat. 初音ミク)",
          "Warudoizumain-Chu Yin Miku\"Majikarumirai 2021\"Live- (feat. Chu Yin Miku)", 256);
    check("Beyoncé, Sigur Rós, Mötley Crüe, Straße, Łódź", "Beyonce, Sigur Ros, Motley Crue, Strasse, Lodz", 256);
    check("Don’t Stop — “Live” … ＡＢＣ１２３！", "Don't Stop - \"Live\" ... ABC123!", 256);
    check("【MV】 Song 🔥🎵", "[MV] Song ", 256);
    check("علي رمش عيونها", "Ali Rmsh Ayunha", 256);
    check("وديع الصافي", "Wdia Alsafi", 256);
    check("گوگوش", "Gugush", 256);
    check("محسن چاوشی", "Mhsn Chawshi", 256);
    check("عَلِيّ ٢٠٢١ ۱۴۰۰؟", "Ali 2021 1400?", 256);
    check("Μίκης Θεοδωράκης", "Mikis Theodorakis", 256);
    check("שָׁלוֹם עליכם", "Shalvom Lykhm", 256);
    check("© 2024 ™ €5", "(c) 2024 (tm) EUR5", 256);
    check("Björk", "Bjor", 5);
    check("€€", "EUR", 4);
    check("\xff\xfeok", "ok", 256);
    check_ell("Short", 80, "Short");
    check_ell("abcdefghij", 10, "abcdefghij");
    check_ell("abcdefghijk", 10, "abcdefg...");
    check_ell("abcdef  hijk", 10, "abcdef...");
    /* Every BMP character, between letters, must come out as printable ASCII only. */
    for (unsigned int cp = 0x80; cp <= 0xffff; ++cp) {
        if (cp >= 0xd800 && cp <= 0xdfff) {
            continue;
        }
        char in[16], out[256];
        int k = 0;
        in[k++] = 'x';
        if (cp < 0x800) {
            in[k++] = (char)(0xc0 | (cp >> 6));
            in[k++] = (char)(0x80 | (cp & 0x3f));
        } else {
            in[k++] = (char)(0xe0 | (cp >> 12));
            in[k++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            in[k++] = (char)(0x80 | (cp & 0x3f));
        }
        in[k++] = 'y';
        in[k] = 0;
        romanize_text(in, out, sizeof(out));
        for (const char *p = out; *p; ++p) {
            if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) {
                printf("FAIL non-ASCII output for U+%04X\n", cp);
                fails++;
                break;
            }
        }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
