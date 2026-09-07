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

static void test_omarchy_colors_toml_parsing(void) {
    tests_run++;

    char tmp_file[] = "/tmp/dwmterm_omarchy_test_XXXXXX";
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

    // 2. Omarchy colors.toml
    char omarchy_dir[1024];
    snprintf(omarchy_dir, sizeof(omarchy_dir), "%s/.local/state/omarchy/current/theme", fake_home);
    char cmd[PATH_MAX * 4];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", omarchy_dir);
    int rc = system(cmd);
    (void)rc;
    char omarchy_file[PATH_MAX];
    snprintf(omarchy_file, sizeof(omarchy_file), "%s/colors.toml", omarchy_dir);
    FILE *f1 = fopen(omarchy_file, "w");
    assert(f1 != NULL);
    fprintf(f1, "background = \"#101315\"\n");
    fclose(f1);

    assert(resolve_theme_path(resolved, sizeof(resolved)) == 1);
    assert(strcmp(resolved, omarchy_file) == 0);

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
    unlink(omarchy_file);
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
    fprintf(f, "font_size = 14\n");
    fprintf(f, "font_family = \"JetBrainsMono Nerd Font\"\n");
    fprintf(f, "cols = 100\n");
    fprintf(f, "rows = 35\n");
    fprintf(f, "padding = 16\n");
    fclose(f);

    font_pt = 10;
    default_font_pt = 10;
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

    // Reset back to defaults
    font_pt = 10;
    default_font_pt = 10;
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
    assert(mouse_mode == 0);
    assert(mouse_sgr == 0);

    // Enable DECSET 1000 (normal tracking)
    feed_bytes(&term, "\x1b[?1000h");
    assert(mouse_mode == 1000);

    // Enable DECSET 1002 (button event tracking)
    feed_bytes(&term, "\x1b[?1002h");
    assert(mouse_mode == 1002);

    // Enable DECSET 1006 (SGR mode)
    feed_bytes(&term, "\x1b[?1006h");
    assert(mouse_sgr == 1);

    // Disable DECSET 1006
    feed_bytes(&term, "\x1b[?1006l");
    assert(mouse_sgr == 0);

    // Disable DECSET 1000
    feed_bytes(&term, "\x1b[?1000l");
    assert(mouse_mode == 0);

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
    test_omarchy_colors_toml_parsing();
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
    test_synchronized_output_mode_2026();
    test_charset_designation_filtering();
    test_esc_reverse_index();
    test_wide_character_grid();
    test_mouse_mode_and_sgr_toggles();
    test_bracketed_paste_toggle();
    test_decscusr_cursor_style();

    printf("====================================================\n");
    printf("All %d/%d tests passed successfully!\n", tests_passed, tests_run);
    printf("====================================================\n");
    return 0;
}
