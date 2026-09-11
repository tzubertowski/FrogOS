#ifndef FONT_H
#define FONT_H

#include <stdint.h>

// Initialize font system
void font_init(void);

// Load font from settings (call when font setting changes)
void font_load_from_settings(const char *font_name);

// Load a font directly by its on-disk filename (e.g. "monogram.ttf").
// Searches the standard font directories. Used by the dynamic font picker.
void font_load_file(const char *font_filename);

// Change the active UI font size and invalidate cached glyphs.
void font_set_size(int pixels);

// Retrieve current base UI font size in points/pixels.
int font_get_size(void);

/* Select one face for the entire UI when the chosen language needs glyphs
 * absent from the configured primary font. */
void font_sync_language_fallback(void);

// Draw a single character at position (x, y) with given color
void font_draw_char(uint16_t *framebuffer, int screen_width, int screen_height,
                    int x, int y, char c, uint16_t color);

// Draw a text string at position (x, y) with given color
void font_draw_text(uint16_t *framebuffer, int screen_width, int screen_height,
                    int x, int y, const char *text, uint16_t color);

// Measure text width in pixels
int font_measure_text(const char *text);

// Vertical metrics for centering: baseline (top-of-cell to baseline) and
// cap_height (pixel height of capitals = the visible ink band).
void font_cap_metrics(int *baseline_out, int *cap_height_out);

// Get font character width/height - scale with UI_SCALE
#ifndef UI_SCALE
#define UI_SCALE 100
#endif

#endif // FONT_H
