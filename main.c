#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <ctype.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pwd.h>
#include <signal.h>
#include <limits.h>
#include <wchar.h>
#include <locale.h>
#include <math.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#ifndef DATADIR
#define DATADIR "/usr/local/share"
#endif

#ifndef VERSION
#define VERSION "0.1"
#endif

#define DEFAULT_COLS 90
#define DEFAULT_ROWS 28
#define DEFAULT_PADDING 12
static int padding = DEFAULT_PADDING;
static int padding_x = DEFAULT_PADDING;
static int padding_y = DEFAULT_PADDING;
#define CLEAN_MASK(s) ((s) & ~(LockMask | Mod2Mask))
#define MAX_HIST_LINES 4096

// Default Palette
#define DEFAULT_COLOR_BG      0x00000000 // Pitch Black
#define DEFAULT_COLOR_FG      0x00E5E5E5 // Standard Light Gray / White
#define DEFAULT_COLOR_CURSOR  0x00FFFFFF // Fixed White
#define DEFAULT_COLOR_SEL_BG  0x00444444 // Neutral Dark Gray
#define DEFAULT_COLOR_SEL_FG  0x00FFFFFF // Pure White
#define DEFAULT_COLOR_HUD_BG  0x00444444 // Neutral Dark Gray (Scrubber HUD)
#define DEFAULT_COLOR_HUD_FG  0x00FFFFFF // Pure White

// Standard Xterm / ANSI 16-Color Palette
static const uint32_t default_ansi_palette[16] = {
    0x00000000, // 0:  Black
    0x00CD0000, // 1:  Red
    0x0000CD00, // 2:  Green
    0x00CDCD00, // 3:  Yellow
    0x000000EE, // 4:  Blue
    0x00CD00CD, // 5:  Magenta
    0x0000CDCD, // 6:  Cyan
    0x00E5E5E5, // 7:  White
    0x007F7F7F, // 8:  Bright Black (Gray)
    0x00FF0000, // 9:  Bright Red
    0x0000FF00, // 10: Bright Green
    0x00FFFF00, // 11: Bright Yellow
    0x005C5CFF, // 12: Bright Blue
    0x00FF00FF, // 13: Bright Magenta
    0x0000FFFF, // 14: Bright Cyan
    0x00FFFFFF  // 15: Bright White
};

static uint32_t color_bg = DEFAULT_COLOR_BG;
static uint32_t color_fg = DEFAULT_COLOR_FG;
static uint32_t color_sel_bg = DEFAULT_COLOR_SEL_BG;
static uint32_t color_sel_fg = DEFAULT_COLOR_SEL_FG;
static uint32_t color_hud_bg = DEFAULT_COLOR_HUD_BG;
static uint32_t color_hud_fg = DEFAULT_COLOR_HUD_FG;

#define COLOR_BG color_bg
#define COLOR_FG color_fg
#define COLOR_CURSOR 0x00FFFFFF
#define COLOR_SEL_BG color_sel_bg
#define COLOR_SEL_FG color_sel_fg
#define COLOR_HUD_BG color_hud_bg
#define COLOR_HUD_FG color_hud_fg

static uint32_t ansi_palette[16] = {
    0x002E3440, 0x00BF616A, 0x00A3BE8C, 0x00EBCB8B,
    0x0081A1C1, 0x00B48EAD, 0x0088C0D0, 0x00E5E9F0,
    0x004C566A, 0x00BF616A, 0x00A3BE8C, 0x00EBCB8B,
    0x0081A1C1, 0x00B48EAD, 0x008FBCBB, 0x00ECEFF4
};

// Fast Perceptual Gamma (~2.0) Blending LUTs
static uint16_t sq_lut[256];
static uint8_t sqrt_lut[65536];
static int gamma_lut_inited = 0;

static void init_gamma_lut(void) {
    if (gamma_lut_inited) return;
    for (int i = 0; i < 256; i++) {
        sq_lut[i] = (uint16_t)(i * i);
    }
    for (int i = 0; i < 65536; i++) {
        sqrt_lut[i] = (uint8_t)(sqrt((double)i) + 0.5);
    }
    gamma_lut_inited = 1;
}

static volatile sig_atomic_t sig_reload_theme = 0;

static void handle_sigusr1(int sig) {
    (void)sig;
    sig_reload_theme = 1;
}

#define FLAG_BOLD       (1 << 0)
#define FLAG_UNDERLINE  (1 << 1)
#define FLAG_INVERSE    (1 << 2)
#define FLAG_WIDE       (1 << 3)
#define FLAG_WIDE_DUMMY (1 << 4)

typedef struct {
    uint32_t codepoint;
    uint32_t fg;
    uint32_t bg;
    uint8_t flags;
} Cell;

enum TermState {
    STATE_NORMAL,
    STATE_ESC,
    STATE_CSI,
    STATE_OSC,
    STATE_APC,
    STATE_CHARSET
};

#define MAX_CSI_PARAMS 16

typedef struct {
    Cell *primary_grid;
    Cell *alt_grid;
    Cell *grid;
    int is_alt_screen;

    int cols, rows;
    int cursor_x, cursor_y;
    int saved_cursor_x, saved_cursor_y;
    int cursor_visible;
    uint32_t cur_fg, cur_bg;
    uint8_t cur_flags;

    enum TermState state;
    int csi_params[MAX_CSI_PARAMS];
    int csi_nparams;
    int csi_has_param;
    int csi_private;

    char osc_buf[1024];
    size_t osc_len;

    uint32_t utf8_cp;
    int utf8_remain;
} Terminal;

static Terminal live_term;
static Terminal replay_term;

static int cols = DEFAULT_COLS;
static int rows = DEFAULT_ROWS;
static uint8_t *dirty = NULL;
static int dirty_all = 1;

static inline void mark_line_dirty(int r, int max_r) {
    if (dirty && r >= 0 && r < max_r) {
        dirty[r] = 1;
    }
}

static int prev_cursor_x = 0;
static int prev_cursor_y = 0;

// Scrollback Buffer
static Cell *history[MAX_HIST_LINES];
static int hist_cols[MAX_HIST_LINES];
static int hist_head = 0;
static int hist_count = 0;
static int scroll_offset = 0;

// Font Metrics & Scaling
static int font_pt = 12;
static int default_font_pt = 12;
static char config_font_family[128] = {0};
static int display_dpi = 96;

static void update_display_dpi(Display *d) {
    if (!d) return;
    int screen = DefaultScreen(d);
    int w_px = DisplayWidth(d, screen);
    int w_mm = DisplayWidthMM(d, screen);
    if (w_mm > 0) {
        int dpi = (int)((w_px * 25.4) / w_mm + 0.5);
        if (dpi >= 72 && dpi <= 400) {
            display_dpi = dpi;
        }
    }
}

static inline int pt_to_px(int pt) {
    int px = (int)((pt * display_dpi) / 72.0 + 0.5);
    return (px < 6) ? 6 : px;
}
static int char_w = 11;
static int char_h = 24;
static int ascender_px = 19;
static int win_w = 0;
static int win_h = 0;
static uint32_t *pixels = NULL;

static int pty_master = -1;

static inline void pty_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || !buf || count == 0) return;
    const char *p = (const char *)buf;
    while (count > 0) {
        ssize_t n = write(fd, p, count);
        if (n <= 0) break;
        p += n;
        count -= (size_t)n;
    }
}
static int in_sync_update = 0;
static uint64_t sync_update_start_us = 0;
static int app_cursor_keys = 0;
static int mouse_mode = 0;       // 0=off, 1000=normal, 1002=btn-event, 1003=any-event
static int mouse_sgr = 0;        // 1=SGR 1006 extended mode
static int bracketed_paste = 0;  // DECSET 2004
static int default_cursor_style = 6;
static int cursor_style = 6;     // DECSCUSR: 0=default, 1..2=block, 3..4=underline, 5..6=beam
static int default_cursor_blink = 0;
static int cursor_blink_enabled = 0;
static int cursor_blink_state = 1;
static uint64_t last_cursor_blink_us = 0;

// Selection & Clipboard
static int sel_active = 0;
static int sel_start_c = -1, sel_start_r = -1;
static int sel_end_c = -1, sel_end_r = -1;
static char *sel_text = NULL;

static Atom atom_clipboard = 0;
static Atom atom_utf8 = 0;
static Atom atom_targets = 0;
static Atom atom_sel_data = 0;
static Atom atom_net_wm_name = 0;
static Atom atom_net_wm_icon_name = 0;
static Atom atom_net_wm_pid = 0;

// Modifiers and Keybindings
enum {
    BIND_MOD_CTRL  = (1 << 0),
    BIND_MOD_SHIFT = (1 << 1),
    BIND_MOD_ALT   = (1 << 2),
    BIND_MOD_SUPER = (1 << 3),
    BIND_MOD_MOD2  = (1 << 4),
    BIND_MOD_MOD3  = (1 << 5),
    BIND_MOD_MOD5  = (1 << 6)
};

enum {
    ACTION_NONE = 0,
    ACTION_COPY,
    ACTION_PASTE
};

typedef struct {
    unsigned int mods;
    KeySym ksym;
    int action;
} KeyBinding;

#define MAX_KEYBINDINGS 64
static KeyBinding keybindings[MAX_KEYBINDINGS];
static int keybinding_count = 0;
static unsigned int super_mod_mask = Mod4Mask;
static unsigned int alt_mod_mask = Mod1Mask;

static void detect_modifier_masks(Display *d) {
    if (!d) return;
    XModifierKeymap *modmap = XGetModifierMapping(d);
    if (!modmap) return;
    for (int m = 0; m < 8; m++) {
        for (int k = 0; k < modmap->max_keypermod; k++) {
            KeyCode kc = modmap->modifiermap[m * modmap->max_keypermod + k];
            if (!kc) continue;
            KeySym ks = XkbKeycodeToKeysym(d, kc, 0, 0);
            if (ks == XK_Super_L || ks == XK_Super_R) {
                super_mod_mask = (1U << m);
            } else if (ks == XK_Alt_L || ks == XK_Alt_R) {
                alt_mod_mask = (1U << m);
            }
        }
    }
    XFreeModifiermap(modmap);
}

static KeySym parse_keysym_name(const char *name) {
    if (!name || !*name) return NoSymbol;
    if (strlen(name) == 1) {
        char ch = (char)tolower((unsigned char)name[0]);
        if (ch >= 'a' && ch <= 'z') return (KeySym)(XK_a + (ch - 'a'));
        if (ch >= '0' && ch <= '9') return (KeySym)(XK_0 + (ch - '0'));
    }
    if (strcasecmp(name, "insert") == 0) return XK_Insert;
    if (strcasecmp(name, "kp_insert") == 0) return XK_KP_Insert;
    if (strcasecmp(name, "delete") == 0 || strcasecmp(name, "del") == 0) return XK_Delete;
    if (strcasecmp(name, "return") == 0 || strcasecmp(name, "enter") == 0) return XK_Return;
    if (strcasecmp(name, "backspace") == 0) return XK_BackSpace;
    if (strcasecmp(name, "tab") == 0) return XK_Tab;
    if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0) return XK_Escape;
    if (strcasecmp(name, "pageup") == 0 || strcasecmp(name, "page_up") == 0) return XK_Page_Up;
    if (strcasecmp(name, "pagedown") == 0 || strcasecmp(name, "page_down") == 0) return XK_Page_Down;
    if (strcasecmp(name, "home") == 0) return XK_Home;
    if (strcasecmp(name, "end") == 0) return XK_End;
    if (strcasecmp(name, "left") == 0) return XK_Left;
    if (strcasecmp(name, "right") == 0) return XK_Right;
    if (strcasecmp(name, "up") == 0) return XK_Up;
    if (strcasecmp(name, "down") == 0) return XK_Down;

    KeySym ks = XStringToKeysym(name);
    if (ks != NoSymbol) return ks;

    char buf[64];
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    buf[0] = (char)toupper((unsigned char)buf[0]);
    for (size_t i = 1; i < strlen(buf); i++) buf[i] = (char)tolower((unsigned char)buf[i]);
    return XStringToKeysym(buf);
}

static int parse_key_combo(const char *str, unsigned int *out_mods, KeySym *out_ksym) {
    if (!str || !*str || !out_mods || !out_ksym) return 0;
    char buf[128];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    unsigned int mods = 0;
    KeySym ksym = NoSymbol;

    char *saveptr = NULL;
    char *token = strtok_r(buf, "+-", &saveptr);
    while (token) {
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end >= token && (*end == ' ' || *end == '\t')) { *end = '\0'; end--; }

        if (*token) {
            if (strcasecmp(token, "ctrl") == 0 || strcasecmp(token, "control") == 0) {
                mods |= BIND_MOD_CTRL;
            } else if (strcasecmp(token, "shift") == 0) {
                mods |= BIND_MOD_SHIFT;
            } else if (strcasecmp(token, "alt") == 0 || strcasecmp(token, "mod1") == 0 || strcasecmp(token, "meta") == 0) {
                mods |= BIND_MOD_ALT;
            } else if (strcasecmp(token, "super") == 0 || strcasecmp(token, "mod") == 0 ||
                       strcasecmp(token, "mod4") == 0 || strcasecmp(token, "win") == 0 ||
                       strcasecmp(token, "cmd") == 0) {
                mods |= BIND_MOD_SUPER;
            } else if (strcasecmp(token, "mod2") == 0) {
                mods |= BIND_MOD_MOD2;
            } else if (strcasecmp(token, "mod3") == 0) {
                mods |= BIND_MOD_MOD3;
            } else if (strcasecmp(token, "mod5") == 0) {
                mods |= BIND_MOD_MOD5;
            } else {
                KeySym ks = parse_keysym_name(token);
                if (ks != NoSymbol) {
                    ksym = ks;
                }
            }
        }
        token = strtok_r(NULL, "+-", &saveptr);
    }

    if (ksym == NoSymbol) return 0;
    *out_mods = mods;
    *out_ksym = ksym;
    return 1;
}

static void add_or_update_keybinding(unsigned int mods, KeySym ksym, int action) {
    for (int i = 0; i < keybinding_count; i++) {
        if (keybindings[i].mods == mods && keybindings[i].ksym == ksym) {
            if (action == ACTION_NONE) {
                for (int j = i; j < keybinding_count - 1; j++) {
                    keybindings[j] = keybindings[j + 1];
                }
                keybinding_count--;
            } else {
                keybindings[i].action = action;
            }
            return;
        }
    }
    if (keybinding_count < MAX_KEYBINDINGS && action != ACTION_NONE) {
        keybindings[keybinding_count].mods = mods;
        keybindings[keybinding_count].ksym = ksym;
        keybindings[keybinding_count].action = action;
        keybinding_count++;
    }
}

static void clear_keybindings_for_action(int action) {
    int w = 0;
    for (int i = 0; i < keybinding_count; i++) {
        if (keybindings[i].action != action) {
            keybindings[w++] = keybindings[i];
        }
    }
    keybinding_count = w;
}

static void init_default_keybindings(void) {
    keybinding_count = 0;
    add_or_update_keybinding(BIND_MOD_CTRL | BIND_MOD_SHIFT, XK_c, ACTION_COPY);
    add_or_update_keybinding(BIND_MOD_SUPER, XK_c, ACTION_COPY);
    add_or_update_keybinding(BIND_MOD_CTRL, XK_Insert, ACTION_COPY);
    add_or_update_keybinding(BIND_MOD_SUPER, XK_Insert, ACTION_COPY);

    add_or_update_keybinding(BIND_MOD_CTRL | BIND_MOD_SHIFT, XK_v, ACTION_PASTE);
    add_or_update_keybinding(BIND_MOD_SUPER, XK_v, ACTION_PASTE);
    add_or_update_keybinding(BIND_MOD_SHIFT, XK_Insert, ACTION_PASTE);
}

static unsigned int state_to_sym_mods(unsigned int state) {
    unsigned int sym_mods = 0;
    if (state & ControlMask) sym_mods |= BIND_MOD_CTRL;
    if (state & ShiftMask) sym_mods |= BIND_MOD_SHIFT;
    if (state & alt_mod_mask) sym_mods |= BIND_MOD_ALT;
    if (state & super_mod_mask) sym_mods |= BIND_MOD_SUPER;
    if ((state & Mod3Mask) && super_mod_mask != Mod3Mask && alt_mod_mask != Mod3Mask) sym_mods |= BIND_MOD_MOD3;
    if ((state & Mod5Mask) && super_mod_mask != Mod5Mask && alt_mod_mask != Mod5Mask) sym_mods |= BIND_MOD_MOD5;
    return sym_mods;
}

static int match_keybinding(unsigned int state, KeySym ksym) {
    unsigned int sym_mods = state_to_sym_mods(CLEAN_MASK(state));
    for (int i = 0; i < keybinding_count; i++) {
        if (keybindings[i].mods == sym_mods) {
            KeySym bks = keybindings[i].ksym;
            if (bks == ksym) return keybindings[i].action;
            if (bks >= XK_a && bks <= XK_z && (ksym == bks || ksym == (bks - XK_a + XK_A))) {
                return keybindings[i].action;
            }
            if (bks >= XK_A && bks <= XK_Z && (ksym == bks || ksym == (bks - XK_A + XK_a))) {
                return keybindings[i].action;
            }
            if ((bks == XK_Insert || bks == XK_KP_Insert) && (ksym == XK_Insert || ksym == XK_KP_Insert)) {
                return keybindings[i].action;
            }
            if ((bks == XK_Return || bks == XK_KP_Enter) && (ksym == XK_Return || ksym == XK_KP_Enter)) {
                return keybindings[i].action;
            }
        }
    }
    return ACTION_NONE;
}

static void parse_keybind_line(const char *val) {
    if (!val || !*val) return;
    char buf[128];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *sep = strchr(buf, '=');
    if (!sep) sep = strchr(buf, ':');
    char *action_str = NULL;
    if (sep) {
        *sep = '\0';
        action_str = sep + 1;
    } else {
        char *sp = strrchr(buf, ' ');
        if (sp) {
            *sp = '\0';
            action_str = sp + 1;
        }
    }
    if (!action_str) return;
    while (*action_str == ' ' || *action_str == '\t') action_str++;
    char *act_end = action_str + strlen(action_str) - 1;
    while (act_end >= action_str && (*act_end == ' ' || *act_end == '\t')) { *act_end = '\0'; act_end--; }

    int act = ACTION_NONE;
    if (strcasecmp(action_str, "copy") == 0 || strcasecmp(action_str, "copy_to_clipboard") == 0) {
        act = ACTION_COPY;
    } else if (strcasecmp(action_str, "paste") == 0 || strcasecmp(action_str, "paste_from_clipboard") == 0) {
        act = ACTION_PASTE;
    } else if (strcasecmp(action_str, "none") == 0 || strcasecmp(action_str, "unbind") == 0 || strcasecmp(action_str, "disabled") == 0) {
        act = ACTION_NONE;
    } else {
        return;
    }

    unsigned int mods = 0;
    KeySym ksym = NoSymbol;
    if (parse_key_combo(buf, &mods, &ksym)) {
        add_or_update_keybinding(mods, ksym, act);
    }
}

static void parse_key_list(const char *val, int action) {
    if (!val) return;
    clear_keybindings_for_action(action);
    char buf[256];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);
    while (token) {
        unsigned int mods = 0;
        KeySym ksym = NoSymbol;
        if (parse_key_combo(token, &mods, &ksym)) {
            add_or_update_keybinding(mods, ksym, action);
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
}

// FreeType & Glyph Cache
static FT_Library ft_lib;
static FT_Face ft_face;

#define MAX_FALLBACK_FACES 16
static FT_Face fallback_faces[MAX_FALLBACK_FACES];
static int num_fallback_faces = 0;

#define GLYPH_CACHE_SIZE 4096
typedef struct {
    uint32_t codepoint;
    int width;
    int height;
    int left;
    int top;
    int is_color; // 0 = 8-bit alpha mask, 1 = 32-bit BGRA
    uint8_t *bitmap;
} CachedGlyph;

static CachedGlyph glyph_cache[GLYPH_CACHE_SIZE];

// Flight Recorder & Time Machine
#define MAX_FLIGHT_CHUNKS 4096

typedef struct {
    uint64_t ts_us;
    uint32_t len;
    char *data;
} FlightChunk;

static FlightChunk flight_log[MAX_FLIGHT_CHUNKS];
static int flight_head = 0;
static int flight_count = 0;
static int flight_total_recorded = 0;
static uint64_t flight_start_us = 0;

static int replay_mode = 0;
static int replay_chunk_idx = 0;
static char hud_message[256] = {0};

static Display *dpy = NULL;
static Window win = 0;
static GC gc = 0;
static XImage *img = NULL;
static Visual *vis = NULL;
static int depth = 0;

static int shm_available = 0;
static XShmSegmentInfo shm_info;
static volatile int shm_error_flag = 0;
static Atom atom_net_wm_icon = 0;

static int shm_error_handler(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    shm_error_flag = 1;
    return 0;
}

static void destroy_framebuffer(void) {
    if (img) {
        if (shm_available) {
            XSync(dpy, False);
            XShmDetach(dpy, &shm_info);
            shmdt(shm_info.shmaddr);
            shmctl(shm_info.shmid, IPC_RMID, 0);
            img->data = NULL;
            XDestroyImage(img);
            img = NULL;
            pixels = NULL;
            shm_available = 0;
        } else {
            img->data = NULL;
            XDestroyImage(img);
            img = NULL;
            free(pixels);
            pixels = NULL;
        }
    } else if (pixels) {
        free(pixels);
        pixels = NULL;
    }
}

static int init_framebuffer(int w, int h) {
    destroy_framebuffer();
    win_w = w;
    win_h = h;

    if (XShmQueryExtension(dpy)) {
        memset(&shm_info, 0, sizeof(shm_info));
        shm_info.shmaddr = (char *)-1;
        img = XShmCreateImage(dpy, vis, depth, ZPixmap, NULL, &shm_info, win_w, win_h);
        if (img) {
            size_t size = (size_t)img->bytes_per_line * (size_t)img->height;
            shm_info.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0777);
            if (shm_info.shmid >= 0) {
                shm_info.shmaddr = shmat(shm_info.shmid, 0, 0);
                if (shm_info.shmaddr != (char *)-1) {
                    img->data = shm_info.shmaddr;
                    pixels = (uint32_t *)img->data;
                    shm_info.readOnly = False;

                    shm_error_flag = 0;
                    XErrorHandler old_handler = XSetErrorHandler(shm_error_handler);
                    XShmAttach(dpy, &shm_info);
                    XSync(dpy, False);
                    XSetErrorHandler(old_handler);

                    if (!shm_error_flag) {
                        shmctl(shm_info.shmid, IPC_RMID, 0);
                        shm_available = 1;
                        for (size_t i = 0; i < (size_t)(win_w * win_h); i++) {
                            pixels[i] = COLOR_BG;
                        }
                        return 0;
                    }
                    shmdt(shm_info.shmaddr);
                }
                shmctl(shm_info.shmid, IPC_RMID, 0);
            }
            img->data = NULL;
            XDestroyImage(img);
            img = NULL;
        }
    }

    shm_available = 0;
    pixels = malloc(win_w * win_h * sizeof(uint32_t));
    if (!pixels) return -1;
    for (size_t i = 0; i < (size_t)(win_w * win_h); i++) {
        pixels[i] = COLOR_BG;
    }
    img = XCreateImage(dpy, vis, depth, ZPixmap, 0, (char *)pixels, win_w, win_h, 32, 0);
    if (!img) {
        free(pixels);
        pixels = NULL;
        return -1;
    }
    return 0;
}

static void blit_subimage(int src_x, int src_y, int dst_x, int dst_y, unsigned int w, unsigned int h) {
    if (!img || w == 0 || h == 0) return;
    if (shm_available) {
        XShmPutImage(dpy, win, gc, img, src_x, src_y, dst_x, dst_y, w, h, False);
    } else {
        XPutImage(dpy, win, gc, img, src_x, src_y, dst_x, dst_y, w, h);
    }
}

static void setup_window_icon(Display *d, Window w) {
    if (!atom_net_wm_icon) atom_net_wm_icon = XInternAtom(d, "_NET_WM_ICON", False);
    const int count16 = 16 * 16;
    const int count32 = 32 * 32;
    const int total = 2 + count16 + 2 + count32;
    unsigned long *data = calloc(total, sizeof(unsigned long));
    if (!data) return;

    const unsigned long C_TRANS = 0x00000000;
    const unsigned long C_BORDER = 0xFF4C566A;
    const unsigned long C_TITLEBG = 0xFF3B4252;
    const unsigned long C_BG = 0xFF2E3440;
    const unsigned long C_DOT_R = 0xFFBF616A;
    const unsigned long C_DOT_Y = 0xFFEBCB8B;
    const unsigned long C_DOT_G = 0xFFA3BE8C;
    const unsigned long C_PROMPT = 0xFF88C0D0;
    const unsigned long C_CURSOR = 0xFFFFFFFF;

    // 16x16 icon
    data[0] = 16;
    data[1] = 16;
    unsigned long *p16 = &data[2];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            unsigned long c = C_BG;
            if ((y == 0 && (x == 0 || x == 15)) || (y == 15 && (x == 0 || x == 15))) {
                c = C_TRANS;
            } else if (y == 0 || y == 15 || x == 0 || x == 15) {
                c = C_BORDER;
            } else if (y == 1 || y == 2) {
                c = C_TITLEBG;
                if (y == 1) {
                    if (x == 2) c = C_DOT_R;
                    else if (x == 4) c = C_DOT_Y;
                    else if (x == 6) c = C_DOT_G;
                }
            } else if (y == 3) {
                c = C_BORDER;
            }
            p16[y * 16 + x] = c;
        }
    }
    p16[5 * 16 + 3] = C_PROMPT; p16[5 * 16 + 4] = C_PROMPT;
    p16[6 * 16 + 4] = C_PROMPT; p16[6 * 16 + 5] = C_PROMPT;
    p16[7 * 16 + 5] = C_PROMPT; p16[7 * 16 + 6] = C_PROMPT;
    p16[8 * 16 + 5] = C_PROMPT; p16[8 * 16 + 6] = C_PROMPT;
    p16[9 * 16 + 4] = C_PROMPT; p16[9 * 16 + 5] = C_PROMPT;
    p16[10 * 16 + 3] = C_PROMPT; p16[10 * 16 + 4] = C_PROMPT;
    for (int cx = 8; cx <= 12; cx++) {
        p16[10 * 16 + cx] = C_CURSOR;
        p16[11 * 16 + cx] = C_CURSOR;
    }

    // 32x32 icon
    int off32 = 2 + count16;
    data[off32] = 32;
    data[off32 + 1] = 32;
    unsigned long *p32 = &data[off32 + 2];
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            unsigned long c = C_BG;
            int corner = ((y < 2 && (x < 2 || x >= 30)) || (y >= 30 && (x < 2 || x >= 30)));
            if (corner) {
                c = C_TRANS;
            } else if (y == 0 || y == 31 || x == 0 || x == 31 || y == 1 || y == 30 || x == 1 || x == 30) {
                c = C_BORDER;
            } else if (y >= 2 && y <= 6) {
                c = C_TITLEBG;
                if (y >= 3 && y <= 5) {
                    if (x >= 4 && x <= 6) c = C_DOT_R;
                    else if (x >= 8 && x <= 10) c = C_DOT_Y;
                    else if (x >= 12 && x <= 14) c = C_DOT_G;
                }
            } else if (y == 7) {
                c = C_BORDER;
            }
            p32[y * 32 + x] = c;
        }
    }
    for (int i = 0; i < 4; i++) {
        int py_top = 11 + i * 2;
        int py_bot = 23 - i * 2;
        int px = 5 + i * 2;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                p32[(py_top + dy) * 32 + (px + dx)] = C_PROMPT;
                p32[(py_bot - dy) * 32 + (px + dx)] = C_PROMPT;
            }
        }
    }
    for (int cy = 22; cy <= 24; cy++) {
        for (int cx = 16; cx <= 24; cx++) {
            p32[cy * 32 + cx] = C_CURSOR;
        }
    }

    XChangeProperty(d, w, atom_net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)data, total);
    free(data);
}

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static void scale_bgra_box(const uint8_t *src, int sw, int sh, int spitch,
                           uint8_t *dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int dy = 0; dy < dh; dy++) {
        int sy0 = (dy * sh) / dh;
        int sy1 = ((dy + 1) * sh) / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        uint32_t *dst_row = (uint32_t *)(dst + dy * dw * 4);
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = (dx * sw) / dw;
            int sx1 = ((dx + 1) * sw) / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, count = 0;
            for (int sy = sy0; sy < sy1 && sy < sh; sy++) {
                const uint32_t *srow = (const uint32_t *)(src + sy * spitch);
                for (int sx = sx0; sx < sx1 && sx < sw; sx++) {
                    uint32_t px = srow[sx];
                    b += (px & 0xFF);
                    g += ((px >> 8) & 0xFF);
                    r += ((px >> 16) & 0xFF);
                    a += ((px >> 24) & 0xFF);
                    count++;
                }
            }
            if (count > 0) {
                b /= count; g /= count; r /= count; a /= count;
            }
            dst_row[dx] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void clear_glyph_cache(void) {
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        if (glyph_cache[i].bitmap) {
            free(glyph_cache[i].bitmap);
            glyph_cache[i].bitmap = NULL;
        }
        glyph_cache[i].codepoint = 0;
        glyph_cache[i].is_color = 0;
    }
}

static void set_window_title(const char *title) {
    if (!dpy || !win) return;
    if (!title) title = "DWM-Terminal";

    XStoreName(dpy, win, title);
    XSetIconName(dpy, win, title);

    if (!atom_net_wm_name) atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    if (!atom_net_wm_icon_name) atom_net_wm_icon_name = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
    if (!atom_utf8) atom_utf8 = XInternAtom(dpy, "UTF8_STRING", False);

    int len = (int)strlen(title);
    XChangeProperty(dpy, win, atom_net_wm_name, atom_utf8, 8, PropModeReplace,
                    (const unsigned char *)title, len);
    XChangeProperty(dpy, win, atom_net_wm_icon_name, atom_utf8, 8, PropModeReplace,
                    (const unsigned char *)title, len);
}

static CachedGlyph *get_glyph(uint32_t cp) {
    if (cp == 0 || cp == ' ' || !ft_face) return NULL;
    uint32_t hash = (cp ^ (cp >> 16)) * 0x45D9F3B;
    uint32_t idx = hash % GLYPH_CACHE_SIZE;

    for (int i = 0; i < 16; i++) {
        uint32_t slot = (idx + i) % GLYPH_CACHE_SIZE;
        if (glyph_cache[slot].codepoint == cp) {
            return &glyph_cache[slot];
        }
        if (glyph_cache[slot].codepoint == 0) {
            FT_UInt g_idx = FT_Get_Char_Index(ft_face, cp);
            FT_Face target_face = ft_face;

            if (g_idx == 0) {
                for (int f = 0; f < num_fallback_faces; f++) {
                    g_idx = FT_Get_Char_Index(fallback_faces[f], cp);
                    if (g_idx != 0) {
                        target_face = fallback_faces[f];
                        break;
                    }
                }
            }

            if (g_idx == 0 && num_fallback_faces < MAX_FALLBACK_FACES) {
                FcPattern *pat = FcPatternCreate();
                if (pat) {
                    FcCharSet *cs = FcCharSetCreate();
                    if (cs) {
                        FcCharSetAddChar(cs, cp);
                        FcPatternAddCharSet(pat, FC_CHARSET, cs);
                        FcConfigSubstitute(NULL, pat, FcMatchPattern);
                        FcDefaultSubstitute(pat);
                        FcResult res;
                        FcPattern *match = FcFontMatch(NULL, pat, &res);
                        if (match) {
                            FcChar8 *file = NULL;
                            int f_idx = 0;
                            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
                                FcPatternGetInteger(match, FC_INDEX, 0, &f_idx);
                                FT_Face new_face = NULL;
                                if (FT_New_Face(ft_lib, (const char *)file, f_idx, &new_face) == 0) {
                                    if (FT_IS_SCALABLE(new_face)) {
                                        FT_Set_Pixel_Sizes(new_face, 0, pt_to_px(font_pt));
                                    } else if (new_face->num_fixed_sizes > 0) {
                                        FT_Select_Size(new_face, 0);
                                    }
                                    fallback_faces[num_fallback_faces++] = new_face;
                                    target_face = new_face;
                                    g_idx = FT_Get_Char_Index(new_face, cp);
                                }
                            }
                            FcPatternDestroy(match);
                        }
                        FcCharSetDestroy(cs);
                    }
                    FcPatternDestroy(pat);
                }
            }

            if (g_idx == 0) {
                return NULL;
            }

            int load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT;
            if (!FT_IS_SCALABLE(target_face) || target_face->num_fixed_sizes > 0) {
                load_flags |= FT_LOAD_COLOR;
            }
            if (FT_Load_Glyph(target_face, g_idx, load_flags) != 0) {
                return NULL;
            }

            FT_GlyphSlot g = target_face->glyph;
            glyph_cache[slot].codepoint = cp;

            if (g->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                int max_w = char_w * 2;
                int max_h = char_h;
                int sw = g->bitmap.width;
                int sh = g->bitmap.rows;
                int dw = max_w - 2;
                int dh = max_h - 2;
                if (dw <= 0) dw = max_w;
                if (dh <= 0) dh = max_h;
                if (sw > 0 && sh > 0) {
                    if (dw * sh > dh * sw) {
                        dw = (dh * sw) / sh;
                    } else {
                        dh = (dw * sh) / sw;
                    }
                }
                if (dw <= 0) dw = 1;
                if (dh <= 0) dh = 1;

                uint8_t *scaled = malloc((size_t)dw * dh * 4);
                if (!scaled) return NULL;
                scale_bgra_box(g->bitmap.buffer, sw, sh, g->bitmap.pitch, scaled, dw, dh);

                glyph_cache[slot].width = dw;
                glyph_cache[slot].height = dh;
                glyph_cache[slot].left = (max_w - dw) / 2;
                glyph_cache[slot].top = ascender_px - (max_h - dh) / 2;
                glyph_cache[slot].is_color = 1;
                glyph_cache[slot].bitmap = scaled;
            } else {
                glyph_cache[slot].width = g->bitmap.width;
                glyph_cache[slot].height = g->bitmap.rows;
                glyph_cache[slot].left = g->bitmap_left;
                glyph_cache[slot].top = g->bitmap_top;
                glyph_cache[slot].is_color = 0;

                size_t sz = (size_t)g->bitmap.width * g->bitmap.rows;
                if (sz > 0) {
                    glyph_cache[slot].bitmap = malloc(sz);
                    if (!glyph_cache[slot].bitmap) return NULL;
                    memcpy(glyph_cache[slot].bitmap, g->bitmap.buffer, sz);
                } else {
                    glyph_cache[slot].bitmap = NULL;
                }
            }
            return &glyph_cache[slot];
        }
    }
    return NULL;
}

static uint32_t get_256_color(uint8_t idx) {
    if (idx < 16) return ansi_palette[idx];
    if (idx < 232) {
        idx -= 16;
        uint8_t r = idx / 36;
        uint8_t g = (idx / 6) % 6;
        uint8_t b = idx % 6;
        uint32_t cr = r ? (r * 40 + 55) : 0;
        uint32_t cg = g ? (g * 40 + 55) : 0;
        uint32_t cb = b ? (b * 40 + 55) : 0;
        return (cr << 16) | (cg << 8) | cb;
    }
    uint32_t gray = (idx - 232) * 10 + 8;
    return (gray << 16) | (gray << 8) | gray;
}

static void term_init(Terminal *t, int c, int r) {
    init_gamma_lut();
    memset(t, 0, sizeof(Terminal));
    t->cols = c;
    t->rows = r;
    t->primary_grid = calloc(c * r, sizeof(Cell));
    t->alt_grid = calloc(c * r, sizeof(Cell));
    for (int i = 0; i < c * r; i++) {
        t->primary_grid[i] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
        t->alt_grid[i] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
    }
    t->is_alt_screen = 0;
    t->grid = t->primary_grid;
    t->cursor_x = 0;
    t->cursor_y = 0;
    t->saved_cursor_x = 0;
    t->saved_cursor_y = 0;
    t->cursor_visible = 1;
    t->cur_fg = COLOR_FG;
    t->cur_bg = COLOR_BG;
    t->cur_flags = 0;
    t->state = STATE_NORMAL;
    t->utf8_remain = 0;
}

static void term_scroll_internal(Terminal *t, int is_live) {
    if (is_live && !t->is_alt_screen) {
        if (history[hist_head] == NULL || hist_cols[hist_head] != cols) {
            history[hist_head] = realloc(history[hist_head], cols * sizeof(Cell));
            hist_cols[hist_head] = cols;
        }
        memcpy(history[hist_head], &t->grid[0], cols * sizeof(Cell));
        hist_head = (hist_head + 1) % MAX_HIST_LINES;
        if (hist_count < MAX_HIST_LINES) hist_count++;
    }

    memmove(&t->grid[0], &t->grid[cols], sizeof(Cell) * cols * (t->rows - 1));
    for (int c = 0; c < cols; c++) {
        t->grid[(t->rows - 1) * cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
    }
    t->cursor_y = t->rows - 1;
    if (is_live) dirty_all = 1;
}

static void handle_sgr_internal(Terminal *t) {
    if (!t->csi_has_param && t->csi_nparams == 0) {
        t->cur_fg = COLOR_FG;
        t->cur_bg = COLOR_BG;
        t->cur_flags = 0;
        return;
    }

    int count = t->csi_has_param ? t->csi_nparams + 1 : 0;
    for (int i = 0; i < count; i++) {
        int p = t->csi_params[i];
        if (p == 0) {
            t->cur_fg = COLOR_FG;
            t->cur_bg = COLOR_BG;
            t->cur_flags = 0;
        } else if (p == 1) {
            t->cur_flags |= FLAG_BOLD;
        } else if (p == 4) {
            t->cur_flags |= FLAG_UNDERLINE;
        } else if (p == 7) {
            t->cur_flags |= FLAG_INVERSE;
        } else if (p == 22) {
            t->cur_flags &= ~FLAG_BOLD;
        } else if (p == 24) {
            t->cur_flags &= ~FLAG_UNDERLINE;
        } else if (p == 27) {
            t->cur_flags &= ~FLAG_INVERSE;
        } else if (p >= 30 && p <= 37) {
            t->cur_fg = ansi_palette[p - 30];
        } else if (p == 38) {
            if (i + 2 < count && t->csi_params[i + 1] == 5) {
                t->cur_fg = get_256_color(t->csi_params[i + 2] & 0xFF);
                i += 2;
            } else if (i + 4 < count && t->csi_params[i + 1] == 2) {
                uint32_t r = t->csi_params[i + 2] & 0xFF;
                uint32_t g = t->csi_params[i + 3] & 0xFF;
                uint32_t b = t->csi_params[i + 4] & 0xFF;
                t->cur_fg = (r << 16) | (g << 8) | b;
                i += 4;
            }
        } else if (p == 39) {
            t->cur_fg = COLOR_FG;
        } else if (p >= 40 && p <= 47) {
            t->cur_bg = ansi_palette[p - 40];
        } else if (p == 48) {
            if (i + 2 < count && t->csi_params[i + 1] == 5) {
                t->cur_bg = get_256_color(t->csi_params[i + 2] & 0xFF);
                i += 2;
            } else if (i + 4 < count && t->csi_params[i + 1] == 2) {
                uint32_t r = t->csi_params[i + 2] & 0xFF;
                uint32_t g = t->csi_params[i + 3] & 0xFF;
                uint32_t b = t->csi_params[i + 4] & 0xFF;
                t->cur_bg = (r << 16) | (g << 8) | b;
                i += 4;
            }
        } else if (p == 49) {
            t->cur_bg = COLOR_BG;
        } else if (p >= 90 && p <= 97) {
            t->cur_fg = ansi_palette[(p - 90) + 8];
        } else if (p >= 100 && p <= 107) {
            t->cur_bg = ansi_palette[(p - 100) + 8];
        }
    }
}

static void handle_csi_internal(Terminal *t, unsigned char final_char, int is_live) {
    int p1 = t->csi_has_param ? t->csi_params[0] : 0;
    int p2 = (t->csi_nparams >= 1) ? t->csi_params[1] : 0;

    switch (final_char) {
        case '@': {
            int n = (p1 > 0) ? p1 : 1;
            if (n > t->cols - t->cursor_x) n = t->cols - t->cursor_x;
            if (n > 0) {
                int row_start = t->cursor_y * t->cols;
                memmove(&t->grid[row_start + t->cursor_x + n],
                        &t->grid[row_start + t->cursor_x],
                        (size_t)(t->cols - t->cursor_x - n) * sizeof(Cell));
                for (int c = 0; c < n; c++) {
                    t->grid[row_start + t->cursor_x + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                }
                if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            }
            break;
        }
        case 'P': {
            int n = (p1 > 0) ? p1 : 1;
            if (n > t->cols - t->cursor_x) n = t->cols - t->cursor_x;
            if (n > 0) {
                int row_start = t->cursor_y * t->cols;
                memmove(&t->grid[row_start + t->cursor_x],
                        &t->grid[row_start + t->cursor_x + n],
                        (size_t)(t->cols - t->cursor_x - n) * sizeof(Cell));
                for (int c = t->cols - n; c < t->cols; c++) {
                    t->grid[row_start + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                }
                if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            }
            break;
        }
        case 'X': {
            int n = (p1 > 0) ? p1 : 1;
            if (n > t->cols - t->cursor_x) n = t->cols - t->cursor_x;
            int row_start = t->cursor_y * t->cols;
            for (int c = 0; c < n; c++) {
                t->grid[row_start + t->cursor_x + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
            }
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'L': {
            int n = (p1 > 0) ? p1 : 1;
            if (n > t->rows - t->cursor_y) n = t->rows - t->cursor_y;
            if (n > 0) {
                memmove(&t->grid[(t->cursor_y + n) * t->cols],
                        &t->grid[t->cursor_y * t->cols],
                        (size_t)(t->rows - t->cursor_y - n) * t->cols * sizeof(Cell));
                for (int r = t->cursor_y; r < t->cursor_y + n; r++) {
                    for (int c = 0; c < t->cols; c++) {
                        t->grid[r * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                    }
                }
                if (is_live) dirty_all = 1;
            }
            break;
        }
        case 'M': {
            int n = (p1 > 0) ? p1 : 1;
            if (n > t->rows - t->cursor_y) n = t->rows - t->cursor_y;
            if (n > 0) {
                memmove(&t->grid[t->cursor_y * t->cols],
                        &t->grid[(t->cursor_y + n) * t->cols],
                        (size_t)(t->rows - t->cursor_y - n) * t->cols * sizeof(Cell));
                for (int r = t->rows - n; r < t->rows; r++) {
                    for (int c = 0; c < t->cols; c++) {
                        t->grid[r * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                    }
                }
                if (is_live) dirty_all = 1;
            }
            break;
        }
        case 'H':
        case 'f': {
            int r = (p1 > 0) ? p1 - 1 : 0;
            int c = (p2 > 0) ? p2 - 1 : 0;
            t->cursor_y = (r < t->rows) ? r : t->rows - 1;
            t->cursor_x = (c < t->cols) ? c : t->cols - 1;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'A': {
            int n = (p1 > 0) ? p1 : 1;
            t->cursor_y = (t->cursor_y >= n) ? t->cursor_y - n : 0;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'B': {
            int n = (p1 > 0) ? p1 : 1;
            t->cursor_y = (t->cursor_y + n < t->rows) ? t->cursor_y + n : t->rows - 1;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'C': {
            int n = (p1 > 0) ? p1 : 1;
            t->cursor_x = (t->cursor_x + n < t->cols) ? t->cursor_x + n : t->cols - 1;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'D': {
            int n = (p1 > 0) ? p1 : 1;
            t->cursor_x = (t->cursor_x >= n) ? t->cursor_x - n : 0;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'G': {
            int c = (p1 > 0) ? p1 - 1 : 0;
            t->cursor_x = (c < t->cols) ? c : t->cols - 1;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'd': {
            int r = (p1 > 0) ? p1 - 1 : 0;
            t->cursor_y = (r < t->rows) ? r : t->rows - 1;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'J': {
            if (p1 == 0) {
                for (int c = t->cursor_x; c < t->cols; c++) t->grid[t->cursor_y * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                for (int r = t->cursor_y + 1; r < t->rows; r++) {
                    for (int c = 0; c < t->cols; c++) t->grid[r * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                    if (is_live) mark_line_dirty(r, t->rows);
                }
                if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            } else if (p1 == 1) {
                for (int r = 0; r < t->cursor_y; r++) {
                    for (int c = 0; c < t->cols; c++) t->grid[r * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                    if (is_live) mark_line_dirty(r, t->rows);
                }
                for (int c = 0; c <= t->cursor_x && c < t->cols; c++) t->grid[t->cursor_y * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            } else if (p1 == 2) {
                for (int r = 0; r < t->rows; r++) {
                    for (int c = 0; c < t->cols; c++) t->grid[r * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                }
                if (is_live) dirty_all = 1;
            } else if (p1 == 3) {
                for (int r = 0; r < t->rows; r++) {
                    for (int c = 0; c < t->cols; c++) t->grid[r * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                }
                if (is_live) {
                    for (int i = 0; i < MAX_HIST_LINES; i++) {
                        if (history[i]) { free(history[i]); history[i] = NULL; }
                        hist_cols[i] = 0;
                    }
                    hist_head = 0;
                    hist_count = 0;
                    scroll_offset = 0;
                    dirty_all = 1;
                }
            }
            break;
        }
        case 'K': {
            if (p1 == 0) {
                for (int c = t->cursor_x; c < t->cols; c++) t->grid[t->cursor_y * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
            } else if (p1 == 1) {
                for (int c = 0; c <= t->cursor_x && c < t->cols; c++) t->grid[t->cursor_y * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
            } else if (p1 == 2) {
                for (int c = 0; c < t->cols; c++) t->grid[t->cursor_y * t->cols + c] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
            }
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        }
        case 'h': {
            int count = t->csi_has_param ? t->csi_nparams + 1 : 0;
            for (int i = 0; i < count; i++) {
                int p = t->csi_params[i];
                if (t->csi_private) {
                    if (p == 1049) {
                        t->saved_cursor_x = t->cursor_x;
                        t->saved_cursor_y = t->cursor_y;
                        if (!t->is_alt_screen) {
                            t->is_alt_screen = 1;
                            t->grid = t->alt_grid;
                        }
                        for (int j = 0; j < t->cols * t->rows; j++) {
                            t->alt_grid[j] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
                        }
                        t->cursor_x = 0;
                        t->cursor_y = 0;
                        if (is_live) {
                            scroll_offset = 0;
                            dirty_all = 1;
                        }
                    } else if (p == 1048) {
                        t->saved_cursor_x = t->cursor_x;
                        t->saved_cursor_y = t->cursor_y;
                    } else if (p == 1047 || p == 47) {
                        if (!t->is_alt_screen) {
                            t->is_alt_screen = 1;
                            t->grid = t->alt_grid;
                        }
                        if (is_live) {
                            scroll_offset = 0;
                            dirty_all = 1;
                        }
                    } else if (p == 2026) {
                        in_sync_update = 1;
                        sync_update_start_us = get_time_us();
                    } else if (p == 1) {
                        app_cursor_keys = 1;
                    } else if (p == 25) {
                        t->cursor_visible = 1;
                        if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                    } else if (p == 1000 || p == 1002 || p == 1003) {
                        mouse_mode = p;
                    } else if (p == 1006) {
                        mouse_sgr = 1;
                    } else if (p == 2004) {
                        bracketed_paste = 1;
                    }
                }
            }
            break;
        }
        case 'l': {
            int count = t->csi_has_param ? t->csi_nparams + 1 : 0;
            for (int i = 0; i < count; i++) {
                int p = t->csi_params[i];
                if (t->csi_private) {
                    if (p == 1049) {
                        for (int j = 0; j < t->cols * t->rows; j++) {
                            t->alt_grid[j] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
                        }
                        if (t->is_alt_screen) {
                            t->is_alt_screen = 0;
                            t->grid = t->primary_grid;
                        }
                        t->cursor_x = t->saved_cursor_x;
                        t->cursor_y = t->saved_cursor_y;
                        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
                        if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;
                        if (t->cursor_x < 0) t->cursor_x = 0;
                        if (t->cursor_y < 0) t->cursor_y = 0;
                        if (is_live) {
                            scroll_offset = 0;
                            dirty_all = 1;
                        }
                    } else if (p == 1048) {
                        t->cursor_x = t->saved_cursor_x;
                        t->cursor_y = t->saved_cursor_y;
                        if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
                        if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;
                        if (t->cursor_x < 0) t->cursor_x = 0;
                        if (t->cursor_y < 0) t->cursor_y = 0;
                        if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                    } else if (p == 1047) {
                        for (int j = 0; j < t->cols * t->rows; j++) {
                            t->alt_grid[j] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
                        }
                        if (t->is_alt_screen) {
                            t->is_alt_screen = 0;
                            t->grid = t->primary_grid;
                            if (is_live) {
                                scroll_offset = 0;
                                dirty_all = 1;
                            }
                        }
                    } else if (p == 47) {
                        if (t->is_alt_screen) {
                            t->is_alt_screen = 0;
                            t->grid = t->primary_grid;
                            if (is_live) {
                                scroll_offset = 0;
                                dirty_all = 1;
                            }
                        }
                    } else if (p == 2026) {
                        in_sync_update = 0;
                    } else if (p == 1) {
                        app_cursor_keys = 0;
                    } else if (p == 25) {
                        t->cursor_visible = 0;
                        if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                    } else if (p == 1000 || p == 1002 || p == 1003) {
                        mouse_mode = 0;
                    } else if (p == 1006) {
                        mouse_sgr = 0;
                    } else if (p == 2004) {
                        bracketed_paste = 0;
                    }
                }
            }
            break;
        }
        case 's':
            t->saved_cursor_x = t->cursor_x;
            t->saved_cursor_y = t->cursor_y;
            break;
        case 'u':
            t->cursor_x = t->saved_cursor_x;
            t->cursor_y = t->saved_cursor_y;
            if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
            if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;
            if (t->cursor_x < 0) t->cursor_x = 0;
            if (t->cursor_y < 0) t->cursor_y = 0;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        case 'n': {
            if (is_live && pty_master >= 0 && !t->csi_private) {
                if (p1 == 6) {
                    char resp[32];
                    int len = snprintf(resp, sizeof(resp), "\x1b[%d;%dR", t->cursor_y + 1, t->cursor_x + 1);
                    if (len > 0) pty_write(pty_master, resp, (size_t)len);
                } else if (p1 == 5) {
                    pty_write(pty_master, "\x1b[0n", 4);
                }
            }
            break;
        }
        case 'c': {
            if (is_live && pty_master >= 0 && !t->csi_private && (p1 == 0 || !t->csi_has_param)) {
                pty_write(pty_master, "\x1b[?6c", 5);
            }
            break;
        }
        case 'm':
            handle_sgr_internal(t);
            break;
        case 'q':
            if (p1 >= 0 && p1 <= 6) {
                cursor_style = p1;
                if (p1 == 0) cursor_blink_enabled = default_cursor_blink;
                else if (p1 == 1 || p1 == 3 || p1 == 5) cursor_blink_enabled = 1;
                else if (p1 == 2 || p1 == 4 || p1 == 6) cursor_blink_enabled = 0;
                if (is_live) dirty_all = 1;
            }
            break;
        case 't':
            break;
    }
}

static void term_put_codepoint_internal(Terminal *t, uint32_t cp, int is_live) {
    switch (cp) {
        case '\r':
            t->cursor_x = 0;
            break;
        case '\n':
            t->cursor_y++;
            if (t->cursor_y >= t->rows) term_scroll_internal(t, is_live);
            else if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        case '\b':
            if (t->cursor_x > 0) t->cursor_x--;
            if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            break;
        case '\t':
            t->cursor_x = (t->cursor_x + 8) & ~7;
            if (t->cursor_x >= t->cols) {
                t->cursor_x = 0;
                t->cursor_y++;
                if (t->cursor_y >= t->rows) term_scroll_internal(t, is_live);
                else if (is_live) mark_line_dirty(t->cursor_y, t->rows);
            }
            break;
        default:
            if (cp >= 32) {
                int w = wcwidth((wchar_t)cp);
                if (w < 0) {
                    if (cp >= 0x1F300 && cp <= 0x1FAFF) w = 2;
                    else w = 1;
                }
                if (w == 0) return;

                if (w == 2) {
                    if (t->cursor_x + 1 >= t->cols) {
                        t->cursor_x = 0;
                        t->cursor_y++;
                        if (t->cursor_y >= t->rows) term_scroll_internal(t, is_live);
                    }
                    if (t->cursor_x > 0 && (t->grid[t->cursor_y * t->cols + t->cursor_x].flags & FLAG_WIDE_DUMMY)) {
                        t->grid[t->cursor_y * t->cols + t->cursor_x - 1] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                    }
                    t->grid[t->cursor_y * t->cols + t->cursor_x] = (Cell){cp, t->cur_fg, t->cur_bg, t->cur_flags | FLAG_WIDE};
                    if (t->cursor_x + 1 < t->cols) {
                        t->grid[t->cursor_y * t->cols + t->cursor_x + 1] = (Cell){' ', t->cur_fg, t->cur_bg, t->cur_flags | FLAG_WIDE_DUMMY};
                    }
                    if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                    t->cursor_x += 2;
                } else {
                    if (t->cursor_x >= t->cols) {
                        t->cursor_x = 0;
                        t->cursor_y++;
                        if (t->cursor_y >= t->rows) term_scroll_internal(t, is_live);
                    }
                    if (t->cursor_x > 0 && (t->grid[t->cursor_y * t->cols + t->cursor_x].flags & FLAG_WIDE_DUMMY)) {
                        t->grid[t->cursor_y * t->cols + t->cursor_x - 1] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                    }
                    if (t->grid[t->cursor_y * t->cols + t->cursor_x].flags & FLAG_WIDE) {
                        if (t->cursor_x + 1 < t->cols) {
                            t->grid[t->cursor_y * t->cols + t->cursor_x + 1] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                        }
                    }
                    t->grid[t->cursor_y * t->cols + t->cursor_x] = (Cell){cp, t->cur_fg, t->cur_bg, t->cur_flags};
                    if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                    t->cursor_x++;
                }
            }
            break;
    }
}

// Base64 Decode helper
static int b64_decode(const char *in, size_t in_len, char *out, size_t out_max) {
    static const signed char b_table[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
    };
    size_t out_len = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = in[i];
        if (c == '=') break;
        if (c > 127 || (b_table[c] == 0 && c != 'A')) continue;
        buf = (buf << 6) | b_table[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len + 1 < out_max) {
                out[out_len++] = (buf >> bits) & 0xFF;
            }
        }
    }
    out[out_len] = '\0';
    return out_len;
}

static void term_put_byte_internal(Terminal *t, unsigned char c, int is_live) {
    switch (t->state) {
        case STATE_NORMAL:
            if (c == 0x1B) {
                t->state = STATE_ESC;
                return;
            }
            if (c < 0x80) {
                t->utf8_remain = 0;
                term_put_codepoint_internal(t, c, is_live);
            } else if ((c & 0xE0) == 0xC0) {
                t->utf8_cp = c & 0x1F;
                t->utf8_remain = 1;
            } else if ((c & 0xF0) == 0xE0) {
                t->utf8_cp = c & 0x0F;
                t->utf8_remain = 2;
            } else if ((c & 0xF8) == 0xF0) {
                t->utf8_cp = c & 0x07;
                t->utf8_remain = 3;
            } else if ((c & 0xC0) == 0x80 && t->utf8_remain > 0) {
                t->utf8_cp = (t->utf8_cp << 6) | (c & 0x3F);
                t->utf8_remain--;
                if (t->utf8_remain == 0) {
                    term_put_codepoint_internal(t, t->utf8_cp, is_live);
                }
            } else {
                t->utf8_remain = 0;
            }
            break;

        case STATE_ESC:
            if (c == '[') {
                t->state = STATE_CSI;
                t->csi_nparams = 0;
                t->csi_has_param = 0;
                t->csi_private = 0;
                memset(t->csi_params, 0, sizeof(t->csi_params));
            } else if (c == ']') {
                t->state = STATE_OSC;
                t->osc_len = 0;
            } else if (c == '_' || c == 'P') {
                t->state = STATE_APC;
            } else if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%') {
                t->state = STATE_CHARSET;
            } else if (c == 'M') {
                if (t->cursor_y > 0) {
                    t->cursor_y--;
                    if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                } else {
                    memmove(&t->grid[t->cols], &t->grid[0], (size_t)(t->rows - 1) * t->cols * sizeof(Cell));
                    for (int x = 0; x < t->cols; x++) t->grid[x] = (Cell){' ', t->cur_fg, t->cur_bg, 0};
                    if (is_live) dirty_all = 1;
                }
                t->state = STATE_NORMAL;
            } else if (c == 'E') {
                t->cursor_x = 0;
                t->cursor_y++;
                if (t->cursor_y >= t->rows) term_scroll_internal(t, is_live);
                else if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                t->state = STATE_NORMAL;
            } else if (c == 'D') {
                t->cursor_y++;
                if (t->cursor_y >= t->rows) term_scroll_internal(t, is_live);
                else if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                t->state = STATE_NORMAL;
            } else if (c == '7') {
                t->saved_cursor_x = t->cursor_x;
                t->saved_cursor_y = t->cursor_y;
                t->state = STATE_NORMAL;
            } else if (c == '8') {
                t->cursor_x = t->saved_cursor_x;
                t->cursor_y = t->saved_cursor_y;
                if (t->cursor_x >= t->cols) t->cursor_x = t->cols - 1;
                if (t->cursor_y >= t->rows) t->cursor_y = t->rows - 1;
                if (t->cursor_x < 0) t->cursor_x = 0;
                if (t->cursor_y < 0) t->cursor_y = 0;
                if (is_live) mark_line_dirty(t->cursor_y, t->rows);
                t->state = STATE_NORMAL;
            } else {
                t->state = STATE_NORMAL;
            }
            break;

        case STATE_CSI:
            if (c == '?') {
                t->csi_private = 1;
            } else if (c >= '0' && c <= '9') {
                t->csi_params[t->csi_nparams] = t->csi_params[t->csi_nparams] * 10 + (c - '0');
                t->csi_has_param = 1;
            } else if (c == ';') {
                if (t->csi_nparams < MAX_CSI_PARAMS - 1) t->csi_nparams++;
                t->csi_has_param = 1;
            } else if (c >= 0x40 && c <= 0x7E) {
                handle_csi_internal(t, c, is_live);
                t->state = STATE_NORMAL;
            }
            break;

        case STATE_OSC:
            if (c == 0x07 || c == 0x1B || c == 0x9C) {
                t->osc_buf[t->osc_len] = '\0';
                if (is_live) {
                    if ((t->osc_buf[0] == '0' || t->osc_buf[0] == '2') && t->osc_buf[1] == ';') {
                        if (dpy && win) {
                            set_window_title(&t->osc_buf[2]);
                            XFlush(dpy);
                        }
                    } else if (strncmp(t->osc_buf, "52;", 3) == 0) {
                        const char *p = strchr(t->osc_buf + 3, ';');
                        if (p) {
                            p++;
                            char decoded[4096];
                            int n = b64_decode(p, strlen(p), decoded, sizeof(decoded));
                            if (n > 0) {
                                free(sel_text);
                                sel_text = strdup(decoded);
                                XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
                                XSetSelectionOwner(dpy, atom_clipboard, win, CurrentTime);
                            }
                        }
                    }
                }
                t->state = (c == 0x1B) ? STATE_ESC : STATE_NORMAL;
            } else if (t->osc_len < sizeof(t->osc_buf) - 1) {
                t->osc_buf[t->osc_len++] = c;
            }
            break;

        case STATE_APC:
            if (c == 0x07 || c == 0x9C) {
                t->state = STATE_NORMAL;
            } else if (c == 0x1B) {
                t->state = STATE_ESC;
            }
            break;

        case STATE_CHARSET:
            t->state = STATE_NORMAL;
            break;
    }
}

static Cell get_cell(int r, int c) {
    if (replay_mode) {
        if (r < replay_term.rows && c < replay_term.cols) {
            return replay_term.grid[r * replay_term.cols + c];
        }
        return (Cell){' ', COLOR_FG, COLOR_BG, 0};
    }

    if (live_term.is_alt_screen || scroll_offset == 0) {
        return live_term.grid[r * cols + c];
    }
    int from_bottom = (rows - 1 - r) + scroll_offset;
    if (from_bottom < rows) {
        int active_r = rows - 1 - from_bottom;
        return live_term.grid[active_r * cols + c];
    }
    int hist_steps = from_bottom - rows;
    if (hist_steps >= hist_count) {
        return (Cell){' ', COLOR_FG, COLOR_BG, 0};
    }
    int hist_idx = (hist_head - 1 - hist_steps + MAX_HIST_LINES * 4) % MAX_HIST_LINES;
    if (!history[hist_idx] || c >= hist_cols[hist_idx]) {
        return (Cell){' ', COLOR_FG, COLOR_BG, 0};
    }
    return history[hist_idx][c];
}

static void term_reset(Terminal *t) {
    for (int i = 0; i < t->cols * t->rows; i++) {
        t->primary_grid[i] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
        t->alt_grid[i] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
    }
    t->is_alt_screen = 0;
    t->grid = t->primary_grid;
    t->cursor_x = 0;
    t->cursor_y = 0;
    t->saved_cursor_x = 0;
    t->saved_cursor_y = 0;
    t->cursor_visible = 1;
    t->cur_fg = COLOR_FG;
    t->cur_bg = COLOR_BG;
    t->cur_flags = 0;
    t->state = STATE_NORMAL;
    t->csi_nparams = 0;
    t->csi_has_param = 0;
    t->csi_private = 0;
    memset(t->csi_params, 0, sizeof(t->csi_params));
    t->osc_len = 0;
    t->utf8_cp = 0;
    t->utf8_remain = 0;
}

static void scrub_to_chunk(int target_chunk) {
    if (target_chunk < 0) target_chunk = 0;
    if (target_chunk >= flight_count) target_chunk = flight_count - 1;
    replay_chunk_idx = target_chunk;

    // Reset replay terminal to clean baseline
    term_reset(&replay_term);

    // Replay all chunks deterministically from beginning to target_chunk
    for (int i = 0; i <= target_chunk; i++) {
        int idx = (flight_total_recorded >= MAX_FLIGHT_CHUNKS) ?
                  (flight_head - flight_count + i + MAX_FLIGHT_CHUNKS * 2) % MAX_FLIGHT_CHUNKS : i;
        FlightChunk *fc = &flight_log[idx];
        for (size_t b = 0; b < fc->len; b++) {
            term_put_byte_internal(&replay_term, (unsigned char)fc->data[b], 0);
        }
    }

    dirty_all = 1;
}

static void export_asciinema(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char fname[128];
    snprintf(fname, sizeof(fname), "session_%04d%02d%02d_%02d%02d%02d.cast",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    FILE *f = fopen(fname, "w");
    if (!f) return;

    fprintf(f, "{\"version\": 2, \"width\": %d, \"height\": %d, \"timestamp\": %ld, \"title\": \"DWM Terminal Flight Session\"}\n",
            cols, rows, (long)t);

    if (flight_count > 0) {
        int start_idx = (flight_total_recorded >= MAX_FLIGHT_CHUNKS) ? flight_head : 0;
        uint64_t base_ts = flight_log[start_idx].ts_us;

        for (int i = 0; i < flight_count; i++) {
            int idx = (start_idx + i) % MAX_FLIGHT_CHUNKS;
            FlightChunk *fc = &flight_log[idx];
            double sec = (double)(fc->ts_us - base_ts) / 1000000.0;

            fprintf(f, "[%.6f, \"o\", \"", sec);
            for (size_t j = 0; j < fc->len; j++) {
                unsigned char c = (unsigned char)fc->data[j];
                if (c == '"') fputs("\\\"", f);
                else if (c == '\\') fputs("\\\\", f);
                else if (c == '\b') fputs("\\b", f);
                else if (c == '\f') fputs("\\f", f);
                else if (c == '\n') fputs("\\n", f);
                else if (c == '\r') fputs("\\r", f);
                else if (c == '\t') fputs("\\t", f);
                else if (c < 32) fprintf(f, "\\u%04x", c);
                else fputc(c, f);
            }
            fprintf(f, "\"]\n");
        }
    }

    fclose(f);
    snprintf(hud_message, sizeof(hud_message), "Exported: %s", fname);
    dirty_all = 1;
}

static int is_selected(int r, int c) {
    if (replay_mode) return 0;
    if (!sel_active && sel_start_r < 0) return 0;
    if (sel_start_r < 0 || sel_end_r < 0) return 0;

    int sr = sel_start_r, sc = sel_start_c;
    int er = sel_end_r, ec = sel_end_c;

    if (sr > er || (sr == er && sc > ec)) {
        int tr = sr; sr = er; er = tr;
        int tc = sc; sc = ec; ec = tc;
    }

    if (r < sr || r > er) return 0;
    if (sr == er) return (c >= sc && c <= ec);
    if (r == sr) return (c >= sc);
    if (r == er) return (c <= ec);
    return 1;
}

static int codepoint_to_utf8(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp; return 1;
    } else if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2;
    } else if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); return 3;
    } else {
        out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F); out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); return 4;
    }
}

static void copy_selection_text(void) {
    if (sel_start_r < 0 || sel_end_r < 0) return;

    int sr = sel_start_r, sc = sel_start_c;
    int er = sel_end_r, ec = sel_end_c;
    if (sr > er || (sr == er && sc > ec)) {
        int tr = sr; sr = er; er = tr;
        int tc = sc; sc = ec; ec = tc;
    }

    size_t cap = 4096, len = 0;
    free(sel_text);
    sel_text = malloc(cap);

    for (int r = sr; r <= er; r++) {
        int c_start = (r == sr) ? sc : 0;
        int c_end = (r == er) ? ec : cols - 1;

        int line_end = c_end;
        while (line_end >= c_start) {
            Cell cell = get_cell(r, line_end);
            if (cell.codepoint != ' ' && cell.codepoint != 0) break;
            line_end--;
        }

        for (int c = c_start; c <= line_end; c++) {
            Cell cell = get_cell(r, c);
            char u[8];
            int n = codepoint_to_utf8(cell.codepoint ? cell.codepoint : ' ', u);
            if (len + n + 2 >= cap) {
                cap *= 2;
                sel_text = realloc(sel_text, cap);
            }
            memcpy(&sel_text[len], u, n);
            len += n;
        }

        if (r < er) {
            if (len + 2 >= cap) {
                cap *= 2;
                sel_text = realloc(sel_text, cap);
            }
            sel_text[len++] = '\n';
        }
    }
    sel_text[len] = '\0';

    XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
    XSetSelectionOwner(dpy, atom_clipboard, win, CurrentTime);
}

static void reflow_terminal(void) {
    int new_cols = (win_w - padding_x * 2) / char_w;
    int new_rows = (win_h - padding_y * 2) / char_h;
    if (new_cols < 4) new_cols = 4;
    if (new_rows < 2) new_rows = 2;

    Cell *new_live_pri = calloc(new_rows * new_cols, sizeof(Cell));
    Cell *new_live_alt = calloc(new_rows * new_cols, sizeof(Cell));
    Cell *new_rep_pri = calloc(new_rows * new_cols, sizeof(Cell));
    Cell *new_rep_alt = calloc(new_rows * new_cols, sizeof(Cell));
    if (!new_live_pri || !new_live_alt || !new_rep_pri || !new_rep_alt) {
        free(new_live_pri);
        free(new_live_alt);
        free(new_rep_pri);
        free(new_rep_alt);
        return;
    }

    for (int r = 0; r < new_rows; r++) {
        for (int c = 0; c < new_cols; c++) {
            if (r < rows && c < cols) {
                new_live_pri[r * new_cols + c] = live_term.primary_grid[r * cols + c];
                new_live_alt[r * new_cols + c] = live_term.alt_grid[r * cols + c];
                new_rep_pri[r * new_cols + c] = replay_term.primary_grid[r * cols + c];
                new_rep_alt[r * new_cols + c] = replay_term.alt_grid[r * cols + c];
            } else {
                new_live_pri[r * new_cols + c] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
                new_live_alt[r * new_cols + c] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
                new_rep_pri[r * new_cols + c] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
                new_rep_alt[r * new_cols + c] = (Cell){' ', COLOR_FG, COLOR_BG, 0};
            }
        }
    }
    free(live_term.primary_grid);
    free(live_term.alt_grid);
    free(replay_term.primary_grid);
    free(replay_term.alt_grid);

    live_term.primary_grid = new_live_pri;
    live_term.alt_grid = new_live_alt;
    live_term.grid = live_term.is_alt_screen ? live_term.alt_grid : live_term.primary_grid;

    replay_term.primary_grid = new_rep_pri;
    replay_term.alt_grid = new_rep_alt;
    replay_term.grid = replay_term.is_alt_screen ? replay_term.alt_grid : replay_term.primary_grid;

    cols = new_cols;
    rows = new_rows;
    live_term.cols = new_cols;
    live_term.rows = new_rows;
    replay_term.cols = new_cols;
    replay_term.rows = new_rows;

    uint8_t *new_dirty = realloc(dirty, rows * sizeof(uint8_t));
    if (new_dirty) {
        dirty = new_dirty;
        memset(dirty, 1, rows * sizeof(uint8_t));
    }
    dirty_all = 1;

    if (live_term.cursor_x >= cols) live_term.cursor_x = cols - 1;
    if (live_term.cursor_y >= rows) live_term.cursor_y = rows - 1;
    if (live_term.cursor_x < 0) live_term.cursor_x = 0;
    if (live_term.cursor_y < 0) live_term.cursor_y = 0;

    if (replay_term.cursor_x >= cols) replay_term.cursor_x = cols - 1;
    if (replay_term.cursor_y >= rows) replay_term.cursor_y = rows - 1;
    if (replay_term.cursor_x < 0) replay_term.cursor_x = 0;
    if (replay_term.cursor_y < 0) replay_term.cursor_y = 0;

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
        .ws_xpixel = (unsigned short)win_w,
        .ws_ypixel = (unsigned short)win_h
    };
    ioctl(pty_master, TIOCSWINSZ, &ws);
}

static void update_wm_normal_hints(void) {
    if (!dpy || !win) return;
    XSizeHints *hints = XAllocSizeHints();
    if (!hints) return;
    hints->flags = PResizeInc | PBaseSize | PMinSize;
    hints->width_inc = char_w;
    hints->height_inc = char_h;
    hints->base_width = padding_x * 2;
    hints->base_height = padding_y * 2;
    hints->min_width = padding_x * 2 + char_w * 4;
    hints->min_height = padding_y * 2 + char_h * 2;
    XSetWMNormalHints(dpy, win, hints);
    XFree(hints);
}

static void set_font_size(int new_pt) {
    if (new_pt < 6) new_pt = 6;
    if (new_pt > 72) new_pt = 72;
    if (new_pt == font_pt) return;
    font_pt = new_pt;

    int px = pt_to_px(font_pt);
    FT_Set_Pixel_Sizes(ft_face, 0, px);
    for (int i = 0; i < num_fallback_faces; i++) {
        if (FT_IS_SCALABLE(fallback_faces[i])) {
            FT_Set_Pixel_Sizes(fallback_faces[i], 0, px);
        } else if (fallback_faces[i]->num_fixed_sizes > 0) {
            FT_Select_Size(fallback_faces[i], 0);
        }
    }
    clear_glyph_cache();

    FT_Load_Char(ft_face, 'M', FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT);
    char_w = ft_face->glyph->advance.x >> 6;
    char_h = ft_face->size->metrics.height >> 6;
    ascender_px = ft_face->size->metrics.ascender >> 6;
    if (char_w <= 0) char_w = 8;
    if (char_h <= 0) char_h = 16;

    reflow_terminal();
    update_wm_normal_hints();
}

static void resize_terminal(int new_w, int new_h) {
    if (new_w <= 0 || new_h <= 0) return;
    if (new_w == win_w && new_h == win_h) return;

    if (init_framebuffer(new_w, new_h) != 0) return;
    reflow_terminal();
}

static void render_frame(void) {
    if (!replay_mode && in_sync_update) {
        uint64_t now = get_time_us();
        if (now - sync_update_start_us < 100000ULL) {
            return;
        }
        in_sync_update = 0;
    }

    int cur_cx = replay_mode ? replay_term.cursor_x : live_term.cursor_x;
    int cur_cy = replay_mode ? replay_term.cursor_y : live_term.cursor_y;

    if (cur_cx != prev_cursor_x || cur_cy != prev_cursor_y) {
        if (prev_cursor_y >= 0 && prev_cursor_y < rows) dirty[prev_cursor_y] = 1;
        if (cur_cy >= 0 && cur_cy < rows) dirty[cur_cy] = 1;
        prev_cursor_x = cur_cx;
        prev_cursor_y = cur_cy;
    }

    int rendered_any = 0;
    int min_dirty_y = win_h;
    int max_dirty_y = 0;

    for (int r = 0; r < rows; r++) {
        if (!dirty_all && !dirty[r]) continue;
        dirty[r] = 0;
        rendered_any = 1;

        int cell_y0 = padding_y + r * char_h;
        int y_end = cell_y0 + char_h;
        if (cell_y0 < min_dirty_y) min_dirty_y = (cell_y0 >= 0) ? cell_y0 : 0;
        if (y_end > max_dirty_y) max_dirty_y = (y_end <= win_h) ? y_end : win_h;

        // Check if this is the bottom line and we are in Replay Mode (Scrubber HUD banner!)
        int is_hud_line = (replay_mode && r == rows - 1);

        for (int y = 0; y < char_h; y++) {
            int py = cell_y0 + y;
            if (py >= win_h) break;
            if (py < 0) continue;
            uint32_t *dst = &pixels[py * win_w];
            for (int x = 0; x < win_w; x++) {
                dst[x] = is_hud_line ? COLOR_HUD_BG : COLOR_BG;
            }
        }

        if (is_hud_line) {
            // Render Scrubber HUD Banner
            char hud_text[256];
            if (hud_message[0] != '\0') {
                snprintf(hud_text, sizeof(hud_text), " %.200s [Any key to continue]", hud_message);
            } else {
                int total_s = 0, curr_s = 0;
                if (flight_count > 0) {
                    int start_idx = (flight_total_recorded >= MAX_FLIGHT_CHUNKS) ? flight_head : 0;
                    uint64_t base_ts = flight_log[start_idx].ts_us;
                    int cur_idx = (flight_total_recorded >= MAX_FLIGHT_CHUNKS) ?
                                  (flight_head - flight_count + replay_chunk_idx + MAX_FLIGHT_CHUNKS * 2) % MAX_FLIGHT_CHUNKS : replay_chunk_idx;
                    int last_idx = (flight_total_recorded >= MAX_FLIGHT_CHUNKS) ?
                                   (flight_head - 1 + MAX_FLIGHT_CHUNKS) % MAX_FLIGHT_CHUNKS : flight_count - 1;

                    curr_s = (flight_log[cur_idx].ts_us - base_ts) / 1000000ULL;
                    total_s = (flight_log[last_idx].ts_us - base_ts) / 1000000ULL;
                }
                snprintf(hud_text, sizeof(hud_text),
                         " REPLAY [%02d:%02d / %02d:%02d] (Chunk %d/%d)  [<-/->: Scrub | Home/End | e: Export .cast | Esc: Live] ",
                         curr_s / 60, curr_s % 60, total_s / 60, total_s % 60,
                         replay_chunk_idx + 1, flight_count);
            }

            for (size_t c = 0; c < strlen(hud_text) && (int)c < cols; c++) {
                CachedGlyph *g = get_glyph((unsigned char)hud_text[c]);
                if (g && g->bitmap) {
                    int cell_x0 = padding_x + c * char_w;
                    int base_y = cell_y0 + ascender_px;
                    int gx = cell_x0 + g->left;
                    int gy = base_y - g->top;

                    for (int row = 0; row < g->height; row++) {
                        int py = gy + row;
                        if (py < 0 || py >= win_h) continue;
                        uint8_t *src_row = &g->bitmap[row * g->width];
                        uint32_t *dst_row = &pixels[py * win_w];

                        for (int col = 0; col < g->width; col++) {
                            int px = gx + col;
                            if (px < 0 || px >= win_w) continue;
                            uint32_t alpha = src_row[col];
                            if (alpha > 128) {
                                dst_row[px] = COLOR_HUD_FG;
                            }
                        }
                    }
                }
            }
            continue;
        }

        for (int c = 0; c < cols; c++) {
            Cell cell = get_cell(r, c);
            if (cell.flags & FLAG_WIDE_DUMMY) {
                continue;
            }

            int span_w = (cell.flags & FLAG_WIDE) ? (char_w * 2) : char_w;
            int cur_visible = replay_mode ? 1 : (live_term.cursor_visible && (!cursor_blink_enabled || cursor_blink_state));
            int is_cursor = (!replay_mode && cur_visible && scroll_offset == 0 && r == cur_cy && c == cur_cx);
            int selected = is_selected(r, c);

            uint32_t fg = cell.fg;
            uint32_t bg = cell.bg;

            if (cell.flags & FLAG_INVERSE) {
                uint32_t tmp = fg; fg = bg; bg = tmp;
            }
            if (selected) {
                bg = COLOR_SEL_BG;
                fg = COLOR_SEL_FG;
            }

            int eff_style = (cursor_style == 0) ? default_cursor_style : cursor_style;
            int draw_bar_cursor = (is_cursor && (eff_style == 5 || eff_style == 6));
            int draw_underline_cursor = (is_cursor && (eff_style == 3 || eff_style == 4));
            if (is_cursor && !draw_bar_cursor && !draw_underline_cursor) {
                bg = COLOR_CURSOR;
                fg = COLOR_BG;
            }

            int cell_x0 = padding_x + c * char_w;

            if (bg != COLOR_BG || is_cursor || selected) {
                for (int y = 0; y < char_h; y++) {
                    int py = cell_y0 + y;
                    if (py >= win_h) break;
                    if (py < 0) continue;
                    for (int x = 0; x < span_w; x++) {
                        int px = cell_x0 + x;
                        if (px >= 0 && px < win_w) {
                            pixels[py * win_w + px] = bg;
                        }
                    }
                }
            }

            if (cell.codepoint > 32) {
                CachedGlyph *g = get_glyph(cell.codepoint);
                if (g && g->bitmap) {
                    int base_y = cell_y0 + ascender_px;
                    int gx = cell_x0 + g->left;
                    int gy = base_y - g->top;

                    if (g->is_color) {
                        const uint32_t *src_pixels = (const uint32_t *)g->bitmap;
                        for (int row = 0; row < g->height; row++) {
                            int py = gy + row;
                            if (py < 0 || py >= win_h) continue;
                            uint32_t *dst_row = &pixels[py * win_w];

                            for (int col = 0; col < g->width; col++) {
                                int px = gx + col;
                                if (px < 0 || px >= win_w) continue;
                                uint32_t col_val = src_pixels[row * g->width + col];
                                uint32_t alpha = (col_val >> 24) & 0xFF;
                                if (alpha == 0) continue;

                                if (alpha == 255) {
                                    dst_row[px] = col_val & 0x00FFFFFF;
                                } else {
                                    uint32_t orig = dst_row[px];
                                    uint32_t sr = (col_val >> 16) & 0xFF;
                                    uint32_t sg = (col_val >> 8) & 0xFF;
                                    uint32_t sb = col_val & 0xFF;
                                    uint32_t dr = (orig >> 16) & 0xFF;
                                    uint32_t dg = (orig >> 8) & 0xFF;
                                    uint32_t db = orig & 0xFF;

                                    uint32_t nr = (sr * alpha + dr * (255 - alpha)) / 255;
                                    uint32_t ng = (sg * alpha + dg * (255 - alpha)) / 255;
                                    uint32_t nb = (sb * alpha + db * (255 - alpha)) / 255;

                                    dst_row[px] = (nr << 16) | (ng << 8) | nb;
                                }
                            }
                        }
                    } else {
                        uint32_t fr = (fg >> 16) & 0xFF;
                        uint32_t fg_val = (fg >> 8) & 0xFF;
                        uint32_t fb = fg & 0xFF;

                        for (int row = 0; row < g->height; row++) {
                            int py = gy + row;
                            if (py < 0 || py >= win_h) continue;
                            uint8_t *src_row = &g->bitmap[row * g->width];
                            uint32_t *dst_row = &pixels[py * win_w];

                            for (int col = 0; col < g->width; col++) {
                                int px = gx + col;
                                if (px < 0 || px >= win_w) continue;
                                uint32_t alpha = src_row[col];
                                if (alpha == 0) continue;

                                if (alpha == 255) {
                                    dst_row[px] = fg;
                                } else {
                                    uint32_t orig = dst_row[px];
                                    uint32_t obr = (orig >> 16) & 0xFF;
                                    uint32_t obg = (orig >> 8) & 0xFF;
                                    uint32_t obb = orig & 0xFF;

                                    uint32_t lin_r = (sq_lut[fr] * alpha + sq_lut[obr] * (255 - alpha)) / 255;
                                    uint32_t lin_g = (sq_lut[fg_val] * alpha + sq_lut[obg] * (255 - alpha)) / 255;
                                    uint32_t lin_b = (sq_lut[fb] * alpha + sq_lut[obb] * (255 - alpha)) / 255;

                                    dst_row[px] = ((uint32_t)sqrt_lut[lin_r] << 16) | ((uint32_t)sqrt_lut[lin_g] << 8) | (uint32_t)sqrt_lut[lin_b];
                                }
                            }
                        }
                    }
                }
            }

            if (cell.flags & FLAG_UNDERLINE) {
                int py = cell_y0 + char_h - 2;
                if (py >= 0 && py < win_h) {
                    for (int x = 0; x < span_w; x++) {
                        int px = cell_x0 + x;
                        if (px >= 0 && px < win_w) {
                            pixels[py * win_w + px] = fg;
                        }
                    }
                }
            }

            if (draw_bar_cursor) {
                for (int y = 0; y < char_h; y++) {
                    int py = cell_y0 + y;
                    if (py >= 0 && py < win_h) {
                        for (int bx = 0; bx < 2; bx++) {
                            int px = cell_x0 + bx;
                            if (px >= 0 && px < win_w) {
                                pixels[py * win_w + px] = COLOR_CURSOR;
                            }
                        }
                    }
                }
            } else if (draw_underline_cursor) {
                for (int uy = char_h - 2; uy < char_h; uy++) {
                    int py = cell_y0 + uy;
                    if (py >= 0 && py < win_h) {
                        for (int x = 0; x < span_w; x++) {
                            int px = cell_x0 + x;
                            if (px >= 0 && px < win_w) {
                                pixels[py * win_w + px] = COLOR_CURSOR;
                            }
                        }
                    }
                }
            }
        }
    }

    if (dirty_all) {
        int top_h = padding_y;
        int bot_y = padding_y + rows * char_h;
        for (int y = 0; y < top_h && y < win_h; y++) {
            for (int x = 0; x < win_w; x++) {
                pixels[y * win_w + x] = COLOR_BG;
            }
        }
        if (bot_y < win_h && bot_y >= 0) {
            for (int y = bot_y; y < win_h; y++) {
                for (int x = 0; x < win_w; x++) {
                    pixels[y * win_w + x] = COLOR_BG;
                }
            }
        }
        int left_w = padding_x;
        int right_x = padding_x + cols * char_w;
        for (int y = top_h; y < bot_y && y < win_h; y++) {
            for (int x = 0; x < left_w && x < win_w; x++) {
                pixels[y * win_w + x] = COLOR_BG;
            }
            if (right_x < win_w && right_x >= 0) {
                for (int x = right_x; x < win_w; x++) {
                    pixels[y * win_w + x] = COLOR_BG;
                }
            }
        }
        min_dirty_y = 0;
        max_dirty_y = win_h;
        rendered_any = 1;
        dirty_all = 0;
    }

    if (rendered_any && max_dirty_y > min_dirty_y) {
        blit_subimage(0, min_dirty_y, 0, min_dirty_y, win_w, max_dirty_y - min_dirty_y);
        XFlush(dpy);
    }
}

static char *resolve_font_path(const char *pattern_str, int check_family_match, int *face_index) {
    if (face_index) *face_index = 0;
    if (!pattern_str || !*pattern_str) return NULL;

    FcPattern *pat = NULL;
    if (strchr(pattern_str, ':')) {
        pat = FcNameParse((const FcChar8 *)pattern_str);
    } else {
        pat = FcPatternCreate();
        if (pat) {
            FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)pattern_str);
        }
    }
    if (!pat) return NULL;

    int existing_spacing = -1;
    if (FcPatternGetInteger(pat, FC_SPACING, 0, &existing_spacing) != FcResultMatch) {
        FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
    }
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    char *path = NULL;
    if (match) {
        int family_ok = 1;
        if (check_family_match) {
            family_ok = 0;
            char base_pat[128] = {0};
            strncpy(base_pat, pattern_str, sizeof(base_pat) - 1);
            char *colon = strchr(base_pat, ':');
            if (colon) *colon = '\0';
            char *pbe = base_pat + strlen(base_pat) - 1;
            while (pbe >= base_pat && (*pbe == ' ' || *pbe == '\t')) { *pbe = '\0'; pbe--; }

            char first_word[64] = {0};
            sscanf(base_pat, "%63s", first_word);

            FcChar8 *family = NULL;
            for (int i = 0; FcPatternGetString(match, FC_FAMILY, i, &family) == FcResultMatch; i++) {
                if (family) {
                    if (strcasestr((const char *)family, base_pat) != NULL ||
                        strcasestr(base_pat, (const char *)family) != NULL) {
                        family_ok = 1;
                        break;
                    }
                    if (first_word[0] && strlen(first_word) >= 3 &&
                        strcasestr((const char *)family, first_word) != NULL) {
                        family_ok = 1;
                        break;
                    }
                }
            }
        }
        int spacing = -1;
        if (FcPatternGetInteger(match, FC_SPACING, 0, &spacing) == FcResultMatch) {
            if (spacing == FC_PROPORTIONAL) {
                family_ok = 0;
            }
        }
        if (family_ok) {
            FcChar8 *file = NULL;
            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
                path = strdup((const char *)file);
            }
            if (face_index) {
                int idx = 0;
                if (FcPatternGetInteger(match, FC_INDEX, 0, &idx) == FcResultMatch) {
                    *face_index = idx;
                }
            }
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return path;
}


static uint32_t parse_hex_color(const char *s, uint32_t default_val) {
    if (!s) return default_val;
    while (*s == ' ' || *s == '\t' || *s == '"' || *s == '\'') s++;
    if (*s == '#') s++;
    char hex[7] = {0};
    int len = 0;
    while (len < 6 && ((s[len] >= '0' && s[len] <= '9') ||
                       (s[len] >= 'a' && s[len] <= 'f') ||
                       (s[len] >= 'A' && s[len] <= 'F'))) {
        hex[len] = s[len];
        len++;
    }
    if (len == 6) {
        return (uint32_t)strtoul(hex, NULL, 16);
    } else if (len == 3) {
        char full[7] = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2], '\0'};
        return (uint32_t)strtoul(full, NULL, 16);
    }
    return default_val;
}

static int resolve_config_path(char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return 0;
    out_path[0] = '\0';

    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[PATH_MAX];

    if (xdg && *xdg) {
        snprintf(path, sizeof(path), "%s/dwmterm/config", xdg);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, max_len, "%s", path);
            return 1;
        }
    }
    if (home && *home) {
        snprintf(path, sizeof(path), "%s/.config/dwmterm/config", home);
        if (access(path, R_OK) == 0) {
            snprintf(out_path, max_len, "%s", path);
            return 1;
        }
    }
    return 0;
}

static void load_config_from_file(const char *config_path) {
    if (!config_path || !*config_path) return;
    FILE *f = fopen(config_path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        // Strip trailing inline comments outside of quotes
        char in_quote = '\0';
        for (char *c = p; *c; c++) {
            if (in_quote) {
                if (*c == in_quote) in_quote = '\0';
            } else {
                if (*c == '"' || *c == '\'') {
                    in_quote = *c;
                } else if (*c == '#' || *c == ';') {
                    *c = '\0';
                    break;
                }
            }
        }

        // Trim trailing whitespace from stripped line
        char *pe = p + strlen(p) - 1;
        while (pe >= p && (*pe == ' ' || *pe == '\t' || *pe == '\n' || *pe == '\r')) { *pe = '\0'; pe--; }
        if (*p == '\0') continue;

        char key[64] = {0};
        char val[128] = {0};
        char *sep = strpbrk(p, "=:");
        if (sep) {
            size_t klen = (size_t)(sep - p);
            if (klen >= sizeof(key)) klen = sizeof(key) - 1;
            strncpy(key, p, klen);
            key[klen] = '\0';
            char *ke = key + strlen(key) - 1;
            while (ke >= key && (*ke == ' ' || *ke == '\t')) { *ke = '\0'; ke--; }

            char *v = sep + 1;
            while (*v == ' ' || *v == '\t') v++;
            strncpy(val, v, sizeof(val) - 1);
            char *ve = val + strlen(val) - 1;
            while (ve >= val && (*ve == ' ' || *ve == '\t' || *ve == '\n' || *ve == '\r')) { *ve = '\0'; ve--; }
        } else {
            if (sscanf(p, "%63s %127s", key, val) != 2) continue;
        }

        // Strip surrounding quotes from val
        char *clean_val = val;
        while (*clean_val == '"' || *clean_val == '\'') clean_val++;
        char *cve = clean_val + strlen(clean_val) - 1;
        while (cve >= clean_val && (*cve == '"' || *cve == '\'')) { *cve = '\0'; cve--; }

        if (strcasecmp(key, "font_size") == 0 || strcasecmp(key, "fontsize") == 0 || strcasecmp(key, "size") == 0) {
            int sz = atoi(clean_val);
            if (sz >= 6 && sz <= 72) {
                font_pt = sz;
                default_font_pt = sz;
            }
        } else if (strcasecmp(key, "font_family") == 0 || strcasecmp(key, "font") == 0 || strcasecmp(key, "fontfamily") == 0 || strcasecmp(key, "font_name") == 0) {
            if (clean_val[0]) {
                strncpy(config_font_family, clean_val, sizeof(config_font_family) - 1);
                config_font_family[sizeof(config_font_family) - 1] = '\0';
            }
        } else if (strcasecmp(key, "cols") == 0 || strcasecmp(key, "columns") == 0) {
            int c = atoi(clean_val);
            if (c >= 20 && c <= 500) cols = c;
        } else if (strcasecmp(key, "rows") == 0 || strcasecmp(key, "lines") == 0) {
            int r = atoi(clean_val);
            if (r >= 4 && r <= 200) rows = r;
        } else if (strcasecmp(key, "padding") == 0 || strcasecmp(key, "pad") == 0) {
            int p = atoi(clean_val);
            if (p >= 0 && p <= 100) {
                padding = p;
                padding_x = p;
                padding_y = p;
            }
        } else if (strcasecmp(key, "padding_x") == 0 || strcasecmp(key, "padding-x") == 0 ||
                   strcasecmp(key, "window-padding-x") == 0 || strcasecmp(key, "window_padding_x") == 0) {
            int p = atoi(clean_val);
            if (p >= 0 && p <= 100) padding_x = p;
        } else if (strcasecmp(key, "padding_y") == 0 || strcasecmp(key, "padding-y") == 0 ||
                   strcasecmp(key, "window-padding-y") == 0 || strcasecmp(key, "window_padding_y") == 0) {
            int p = atoi(clean_val);
            if (p >= 0 && p <= 100) padding_y = p;
        } else if (strcasecmp(key, "cursor_style") == 0 || strcasecmp(key, "cursor-style") == 0 ||
                   strcasecmp(key, "cursor_shape") == 0 || strcasecmp(key, "cursorshape") == 0) {
            if (strcasecmp(clean_val, "bar") == 0 || strcasecmp(clean_val, "beam") == 0 || strcasecmp(clean_val, "line") == 0) {
                cursor_style = 6;
                default_cursor_style = 6;
            } else if (strcasecmp(clean_val, "block") == 0) {
                cursor_style = 2;
                default_cursor_style = 2;
            } else if (strcasecmp(clean_val, "underline") == 0) {
                cursor_style = 4;
                default_cursor_style = 4;
            }
        } else if (strcasecmp(key, "cursor_blink") == 0 || strcasecmp(key, "cursor-blink") == 0 ||
                   strcasecmp(key, "cursor-style-blink") == 0 || strcasecmp(key, "cursor_style_blink") == 0) {
            if (strcasecmp(clean_val, "true") == 0 || strcasecmp(clean_val, "1") == 0 ||
                strcasecmp(clean_val, "yes") == 0 || strcasecmp(clean_val, "on") == 0) {
                cursor_blink_enabled = 1;
                default_cursor_blink = 1;
            } else if (strcasecmp(clean_val, "false") == 0 || strcasecmp(clean_val, "0") == 0 ||
                       strcasecmp(clean_val, "no") == 0 || strcasecmp(clean_val, "off") == 0) {
                cursor_blink_enabled = 0;
                default_cursor_blink = 0;
            }
        } else if (strcasecmp(key, "keybind") == 0 || strcasecmp(key, "keybinding") == 0 || strcasecmp(key, "bind") == 0) {
            parse_keybind_line(clean_val);
        } else if (strcasecmp(key, "copy_keys") == 0 || strcasecmp(key, "copy_key") == 0 ||
                   strcasecmp(key, "copy-keys") == 0 || strcasecmp(key, "copy-key") == 0) {
            parse_key_list(clean_val, ACTION_COPY);
        } else if (strcasecmp(key, "paste_keys") == 0 || strcasecmp(key, "paste_key") == 0 ||
                   strcasecmp(key, "paste-keys") == 0 || strcasecmp(key, "paste-key") == 0) {
            parse_key_list(clean_val, ACTION_PASTE);
        }
    }
    fclose(f);
}

static void load_config(void) {
    char cfg_path[PATH_MAX] = {0};
    if (resolve_config_path(cfg_path, sizeof(cfg_path))) {
        load_config_from_file(cfg_path);
    }
}

static char active_theme_path[PATH_MAX] = {0};
static time_t active_theme_mtime = 0;
static ino_t active_theme_ino = 0;
static uint64_t last_theme_check_us = 0;

static int resolve_theme_path(char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return 0;
    out_path[0] = '\0';

    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[PATH_MAX];

    // 1. User explicit override: $XDG_CONFIG_HOME/dwmterm/colors or ~/.config/dwmterm/colors
    if (xdg && *xdg) {
        snprintf(path, sizeof(path), "%s/dwmterm/colors", xdg);
        if (access(path, R_OK) == 0) {
            strncpy(out_path, path, max_len - 1);
            out_path[max_len - 1] = '\0';
            return 1;
        }
    }
    if (home && *home) {
        snprintf(path, sizeof(path), "%s/.config/dwmterm/colors", home);
        if (access(path, R_OK) == 0) {
            strncpy(out_path, path, max_len - 1);
            out_path[max_len - 1] = '\0';
            return 1;
        }
    }

    // 2. Active desktop session theme: ~/.local/state/$DESKTOP_SESSION/current/theme/colors.toml or ghostty.conf
    const char *desktop_session = getenv("DESKTOP_SESSION");
    if (home && *home && desktop_session && *desktop_session) {
        snprintf(path, sizeof(path), "%s/.local/state/%s/current/theme/colors.toml", home, desktop_session);
        if (access(path, R_OK) == 0) {
            strncpy(out_path, path, max_len - 1);
            out_path[max_len - 1] = '\0';
            return 1;
        }
        snprintf(path, sizeof(path), "%s/.local/state/%s/current/theme/ghostty.conf", home, desktop_session);
        if (access(path, R_OK) == 0) {
            strncpy(out_path, path, max_len - 1);
            out_path[max_len - 1] = '\0';
            return 1;
        }
    }

    // 3. DWM-Titus theme: $XDG_CONFIG_HOME/dwm/themes.toml or ~/.config/dwm-titus/themes.toml
    if (xdg && *xdg) {
        snprintf(path, sizeof(path), "%s/dwm/themes.toml", xdg);
        if (access(path, R_OK) == 0) {
            strncpy(out_path, path, max_len - 1);
            out_path[max_len - 1] = '\0';
            return 1;
        }
    }
    if (home && *home) {
        snprintf(path, sizeof(path), "%s/.config/dwm/themes.toml", home);
        if (access(path, R_OK) == 0) {
            strncpy(out_path, path, max_len - 1);
            out_path[max_len - 1] = '\0';
            return 1;
        }
        snprintf(path, sizeof(path), "%s/.config/dwm-titus/themes.toml", home);
        if (access(path, R_OK) == 0) {
            strncpy(out_path, path, max_len - 1);
            out_path[max_len - 1] = '\0';
            return 1;
        }
    }

    return 0; // Built-in default
}

static void load_theme_colors_from_file(const char *custom_path, int initial) {
    uint32_t old_bg = color_bg;
    uint32_t old_fg = color_fg;

    // Reset to defaults first so missing keys in custom themes fallback cleanly
    memcpy(ansi_palette, default_ansi_palette, sizeof(ansi_palette));
    color_bg = DEFAULT_COLOR_BG;
    color_fg = DEFAULT_COLOR_FG;
    color_sel_bg = DEFAULT_COLOR_SEL_BG;
    color_sel_fg = DEFAULT_COLOR_SEL_FG;
    color_hud_bg = DEFAULT_COLOR_HUD_BG;
    color_hud_fg = DEFAULT_COLOR_HUD_FG;

    if (!custom_path || !*custom_path) {
        if (!initial) {
            int total_cells = live_term.cols * live_term.rows;
            for (int i = 0; i < total_cells; i++) {
                if (live_term.primary_grid && live_term.primary_grid[i].bg == old_bg) live_term.primary_grid[i].bg = color_bg;
                if (live_term.primary_grid && live_term.primary_grid[i].fg == old_fg) live_term.primary_grid[i].fg = color_fg;
                if (live_term.alt_grid && live_term.alt_grid[i].bg == old_bg) live_term.alt_grid[i].bg = color_bg;
                if (live_term.alt_grid && live_term.alt_grid[i].fg == old_fg) live_term.alt_grid[i].fg = color_fg;
                if (replay_term.primary_grid && replay_term.primary_grid[i].bg == old_bg) replay_term.primary_grid[i].bg = color_bg;
                if (replay_term.primary_grid && replay_term.primary_grid[i].fg == old_fg) replay_term.primary_grid[i].fg = color_fg;
                if (replay_term.alt_grid && replay_term.alt_grid[i].bg == old_bg) replay_term.alt_grid[i].bg = color_bg;
                if (replay_term.alt_grid && replay_term.alt_grid[i].fg == old_fg) replay_term.alt_grid[i].fg = color_fg;
            }
            if (live_term.cur_bg == old_bg) live_term.cur_bg = color_bg;
            if (live_term.cur_fg == old_fg) live_term.cur_fg = color_fg;
            if (replay_term.cur_bg == old_bg) replay_term.cur_bg = color_bg;
            if (replay_term.cur_fg == old_fg) replay_term.cur_fg = color_fg;
            if (win && dpy) {
                XSetWindowBackground(dpy, win, color_bg);
            }
            if (dirty) {
                memset(dirty, 1, live_term.rows * sizeof(uint8_t));
            }
            dirty_all = 1;
        }
        return;
    }

    FILE *f = fopen(custom_path, "r");
    if (!f) return;

    char active_theme_name[64] = {0};
    char target_section[128] = {0};
    char current_section[128] = {0};
    char line[512];

    // Pass 1: detect if this is a sectioned TOML file with [active] theme = "name"
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        if (*p == '[') {
            char *end_b = strchr(p, ']');
            if (end_b) {
                size_t slen = (size_t)(end_b - (p + 1));
                if (slen >= sizeof(current_section)) slen = sizeof(current_section) - 1;
                strncpy(current_section, p + 1, slen);
                current_section[slen] = '\0';
                char *cs_end = current_section + strlen(current_section) - 1;
                while (cs_end >= current_section && (*cs_end == ' ' || *cs_end == '\t')) { *cs_end = '\0'; cs_end--; }
            }
            continue;
        }
        if (strcasecmp(current_section, "active") == 0) {
            char *sep = strpbrk(p, "=:");
            if (sep) {
                char k[64] = {0}, v[64] = {0};
                size_t klen = (size_t)(sep - p);
                if (klen >= sizeof(k)) klen = sizeof(k) - 1;
                strncpy(k, p, klen);
                k[klen] = '\0';
                char *ke = k + strlen(k) - 1;
                while (ke >= k && (*ke == ' ' || *ke == '\t')) { *ke = '\0'; ke--; }

                char *val_p = sep + 1;
                while (*val_p == ' ' || *val_p == '\t' || *val_p == '"' || *val_p == '\'') val_p++;
                strncpy(v, val_p, sizeof(v) - 1);
                char *ve = v + strlen(v) - 1;
                while (ve >= v && (*ve == ' ' || *ve == '\t' || *ve == '"' || *ve == '\'' || *ve == '\n' || *ve == '\r')) { *ve = '\0'; ve--; }

                if (strcasecmp(k, "theme") == 0 && v[0]) {
                    snprintf(active_theme_name, sizeof(active_theme_name), "%s", v);
                    snprintf(target_section, sizeof(target_section), "theme.%s", active_theme_name);
                }
            }
        }
    }

    fseek(f, 0, SEEK_SET);
    current_section[0] = '\0';

    // Pass 2: parse key/value pairs
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        if (*p == '[') {
            char *end_b = strchr(p, ']');
            if (end_b) {
                size_t slen = (size_t)(end_b - (p + 1));
                if (slen >= sizeof(current_section)) slen = sizeof(current_section) - 1;
                strncpy(current_section, p + 1, slen);
                current_section[slen] = '\0';
                char *cs_end = current_section + strlen(current_section) - 1;
                while (cs_end >= current_section && (*cs_end == ' ' || *cs_end == '\t')) { *cs_end = '\0'; cs_end--; }
            }
            continue;
        }

        // Section filter
        int in_valid_section = 0;
        if (target_section[0] != '\0') {
            in_valid_section = (strcasecmp(current_section, target_section) == 0);
        } else {
            in_valid_section = (current_section[0] == '\0' ||
                                strcasecmp(current_section, "theme") == 0 ||
                                strcasecmp(current_section, "colors") == 0);
        }
        if (!in_valid_section) continue;

        char key[64] = {0};
        char val[128] = {0};
        char *sep = strpbrk(p, "=:");
        if (sep) {
            size_t klen = (size_t)(sep - p);
            if (klen >= sizeof(key)) klen = sizeof(key) - 1;
            strncpy(key, p, klen);
            key[klen] = '\0';
            char *ke = key + strlen(key) - 1;
            while (ke >= key && (*ke == ' ' || *ke == '\t')) { *ke = '\0'; ke--; }

            char *v = sep + 1;
            while (*v == ' ' || *v == '\t') v++;
            strncpy(val, v, sizeof(val) - 1);
            char *ve = val + strlen(val) - 1;
            while (ve >= val && (*ve == ' ' || *ve == '\t' || *ve == '\n' || *ve == '\r')) { *ve = '\0'; ve--; }
        } else {
            if (sscanf(p, "%63s %127s", key, val) != 2) continue;
        }

        // Strip surrounding quotes
        char *clean_val = val;
        while (*clean_val == '"' || *clean_val == '\'') clean_val++;
        char *cve = clean_val + strlen(clean_val) - 1;
        while (cve >= clean_val && (*cve == '"' || *cve == '\'')) { *cve = '\0'; cve--; }

        if (strcasecmp(key, "background") == 0 || strcasecmp(key, "bg") == 0 || strcasecmp(key, "term_bg") == 0) {
            color_bg = parse_hex_color(clean_val, color_bg);
            ansi_palette[0] = color_bg;
        } else if (strcasecmp(key, "foreground") == 0 || strcasecmp(key, "fg") == 0 || strcasecmp(key, "term_fg") == 0) {
            color_fg = parse_hex_color(clean_val, color_fg);
            ansi_palette[7] = color_fg;
        } else if (strcasecmp(key, "selection") == 0 || strcasecmp(key, "selection_bg") == 0 ||
                   strcasecmp(key, "selection-background") == 0 || strcasecmp(key, "sel_bg") == 0) {
            color_sel_bg = parse_hex_color(clean_val, color_sel_bg);
        } else if (strcasecmp(key, "selection_fg") == 0 || strcasecmp(key, "selection-foreground") == 0 ||
                   strcasecmp(key, "sel_fg") == 0) {
            color_sel_fg = parse_hex_color(clean_val, color_sel_fg);
        } else if (strcasecmp(key, "black") == 0 || strcasecmp(key, "term_color0") == 0) {
            ansi_palette[0] = parse_hex_color(clean_val, ansi_palette[0]);
        } else if (strcasecmp(key, "red") == 0 || strcasecmp(key, "term_color1") == 0) {
            ansi_palette[1] = parse_hex_color(clean_val, ansi_palette[1]);
        } else if (strcasecmp(key, "green") == 0 || strcasecmp(key, "term_color2") == 0) {
            ansi_palette[2] = parse_hex_color(clean_val, ansi_palette[2]);
        } else if (strcasecmp(key, "yellow") == 0 || strcasecmp(key, "term_color3") == 0) {
            ansi_palette[3] = parse_hex_color(clean_val, ansi_palette[3]);
        } else if (strcasecmp(key, "blue") == 0 || strcasecmp(key, "term_color4") == 0) {
            ansi_palette[4] = parse_hex_color(clean_val, ansi_palette[4]);
        } else if (strcasecmp(key, "magenta") == 0 || strcasecmp(key, "purple") == 0 || strcasecmp(key, "term_color5") == 0) {
            ansi_palette[5] = parse_hex_color(clean_val, ansi_palette[5]);
        } else if (strcasecmp(key, "cyan") == 0 || strcasecmp(key, "term_color6") == 0) {
            ansi_palette[6] = parse_hex_color(clean_val, ansi_palette[6]);
        } else if (strcasecmp(key, "white") == 0 || strcasecmp(key, "term_color7") == 0) {
            ansi_palette[7] = parse_hex_color(clean_val, ansi_palette[7]);
        } else if (strcasecmp(key, "muted") == 0 || strcasecmp(key, "bright_black") == 0 ||
                   strcasecmp(key, "dark_foreground") == 0 || strcasecmp(key, "term_color8") == 0) {
            ansi_palette[8] = parse_hex_color(clean_val, ansi_palette[8]);
        } else if (strcasecmp(key, "bright_red") == 0 || strcasecmp(key, "term_color9") == 0) {
            ansi_palette[9] = parse_hex_color(clean_val, ansi_palette[9]);
        } else if (strcasecmp(key, "bright_green") == 0 || strcasecmp(key, "term_color10") == 0) {
            ansi_palette[10] = parse_hex_color(clean_val, ansi_palette[10]);
        } else if (strcasecmp(key, "bright_yellow") == 0 || strcasecmp(key, "term_color11") == 0) {
            ansi_palette[11] = parse_hex_color(clean_val, ansi_palette[11]);
        } else if (strcasecmp(key, "bright_blue") == 0 || strcasecmp(key, "term_color12") == 0) {
            ansi_palette[12] = parse_hex_color(clean_val, ansi_palette[12]);
        } else if (strcasecmp(key, "bright_magenta") == 0 || strcasecmp(key, "bright_purple") == 0 ||
                   strcasecmp(key, "term_color13") == 0) {
            ansi_palette[13] = parse_hex_color(clean_val, ansi_palette[13]);
        } else if (strcasecmp(key, "bright_cyan") == 0 || strcasecmp(key, "term_color14") == 0) {
            ansi_palette[14] = parse_hex_color(clean_val, ansi_palette[14]);
        } else if (strcasecmp(key, "bright_white") == 0 || strcasecmp(key, "bright_foreground") == 0 ||
                   strcasecmp(key, "light_foreground") == 0 || strcasecmp(key, "term_color15") == 0) {
            ansi_palette[15] = parse_hex_color(clean_val, ansi_palette[15]);
        } else if (strncasecmp(key, "color", 5) == 0 && (key[5] >= '0' && key[5] <= '9')) {
            int c_idx = atoi(key + 5);
            if (c_idx >= 0 && c_idx < 16) {
                ansi_palette[c_idx] = parse_hex_color(clean_val, ansi_palette[c_idx]);
            }
        } else if (strcasecmp(key, "palette") == 0) {
            int c_idx = atoi(clean_val);
            char *peq = strchr(clean_val, '=');
            if (peq && c_idx >= 0 && c_idx < 16) {
                ansi_palette[c_idx] = parse_hex_color(peq + 1, ansi_palette[c_idx]);
            }
        }
    }
    fclose(f);

    if (!initial) {
        if (color_bg != old_bg || color_fg != old_fg) {
            int total_cells = live_term.cols * live_term.rows;
            for (int i = 0; i < total_cells; i++) {
                if (live_term.primary_grid && live_term.primary_grid[i].bg == old_bg) live_term.primary_grid[i].bg = color_bg;
                if (live_term.primary_grid && live_term.primary_grid[i].fg == old_fg) live_term.primary_grid[i].fg = color_fg;
                if (live_term.alt_grid && live_term.alt_grid[i].bg == old_bg) live_term.alt_grid[i].bg = color_bg;
                if (live_term.alt_grid && live_term.alt_grid[i].fg == old_fg) live_term.alt_grid[i].fg = color_fg;

                if (replay_term.primary_grid && replay_term.primary_grid[i].bg == old_bg) replay_term.primary_grid[i].bg = color_bg;
                if (replay_term.primary_grid && replay_term.primary_grid[i].fg == old_fg) replay_term.primary_grid[i].fg = color_fg;
                if (replay_term.alt_grid && replay_term.alt_grid[i].bg == old_bg) replay_term.alt_grid[i].bg = color_bg;
                if (replay_term.alt_grid && replay_term.alt_grid[i].fg == old_fg) replay_term.alt_grid[i].fg = color_fg;
            }
            if (live_term.cur_bg == old_bg) live_term.cur_bg = color_bg;
            if (live_term.cur_fg == old_fg) live_term.cur_fg = color_fg;
            if (replay_term.cur_bg == old_bg) replay_term.cur_bg = color_bg;
            if (replay_term.cur_fg == old_fg) replay_term.cur_fg = color_fg;
        }
        if (win && dpy) {
            XSetWindowBackground(dpy, win, color_bg);
        }
        if (dirty) {
            memset(dirty, 1, live_term.rows * sizeof(uint8_t));
        }
        dirty_all = 1;
    }
}

static void load_theme_colors(int initial) {
    char theme_path[PATH_MAX] = {0};
    int found = resolve_theme_path(theme_path, sizeof(theme_path));
    if (found && theme_path[0]) {
        load_theme_colors_from_file(theme_path, initial);
        struct stat st;
        if (stat(theme_path, &st) == 0) {
            strncpy(active_theme_path, theme_path, sizeof(active_theme_path) - 1);
            active_theme_path[sizeof(active_theme_path) - 1] = '\0';
            active_theme_mtime = st.st_mtime;
            active_theme_ino = st.st_ino;
        }
    } else {
        load_theme_colors_from_file(NULL, initial);
        active_theme_path[0] = '\0';
        active_theme_mtime = 0;
        active_theme_ino = 0;
    }
}

static void check_and_reload_theme(void) {
    char cur_path[PATH_MAX] = {0};
    int found = resolve_theme_path(cur_path, sizeof(cur_path));

    if (strcmp(cur_path, active_theme_path) != 0) {
        load_theme_colors(0);
        return;
    }

    if (found && cur_path[0]) {
        struct stat st;
        if (stat(cur_path, &st) == 0) {
            if (st.st_mtime != active_theme_mtime || st.st_ino != active_theme_ino) {
                load_theme_colors(0);
            }
        }
    }
}

static void configure_child_env(void) {
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "dwmterm", 0);
    setenv("TERM_PROGRAM_VERSION", VERSION, 0);
#ifdef DATADIR
    setenv("DWMTERM_DATADIR", DATADIR, 0);
#endif
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    init_gamma_lut();
    init_default_keybindings();
    const char *opt_title = NULL;
    const char *opt_dir = NULL;
    char **cmd_argv = NULL;

    load_config();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dwmterm: -e requires an argument\n");
                return 1;
            }
            cmd_argv = &argv[i + 1];
            break;
        } else if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dwmterm: %s requires an argument\n", argv[i]);
                return 1;
            }
            opt_title = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--working-directory") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dwmterm: %s requires an argument\n", argv[i]);
                return 1;
            }
            opt_dir = argv[++i];
        } else if (strncmp(argv[i], "--working-directory=", 20) == 0) {
            opt_dir = argv[i] + 20;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--font-size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dwmterm: %s requires an argument\n", argv[i]);
                return 1;
            }
            int sz = atoi(argv[++i]);
            if (sz >= 6 && sz <= 72) {
                font_pt = sz;
                default_font_pt = sz;
            }
        } else if (strncmp(argv[i], "--font-size=", 12) == 0) {
            int sz = atoi(argv[i] + 12);
            if (sz >= 6 && sz <= 72) {
                font_pt = sz;
                default_font_pt = sz;
            }
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--font") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dwmterm: %s requires an argument\n", argv[i]);
                return 1;
            }
            const char *farg = argv[++i];
            strncpy(config_font_family, farg, sizeof(config_font_family) - 1);
            config_font_family[sizeof(config_font_family) - 1] = '\0';
        } else if (strncmp(argv[i], "--font=", 7) == 0) {
            const char *farg = argv[i] + 7;
            strncpy(config_font_family, farg, sizeof(config_font_family) - 1);
            config_font_family[sizeof(config_font_family) - 1] = '\0';
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--padding") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dwmterm: %s requires an argument\n", argv[i]);
                return 1;
            }
            int p = atoi(argv[++i]);
            if (p >= 0 && p <= 100) { padding = p; padding_x = p; padding_y = p; }
        } else if (strncmp(argv[i], "--padding=", 10) == 0) {
            int p = atoi(argv[i] + 10);
            if (p >= 0 && p <= 100) { padding = p; padding_x = p; padding_y = p; }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("dwmterm %s\n", VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: dwmterm [options] [-e <cmd> [args...]]\n\n"
                   "Options:\n"
                   "  -e <cmd> [args...]             Execute command with arguments instead of shell\n"
                   "  -T, -t <title>                 Override initial window title\n"
                   "  -d, --working-directory <dir>  Set starting working directory\n"
                   "  -s, --font-size <pt>           Set font size in points (6..72, default: 12)\n"
                   "  -f, --font <family>            Set font family (e.g. 'MesloLGS Nerd Font')\n"
                   "  -p, --padding <px>             Set internal window padding in pixels (default: 12)\n"
                   "  -v, --version                  Display version information and exit\n"
                   "  -h, --help                     Display this help message and exit\n");
            return 0;
        } else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) {
                cmd_argv = &argv[i + 1];
            }
            break;
        } else {
            fprintf(stderr, "dwmterm: invalid option '%s'\n", argv[i]);
            fprintf(stderr, "Try 'dwmterm --help' for more information.\n");
            return 1;
        }
    }

    if (opt_dir) {
        struct stat st;
        if (stat(opt_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "dwmterm: cannot access directory '%s': No such directory\n", opt_dir);
            return 1;
        }
    }

    load_theme_colors(1);
    signal(SIGUSR1, handle_sigusr1);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open X display\n");
        return 1;
    }
    detect_modifier_masks(dpy);
    update_display_dpi(dpy);

    if (FT_Init_FreeType(&ft_lib)) {
        fprintf(stderr, "Failed to initialize FreeType\n");
        XCloseDisplay(dpy);
        return 1;
    }

    if (!FcInit()) {
        fprintf(stderr, "Failed to initialize Fontconfig\n");
        FT_Done_FreeType(ft_lib);
        XCloseDisplay(dpy);
        return 1;
    }

    int face_idx = 0;
    char *font_file = NULL;
    ft_face = NULL;

    // 0. Check custom font family if configured
    if (config_font_family[0]) {
        font_file = resolve_font_path(config_font_family, 1, &face_idx);
        if (font_file && FT_New_Face(ft_lib, font_file, face_idx, &ft_face) == 0) {
            // Successfully loaded custom font
        } else {
            if (font_file) { free(font_file); font_file = NULL; }
            fprintf(stderr, "dwmterm: warning: could not load requested font '%s', falling back to defaults\n", config_font_family);
        }
    }

    if (!ft_face) {
        // 1. Cascade through system Fontconfig monospace patterns and aliases
        const char *fc_cascade[] = {
            "monospace",
            "ui-monospace",
            "fixed",
            "terminal",
            ":spacing=mono",
            NULL
        };
        for (int i = 0; fc_cascade[i]; i++) {
            font_file = resolve_font_path(fc_cascade[i], 0, &face_idx);
            if (font_file) {
                if (FT_New_Face(ft_lib, font_file, face_idx, &ft_face) == 0) {
                    break;
                }
                free(font_file);
                font_file = NULL;
            }
        }
    }

    if (!ft_face) {
        // 2. Check bundled Meslo Nerd Font relative to executable, DATADIR, or cwd
        char exe_buf[1024];
        ssize_t elen = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
        if (elen > 0) {
            exe_buf[elen] = '\0';
            char *slash = strrchr(exe_buf, '/');
            if (slash) {
                *slash = '\0';
                char fpath[1024];
                if (snprintf(fpath, sizeof(fpath), "%s/fonts/MesloLGSNerdFont-Regular.ttf", exe_buf) < (int)sizeof(fpath) && access(fpath, R_OK) == 0) {
                    font_file = strdup(fpath);
                } else if (snprintf(fpath, sizeof(fpath), "%s/../share/dwmterm/fonts/MesloLGSNerdFont-Regular.ttf", exe_buf) < (int)sizeof(fpath) && access(fpath, R_OK) == 0) {
                    font_file = strdup(fpath);
                }
            }
        }
        if (!font_file) {
            const char *sys_paths[] = {
#ifdef DATADIR
                DATADIR "/dwmterm/fonts/MesloLGSNerdFont-Regular.ttf",
#endif
                "/usr/local/share/dwmterm/fonts/MesloLGSNerdFont-Regular.ttf",
                "/usr/share/dwmterm/fonts/MesloLGSNerdFont-Regular.ttf",
                "fonts/MesloLGSNerdFont-Regular.ttf",
                "./fonts/MesloLGSNerdFont-Regular.ttf",
                NULL
            };
            for (int i = 0; sys_paths[i]; i++) {
                if (access(sys_paths[i], R_OK) == 0) {
                    font_file = strdup(sys_paths[i]);
                    break;
                }
            }
        }
        if (font_file) {
            if (FT_New_Face(ft_lib, font_file, face_idx, &ft_face) != 0) {
                free(font_file);
                font_file = NULL;
            }
        }
    }

    if (!ft_face) {
        // 3. Fall back to remaining Fontconfig lookups
        const char *fallback_names[] = {
            "MesloLGS Nerd Font",
            "Meslo Nerd Font",
            "JetBrainsMono Nerd Font",
            "Nerd Font",
            NULL
        };
        for (int i = 0; fallback_names[i]; i++) {
            font_file = resolve_font_path(fallback_names[i], 0, &face_idx);
            if (font_file) {
                if (FT_New_Face(ft_lib, font_file, face_idx, &ft_face) == 0) {
                    break;
                }
                free(font_file);
                font_file = NULL;
            }
        }
        if (!ft_face) {
            fprintf(stderr, "dwmterm: could not load system monospace or fallback fonts\n");
            if (font_file) free(font_file);
            FcFini();
            FT_Done_FreeType(ft_lib);
            XCloseDisplay(dpy);
            return 1;
        }
    }
    if (font_file) free(font_file);
    FcFini();

    FT_Set_Pixel_Sizes(ft_face, 0, pt_to_px(font_pt));
    FT_Load_Char(ft_face, 'M', FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT);

    char_w = ft_face->glyph->advance.x >> 6;
    char_h = ft_face->size->metrics.height >> 6;
    ascender_px = ft_face->size->metrics.ascender >> 6;
    if (char_w <= 0) char_w = 8;
    if (char_h <= 0) char_h = 16;

    win_w = cols * char_w + padding_x * 2;
    win_h = rows * char_h + padding_y * 2;
    term_init(&live_term, cols, rows);
    term_init(&replay_term, cols, rows);

    dirty = calloc(rows, sizeof(uint8_t));
    memset(dirty, 1, rows * sizeof(uint8_t));

    struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols, .ws_xpixel = (unsigned short)win_w, .ws_ypixel = (unsigned short)win_h };
    pid_t pid = forkpty(&pty_master, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_lib);
        XCloseDisplay(dpy);
        return 1;
    }
    if (pid == 0) {
        if (pty_master > 2) close(pty_master);
        if (opt_dir) {
            if (chdir(opt_dir) != 0) {
                perror("dwmterm: chdir");
            }
        }
        configure_child_env();
        if (cmd_argv) {
            execvp(cmd_argv[0], cmd_argv);
            perror("execvp");
            _exit(127);
        }
        const char *shell = getenv("SHELL");
        if (!shell || !*shell) {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_shell && *pw->pw_shell) {
                shell = pw->pw_shell;
            } else {
                shell = "/bin/sh";
            }
        }

        execl(shell, shell, (char *)NULL);
        _exit(1);
    }

    int flags = fcntl(pty_master, F_GETFL, 0);
    fcntl(pty_master, F_SETFL, flags | O_NONBLOCK);

    int screen = DefaultScreen(dpy);
    vis = DefaultVisual(dpy, screen);
    depth = DefaultDepth(dpy, screen);
    Window root = RootWindow(dpy, screen);
    win = XCreateSimpleWindow(dpy, root, 100, 100, win_w, win_h, 0, 0, COLOR_BG);
    gc = XCreateGC(dpy, win, 0, NULL);

    if (init_framebuffer(win_w, win_h) != 0) {
        fprintf(stderr, "Failed to initialize framebuffer\n");
        kill(pid, SIGTERM);
        return 1;
    }

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask |
                           ButtonPressMask | ButtonReleaseMask | ButtonMotionMask);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    atom_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    atom_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    atom_targets = XInternAtom(dpy, "TARGETS", False);
    atom_sel_data = XInternAtom(dpy, "DWMTERM_SELECTION", False);
    atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_net_wm_icon_name = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
    atom_net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    atom_net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);

    XClassHint class_hint;
    class_hint.res_name = "dwmterm";
    class_hint.res_class = "Dwmterm";
    XSetClassHint(dpy, win, &class_hint);

    update_wm_normal_hints();

    long pid_val = (long)getpid();
    XChangeProperty(dpy, win, atom_net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&pid_val, 1);

    set_window_title(opt_title ? opt_title : "DWM-Terminal");
    setup_window_icon(dpy, win);
    XMapWindow(dpy, win);

    struct pollfd fds[2];
    fds[0].fd = pty_master;
    fds[0].events = POLLIN;
    fds[1].fd = ConnectionNumber(dpy);
    fds[1].events = POLLIN;

    int running = 1;
    int child_alive = 1;

    while (running) {
        if (sig_reload_theme) {
            sig_reload_theme = 0;
            load_theme_colors(0);
        } else {
            uint64_t now_us = get_time_us();
            if (now_us - last_theme_check_us >= 500000ULL) {
                last_theme_check_us = now_us;
                check_and_reload_theme();
            }
        }

        if (cursor_blink_enabled && !replay_mode && live_term.cursor_visible) {
            uint64_t now_blink = get_time_us();
            if (now_blink - last_cursor_blink_us >= 500000ULL) {
                last_cursor_blink_us = now_blink;
                cursor_blink_state = !cursor_blink_state;
                dirty_all = 1;
            }
        }

        render_frame();

        int status;
        pid_t wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid || (wpid < 0 && errno != EINTR)) {
            child_alive = 0;
            char buf[16384];
            ssize_t n;
            while ((n = read(pty_master, buf, sizeof(buf))) > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    term_put_byte_internal(&live_term, (unsigned char)buf[i], 1);
                }
            }
            render_frame();
            break;
        }

        int ret = poll(fds, 2, 20);
        if (ret < 0 && errno == EINTR) continue;

        if (fds[0].revents & POLLIN) {
            if (cursor_blink_enabled) {
                cursor_blink_state = 1;
                last_cursor_blink_us = get_time_us();
            }
            char buf[16384];
            ssize_t n;
            while ((n = read(pty_master, buf, sizeof(buf))) > 0) {
                // Record into Flight Log
                if (flight_start_us == 0) flight_start_us = get_time_us();
                uint64_t now = get_time_us();

                int f_idx = flight_head;
                if (flight_log[f_idx].data) free(flight_log[f_idx].data);
                flight_log[f_idx].ts_us = now;
                flight_log[f_idx].len = n;
                flight_log[f_idx].data = malloc(n);
                if (flight_log[f_idx].data) memcpy(flight_log[f_idx].data, buf, n);

                flight_head = (flight_head + 1) % MAX_FLIGHT_CHUNKS;
                if (flight_count < MAX_FLIGHT_CHUNKS) flight_count++;
                flight_total_recorded++;

                // Parse into Live Terminal
                for (ssize_t i = 0; i < n; i++) {
                    term_put_byte_internal(&live_term, (unsigned char)buf[i], 1);
                }

                if (!replay_mode && scroll_offset > 0) {
                    scroll_offset = 0;
                    dirty_all = 1;
                }
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                running = 0;
            }
        }

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == KeyPress) {
                char kbuf[32];
                KeySym ksym;
                int len = XLookupString(&ev.xkey, kbuf, sizeof(kbuf), &ksym, NULL);
                unsigned int state = CLEAN_MASK(ev.xkey.state);

                if (cursor_blink_enabled) {
                    cursor_blink_state = 1;
                    last_cursor_blink_us = get_time_us();
                }

                if (hud_message[0] != '\0') {
                    hud_message[0] = '\0';
                    dirty_all = 1;
                    continue;
                }

                // Toggle Replay Mode with F1 or Ctrl+Shift+R
                if (ksym == XK_F1 || ((state & ControlMask) && (state & ShiftMask) && (ksym == XK_R || ksym == XK_r))) {
                    replay_mode = !replay_mode;
                    if (replay_mode) {
                        scroll_offset = 0;
                        scrub_to_chunk(flight_count - 1);
                    } else {
                        dirty_all = 1;
                    }
                    continue;
                }

                if (replay_mode) {
                    // Replay Scrubber Navigation
                    if (ksym == XK_Escape || ksym == XK_q) {
                        replay_mode = 0;
                        dirty_all = 1;
                    } else if (ksym == XK_Left || ksym == XK_h) {
                        scrub_to_chunk(replay_chunk_idx - ((state & ShiftMask) ? 10 : 1));
                    } else if (ksym == XK_Right || ksym == XK_l) {
                        scrub_to_chunk(replay_chunk_idx + ((state & ShiftMask) ? 10 : 1));
                    } else if (ksym == XK_Home || ksym == XK_0) {
                        scrub_to_chunk(0);
                    } else if (ksym == XK_End || ksym == XK_dollar) {
                        scrub_to_chunk(flight_count - 1);
                    } else if (ksym == XK_e) {
                        export_asciinema();
                    }
                    continue;
                }

                // Font Zooming via Ctrl+Plus / Ctrl+Minus / Ctrl+0
                if (state & ControlMask) {
                    if (ksym == XK_equal || ksym == XK_plus || ksym == XK_KP_Add) {
                        set_font_size(font_pt + 2);
                        continue;
                    } else if (ksym == XK_minus || ksym == XK_underscore || ksym == XK_KP_Subtract) {
                        set_font_size(font_pt - 2);
                        continue;
                    } else if (ksym == XK_0 || ksym == XK_KP_0) {
                        set_font_size(default_font_pt);
                        continue;
                    }
                }

                // Configurable keybindings: Copy, Paste, etc.
                int act = match_keybinding(state, ksym);
                if (act == ACTION_COPY) {
                    copy_selection_text();
                    continue;
                } else if (act == ACTION_PASTE) {
                    XConvertSelection(dpy, atom_clipboard, atom_utf8, atom_sel_data, win, CurrentTime);
                    continue;
                }

                // Scrollback navigation via Shift+PageUp / Shift+PageDown
                if (state & ShiftMask) {
                    if (ksym == XK_Page_Up && !live_term.is_alt_screen) {
                        scroll_offset += rows / 2;
                        if (scroll_offset > hist_count) scroll_offset = hist_count;
                        dirty_all = 1;
                        continue;
                    } else if (ksym == XK_Page_Down && !live_term.is_alt_screen) {
                        scroll_offset -= rows / 2;
                        if (scroll_offset < 0) scroll_offset = 0;
                        dirty_all = 1;
                        continue;
                    }
                }

                if (scroll_offset > 0) {
                    scroll_offset = 0;
                    dirty_all = 1;
                }

                // Clear selection on typing
                if (sel_active || sel_start_r >= 0) {
                    sel_active = 0;
                    sel_start_r = sel_start_c = sel_end_r = sel_end_c = -1;
                    dirty_all = 1;
                }

                if (ksym == XK_Return || ksym == XK_KP_Enter) {
                    if ((state & ShiftMask) && (state & Mod1Mask)) {
                        pty_write(pty_master, "\x1b[13;4u", 7);
                    } else if (state & ShiftMask) {
                        pty_write(pty_master, "\x1b[13;2u", 7);
                    } else if (state & Mod1Mask) {
                        pty_write(pty_master, "\x1b\r", 2);
                    } else {
                        pty_write(pty_master, "\r", 1);
                    }
                } else if (ksym == XK_BackSpace || (len == 1 && (unsigned char)kbuf[0] == 0x08)) {
                    pty_write(pty_master, "\x7f", 1);
                } else if (ksym == XK_Delete) {
                    pty_write(pty_master, "\x1b[3~", 4);
                } else if (ksym == XK_Insert) {
                    pty_write(pty_master, "\x1b[2~", 4);
                } else if (ksym == XK_Home) {
                    pty_write(pty_master, app_cursor_keys ? "\x1bOH" : "\x1b[H", 3);
                } else if (ksym == XK_End) {
                    pty_write(pty_master, app_cursor_keys ? "\x1bOF" : "\x1b[F", 3);
                } else if (ksym == XK_Page_Up || ksym == XK_Prior) {
                    pty_write(pty_master, "\x1b[5~", 4);
                } else if (ksym == XK_Page_Down || ksym == XK_Next) {
                    pty_write(pty_master, "\x1b[6~", 4);
                } else if (len > 0) {
                    pty_write(pty_master, kbuf, len);
                } else {
                    if (ksym == XK_Up) pty_write(pty_master, app_cursor_keys ? "\x1bOA" : "\x1b[A", 3);
                    else if (ksym == XK_Down) pty_write(pty_master, app_cursor_keys ? "\x1bOB" : "\x1b[B", 3);
                    else if (ksym == XK_Right) pty_write(pty_master, app_cursor_keys ? "\x1bOC" : "\x1b[C", 3);
                    else if (ksym == XK_Left) pty_write(pty_master, app_cursor_keys ? "\x1bOD" : "\x1b[D", 3);
                }
            } else if (ev.type == ButtonPress) {
                if (replay_mode) continue;
                int c = (ev.xbutton.x - padding_x) / char_w;
                int r = (ev.xbutton.y - padding_y) / char_h;
                if (c < 0) c = 0;
                if (c >= cols) c = cols - 1;
                if (r < 0) r = 0;
                if (r >= rows) r = rows - 1;

                if (mouse_mode && !(ev.xbutton.state & ShiftMask)) {
                    int btn = -1;
                    if (ev.xbutton.button == Button1) btn = 0;
                    else if (ev.xbutton.button == Button2) btn = 1;
                    else if (ev.xbutton.button == Button3) btn = 2;
                    else if (ev.xbutton.button == Button4) btn = 64;
                    else if (ev.xbutton.button == Button5) btn = 65;

                    if (btn >= 0) {
                        if (ev.xbutton.state & ShiftMask) btn |= 4;
                        if (ev.xbutton.state & Mod1Mask) btn |= 8;
                        if (ev.xbutton.state & ControlMask) btn |= 16;

                        char mbuf[32];
                        int mlen;
                        if (mouse_sgr) {
                            mlen = snprintf(mbuf, sizeof(mbuf), "\x1b[<%d;%d;%dM", btn, c + 1, r + 1);
                        } else if (btn < 32) {
                            mlen = snprintf(mbuf, sizeof(mbuf), "\x1b[M%c%c%c", 32 + btn, 33 + c, 33 + r);
                        } else {
                            mlen = 0;
                        }
                        if (mlen > 0) pty_write(pty_master, mbuf, (size_t)mlen);
                    }
                    continue;
                }

                if (ev.xbutton.button == Button1) {
                    sel_active = 1;
                    sel_start_c = sel_end_c = c;
                    sel_start_r = sel_end_r = r;
                    dirty_all = 1;
                } else if (ev.xbutton.button == Button2) {
                    XConvertSelection(dpy, XA_PRIMARY, atom_utf8, atom_sel_data, win, CurrentTime);
                } else if (ev.xbutton.button == Button4) {
                    if (live_term.is_alt_screen) {
                        pty_write(pty_master, "\x1b[A\x1b[A\x1b[A", 9);
                    } else {
                        scroll_offset += 3;
                        if (scroll_offset > hist_count) scroll_offset = hist_count;
                        dirty_all = 1;
                    }
                } else if (ev.xbutton.button == Button5) {
                    if (live_term.is_alt_screen) {
                        pty_write(pty_master, "\x1b[B\x1b[B\x1b[B", 9);
                    } else {
                        scroll_offset -= 3;
                        if (scroll_offset < 0) scroll_offset = 0;
                        dirty_all = 1;
                    }
                }
            } else if (ev.type == MotionNotify && !replay_mode) {
                int c = (ev.xmotion.x - padding_x) / char_w;
                int r = (ev.xmotion.y - padding_y) / char_h;
                if (c < 0) c = 0;
                if (c >= cols) c = cols - 1;
                if (r < 0) r = 0;
                if (r >= rows) r = rows - 1;

                if (mouse_mode && !(ev.xmotion.state & ShiftMask)) {
                    if (mouse_mode == 1002 && !(ev.xmotion.state & (Button1Mask | Button2Mask | Button3Mask))) {
                        continue;
                    }
                    int btn = 32;
                    if (ev.xmotion.state & Button1Mask) btn += 0;
                    else if (ev.xmotion.state & Button2Mask) btn += 1;
                    else if (ev.xmotion.state & Button3Mask) btn += 2;
                    else btn += 3;

                    if (ev.xmotion.state & ShiftMask) btn |= 4;
                    if (ev.xmotion.state & Mod1Mask) btn |= 8;
                    if (ev.xmotion.state & ControlMask) btn |= 16;

                    char mbuf[32];
                    int mlen;
                    if (mouse_sgr) {
                        mlen = snprintf(mbuf, sizeof(mbuf), "\x1b[<%d;%d;%dM", btn, c + 1, r + 1);
                    } else {
                        mlen = snprintf(mbuf, sizeof(mbuf), "\x1b[M%c%c%c", 32 + btn, 33 + c, 33 + r);
                    }
                    if (mlen > 0) pty_write(pty_master, mbuf, (size_t)mlen);
                    continue;
                }

                if (sel_active) {
                    if (c != sel_end_c || r != sel_end_r) {
                        sel_end_c = c;
                        sel_end_r = r;
                        dirty_all = 1;
                    }
                }
            } else if (ev.type == ButtonRelease && !replay_mode) {
                int c = (ev.xbutton.x - padding_x) / char_w;
                int r = (ev.xbutton.y - padding_y) / char_h;
                if (c < 0) c = 0;
                if (c >= cols) c = cols - 1;
                if (r < 0) r = 0;
                if (r >= rows) r = rows - 1;

                if (mouse_mode && !(ev.xbutton.state & ShiftMask)) {
                    int btn = -1;
                    if (ev.xbutton.button == Button1) btn = 0;
                    else if (ev.xbutton.button == Button2) btn = 1;
                    else if (ev.xbutton.button == Button3) btn = 2;

                    if (btn >= 0) {
                        if (ev.xbutton.state & ShiftMask) btn |= 4;
                        if (ev.xbutton.state & Mod1Mask) btn |= 8;
                        if (ev.xbutton.state & ControlMask) btn |= 16;

                        char mbuf[32];
                        int mlen;
                        if (mouse_sgr) {
                            mlen = snprintf(mbuf, sizeof(mbuf), "\x1b[<%d;%d;%dm", btn, c + 1, r + 1);
                        } else {
                            mlen = snprintf(mbuf, sizeof(mbuf), "\x1b[M%c%c%c", 32 + 3, 33 + c, 33 + r);
                        }
                        if (mlen > 0) pty_write(pty_master, mbuf, (size_t)mlen);
                    }
                    continue;
                }

                if (ev.xbutton.button == Button1 && sel_active) {
                    sel_active = 0;
                    sel_end_c = c;
                    sel_end_r = r;
                    if (sel_start_c == sel_end_c && sel_start_r == sel_end_r) {
                        sel_start_r = sel_start_c = sel_end_r = sel_end_c = -1;
                    } else {
                        copy_selection_text();
                    }
                    dirty_all = 1;
                }
            } else if (ev.type == SelectionRequest) {
                XSelectionRequestEvent *req = &ev.xselectionrequest;
                XSelectionEvent se;
                se.type = SelectionNotify;
                se.requestor = req->requestor;
                se.selection = req->selection;
                se.target = req->target;
                se.time = req->time;
                se.property = None;

                if (req->target == atom_targets) {
                    Atom targets[] = { atom_utf8, XA_STRING };
                    XChangeProperty(dpy, req->requestor, req->property, XA_ATOM, 32,
                                    PropModeReplace, (unsigned char *)targets, 2);
                    se.property = req->property;
                } else if (req->target == atom_utf8 || req->target == XA_STRING) {
                    if (sel_text) {
                        XChangeProperty(dpy, req->requestor, req->property, req->target, 8,
                                        PropModeReplace, (unsigned char *)sel_text, strlen(sel_text));
                        se.property = req->property;
                    }
                }
                XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&se);
            } else if (ev.type == SelectionNotify) {
                if (ev.xselection.property != None) {
                    Atom type;
                    int format;
                    unsigned long nitems, bytes_after;
                    unsigned char *prop = NULL;
                    if (XGetWindowProperty(dpy, win, ev.xselection.property, 0, 65536, True,
                                           AnyPropertyType, &type, &format, &nitems, &bytes_after, &prop) == Success) {
                        if (prop && nitems > 0) {
                            if (bracketed_paste) pty_write(pty_master, "\x1b[200~", 6);
                            pty_write(pty_master, prop, nitems);
                            if (bracketed_paste) pty_write(pty_master, "\x1b[201~", 6);
                        }
                        if (prop) XFree(prop);
                    }
                }
            } else if (ev.type == ConfigureNotify) {
                resize_terminal(ev.xconfigure.width, ev.xconfigure.height);
            } else if (ev.type == Expose) {
                dirty_all = 1;
            } else if (ev.type == ClientMessage) {
                if ((Atom)ev.xclient.data.l[0] == wm_delete) {
                    running = 0;
                }
            }
        }
    }

    destroy_framebuffer();
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    close(pty_master);
    if (child_alive) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }

    for (int i = 0; i < MAX_HIST_LINES; i++) {
        if (history[i]) free(history[i]);
    }
    for (int i = 0; i < MAX_FLIGHT_CHUNKS; i++) {
        if (flight_log[i].data) free(flight_log[i].data);
    }
    free(live_term.primary_grid);
    free(live_term.alt_grid);
    free(replay_term.primary_grid);
    free(replay_term.alt_grid);
    free(dirty);
    free(sel_text);
    clear_glyph_cache();
    for (int i = 0; i < num_fallback_faces; i++) {
        if (fallback_faces[i]) {
            FT_Done_Face(fallback_faces[i]);
            fallback_faces[i] = NULL;
        }
    }
    num_fallback_faces = 0;
    FT_Done_Face(ft_face);
    FT_Done_FreeType(ft_lib);

    return 0;
}
