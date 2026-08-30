#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <psprtc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <zlib.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdarg.h>

#include "core.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#define SCREEN_WIDTH   480
#define SCREEN_HEIGHT  272
#define BUFFER_WIDTH   512
#define BUFFER_SIZE    (BUFFER_WIDTH * SCREEN_HEIGHT * 4)

#define VRAM_CACHED_0  ((void *)0x04000000)
#define VRAM_CACHED_1  ((void *)(0x04000000 + BUFFER_SIZE))
#define VRAM_UNCACHED_0 ((void *)0x44000000)
#define VRAM_UNCACHED_1 ((void *)(0x44000000 + BUFFER_SIZE))

#define MAX_SERIES     32
#define MAX_CHAPTERS   128
#define MAX_PAGES      1024
#define MAX_PATH_LEN   512
#define MAX_NAME_LEN   128
#define MAX_BOOKMARKS_SIZE (32 * 1024)
#define MAX_PAGE_DATA_SIZE (16 * 1024 * 1024)
#define PAGE_DECODE_TARGET_WIDTH 1200
#define PAGE_DECODE_MAX_BYTES (12 * 1024 * 1024)

#define APP_NAME "TANKOBON"

/* Covers are cached at THUMB_* and box-filtered down to the grid cell size, so
   changing the grid layout never invalidates the on-disk cache. */
#define THUMB_WIDTH    128
#define THUMB_HEIGHT   182

#define GRID_COLS      5
#define GRID_ROWS      2
#define GRID_THUMB_W   68
#define GRID_THUMB_H   96
#define GRID_GAP_X     25
#define GRID_LABEL_H   10
#define GRID_GAP_Y     12
#define GRID_START_X   20
#define GRID_START_Y   20
#define GRID_ROW_PITCH (GRID_THUMB_H + GRID_LABEL_H + GRID_GAP_Y)

#define LIST_ROW_H     16
#define LIST_TOP       24
#define LIST_VISIBLE   14
#define COVER_LIST_ROW_H   38
#define COVER_LIST_VISIBLE 6
#define COVER_LIST_THUMB_W 22
#define COVER_LIST_THUMB_H 32

#define STATUSBAR_H    16
#define HINTBAR_H      16
#define HINTBAR_Y      (SCREEN_HEIGHT - HINTBAR_H)

/* Palette: eight ramp entries, reused across both themes. The framebuffer is
   ABGR8888, so these read backwards from the RGB hex they came from. */
#define RAMP_INK    0xFF1C1C1CU  /* #1C1C1C */
#define RAMP_COAL   0xFF2C2C2CU  /* #2C2C2C */
#define RAMP_SLATE  0xFF4A4A4AU  /* #4A4A4A */
#define RAMP_ASH    0xFF8A8A8AU  /* #8A8A8A */
#define RAMP_MIST   0xFFD7D7D6U  /* #D6D7D7 */
#define RAMP_PAPER  0xFFF4F4F3U  /* #F3F4F4 */
#define RAMP_PLUM   0xFF532D61U  /* #612D53 */
#define RAMP_WINE   0xFF533985U  /* #853953 */

/* Colours are looked up at draw time so the theme can change without a
   restart. The COL_* names keep every call site theme-agnostic. */
typedef struct {
    unsigned int bg;
    unsigned int panel;
    unsigned int panel_hi;
    unsigned int sel_bg;      /* selected row on the open field */
    unsigned int sel_panel;   /* selected row inside a panel */
    unsigned int fg;
    unsigned int fg_dim;
    unsigned int fg_muted;
    unsigned int line;
    unsigned int accent;
    unsigned int warn;
    int scrim;          /* 0..256, how hard a modal darkens what is behind it */
} Palette;

extern Palette theme;

#define COL_BG         theme.bg
#define COL_PANEL      theme.panel
#define COL_PANEL_HI   theme.panel_hi
#define COL_SEL_BG     theme.sel_bg
#define COL_SEL_PANEL  theme.sel_panel
#define COL_FG         theme.fg
#define COL_FG_DIM     theme.fg_dim
#define COL_FG_MUTED   theme.fg_muted
#define COL_LINE       theme.line
#define COL_ACCENT     theme.accent
#define COL_WARN       theme.warn

/* Motion. The main loop is locked to vblank, so one unit is one 60 Hz frame.
   Tweens are exponential ease-out: rate/8 of the remaining distance per frame. */
#define TWEEN_RATE_CURSOR  3
#define TWEEN_RATE_MODAL   4
#define TWEEN_RATE_PAGE    4
#define HUD_HOLD_FRAMES    70
#define HUD_FADE_FRAMES    20
#define SPINNER_X          (SCREEN_WIDTH - 22)
#define SPINNER_Y          (SCREEN_HEIGHT - 26)
#define LOAD_CHUNK_BYTES   (128 * 1024)

#define SCROLL_MAX_VEL_Q8  3600
#define SCROLL_FRICTION_N  7
#define SCROLL_FRICTION_D  8

/* Menu D-pad repeat timing, in 60 Hz frames. */
#define MENU_REPEAT_DELAY_FRAMES  18
#define MENU_REPEAT_SLOW_FRAMES    5
#define MENU_REPEAT_FAST_AFTER    60
#define MENU_REPEAT_FAST_FRAMES    2

PSP_MODULE_INFO("tankobon", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#ifdef DEBUG_BOOT
/* Boot tracing. Every stage is appended to ms0:/tankobon_boot.log and closed
   immediately, so the last line survives a freeze or a crash. */
#define BOOT_LOG_PATH "ms0:/tankobon_boot.log"

static void boot_log(const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (len < 0) return;
    if (len > (int)sizeof(line) - 2) len = (int)sizeof(line) - 2;
    line[len++] = '\n';
    line[len] = '\0';

    SceUID fd = sceIoOpen(BOOT_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, len);
        sceIoClose(fd);
    }
}

static void boot_log_reset(void) {
    SceUID fd = sceIoOpen(BOOT_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) sceIoClose(fd);
}

/* Paints the visible framebuffer directly so "did we reach main()" is
   answerable on the PSP itself, without pulling the memory stick. */
static void boot_marker(unsigned int color) {
    unsigned int *fb = (unsigned int *)VRAM_UNCACHED_0;
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) fb[y * BUFFER_WIDTH + x] = color;
    }
}

#ifdef DEBUG_DISPLAY_PROBE
#include <pspgu.h>

static unsigned int probe_gu_list[16384] __attribute__((aligned(16)));

static void probe_fill(void *base, unsigned int color) {
    unsigned int *fb = (unsigned int *)base;
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) fb[y * BUFFER_WIDTH + x] = color;
    }
}

static void probe_hold(int seconds) {
    for (int i = 0; i < seconds * 60; i++) sceDisplayWaitVblankStart();
}

static void probe_report(const char *tag) {
    void *top = NULL;
    int bw = -1, pf = -1;
    int rc = sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_IMMEDIATE);
    boot_log("%s readback rc=0x%08X top=%p bw=%d pf=%d fg=%d vcount=%u",
             tag, rc, top, bw, pf,
             sceDisplayIsForeground(), sceDisplayGetVcount());
}

/* Four ways of getting a colour on screen, three seconds each.
   Whichever colour actually appears identifies the working path. */
static void display_probe(void) {
    int rc, mode = -1, w = -1, h = -1;

    rc = sceDisplaySetMode(0, SCREEN_WIDTH, SCREEN_HEIGHT);
    boot_log("probe: SetMode -> 0x%08X", rc);
    rc = sceDisplayGetMode(&mode, &w, &h);
    boot_log("probe: GetMode -> 0x%08X mode=%d %dx%d", rc, mode, w, h);

    /* 1 - RED: exactly what the app does today. */
    probe_fill(VRAM_UNCACHED_0, 0xFF0000FF);
    rc = sceDisplaySetFrameBuf(VRAM_UNCACHED_0, BUFFER_WIDTH,
                               PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    boot_log("probe1 RED  uncached/IMMEDIATE -> 0x%08X", rc);
    probe_report("probe1");
    probe_hold(3);

    /* 2 - GREEN: same, but latched at the next vblank instead. */
    probe_fill(VRAM_UNCACHED_0, 0xFF00FF00);
    rc = sceDisplaySetFrameBuf(VRAM_UNCACHED_0, BUFFER_WIDTH,
                               PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    boot_log("probe2 GREEN uncached/NEXTFRAME -> 0x%08X", rc);
    probe_report("probe2");
    probe_hold(3);

    /* 3 - BLUE: bring the GE up first, which is what turns the display on
       for most homebrew. The app never does this. */
    sceGuInit();
    sceGuStart(GU_DIRECT, probe_gu_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUFFER_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)0, BUFFER_WIDTH);
    sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    boot_log("probe3 sceGuInit + sceGuDisplay(GU_TRUE) done");

    probe_fill(VRAM_UNCACHED_0, 0xFFFF0000);
    rc = sceDisplaySetFrameBuf(VRAM_UNCACHED_0, BUFFER_WIDTH,
                               PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    boot_log("probe3 BLUE  after GU init -> 0x%08X", rc);
    probe_report("probe3");
    probe_hold(3);

    /* 4 - WHITE: cached write + writeback, the path flip_display uses. */
    probe_fill(VRAM_CACHED_0, 0xFFFFFFFF);
    sceKernelDcacheWritebackRange(VRAM_CACHED_0, BUFFER_SIZE);
    rc = sceDisplaySetFrameBuf(VRAM_UNCACHED_0, BUFFER_WIDTH,
                               PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    boot_log("probe4 WHITE cached+writeback -> 0x%08X", rc);
    probe_report("probe4");
    probe_hold(3);
}
#else
#define display_probe() ((void)0)
#endif

#else
#define boot_log(...)       ((void)0)
#define boot_log_reset()    ((void)0)
#define boot_marker(color)  ((void)0)
#define display_probe()     ((void)0)
#endif

typedef enum {
    STATE_LIBRARY,
    STATE_SERIES_DETAIL,
    STATE_READER
} AppState;

typedef enum {
    LIB_VIEW_GRID = 0,
    LIB_VIEW_COVER_LIST,
    LIB_VIEW_DENSE
} LibraryViewMode;

typedef enum {
    VIEW_FIT_WIDTH = 0,
    VIEW_FIT_SCREEN,
    VIEW_ROTATE_90
} ViewMode;

typedef enum {
    BG_AUTO = 0,
    BG_FORCE_BLACK,
    BG_FORCE_WHITE
} BgMode;

typedef enum {
    READ_MANGA_RTL = 0,
    READ_WESTERN_LTR
} ReadDirection;

typedef struct {
    unsigned char *data;
    int width;
    int height;
    int channels;
    int crop_x0;
    int crop_y0;
    int crop_x1;
    int crop_y1;
} TextureImage;

typedef struct {
    char name[MAX_NAME_LEN];
    unsigned int local_header_offset;
    unsigned int compressed_size;
    unsigned int uncompressed_size;
    unsigned short compression_method;
} PageEntry;

typedef struct {
    char name[MAX_NAME_LEN];
    char folder_path[MAX_PATH_LEN];
    char cover_path[MAX_PATH_LEN];
    char chapter_files[MAX_CHAPTERS][MAX_NAME_LEN];
    int chapter_count;
    int has_progress;
    int progress_chapter_index;
    int progress_page_index;
    int progress_total_pages;
    TextureImage cover_thumb;
} MangaSeries;

typedef struct {
    PageEntry pages[MAX_PAGES];
    int total_pages;
    int current_page_index;
    char full_path[MAX_PATH_LEN];
    char current_file[MAX_NAME_LEN];
    unsigned int archive_size;
} ComicBook;

typedef struct {
    int theme_index;
    ViewMode view_mode;
    BgMode bg_mode;
    ReadDirection read_direction;
    int auto_crop;
    int page_transitions;
} Settings;

MangaSeries library[MAX_SERIES];
int series_count = 0;
ComicBook current_comic;
TextureImage current_image = { NULL, 0, 0, 0, 0, 0, 0, 0 };

char base_dir[MAX_PATH_LEN] = {0};
char mangas_dir[MAX_PATH_LEN] = {0};
char cache_dir[MAX_PATH_LEN] = {0};
char bookmarks_path[MAX_PATH_LEN] = {0};

AppState state = STATE_LIBRARY;
LibraryViewMode lib_view = LIB_VIEW_GRID;

Settings config = {
    .theme_index = 0,
    .view_mode = VIEW_FIT_WIDTH,
    .bg_mode = BG_AUTO,
    .read_direction = READ_MANGA_RTL,
    .auto_crop = 0,
    .page_transitions = 1
};

int lib_selected_index = 0;
int chapter_selected_index = 0;
int scroll_y = 0;
int current_buffer = 0;

int menu_held_frames[4] = { 0, 0, 0, 0 };

int settings_cursor = 0;
int hud_display_frames = 0;
int page_load_error = 0;

int is_magnified = 0;
int mag_offset_x = 0;
int mag_offset_y = 0;

unsigned int detected_bg_color = 0xFF000000;
char last_read_path[MAX_PATH_LEN] = {0};
char config_path[MAX_PATH_LEN] = {0};

Palette theme;

typedef struct {
    const char *name;
    int is_light;
    unsigned int bg;
    unsigned int fg;
    unsigned int selection;
    unsigned int accent;
    unsigned int warn;
} ThemePreset;

/* Convert conventional RRGGBB literals to the PSP's ABGR8888 framebuffer. */
#define RGB24(c) (0xFF000000U | (((c) & 0x0000FFU) << 16) | ((c) & 0x00FF00U) | (((c) & 0xFF0000U) >> 16))

static const ThemePreset theme_presets[] = {
    { "Dracula", 0, RGB24(0x282A36), RGB24(0xF8F8F2), RGB24(0x44475A), RGB24(0xBD93F9), RGB24(0xFF5555) },
    { "Night Owl", 0, RGB24(0x011627), RGB24(0xD6DEEB), RGB24(0x5F7E97), RGB24(0x82AAFF), RGB24(0xEF5350) },
    { "TokyoNight Night", 0, RGB24(0x1A1B26), RGB24(0xC0CAF5), RGB24(0x283457), RGB24(0x7AA2F7), RGB24(0xF7768E) },
    { "Gruvbox Dark", 0, RGB24(0x282828), RGB24(0xEBDBB2), RGB24(0x665C54), RGB24(0x458588), RGB24(0xCC241D) },
    { "Nord", 0, RGB24(0x2E3440), RGB24(0xD8DEE9), RGB24(0xECEFF4), RGB24(0x81A1C1), RGB24(0xBF616A) },
    { "Catppuccin Macchiato", 0, RGB24(0x24273A), RGB24(0xCAD3F5), RGB24(0x5B6078), RGB24(0x8AADF4), RGB24(0xED8796) },
    { "Atom One Dark", 0, RGB24(0x21252B), RGB24(0xABB2BF), RGB24(0x323844), RGB24(0x61AFEF), RGB24(0xE06C75) },
    { "Catppuccin Mocha", 0, RGB24(0x1E1E2E), RGB24(0xCDD6F4), RGB24(0xF5E0DC), RGB24(0x89B4FA), RGB24(0xF38BA8) },
    { "Rose Pine", 0, RGB24(0x191724), RGB24(0xE0DEF4), RGB24(0x403D52), RGB24(0x9CCFD8), RGB24(0xEB6F92) },
    { "Catppuccin Frappe", 0, RGB24(0x303446), RGB24(0xC6D0F5), RGB24(0x626880), RGB24(0x8CAAEE), RGB24(0xE78284) },
    { "Lovelace", 0, RGB24(0x1D1F28), RGB24(0xFDFDFD), RGB24(0xC1DEFF), RGB24(0x8897F4), RGB24(0xF37F97) },
    { "Atom One Light", 1, RGB24(0xF9F9F9), RGB24(0x2A2C33), RGB24(0xEDEDED), RGB24(0x2F5AF3), RGB24(0xDE3E35) },
    { "Ayu Light", 1, RGB24(0xF8F9FA), RGB24(0x5C6166), RGB24(0x035BD6), RGB24(0x3199E1), RGB24(0xEA6C6D) },
    { "Catppuccin Latte", 1, RGB24(0xEFF1F5), RGB24(0x4C4F69), RGB24(0xDC8A78), RGB24(0x1E66F5), RGB24(0xD20F39) },
    { "Everforest Light Med", 1, RGB24(0xEFEBD4), RGB24(0x5C6A72), RGB24(0xEAEDC8), RGB24(0x7FBBB3), RGB24(0xE67E80) },
    { "GitHub Light Default", 1, RGB24(0xFFFFFF), RGB24(0x1F2328), RGB24(0x1F2328), RGB24(0x0969DA), RGB24(0xCF222E) },
    { "Gruvbox Light", 1, RGB24(0xFBF1C7), RGB24(0x3C3836), RGB24(0x3C3836), RGB24(0x458588), RGB24(0xCC241D) },
    { "Night Owlish Light", 1, RGB24(0xFFFFFF), RGB24(0x403F53), RGB24(0xF2F2F2), RGB24(0x4876D6), RGB24(0xD3423E) },
    { "Rose Pine Dawn", 1, RGB24(0xFAF4ED), RGB24(0x575279), RGB24(0xDFDAD9), RGB24(0x56949F), RGB24(0xB4637A) },
    { "TokyoNight Day", 1, RGB24(0xE1E2E7), RGB24(0x3760BF), RGB24(0x99A7DF), RGB24(0x2E7DE9), RGB24(0xF52A65) },
    { "Alucard", 1, RGB24(0xFFFBEB), RGB24(0x1F1F1F), RGB24(0xCFCFDE), RGB24(0x036A96), RGB24(0xCB3A2A) }
};

#define THEME_COUNT ((int)(sizeof(theme_presets) / sizeof(theme_presets[0])))
static unsigned int theme_mix(unsigned int a, unsigned int b, int amount) {
    unsigned int ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    unsigned int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
    unsigned int r = (ar * (256 - amount) + br * amount) >> 8;
    unsigned int g = (ag * (256 - amount) + bg * amount) >> 8;
    unsigned int bl = (ab * (256 - amount) + bb * amount) >> 8;
    return 0xFF000000U | (bl << 16) | (g << 8) | r;
}

static void theme_apply(void) {
    const ThemePreset *p;
    if (config.theme_index < 0 || config.theme_index >= THEME_COUNT) config.theme_index = 0;
    p = &theme_presets[config.theme_index];
    theme.bg = p->bg;
    theme.panel = theme_mix(p->bg, p->fg, p->is_light ? 14 : 22);
    theme.panel_hi = theme_mix(p->bg, p->fg, p->is_light ? 25 : 38);
    theme.sel_bg = theme_mix(p->bg, p->selection, p->is_light ? 48 : 72);
    theme.sel_panel = theme_mix(theme.panel, p->selection, p->is_light ? 48 : 72);
    theme.fg = p->fg;
    theme.fg_dim = theme_mix(p->bg, p->fg, 196);
    theme.fg_muted = theme_mix(p->bg, p->fg, 132);
    theme.line = theme_mix(p->bg, p->fg, 68);
    theme.accent = p->accent;
    theme.warn = p->warn;
    theme.scrim = p->is_light ? 80 : 150;
}

/* ------------------------------------------------------------------ motion */

/* Interruptible exponential ease-out. Positions are 24.8 fixed point so a
   tween can be retargeted mid-flight without a restart or a duration reset. */
typedef struct {
    int cur_q8;
    int target;
    int rate;    /* eighths of the remaining distance consumed per frame */
    int active;
} Tween;

static void tween_init(Tween *t, int value, int rate) {
    t->cur_q8 = value << 8;
    t->target = value;
    t->rate = rate;
    t->active = 0;
}

static void tween_set(Tween *t, int target) {
    if (t->target != target) {
        t->target = target;
        t->active = 1;
    }
}

static void tween_snap(Tween *t, int value) {
    t->cur_q8 = value << 8;
    t->target = value;
    t->active = 0;
}

/* Returns 1 while the tween still needs frames. */
static int tween_step(Tween *t) {
    int delta;
    if (!t->active) return 0;
    delta = (t->target << 8) - t->cur_q8;
    if (delta > -256 && delta < 256) {
        t->cur_q8 = t->target << 8;
        t->active = 0;
        return 0;
    }
    t->cur_q8 += (delta * t->rate) / 8;
    return 1;
}

static int tween_value(const Tween *t) {
    return t->cur_q8 >> 8;
}

typedef enum {
    MODAL_NONE = 0,
    MODAL_HELP,
    MODAL_SETTINGS,
    MODAL_THEME_PICKER
} ModalKind;

#define SETTINGS_ROWS 6

ModalKind modal_kind = MODAL_NONE;
int modal_open = 0;
Tween modal_progress;        /* 0..256 */

Tween lib_cursor_y;
Tween chapter_cursor_y;
Tween page_slide;

int lib_list_offset = 0;
int lib_grid_row_offset = 0;
int chapter_list_offset = 0;

int scroll_y_q8 = 0;
int scroll_vel_q8 = 0;
int loading_active = 0;
int loading_phase = 0;

/* Theme picker: filter 0=all, 1=dark, 2=light. Search is entered with a
   controller-friendly on-screen A-Z keyboard, so it also works on PSPs with
   no external keyboard. */
int theme_filter = 0;
int theme_picker_row = 0;
int theme_search_active = 0;
int theme_keyboard_cursor = 0;
char theme_search[17] = {0};

static int contains_case_insensitive(const char *text, const char *needle) {
    int needle_len = strlen(needle);
    if (needle_len == 0) return 1;
    for (int i = 0; text[i]; i++) {
        int j = 0;
        while (j < needle_len && text[i + j] &&
               (text[i + j] | 32) == (needle[j] | 32)) j++;
        if (j == needle_len) return 1;
    }
    return 0;
}

static int theme_is_visible(int index) {
    if (theme_filter == 1 && theme_presets[index].is_light) return 0;
    if (theme_filter == 2 && !theme_presets[index].is_light) return 0;
    return contains_case_insensitive(theme_presets[index].name, theme_search);
}

static int theme_filtered_count(void) {
    int count = 0;
    for (int i = 0; i < THEME_COUNT; i++) if (theme_is_visible(i)) count++;
    return count;
}

static int theme_filtered_index(int row) {
    for (int i = 0; i < THEME_COUNT; i++) {
        if (!theme_is_visible(i)) continue;
        if (row-- == 0) return i;
    }
    return -1;
}

static void motion_init(void) {
    tween_init(&modal_progress, 0, TWEEN_RATE_MODAL);
    tween_init(&lib_cursor_y, LIST_TOP, TWEEN_RATE_CURSOR);
    tween_init(&chapter_cursor_y, LIST_TOP, TWEEN_RATE_CURSOR);
    tween_init(&page_slide, 0, TWEEN_RATE_PAGE);
}

/* Keeps `selected` inside a scrolling window with `margin` rows of lookahead,
   instead of jumping a whole page when the cursor crosses a page boundary. */
static int clamp_window(int selected, int count, int visible, int margin, int offset) {
    if (count <= visible) return 0;
    if (selected - margin < offset) offset = selected - margin;
    if (selected + margin > offset + visible - 1) offset = selected + margin - visible + 1;
    if (offset > count - visible) offset = count - visible;
    if (offset < 0) offset = 0;
    return offset;
}

static void modal_show(ModalKind kind) {
    modal_kind = kind;
    modal_open = 1;
    tween_snap(&modal_progress, 256);
}

static void modal_hide(void) {
    modal_open = 0;
    modal_kind = MODAL_NONE;
    tween_snap(&modal_progress, 0);
}

void flip_display(void (*render_func)(unsigned int *));
void present_frame(void (*render_func)(unsigned int *), int wait_vblank);
void render_reader_screen(unsigned int *vram);
void render_current_screen(unsigned int *vram);
void save_config(void);
int page_surface_build(void);
void page_surface_sync(void);
void page_surface_free(void);
void page_view_extent(int *max_x, int *max_y);

static const unsigned char font8x8_modern[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    {0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00}, {0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00,0x00},
    {0x18,0x7E,0xC0,0x7C,0x06,0x7E,0x18,0x00}, {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x7C,0x06,0x06,0x3C,0x60,0x60,0x7E,0x00}, {0x7E,0x06,0x1C,0x06,0x06,0x66,0x3C,0x00},
    {0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x00}, {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, {0x7E,0x06,0x0C,0x18,0x18,0x18,0x18,0x00},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, {0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00},
    {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00}, {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00},
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3A,0x00},
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06,0x00},
    {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}, {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00},
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3B,0x00},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    {0x0E,0x18,0x7E,0x18,0x18,0x18,0x18,0x00}, {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x3C},
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x3C,0x00}, {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x07},
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00}, {0x00,0x00,0x66,0x66,0x66,0x66,0x3B,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, {0x00,0x00,0x63,0x6B,0x7F,0x3E,0x36,0x00},
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}
};

static inline unsigned int read_uint32_safe(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static inline unsigned short read_uint16_safe(const unsigned char *p) {
    return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}

/* ----------------------------------------------------------------- drawing */

/* alpha is 0..256. The framebuffer is opaque, so the result is always A=FF. */
static inline unsigned int blend_abgr(unsigned int dst, unsigned int src, int alpha) {
    unsigned int inv = (unsigned int)(256 - alpha);
    unsigned int a = (unsigned int)alpha;
    unsigned int r = (((src      ) & 0xFF) * a + ((dst      ) & 0xFF) * inv) >> 8;
    unsigned int g = (((src >>  8) & 0xFF) * a + ((dst >>  8) & 0xFF) * inv) >> 8;
    unsigned int b = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * inv) >> 8;
    return 0xFF000000U | (b << 16) | (g << 8) | r;
}

int text_width(const char *str) {
    return (int)strlen(str) * 8;
}

void draw_char(unsigned int *vram, int x, int y, char c, unsigned int color) {
    if (c < 32 || c > 126) c = '?';
    const unsigned char *glyph = font8x8_modern[c - 32];

    for (int cy = 0; cy < 8; cy++) {
        int py = y + cy;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned char row = glyph[cy];
        unsigned int *dst = vram + py * BUFFER_WIDTH;

        for (int cx = 0; cx < 8; cx++) {
            int px = x + cx;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            if (row & (0x80 >> cx)) {
                dst[px] = color;
            }
        }
    }
}

void draw_char_blend(unsigned int *vram, int x, int y, char c, unsigned int color, int alpha) {
    if (alpha >= 256) { draw_char(vram, x, y, c, color); return; }
    if (alpha <= 0) return;
    if (c < 32 || c > 126) c = '?';
    const unsigned char *glyph = font8x8_modern[c - 32];

    for (int cy = 0; cy < 8; cy++) {
        int py = y + cy;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned char row = glyph[cy];
        unsigned int *dst = vram + py * BUFFER_WIDTH;

        for (int cx = 0; cx < 8; cx++) {
            int px = x + cx;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            if (row & (0x80 >> cx)) {
                dst[px] = blend_abgr(dst[px], color, alpha);
            }
        }
    }
}

/* The same 8x8 bitmap doubled, so headings scale without a second font. */
void draw_char_2x(unsigned int *vram, int x, int y, char c, unsigned int color) {
    if (c < 32 || c > 126) c = '?';
    const unsigned char *glyph = font8x8_modern[c - 32];

    for (int cy = 0; cy < 16; cy++) {
        int py = y + cy;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned char row = glyph[cy >> 1];
        unsigned int *dst = vram + py * BUFFER_WIDTH;

        for (int cx = 0; cx < 16; cx++) {
            int px = x + cx;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            if (row & (0x80 >> (cx >> 1))) {
                dst[px] = color;
            }
        }
    }
}

void draw_text(unsigned int *vram, int x, int y, const char *str, unsigned int color) {
    int cur_x = x;
    while (*str) {
        draw_char(vram, cur_x, y, *str, color);
        cur_x += 8;
        str++;
    }
}

void draw_text_blend(unsigned int *vram, int x, int y, const char *str, unsigned int color, int alpha) {
    int cur_x = x;
    while (*str) {
        draw_char_blend(vram, cur_x, y, *str, color, alpha);
        cur_x += 8;
        str++;
    }
}

void draw_text_2x(unsigned int *vram, int x, int y, const char *str, unsigned int color) {
    int cur_x = x;
    while (*str) {
        draw_char_2x(vram, cur_x, y, *str, color);
        cur_x += 16;
        str++;
    }
}

/* Truncates with a trailing ".." rather than letting the string run off. */
void draw_text_clipped(unsigned int *vram, int x, int y, const char *str, unsigned int color, int max_w) {
    int fits = max_w / 8;
    int len = (int)strlen(str);
    int cur_x = x;
    int keep;

    if (fits <= 0) return;
    if (len <= fits) { draw_text(vram, x, y, str, color); return; }
    if (fits <= 2) return;

    keep = fits - 2;
    for (int i = 0; i < keep; i++) {
        draw_char(vram, cur_x, y, str[i], color);
        cur_x += 8;
    }
    draw_char(vram, cur_x, y, '.', color);
    draw_char(vram, cur_x + 8, y, '.', color);
}

void draw_rect_solid(unsigned int *vram, int x, int y, int w, int h, unsigned int color) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned int *dst = vram + py * BUFFER_WIDTH;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            dst[px] = color;
        }
    }
}

void draw_rect_blend(unsigned int *vram, int x, int y, int w, int h, unsigned int color, int alpha) {
    if (alpha >= 256) { draw_rect_solid(vram, x, y, w, h, color); return; }
    if (alpha <= 0) return;

    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;
        unsigned int *dst = vram + py * BUFFER_WIDTH;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= SCREEN_WIDTH) continue;
            dst[px] = blend_abgr(dst[px], color, alpha);
        }
    }
}

/* amount is 0..256: how far the region is pushed towards black. */
void draw_rect_dimmed(unsigned int *vram, int x, int y, int w, int h, int amount) {
    draw_rect_blend(vram, x, y, w, h, 0xFF000000U, amount);
}

void draw_frame(unsigned int *vram, int x, int y, int w, int h, unsigned int color) {
    draw_rect_solid(vram, x, y, w, 1, color);
    draw_rect_solid(vram, x, y + h - 1, w, 1, color);
    draw_rect_solid(vram, x, y, 1, h, color);
    draw_rect_solid(vram, x + w - 1, y, 1, h, color);
}

/* Panel with a soft drop shadow, so modals sit above the page instead of
   being pasted onto it. */
void draw_panel(unsigned int *vram, int x, int y, int w, int h) {
    draw_rect_blend(vram, x + 4, y + 5, w, h, 0xFF000000U, 110);
    draw_rect_blend(vram, x + 2, y + 3, w, h, 0xFF000000U, 110);
    draw_rect_solid(vram, x, y, w, h, COL_PANEL);
    draw_frame(vram, x, y, w, h, COL_LINE);
    draw_rect_solid(vram, x + 1, y + 1, w - 2, 1, COL_PANEL_HI);
}

/* Selected row: flat tint plus an accent bar on the leading edge. Replaces the
   old gradient, which wrote an alpha byte the opaque framebuffer ignored and
   faded to pure black at the edges. */
void draw_selection_row(unsigned int *vram, int x, int y, int w, int h) {
    draw_rect_solid(vram, x, y, w, h, COL_SEL_BG);
    draw_rect_solid(vram, x, y, 2, h, COL_ACCENT);
}

void draw_ps_glyph(unsigned int *vram, int x, int y, char type, unsigned int color) {
    if (type == 'X') {
        for (int i = 0; i < 7; i++) {
            vram[(y + i) * BUFFER_WIDTH + (x + i)] = color;
            vram[(y + i) * BUFFER_WIDTH + (x + 6 - i)] = color;
        }
    } else if (type == 'O') {
        draw_rect_solid(vram, x + 2, y, 4, 1, color);
        draw_rect_solid(vram, x + 2, y + 7, 4, 1, color);
        draw_rect_solid(vram, x, y + 2, 1, 4, color);
        draw_rect_solid(vram, x + 7, y + 2, 1, 4, color);
        vram[(y + 1) * BUFFER_WIDTH + (x + 1)] = color;
        vram[(y + 1) * BUFFER_WIDTH + (x + 6)] = color;
        vram[(y + 6) * BUFFER_WIDTH + (x + 1)] = color;
        vram[(y + 6) * BUFFER_WIDTH + (x + 6)] = color;
    } else if (type == 'S') {
        draw_rect_solid(vram, x, y, 8, 1, color);
        draw_rect_solid(vram, x, y + 7, 8, 1, color);
        draw_rect_solid(vram, x, y, 1, 8, color);
        draw_rect_solid(vram, x + 7, y, 1, 8, color);
    } else if (type == 'T') {
        vram[y * BUFFER_WIDTH + (x + 3)] = color;
        vram[y * BUFFER_WIDTH + (x + 4)] = color;
        vram[(y + 1) * BUFFER_WIDTH + (x + 2)] = color;
        vram[(y + 1) * BUFFER_WIDTH + (x + 5)] = color;
        vram[(y + 2) * BUFFER_WIDTH + (x + 2)] = color;
        vram[(y + 2) * BUFFER_WIDTH + (x + 5)] = color;
        vram[(y + 3) * BUFFER_WIDTH + (x + 1)] = color;
        vram[(y + 3) * BUFFER_WIDTH + (x + 6)] = color;
        vram[(y + 4) * BUFFER_WIDTH + (x + 1)] = color;
        vram[(y + 4) * BUFFER_WIDTH + (x + 6)] = color;
        vram[(y + 5) * BUFFER_WIDTH + x] = color;
        vram[(y + 5) * BUFFER_WIDTH + (x + 7)] = color;
        draw_rect_solid(vram, x, y + 6, 8, 1, color);
    } else if (type == 'U') {
        vram[y * BUFFER_WIDTH + (x + 3)] = color;
        draw_rect_solid(vram, x + 2, y + 1, 3, 1, color);
        draw_rect_solid(vram, x + 1, y + 2, 5, 1, color);
        draw_rect_solid(vram, x + 2, y + 3, 3, 4, color);
    } else if (type == 'D') {
        draw_rect_solid(vram, x + 2, y, 3, 4, color);
        draw_rect_solid(vram, x + 1, y + 4, 5, 1, color);
        draw_rect_solid(vram, x + 2, y + 5, 3, 1, color);
        vram[(y + 6) * BUFFER_WIDTH + (x + 3)] = color;
    } else if (type == 'L') {
        vram[(y + 3) * BUFFER_WIDTH + x] = color;
        draw_rect_solid(vram, x + 1, y + 2, 1, 3, color);
        draw_rect_solid(vram, x + 2, y + 1, 1, 5, color);
        draw_rect_solid(vram, x + 3, y + 2, 4, 3, color);
    } else if (type == 'R') {
        draw_rect_solid(vram, x, y + 2, 4, 3, color);
        draw_rect_solid(vram, x + 4, y + 1, 1, 5, color);
        draw_rect_solid(vram, x + 5, y + 2, 1, 3, color);
        vram[(y + 3) * BUFFER_WIDTH + (x + 6)] = color;
    }
}

static void draw_info_icon_blend(unsigned int *vram, int x, int y, int alpha) {
    draw_rect_blend(vram, x + 3, y, 6, 1, COL_ACCENT, alpha);
    draw_rect_blend(vram, x + 3, y + 11, 6, 1, COL_ACCENT, alpha);
    draw_rect_blend(vram, x, y + 3, 1, 6, COL_ACCENT, alpha);
    draw_rect_blend(vram, x + 11, y + 3, 1, 6, COL_ACCENT, alpha);
    draw_rect_blend(vram, x + 1, y + 1, 2, 2, COL_ACCENT, alpha);
    draw_rect_blend(vram, x + 9, y + 1, 2, 2, COL_ACCENT, alpha);
    draw_rect_blend(vram, x + 1, y + 9, 2, 2, COL_ACCENT, alpha);
    draw_rect_blend(vram, x + 9, y + 9, 2, 2, COL_ACCENT, alpha);
    draw_rect_blend(vram, x + 5, y + 3, 2, 2, COL_ACCENT, alpha);
    draw_rect_blend(vram, x + 5, y + 6, 2, 4, COL_ACCENT, alpha);
}

/* Busy dial: eight segments with a fading tail, bottom-right like the PSP's
   own. Deliberately theme-independent -- it sits over arbitrary page art, where
   only white-on-a-dark-halo reads reliably on both black and white scans. */
void draw_busy_spinner(unsigned int *vram, int cx, int cy, int phase) {
    static const signed char offset[8][2] = {
        {  0, -9 }, {  6, -6 }, {  9,  0 }, {  6,  6 },
        {  0,  9 }, { -6,  6 }, { -9,  0 }, { -6, -6 }
    };
    /* The tail dims but never goes out, so the ring stays a ring. */
    static const short trail[8] = { 256, 205, 168, 138, 114, 96, 84, 76 };

    /* Halos first, at constant strength: a halo that faded with its dot left
       the tail invisible on a white scan, and the dial read as loose specks. */
    for (int i = 0; i < 8; i++) {
        draw_rect_blend(vram, cx + offset[i][0] - 3, cy + offset[i][1] - 3, 6, 6,
                        0xFF000000U, 150);
    }

    for (int i = 0; i < 8; i++) {
        draw_rect_blend(vram, cx + offset[i][0] - 2, cy + offset[i][1] - 2, 4, 4,
                        0xFFFFFFFFU, trail[(phase - i) & 7]);
    }
}

/* Hint bar items flow left to right from a running cursor, so editing a label
   no longer means re-deriving every x position by hand. */
typedef struct {
    unsigned int *vram;
    int x;
    int y;
} HintBar;

static void hint_bar_begin(HintBar *bar, unsigned int *vram, int y) {
    bar->vram = vram;
    bar->x = 8;
    bar->y = y;
}

/* glyphs: a string of draw_ps_glyph types, or NULL to use the `key` text. */
static void hint_bar_item(HintBar *bar, const char *glyphs, const char *key, const char *label) {
    if (glyphs) {
        for (const char *g = glyphs; *g; g++) {
            draw_ps_glyph(bar->vram, bar->x, bar->y, *g, COL_FG);
            bar->x += 9;
        }
        bar->x += 3;
    } else if (key) {
        draw_text(bar->vram, bar->x, bar->y, key, COL_ACCENT);
        bar->x += text_width(key) + 5;
    }
    draw_text(bar->vram, bar->x, bar->y, label, COL_FG_DIM);
    bar->x += text_width(label) + 14;
}

/* Average of the source box that maps to one destination pixel. Sampling is
   capped at 4x4 so the cost stays bounded for large downscales. */
static unsigned int sample_box(const unsigned int *src, int src_stride,
                               int sx0, int sy0, int sx1, int sy1) {
    unsigned int r = 0, g = 0, b = 0;
    int n = 0;
    int step_x, step_y;

    if (sx1 <= sx0) sx1 = sx0 + 1;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    step_x = (sx1 - sx0) > 4 ? (sx1 - sx0) / 4 : 1;
    step_y = (sy1 - sy0) > 4 ? (sy1 - sy0) / 4 : 1;

    for (int y = sy0; y < sy1; y += step_y) {
        const unsigned int *row = src + y * src_stride;
        for (int x = sx0; x < sx1; x += step_x) {
            unsigned int p = row[x];
            r += p & 0xFF;
            g += (p >> 8) & 0xFF;
            b += (p >> 16) & 0xFF;
            n++;
        }
    }

    if (n == 0) return src[sy0 * src_stride + sx0];
    return 0xFF000000U | ((b / n) << 16) | ((g / n) << 8) | (r / n);
}

/* Box-filtered when shrinking. Nearest-neighbour covers turned linework into
   aliased noise at grid size. */
void draw_image_scaled(unsigned int *vram, const TextureImage *img, int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!img || !img->data || img->width <= 0 || img->height <= 0) {
        draw_rect_solid(vram, dst_x, dst_y, dst_w, dst_h, COL_PANEL);
        draw_frame(vram, dst_x, dst_y, dst_w, dst_h, COL_LINE);
        /* A crossed-out frame stays legible even in the narrow cover-list
           thumbnail, where the old NO COVER label overflowed both sides. */
        if (dst_w > 6 && dst_h > 6) {
            int inner_w = dst_w - 7;
            int inner_h = dst_h - 7;
            for (int y = 0; y <= inner_h; y++) {
                int x = (y * inner_w) / inner_h;
                int py = dst_y + 3 + y;
                int left = dst_x + 3 + x;
                int right = dst_x + dst_w - 4 - x;
                if (py >= 0 && py < SCREEN_HEIGHT) {
                    if (left >= 0 && left < SCREEN_WIDTH) vram[py * BUFFER_WIDTH + left] = COL_FG_MUTED;
                    if (right >= 0 && right < SCREEN_WIDTH) vram[py * BUFFER_WIDTH + right] = COL_FG_MUTED;
                }
            }
        }
        return;
    }

    const unsigned int *src = (const unsigned int *)img->data;
    int shrinking = (img->width > dst_w || img->height > dst_h);

    for (int y = 0; y < dst_h; y++) {
        int py = dst_y + y;
        if (py < 0 || py >= SCREEN_HEIGHT) continue;

        int sy0 = (y * img->height) / dst_h;
        int sy1 = ((y + 1) * img->height) / dst_h;
        if (sy0 >= img->height) sy0 = img->height - 1;

        unsigned int *dst_row = vram + py * BUFFER_WIDTH;
        const unsigned int *src_row = src + sy0 * img->width;

        for (int x = 0; x < dst_w; x++) {
            int px = dst_x + x;
            if (px < 0 || px >= SCREEN_WIDTH) continue;

            int sx0 = (x * img->width) / dst_w;
            int sx1 = ((x + 1) * img->width) / dst_w;
            if (sx0 >= img->width) sx0 = img->width - 1;

            dst_row[px] = shrinking ? sample_box(src, img->width, sx0, sy0, sx1, sy1)
                                    : src_row[sx0];
        }
    }
}

unsigned int detect_edge_background(const TextureImage *img) {
    if (!img->data || img->width <= 0 || img->height <= 0) return 0xFF000000;

    const unsigned int *src = (const unsigned int *)img->data;
    long long total_luma = 0;
    int samples = 0;

    for (int i = 0; i < 32; i++) {
        int sx = (i * (img->width - 1)) / 31;
        unsigned int p_top = src[sx];
        unsigned int p_bot = src[(img->height - 1) * img->width + sx];
        total_luma += (int)(0.299f * (p_top & 0xFF) + 0.587f * ((p_top >> 8) & 0xFF) + 0.114f * ((p_top >> 16) & 0xFF));
        total_luma += (int)(0.299f * (p_bot & 0xFF) + 0.587f * ((p_bot >> 8) & 0xFF) + 0.114f * ((p_bot >> 16) & 0xFF));
        samples += 2;
    }

    for (int i = 0; i < 32; i++) {
        int sy = (i * (img->height - 1)) / 31;
        unsigned int p_left = src[sy * img->width];
        unsigned int p_right = src[sy * img->width + (img->width - 1)];
        total_luma += (int)(0.299f * (p_left & 0xFF) + 0.587f * ((p_left >> 8) & 0xFF) + 0.114f * ((p_left >> 16) & 0xFF));
        total_luma += (int)(0.299f * (p_right & 0xFF) + 0.587f * ((p_right >> 8) & 0xFF) + 0.114f * ((p_right >> 16) & 0xFF));
        samples += 2;
    }

    int avg_luma = (int)(total_luma / samples);
    return (avg_luma > 128) ? 0xFFFFFFFF : 0xFF000000;
}

void calculate_autocrop_box(TextureImage *img) {
    img->crop_x0 = 0;
    img->crop_y0 = 0;
    img->crop_x1 = img->width - 1;
    img->crop_y1 = img->height - 1;

    if (!img->data || img->width <= 32 || img->height <= 32) return;

    const unsigned int *src = (const unsigned int *)img->data;
    int step_x = img->width / 64;
    if (step_x < 1) step_x = 1;
    int step_y = img->height / 64;
    if (step_y < 1) step_y = 1;

    for (int y = 0; y < img->height / 4; y += step_y) {
        int has_content = 0;
        for (int x = 0; x < img->width; x += step_x) {
            unsigned int p = src[y * img->width + x];
            int luma = (int)(0.299f * (p & 0xFF) + 0.587f * ((p >> 8) & 0xFF) + 0.114f * ((p >> 16) & 0xFF));
            if (luma < 220) { has_content = 1; break; }
        }
        if (has_content) { img->crop_y0 = y; break; }
    }

    for (int y = img->height - 1; y > (img->height * 3) / 4; y -= step_y) {
        int has_content = 0;
        for (int x = 0; x < img->width; x += step_x) {
            unsigned int p = src[y * img->width + x];
            int luma = (int)(0.299f * (p & 0xFF) + 0.587f * ((p >> 8) & 0xFF) + 0.114f * ((p >> 16) & 0xFF));
            if (luma < 220) { has_content = 1; break; }
        }
        if (has_content) { img->crop_y1 = y; break; }
    }

    for (int x = 0; x < img->width / 4; x += step_x) {
        int has_content = 0;
        for (int y = 0; y < img->height; y += step_y) {
            unsigned int p = src[y * img->width + x];
            int luma = (int)(0.299f * (p & 0xFF) + 0.587f * ((p >> 8) & 0xFF) + 0.114f * ((p >> 16) & 0xFF));
            if (luma < 220) { has_content = 1; break; }
        }
        if (has_content) { img->crop_x0 = x; break; }
    }

    for (int x = img->width - 1; x > (img->width * 3) / 4; x -= step_x) {
        int has_content = 0;
        for (int y = 0; y < img->height; y += step_y) {
            unsigned int p = src[y * img->width + x];
            int luma = (int)(0.299f * (p & 0xFF) + 0.587f * ((p >> 8) & 0xFF) + 0.114f * ((p >> 16) & 0xFF));
            if (luma < 220) { has_content = 1; break; }
        }
        if (has_content) { img->crop_x1 = x; break; }
    }
}

char *read_bookmarks_file(void) {
    SceUID fd = sceIoOpen(bookmarks_path, PSP_O_RDONLY, 0777);
    if (fd < 0) return NULL;

    SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (size < 0 || size > MAX_BOOKMARKS_SIZE) {
        sceIoClose(fd);
        return NULL;
    }
    sceIoLseek(fd, 0, PSP_SEEK_SET);

    char *contents = (char *)malloc((size_t)size + 1);
    if (!contents) {
        sceIoClose(fd);
        return NULL;
    }

    int bytes_read = sceIoRead(fd, contents, (unsigned int)size);
    sceIoClose(fd);
    if (bytes_read < 0) {
        free(contents);
        return NULL;
    }
    contents[bytes_read] = '\0';
    return contents;
}

int append_bookmark_text(char *out, size_t capacity, size_t *used, const char *format, ...) {
    if (*used >= capacity) return 0;

    va_list args;
    va_start(args, format);
    int written = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= capacity - *used) return 0;
    *used += (size_t)written;
    return 1;
}

int load_saved_page(const char *full_path, const char *legacy_filename) {
    char *contents = read_bookmarks_file();
    if (!contents) return 0;

    int saved_page = 0;
    char *entry = strtok(contents, "\n");
    while (entry) {
        const char *saved_path = NULL;
        int parsed_page = 0;
        if (parse_page_bookmark(entry, &saved_path, &parsed_page)) {
            if (strcmp(saved_path, full_path) == 0) {
                saved_page = parsed_page;
                break;
            }
        } else {
            char name[MAX_NAME_LEN];
            int p = 0;
            if (sscanf(entry, "%127[^=]=%d", name, &p) == 2 &&
                strcmp(name, legacy_filename) == 0) {
                saved_page = p;
                break;
            }
        }
        entry = strtok(NULL, "\n");
    }

    free(contents);
    return saved_page;
}

static int parse_total_bookmark(char *line, const char **path, int *total) {
    char *total_text;
    char *end = NULL;
    long parsed;

    if (strncmp(line, "TOTAL\t", 6) != 0) return 0;
    total_text = strrchr(line + 6, '\t');
    if (!total_text) return 0;
    parsed = strtol(total_text + 1, &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > MAX_PAGES) return 0;
    *total_text = '\0';
    *path = line + 6;
    *total = (int)parsed;
    return 1;
}

static int series_chapter_for_path(const MangaSeries *series, const char *path) {
    size_t folder_len = strlen(series->folder_path);
    if (strncmp(path, series->folder_path, folder_len) != 0 || path[folder_len] != '/') return -1;
    for (int i = 0; i < series->chapter_count; i++) {
        if (strcmp(path + folder_len + 1, series->chapter_files[i]) == 0) return i;
    }
    return -1;
}

static int load_saved_total(const char *full_path) {
    char *contents = read_bookmarks_file();
    char *entry;
    int result = 0;
    if (!contents) return 0;

    entry = strtok(contents, "\n");
    while (entry) {
        const char *saved_path = NULL;
        int total = 0;
        if (parse_total_bookmark(entry, &saved_path, &total) && strcmp(saved_path, full_path) == 0) {
            result = total;
            break;
        }
        entry = strtok(NULL, "\n");
    }
    free(contents);
    return result;
}

static void apply_library_progress(void) {
    char *contents;
    char *entry;

    for (int i = 0; i < series_count; i++) {
        library[i].has_progress = 0;
        library[i].progress_chapter_index = 0;
        library[i].progress_page_index = 0;
        library[i].progress_total_pages = 0;
    }

    contents = read_bookmarks_file();
    if (contents) {
        entry = strtok(contents, "\n");
        while (entry) {
            const char *saved_path = NULL;
            int page = 0;
            if (parse_page_bookmark(entry, &saved_path, &page)) {
                for (int i = 0; i < series_count; i++) {
                    int chapter = series_chapter_for_path(&library[i], saved_path);
                    if (chapter >= 0 && (!library[i].has_progress ||
                                         chapter >= library[i].progress_chapter_index)) {
                        library[i].has_progress = 1;
                        library[i].progress_chapter_index = chapter;
                        library[i].progress_page_index = page;
                    }
                }
            }
            entry = strtok(NULL, "\n");
        }
        free(contents);
    }

    contents = read_bookmarks_file();
    if (contents) {
        entry = strtok(contents, "\n");
        while (entry) {
            const char *saved_path = NULL;
            int total = 0;
            if (parse_total_bookmark(entry, &saved_path, &total)) {
                for (int i = 0; i < series_count; i++) {
                    int chapter = series_chapter_for_path(&library[i], saved_path);
                    if (library[i].has_progress && chapter == library[i].progress_chapter_index) {
                        library[i].progress_total_pages = total;
                    }
                }
            }
            entry = strtok(NULL, "\n");
        }
        free(contents);
    }

    if (last_read_path[0]) {
        for (int i = 0; i < series_count; i++) {
            int chapter = series_chapter_for_path(&library[i], last_read_path);
            if (chapter >= 0) {
                const char *filename = strrchr(last_read_path, '/');
                library[i].has_progress = 1;
                library[i].progress_chapter_index = chapter;
                library[i].progress_page_index = load_saved_page(last_read_path,
                    filename ? filename + 1 : last_read_path);
                library[i].progress_total_pages = load_saved_total(last_read_path);
                break;
            }
        }
    }
}

void load_last_read_target(void) {
    char *contents = read_bookmarks_file();
    if (!contents) return;

    char *entry = strtok(contents, "\n");
    while (entry) {
        if (strncmp(entry, "LAST_PATH=", 10) == 0) {
            snprintf(last_read_path, sizeof(last_read_path), "%s", entry + 10);
            break;
        }
        entry = strtok(NULL, "\n");
    }
    free(contents);
}

/* Settings live in their own file so a corrupt or truncated line can never
   take reading progress down with it. */
void save_config(void) {
    char buf[192];
    int len = snprintf(buf, sizeof(buf),
                       "themeidx %d\nview %d\nbg %d\ndir %d\ncrop %d\nlibview %d\ntransitions %d\n",
                       config.theme_index, (int)config.view_mode, (int)config.bg_mode,
                       (int)config.read_direction, config.auto_crop, (int)lib_view,
                       config.page_transitions);
    SceUID fd;

    if (len <= 0 || strlen(config_path) == 0) return;

    fd = sceIoOpen(config_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
}

static void config_set(const char *key, int value) {
    if (strcmp(key, "themeidx") == 0 && value >= 0 && value < THEME_COUNT) {
        config.theme_index = value;
    } else if (strcmp(key, "theme") == 0 && value >= 0 && value <= 1) {
        /* Migrate the old Dark/Light setting to Dracula/Solarized Light. */
        config.theme_index = value ? (THEME_COUNT - 1) : 0;
    } else if (strcmp(key, "view") == 0 && value >= 0 && value <= 2) {
        config.view_mode = (ViewMode)value;
    } else if (strcmp(key, "bg") == 0 && value >= 0 && value <= 2) {
        config.bg_mode = (BgMode)value;
    } else if (strcmp(key, "dir") == 0 && value >= 0 && value <= 1) {
        config.read_direction = (ReadDirection)value;
    } else if (strcmp(key, "crop") == 0) {
        config.auto_crop = value ? 1 : 0;
    } else if (strcmp(key, "libview") == 0 && value >= 0 && value <= 2) {
        lib_view = (LibraryViewMode)value;
    } else if (strcmp(key, "transitions") == 0) {
        config.page_transitions = value ? 1 : 0;
    }
}

void load_config(void) {
    char buf[257];
    SceUID fd;
    int read_bytes;
    char *line;

    if (strlen(config_path) == 0) return;

    fd = sceIoOpen(config_path, PSP_O_RDONLY, 0777);
    if (fd < 0) return;

    read_bytes = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (read_bytes <= 0) return;
    buf[read_bytes] = '\0';

    line = strtok(buf, "\n");
    while (line) {
        char key[32];
        int value;
        if (sscanf(line, "%31s %d", key, &value) == 2) config_set(key, value);
        line = strtok(NULL, "\n");
    }
}

void save_current_progress(void) {
    if (strlen(current_comic.current_file) == 0) return;
    snprintf(last_read_path, sizeof(last_read_path), "%s", current_comic.full_path);

    char *contents = read_bookmarks_file();
    char *out_buf = (char *)calloc(MAX_BOOKMARKS_SIZE + 1, 1);
    if (!out_buf) {
        free(contents);
        return;
    }

    size_t used = 0;
    if (!append_bookmark_text(out_buf, MAX_BOOKMARKS_SIZE + 1, &used,
                              "LAST_PATH=%s\n", current_comic.full_path)) {
        free(contents);
        free(out_buf);
        return;
    }
    if (!append_bookmark_text(out_buf, MAX_BOOKMARKS_SIZE + 1, &used,
                              "TOTAL\t%s\t%d\n", current_comic.full_path,
                              current_comic.total_pages)) {
        free(contents);
        free(out_buf);
        return;
    }

    char *entry = contents ? strtok(contents, "\n") : NULL;
    int replaced = 0;

    while (entry) {
        if (strncmp(entry, "LAST_PATH=", 10) == 0) {
            entry = strtok(NULL, "\n");
            continue;
        }

        if (strncmp(entry, "TOTAL\t", 6) == 0) {
            const char *saved_path = NULL;
            int saved_total = 0;
            if (parse_total_bookmark(entry, &saved_path, &saved_total)) {
                if (strcmp(saved_path, current_comic.full_path) != 0) {
                    append_bookmark_text(out_buf, MAX_BOOKMARKS_SIZE + 1, &used,
                                         "TOTAL\t%s\t%d\n", saved_path, saved_total);
                }
            }
            entry = strtok(NULL, "\n");
            continue;
        }

        const char *saved_path = NULL;
        int saved_page = 0;
        if (parse_page_bookmark(entry, &saved_path, &saved_page)) {
            if (strcmp(saved_path, current_comic.full_path) == 0) {
                append_bookmark_text(out_buf, MAX_BOOKMARKS_SIZE + 1, &used,
                                     "PAGE\t%s\t%d\n", current_comic.full_path,
                                     current_comic.current_page_index);
                replaced = 1;
            } else {
                append_bookmark_text(out_buf, MAX_BOOKMARKS_SIZE + 1, &used,
                                     "PAGE\t%s\t%d\n", saved_path, saved_page);
            }
        } else {
            char name[MAX_NAME_LEN];
            int p = 0;
            if (sscanf(entry, "%127[^=]=%d", name, &p) == 2 &&
                strcmp(name, current_comic.current_file) != 0) {
                append_bookmark_text(out_buf, MAX_BOOKMARKS_SIZE + 1, &used, "%s\n", entry);
            }
        }
        entry = strtok(NULL, "\n");
    }

    if (!replaced) {
        append_bookmark_text(out_buf, MAX_BOOKMARKS_SIZE + 1, &used,
                             "PAGE\t%s\t%d\n", current_comic.full_path,
                             current_comic.current_page_index);
    }

    SceUID fd = sceIoOpen(bookmarks_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, out_buf, used);
        sceIoClose(fd);
    }
    free(contents);
    free(out_buf);

    for (int i = 0; i < series_count; i++) {
        int chapter = series_chapter_for_path(&library[i], current_comic.full_path);
        if (chapter >= 0) {
            library[i].has_progress = 1;
            library[i].progress_chapter_index = chapter;
            library[i].progress_page_index = current_comic.current_page_index;
            library[i].progress_total_pages = current_comic.total_pages;
            break;
        }
    }
}

int exit_callback(int arg1, int arg2, void *common) {
    save_current_progress();
    sceKernelExitGame();
    return 0;
}

int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
}

int is_cbz_file(const char *name) {
    int len = strlen(name);
    if (len < 4) return 0;
    return (strcasecmp(name + len - 4, ".cbz") == 0);
}

int is_image_file(const char *name) {
    int len = strlen(name);
    if (len < 4) return 0;
    if (strcasecmp(name + len - 4, ".jpg") == 0 || strcasecmp(name + len - 4, ".png") == 0) return 1;
    if (len >= 5 && strcasecmp(name + len - 5, ".jpeg") == 0) return 1;
    return 0;
}

int compare_strings(const void *a, const void *b) {
    return natural_compare((const char *)a, (const char *)b);
}

int compare_pages(const void *a, const void *b) {
    const PageEntry *page_a = (const PageEntry *)a;
    const PageEntry *page_b = (const PageEntry *)b;
    return natural_compare(page_a->name, page_b->name);
}

int load_cached_thumbnail(const char *series_name, TextureImage *out_thumb) {
    char cache_file[MAX_PATH_LEN];
    snprintf(cache_file, sizeof(cache_file), "%s/%s_v7.bin", cache_dir, series_name);

    SceUID fd = sceIoOpen(cache_file, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;

    int expected_size = THUMB_WIDTH * THUMB_HEIGHT * 4;
    unsigned int *buf = (unsigned int *)malloc(expected_size);
    if (!buf) {
        sceIoClose(fd);
        return 0;
    }

    int read_bytes = sceIoRead(fd, buf, expected_size);
    sceIoClose(fd);

    if (read_bytes == expected_size) {
        out_thumb->data = (unsigned char *)buf;
        out_thumb->width = THUMB_WIDTH;
        out_thumb->height = THUMB_HEIGHT;
        out_thumb->channels = 4;
        return 1;
    }

    free(buf);
    return 0;
}

void save_cached_thumbnail(const char *series_name, const TextureImage *thumb) {
    if (!thumb->data) return;

    sceIoMkdir(cache_dir, 0777);

    char cache_file[MAX_PATH_LEN];
    snprintf(cache_file, sizeof(cache_file), "%s/%s_v7.bin", cache_dir, series_name);

    SceUID fd = sceIoOpen(cache_file, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, thumb->data, THUMB_WIDTH * THUMB_HEIGHT * 4);
        sceIoClose(fd);
    }
}

struct my_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void my_error_exit(j_common_ptr cinfo) {
    struct my_error_mgr *myerr = (struct my_error_mgr *)cinfo->err;
    longjmp(myerr->setjmp_buffer, 1);
}

int decode_jpeg_scaled_thumbnail(const char *filepath, const char *series_name, TextureImage *out_thumb) {
    FILE *infile = fopen(filepath, "rb");
    if (!infile) return 0;

    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        return 0;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, infile);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.scale_num = 1;
    /* Use the cheapest native JPEG scale that still exceeds the cache size.
       Fixed 1/8 decoding enlarged many ordinary covers after decoding. */
    cinfo.scale_denom = 1;
    if (cinfo.image_width / 8 >= THUMB_WIDTH && cinfo.image_height / 8 >= THUMB_HEIGHT) {
        cinfo.scale_denom = 8;
    } else if (cinfo.image_width / 4 >= THUMB_WIDTH && cinfo.image_height / 4 >= THUMB_HEIGHT) {
        cinfo.scale_denom = 4;
    } else if (cinfo.image_width / 2 >= THUMB_WIDTH && cinfo.image_height / 2 >= THUMB_HEIGHT) {
        cinfo.scale_denom = 2;
    }
    cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    int scaled_w = cinfo.output_width;
    int scaled_h = cinfo.output_height;

    unsigned char *temp_rgb = (unsigned char *)malloc(scaled_w * scaled_h * 3);
    if (!temp_rgb) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        return 0;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *row_ptr = temp_rgb + (cinfo.output_scanline * scaled_w * 3);
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(infile);

    int crop_w, crop_h, crop_x, crop_y;
    if (scaled_w * THUMB_HEIGHT > scaled_h * THUMB_WIDTH) {
        crop_h = scaled_h;
        crop_w = (scaled_h * THUMB_WIDTH) / THUMB_HEIGHT;
        crop_x = (scaled_w - crop_w) / 2;
        crop_y = 0;
    } else {
        crop_w = scaled_w;
        crop_h = (scaled_w * THUMB_HEIGHT) / THUMB_WIDTH;
        crop_x = 0;
        crop_y = (scaled_h - crop_h) / 2;
    }

    unsigned int *thumb_pixels = (unsigned int *)malloc(THUMB_WIDTH * THUMB_HEIGHT * 4);
    if (thumb_pixels) {
        for (int y = 0; y < THUMB_HEIGHT; y++) {
            int sy = crop_y + (y * crop_h) / THUMB_HEIGHT;
            for (int x = 0; x < THUMB_WIDTH; x++) {
                int sx = crop_x + (x * crop_w) / THUMB_WIDTH;
                unsigned char *p = temp_rgb + (sy * scaled_w + sx) * 3;
                unsigned int r = p[0];
                unsigned int g = p[1];
                unsigned int b = p[2];
                thumb_pixels[y * THUMB_WIDTH + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            }
        }
        out_thumb->data = (unsigned char *)thumb_pixels;
        out_thumb->width = THUMB_WIDTH;
        out_thumb->height = THUMB_HEIGHT;
        out_thumb->channels = 4;

        save_cached_thumbnail(series_name, out_thumb);
    }

    free(temp_rgb);
    return (out_thumb->data != NULL);
}

/* Decode only as much JPEG detail as the PSP can display. libjpeg first uses
   its cheap IDCT scaling, then scanlines are reduced directly into the final
   RGBA buffer so a full-resolution intermediate is never allocated. */
int decode_jpeg_scaled_page(const unsigned char *data, unsigned int size, TextureImage *out_image) {
    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;
    volatile unsigned int *pixels = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        if (pixels) free((void *)pixels);
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, size);
    jpeg_read_header(&cinfo, TRUE);

    int target_w = cinfo.image_width > PAGE_DECODE_TARGET_WIDTH
                 ? PAGE_DECODE_TARGET_WIDTH : (int)cinfo.image_width;
    int target_h = ((int)cinfo.image_height * target_w) / (int)cinfo.image_width;
    while ((long long)target_w * target_h * 4 > PAGE_DECODE_MAX_BYTES && target_w > SCREEN_WIDTH) {
        target_w = (target_w * 3) / 4;
        target_h = ((int)cinfo.image_height * target_w) / (int)cinfo.image_width;
    }
    if (target_w < 1) target_w = 1;
    if (target_h < 1) target_h = 1;

    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    if (cinfo.image_width / 8 >= (unsigned int)target_w) cinfo.scale_denom = 8;
    else if (cinfo.image_width / 4 >= (unsigned int)target_w) cinfo.scale_denom = 4;
    else if (cinfo.image_width / 2 >= (unsigned int)target_w) cinfo.scale_denom = 2;
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int decoded_w = (int)cinfo.output_width;
    int decoded_h = (int)cinfo.output_height;
    if (target_w > decoded_w) target_w = decoded_w;
    target_h = (decoded_h * target_w) / decoded_w;
    if (target_h < 1) target_h = 1;

    pixels = (unsigned int *)malloc((size_t)target_w * target_h * 4);
    if (!pixels) {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }

    JSAMPARRAY row = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                                cinfo.output_width * cinfo.output_components, 1);
    int out_y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, row, 1);
        int source_y = (int)cinfo.output_scanline - 1;

        while (out_y < target_h && (out_y * decoded_h) / target_h == source_y) {
            unsigned int *dst = (unsigned int *)pixels + out_y * target_w;
            for (int x = 0; x < target_w; x++) {
                int source_x = (x * decoded_w) / target_w;
                unsigned char *p = row[0] + source_x * 3;
                dst[x] = 0xFF000000U | ((unsigned int)p[2] << 16) |
                         ((unsigned int)p[1] << 8) | p[0];
            }
            out_y++;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    out_image->data = (unsigned char *)pixels;
    out_image->width = target_w;
    out_image->height = target_h;
    out_image->channels = 4;
    return 1;
}

void load_cover_thumbnail(MangaSeries *series) {
    if (load_cached_thumbnail(series->name, &series->cover_thumb)) {
        return;
    }

    if (strlen(series->cover_path) > 0) {
        int len = strlen(series->cover_path);
        if (len >= 4 && (strcasecmp(series->cover_path + len - 4, ".jpg") == 0 || strcasecmp(series->cover_path + len - 5, ".jpeg") == 0)) {
            if (decode_jpeg_scaled_thumbnail(series->cover_path, series->name, &series->cover_thumb)) {
                return;
            }
        }

        SceUID fd = sceIoOpen(series->cover_path, PSP_O_RDONLY, 0777);
        if (fd >= 0) {
            SceOff fsize = sceIoLseek(fd, 0, PSP_SEEK_END);
            sceIoLseek(fd, 0, PSP_SEEK_SET);
            if (fsize > 0 && fsize < (2 * 1024 * 1024)) {
                unsigned char *raw = (unsigned char *)malloc(fsize);
                if (raw) {
                    sceIoRead(fd, raw, fsize);
                    int orig_w, orig_h, comp;
                    unsigned char *full = stbi_load_from_memory(raw, fsize, &orig_w, &orig_h, &comp, 4);
                    if (full) {
                        int crop_w, crop_h, crop_x, crop_y;
                        if (orig_w * THUMB_HEIGHT > orig_h * THUMB_WIDTH) {
                            crop_h = orig_h;
                            crop_w = (orig_h * THUMB_WIDTH) / THUMB_HEIGHT;
                            crop_x = (orig_w - crop_w) / 2;
                            crop_y = 0;
                        } else {
                            crop_w = orig_w;
                            crop_h = (orig_w * THUMB_HEIGHT) / THUMB_WIDTH;
                            crop_x = 0;
                            crop_y = (orig_h - crop_h) / 2;
                        }

                        unsigned int *thumb_pixels = (unsigned int *)malloc(THUMB_WIDTH * THUMB_HEIGHT * 4);
                        if (thumb_pixels) {
                            unsigned int *src = (unsigned int *)full;
                            for (int y = 0; y < THUMB_HEIGHT; y++) {
                                int sy = crop_y + (y * crop_h) / THUMB_HEIGHT;
                                for (int x = 0; x < THUMB_WIDTH; x++) {
                                    int sx = crop_x + (x * crop_w) / THUMB_WIDTH;
                                    unsigned int pixel = src[sy * orig_w + sx];
                                    unsigned char r = pixel & 0xFF;
                                    unsigned char g = (pixel >> 8) & 0xFF;
                                    unsigned char b = (pixel >> 16) & 0xFF;
                                    unsigned char a = (pixel >> 24) & 0xFF;
                                    thumb_pixels[y * THUMB_WIDTH + x] = (a << 24) | (b << 16) | (g << 8) | r;
                                }
                            }
                            series->cover_thumb.data = (unsigned char *)thumb_pixels;
                            series->cover_thumb.width = THUMB_WIDTH;
                            series->cover_thumb.height = THUMB_HEIGHT;
                            series->cover_thumb.channels = 4;
                            save_cached_thumbnail(series->name, &series->cover_thumb);
                        }
                        stbi_image_free(full);
                    }
                    free(raw);
                }
            }
            sceIoClose(fd);
        }
    }
}

void scan_mangas_library(const char *root_path) {
    for (int i = 0; i < series_count; i++) {
        if (library[i].cover_thumb.data) {
            free(library[i].cover_thumb.data);
            library[i].cover_thumb.data = NULL;
        }
    }
    series_count = 0;

    char dir_names[MAX_SERIES][MAX_NAME_LEN];
    int found_dirs = 0;

    SceUID dfd = sceIoDopen(root_path);
    if (dfd >= 0) {
        SceIoDirent dir;
        memset(&dir, 0, sizeof(SceIoDirent));

        while (sceIoDread(dfd, &dir) > 0 && found_dirs < MAX_SERIES) {
            if (dir.d_name[0] == '.') continue;

            if ((dir.d_stat.st_attr & 0x0010) || FIO_SO_ISDIR(dir.d_stat.st_attr)) {
                snprintf(dir_names[found_dirs], sizeof(dir_names[found_dirs]), "%s", dir.d_name);
                found_dirs++;
            }
        }
        sceIoDclose(dfd);
    }

    for (int i = 0; i < found_dirs && series_count < MAX_SERIES; i++) {
        MangaSeries *s = &library[series_count];
        memset(s, 0, sizeof(MangaSeries));

        snprintf(s->name, sizeof(s->name), "%s", dir_names[i]);
        snprintf(s->folder_path, sizeof(s->folder_path), "%s/%s", root_path, dir_names[i]);

        char current_folder[MAX_PATH_LEN];
        snprintf(current_folder, sizeof(current_folder), "%s", s->folder_path);

        SceUID sub_dfd = sceIoDopen(current_folder);
        if (sub_dfd >= 0) {
            SceIoDirent sub_dir;
            memset(&sub_dir, 0, sizeof(SceIoDirent));

            while (sceIoDread(sub_dfd, &sub_dir) > 0) {
                if (sub_dir.d_name[0] == '.') continue;

                if (is_cbz_file(sub_dir.d_name) && s->chapter_count < MAX_CHAPTERS) {
                    snprintf(s->chapter_files[s->chapter_count], sizeof(s->chapter_files[s->chapter_count]), "%s", sub_dir.d_name);
                    s->chapter_count++;
                } else if (is_image_file(sub_dir.d_name)) {
                    if (strncasecmp(sub_dir.d_name, "cover", 5) == 0 || strncasecmp(sub_dir.d_name, "folder", 6) == 0) {
                        snprintf(s->cover_path, sizeof(s->cover_path), "%s/%s", current_folder, sub_dir.d_name);
                    }
                }
            }
            sceIoDclose(sub_dfd);
        }

        if (s->chapter_count > 0) {
            boot_log("scan: sorting %d chapters of '%s'", s->chapter_count, s->name);
            qsort(s->chapter_files, s->chapter_count, MAX_NAME_LEN, compare_strings);
            boot_log("scan: loading cover '%s'", s->cover_path);
            load_cover_thumbnail(s);
            boot_log("scan: '%s' done", s->name);
            series_count++;
        }
    }

    if (lib_selected_index >= series_count && series_count > 0) {
        lib_selected_index = series_count - 1;
    }
    apply_library_progress();
}

int load_cbz_metadata(const char *full_path) {
    SceUID fd = sceIoOpen(full_path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;

    SceOff file_size = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (file_size < 22 || file_size > 0x7FFFFFFF) {
        sceIoClose(fd);
        return 0;
    }

    int search_buf_size = file_size < 65557 ? (int)file_size : 65557;
    unsigned char *buffer = (unsigned char *)malloc(search_buf_size);
    if (!buffer) {
        sceIoClose(fd);
        return 0;
    }
    sceIoLseek(fd, file_size - search_buf_size, PSP_SEEK_SET);
    if (sceIoRead(fd, buffer, search_buf_size) != search_buf_size) {
        free(buffer);
        sceIoClose(fd);
        return 0;
    }

    int eocd_pos = -1;
    for (int i = search_buf_size - 22; i >= 0; i--) {
        if (read_uint32_safe(buffer + i) == 0x06054B50) {
            eocd_pos = i;
            break;
        }
    }

    if (eocd_pos < 0) {
        free(buffer);
        sceIoClose(fd);
        return 0;
    }

    const unsigned char *eocd_ptr = buffer + eocd_pos;
    unsigned short total_entries = read_uint16_safe(eocd_ptr + 10);
    unsigned int central_dir_offset = read_uint32_safe(eocd_ptr + 16);
    free(buffer);

    if ((SceOff)central_dir_offset >= file_size) {
        sceIoClose(fd);
        return 0;
    }

    current_comic.total_pages = 0;
    snprintf(current_comic.full_path, sizeof(current_comic.full_path), "%s", full_path);
    current_comic.archive_size = (unsigned int)file_size;

    sceIoLseek(fd, central_dir_offset, PSP_SEEK_SET);

    unsigned char cd_raw[46];
    char filename_buffer[MAX_NAME_LEN];

    for (int i = 0; i < total_entries && current_comic.total_pages < MAX_PAGES; i++) {
        if (sceIoRead(fd, cd_raw, 46) != 46) break;
        if (read_uint32_safe(cd_raw) != 0x02014B50) break;

        unsigned short compression_method = read_uint16_safe(cd_raw + 10);
        unsigned int compressed_size = read_uint32_safe(cd_raw + 20);
        unsigned int uncompressed_size = read_uint32_safe(cd_raw + 24);
        unsigned short filename_length = read_uint16_safe(cd_raw + 28);
        unsigned short extra_field_length = read_uint16_safe(cd_raw + 30);
        unsigned short file_comment_length = read_uint16_safe(cd_raw + 32);
        unsigned int relative_offset = read_uint32_safe(cd_raw + 42);

        int name_len = filename_length >= MAX_NAME_LEN ? MAX_NAME_LEN - 1 : filename_length;
        memset(filename_buffer, 0, sizeof(filename_buffer));
        if (sceIoRead(fd, filename_buffer, name_len) != name_len) break;

        int skip = (filename_length - name_len) + extra_field_length + file_comment_length;
        if (skip > 0) sceIoLseek(fd, skip, PSP_SEEK_CUR);

        if (is_image_file(filename_buffer) &&
            relative_offset < (unsigned int)file_size &&
            compressed_size > 0 && compressed_size <= MAX_PAGE_DATA_SIZE &&
            uncompressed_size > 0 && uncompressed_size <= MAX_PAGE_DATA_SIZE &&
            (compression_method == 0 || compression_method == 8) &&
            (compression_method != 0 || compressed_size == uncompressed_size)) {
            PageEntry *p = &current_comic.pages[current_comic.total_pages];
            snprintf(p->name, sizeof(p->name), "%s", filename_buffer);
            p->local_header_offset = relative_offset;
            p->compressed_size = compressed_size;
            p->uncompressed_size = uncompressed_size;
            p->compression_method = compression_method;
            current_comic.total_pages++;
        }
    }

    sceIoClose(fd);

    if (current_comic.total_pages > 0) {
        qsort(current_comic.pages, current_comic.total_pages, sizeof(PageEntry), compare_pages);
        int saved = load_saved_page(current_comic.full_path, current_comic.current_file);
        current_comic.current_page_index = (saved >= 0 && saved < current_comic.total_pages) ? saved : 0;
        return 1;
    }

    return 0;
}

/* Keep the last visible screen while the decoded page and render surface are
   released for memory. The other VRAM buffer always contains the most recent
   presented frame, including the previous spinner phase. */
static void render_loading_overlay(unsigned int *vram) {
    const unsigned int *previous = (vram == (unsigned int *)VRAM_CACHED_0)
                                 ? (const unsigned int *)VRAM_CACHED_1
                                 : (const unsigned int *)VRAM_CACHED_0;
    memcpy(vram, previous, BUFFER_SIZE);
    draw_busy_spinner(vram, SPINNER_X, SPINNER_Y, loading_phase);
}

/* Advances the busy dial and pushes a frame without waiting for vblank. The
   decode below is synchronous, so these stage boundaries are the only chance
   the indicator gets to move. */
static void loading_tick(void) {
    loading_phase = (loading_phase + 1) & 7;
    present_frame(render_loading_overlay, 0);
}

static int load_current_page_inner(void) {
    if (current_comic.total_pages == 0 || current_comic.current_page_index >= current_comic.total_pages) {
        page_load_error = 1;
        return 0;
    }

    PageEntry *page = &current_comic.pages[current_comic.current_page_index];
    if (current_comic.archive_size < 30 ||
        page->local_header_offset > current_comic.archive_size - 30 ||
        page->compressed_size > MAX_PAGE_DATA_SIZE ||
        page->uncompressed_size > MAX_PAGE_DATA_SIZE ||
        (page->compression_method == 0 && page->compressed_size != page->uncompressed_size)) {
        page_load_error = 1;
        return 0;
    }

    SceUID fd = sceIoOpen(current_comic.full_path, PSP_O_RDONLY, 0777);
    if (fd < 0) { page_load_error = 1; return 0; }

    sceIoLseek(fd, page->local_header_offset, PSP_SEEK_SET);
    
    unsigned char local_raw[30];
    if (sceIoRead(fd, local_raw, 30) != 30) {
        sceIoClose(fd);
        page_load_error = 1;
        return 0;
    }

    if (read_uint32_safe(local_raw) != 0x04034B50) {
        sceIoClose(fd);
        page_load_error = 1;
        return 0;
    }

    unsigned short fn_len = read_uint16_safe(local_raw + 26);
    unsigned short extra_len = read_uint16_safe(local_raw + 28);
    SceOff data_offset = (SceOff)page->local_header_offset + 30 + fn_len + extra_len;
    if (data_offset > current_comic.archive_size ||
        page->compressed_size > current_comic.archive_size - (unsigned int)data_offset) {
        sceIoClose(fd);
        page_load_error = 1;
        return 0;
    }
    sceIoLseek(fd, fn_len + extra_len, PSP_SEEK_CUR);

    /* Drop the previous render before allocating archive and decode buffers.
       This is the difference between fitting one large page and briefly
       needing enough RAM for two. Failed page turns reload the old page. */
    page_surface_free();
    if (current_image.data) {
        stbi_image_free(current_image.data);
        current_image.data = NULL;
    }

    unsigned char *raw_image_data = (unsigned char *)malloc(page->uncompressed_size);
    int raw_image_size = page->uncompressed_size;
    if (!raw_image_data) {
        sceIoClose(fd);
        page_load_error = 1;
        return 0;
    }

    if (page->compression_method == 8) {
        z_stream stream;
        unsigned char *input = (unsigned char *)malloc(LOAD_CHUNK_BYTES);
        unsigned int remaining = page->compressed_size;
        int inflate_status = Z_OK;
        if (!input) {
            free(raw_image_data);
            sceIoClose(fd);
            page_load_error = 1;
            return 0;
        }
        memset(&stream, 0, sizeof(stream));
        stream.next_out = raw_image_data;
        stream.avail_out = page->uncompressed_size;

        inflate_status = inflateInit2(&stream, -MAX_WBITS);
        while (inflate_status == Z_OK && remaining > 0) {
            unsigned int chunk = remaining > LOAD_CHUNK_BYTES ? LOAD_CHUNK_BYTES : remaining;
            if (sceIoRead(fd, input, (int)chunk) != (int)chunk) {
                inflate_status = Z_ERRNO;
                break;
            }
            remaining -= chunk;
            stream.next_in = input;
            stream.avail_in = chunk;
            while (inflate_status == Z_OK && stream.avail_in > 0) {
                inflate_status = inflate(&stream, remaining == 0 ? Z_FINISH : Z_NO_FLUSH);
            }
            loading_tick();
        }
        inflateEnd(&stream);
        free(input);
        sceIoClose(fd);
        if (inflate_status != Z_STREAM_END || stream.total_out != page->uncompressed_size) {
            free(raw_image_data);
            page_load_error = 1;
            return 0;
        }
    } else {
        unsigned int done = 0;
        while (done < page->uncompressed_size) {
            unsigned int chunk = page->uncompressed_size - done;
            if (chunk > LOAD_CHUNK_BYTES) chunk = LOAD_CHUNK_BYTES;
            if (sceIoRead(fd, raw_image_data + done, (int)chunk) != (int)chunk) {
                free(raw_image_data);
                sceIoClose(fd);
                page_load_error = 1;
                return 0;
            }
            done += chunk;
            loading_tick();
        }
        sceIoClose(fd);
    }

    loading_tick();

    TextureImage next_image = { NULL, 0, 0, 0, 0, 0, 0, 0 };
    if (raw_image_size >= 2 && raw_image_data[0] == 0xFF && raw_image_data[1] == 0xD8) {
        decode_jpeg_scaled_page(raw_image_data, (unsigned int)raw_image_size, &next_image);
    } else {
        next_image.data = stbi_load_from_memory(raw_image_data, raw_image_size,
                                               &next_image.width,
                                               &next_image.height,
                                               &next_image.channels, 4);
    }
    free(raw_image_data);

    if (next_image.data) {
        current_image = next_image;
        scroll_y = 0;
        scroll_y_q8 = 0;
        scroll_vel_q8 = 0;
        mag_offset_x = 0;
        mag_offset_y = 0;
        hud_display_frames = HUD_HOLD_FRAMES + HUD_FADE_FRAMES;
        page_load_error = 0;
        detected_bg_color = detect_edge_background(&current_image);
        calculate_autocrop_box(&current_image);
        page_surface_build();
        save_current_progress();
    } else {
        page_load_error = 1;
    }

    return (current_image.data != NULL);
}

/* Wrapper so every early return in the loader still clears the indicator. */
int load_current_page(void) {
    int loaded;

    loading_active = 1;
    loading_phase = 0;
    loading_tick();

    loaded = load_current_page_inner();

    loading_active = 0;
    return loaded;
}

/* ------------------------------------------------------------ page surface */

/* The reader used to resample the source image on every frame, which made any
   motion unaffordable. The page is now resampled once into a screen-space
   surface (already rotated for VIEW_ROTATE_90); scrolling and page slides are
   then row memcpys. Pages too large to buffer fall back to the direct path. */

#define PAGE_SURFACE_MAX_BYTES (6 * 1024 * 1024)

typedef struct {
    unsigned int *pixels;
    int width;
    int height;
    int valid;
    int key_page;
    int key_view;
    int key_mag;
    int key_crop;
} PageSurface;

PageSurface page_surface = { NULL, 0, 0, 0, -1, -1, -1, -1 };

void page_surface_free(void) {
    if (page_surface.pixels) free(page_surface.pixels);
    page_surface.pixels = NULL;
    page_surface.width = 0;
    page_surface.height = 0;
    page_surface.valid = 0;
    page_surface.key_page = -1;
}

static void page_source_rect(int *x0, int *y0, int *w, int *h) {
    *x0 = config.auto_crop ? current_image.crop_x0 : 0;
    *y0 = config.auto_crop ? current_image.crop_y0 : 0;
    *w  = config.auto_crop ? (current_image.crop_x1 - current_image.crop_x0 + 1) : current_image.width;
    *h  = config.auto_crop ? (current_image.crop_y1 - current_image.crop_y0 + 1) : current_image.height;
}

/* Size the page occupies on screen, before rotation is applied. */
static void page_render_size(int src_w, int src_h, int *out_w, int *out_h) {
    float scale;

    if (config.view_mode == VIEW_ROTATE_90) {
        float sx = (float)SCREEN_HEIGHT / (float)src_w;
        float sy = (float)SCREEN_WIDTH / (float)src_h;
        scale = sx < sy ? sx : sy;
        if (is_magnified) scale *= 1.25f;
    } else if (config.view_mode == VIEW_FIT_SCREEN) {
        float sx = (float)SCREEN_WIDTH / (float)src_w;
        float sy = (float)SCREEN_HEIGHT / (float)src_h;
        scale = sx < sy ? sx : sy;
        if (is_magnified) scale *= 1.75f;
    } else {
        scale = (float)SCREEN_WIDTH / (float)src_w;
        if (is_magnified) scale *= 1.25f;
    }

    *out_w = (int)(src_w * scale);
    *out_h = (int)(src_h * scale);
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;
}

int page_surface_build(void) {
    int src_x0, src_y0, src_w, src_h;
    int render_w, render_h, out_w, out_h, rotated;
    long long bytes;
    const unsigned int *src;

    page_surface.valid = 0;
    if (!current_image.data) return 0;

    page_source_rect(&src_x0, &src_y0, &src_w, &src_h);
    if (src_w <= 0 || src_h <= 0) return 0;

    page_render_size(src_w, src_h, &render_w, &render_h);
    rotated = (config.view_mode == VIEW_ROTATE_90);
    out_w = rotated ? render_h : render_w;
    out_h = rotated ? render_w : render_h;

    bytes = (long long)out_w * out_h * 4;
    if (bytes > PAGE_SURFACE_MAX_BYTES) return 0;

    if (page_surface.pixels) free(page_surface.pixels);
    page_surface.pixels = (unsigned int *)malloc((size_t)bytes);
    if (!page_surface.pixels) return 0;

    src = (const unsigned int *)current_image.data;

    for (int oy = 0; oy < out_h; oy++) {
        unsigned int *dst = page_surface.pixels + oy * out_w;

        if (!rotated) {
            int sy0 = src_y0 + (oy * src_h) / render_h;
            int sy1 = src_y0 + ((oy + 1) * src_h) / render_h;
            for (int ox = 0; ox < out_w; ox++) {
                int sx0 = src_x0 + (ox * src_w) / render_w;
                int sx1 = src_x0 + ((ox + 1) * src_w) / render_w;
                dst[ox] = sample_box(src, current_image.width, sx0, sy0, sx1, sy1);
            }
        } else {
            /* oy walks the pre-rotation x axis; ox walks its y axis backwards */
            int sx0 = src_x0 + (oy * src_w) / render_w;
            int sx1 = src_x0 + ((oy + 1) * src_w) / render_w;
            for (int ox = 0; ox < out_w; ox++) {
                int ry = render_h - 1 - ox;
                int sy0 = src_y0 + (ry * src_h) / render_h;
                int sy1 = src_y0 + ((ry + 1) * src_h) / render_h;
                dst[ox] = sample_box(src, current_image.width, sx0, sy0, sx1, sy1);
            }
        }
    }

    page_surface.width = out_w;
    page_surface.height = out_h;
    page_surface.valid = 1;
    page_surface.key_page = current_comic.current_page_index;
    page_surface.key_view = (int)config.view_mode;
    page_surface.key_mag = is_magnified;
    page_surface.key_crop = config.auto_crop;
    return 1;
}

/* Rebuilds only when something that shapes the surface actually changed. */
void page_surface_sync(void) {
    if (page_surface.valid &&
        page_surface.key_page == current_comic.current_page_index &&
        page_surface.key_view == (int)config.view_mode &&
        page_surface.key_mag == is_magnified &&
        page_surface.key_crop == config.auto_crop) {
        return;
    }
    page_surface_build();
}

/* How far the page can be panned before it runs out, in screen pixels. */
void page_view_extent(int *max_x, int *max_y) {
    int w, h;

    if (page_surface.valid) {
        w = page_surface.width;
        h = page_surface.height;
    } else if (current_image.data) {
        int src_x0, src_y0, src_w, src_h, render_w, render_h;
        page_source_rect(&src_x0, &src_y0, &src_w, &src_h);
        page_render_size(src_w, src_h, &render_w, &render_h);
        w = (config.view_mode == VIEW_ROTATE_90) ? render_h : render_w;
        h = (config.view_mode == VIEW_ROTATE_90) ? render_w : render_h;
    } else {
        w = SCREEN_WIDTH;
        h = SCREEN_HEIGHT;
    }

    *max_x = w > SCREEN_WIDTH ? w - SCREEN_WIDTH : 0;
    *max_y = h > SCREEN_HEIGHT ? h - SCREEN_HEIGHT : 0;
}

/* Blits the prepared surface, painting background only where it does not
   reach. slide_x offsets the whole page for the page-turn transition. */
void blit_page_surface(unsigned int *vram, int ox, int oy, int slide_x, unsigned int bg) {
    const PageSurface *s = &page_surface;
    int base_x = (s->width  < SCREEN_WIDTH)  ? (SCREEN_WIDTH  - s->width)  / 2 : 0;
    int base_y = (s->height < SCREEN_HEIGHT) ? (SCREEN_HEIGHT - s->height) / 2 : 0;
    int dst_x = base_x - ox + slide_x;
    int dst_y = base_y - oy;
    int x0 = dst_x < 0 ? 0 : dst_x;
    int y0 = dst_y < 0 ? 0 : dst_y;
    int x1 = dst_x + s->width;
    int y1 = dst_y + s->height;
    int span;

    if (x1 > SCREEN_WIDTH)  x1 = SCREEN_WIDTH;
    if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;

    if (x1 <= x0 || y1 <= y0) {
        draw_rect_solid(vram, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, bg);
        return;
    }

    if (y0 > 0) draw_rect_solid(vram, 0, 0, SCREEN_WIDTH, y0, bg);
    if (y1 < SCREEN_HEIGHT) draw_rect_solid(vram, 0, y1, SCREEN_WIDTH, SCREEN_HEIGHT - y1, bg);

    span = x1 - x0;
    for (int y = y0; y < y1; y++) {
        unsigned int *dst_row = vram + y * BUFFER_WIDTH;
        for (int x = 0; x < x0; x++) dst_row[x] = bg;
        for (int x = x1; x < SCREEN_WIDTH; x++) dst_row[x] = bg;
        memcpy(dst_row + x0, s->pixels + (y - dst_y) * s->width + (x0 - dst_x), (size_t)span * 4);
    }
}

/* Fallback for pages too large to buffer: samples straight from the source,
   using the same geometry as the surface path. */
void render_page_direct(unsigned int *vram, int ox, int oy, int slide_x, unsigned int bg) {
    int src_x0, src_y0, src_w, src_h, render_w, render_h, rotated, out_w, out_h, base_x, base_y;
    const unsigned int *src = (const unsigned int *)current_image.data;

    page_source_rect(&src_x0, &src_y0, &src_w, &src_h);
    page_render_size(src_w, src_h, &render_w, &render_h);
    rotated = (config.view_mode == VIEW_ROTATE_90);
    out_w = rotated ? render_h : render_w;
    out_h = rotated ? render_w : render_h;
    base_x = (out_w < SCREEN_WIDTH)  ? (SCREEN_WIDTH  - out_w)  / 2 : 0;
    base_y = (out_h < SCREEN_HEIGHT) ? (SCREEN_HEIGHT - out_h) / 2 : 0;

    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        unsigned int *dst_row = vram + y * BUFFER_WIDTH;
        int sy_out = y - base_y + oy;

        for (int x = 0; x < SCREEN_WIDTH; x++) {
            int sx_out = x - base_x + ox - slide_x;
            int rx, ry, sx, sy;

            if (sx_out < 0 || sx_out >= out_w || sy_out < 0 || sy_out >= out_h) {
                dst_row[x] = bg;
                continue;
            }

            rx = rotated ? sy_out : sx_out;
            ry = rotated ? (render_h - 1 - sx_out) : sy_out;
            sx = src_x0 + (rx * src_w) / render_w;
            sy = src_y0 + (ry * src_h) / render_h;
            if (sx >= current_image.width)  sx = current_image.width - 1;
            if (sy >= current_image.height) sy = current_image.height - 1;

            dst_row[x] = src[sy * current_image.width + sx];
        }
    }
}

/* ----------------------------------------------------------------- screens */

void draw_statusbar(unsigned int *vram, const char *title) {
    ScePspDateTime time;
    char clock_str[16];
    char batt_str[24];
    int bat_percent, bat_time, batt_x, clock_x, title_x;

    draw_rect_solid(vram, 0, 0, SCREEN_WIDTH, STATUSBAR_H - 1, COL_PANEL);
    draw_rect_solid(vram, 0, STATUSBAR_H - 1, SCREEN_WIDTH, 1, COL_LINE);

    sceRtcGetCurrentClockLocalTime(&time);
    bat_percent = scePowerGetBatteryLifePercent();
    bat_time = scePowerGetBatteryLifeTime();

    snprintf(clock_str, sizeof(clock_str), "%02d:%02d", time.hour, time.minute);
    if (bat_time > 0) {
        snprintf(batt_str, sizeof(batt_str), "%dh%02dm %d%%", bat_time / 60, bat_time % 60, bat_percent);
    } else {
        snprintf(batt_str, sizeof(batt_str), "%d%%", bat_percent);
    }

    batt_x = SCREEN_WIDTH - 6 - text_width(batt_str);
    draw_text(vram, batt_x, 4, batt_str, bat_percent <= 20 ? COL_WARN : COL_FG_DIM);

    clock_x = batt_x - 12 - text_width(clock_str);
    draw_text(vram, clock_x, 4, clock_str, COL_FG);

    draw_text(vram, 6, 4, APP_NAME, COL_ACCENT);
    title_x = 6 + text_width(APP_NAME) + 10;
    if (title && *title) {
        draw_rect_solid(vram, title_x - 6, 3, 1, 9, COL_LINE);
        draw_text_clipped(vram, title_x, 4, title, COL_FG_DIM, clock_x - title_x - 10);
    }
}

/* Modal chrome. `p` is the open progress, 0..256: the panel eases up from 82%
   height and the backdrop dim and content opacity ride the same curve. */
static void modal_shell(unsigned int *vram, int w, int h, int p, int *out_x, int *out_y) {
    int panel_h = (h * (225 + (31 * p) / 256)) / 256;
    int panel_x = (SCREEN_WIDTH - w) / 2;
    int panel_y = (SCREEN_HEIGHT - panel_h) / 2;

    draw_rect_dimmed(vram, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, (theme.scrim * p) / 256);
    draw_panel(vram, panel_x, panel_y, w, panel_h);

    *out_x = panel_x;
    *out_y = (SCREEN_HEIGHT - h) / 2;
}

static void modal_title(unsigned int *vram, int x, int w, int y, const char *text, int alpha) {
    draw_text_blend(vram, x + (w - (int)strlen(text) * 16) / 2, y, text, COL_FG, alpha);
    draw_rect_blend(vram, x + 20, y + 22, w - 40, 1, COL_LINE, alpha);
}

void render_help_modal(unsigned int *vram, int p) {
    static const char *keys[]   = { "ANALOG", "D-PAD", "L / R", NULL, NULL, NULL, "START", "SELECT" };
    static const char  glyphs[] = { 0, 0, 0, 'S', 'O', 'T', 0, 0 };
    static const char *acts[]   = { "Scroll and pan the page",
                                    "Fast scroll",
                                    "Previous / next page",
                                    "Toggle magnifier",
                                    "Cycle view mode",
                                    "Close volume",
                                    "Options",
                                    "Close this help" };
    const int w = 404;
    const int h = 200;
    int x, y, row_y;

    modal_shell(vram, w, h, p, &x, &y);
    modal_title(vram, x, w, y + 14, "CONTROLS", p);

    row_y = y + 50;
    for (int i = 0; i < 8; i++) {
        int label_x = x + 26;
        if (glyphs[i]) {
            draw_ps_glyph(vram, label_x, row_y, glyphs[i], COL_ACCENT);
        } else {
            draw_text_blend(vram, label_x, row_y, keys[i], COL_ACCENT, p);
        }
        draw_text_blend(vram, x + 128, row_y, acts[i], COL_FG_DIM, p);
        row_y += 18;
    }
}

void render_settings_modal(unsigned int *vram, int p) {
    static const char *labels[SETTINGS_ROWS] = {
        "Theme", "View mode", "Page background", "Reading direction", "Auto-crop",
        "Page transitions"
    };
    static const char *descriptions[SETTINGS_ROWS] = {
        "Choose the interface color palette.",
        "Choose how pages fit the PSP screen.",
        "Color beside pages that do not fill the screen.",
        "Choose which side advances to the next page.",
        "Trim plain scan margins before fitting the page.",
        "Animate page changes or switch instantly."
    };
    const char *values[SETTINGS_ROWS];
    const int w = 420;
    const int h = 230;
    int x, y, row_y;

    values[0] = theme_presets[config.theme_index].name;
    values[1] = (config.view_mode == VIEW_FIT_WIDTH) ? "Fit width" :
                (config.view_mode == VIEW_FIT_SCREEN) ? "Fit screen" : "Rotate 90";
    values[2] = (config.bg_mode == BG_AUTO) ? "Auto" :
                (config.bg_mode == BG_FORCE_BLACK) ? "Black" : "White";
    values[3] = (config.read_direction == READ_MANGA_RTL) ? "Manga R-L" : "Western L-R";
    values[4] = config.auto_crop ? "On" : "Off";
    values[5] = config.page_transitions ? "On" : "Off";

    modal_shell(vram, w, h, p, &x, &y);
    modal_title(vram, x, w, y + 14, "OPTIONS", p);

    row_y = y + 50;
    for (int i = 0; i < SETTINGS_ROWS; i++) {
        int selected = (settings_cursor == i);
        int value_x = x + w - 34 - text_width(values[i]);

        if (selected && p > 128) {
            draw_rect_blend(vram, x + 14, row_y - 5, w - 28, 18, COL_SEL_PANEL, p);
            draw_rect_blend(vram, x + 14, row_y - 5, 2, 18, COL_ACCENT, p);
            draw_text_blend(vram, value_x - 16, row_y, "<", COL_FG_MUTED, p);
            draw_text_blend(vram, x + w - 26, row_y, ">", COL_FG_MUTED, p);
        }

        draw_text_blend(vram, x + 26, row_y, labels[i], selected ? COL_FG : COL_FG_DIM, p);
        draw_text_blend(vram, value_x, row_y, values[i], selected ? COL_FG : COL_FG_DIM, p);
        row_y += 24;
    }

    draw_info_icon_blend(vram, x + 16, y + h - 43, p);
    draw_text_blend(vram, x + 36, y + h - 40, descriptions[settings_cursor], COL_FG_DIM, p);
    draw_text_blend(vram, x + (w - text_width("LEFT/RIGHT change   START closes")) / 2, y + h - 22,
                    "LEFT/RIGHT change   START closes", COL_FG_MUTED, p);
}

void render_theme_picker_modal(unsigned int *vram, int p) {
    static const char keyboard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ<";
    static const char *filter_names[] = { "ALL", "DARK", "LIGHT" };
    const int w = 420;
    const int h = 238;
    int x, y;

    modal_shell(vram, w, h, p, &x, &y);
    modal_title(vram, x, w, y + 10, "THEMES", p);
    draw_text_blend(vram, x + 20, y + 38, "FILTER", COL_FG_MUTED, p);
    draw_text_blend(vram, x + 76, y + 38, filter_names[theme_filter], COL_ACCENT, p);
    draw_text_blend(vram, x + 178, y + 38, "SEARCH", COL_FG_MUTED, p);
    draw_text_blend(vram, x + 242, y + 38, theme_search[0] ? theme_search : "(NONE)", COL_FG, p);

    if (theme_search_active) {
        draw_text_blend(vram, x + 20, y + 64, "CHOOSE LETTERS - SELECT WHEN DONE", COL_FG_DIM, p);
        for (int i = 0; i < 27; i++) {
            int col = i % 9;
            int row = i / 9;
            int key_x = x + 55 + col * 34;
            int key_y = y + 94 + row * 34;
            char label[2] = { keyboard[i], 0 };
            if (i == theme_keyboard_cursor) {
                draw_rect_blend(vram, key_x - 9, key_y - 7, 26, 22, COL_SEL_PANEL, p);
                draw_frame(vram, key_x - 9, key_y - 7, 26, 22, COL_ACCENT);
            }
            draw_text_blend(vram, key_x, key_y, label, i == theme_keyboard_cursor ? COL_FG : COL_FG_DIM, p);
        }
        draw_text_blend(vram, x + 20, y + h - 24, "X ADD   SQUARE DELETE   SELECT DONE", COL_FG_MUTED, p);
    } else {
        int count = theme_filtered_count();
        int first = theme_picker_row - 3;
        if (first < 0) first = 0;
        if (first > count - 8) first = count - 8;
        if (first < 0) first = 0;

        if (count == 0) {
            draw_text_blend(vram, x + 20, y + 94, "NO MATCHING THEMES", COL_FG_DIM, p);
        }
        for (int row = 0; row < 8 && first + row < count; row++) {
            int result_row = first + row;
            int index = theme_filtered_index(result_row);
            int row_y = y + 62 + row * 18;
            const char *kind = theme_presets[index].is_light ? "LIGHT" : "DARK";
            if (result_row == theme_picker_row) {
                draw_rect_blend(vram, x + 14, row_y - 5, w - 28, 17, COL_SEL_PANEL, p);
                draw_rect_blend(vram, x + 14, row_y - 5, 2, 17, COL_ACCENT, p);
            }
            draw_text_blend(vram, x + 24, row_y, theme_presets[index].name,
                            result_row == theme_picker_row ? COL_FG : COL_FG_DIM, p);
            draw_text_blend(vram, x + w - 24 - text_width(kind), row_y, kind, COL_FG_MUTED, p);
        }
        draw_text_blend(vram, x + 20, y + h - 24,
                        "X APPLY   SELECT SEARCH   L/R FILTER   O BACK", COL_FG_MUTED, p);
    }
}

static void draw_scrollbar(unsigned int *vram, int x, int y, int track_h,
                           int count, int visible, int selected) {
    int knob_h, knob_y;

    if (count <= 1 || count <= visible) return;
    knob_h = track_h / 4;
    if (knob_h < 10) knob_h = 10;
    knob_y = y + (selected * (track_h - knob_h)) / (count - 1);

    draw_rect_solid(vram, x, y, 3, track_h, COL_LINE);
    draw_rect_solid(vram, x, knob_y, 3, knob_h, COL_ACCENT);
}

static int series_progress_percent(const MangaSeries *series) {
    int page_percent;
    int percent;
    if (!series->has_progress || series->chapter_count <= 0 || series->progress_total_pages <= 0) return -1;
    page_percent = ((series->progress_page_index + 1) * 100) / series->progress_total_pages;
    if (page_percent > 100) page_percent = 100;
    percent = (series->progress_chapter_index * 100 + page_percent) / series->chapter_count;
    if (percent > 100) percent = 100;
    return percent;
}

static void draw_series_progress_bar(unsigned int *vram, const MangaSeries *series,
                                     int x, int y, int w) {
    int percent = series_progress_percent(series);
    if (percent < 0 || w < 2) return;
    draw_rect_solid(vram, x, y, w, 3, COL_LINE);
    draw_rect_solid(vram, x, y, (w * percent) / 100, 3, COL_ACCENT);
}

void render_library_screen(unsigned int *vram) {
    HintBar bar;
    char counter[24];
    char status_title[96];

    draw_rect_solid(vram, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG);

    if (series_count == 0) {
        char err_str[MAX_PATH_LEN + 32];
        draw_text_2x(vram, 20, 100, "NO MANGA FOUND", COL_FG);
        snprintf(err_str, sizeof(err_str), "Looked in: %s", mangas_dir);
        draw_text_clipped(vram, 20, 128, err_str, COL_FG_DIM, SCREEN_WIDTH - 40);
        draw_text(vram, 20, 146, "One folder per series, .cbz files inside.", COL_FG_MUTED);
        draw_text(vram, 20, 164, "Press SELECT to rescan.", COL_FG_MUTED);
    } else if (lib_view == LIB_VIEW_GRID) {
        int scroll = lib_grid_row_offset * GRID_ROW_PITCH;
        int cur_x = GRID_START_X + (lib_selected_index % GRID_COLS) * (GRID_THUMB_W + GRID_GAP_X);
        int cur_y = GRID_START_Y + (lib_selected_index / GRID_COLS) * GRID_ROW_PITCH - scroll;

        draw_rect_solid(vram, cur_x - 3, cur_y - 3, GRID_THUMB_W + 6, GRID_THUMB_H + 6, COL_SEL_BG);
        draw_frame(vram, cur_x - 3, cur_y - 3, GRID_THUMB_W + 6, GRID_THUMB_H + 6, COL_ACCENT);

        for (int i = 0; i < series_count; i++) {
            int col = i % GRID_COLS;
            int row = i / GRID_COLS;
            int x = GRID_START_X + col * (GRID_THUMB_W + GRID_GAP_X);
            int y = GRID_START_Y + row * GRID_ROW_PITCH - scroll;

            if (y + GRID_THUMB_H + GRID_LABEL_H < STATUSBAR_H || y > HINTBAR_Y) continue;

            draw_image_scaled(vram, &library[i].cover_thumb, x, y, GRID_THUMB_W, GRID_THUMB_H);
            draw_series_progress_bar(vram, &library[i], x, y + GRID_THUMB_H - 3, GRID_THUMB_W);
            /* Series names are primary content, so they never drop to the
               muted step -- that tier is for counts and hints. */
            draw_text_clipped(vram, x, y + GRID_THUMB_H + 6, library[i].name,
                              i == lib_selected_index ? COL_FG : COL_FG_DIM, GRID_THUMB_W);
        }
        draw_scrollbar(vram, SCREEN_WIDTH - 10, LIST_TOP,
                       GRID_ROWS * GRID_ROW_PITCH - GRID_GAP_Y,
                       series_count, GRID_COLS * GRID_ROWS, lib_selected_index);
    } else if (lib_view == LIB_VIEW_COVER_LIST) {
        int cursor_y = LIST_TOP + (lib_selected_index - lib_list_offset) * COVER_LIST_ROW_H;

        draw_selection_row(vram, 8, cursor_y, SCREEN_WIDTH - 24, COVER_LIST_ROW_H - 2);
        for (int i = 0; i < COVER_LIST_VISIBLE; i++) {
            int idx = lib_list_offset + i;
            int y = LIST_TOP + i * COVER_LIST_ROW_H;
            char count_str[32];
            if (idx >= series_count) break;
            draw_image_scaled(vram, &library[idx].cover_thumb, 16, y + 2,
                              COVER_LIST_THUMB_W, COVER_LIST_THUMB_H);
            draw_text_clipped(vram, 48, y + 7, library[idx].name,
                              idx == lib_selected_index ? COL_FG : COL_FG_DIM, SCREEN_WIDTH - 118);
            int percent = series_progress_percent(&library[idx]);
            if (percent >= 0) snprintf(count_str, sizeof(count_str), "%d volumes  %d%%", library[idx].chapter_count, percent);
            else snprintf(count_str, sizeof(count_str), "%d volumes", library[idx].chapter_count);
            draw_text(vram, 48, y + 21, count_str,
                      idx == lib_selected_index ? COL_FG_DIM : COL_FG_MUTED);
        }
        draw_scrollbar(vram, SCREEN_WIDTH - 10, LIST_TOP, COVER_LIST_VISIBLE * COVER_LIST_ROW_H,
                       series_count, COVER_LIST_VISIBLE, lib_selected_index);
    } else {
        int cursor_y = tween_value(&lib_cursor_y);

        draw_selection_row(vram, 8, cursor_y, SCREEN_WIDTH - 24, LIST_ROW_H - 2);

        for (int i = 0; i < LIST_VISIBLE; i++) {
            int idx = lib_list_offset + i;
            int y = LIST_TOP + i * LIST_ROW_H;
            char count_str[24];

            if (idx >= series_count) break;

            draw_text_clipped(vram, 16, y + 3, library[idx].name,
                              idx == lib_selected_index ? COL_FG : COL_FG_DIM, SCREEN_WIDTH - 90);
            int percent = series_progress_percent(&library[idx]);
            if (percent >= 0) snprintf(count_str, sizeof(count_str), "%dv %d%%", library[idx].chapter_count, percent);
            else snprintf(count_str, sizeof(count_str), "%dv", library[idx].chapter_count);
            draw_text(vram, SCREEN_WIDTH - 26 - text_width(count_str), y + 3, count_str,
                      idx == lib_selected_index ? COL_FG : COL_FG_MUTED);
        }

        draw_scrollbar(vram, SCREEN_WIDTH - 10, LIST_TOP, LIST_VISIBLE * LIST_ROW_H,
                       series_count, LIST_VISIBLE, lib_selected_index);
    }

    /* Chrome last: it doubles as the clip for rows sliding in and out. */
    snprintf(status_title, sizeof(status_title), "Library");
    if (series_count > 0 && library[lib_selected_index].has_progress) {
        MangaSeries *selected = &library[lib_selected_index];
        int percent = series_progress_percent(selected);
        if (percent >= 0) {
            snprintf(status_title, sizeof(status_title), "Vol %d/%d  Page %d/%d  %d%%",
                     selected->progress_chapter_index + 1, selected->chapter_count,
                     selected->progress_page_index + 1, selected->progress_total_pages, percent);
        } else {
            snprintf(status_title, sizeof(status_title), "Vol %d/%d  Page %d",
                     selected->progress_chapter_index + 1, selected->chapter_count,
                     selected->progress_page_index + 1);
        }
    }
    draw_statusbar(vram, status_title);

    draw_rect_solid(vram, 0, HINTBAR_Y, SCREEN_WIDTH, HINTBAR_H, COL_PANEL);
    draw_rect_solid(vram, 0, HINTBAR_Y, SCREEN_WIDTH, 1, COL_LINE);

    hint_bar_begin(&bar, vram, HINTBAR_Y + 4);
    hint_bar_item(&bar, "X", NULL, "Open");
    hint_bar_item(&bar, "S", NULL, "View");
    hint_bar_item(&bar, "O", NULL, "Resume");
    hint_bar_item(&bar, NULL, "SELECT", "Scan");
    hint_bar_item(&bar, NULL, "START", "Options");

    if (series_count > 0) {
        snprintf(counter, sizeof(counter), "%d/%d", lib_selected_index + 1, series_count);
        draw_text(vram, SCREEN_WIDTH - 8 - text_width(counter), HINTBAR_Y + 4, counter, COL_FG_MUTED);
    }
}

void render_series_detail_screen(unsigned int *vram) {
    MangaSeries *s = &library[lib_selected_index];
    HintBar bar;
    const int cover_w = 108;
    const int cover_h = 153;
    const int list_x = 140;
    int cursor_y = tween_value(&chapter_cursor_y);
    char counter[24];

    draw_rect_solid(vram, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG);

    draw_image_scaled(vram, &s->cover_thumb, 16, 26, cover_w, cover_h);
    draw_frame(vram, 15, 25, cover_w + 2, cover_h + 2, COL_LINE);

    draw_text_clipped(vram, 16, 26 + cover_h + 8, s->name, COL_FG, cover_w + 4);
    snprintf(counter, sizeof(counter), "%d volume%s", s->chapter_count, s->chapter_count == 1 ? "" : "s");
    draw_text(vram, 16, 26 + cover_h + 22, counter, COL_FG_MUTED);

    if (s->chapter_count > 0) {
        draw_selection_row(vram, list_x, cursor_y, SCREEN_WIDTH - list_x - 20, LIST_ROW_H - 2);

        for (int i = 0; i < LIST_VISIBLE; i++) {
            int idx = chapter_list_offset + i;
            int y = LIST_TOP + i * LIST_ROW_H;
            char saved_position[24];

            if (idx >= s->chapter_count) break;

            draw_text_clipped(vram, list_x + 8, y + 3, s->chapter_files[idx],
                              idx == chapter_selected_index ? COL_FG : COL_FG_DIM,
                              (s->has_progress && idx == s->progress_chapter_index)
                                  ? SCREEN_WIDTH - list_x - 104
                                  : SCREEN_WIDTH - list_x - 34);
            if (s->has_progress && idx == s->progress_chapter_index) {
                if (s->progress_total_pages > 0) {
                    snprintf(saved_position, sizeof(saved_position), "P%d/%d",
                             s->progress_page_index + 1, s->progress_total_pages);
                } else {
                    snprintf(saved_position, sizeof(saved_position), "P%d", s->progress_page_index + 1);
                }
                draw_text(vram, SCREEN_WIDTH - 20 - text_width(saved_position), y + 3,
                          saved_position, idx == chapter_selected_index ? COL_ACCENT : COL_FG_MUTED);
            }
        }

        draw_scrollbar(vram, SCREEN_WIDTH - 10, LIST_TOP, LIST_VISIBLE * LIST_ROW_H,
                       s->chapter_count, LIST_VISIBLE, chapter_selected_index);
    } else {
        draw_text(vram, list_x, 120, "No volumes in this folder.", COL_FG_MUTED);
    }

    draw_statusbar(vram, s->name);

    draw_rect_solid(vram, 0, HINTBAR_Y, SCREEN_WIDTH, HINTBAR_H, COL_PANEL);
    draw_rect_solid(vram, 0, HINTBAR_Y, SCREEN_WIDTH, 1, COL_LINE);

    hint_bar_begin(&bar, vram, HINTBAR_Y + 4);
    hint_bar_item(&bar, "X", NULL, "Read");
    hint_bar_item(&bar, "T", NULL, "Library");
    hint_bar_item(&bar, "UD", NULL, "Browse");
    hint_bar_item(&bar, NULL, "START", "Options");

    if (s->chapter_count > 0) {
        snprintf(counter, sizeof(counter), "%d/%d", chapter_selected_index + 1, s->chapter_count);
        draw_text(vram, SCREEN_WIDTH - 8 - text_width(counter), HINTBAR_Y + 4, counter, COL_FG_MUTED);
    }
}

void render_reader_screen(unsigned int *vram) {
    unsigned int active_bg = (config.bg_mode == BG_FORCE_BLACK) ? 0xFF000000U :
                             (config.bg_mode == BG_FORCE_WHITE) ? 0xFFFFFFFFU : detected_bg_color;
    int ox, oy, slide;

    if (!current_image.data) {
        draw_rect_solid(vram, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COL_BG);
        if (loading_active) {
            draw_busy_spinner(vram, SPINNER_X, SPINNER_Y, loading_phase);
        } else {
            draw_text_2x(vram, (SCREEN_WIDTH - 14 * 16) / 2, 112, "PAGE UNREADABLE", COL_FG);
            draw_text(vram, (SCREEN_WIDTH - text_width("L / R to try another page")) / 2, 142,
                      "L / R to try another page", COL_FG_DIM);
        }
        return;
    }

    ox = is_magnified ? mag_offset_x : 0;
    oy = is_magnified ? mag_offset_y : scroll_y;
    slide = tween_value(&page_slide);

    if (page_surface.valid) {
        blit_page_surface(vram, ox, oy, slide, active_bg);
    } else {
        render_page_direct(vram, ox, oy, slide, active_bg);
    }

    /* Always-on reading position. Two pixels, so it never competes with art. */
    if (current_comic.total_pages > 0 && modal_kind == MODAL_NONE) {
        int filled = ((current_comic.current_page_index + 1) * SCREEN_WIDTH) / current_comic.total_pages;
        draw_rect_dimmed(vram, 0, SCREEN_HEIGHT - 2, SCREEN_WIDTH, 2, 110);
        draw_rect_solid(vram, 0, SCREEN_HEIGHT - 2, filled, 2, COL_ACCENT);
    }

    if (hud_display_frames > 0 && modal_kind == MODAL_NONE) {
        int alpha = hud_display_frames >= HUD_FADE_FRAMES
                    ? 256 : (hud_display_frames * 256) / HUD_FADE_FRAMES;
        const char *mode = (config.view_mode == VIEW_FIT_WIDTH) ? "FIT WIDTH" :
                           (config.view_mode == VIEW_FIT_SCREEN) ? "FIT SCREEN" : "ROTATED";
        char osd[32];
        int w;

        snprintf(osd, sizeof(osd), "%d / %d", current_comic.current_page_index + 1,
                 current_comic.total_pages);
        w = text_width(osd) + 16;
        draw_rect_blend(vram, SCREEN_WIDTH - w - 10, 10, w, 18, 0xFF000000U, (alpha * 190) >> 8);
        draw_text_blend(vram, SCREEN_WIDTH - w - 2, 15, osd, COL_FG, alpha);

        w = text_width(mode) + (is_magnified ? text_width(" ZOOM") : 0) + 16;
        draw_rect_blend(vram, 10, 10, w, 18, 0xFF000000U, (alpha * 190) >> 8);
        draw_text_blend(vram, 18, 15, mode, COL_FG_DIM, alpha);
        if (is_magnified) {
            draw_text_blend(vram, 18 + text_width(mode) + 8, 15, "ZOOM", COL_ACCENT, alpha);
        }
    }

    if (loading_active) {
        draw_busy_spinner(vram, SPINNER_X, SPINNER_Y, loading_phase);
    }

    if (modal_kind == MODAL_NONE && page_load_error) {
        int w = 300;
        int x = (SCREEN_WIDTH - w) / 2;
        draw_panel(vram, x, 110, w, 52);
        draw_text(vram, x + (w - text_width("PAGE COULD NOT BE LOADED")) / 2, 124,
                  "PAGE COULD NOT BE LOADED", COL_FG);
        draw_text(vram, x + (w - text_width("Showing previous page")) / 2, 140,
                  "Showing previous page", COL_FG_DIM);
    }
}

/* One entry point for the flip, so a modal can float above any screen. */
void render_current_screen(unsigned int *vram) {
    int modal_p = tween_value(&modal_progress);

    if (state == STATE_LIBRARY) {
        render_library_screen(vram);
    } else if (state == STATE_SERIES_DETAIL) {
        render_series_detail_screen(vram);
    } else {
        render_reader_screen(vram);
    }

    if (modal_kind == MODAL_HELP) {
        render_help_modal(vram, modal_p);
    } else if (modal_kind == MODAL_SETTINGS) {
        render_settings_modal(vram, modal_p);
    } else if (modal_kind == MODAL_THEME_PICKER) {
        render_theme_picker_modal(vram, modal_p);
    }
}

void select_series_for_path(const char *full_path) {
    for (int i = 0; i < series_count; i++) {
        size_t folder_len = strlen(library[i].folder_path);
        if (strncmp(full_path, library[i].folder_path, folder_len) == 0 &&
            full_path[folder_len] == '/') {
            lib_selected_index = i;
            const char *filename = full_path + folder_len + 1;
            for (int chapter = 0; chapter < library[i].chapter_count; chapter++) {
                if (strcmp(filename, library[i].chapter_files[chapter]) == 0) {
                    chapter_selected_index = chapter;
                    break;
                }
            }
            return;
        }
    }
}

void present_frame(void (*render_func)(unsigned int *), int wait_vblank) {
    void *draw_cached = (current_buffer == 0) ? VRAM_CACHED_1 : VRAM_CACHED_0;
    void *draw_uncached = (current_buffer == 0) ? VRAM_UNCACHED_1 : VRAM_UNCACHED_0;

    render_func((unsigned int *)draw_cached);

    sceKernelDcacheWritebackRange(draw_cached, BUFFER_SIZE);
    sceDisplaySetFrameBuf(draw_uncached, BUFFER_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);

    /* The buffer just handed over is latched at the next vblank, so stepping to
       the other one immediately is safe even when we do not wait. */
    if (wait_vblank) sceDisplayWaitVblankStart();

    current_buffer = 1 - current_buffer;
}

void flip_display(void (*render_func)(unsigned int *)) {
    present_frame(render_func, 1);
}

int menu_repeat_buttons(unsigned int buttons, int pressed) {
    const unsigned int directions[4] = { PSP_CTRL_UP, PSP_CTRL_DOWN, PSP_CTRL_LEFT, PSP_CTRL_RIGHT };
    int repeated = pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
    int i;

    for (i = 0; i < 4; i++) {
        if (buttons & directions[i]) {
            int interval;
            menu_held_frames[i]++;
            interval = (menu_held_frames[i] >= MENU_REPEAT_FAST_AFTER)
                ? MENU_REPEAT_FAST_FRAMES
                : MENU_REPEAT_SLOW_FRAMES;

            if (menu_held_frames[i] >= MENU_REPEAT_DELAY_FRAMES &&
                ((menu_held_frames[i] - MENU_REPEAT_DELAY_FRAMES) % interval) == 0) {
                repeated |= directions[i];
            }
        } else {
            menu_held_frames[i] = 0;
        }
    }

    return repeated;
}

int main(int argc, char *argv[]) {
    boot_log_reset();
    boot_log("main() entered, argc=%d argv0=%s", argc, (argc > 0 && argv[0]) ? argv[0] : "(null)");

    setup_callbacks();
    boot_log("callbacks registered");

    scePowerSetClockFrequency(333, 333, 166);
    boot_log("clock set to 333/333/166");

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    boot_log("ctrl configured");

    sceDisplaySetMode(0, SCREEN_WIDTH, SCREEN_HEIGHT);
    /* NEXTFRAME, not IMMEDIATE: IMMEDIATE is PSP_DISPLAY_SETBUF_NEXTHSYNC, which
       silently fails to latch on some hardware/CFW, pinning the display to
       whatever buffer it was already showing. */
    sceDisplaySetFrameBuf(VRAM_UNCACHED_0, BUFFER_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    boot_marker(0xFF802000);  /* dark blue: display is up, main() is running */
    boot_log("display configured");
    display_probe();

    memset(base_dir, 0, sizeof(base_dir));
    if (getcwd(base_dir, sizeof(base_dir) - 1) == NULL || strlen(base_dir) == 0) {
        if (argc > 0 && argv[0] != NULL) {
            snprintf(base_dir, sizeof(base_dir), "%s", argv[0]);
            char *last_slash = strrchr(base_dir, '/');
            if (last_slash) *last_slash = '\0';
        }
    }

    if (strlen(base_dir) == 0) {
        snprintf(base_dir, sizeof(base_dir), ".");
    }

    snprintf(mangas_dir, sizeof(mangas_dir), "%s/mangas", base_dir);
    snprintf(cache_dir, sizeof(cache_dir), "%s/.thumbs", base_dir);
    snprintf(bookmarks_path, sizeof(bookmarks_path), "%s/bookmarks.dat", base_dir);
    snprintf(config_path, sizeof(config_path), "%s/settings.dat", base_dir);

    SceUID test_dfd = sceIoDopen(mangas_dir);
    if (test_dfd < 0) {
        snprintf(mangas_dir, sizeof(mangas_dir), "./mangas");
        snprintf(cache_dir, sizeof(cache_dir), "./.thumbs");
        snprintf(bookmarks_path, sizeof(bookmarks_path), "./bookmarks.dat");
        snprintf(config_path, sizeof(config_path), "./settings.dat");
    } else {
        sceIoDclose(test_dfd);
    }

    boot_log("base_dir=%s", base_dir);
    boot_log("mangas_dir=%s", mangas_dir);

    load_config();
    theme_apply();
    boot_log("config loaded: theme=%d view=%d", config.theme_index, (int)config.view_mode);

    load_last_read_target();
    boot_log("last read target loaded: %s", last_read_path);

    motion_init();
    theme_apply();

    boot_marker(0xFF008020);  /* dark green: about to scan the library */
    scan_mangas_library(mangas_dir);
    boot_log("library scanned, series_count=%d", series_count);

    SceCtrlData pad;
    SceCtrlData last_pad;
    memset(&last_pad, 0, sizeof(SceCtrlData));

    int needs_redraw = 1;
    int animating = 0;
    int snap_library_cursor = 1;
    int frames_traced = 0;
    (void)frames_traced;
    boot_log("entering main loop, state=%d", (int)state);

    while (1) {
        sceCtrlReadBufferPositive(&pad, 1);
        int pressed = pad.Buttons & ~last_pad.Buttons;
        int menu_pressed = menu_repeat_buttons(pad.Buttons, pressed);

        /* Options are reachable from every screen, so the modal gets first
           refusal on input before the screen underneath sees any of it. */
        if (pressed & PSP_CTRL_START) {
            if (modal_open && (modal_kind == MODAL_SETTINGS || modal_kind == MODAL_THEME_PICKER)) {
                save_config();
                modal_hide();
            } else if (!modal_open) {
                modal_show(MODAL_SETTINGS);
            }
            needs_redraw = 1;
        }

        if (state == STATE_READER && (pressed & PSP_CTRL_SELECT)) {
            if (modal_open && modal_kind == MODAL_HELP) modal_hide();
            else if (!modal_open) modal_show(MODAL_HELP);
            needs_redraw = 1;
        }

        if (modal_open) {
            if (modal_kind == MODAL_SETTINGS) {
                int step = 0;

                if (menu_pressed & PSP_CTRL_DOWN) { settings_cursor = (settings_cursor + 1) % SETTINGS_ROWS; needs_redraw = 1; }
                if (menu_pressed & PSP_CTRL_UP)   { settings_cursor = (settings_cursor - 1 + SETTINGS_ROWS) % SETTINGS_ROWS; needs_redraw = 1; }

                if (settings_cursor == 0 && (pressed & PSP_CTRL_CROSS)) {
                    theme_picker_row = 0;
                    theme_search_active = 0;
                    modal_show(MODAL_THEME_PICKER);
                    needs_redraw = 1;
                } else if (pressed & PSP_CTRL_RIGHT) step = 1;
                else if (pressed & PSP_CTRL_LEFT) step = -1;

                if (step) {
                    if (settings_cursor == 0) {
                        config.theme_index = (config.theme_index + THEME_COUNT + step) % THEME_COUNT;
                        theme_apply();
                    } else if (settings_cursor == 1) {
                        config.view_mode = (ViewMode)((config.view_mode + 3 + step) % 3);
                        is_magnified = 0;
                    } else if (settings_cursor == 2) {
                        config.bg_mode = (BgMode)((config.bg_mode + 3 + step) % 3);
                    } else if (settings_cursor == 3) {
                        config.read_direction = (ReadDirection)((config.read_direction + 2 + step) % 2);
                    } else if (settings_cursor == 4) {
                        config.auto_crop = !config.auto_crop;
                    } else {
                        config.page_transitions = !config.page_transitions;
                    }
                    needs_redraw = 1;
                }

                if (pressed & PSP_CTRL_CIRCLE) {
                    save_config();
                    modal_hide();
                    needs_redraw = 1;
                }
            } else if (modal_kind == MODAL_THEME_PICKER) {
                if (theme_search_active) {
                    int search_len = strlen(theme_search);
                    if (menu_pressed & PSP_CTRL_LEFT)  theme_keyboard_cursor--;
                    if (menu_pressed & PSP_CTRL_RIGHT) theme_keyboard_cursor++;
                    if (menu_pressed & PSP_CTRL_UP)    theme_keyboard_cursor -= 9;
                    if (menu_pressed & PSP_CTRL_DOWN)  theme_keyboard_cursor += 9;
                    while (theme_keyboard_cursor < 0) theme_keyboard_cursor += 27;
                    while (theme_keyboard_cursor >= 27) theme_keyboard_cursor -= 27;

                    if (pressed & PSP_CTRL_CROSS) {
                        if (theme_keyboard_cursor == 26) {
                            if (search_len > 0) theme_search[search_len - 1] = '\0';
                        } else if (search_len < (int)sizeof(theme_search) - 1) {
                            theme_search[search_len] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[theme_keyboard_cursor];
                            theme_search[search_len + 1] = '\0';
                        }
                        theme_picker_row = 0;
                    }
                    if ((pressed & PSP_CTRL_SQUARE) && search_len > 0) {
                        theme_search[search_len - 1] = '\0';
                        theme_picker_row = 0;
                    }
                    if (pressed & PSP_CTRL_SELECT) theme_search_active = 0;
                    if (pressed & PSP_CTRL_CIRCLE) theme_search_active = 0;
                    needs_redraw = 1;
                } else {
                    int count;
                    if (menu_pressed & PSP_CTRL_UP) theme_picker_row--;
                    if (menu_pressed & PSP_CTRL_DOWN) theme_picker_row++;
                    if (pressed & PSP_CTRL_LEFT) {
                        theme_filter = (theme_filter + 2) % 3;
                        theme_picker_row = 0;
                    }
                    if (pressed & PSP_CTRL_RIGHT) {
                        theme_filter = (theme_filter + 1) % 3;
                        theme_picker_row = 0;
                    }
                    if (pressed & PSP_CTRL_SQUARE) {
                        theme_search[0] = '\0';
                        theme_picker_row = 0;
                    }
                    count = theme_filtered_count();
                    if (theme_picker_row < 0) theme_picker_row = 0;
                    if (theme_picker_row >= count) theme_picker_row = count > 0 ? count - 1 : 0;

                    if (pressed & PSP_CTRL_SELECT) {
                        theme_search_active = 1;
                        theme_keyboard_cursor = 0;
                    }
                    if ((pressed & PSP_CTRL_CROSS) && count > 0) {
                        int index = theme_filtered_index(theme_picker_row);
                        if (index >= 0) {
                            config.theme_index = index;
                            theme_apply();
                            save_config();
                            modal_show(MODAL_SETTINGS);
                        }
                    }
                    if (pressed & PSP_CTRL_CIRCLE) modal_show(MODAL_SETTINGS);
                    needs_redraw = 1;
                }
            }
        }
        else if (state == STATE_LIBRARY) {
            if (pressed & PSP_CTRL_SQUARE) {
                lib_view = (LibraryViewMode)(((int)lib_view + 1) % 3);
                snap_library_cursor = 1;
                save_config();
                needs_redraw = 1;
            }

            if (pressed & PSP_CTRL_SELECT) {
                scan_mangas_library(mangas_dir);
                if (lib_selected_index >= series_count) lib_selected_index = 0;
                snap_library_cursor = 1;
                needs_redraw = 1;
            }

            if (pressed & PSP_CTRL_CIRCLE) {
                if (strlen(last_read_path) > 0) {
                    char *slash = strrchr(last_read_path, '/');
                    if (slash) {
                        snprintf(current_comic.current_file, sizeof(current_comic.current_file), "%s", slash + 1);
                    }
                    if (load_cbz_metadata(last_read_path)) {
                        select_series_for_path(last_read_path);
                        state = STATE_READER;
                        modal_kind = MODAL_NONE;
                        modal_open = 0;
                        tween_snap(&modal_progress, 0);
                        tween_snap(&page_slide, 0);
                        is_magnified = 0;
                        load_current_page();
                        needs_redraw = 1;
                    }
                }
            }

            if (series_count > 0) {
                int last = series_count - 1;

                if (lib_view == LIB_VIEW_GRID) {
                    if (menu_pressed & PSP_CTRL_RIGHT) { lib_selected_index++; needs_redraw = 1; }
                    if (menu_pressed & PSP_CTRL_LEFT)  { lib_selected_index--; needs_redraw = 1; }
                    if (menu_pressed & PSP_CTRL_DOWN)  { lib_selected_index += GRID_COLS; needs_redraw = 1; }
                    if (menu_pressed & PSP_CTRL_UP)    { lib_selected_index -= GRID_COLS; needs_redraw = 1; }
                } else {
                    if (menu_pressed & PSP_CTRL_DOWN) { lib_selected_index++; needs_redraw = 1; }
                    if (menu_pressed & PSP_CTRL_UP)   { lib_selected_index--; needs_redraw = 1; }
                    int visible = (lib_view == LIB_VIEW_COVER_LIST) ? COVER_LIST_VISIBLE : LIST_VISIBLE;
                    if (pressed & PSP_CTRL_RTRIGGER)  { lib_selected_index += visible; needs_redraw = 1; }
                    if (pressed & PSP_CTRL_LTRIGGER)  { lib_selected_index -= visible; needs_redraw = 1; }
                }

                if (lib_selected_index < 0) lib_selected_index = 0;
                if (lib_selected_index > last) lib_selected_index = last;

                if (pressed & PSP_CTRL_CROSS) {
                    state = STATE_SERIES_DETAIL;
                    chapter_selected_index = 0;
                    chapter_list_offset = 0;
                    tween_snap(&chapter_cursor_y, LIST_TOP);
                    needs_redraw = 1;
                }
            }
        }
        else if (state == STATE_SERIES_DETAIL) {
            MangaSeries *s = &library[lib_selected_index];

            if (pressed & PSP_CTRL_TRIANGLE) {
                state = STATE_LIBRARY;
                snap_library_cursor = 1;
                needs_redraw = 1;
            }

            if (s->chapter_count > 0) {
                int last = s->chapter_count - 1;

                if (menu_pressed & PSP_CTRL_DOWN) { chapter_selected_index++; needs_redraw = 1; }
                if (menu_pressed & PSP_CTRL_UP)   { chapter_selected_index--; needs_redraw = 1; }
                if (pressed & PSP_CTRL_RTRIGGER)  { chapter_selected_index += LIST_VISIBLE; needs_redraw = 1; }
                if (pressed & PSP_CTRL_LTRIGGER)  { chapter_selected_index -= LIST_VISIBLE; needs_redraw = 1; }

                if (chapter_selected_index < 0) chapter_selected_index = 0;
                if (chapter_selected_index > last) chapter_selected_index = last;

                if (pressed & PSP_CTRL_CROSS) {
                    char full_path[MAX_PATH_LEN];
                    snprintf(full_path, sizeof(full_path), "%s/%s", s->folder_path, s->chapter_files[chapter_selected_index]);
                    snprintf(current_comic.current_file, sizeof(current_comic.current_file), "%s", s->chapter_files[chapter_selected_index]);

                    if (load_cbz_metadata(full_path)) {
                        state = STATE_READER;
                        modal_kind = MODAL_NONE;
                        modal_open = 0;
                        tween_snap(&modal_progress, 0);
                        tween_snap(&page_slide, 0);
                        is_magnified = 0;
                        load_current_page();
                        needs_redraw = 1;
                    }
                }
            }
        }
        else if (state == STATE_READER) {
            {
                int analog_x = (int)pad.Lx - 128;
                int analog_y = (int)pad.Ly - 128;
                int go_next = 0;
                int go_prev = 0;

                if (pressed & PSP_CTRL_SQUARE) {
                    is_magnified = !is_magnified;
                    hud_display_frames = HUD_HOLD_FRAMES + HUD_FADE_FRAMES;
                    needs_redraw = 1;
                }

                if (pressed & PSP_CTRL_CIRCLE) {
                    config.view_mode = (ViewMode)((config.view_mode + 1) % 3);
                    is_magnified = 0;
                    hud_display_frames = HUD_HOLD_FRAMES + HUD_FADE_FRAMES;
                    needs_redraw = 1;
                }

                if (pressed & PSP_CTRL_LTRIGGER) go_prev = 1;
                if (pressed & PSP_CTRL_RTRIGGER) go_next = 1;

                if (config.view_mode == VIEW_ROTATE_90) {
                    if (pressed & PSP_CTRL_UP) go_next = 1;
                    if (pressed & PSP_CTRL_DOWN) go_prev = 1;
                } else if (config.read_direction == READ_MANGA_RTL) {
                    if (pressed & PSP_CTRL_LEFT) go_next = 1;
                    if (pressed & PSP_CTRL_RIGHT) go_prev = 1;
                } else {
                    if (pressed & PSP_CTRL_RIGHT) go_next = 1;
                    if (pressed & PSP_CTRL_LEFT) go_prev = 1;
                }

                if (go_next || go_prev) {
                    int delta = go_next ? 1 : -1;
                    int target = current_comic.current_page_index + delta;

                    if (target >= 0 && target < current_comic.total_pages) {
                        int previous_index = current_comic.current_page_index;
                        current_comic.current_page_index = target;

                        if (!load_current_page()) {
                            current_comic.current_page_index = previous_index;
                            /* The low-memory loader releases the previous page
                               before decoding. Restore it if the target page is bad. */
                            load_current_page();
                        } else {
                            /* The new page enters from the side the reader
                               turned towards, so direction matches the binding. */
                            if (config.page_transitions) {
                                int from_right = (config.read_direction == READ_MANGA_RTL) ? go_prev : go_next;
                                tween_snap(&page_slide, from_right ? SCREEN_WIDTH : -SCREEN_WIDTH);
                                tween_set(&page_slide, 0);
                            } else {
                                tween_snap(&page_slide, 0);
                            }
                        }
                        needs_redraw = 1;
                    }
                }

                if (is_magnified) {
                    if (analog_x > 30 || analog_x < -30) { mag_offset_x += (analog_x * 14) / 128; needs_redraw = 1; }
                    if (analog_y > 30 || analog_y < -30) { mag_offset_y += (analog_y * 14) / 128; needs_redraw = 1; }
                    scroll_vel_q8 = 0;
                } else {
                    /* Velocity, not position: releasing the stick coasts to a
                       stop instead of stopping dead. */
                    int drive = 0;

                    if (analog_y > 30 || analog_y < -30) {
                        drive = (analog_y * SCROLL_MAX_VEL_Q8) / 128;
                    } else if (config.view_mode != VIEW_ROTATE_90) {
                        if (pad.Buttons & PSP_CTRL_DOWN) drive = SCROLL_MAX_VEL_Q8;
                        if (pad.Buttons & PSP_CTRL_UP)   drive = -SCROLL_MAX_VEL_Q8;
                    }

                    if (drive) {
                        scroll_vel_q8 = drive;
                    } else {
                        scroll_vel_q8 = (scroll_vel_q8 * SCROLL_FRICTION_N) / SCROLL_FRICTION_D;
                        if (scroll_vel_q8 > -96 && scroll_vel_q8 < 96) scroll_vel_q8 = 0;
                    }
                }

                if (pressed & PSP_CTRL_TRIANGLE) {
                    save_current_progress();
                    page_surface_free();
                    if (current_image.data) {
                        stbi_image_free(current_image.data);
                        current_image.data = NULL;
                    }
                    hud_display_frames = 0;
                    is_magnified = 0;
                    scroll_vel_q8 = 0;
                    state = STATE_SERIES_DETAIL;
                    needs_redraw = 1;
                }
            }
        }

        /* ---- update: surface, clamping, motion ------------------------- */

        if (state == STATE_READER && current_image.data) {
            int max_x, max_y;

            page_surface_sync();
            page_view_extent(&max_x, &max_y);

            if (is_magnified) {
                if (mag_offset_x > max_x) mag_offset_x = max_x;
                if (mag_offset_x < 0) mag_offset_x = 0;
                if (mag_offset_y > max_y) mag_offset_y = max_y;
                if (mag_offset_y < 0) mag_offset_y = 0;
            } else if (scroll_vel_q8 || scroll_y > max_y) {
                scroll_y_q8 += scroll_vel_q8;
                if (scroll_y_q8 < 0) { scroll_y_q8 = 0; scroll_vel_q8 = 0; }
                if (scroll_y_q8 > (max_y << 8)) { scroll_y_q8 = max_y << 8; scroll_vel_q8 = 0; }
                scroll_y = scroll_y_q8 >> 8;
                needs_redraw = 1;
            }
        }

        if (state == STATE_LIBRARY && series_count > 0) {
            if (lib_view == LIB_VIEW_GRID) {
                int rows = (series_count + GRID_COLS - 1) / GRID_COLS;
                lib_grid_row_offset = clamp_window(lib_selected_index / GRID_COLS, rows,
                                                   GRID_ROWS, 0, lib_grid_row_offset);
            } else {
                int visible = (lib_view == LIB_VIEW_COVER_LIST) ? COVER_LIST_VISIBLE : LIST_VISIBLE;
                int row_h = (lib_view == LIB_VIEW_COVER_LIST) ? COVER_LIST_ROW_H : LIST_ROW_H;
                lib_list_offset = clamp_window(lib_selected_index, series_count, visible, 2, lib_list_offset);
                tween_set(&lib_cursor_y, LIST_TOP + (lib_selected_index - lib_list_offset) * row_h);
                if (snap_library_cursor || lib_view == LIB_VIEW_COVER_LIST) tween_snap(&lib_cursor_y, lib_cursor_y.target);
            }
            snap_library_cursor = 0;
        }

        if (state == STATE_SERIES_DETAIL && library[lib_selected_index].chapter_count > 0) {
            int count = library[lib_selected_index].chapter_count;
            chapter_list_offset = clamp_window(chapter_selected_index, count, LIST_VISIBLE, 2, chapter_list_offset);
            tween_set(&chapter_cursor_y, LIST_TOP + (chapter_selected_index - chapter_list_offset) * LIST_ROW_H);
        }

        animating  = tween_step(&lib_cursor_y);
        animating |= tween_step(&chapter_cursor_y);
        animating |= tween_step(&modal_progress);
        animating |= tween_step(&page_slide);

        if (!modal_open && !modal_progress.active && tween_value(&modal_progress) == 0) {
            modal_kind = MODAL_NONE;
        }

        if (state == STATE_READER && hud_display_frames > 0) {
            hud_display_frames--;
            animating = 1;
        }

        if (animating) needs_redraw = 1;

        if (needs_redraw) {
            if (frames_traced < 3) {
                boot_log("render #%d state=%d buffer=%d", frames_traced, (int)state, current_buffer);
                frames_traced++;
            }
            flip_display(render_current_screen);
            needs_redraw = 0;
        } else {
            /* flip_display already waits for vblank; match that when idle so
               one loop iteration is always one 60 Hz frame. Key repeat and
               every tween are timed in those frames. */
            sceDisplayWaitVblankStart();
        }

        last_pad = pad;
    }

    sceKernelExitGame();
    return 0;
}
