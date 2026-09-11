#include "render.h"
#include "font.h"
#include "common/i18n.h"
#include "stb_image.h"   /* decls only; impl lives in banner.c */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

/* Runtime panel geometry (defaults = SF3000 854x480; picoarch overrides via env). */
int g_screen_w  = 854;
int g_screen_h  = 480;
int g_ui_scale  = 150;

void render_set_geometry(int w, int h, int ui_scale) {
    if (w > 0)        g_screen_w = w;
    if (h > 0)        g_screen_h = h;
    if (ui_scale > 0) g_ui_scale = ui_scale;
}

void render_init(uint16_t *framebuffer) {
    if (framebuffer) {
        render_clear_screen(framebuffer);
    }
}

void render_clear_screen(uint16_t *framebuffer) {
    if (!framebuffer) return;
    
    // Fill with background color
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        framebuffer[i] = COLOR_BG;
    }
}

void render_fill_rect(uint16_t *framebuffer, int x, int y, int width, int height, uint16_t color) {
    if (!framebuffer) return;
    
    for (int py = y; py < y + height && py < SCREEN_HEIGHT; py++) {
        for (int px = x; px < x + width && px < SCREEN_WIDTH; px++) {
            if (px >= 0 && py >= 0) {
                framebuffer[py * SCREEN_WIDTH + px] = color;
            }
        }
    }
}

void render_rounded_rect(uint16_t *framebuffer, int x, int y, int width, int height, int radius, uint16_t color) {
    if (!framebuffer) return;
    
    // Draw main body (excluding corners)
    render_fill_rect(framebuffer, x + radius, y, width - 2 * radius, height, color);
    render_fill_rect(framebuffer, x, y + radius, width, height - 2 * radius, color);
    
    // Draw rounded corners using circle approximation
    for (int corner_y = 0; corner_y < radius; corner_y++) {
        for (int corner_x = 0; corner_x < radius; corner_x++) {
            int dx = radius - corner_x;
            int dy = radius - corner_y;
            int dist_sq = dx * dx + dy * dy;
            int radius_sq = radius * radius;
            
            if (dist_sq <= radius_sq) {
                // Top-left corner
                int px = x + corner_x;
                int py = y + corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = color;
                }
                
                // Top-right corner
                px = x + width - 1 - corner_x;
                py = y + corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = color;
                }
                
                // Bottom-left corner
                px = x + corner_x;
                py = y + height - 1 - corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = color;
                }
                
                // Bottom-right corner
                px = x + width - 1 - corner_x;
                py = y + height - 1 - corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = color;
                }
            }
        }
    }
}

void render_text_pillbox(uint16_t *framebuffer, int x, int y, const char *text,
                        uint16_t bg_color, uint16_t text_color, int padding) {
    if (!framebuffer || !text) return;

    int baseline, cap_h;
    font_cap_metrics(&baseline, &cap_h);
    int text_width = font_measure_text(text);

    int pad_x = padding;
    int pad_y = padding;

    // Total box height based on actual cap height
    int pillbox_width = text_width + (pad_x * 2);
    int pillbox_height = cap_h + (pad_y * 2);

    int pillbox_x = x - pad_x;
    // Offset pillbox background to bound text vertically
    int pillbox_y = y + (baseline - cap_h) - pad_y;

    render_rounded_rect(framebuffer, pillbox_x, pillbox_y, pillbox_width, pillbox_height, 6, bg_color);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, x, y, text, text_color);
}

void render_battery_colors(uint16_t *framebuffer, int pct,
                           uint16_t bg_color, uint16_t accent_color) {
    if (!framebuffer || pct < 0) return;
    if (pct > 100) pct = 100;
    extern int frogui_battery_charging(void);
    extern int frogui_battery_color_mode(void);
    int charging = frogui_battery_charging();

    /* Same battery glyph in both modes. "Battery Colour Mode" just changes the
     * fill colour to a level band: green 70-100, blue 30-70, red 0-30 (charging
     * = green). Normal mode: accent fill, red under 15%. */
    int color_mode = frogui_battery_color_mode();

    /* NextUI-style: rounded body + a little terminal nub, fill proportional. */
    int bw = UI_S(26), bh = UI_S(13);        /* body */
    int nub_w = UI_S(3), nub_h = bh / 2;
    int pad = UI_S(2);
    int x = SCREEN_WIDTH - PADDING - bw - nub_w;
    int y = 10;                               /* header baseline area */

    uint16_t outline = accent_color;
    uint16_t green   = 0x2FE6;                /* charging fill */
    uint16_t fillcol;
    if (color_mode)
        fillcol = charging ? green : (pct >= 70) ? green : (pct >= 30) ? 0x041F : 0xF800;
    else
        fillcol = charging ? green : ((pct <= 15) ? 0xF800 /*red*/ : accent_color);

    /* body outline (rounded), hollow */
    int r = UI_S(3);
    render_rounded_rect(framebuffer, x, y, bw, bh, r, outline);
    render_rounded_rect(framebuffer, x + 1, y + 1, bw - 2, bh - 2, r - 1, bg_color);
    /* terminal nub */
    render_fill_rect(framebuffer, x + bw, y + (bh - nub_h) / 2, nub_w, nub_h, outline);
    /* fill proportional to pct */
    int inner_w = bw - 2 * pad;
    int fw = inner_w * pct / 100; if (fw < 0) fw = 0; if (fw > inner_w) fw = inner_w;
    if (fw > 0)
        render_fill_rect(framebuffer, x + pad, y + pad, fw, bh - 2 * pad, fillcol);

    /* Charging bolt: a small lightning glyph centered on the body. */
    if (charging) {
        int cx = x + bw / 2, cy = y + bh / 2;
        uint16_t bolt = 0xFFFF;
        int s = UI_S(1); if (s < 1) s = 1;
        /* two offset triangles forming a zig-zag bolt (cheap raster) */
        for (int i = -bh/2 + pad; i < bh/2 - pad; i++) {
            int w = (i < 0) ? (bh/2 + i) : (bh/2 - i);   /* taper */
            int off = (i < 0) ? -s : s;
            for (int j = -s; j <= s; j++)
                render_fill_rect(framebuffer, cx + off + j, cy + i, 1, 1, bolt);
            (void)w;
        }
    }
}

void render_battery(uint16_t *framebuffer, int pct) {
    render_battery_colors(framebuffer, pct, COLOR_BG, COLOR_HEADER);
}

void render_header(uint16_t *framebuffer, const char *title) {
    if (!framebuffer || !title) return;

    // Draw folder/section name in header area
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 10, title, COLOR_HEADER);

    // Battery indicator, top-right (custom icon; fb1 masking is disabled for test).
    { extern int frogui_battery_pct(void); render_battery(framebuffer, frogui_battery_pct()); }
}

void render_tabs(uint16_t *framebuffer, int active, uint16_t header_bg) {
    static const char *keys[] = { "tab.recents", "tab.games", "tab.apps", "tab.settings" };
    extern int frogui_battery_pct(void);
    if (!framebuffer) return;

    int x = PADDING;
    int y = UI_S(5);
    int h = ITEM_HEIGHT - UI_S(5);
    int gap = UI_S(8);
    for (int i = 0; i < 4; i++) {
        int pad = UI_S(7);
        const char *label = tr(keys[i]);
        int tw = font_measure_text(label);
        int w = tw + pad * 2;
        int baseline, cap_h;
        font_cap_metrics(&baseline, &cap_h);
        int ty = y + h / 2 - (baseline - cap_h / 2);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       x + pad, ty, label,
                       i == active ? COLOR_TEXT : COLOR_DISABLED);
        x += w + gap;
    }
    render_battery_colors(framebuffer, frogui_battery_pct(), header_bg, COLOR_HEADER);
}

void render_legend(uint16_t *framebuffer, int x_button_mode, int show_select, int show_search) {
    if (!framebuffer) return;

    int pill_h   = ITEM_HEIGHT - UI_S(4);
    int legend_y = SCREEN_HEIGHT - pill_h - UI_S(6);
    int baseline, cap_h;
    font_cap_metrics(&baseline, &cap_h);
    int text_y   = legend_y + pill_h / 2 - (baseline - cap_h / 2);
    int spacing  = UI_S(8);
    int anchor = SCREEN_WIDTH - PADDING;

    {
        const char *nav = tr("legend.enter_back");
        int w = font_measure_text(nav);
        int x = anchor - w;
        render_rounded_rect(framebuffer, x - 4, legend_y, w + 8, pill_h,
                            UI_S(8), COLOR_LEGEND_BG);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       x, text_y, nav, COLOR_LEGEND);
        anchor = x - spacing;
    }

    if (x_button_mode != LEGEND_X_NONE) {
        const char *xl = tr(x_button_mode == LEGEND_X_REMOVE ? "legend.unfavourite" : "legend.favourite");
        int w = font_measure_text(xl);
        int x = anchor - w;
        render_rounded_rect(framebuffer, x - 4, legend_y, w + 8, pill_h,
                            UI_S(8), COLOR_LEGEND_BG);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       x, text_y, xl, COLOR_LEGEND);
        anchor = x - spacing;
    }

    if (show_select) {
        const char *sl = tr("legend.options");
        int w = font_measure_text(sl);
        int x = anchor - w;
        render_rounded_rect(framebuffer, x - 4, legend_y, w + 8, pill_h,
                            UI_S(8), COLOR_LEGEND_BG);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       x, text_y, sl, COLOR_LEGEND);
        anchor = x - spacing;
    }

    if (show_search) {
        const char *xs = tr("legend.search");
        int w = font_measure_text(xs);
        int x = anchor - w;
        render_rounded_rect(framebuffer, x - 4, legend_y, w + 8, pill_h,
                            UI_S(8), COLOR_LEGEND_BG);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       x, text_y, xs, COLOR_LEGEND);
    }
}

void render_scroll_indicator(uint16_t *framebuffer, int total, int selected, int visible) {
    if (!framebuffer || total <= visible || visible <= 0) return;
    int top = START_Y;
    int bottom = SCREEN_HEIGHT - ITEM_HEIGHT - UI_S(10);
    int h = bottom - top;
    if (h < UI_S(20)) return;
    int w = UI_S(3); if (w < 2) w = 2;
    int x = SCREEN_WIDTH - UI_S(7) - w;
    render_rounded_rect(framebuffer, x, top, w, h, w / 2, COLOR_LEGEND_BG);
    int thumb_h = h * visible / total;
    if (thumb_h < UI_S(12)) thumb_h = UI_S(12);
    if (thumb_h > h) thumb_h = h;
    int range = h - thumb_h;
    int y = top + (total > 1 ? range * selected / (total - 1) : 0);
    render_rounded_rect(framebuffer, x, y, w, thumb_h, w / 2, COLOR_SELECT_BG);
}

void render_toast(uint16_t *framebuffer, const char *text) {
    if (!framebuffer || !text || !*text) return;
    int pad = UI_S(10);
    int w = font_measure_text(text) + pad * 2;
    int max_w = SCREEN_WIDTH - PADDING * 2;
    if (w > max_w) w = max_w;
    int h = ITEM_HEIGHT - UI_S(3);
    int x = (SCREEN_WIDTH - w) / 2;
    int y = SCREEN_HEIGHT - ITEM_HEIGHT * 2 - UI_S(8);
    render_rounded_rect(framebuffer, x, y, w, h, UI_S(8), COLOR_SELECT_BG);
    int baseline, cap_h;
    font_cap_metrics(&baseline, &cap_h);
    int ty = y + h / 2 - (baseline - cap_h / 2);
    int tx = (SCREEN_WIDTH - font_measure_text(text)) / 2;
    if (tx < x + UI_S(4)) tx = x + UI_S(4);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                   tx, ty, text, COLOR_SELECT_TEXT);
}

/* Draw one row at an explicit pixel y (used by the animated list). */
void render_menu_row(uint16_t *framebuffer, const char *name, int is_dir,
                     int is_selected, int is_favorited, int y) {
    if (!framebuffer || !name) return;

    int text_x = PADDING;
    if (is_favorited) {
        const char *star = "*";
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, star, COLOR_HEADER);
        text_x = PADDING + 15;
    }

    if (is_selected) {
        render_text_pillbox(framebuffer, text_x, y, name, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
    } else {
        uint16_t text_color = is_dir ? COLOR_FOLDER : COLOR_DISABLED;
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, text_x, y, name, text_color);
    }
}

void render_menu_item(uint16_t *framebuffer, int index, const char *name, int is_dir,
                     int is_selected, int scroll_offset, int is_favorited) {
    if (!framebuffer || !name) return;

    int visible_index = index - scroll_offset;
    if (visible_index < 0 || visible_index >= VISIBLE_ENTRIES) return;

    render_menu_row(framebuffer, name, is_dir, is_selected, is_favorited,
                    START_Y + visible_index * ITEM_HEIGHT);
}

void render_menu_item_centered(uint16_t *framebuffer, int index, const char *name,
                               int is_dir, int is_selected, int scroll_offset) {
    if (!framebuffer || !name) return;

    int visible_index = index - scroll_offset;
    if (visible_index < 0 || visible_index >= VISIBLE_ENTRIES) return;

    int y = START_Y + visible_index * ITEM_HEIGHT;
    int text_x = (SCREEN_WIDTH - font_measure_text(name)) / 2;
    if (text_x < PADDING) text_x = PADDING;
    if (is_selected) {
        render_text_pillbox(framebuffer, text_x, y, name,
                            COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
    } else {
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, text_x, y, name,
                       is_dir ? COLOR_FOLDER : COLOR_DISABLED);
    }
}

// Thumbnail implementation

void get_thumbnail_path(const char *game_path, char *thumb_path, size_t thumb_path_size) {
    if (!game_path || !thumb_path || game_path[0] == '\0') {
        thumb_path[0] = '\0';
        return;
    }
    
    // Find the last slash to get directory
    const char *last_slash = strrchr(game_path, '/');
    if (!last_slash) {
        thumb_path[0] = '\0';
        return;
    }
    
    // Copy directory path
    size_t dir_len = last_slash - game_path;
    if (dir_len + 1 >= thumb_path_size) {
        thumb_path[0] = '\0';
        return;
    }
    
    strncpy(thumb_path, game_path, dir_len);
    thumb_path[dir_len] = '\0';
    
    // Add /.res/ subdirectory
    strncat(thumb_path, "/.res/", thumb_path_size - strlen(thumb_path) - 1);
    
    // Get filename without extension
    const char *filename = last_slash + 1;
    const char *last_dot = strrchr(filename, '.');
    
    if (last_dot) {
        size_t name_len = last_dot - filename;
        strncat(thumb_path, filename, min(name_len, thumb_path_size - strlen(thumb_path) - 1));
    } else {
        strncat(thumb_path, filename, thumb_path_size - strlen(thumb_path) - 1);
    }
    
    // No extension: load_thumbnail() probes .png/.jpg/.jpeg/.bmp then .rgb565.
}

static int artwork_base_path(const char *game_path, const char *folder,
                             const char *suffix, char *out, size_t out_size) {
    const char *slash, *filename, *dot;
    size_t dir_len, stem_len;
    if (!game_path || !folder || !suffix || !out || out_size == 0) return 0;
    slash = strrchr(game_path, '/');
    if (!slash) return 0;
    dir_len = (size_t)(slash - game_path);
    filename = slash + 1;
    dot = strrchr(filename, '.');
    stem_len = dot ? (size_t)(dot - filename) : strlen(filename);
    if (stem_len == 0 ||
        snprintf(out, out_size, "%.*s/%s/%.*s%s", (int)dir_len, game_path,
                 folder, (int)stem_len, filename, suffix) >= (int)out_size)
        return 0;
    return 1;
}

/* Persistent negative cache for missing artwork. Folder mtimes invalidate
 * entries when a scraper adds or replaces images. */
typedef struct {
    uint64_t game_hash;
    uint64_t folder_sig;
    uint8_t kind;
    uint8_t reserved[7];
} ArtworkMiss;
#define ARTWORK_MISS_FILE "/mnt/sdcard/frogui/.cache/artwork-misses.bin"
#define ARTWORK_MISS_MAX 4096
static ArtworkMiss artwork_misses[ARTWORK_MISS_MAX];
static size_t artwork_miss_count;
static int artwork_miss_loaded;

static uint64_t artwork_hash(const char *s) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = (h ^ *p) * UINT64_C(1099511628211);
    return h;
}

static uint64_t artwork_folder_signature(const char *game_path) {
    static const char *folders[] = { ".res", "Imgs", "images", "Images", NULL };
    const char *slash = game_path ? strrchr(game_path, '/') : NULL;
    if (!slash) return 0;
    size_t dir_len = (size_t)(slash - game_path);
    uint64_t sig = UINT64_C(1469598103934665603);
    char path[1024];
    for (int i = 0; folders[i]; i++) {
        struct stat st;
        int n = snprintf(path, sizeof path, "%.*s/%s", (int)dir_len, game_path, folders[i]);
        if (n > 0 && (size_t)n < sizeof path && stat(path, &st) == 0) {
            sig ^= (uint64_t)st.st_mtime + UINT64_C(0x9e3779b97f4a7c15) * (uint64_t)(i + 1);
            sig = (sig << 7) | (sig >> 57);
            sig ^= (uint64_t)st.st_size;
        } else {
            sig ^= UINT64_C(0x517cc1b727220a95) + (uint64_t)i;
            sig = (sig << 7) | (sig >> 57);
        }
    }
    return sig;
}

static void artwork_miss_load(void) {
    if (artwork_miss_loaded) return;
    artwork_miss_loaded = 1;
    FILE *f = fopen(ARTWORK_MISS_FILE, "rb");
    if (!f) return;
    artwork_miss_count = fread(artwork_misses, sizeof(ArtworkMiss), ARTWORK_MISS_MAX, f);
    fclose(f);
}

static int artwork_miss_contains(uint64_t game_hash, uint64_t folder_sig, ArtworkKind kind) {
    artwork_miss_load();
    for (size_t i = 0; i < artwork_miss_count; i++)
        if (artwork_misses[i].game_hash == game_hash &&
            artwork_misses[i].folder_sig == folder_sig &&
            artwork_misses[i].kind == (uint8_t)kind)
            return 1;
    return 0;
}

static void artwork_miss_add(uint64_t game_hash, uint64_t folder_sig, ArtworkKind kind) {
    artwork_miss_load();
    if (artwork_miss_contains(game_hash, folder_sig, kind)) return;
    ArtworkMiss miss = { game_hash, folder_sig, (uint8_t)kind, {0} };
    if (artwork_miss_count < ARTWORK_MISS_MAX)
        artwork_misses[artwork_miss_count++] = miss;
    else {
        memmove(artwork_misses, artwork_misses + 1,
                (ARTWORK_MISS_MAX - 1) * sizeof(ArtworkMiss));
        artwork_misses[ARTWORK_MISS_MAX - 1] = miss;
    }
    mkdir("/mnt/sdcard/frogui", 0755);
    mkdir("/mnt/sdcard/frogui/.cache", 0755);
    FILE *f = fopen(ARTWORK_MISS_FILE, "ab");
    if (f) { fwrite(&miss, sizeof miss, 1, f); fclose(f); }
}

int load_game_artwork(const char *game_path, ArtworkKind kind, Thumbnail *thumb) {
    static const char *folders[] = { ".res", "Imgs", "images", "Images", NULL };
    static const char *box_suffixes[] = { "", NULL };
    static const char *title_suffixes[] = {
        "-title", "_title", ".title", "-titlescreen", "_titlescreen",
        ".titlescreen", "-screenshot", "_screenshot", ".screenshot",
        "-screen", "_screen", "-preview", "_preview", NULL
    };
    const char **suffixes = kind == ARTWORK_TITLE_SCREEN ? title_suffixes : box_suffixes;
    char base[1024];
    if (!game_path || !thumb) return 0;
    uint64_t game_hash = artwork_hash(game_path);
    uint64_t folder_sig = artwork_folder_signature(game_path);
    if (artwork_miss_contains(game_hash, folder_sig, kind)) {
        free_thumbnail(thumb);
        return 0;
    }
    for (int f = 0; folders[f]; f++) {
        for (int s = 0; suffixes[s]; s++) {
            if (!artwork_base_path(game_path, folders[f], suffixes[s], base, sizeof base))
                continue;
            if (load_thumbnail(base, thumb)) return 1;
        }
    }
    artwork_miss_add(game_hash, folder_sig, kind);
    return 0;
}

static uint16_t rgb24_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Static buffers for thumbnail - no malloc/free hell
static uint16_t thumbnail_buffer[250 * 250]; // Max size: 250x250
static uint8_t  thumbnail_alpha[250 * 250];  // per-pixel alpha, composited at render
#define THUMB_BUF_W 250
#define THUMB_BUF_H 250

// Decode a JPG/PNG/BMP via stb_image and nearest-neighbor downscale into the
// static RGB565 buffer (aspect-preserving, capped at 250x250). Alpha is kept
// in a side buffer and composited per-pixel at render time, so transparent
// boxart shows whatever is actually behind it (theme/background image).
static int load_image_thumbnail(const char *path, Thumbnail *thumb) {
    int w, h, ch;
    unsigned char *img = stbi_load(path, &w, &h, &ch, 4);  // force RGBA
    if (!img) return 0;
    if (w <= 0 || h <= 0) { stbi_image_free(img); return 0; }

    int dw = w, dh = h;
    if (dw > THUMB_BUF_W) { dh = dh * THUMB_BUF_W / dw; dw = THUMB_BUF_W; }
    if (dh > THUMB_BUF_H) { dw = dw * THUMB_BUF_H / dh; dh = THUMB_BUF_H; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    for (int y = 0; y < dh; y++) {
        int sy = y * h / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * w / dw;
            const unsigned char *p = img + (sy * w + sx) * 4;
            thumbnail_buffer[y * dw + x] = rgb24_to_rgb565(p[0], p[1], p[2]);
            thumbnail_alpha[y * dw + x] = p[3];
        }
    }
    stbi_image_free(img);

    thumb->data = thumbnail_buffer;
    thumb->alpha = thumbnail_alpha;
    thumb->width = dw;
    thumb->height = dh;
    return 1;
}

int load_thumbnail(const char *base_path, Thumbnail *thumb) {
    if (!base_path || !thumb) return 0;
    thumb->data = NULL;
    thumb->alpha = NULL;
    thumb->width = 0;
    thumb->height = 0;

    char p[1024];
    // Decodable image formats first (easy to drop in), then raw fallback.
    static const char *exts[] = { ".png", ".jpg", ".jpeg", ".bmp", NULL };
    for (int i = 0; exts[i]; i++) {
        snprintf(p, sizeof p, "%s%s", base_path, exts[i]);
        if (access(p, F_OK) == 0 && load_image_thumbnail(p, thumb))
            return 1;
    }
    snprintf(p, sizeof p, "%s.rgb565", base_path);
    return load_raw_rgb565(p, thumb);
}

int load_raw_rgb565(const char *path, Thumbnail *thumb) {
    // Check if file exists
    if (access(path, F_OK) != 0) {
        return 0;
    }
    
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    
    // Try common dimensions - including 160x160 for the resized images
    int dimensions[][2] = {{64,64}, {128,128}, {160,160}, {200,200}, {250,200}, {200,250}};
    int num_dims = sizeof(dimensions) / sizeof(dimensions[0]);
    
    for (int i = 0; i < num_dims; i++) {
        int w = dimensions[i][0];
        int h = dimensions[i][1];
        if (w * h * 2 == file_size) {
            
            // Check if it fits in our static buffer
            if (w * h > sizeof(thumbnail_buffer) / 2) {
                fclose(fp);
                return 0;
            }
            
            thumb->width = w;
            thumb->height = h;
            thumb->data = thumbnail_buffer; // Use static buffer
            thumb->alpha = NULL;            // raw rgb565 has no alpha: opaque
            
            size_t read_bytes = fread(thumb->data, 1, file_size, fp);
            fclose(fp);
            
            if (read_bytes == file_size) {
                return 1;
            } else {
                return 0;
            }
        }
    }
    
    fclose(fp);
    return 0;
}

void free_thumbnail(Thumbnail *thumb) {
    if (thumb) {
        // No need to free static buffer, just reset pointer
        thumb->data = NULL;
    thumb->alpha = NULL;
        thumb->width = 0;
        thumb->height = 0;
    }
}

void render_thumbnail(uint16_t *framebuffer, const Thumbnail *thumb) {
    if (!framebuffer || !thumb || !thumb->data) {
        return;
    }
    
    // Calculate scaled dimensions to fit in thumbnail area
    int display_width = thumb->width;
    int display_height = thumb->height;
    
    // Scale down if too large
    if (display_width > THUMBNAIL_MAX_WIDTH) {
        display_height = (display_height * THUMBNAIL_MAX_WIDTH) / display_width;
        display_width = THUMBNAIL_MAX_WIDTH;
    }
    
    if (display_height > THUMBNAIL_MAX_HEIGHT) {
        display_width = (display_width * THUMBNAIL_MAX_HEIGHT) / display_height;
        display_height = THUMBNAIL_MAX_HEIGHT;
    }
    
    // Center in thumbnail area (vertically) and align to right edge
    int start_x = SCREEN_WIDTH - display_width;  // Align to right edge of screen
    
    // Center thumbnail vertically on screen
    int start_y = (SCREEN_HEIGHT - display_height) / 2;
    
    // No backing card/frame: the image composites straight onto whatever is
    // behind it (theme fill or background image) using its own alpha.
    for (int y = 0; y < display_height; y++) {
        for (int x = 0; x < display_width; x++) {
            int screen_x = start_x + x;
            int screen_y = start_y + y;

            if (screen_x >= 0 && screen_x < SCREEN_WIDTH &&
                screen_y >= 0 && screen_y < SCREEN_HEIGHT) {

                // Simple scaling - map display coords to source coords
                int src_x = (x * thumb->width) / display_width;
                int src_y = (y * thumb->height) / display_height;

                if (src_x < thumb->width && src_y < thumb->height) {
                    int si = src_y * thumb->width + src_x;
                    unsigned a = thumb->alpha ? thumb->alpha[si] : 255;
                    if (a == 0) continue;
                    uint16_t pixel = thumb->data[si];
                    uint16_t *dst = &framebuffer[screen_y * SCREEN_WIDTH + screen_x];
                    if (a == 255) {
                        *dst = pixel;
                    } else {
                        // blend RGB565 src over dst by alpha
                        unsigned sr = (pixel >> 11) & 0x1F, sg = (pixel >> 5) & 0x3F, sb = pixel & 0x1F;
                        unsigned dr = (*dst  >> 11) & 0x1F, dg = (*dst  >> 5) & 0x3F, db = *dst & 0x1F;
                        unsigned r = (sr * a + dr * (255 - a)) / 255;
                        unsigned g = (sg * a + dg * (255 - a)) / 255;
                        unsigned b = (sb * a + db * (255 - a)) / 255;
                        *dst = (uint16_t)((r << 11) | (g << 5) | b);
                    }
                }
            }
        }
    }
}
