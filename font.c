#include "font.h"
#include "common/i18n.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb.h>

#include <SheenBidi/SheenBidi.h>

#ifndef UI_SCALE
#define UI_SCALE 100
#endif
#define DEFAULT_FONT_SIZE 26
static float ui_font_size = DEFAULT_FONT_SIZE * UI_SCALE / 100.0f;

#define GLYPH_CACHE_SIZE 4096
#define MAX_STACK_CODEPOINTS 512

// Memory Pool Constants
#define FONT_BUFFER_MAX_SIZE (24 * 1024 * 1024) // 24MB max font file size
#define GLYPH_PIXEL_POOL_SIZE                                                  \
  (2 * 1024 * 1024) // 2MB bump arena for glyph bitmaps

typedef struct {
  uint32_t glyph_index;
  int font_id;
  int width;
  int rows;
  int left;
  int top;
  uint8_t *bitmap;
} CachedGlyph;

typedef struct {
  FT_Face ft_face;
  hb_font_t *hb_font;
  uint8_t *buffer;
  size_t buffer_size;
  int loaded;
} FontFace;

// Static font buffers allocated in BSS to prevent runtime allocation
static uint8_t primary_font_bytes[FONT_BUFFER_MAX_SIZE];
static uint8_t fallback_font_bytes[FONT_BUFFER_MAX_SIZE];
static uint8_t latin_font_bytes[FONT_BUFFER_MAX_SIZE];

/* Static Glyph Bitmap Arena Allocator */
static uint8_t glyph_bitmap_arena[GLYPH_PIXEL_POOL_SIZE];
static size_t arena_offset = 0;

static FT_Library ft_library = NULL;
static FontFace primary_font = {.buffer = primary_font_bytes};
static FontFace fallback_font = {.buffer = fallback_font_bytes};
static FontFace latin_font = {.buffer = latin_font_bytes};

static hb_buffer_t *hb_buf = NULL;
static int language_force_font_id = 0;

static CachedGlyph glyph_cache[GLYPH_CACHE_SIZE];

// Clear Cache Entries & Reset Bump Arena
static void clear_glyph_cache(void) {
  for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
    glyph_cache[i].bitmap = NULL;
    glyph_cache[i].glyph_index = 0;
    glyph_cache[i].font_id = -1;
  }
  arena_offset = 0;
}

// Bitwise RGB565 Alpha Blending with branch-optimized early exit
static inline void font_blend_pixel_fast(uint16_t *dst, uint16_t color,
                                         uint32_t alpha) {
  if (alpha == 255) {
    *dst = color;
    return;
  }

  uint32_t bg = *dst;
  uint32_t inv_a = 255 - alpha;

  uint32_t fg_r = (color >> 11) & 0x1F;
  uint32_t fg_g = (color >> 5) & 0x3F;
  uint32_t fg_b = color & 0x1F;

  uint32_t bg_r = (bg >> 11) & 0x1F;
  uint32_t bg_g = (bg >> 5) & 0x3F;
  uint32_t bg_b = bg & 0x1F;

  uint32_t r = ((fg_r * alpha) + (bg_r * inv_a)) >> 8;
  uint32_t g = ((fg_g * alpha) + (bg_g * inv_a)) >> 8;
  uint32_t b = ((fg_b * alpha) + (bg_b * inv_a)) >> 8;

  *dst = (uint16_t)((r << 11) | (g << 5) | b);
}

static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t cp;
  if (s[0] < 0x80) {
    *p += 1;
    return s[0];
  }
  if ((s[0] & 0xe0) == 0xc0 && s[1]) {
    cp = s[0] & 0x1f;
    cp = (cp << 6) | (s[1] & 0x3f);
    *p += 2;
    return cp;
  }
  if ((s[0] & 0xf0) == 0xe0 && s[1] && s[2]) {
    cp = s[0] & 0x0f;
    cp = (cp << 6) | (s[1] & 0x3f);
    cp = (cp << 6) | (s[2] & 0x3f);
    *p += 3;
    return cp;
  }
  if ((s[0] & 0xf8) == 0xf0 && s[1] && s[2] && s[3]) {
    cp = s[0] & 7;
    cp = (cp << 6) | (s[1] & 0x3f);
    cp = (cp << 6) | (s[2] & 0x3f);
    cp = (cp << 6) | (s[3] & 0x3f);
    *p += 4;
    return cp;
  }
  *p += 1;
  return 0xfffd;
}

static uint32_t unicode_upper(uint32_t cp) {
  if (cp >= 'a' && cp <= 'z')
    return cp - 32;
  if (cp >= 0xE0 && cp <= 0xF6)
    return cp - 0x20;
  if (cp >= 0xF8 && cp <= 0xFE)
    return cp - 0x20;
  switch (cp) {
  case 0x0105:
    return 0x0104;
  case 0x0107:
    return 0x0106;
  case 0x0119:
    return 0x0118;
  case 0x0142:
    return 0x0141;
  case 0x0144:
    return 0x0143;
  case 0x015B:
    return 0x015A;
  case 0x017A:
    return 0x0179;
  case 0x017C:
    return 0x017B;
  default:
    return cp;
  }
}

// Arena allocation for rendered glyph bitmaps
static uint8_t *arena_alloc(size_t size) {
  if (arena_offset + size > GLYPH_PIXEL_POOL_SIZE) {
    /* Cache memory pressure hit: reset arena & clear cache */
    clear_glyph_cache();
  }

  uint8_t *ptr = &glyph_bitmap_arena[arena_offset];
  arena_offset += size;
  return ptr;
}

static CachedGlyph *get_cached_glyph(FontFace *face, uint32_t glyph_index,
                                     int font_id) {
  uint32_t hash =
      (glyph_index ^ ((uint32_t)font_id * 0x9e3779b9)) % GLYPH_CACHE_SIZE;

  if (glyph_cache[hash].glyph_index == glyph_index &&
      glyph_cache[hash].font_id == font_id && glyph_cache[hash].bitmap) {
    return &glyph_cache[hash];
  }

  if (FT_Load_Glyph(face->ft_face, glyph_index, FT_LOAD_RENDER)) {
    return NULL;
  }

  FT_Bitmap *bitmap = &face->ft_face->glyph->bitmap;

  glyph_cache[hash].glyph_index = glyph_index;
  glyph_cache[hash].font_id = font_id;
  glyph_cache[hash].width = bitmap->width;
  glyph_cache[hash].rows = bitmap->rows;
  glyph_cache[hash].left = face->ft_face->glyph->bitmap_left;
  glyph_cache[hash].top = face->ft_face->glyph->bitmap_top;

  size_t size = (size_t)bitmap->width * bitmap->rows;
  if (size > 0) {
    glyph_cache[hash].bitmap = arena_alloc(size);
    if (glyph_cache[hash].bitmap) {
      for (unsigned int r = 0; r < bitmap->rows; r++) {
        memcpy(glyph_cache[hash].bitmap + (r * bitmap->width),
               bitmap->buffer + (r * bitmap->pitch), bitmap->width);
      }
    }
  } else {
    glyph_cache[hash].bitmap = NULL;
  }

  return &glyph_cache[hash];
}

static void update_face_pixel_sizes(FontFace *face) {
  if (face && face->loaded && face->ft_face) {
    FT_Set_Pixel_Sizes(face->ft_face, 0, (FT_UInt)ui_font_size);
    // Ensure HarfBuzz scale matches FreeType's pixel size (26.6 fractional
    // units)
    hb_ft_font_changed(face->hb_font);
  }
}

void font_set_size(int pixels) {
  if (pixels < 18)
    pixels = 18;
  if (pixels > DEFAULT_FONT_SIZE)
    pixels = DEFAULT_FONT_SIZE;
  ui_font_size = pixels * UI_SCALE / 100.0f;

  update_face_pixel_sizes(&primary_font);
  update_face_pixel_sizes(&fallback_font);
  update_face_pixel_sizes(&latin_font);

  clear_glyph_cache();
}

/**
 * Utility function to retrieve the active base font size in points/pixels.
 * Accounts for UI_SCALE to reverse-calculate original pixel settings.
 */
int font_get_size(void) {
  return (int)(ui_font_size * 100.0f / UI_SCALE + 0.5f);
}

static void unload_font_face(FontFace *face) {
  if (face->hb_font) {
    hb_font_destroy(face->hb_font);
    face->hb_font = NULL;
  }
  if (face->ft_face) {
    FT_Done_Face(face->ft_face);
    face->ft_face = NULL;
  }
  face->loaded = 0;
  clear_glyph_cache();
}

static int load_font_face(FontFace *face, const char *paths[], int path_count) {
  FILE *fp = NULL;
  for (int i = 0; i < path_count; i++) {
    if (!paths[i])
      continue;
    fp = fopen(paths[i], "rb");
    if (fp)
      break;
  }
  if (!fp)
    return 0;

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (size <= 0 || size > FONT_BUFFER_MAX_SIZE) {
    fclose(fp);
    return 0;
  }

  unload_font_face(face);

  if (fread(face->buffer, 1, (size_t)size, fp) != (size_t)size) {
    fclose(fp);
    unload_font_face(face);
    return 0;
  }
  fclose(fp);
  face->buffer_size = (size_t)size;

  if (FT_New_Memory_Face(ft_library, face->buffer, (FT_Long)face->buffer_size,
                         0, &face->ft_face)) {
    unload_font_face(face);
    return 0;
  }

  FT_Set_Pixel_Sizes(face->ft_face, 0, (FT_UInt)ui_font_size);
  face->hb_font = hb_ft_font_create_referenced(face->ft_face);
  if (!face->hb_font) {
    unload_font_face(face);
    return 0;
  }

  face->loaded = 1;
  return 1;
}

static int load_font_file(const char *font_filename) {
  char font_paths[4][256];
  snprintf(font_paths[0], sizeof(font_paths[0]), "/mnt/sdcard/cubegm/fonts/%s",
           font_filename);
  snprintf(font_paths[1], sizeof(font_paths[1]), "/mnt/sdcard/frogui/fonts/%s",
           font_filename);
  snprintf(font_paths[2], sizeof(font_paths[2]), "/mnt/sdcard/frogui/fonts/%s",
           font_filename);
  snprintf(font_paths[3], sizeof(font_paths[3]), "fonts/%s", font_filename);

  const char *paths[4] = {font_paths[0], font_paths[1], font_paths[2],
                          font_paths[3]};
  return load_font_face(&primary_font, paths, 4);
}

static int load_fallback_font(void) {
  if (fallback_font.loaded)
    return 1;
  const char *paths[] = {"/mnt/sdcard/frogui/fonts/TreeFrogUnicode.ttf",
                         "/mnt/sdcard/cubegm/fonts/TreeFrogUnicode.ttf",
                         "fonts/TreeFrogUnicode.ttf"};
  return load_font_face(&fallback_font, paths, 3);
}

static int load_latin_fallback(void) {
  if (latin_font.loaded)
    return 1;
  const char *paths[] = {"/mnt/sdcard/frogui/fonts/TreeFrogLatin.ttf",
                         "/mnt/sdcard/cubegm/fonts/TreeFrogLatin.ttf",
                         "fonts/TreeFrogLatin.ttf"};
  return load_font_face(&latin_font, paths, 3);
}

void font_load_file(const char *font_filename) {
  if (!font_filename || !font_filename[0])
    return;
  load_font_file(font_filename);
}

void font_load_from_settings(const char *font_name) {
  const char *font_filename = NULL;
  if (strcmp(font_name, "Monogram") == 0) {
    font_filename = "monogram.ttf";
  } else if (strcmp(font_name, "GamePocket") == 0) {
    font_filename = "GamePocket-Regular-ZeroKern.ttf";
  } else {
    font_filename = "BPreplayBold.otf";
  }
  load_font_file(font_filename);
}

void font_init(void) {
  if (FT_Init_FreeType(&ft_library))
    return;
  hb_buf = hb_buffer_create();

  if (!load_font_file("BPreplayBold.otf")) {
    font_load_from_settings("GamePocket");
  }

  load_fallback_font();
  load_latin_fallback();
}

static inline FontFace *get_face_for_codepoint(uint32_t cp, int *out_font_id) {
  if (language_force_font_id == 1 && load_fallback_font()) {
    if (out_font_id)
      *out_font_id = 1;
    return &fallback_font;
  }
  if (language_force_font_id == 2 && load_latin_fallback()) {
    if (out_font_id)
      *out_font_id = 2;
    return &latin_font;
  }

  if (primary_font.loaded && FT_Get_Char_Index(primary_font.ft_face, cp)) {
    if (out_font_id)
      *out_font_id = 0;
    return &primary_font;
  }

  if (load_fallback_font() && FT_Get_Char_Index(fallback_font.ft_face, cp)) {
    if (out_font_id)
      *out_font_id = 1;
    return &fallback_font;
  }

  if (load_latin_fallback() && FT_Get_Char_Index(latin_font.ft_face, cp)) {
    if (out_font_id)
      *out_font_id = 2;
    return &latin_font;
  }

  if (primary_font.loaded) {
    if (out_font_id)
      *out_font_id = 0;
    return &primary_font;
  }

  if (out_font_id)
    *out_font_id = 1;
  return fallback_font.loaded ? &fallback_font : NULL;
}

// Stack & Arena Renderer Engine with per-codepoint CJK Fallback + Multi-line
// handling
static void shape_and_render_bidi_text(uint16_t *framebuffer, int screen_width,
                                       int screen_height, int x, int y,
                                       const char *text, uint16_t color,
                                       int measure_only, int *out_width) {
  if (!text || !*text) {
    if (out_width)
      *out_width = 0;
    return;
  }

  int start_x = x;
  int current_y = y;
  int max_width = 0;
  const char *line_start = text;

  while (*line_start) {
    SBUInt32 utf32_buf[MAX_STACK_CODEPOINTS];
    SBUInt32 codepoint_count = 0;
    const char *p = line_start;

    // Process characters until end of string or newline
    while (*p && *p != '\n' && codepoint_count < MAX_STACK_CODEPOINTS) {
      utf32_buf[codepoint_count++] = utf8_next(&p);
    }

    if (codepoint_count > 0) {
      SBCodepointSequence sequence = {.stringEncoding = SBStringEncodingUTF32,
                                      .stringBuffer = utf32_buf,
                                      .stringLength = codepoint_count};

      SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
      if (algorithm) {
        SBParagraphRef paragraph = SBAlgorithmCreateParagraph(
            algorithm, 0, codepoint_count, SBLevelDefaultLTR);
        if (paragraph) {
          SBUInt32 paragraph_length = SBParagraphGetLength(paragraph);
          SBLineRef line =
              SBParagraphCreateLine(paragraph, 0, paragraph_length);

          if (line) {
            SBUInt32 run_count = SBLineGetRunCount(line);
            const SBRun *runs = SBLineGetRunsPtr(line);

            int line_width = 0;
            int cursor_x = start_x;

            for (SBUInt32 r = 0; r < run_count; r++) {
              SBUInt32 run_offset = runs[r].offset;
              SBUInt32 run_len = runs[r].length;
              SBLevel run_level = runs[r].level;

              SBUInt32 sub_idx = 0;
              while (sub_idx < run_len) {
                int active_font_id = 0;
                FontFace *face = get_face_for_codepoint(
                    utf32_buf[run_offset + sub_idx], &active_font_id);

                if (!face || !face->loaded) {
                  sub_idx++;
                  continue;
                }

                SBUInt32 chunk_len = 0;
                while ((sub_idx + chunk_len) < run_len) {
                  int next_font_id = 0;
                  get_face_for_codepoint(
                      utf32_buf[run_offset + sub_idx + chunk_len],
                      &next_font_id);
                  if (next_font_id != active_font_id)
                    break;
                  chunk_len++;
                }

                hb_buffer_clear_contents(hb_buf);
                hb_buffer_add_utf32(
                    hb_buf, (const uint32_t *)&utf32_buf[run_offset + sub_idx],
                    chunk_len, 0, chunk_len);

                hb_buffer_set_direction(hb_buf, (run_level & 1)
                                                    ? HB_DIRECTION_RTL
                                                    : HB_DIRECTION_LTR);
                hb_buffer_guess_segment_properties(hb_buf);

                hb_shape(face->hb_font, hb_buf, NULL, 0);

                unsigned int glyph_count;
                hb_glyph_info_t *glyph_info =
                    hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
                hb_glyph_position_t *glyph_pos =
                    hb_buffer_get_glyph_positions(hb_buf, &glyph_count);

                int baseline = face->ft_face->size->metrics.ascender >> 6;
                int is_bold_fallback =
                    (language_force_font_id &&
                     active_font_id == language_force_font_id);

                for (unsigned int i = 0; i < glyph_count; i++) {
                  uint32_t glyph_index = glyph_info[i].codepoint;

                  // HarfBuzz positions are in 26.6 fractional pixels
                  int x_offset = glyph_pos[i].x_offset >> 6;
                  int y_offset = glyph_pos[i].y_offset >> 6;
                  int x_advance = glyph_pos[i].x_advance >> 6;

                  if (!measure_only && framebuffer) {
                    CachedGlyph *cg =
                        get_cached_glyph(face, glyph_index, active_font_id);
                    if (cg && cg->bitmap) {
                      // cg->left is in pixels from FreeType bitmap_left
                      int draw_x = cursor_x + cg->left + x_offset;
                      int draw_y = current_y + baseline - cg->top - y_offset;

                      for (int row = 0; row < cg->rows; row++) {
                        int py = draw_y + row;
                        if (py < 0 || py >= screen_height)
                          continue;

                        uint16_t *line_dst = &framebuffer[py * screen_width];
                        uint8_t *src_ptr = &cg->bitmap[row * cg->width];

                        for (int col = 0; col < cg->width; col++) {
                          uint8_t alpha = src_ptr[col];
                          if (alpha > 0) {
                            int px = draw_x + col;
                            if (px >= 0 && px < screen_width) {
                              font_blend_pixel_fast(&line_dst[px], color,
                                                    alpha);
                              if (is_bold_fallback && (px + 1) < screen_width) {
                                font_blend_pixel_fast(&line_dst[px + 1], color,
                                                      alpha);
                              }
                            }
                          }
                        }
                      }
                    }
                  }

                  // Advance cursor
                  cursor_x += x_advance;
                  line_width += x_advance;
                }

                sub_idx += chunk_len;
              }
            }

            if (line_width > max_width) {
              max_width = line_width;
            }

            SBLineRelease(line);
          }
          SBParagraphRelease(paragraph);
        }
        SBAlgorithmRelease(algorithm);
      }
    }

    line_start = p;
    if (*line_start == '\n') {
      line_start++;
      current_y += (int)ui_font_size + 4;
    }
  }

  if (out_width)
    *out_width = max_width;
}

void font_draw_char(uint16_t *framebuffer, int screen_width, int screen_height,
                    int x, int y, char c, uint16_t color) {
  char str[2] = {c, '\0'};
  font_draw_text(framebuffer, screen_width, screen_height, x, y, str, color);
}

void font_draw_text(uint16_t *framebuffer, int screen_width, int screen_height,
                    int x, int y, const char *text, uint16_t color) {
  shape_and_render_bidi_text(framebuffer, screen_width, screen_height, x, y,
                             text, color, 0, NULL);
}

int font_measure_text(const char *text) {
  int width = 0;
  shape_and_render_bidi_text(NULL, 0, 0, 0, 0, text, 0, 1, &width);
  return width;
}

void font_cap_metrics(int *baseline_out, int *cap_height_out) {
  int baseline = 0, cap = 0;
  if (primary_font.loaded) {
    FT_Size_Metrics *metrics = &primary_font.ft_face->size->metrics;
    baseline = metrics->ascender >> 6;

    FT_UInt gi = FT_Get_Char_Index(primary_font.ft_face, 'H');
    if (gi && !FT_Load_Glyph(primary_font.ft_face, gi, FT_LOAD_DEFAULT)) {
      cap = (int)(primary_font.ft_face->glyph->metrics.height >> 6);
    } else {
      cap = baseline;
    }
  }
  if (baseline_out)
    *baseline_out = baseline;
  if (cap_height_out)
    *cap_height_out = cap;
}

static int active_language_supported(FontFace *face) {
  if (!face || !face->loaded)
    return 0;
  char selected_key[32];
  snprintf(selected_key, sizeof(selected_key), "language.%s",
           i18n_current_language());
  for (int i = 0; i < i18n_value_count(); i++) {
    const char *key = i18n_key_at(i);
    const char *text = i18n_value_at(i);
    if (key && strncmp(key, "language.", 9) == 0 &&
        strcmp(key, selected_key) != 0)
      continue;
    for (const char *p = text; p && *p;) {
      uint32_t cp = unicode_upper(utf8_next(&p));
      if (cp >= 128 && !FT_Get_Char_Index(face->ft_face, cp))
        return 0;
    }
  }
  return 1;
}

void font_sync_language_fallback(void) {
  language_force_font_id = 0;
  if (!primary_font.loaded || active_language_supported(&primary_font))
    return;

  if (load_fallback_font() && active_language_supported(&fallback_font))
    language_force_font_id = 1;
  else if (load_latin_fallback() && active_language_supported(&latin_font))
    language_force_font_id = 2;
}
