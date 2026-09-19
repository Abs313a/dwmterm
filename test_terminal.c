#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

// Rename main so we can link and test main.c static internals
#define main dwmterm_main
#include "main.c"
#undef main

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_PASS() do { \
    printf("  [PASS] %s\n", __func__); \
    tests_passed++; \
} while(0)

static void feed_bytes(Terminal *t, const char *s) {
    size_t len = strlen(s);
    for (size_t i = 0; i < len; i++) {
        term_put_byte_internal(t, (unsigned char)s[i], 1);
    }
}

static void test_term_init(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    assert(term.cols == 80);
    assert(term.rows == 24);
    assert(term.primary_grid != NULL);
    assert(term.alt_grid != NULL);
    assert(term.grid == term.primary_grid);
    assert(term.is_alt_screen == 0);
    assert(term.cursor_x == 0);
    assert(term.cursor_y == 0);
    assert(term.cursor_visible == 1);
    assert(term.cur_fg == COLOR_FG);
    assert(term.cur_bg == COLOR_BG);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_decset_1049_alt_screen(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Initial state: primary screen
    assert(term.is_alt_screen == 0);
    assert(term.grid == term.primary_grid);

    // Enter alternate screen
    feed_bytes(&term, "\x1b[?1049h");
    assert(term.is_alt_screen == 1);
    assert(term.grid == term.alt_grid);
    assert(term.cursor_x == 0);
    assert(term.cursor_y == 0);

    // Write text into alternate buffer
    feed_bytes(&term, "ALT_BUFFER");
    assert(term.alt_grid[0].codepoint == 'A');
    assert(term.primary_grid[0].codepoint == ' '); // Primary grid untouched

    // Exit alternate screen
    feed_bytes(&term, "\x1b[?1049l");
    assert(term.is_alt_screen == 0);
    assert(term.grid == term.primary_grid);
    assert(term.primary_grid[0].codepoint == ' ');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_decset_1047_and_47(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // DECSET 1047
    feed_bytes(&term, "\x1b[?1047h");
    assert(term.is_alt_screen == 1);
    assert(term.grid == term.alt_grid);

    feed_bytes(&term, "\x1b[?1047l");
    assert(term.is_alt_screen == 0);
    assert(term.grid == term.primary_grid);

    // DECSET 47
    feed_bytes(&term, "\x1b[?47h");
    assert(term.is_alt_screen == 1);
    assert(term.grid == term.alt_grid);

    feed_bytes(&term, "\x1b[?47l");
    assert(term.is_alt_screen == 0);
    assert(term.grid == term.primary_grid);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_decset_1048_cursor(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Position cursor at row 7, col 12
    feed_bytes(&term, "\x1b[7;12H");
    assert(term.cursor_y == 6);
    assert(term.cursor_x == 11);

    // Save cursor with DECSET 1048
    feed_bytes(&term, "\x1b[?1048h");
    assert(term.saved_cursor_y == 6);
    assert(term.saved_cursor_x == 11);

    // Move cursor to origin
    feed_bytes(&term, "\x1b[1;1H");
    assert(term.cursor_y == 0);
    assert(term.cursor_x == 0);

    // Restore cursor with DECRST 1048
    feed_bytes(&term, "\x1b[?1048l");
    assert(term.cursor_y == 6);
    assert(term.cursor_x == 11);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_esc78_and_csi_su(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Test ESC 7 and ESC 8
    feed_bytes(&term, "\x1b[10;20H");
    feed_bytes(&term, "\x1b""7"); // ESC 7
    feed_bytes(&term, "\x1b[1;1H");
    feed_bytes(&term, "\x1b""8"); // ESC 8
    assert(term.cursor_y == 9);
    assert(term.cursor_x == 19);

    // Test CSI s and CSI u
    feed_bytes(&term, "\x1b[15;25H");
    feed_bytes(&term, "\x1b[s");
    feed_bytes(&term, "\x1b[1;1H");
    feed_bytes(&term, "\x1b[u");
    assert(term.cursor_y == 14);
    assert(term.cursor_x == 24);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_decset_25_cursor_visibility(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    assert(term.cursor_visible == 1);
    feed_bytes(&term, "\x1b[?25l");
    assert(term.cursor_visible == 0);
    feed_bytes(&term, "\x1b[?25h");
    assert(term.cursor_visible == 1);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_scrollback_isolation(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);
    cols = 80;
    rows = 24;
    dirty = calloc(rows, sizeof(uint8_t));

    // Clear history
    for (int i = 0; i < MAX_HIST_LINES; i++) {
        if (history[i]) { free(history[i]); history[i] = NULL; }
    }
    hist_count = 0;
    hist_head = 0;

    // Scroll 30 lines in primary grid
    for (int i = 0; i < 30; i++) {
        feed_bytes(&term, "\n");
    }
    int primary_hist = hist_count;
    assert(primary_hist > 0);

    // Enter alternate screen
    feed_bytes(&term, "\x1b[?1049h");
    assert(term.is_alt_screen == 1);

    // Scroll 100 lines in alternate screen
    for (int i = 0; i < 100; i++) {
        feed_bytes(&term, "\n");
    }
    // Verify hist_count was NOT incremented by alternate screen scrolling
    assert(hist_count == primary_hist);

    // Exit alternate screen
    feed_bytes(&term, "\x1b[?1049l");
    assert(hist_count == primary_hist);

    free(dirty);
    dirty = NULL;
    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_csi_3_j_scrollback_clear(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);
    cols = 80;
    rows = 24;
    dirty = calloc(rows, sizeof(uint8_t));

    // Generate history lines
    for (int i = 0; i < 30; i++) {
        feed_bytes(&term, "\n");
    }
    assert(hist_count > 0);

    // CSI 3 J clears scrollback
    feed_bytes(&term, "\x1b[3J");
    assert(hist_count == 0);
    assert(hist_head == 0);

    free(dirty);
    dirty = NULL;
    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_utf8_decoding(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // 1-byte ASCII 'Z'
    feed_bytes(&term, "Z");
    assert(term.grid[0].codepoint == 'Z');

    // 2-byte UTF-8 'é' (0xC3 0xA9 -> 0x00E9)
    term.cursor_x = 1;
    feed_bytes(&term, "\xc3\xa9");
    assert(term.grid[1].codepoint == 0x00E9);

    // 3-byte UTF-8 '€' (0xE2 0x82 0xAC -> 0x20AC)
    term.cursor_x = 2;
    feed_bytes(&term, "\xe2\x82\xac");
    assert(term.grid[2].codepoint == 0x20AC);

    // 4-byte UTF-8 '🚀' (0xF0 0x9F 0x9A 0x80 -> 0x1F680)
    term.cursor_x = 3;
    feed_bytes(&term, "\xf0\x9f\x9a\x80");
    assert(term.grid[3].codepoint == 0x1F680);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_sgr_color_parsing(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // ANSI red foreground (31)
    feed_bytes(&term, "\x1b[31m");
    assert(term.cur_fg == ansi_palette[1]);

    // ANSI green background (42)
    feed_bytes(&term, "\x1b[42m");
    assert(term.cur_bg == ansi_palette[2]);

    // Reset SGR
    feed_bytes(&term, "\x1b[0m");
    assert(term.cur_fg == COLOR_FG);
    assert(term.cur_bg == COLOR_BG);

    // 24-bit TrueColor foreground: \e[38;2;123;45;67m
    feed_bytes(&term, "\x1b[38;2;123;45;67m");
    assert(term.cur_fg == ((123 << 16) | (45 << 8) | 67));

    // 24-bit TrueColor background: \e[48;2;200;100;50m
    feed_bytes(&term, "\x1b[48;2;200;100;50m");
    assert(term.cur_bg == ((200 << 16) | (100 << 8) | 50));

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_theme_color_parsing(void) {
    tests_run++;

    // 1. Verify hex parser directly
    assert(parse_hex_color("#2E3440", 0) == 0x2E3440);
    assert(parse_hex_color("BF616A", 0) == 0xBF616A);
    assert(parse_hex_color("  \"#A3BE8C\"  ", 0) == 0xA3BE8C);
    assert(parse_hex_color("'#81A1C1'", 0) == 0x81A1C1);
    assert(parse_hex_color("invalid_hex", 0x123456) == 0x123456);

    // 2. Test loading from custom colors file
    char tmp_file[] = "/tmp/dwmterm_theme_test_XXXXXX";
    int fd = mkstemp(tmp_file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);

    // Write custom theme (e.g. Dracula)
    fprintf(f, "# Test theme file\n");
    fprintf(f, "background = #282A36\n");
    fprintf(f, "foreground = #F8F8F2\n");
    fprintf(f, "cursor = #BD93F9\n");
    fprintf(f, "color0 = #21222C\n");
    fprintf(f, "color1 = #FF5555\n");
    fprintf(f, "color15 = #FFFFFF\n");
    fclose(f);

    // Load custom theme
    load_theme_colors_from_file(tmp_file, 0);

    assert(color_bg == 0x282A36);
    assert(color_fg == 0xF8F8F2);
    assert(COLOR_CURSOR == 0x00FFFFFF);
    assert(ansi_palette[0] == 0x21222C);
    assert(ansi_palette[1] == 0xFF5555);
    assert(ansi_palette[15] == 0xFFFFFF);

    // Reset to defaults
    load_theme_colors_from_file(NULL, 1);
    assert(color_bg == DEFAULT_COLOR_BG);
    assert(color_fg == DEFAULT_COLOR_FG);
    assert(COLOR_CURSOR == DEFAULT_COLOR_CURSOR);
    assert(ansi_palette[0] == default_ansi_palette[0]);

    unlink(tmp_file);
    TEST_PASS();
}

static void test_toml_theme_colors_parsing(void) {
    tests_run++;

    char tmp_file[] = "/tmp/dwmterm_toml_theme_XXXXXX";
    int fd = mkstemp(tmp_file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);

    fprintf(f, "mode = \"dark\"\n");
    fprintf(f, "accent = \"#798186\"\n");
    fprintf(f, "selection = \"#343d41\"\n");
    fprintf(f, "muted = \"#4b4e55\"\n");
    fprintf(f, "background = \"#101315\"\n");
    fprintf(f, "foreground = \"#cacccc\"\n");
    fprintf(f, "bright_foreground = \"#a5aeb4\"\n");
    fprintf(f, "red = \"#565d60\"\n");
    fprintf(f, "green = \"#9fa5a9\"\n");
    fprintf(f, "yellow = \"#d9dbdc\"\n");
    fprintf(f, "blue = \"#798186\"\n");
    fprintf(f, "magenta = \"#aeaeae\"\n");
    fprintf(f, "cyan = \"#707070\"\n");
    fprintf(f, "bright_red = \"#de6145\"\n");
    fprintf(f, "bright_green = \"#343d41\"\n");
    fprintf(f, "bright_yellow = \"#c9c2b4\"\n");
    fprintf(f, "bright_blue = \"#5d6367\"\n");
    fprintf(f, "bright_magenta = \"#9a9a9a\"\n");
    fprintf(f, "bright_cyan = \"#707070\"\n");
    fclose(f);

    load_theme_colors_from_file(tmp_file, 0);

    assert(color_bg == 0x101315);
    assert(color_fg == 0xcacccc);
    assert(COLOR_CURSOR == 0x00FFFFFF);
    assert(color_sel_bg == 0x343d41);
    assert(ansi_palette[0] == 0x101315);
    assert(ansi_palette[1] == 0x565d60);
    assert(ansi_palette[2] == 0x9fa5a9);
    assert(ansi_palette[3] == 0xd9dbdc);
    assert(ansi_palette[4] == 0x798186);
    assert(ansi_palette[5] == 0xaeaeae);
    assert(ansi_palette[6] == 0x707070);
    assert(ansi_palette[7] == 0xcacccc);
    assert(ansi_palette[8] == 0x4b4e55);
    assert(ansi_palette[9] == 0xde6145);
    assert(ansi_palette[10] == 0x343d41);
    assert(ansi_palette[11] == 0xc9c2b4);
    assert(ansi_palette[12] == 0x5d6367);
    assert(ansi_palette[13] == 0x9a9a9a);
    assert(ansi_palette[14] == 0x707070);
    assert(ansi_palette[15] == 0xa5aeb4);

    load_theme_colors_from_file(NULL, 1);
    unlink(tmp_file);
    TEST_PASS();
}

static void test_ghostty_conf_parsing(void) {
    tests_run++;

    char tmp_file[] = "/tmp/dwmterm_ghostty_test_XXXXXX";
    int fd = mkstemp(tmp_file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);

    fprintf(f, "background = #101315\n");
    fprintf(f, "foreground = #cacccc\n");
    fprintf(f, "cursor-color = #a5aeb4\n");
    fprintf(f, "selection-background = #343d41\n");
    fprintf(f, "selection-foreground = #a5aeb4\n");
    fprintf(f, "palette = 0=#101315\n");
    fprintf(f, "palette = 1=#565d60\n");
    fprintf(f, "palette = 15=#a5aeb4\n");
    fclose(f);

    load_theme_colors_from_file(tmp_file, 0);

    assert(color_bg == 0x101315);
    assert(color_fg == 0xcacccc);
    assert(COLOR_CURSOR == 0x00FFFFFF);
    assert(color_sel_bg == 0x343d41);
    assert(color_sel_fg == 0xa5aeb4);
    assert(ansi_palette[0] == 0x101315);
    assert(ansi_palette[1] == 0x565d60);
    assert(ansi_palette[15] == 0xa5aeb4);

    load_theme_colors_from_file(NULL, 1);
    unlink(tmp_file);
    TEST_PASS();
}

static void test_titus_themes_toml_parsing(void) {
    tests_run++;

    char tmp_file[] = "/tmp/dwmterm_titus_test_XXXXXX";
    int fd = mkstemp(tmp_file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);

    fprintf(f, "[active]\n");
    fprintf(f, "theme = \"nord\"\n");
    fprintf(f, "\n");
    fprintf(f, "[theme.dracula]\n");
    fprintf(f, "term_bg = \"#282a36\"\n");
    fprintf(f, "term_fg = \"#f8f8f2\"\n");
    fprintf(f, "\n");
    fprintf(f, "[theme.nord]\n");
    fprintf(f, "term_bg = \"#2e3440\"\n");
    fprintf(f, "term_fg = \"#d8dee9\"\n");
    fprintf(f, "term_cursor = \"#81a1c1\"\n");
    fprintf(f, "term_color0 = \"#3b4252\"\n");
    fprintf(f, "term_color1 = \"#bf616a\"\n");
    fprintf(f, "term_color15 = \"#eceff4\"\n");
    fclose(f);

    load_theme_colors_from_file(tmp_file, 0);

    assert(color_bg == 0x2e3440);
    assert(color_fg == 0xd8dee9);
    assert(COLOR_CURSOR == 0x00FFFFFF);
    assert(ansi_palette[0] == 0x3b4252);
    assert(ansi_palette[1] == 0xbf616a);
    assert(ansi_palette[15] == 0xeceff4);

    load_theme_colors_from_file(NULL, 1);
    unlink(tmp_file);
    TEST_PASS();
}

static void test_theme_discovery_cascade(void) {
    tests_run++;

    char orig_xdg[PATH_MAX] = {0};
    char orig_home[PATH_MAX] = {0};
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x) strncpy(orig_xdg, x, sizeof(orig_xdg) - 1);
    const char *h = getenv("HOME");
    if (h) strncpy(orig_home, h, sizeof(orig_home) - 1);

    char tmp_dir[] = "/tmp/dwmterm_cascade_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    char fake_xdg[512];
    char fake_home[512];
    snprintf(fake_xdg, sizeof(fake_xdg), "%s/config", tmp_dir);
    snprintf(fake_home, sizeof(fake_home), "%s/home", tmp_dir);
    mkdir(fake_xdg, 0777);
    mkdir(fake_home, 0777);

    setenv("XDG_CONFIG_HOME", fake_xdg, 1);
    setenv("HOME", fake_home, 1);

    char resolved[PATH_MAX] = {0};

    // 1. Initially empty: fallback to built-in default
    assert(resolve_theme_path(resolved, sizeof(resolved)) == 0);

    // 2. Desktop session state colors.toml
    char session_theme_dir[1024];
    setenv("DESKTOP_SESSION", "test_session", 1);
    snprintf(session_theme_dir, sizeof(session_theme_dir), "%s/.local/state/test_session/current/theme", fake_home);
    char cmd[PATH_MAX * 4];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", session_theme_dir);
    int rc = system(cmd);
    (void)rc;
    char session_theme_file[PATH_MAX];
    snprintf(session_theme_file, sizeof(session_theme_file), "%s/colors.toml", session_theme_dir);
    FILE *f1 = fopen(session_theme_file, "w");
    assert(f1 != NULL);
    fprintf(f1, "background = \"#101315\"\n");
    fclose(f1);

    assert(resolve_theme_path(resolved, sizeof(resolved)) == 1);
    assert(strcmp(resolved, session_theme_file) == 0);

    // 3. User override in $XDG_CONFIG_HOME/dwmterm/colors has higher priority
    char dwmterm_dir[1024];
    snprintf(dwmterm_dir, sizeof(dwmterm_dir), "%s/dwmterm", fake_xdg);
    mkdir(dwmterm_dir, 0777);
    char override_file[PATH_MAX];
    snprintf(override_file, sizeof(override_file), "%s/colors", dwmterm_dir);
    FILE *f2 = fopen(override_file, "w");
    assert(f2 != NULL);
    fprintf(f2, "background = \"#282a36\"\n");
    fclose(f2);

    assert(resolve_theme_path(resolved, sizeof(resolved)) == 1);
    assert(strcmp(resolved, override_file) == 0);

    // Cleanup
    unlink(override_file);
    rmdir(dwmterm_dir);
    unlink(session_theme_file);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_dir);
    rc = system(cmd);
    (void)rc;

    if (orig_xdg[0]) setenv("XDG_CONFIG_HOME", orig_xdg, 1);
    else unsetenv("XDG_CONFIG_HOME");
    if (orig_home[0]) setenv("HOME", orig_home, 1);

    TEST_PASS();
}

static void test_config_file_parsing(void) {
    tests_run++;

    char tmp_file[] = "/tmp/dwmterm_config_test_XXXXXX";
    int fd = mkstemp(tmp_file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);

    fprintf(f, "# Test configuration file\n");
    fprintf(f, "font_size = 14 # inline comment with hash\n");
    fprintf(f, "font_family = \"JetBrainsMono Nerd Font\" ; trailing semicolon comment\n");
    fprintf(f, "cols = 100\n");
    fprintf(f, "rows = 35 # another comment\n");
    fprintf(f, "padding = 16 ; padding comment\n");
    fclose(f);

    font_pt = 12;
    default_font_pt = 12;
    config_font_family[0] = '\0';
    cols = DEFAULT_COLS;
    rows = DEFAULT_ROWS;
    padding = DEFAULT_PADDING;

    load_config_from_file(tmp_file);

    assert(font_pt == 14);
    assert(default_font_pt == 14);
    assert(strcmp(config_font_family, "JetBrainsMono Nerd Font") == 0);
    assert(cols == 100);
    assert(rows == 35);
    assert(padding == 16);

    // Test unquoted font family with inline comment
    f = fopen(tmp_file, "w");
    assert(f != NULL);
    fprintf(f, "font_family = CaskaydiaMono Nerd Font #MesloLGS Nerd Font\n");
    fprintf(f, "font_size = 26 # font size comment\n");
    fclose(f);

    load_config_from_file(tmp_file);
    assert(font_pt == 26);
    assert(strcmp(config_font_family, "CaskaydiaMono Nerd Font") == 0);

    // Reset back to defaults
    font_pt = 12;
    default_font_pt = 12;
    config_font_family[0] = '\0';
    cols = DEFAULT_COLS;
    rows = DEFAULT_ROWS;
    padding = DEFAULT_PADDING;

    unlink(tmp_file);
    TEST_PASS();
}

static void test_alt_screen_clean_colors(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Set dirty foreground and background colors in primary buffer
    term.cur_fg = 0x00FF0000;
    term.cur_bg = 0x0000FF00;

    // Switch to alternate screen
    feed_bytes(&term, "\x1b[?1049h");

    // Verify alternate screen cells start with clean default colors, NOT dirty cur_bg
    assert(term.alt_grid[0].bg == COLOR_BG);
    assert(term.alt_grid[0].fg == COLOR_FG);

    // Exit alternate screen
    feed_bytes(&term, "\x1b[?1049l");

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_working_directory_handling(void) {
    tests_run++;

    // Test stat check on valid directory
    struct stat st;
    assert(stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode));

    // Test stat check on non-existent path
    assert(stat("/nonexistent_dwmterm_test_dir_12345", &st) != 0);

    TEST_PASS();
}

static void test_apc_sequence_filtering(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Feed Kitty graphics probe sequence (Application Program Command)
    feed_bytes(&term, "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\");
    assert(term.state == STATE_NORMAL);
    for (int i = 0; i < term.cols * term.rows; i++) {
        assert(term.grid[i].codepoint == ' ');
    }

    // Feed DCS probe sequence (ESC P ... ST)
    feed_bytes(&term, "\x1bP+q54657374\x1b\\");
    assert(term.state == STATE_NORMAL);
    for (int i = 0; i < term.cols * term.rows; i++) {
        assert(term.grid[i].codepoint == ' ');
    }

    // Feed APC sequence terminated by BEL (0x07)
    feed_bytes(&term, "\x1b_TEST_PROBE\x07");
    assert(term.state == STATE_NORMAL);
    for (int i = 0; i < term.cols * term.rows; i++) {
        assert(term.grid[i].codepoint == ' ');
    }

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_csi_dsr_and_da(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    pty_master = pipe_fds[1];

    // Set cursor to row 10, col 20 (0-indexed: 9, 19)
    term.cursor_y = 9;
    term.cursor_x = 19;

    // Test CSI 6 n (Cursor Position Report) -> Expect 1-indexed \x1b[10;20R
    feed_bytes(&term, "\x1b[6n");
    char buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);
    assert(n > 0);
    assert(strcmp(buf, "\x1b[10;20R") == 0);

    // Test CSI 5 n (Device Status Report - Terminal OK) -> Expect \x1b[0n
    memset(buf, 0, sizeof(buf));
    feed_bytes(&term, "\x1b[5n");
    n = read(pipe_fds[0], buf, sizeof(buf) - 1);
    assert(n > 0);
    assert(strcmp(buf, "\x1b[0n") == 0);

    // Test CSI c (Primary Device Attributes) -> Expect \x1b[?6c
    memset(buf, 0, sizeof(buf));
    feed_bytes(&term, "\x1b[c");
    n = read(pipe_fds[0], buf, sizeof(buf) - 1);
    assert(n > 0);
    assert(strcmp(buf, "\x1b[?6c") == 0);

    // Test CSI 0 c -> Expect \x1b[?6c
    memset(buf, 0, sizeof(buf));
    feed_bytes(&term, "\x1b[0c");
    n = read(pipe_fds[0], buf, sizeof(buf) - 1);
    assert(n > 0);
    assert(strcmp(buf, "\x1b[?6c") == 0);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    pty_master = -1;

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_csi_character_editing(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // 1. Test ICH (Insert Character: \x1b[@ and \x1b[n@)
    // Type "aaabbbddd"
    feed_bytes(&term, "aaabbbddd");
    // Move cursor left 3 positions (to the first 'd')
    feed_bytes(&term, "\x1b[3D");
    assert(term.cursor_x == 6);
    assert(term.grid[6].codepoint == 'd');

    // Insert 3 characters with CSI 3 @
    feed_bytes(&term, "\x1b[3@");
    // Cursor position should not change
    assert(term.cursor_x == 6);
    // Vacated positions should be space
    assert(term.grid[6].codepoint == ' ');
    assert(term.grid[7].codepoint == ' ');
    assert(term.grid[8].codepoint == ' ');
    // "ddd" should have moved to indices 9, 10, 11
    assert(term.grid[9].codepoint == 'd');
    assert(term.grid[10].codepoint == 'd');
    assert(term.grid[11].codepoint == 'd');

    // Type "ccc"
    feed_bytes(&term, "ccc");
    // Grid row should now be "aaabbbcccddd"
    char line[16];
    for (int i = 0; i < 12; i++) line[i] = (char)term.grid[i].codepoint;
    line[12] = '\0';
    assert(strcmp(line, "aaabbbcccddd") == 0);

    // 2. Test DCH (Delete Character: \x1b[P and \x1b[nP)
    // Move cursor back to index 6 ('c')
    feed_bytes(&term, "\x1b[3D");
    assert(term.cursor_x == 6);
    // Delete 3 characters (removes "ccc")
    feed_bytes(&term, "\x1b[3P");
    assert(term.cursor_x == 6);
    // "ddd" should shift back to index 6, 7, 8
    for (int i = 0; i < 9; i++) line[i] = (char)term.grid[i].codepoint;
    line[9] = '\0';
    assert(strcmp(line, "aaabbbddd") == 0);
    assert(term.grid[9].codepoint == ' ');

    // 3. Test ECH (Erase Character: \x1b[X and \x1b[nX)
    // Erase 3 characters starting at index 6
    feed_bytes(&term, "\x1b[3X");
    assert(term.cursor_x == 6);
    assert(term.grid[6].codepoint == ' ');
    assert(term.grid[7].codepoint == ' ');
    assert(term.grid[8].codepoint == ' ');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_csi_line_editing(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Fill row 0 with 'A', row 1 with 'B', row 2 with 'C'
    feed_bytes(&term, "AAAA\r\nBBBB\r\nCCCC");
    assert(term.grid[0 * 80].codepoint == 'A');
    assert(term.grid[1 * 80].codepoint == 'B');
    assert(term.grid[2 * 80].codepoint == 'C');

    // Move cursor to row 1 (B)
    feed_bytes(&term, "\x1b[2;1H");
    assert(term.cursor_y == 1);

    // Insert 1 line (IL: \x1b[L)
    feed_bytes(&term, "\x1b[1L");
    // Row 0 should still be 'A', row 1 should be blank, row 2 should now be 'B', row 3 should be 'C'
    assert(term.grid[0 * 80].codepoint == 'A');
    assert(term.grid[1 * 80].codepoint == ' ');
    assert(term.grid[2 * 80].codepoint == 'B');
    assert(term.grid[3 * 80].codepoint == 'C');

    // Delete 1 line (DL: \x1b[M) at row 1
    feed_bytes(&term, "\x1b[1M");
    // Row 1 should now be 'B', row 2 should be 'C'
    assert(term.grid[0 * 80].codepoint == 'A');
    assert(term.grid[1 * 80].codepoint == 'B');
    assert(term.grid[2 * 80].codepoint == 'C');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_csi_insert_line_respects_margins(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 8, 8);

    for (int r = 0; r < term.rows; r++) {
        for (int c = 0; c < term.cols; c++) {
            term.grid[r * term.cols + c] = (Cell){(uint32_t)('A' + r), COLOR_FG, COLOR_BG, 0};
        }
    }

    /* Rows 3 through 6 are the scrolling region. */
    feed_bytes(&term, "\x1b[3;6r\x1b[4;1H\x1b[2L");

    /* The header and footer must remain outside the IL operation. */
    assert(term.grid[0 * term.cols].codepoint == 'A');
    assert(term.grid[1 * term.cols].codepoint == 'B');
    assert(term.grid[2 * term.cols].codepoint == 'C');
    assert(term.grid[6 * term.cols].codepoint == 'G');
    assert(term.grid[7 * term.cols].codepoint == 'H');

    /* IL is limited to rows 4 through 6, inclusive. */
    assert(term.grid[3 * term.cols].codepoint == ' ');
    assert(term.grid[4 * term.cols].codepoint == ' ');
    assert(term.grid[5 * term.cols].codepoint == 'D');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_csi_delete_line_respects_margins(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 8, 8);

    for (int r = 0; r < term.rows; r++) {
        for (int c = 0; c < term.cols; c++) {
            term.grid[r * term.cols + c] = (Cell){(uint32_t)('A' + r), COLOR_FG, COLOR_BG, 0};
        }
    }

    /* Rows 3 through 6 are the scrolling region. */
    feed_bytes(&term, "\x1b[3;6r\x1b[4;1H\x1b[2M");

    /* The header and footer must remain outside the DL operation. */
    assert(term.grid[0 * term.cols].codepoint == 'A');
    assert(term.grid[1 * term.cols].codepoint == 'B');
    assert(term.grid[2 * term.cols].codepoint == 'C');
    assert(term.grid[6 * term.cols].codepoint == 'G');
    assert(term.grid[7 * term.cols].codepoint == 'H');

    /* DL is limited to rows 4 through 6, inclusive. */
    assert(term.grid[3 * term.cols].codepoint == 'F');
    assert(term.grid[4 * term.cols].codepoint == ' ');
    assert(term.grid[5 * term.cols].codepoint == ' ');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_csi_line_editing_outside_margins_is_noop(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 8, 8);

    for (int r = 0; r < term.rows; r++) {
        for (int c = 0; c < term.cols; c++) {
            term.grid[r * term.cols + c] = (Cell){(uint32_t)('A' + r), COLOR_FG, COLOR_BG, 0};
        }
    }

    /* Rows 3 through 6 are the scrolling region. */
    feed_bytes(&term, "\x1b[3;6r\x1b[2;1H\x1b[L\x1b[M");
    feed_bytes(&term, "\x1b[8;1H\x1b[L\x1b[M");

    for (int r = 0; r < term.rows; r++) {
        assert(term.grid[r * term.cols].codepoint == (uint32_t)('A' + r));
    }

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_synchronized_output_mode_2026(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    assert(in_sync_update == 0);

    // Enter synchronized output (BSU)
    feed_bytes(&term, "\x1b[?2026h");
    assert(in_sync_update == 1);

    // Exit synchronized output (ESU)
    feed_bytes(&term, "\x1b[?2026l");
    assert(in_sync_update == 0);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_charset_designation_filtering(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Feed ISO 2022 G0 ASCII designation: \x1b(B
    feed_bytes(&term, "\x1b(B");
    assert(term.state == STATE_NORMAL);
    assert(term.grid[0].codepoint == ' ');

    // Feed G1, G2, G3, #, % designations
    feed_bytes(&term, "\x1b)0\x1b*B\x1b+B\x1b#8\x1b%G");
    assert(term.state == STATE_NORMAL);
    for (int i = 0; i < term.cols * term.rows; i++) {
        assert(term.grid[i].codepoint == ' ');
    }

    // Now verify normal 'B' still prints when not preceded by ESC (
    feed_bytes(&term, "B");
    assert(term.grid[0].codepoint == 'B');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_esc_reverse_index(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Move cursor to row 5
    term.cursor_y = 5;
    feed_bytes(&term, "\x1bM");
    assert(term.cursor_y == 4);

    // At top of screen (row 0), ESC M scrolls down
    term.cursor_y = 0;
    term.grid[0].codepoint = 'T';
    feed_bytes(&term, "\x1bM");
    assert(term.cursor_y == 0);
    // Row 0 should now be blank, row 1 should have 'T'
    assert(term.grid[0].codepoint == ' ');
    assert(term.grid[80].codepoint == 'T');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_wide_character_grid(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Feed wide character: 🚀 (U+1F680)
    feed_bytes(&term, "\xf0\x9f\x9a\x80");
    assert(term.cursor_x == 2);
    assert(term.grid[0].codepoint == 0x1F680);
    assert(term.grid[0].flags & FLAG_WIDE);
    assert(term.grid[1].flags & FLAG_WIDE_DUMMY);

    // Feed normal character after: 'A'
    feed_bytes(&term, "A");
    assert(term.cursor_x == 3);
    assert(term.grid[2].codepoint == 'A');
    assert(!(term.grid[2].flags & FLAG_WIDE));

    // Test right margin wrap for wide character
    term.cursor_x = 79;
    term.cursor_y = 0;
    feed_bytes(&term, "\xf0\x9f\x9a\x80");
    // Should wrap to row 1, col 2
    assert(term.cursor_y == 1);
    assert(term.cursor_x == 2);
    assert(term.grid[80].codepoint == 0x1F680);
    assert(term.grid[80].flags & FLAG_WIDE);
    assert(term.grid[81].flags & FLAG_WIDE_DUMMY);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_mouse_mode_and_sgr_toggles(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Initial state
    assert(mouse_mode == MOUSE_MODE_OFF);
    assert(mouse_sgr == 0);

    // Enable DECSET 1000 (normal tracking)
    feed_bytes(&term, "\x1b[?1000h");
    assert(mouse_mode == MOUSE_MODE_NORMAL);

    // Enable DECSET 1002 (button event tracking)
    feed_bytes(&term, "\x1b[?1002h");
    assert(mouse_mode == MOUSE_MODE_BUTTON_EVENT);

    // Enable DECSET 1006 (SGR mode)
    feed_bytes(&term, "\x1b[?1006h");
    assert(mouse_sgr == 1);

    // Disable DECSET 1006
    feed_bytes(&term, "\x1b[?1006l");
    assert(mouse_sgr == 0);

    // Disable DECSET 1000
    feed_bytes(&term, "\x1b[?1000l");
    assert(mouse_mode == MOUSE_MODE_OFF);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_bracketed_paste_toggle(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    assert(bracketed_paste == 0);
    feed_bytes(&term, "\x1b[?2004h");
    assert(bracketed_paste == 1);
    feed_bytes(&term, "\x1b[?2004l");
    assert(bracketed_paste == 0);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_decscusr_cursor_style(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Test block styles (0, 1, 2)
    feed_bytes(&term, "\x1b[2 q");
    assert(cursor_style == 2);

    // Test underline style (3, 4)
    feed_bytes(&term, "\x1b[4 q");
    assert(cursor_style == 4);

    // Test bar/beam style (5, 6)
    feed_bytes(&term, "\x1b[5 q");
    assert(cursor_style == 5);

    // Reset to block
    feed_bytes(&term, "\x1b[0 q");
    assert(cursor_style == 0);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_padding_xy_config_parsing(void) {
    tests_run++;

    char tmp_file[] = "/tmp/dwmterm_padding_test_XXXXXX";
    int fd = mkstemp(tmp_file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);

    fprintf(f, "# Independent padding\n");
    fprintf(f, "padding_x = 18\n");
    fprintf(f, "padding_y = 10\n");
    fclose(f);

    padding_x = DEFAULT_PADDING;
    padding_y = DEFAULT_PADDING;
    load_config_from_file(tmp_file);
    assert(padding_x == 18);
    assert(padding_y == 10);
    unlink(tmp_file);

    // Test window-padding-x and window-padding-y aliases
    char tmp_file2[] = "/tmp/dwmterm_padding_alias_XXXXXX";
    fd = mkstemp(tmp_file2);
    assert(fd >= 0);
    f = fdopen(fd, "w");
    assert(f != NULL);
    fprintf(f, "window-padding-x = 14\n");
    fprintf(f, "window-padding-y = 16\n");
    fclose(f);

    load_config_from_file(tmp_file2);
    assert(padding_x == 14);
    assert(padding_y == 16);
    unlink(tmp_file2);

    // Test single padding key sets both
    char tmp_file3[] = "/tmp/dwmterm_padding_single_XXXXXX";
    fd = mkstemp(tmp_file3);
    assert(fd >= 0);
    f = fdopen(fd, "w");
    assert(f != NULL);
    fprintf(f, "padding = 22\n");
    fclose(f);

    load_config_from_file(tmp_file3);
    assert(padding_x == 22);
    assert(padding_y == 22);
    unlink(tmp_file3);

    padding_x = DEFAULT_PADDING;
    padding_y = DEFAULT_PADDING;
    padding = DEFAULT_PADDING;

    TEST_PASS();
}

static void test_cursor_config_parsing(void) {
    tests_run++;

    char tmp_file[] = "/tmp/dwmterm_cursor_test_XXXXXX";
    int fd = mkstemp(tmp_file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);

    fprintf(f, "cursor_style = block\n");
    fprintf(f, "cursor_blink = true\n");
    fclose(f);

    cursor_style = 6;
    cursor_blink_enabled = 0;
    load_config_from_file(tmp_file);
    assert(cursor_style == 2);
    assert(cursor_blink_enabled == 1);
    unlink(tmp_file);

    // Test bar, underline, and cursor-style-blink aliases
    char tmp_file2[] = "/tmp/dwmterm_cursor_test2_XXXXXX";
    fd = mkstemp(tmp_file2);
    assert(fd >= 0);
    f = fdopen(fd, "w");
    assert(f != NULL);
    fprintf(f, "cursor-style = underline\n");
    fprintf(f, "cursor-style-blink = false\n");
    fclose(f);

    load_config_from_file(tmp_file2);
    assert(cursor_style == 4);
    assert(cursor_blink_enabled == 0);
    unlink(tmp_file2);

    char tmp_file3[] = "/tmp/dwmterm_cursor_test3_XXXXXX";
    fd = mkstemp(tmp_file3);
    assert(fd >= 0);
    f = fdopen(fd, "w");
    assert(f != NULL);
    fprintf(f, "cursor-style = bar\n");
    fclose(f);

    load_config_from_file(tmp_file3);
    assert(cursor_style == 6);
    unlink(tmp_file3);

    cursor_style = 6;
    cursor_blink_enabled = 0;

    TEST_PASS();
}

static void test_clean_mask_and_csi_u_keybindings(void) {
    tests_run++;

    // Verify CLEAN_MASK strips NumLock (Mod2Mask) and CapsLock (LockMask)
    unsigned int raw_state = ControlMask | LockMask | Mod2Mask;
    assert(CLEAN_MASK(raw_state) == ControlMask);

    raw_state = ShiftMask | Mod4Mask | Mod2Mask;
    assert(CLEAN_MASK(raw_state) == (ShiftMask | Mod4Mask));

    raw_state = ShiftMask | Mod1Mask | LockMask;
    assert(CLEAN_MASK(raw_state) == (ShiftMask | Mod1Mask));

    TEST_PASS();
}

static void test_keybind_config_and_super_mod(void) {
    tests_run++;

    // 1. Check default keybindings
    init_default_keybindings();
    super_mod_mask = Mod4Mask;
    alt_mod_mask = Mod1Mask;

    assert(match_keybinding(ControlMask | ShiftMask, XK_c) == ACTION_COPY);
    assert(match_keybinding(ControlMask | ShiftMask, XK_C) == ACTION_COPY);
    assert(match_keybinding(Mod4Mask, XK_c) == ACTION_COPY);
    assert(match_keybinding(Mod4Mask, XK_C) == ACTION_COPY);
    assert(match_keybinding(ControlMask, XK_Insert) == ACTION_COPY);

    assert(match_keybinding(ControlMask | ShiftMask, XK_v) == ACTION_PASTE);
    assert(match_keybinding(Mod4Mask, XK_v) == ACTION_PASTE);
    assert(match_keybinding(ShiftMask, XK_Insert) == ACTION_PASTE);

    // Modifier noise (Mod2/LockMask) should still match
    assert(match_keybinding(Mod4Mask | Mod2Mask | LockMask, XK_v) == ACTION_PASTE);

    // 2. Parse custom config file with keybind directives
    char tmp_file[] = "/tmp/dwmterm_keybind_test_XXXXXX";
    int fd = mkstemp(tmp_file);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f != NULL);

    fprintf(f, "keybind = super+x = copy\n");
    fprintf(f, "keybind = mod+y : paste\n");
    fprintf(f, "keybind = ctrl+shift+z = none\n");
    fprintf(f, "keybind = super+c = none\n");
    fclose(f);

    load_config_from_file(tmp_file);
    unlink(tmp_file);

    // Verify custom binds
    assert(match_keybinding(Mod4Mask, XK_x) == ACTION_COPY);
    assert(match_keybinding(Mod4Mask, XK_y) == ACTION_PASTE);
    assert(match_keybinding(Mod4Mask, XK_c) == ACTION_NONE);

    // 3. Test list syntax: copy_keys and paste_keys
    char tmp_file2[] = "/tmp/dwmterm_keybind_test2_XXXXXX";
    fd = mkstemp(tmp_file2);
    assert(fd >= 0);
    f = fdopen(fd, "w");
    assert(f != NULL);

    fprintf(f, "copy_keys = super+c, alt+w\n");
    fprintf(f, "paste_keys = super+v, ctrl+y\n");
    fclose(f);

    load_config_from_file(tmp_file2);
    unlink(tmp_file2);

    assert(match_keybinding(Mod4Mask, XK_c) == ACTION_COPY);
    assert(match_keybinding(Mod1Mask, XK_w) == ACTION_COPY);
    assert(match_keybinding(Mod4Mask, XK_v) == ACTION_PASTE);
    assert(match_keybinding(ControlMask, XK_y) == ACTION_PASTE);

    // Reset defaults
    init_default_keybindings();

    TEST_PASS();
}

static void test_decstbm_margins(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    assert(term.top_margin == 0);
    assert(term.bottom_margin == 23);

    // Set valid margins 5..20 (1-indexed -> 4..19 0-indexed)
    term.cursor_x = 10;
    term.cursor_y = 15;
    feed_bytes(&term, "\x1b[5;20r");
    assert(term.top_margin == 4);
    assert(term.bottom_margin == 19);
    // DECSTBM homes cursor to (0, 0)
    assert(term.cursor_x == 0);
    assert(term.cursor_y == 0);

    // Set margins with no params -> reset to full screen
    feed_bytes(&term, "\x1b[r");
    assert(term.top_margin == 0);
    assert(term.bottom_margin == 23);

    // Invalid margins: top >= bottom
    feed_bytes(&term, "\x1b[20;5r");
    assert(term.top_margin == 0);
    assert(term.bottom_margin == 23);

    feed_bytes(&term, "\x1b[10;10r");
    assert(term.top_margin == 0);
    assert(term.bottom_margin == 23);

    // Out of bounds: bottom > rows
    feed_bytes(&term, "\x1b[1;50r");
    assert(term.top_margin == 0);
    assert(term.bottom_margin == 23);

    // Reset via term_reset
    term.top_margin = 2;
    term.bottom_margin = 10;
    term_reset(&term);
    assert(term.top_margin == 0);
    assert(term.bottom_margin == 23);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_csi_scroll_up_down_margins(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 10);

    // Fill rows 0..9 with row identifiers
    for (int r = 0; r < 10; r++) {
        for (int c = 0; c < 80; c++) {
            term.grid[r * 80 + c] = (Cell){(uint32_t)('0' + r), COLOR_FG, COLOR_BG, 0};
        }
    }

    // Set margins to rows 2..5 (1-based -> 0-based rows 1..4)
    feed_bytes(&term, "\x1b[2;5r");
    assert(term.top_margin == 1);
    assert(term.bottom_margin == 4);

    // Scroll down 1 line (SD: \x1b[1T or \x1b[T)
    feed_bytes(&term, "\x1b[T");
    // Row 0 ('0') must be untouched
    assert(term.grid[0 * 80 + 0].codepoint == '0');
    // Row 1 should now be vacated (space)
    assert(term.grid[1 * 80 + 0].codepoint == ' ');
    // Rows 2..4 should have shifted down from old 1..3 ('1', '2', '3')
    assert(term.grid[2 * 80 + 0].codepoint == '1');
    assert(term.grid[3 * 80 + 0].codepoint == '2');
    assert(term.grid[4 * 80 + 0].codepoint == '3');
    // Rows 5..9 ('5'..'9') must be untouched
    assert(term.grid[5 * 80 + 0].codepoint == '5');
    assert(term.grid[6 * 80 + 0].codepoint == '6');
    assert(term.grid[9 * 80 + 0].codepoint == '9');

    // Scroll up 1 line (SU: \x1b[1S or \x1b[S)
    feed_bytes(&term, "\x1b[S");
    assert(term.grid[0 * 80 + 0].codepoint == '0');
    // Row 1 should now have '1'
    assert(term.grid[1 * 80 + 0].codepoint == '1');
    assert(term.grid[2 * 80 + 0].codepoint == '2');
    assert(term.grid[3 * 80 + 0].codepoint == '3');
    // Row 4 should be vacated (space)
    assert(term.grid[4 * 80 + 0].codepoint == ' ');
    // Rows 5..9 untouched
    assert(term.grid[5 * 80 + 0].codepoint == '5');

    // Test multi-line scroll down (SD 2 lines: \x1b[2T)
    feed_bytes(&term, "\x1b[2T");
    assert(term.grid[0 * 80 + 0].codepoint == '0');
    assert(term.grid[1 * 80 + 0].codepoint == ' ');
    assert(term.grid[2 * 80 + 0].codepoint == ' ');
    assert(term.grid[3 * 80 + 0].codepoint == '1');
    assert(term.grid[4 * 80 + 0].codepoint == '2');
    assert(term.grid[5 * 80 + 0].codepoint == '5');

    // Test bounded Reverse Index at top margin
    term.cursor_y = 1; // at top margin
    term.cursor_x = 0;
    feed_bytes(&term, "\x1bM");
    // Should scroll down within margins: line 1 vacated
    assert(term.cursor_y == 1);
    assert(term.grid[1 * 80 + 0].codepoint == ' ');
    assert(term.grid[2 * 80 + 0].codepoint == ' ');
    assert(term.grid[3 * 80 + 0].codepoint == ' ');
    assert(term.grid[4 * 80 + 0].codepoint == '1');
    assert(term.grid[0 * 80 + 0].codepoint == '0');
    assert(term.grid[5 * 80 + 0].codepoint == '5');

    // Test bounded Linefeed at bottom margin
    term.cursor_y = 4; // at bottom margin
    term.cursor_x = 0;
    term.grid[4 * 80 + 0].codepoint = 'X';
    feed_bytes(&term, "\n");
    // Cursor stays at bottom margin, region scrolls up
    assert(term.cursor_y == 4);
    assert(term.grid[3 * 80 + 0].codepoint == 'X');
    assert(term.grid[4 * 80 + 0].codepoint == ' ');
    assert(term.grid[0 * 80 + 0].codepoint == '0');
    assert(term.grid[5 * 80 + 0].codepoint == '5');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_bounded_margin_scroll_and_pinned_footer(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 40);

    // Emulate TUI applications with fixed footer/prompt:
    // Rows 0..34: scrollable history text
    for (int r = 0; r <= 34; r++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "History line %d", r);
        for (int c = 0; c < len; c++) {
            term.grid[r * 80 + c] = (Cell){(uint32_t)buf[c], COLOR_FG, COLOR_BG, 0};
        }
    }
    // Rows 35..39: pinned status / prompt
    const char *prompt_str = "Prompt: > test command";
    for (size_t c = 0; c < strlen(prompt_str); c++) {
        term.grid[35 * 80 + c] = (Cell){(uint32_t)prompt_str[c], COLOR_FG, COLOR_BG, 0};
    }
    const char *status_str = "Status: active session";
    for (size_t c = 0; c < strlen(status_str); c++) {
        term.grid[39 * 80 + c] = (Cell){(uint32_t)status_str[c], COLOR_FG, COLOR_BG, 0};
    }

    // Execute scroll up escape sequence within margin:
    // \x1b[1;35r (margins 1..35)
    // \x1b[H     (cursor home)
    // \x1b[2T    (scroll down 2 lines)
    // \x1b[1;40r (reset margins to 1..40)
    feed_bytes(&term, "\x1b[1;35r\x1b[H\x1b[2T\x1b[1;40r");

    // Assert rows 0 and 1 are empty
    for (int c = 0; c < 80; c++) {
        assert(term.grid[0 * 80 + c].codepoint == ' ');
        assert(term.grid[1 * 80 + c].codepoint == ' ');
    }

    // Assert row 2 now has old row 0 ("History line 0")
    assert(term.grid[2 * 80 + 0].codepoint == 'H');
    assert(term.grid[2 * 80 + 13].codepoint == '0');

    // Assert pinned prompt at row 35 and status at row 39 were 100% UNTOUCHED!
    for (size_t c = 0; c < strlen(prompt_str); c++) {
        assert(term.grid[35 * 80 + c].codepoint == (uint32_t)prompt_str[c]);
    }
    for (size_t c = 0; c < strlen(status_str); c++) {
        assert(term.grid[39 * 80 + c].codepoint == (uint32_t)status_str[c]);
    }

    // Print new history line into row 1
    feed_bytes(&term, "\x1b[1;1HNew history line");
    assert(term.grid[0 * 80 + 0].codepoint == 'N');
    assert(term.grid[0 * 80 + 15].codepoint == 'e');
    // Ensure no tail corruption remains in row 0
    assert(term.grid[0 * 80 + 16].codepoint == ' ');

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_private_csi_does_not_set_underline(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    // Normal SGR 4 should set underline
    feed_bytes(&term, "\x1b[4m");
    assert(term.cur_flags & FLAG_UNDERLINE);

    // SGR 24 clears underline
    feed_bytes(&term, "\x1b[24m");
    assert(!(term.cur_flags & FLAG_UNDERLINE));

    // Private sequences ending in m (e.g. modifyOtherKeys \x1b[>4m or \x1b[?4m) must NOT set underline
    feed_bytes(&term, "\x1b[>4m");
    assert(!(term.cur_flags & FLAG_UNDERLINE));

    feed_bytes(&term, "\x1b[?4m");
    assert(!(term.cur_flags & FLAG_UNDERLINE));

    feed_bytes(&term, "\x1b[<4m");
    assert(!(term.cur_flags & FLAG_UNDERLINE));

    feed_bytes(&term, "\x1b[=4m");
    assert(!(term.cur_flags & FLAG_UNDERLINE));

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_double_click_word_selection(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    live_term = term;
    cols = 80;
    rows = 24;

    const char *cmd = "git --work-tree=/home/user/workspace/repo   status";
    for (size_t i = 0; i < strlen(cmd); i++) {
        term.grid[i] = (Cell){(uint32_t)cmd[i], COLOR_FG, COLOR_BG, 0};
    }
    live_term.grid = term.grid;

    // Double-click in the middle of "--work-tree=..." (index 10)
    select_word_at(0, 10);

    // Word should start at index 4 and end at index 40
    assert(sel_start_r == 0);
    assert(sel_end_r == 0);
    assert(sel_start_c == 4);
    assert(sel_end_c == 40);
    assert(sel_text != NULL);
    assert(strcmp(sel_text, "--work-tree=/home/user/workspace/repo") == 0);

    // Select "git" at index 1
    select_word_at(0, 1);
    assert(sel_start_c == 0);
    assert(sel_end_c == 2);
    assert(strcmp(sel_text, "git") == 0);

    // Select full line
    select_line_at(0);
    assert(sel_start_c == 0);
    assert(sel_end_c == 79);
    assert(sel_text != NULL);
    assert(strcmp(sel_text, cmd) == 0);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_modifier_keypress_preserves_selection(void) {
    tests_run++;
    Terminal term;
    term_init(&term, 80, 24);

    live_term = term;
    cols = 80;
    rows = 24;

    const char *sample = "echo hello world";
    for (size_t i = 0; i < strlen(sample); i++) {
        term.grid[i] = (Cell){(uint32_t)sample[i], COLOR_FG, COLOR_BG, 0};
    }
    live_term.grid = term.grid;

    // Select "hello" (cols 5..9)
    select_word_at(0, 7);
    assert(sel_start_r == 0 && sel_end_r == 0);
    assert(sel_start_c == 5 && sel_end_c == 9);
    assert(sel_text != NULL);
    assert(strcmp(sel_text, "hello") == 0);

    // 1. Standalone modifier keys should NOT clear selection coordinates or sel_text
    KeySym mods[] = {
        XK_Super_L, XK_Super_R,
        XK_Shift_L, XK_Shift_R,
        XK_Control_L, XK_Control_R,
        XK_Alt_L, XK_Alt_R,
        XK_Scroll_Lock, XK_Caps_Lock, XK_Num_Lock
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        assert(is_modifier_keysym(mods[i]) == 1);
        handle_key_press_event(mods[i], 0, "", 0);
        assert(sel_start_r == 0 && sel_end_r == 0);
        assert(sel_start_c == 5 && sel_end_c == 9);
        assert(sel_text != NULL);
        assert(strcmp(sel_text, "hello") == 0);
    }

    // 2. Shortcut combo Super+c (match_keybinding -> ACTION_COPY) must also preserve selection
    init_default_keybindings();
    super_mod_mask = Mod4Mask;
    handle_key_press_event(XK_c, super_mod_mask, "", 0);
    assert(sel_start_r == 0 && sel_end_r == 0);
    assert(sel_start_c == 5 && sel_end_c == 9);
    assert(sel_text != NULL);
    assert(strcmp(sel_text, "hello") == 0);

    // 3. Typing an actual character (e.g. 'a') MUST clear the selection
    handle_key_press_event(XK_a, 0, "a", 1);
    assert(sel_start_r == -1);
    assert(sel_start_c == -1);
    assert(sel_end_r == -1);
    assert(sel_end_c == -1);
    assert(sel_active == 0);

    free(term.primary_grid);
    free(term.alt_grid);
    TEST_PASS();
}

static void test_selection_text_growth_and_formatting(void) {
    tests_run++;
    rows = 60;
    cols = 100;
    Terminal term;
    term_init(&term, cols, rows);
    live_term = term;
    scroll_offset = 0;
    replay_mode = 0;

    for (int r = 0; r < 50; r++) {
        for (int c = 0; c < 90; c++) {
            term.grid[r * cols + c] = (Cell){(uint32_t)('0' + (c % 10)), COLOR_FG, COLOR_BG, 0};
        }
    }

    sel_start_r = 0;
    sel_start_c = 0;
    sel_end_r = 49;
    sel_end_c = 89;

    copy_selection_text();

    assert(sel_text != NULL);
    size_t len = strlen(sel_text);
    assert(len == 50 * 90 + 49);
    assert(sel_text[0] == '0');
    assert(sel_text[89] == '9');
    assert(sel_text[90] == '\n');
    assert(sel_text[91] == '0');
    assert(sel_text[len - 1] == '9');

    free(sel_text);
    sel_text = NULL;
    sel_start_r = sel_start_c = sel_end_r = sel_end_c = -1;

    free(term.primary_grid);
    free(term.alt_grid);
    memset(&live_term, 0, sizeof(Terminal));
    TEST_PASS();
}

int main(void) {
    setlocale(LC_ALL, "");
    printf("====================================================\n");
    printf("    DWM Terminal Test Suite: Unit & Integration     \n");
    printf("====================================================\n");

    test_term_init();
    test_decset_1049_alt_screen();
    test_decset_1047_and_47();
    test_decset_1048_cursor();
    test_esc78_and_csi_su();
    test_decset_25_cursor_visibility();
    test_scrollback_isolation();
    test_csi_3_j_scrollback_clear();
    test_utf8_decoding();
    test_sgr_color_parsing();
    test_theme_color_parsing();
    test_toml_theme_colors_parsing();
    test_ghostty_conf_parsing();
    test_titus_themes_toml_parsing();
    test_theme_discovery_cascade();
    test_config_file_parsing();
    test_alt_screen_clean_colors();
    test_working_directory_handling();
    test_apc_sequence_filtering();
    test_csi_dsr_and_da();
    test_csi_character_editing();
    test_csi_line_editing();
    test_csi_insert_line_respects_margins();
    test_csi_delete_line_respects_margins();
    test_csi_line_editing_outside_margins_is_noop();
    test_synchronized_output_mode_2026();
    test_charset_designation_filtering();
    test_esc_reverse_index();
    test_wide_character_grid();
    test_mouse_mode_and_sgr_toggles();
    test_bracketed_paste_toggle();
    test_decscusr_cursor_style();
    test_padding_xy_config_parsing();
    test_cursor_config_parsing();
    test_clean_mask_and_csi_u_keybindings();
    test_keybind_config_and_super_mod();
    test_decstbm_margins();
    test_csi_scroll_up_down_margins();
    test_bounded_margin_scroll_and_pinned_footer();
    test_private_csi_does_not_set_underline();
    test_double_click_word_selection();
    test_modifier_keypress_preserves_selection();
    test_selection_text_growth_and_formatting();

    printf("====================================================\n");
    printf("All %d/%d tests passed successfully!\n", tests_passed, tests_run);
    printf("====================================================\n");
    return 0;
}
