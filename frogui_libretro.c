/*
 * frogui_libretro.c - FrogUI as a libretro core for SF3000
 *
 * Picoarch loads this core and handles ALL display/input/SDL init.
 * FrogUI renders its menu into a RGB565 buffer and passes it to video_cb.
 * When user picks a game, write /tmp/frogui_launch.txt and signal SHUTDOWN.
 * icube reads that file and launches picoarch with the real game core.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>

#include "libretro.h"
#include "font.h"
#include "render.h"
#include "theme.h"
#include "recent_games.h"
#include "favorites.h"
#include "settings.h"
#include "banner.h"
#include "backlight.h"
#include "input.h"
#include "core_override.h"
#include "ext_filter.h"
#include "common/i18n.h"

#define SDCARD_BASE  "/mnt/sdcard"
#define CORES_PATH   SDCARD_BASE "/cubegm/cores"
/* Core audio rate (single source of truth: av_info AND the menu-tick length
 * are both derived from this - see ui_menu_tick). */
#define SAMPLE_RATE  44100
/* roms root: resolved at init. Prefer roms/, accept ROMS/ (exfat-mounted cards
 * are case-sensitive on this kernel), create roms/ if neither exists, so the
 * menu ALWAYS has a valid root instead of NULL entries → SIGBUS addr=0. */
#define ROMS_PATH_DEFAULT SDCARD_BASE "/roms"
#define OTG_MOUNT_PATH "/media/hdd"
#define OTG_ROMS_PATH  OTG_MOUNT_PATH "/roms"
/* The launcher may point FrogUI at a mounted USB disk. */
static char g_roms_path[512] = ROMS_PATH_DEFAULT;
#define ROMS_PATH    g_roms_path
#define LAUNCH_FILE  "/tmp/frogui_launch.txt"
#define LANGUAGE_RESTART_FLAG SDCARD_BASE "/frogui/language_restart.flag"
#define PCSX4ALL_BIN SDCARD_BASE "/cubegm/pcsx4all"
#define PICO286_BIN  SDCARD_BASE "/cubegm/pico286"
#define LGPT_BIN     SDCARD_BASE "/cubegm/lgpt"
#define ROCKBOX_BIN  SDCARD_BASE "/cubegm/rockbox.sh"  /* wrapper sets HOME+SDL env */
#define EBOOK_BIN    SDCARD_BASE "/cubegm/ebook"        /* MuPDF ebook reader (epub/mobi/pdf) */
#define VIDEO_BIN    SDCARD_BASE "/cubegm/video_player" /* hardware-decoded video player */
#define IMAGE_BIN    SDCARD_BASE "/cubegm/image_viewer" /* hardware-decoded image viewer */
#define PPSSPP_BIN   SDCARD_BASE "/cubegm/ppsspp"       /* optional standalone SF3000 port */
#define FROGSHELL_CORE CORES_PATH "/frogshell_libretro.so" /* file manager via picoarch */
#define USB_MODE_BIN SDCARD_BASE "/cubegm/usb_mtp.sh"  /* expose the SD card to a USB host */
#define SHUTDOWN_BIN SDCARD_BASE "/cubegm/shutdown.sh"  /* power off the console */

/* Console → core mapping (folder name → libretro .so)
 * Folder names match /mnt/sdcard/roms/ subdirectories (gb300_multicore convention). */
typedef struct { const char *console_name; const char *core_path; } ConsoleMapping;
static const ConsoleMapping console_mappings[] = {
    /* NES */
    {"nes",    CORES_PATH "/fceumm_libretro.so"},
    {"nesq",   CORES_PATH "/quicknes_libretro.so"},
    {"nest",   CORES_PATH "/nestopia_libretro.so"},
    {"FC",     CORES_PATH "/fceumm_libretro.so"},
    {"fds",    CORES_PATH "/fceumm_libretro.so"},   /* Famicom Disk System (needs disksys.rom BIOS) */
    {"NES",    CORES_PATH "/quicknes_libretro.so"},
    /* SNES */
    {"snes",   CORES_PATH "/snes9x2005_plus_libretro.so"},
    {"snes02", CORES_PATH "/snes9x2002_libretro.so"},
    {"SFC",    CORES_PATH "/snes9x2005_plus_libretro.so"},
    /* Game Boy */
    {"gb",     CORES_PATH "/gambatte_libretro.so"},
    {"gbc",    CORES_PATH "/gambatte_libretro.so"},
    {"gbgb",   CORES_PATH "/gearboy_libretro.so"},
    {"gbb",    CORES_PATH "/tgbdual_libretro.so"},
    {"dblcherrygb", CORES_PATH "/gambatte_libretro.so"},
    /* GBA */
    {"gba",    CORES_PATH "/gpsp_libretro.so"},            /* upstream libretro/gpsp */
    {"GBA",    CORES_PATH "/gpsp_libretro.so"},
    {"gbac",   CORES_PATH "/gpsp_multicore_libretro.so"},  /* tzubertowski gpsp_multicore */
    {"gbav",   CORES_PATH "/vba_next_libretro.so"},
    {"mgba",   CORES_PATH "/mgba_libretro.so"},
    {"gbaf",   CORES_PATH "/mgba_libretro.so"},
    {"GBA",    CORES_PATH "/gpsp_libretro.so"},
    /* Sega */
    {"sega",   CORES_PATH "/picodrive_libretro.so"},
    {"gg",     CORES_PATH "/gearsystem_libretro.so"},
    {"gpgx",   CORES_PATH "/genesis_plus_gx_libretro.so"},
    {"segacd", CORES_PATH "/genesis_plus_gx_libretro.so"},   /* Sega CD / Mega CD (needs BIOS) */
    {"MD",     CORES_PATH "/picodrive_libretro.so"},
    {"32x",    CORES_PATH "/picodrive_libretro.so"},   /* Sega 32X (heavy, may run slow) */
    {"SMS",    CORES_PATH "/picodrive_libretro.so"},
    {"GG",     CORES_PATH "/gearsystem_libretro.so"},
    /* Atari */
    {"a26",    CORES_PATH "/stella2014_libretro.so"},
    {"a5200",  CORES_PATH "/a5200_libretro.so"},
    {"a78",    CORES_PATH "/prosystem_libretro.so"},
    {"a800",   CORES_PATH "/atari800_libretro.so"},
    /* Lynx */
    {"lnx",    CORES_PATH "/handy_libretro.so"},
    /* PC Engine */
    {"pce",    CORES_PATH "/mednafen_pce_fast_libretro.so"},
    {"pcesgx", CORES_PATH "/mednafen_supergrafx_libretro.so"},
    /* Neo Geo Pocket */
    {"ngpc",   CORES_PATH "/race_libretro.so"},
    /* WonderSwan */
    {"wswan",  CORES_PATH "/mednafen_wswan_libretro.so"},
    {"wsv",    CORES_PATH "/potator_libretro.so"},
    /* Virtual Boy */
    {"vb",     CORES_PATH "/mednafen_vb_libretro.so"},
    /* PC-FX */
    {"pcfx",   CORES_PATH "/mednafen_pcfx_libretro.so"},
    /* PC-8800 */
    {"pc8800", CORES_PATH "/quasi88_libretro.so"},
    /* MSX */
    {"msx",    CORES_PATH "/bluemsx_libretro.so"},
    /* C64 */
    {"c64",    CORES_PATH "/vice_x64_libretro.so"},
    {"c64sc",  CORES_PATH "/vice_x64_libretro.so"},   /* only x64 built; x64sc not compiled */
    {"c64f",   CORES_PATH "/frodo_libretro.so"},
    {"c64fc",  CORES_PATH "/frodo_libretro.so"},
    {"vic20",  CORES_PATH "/vice_xvic_libretro.so"},
    /* Amstrad */
    {"amstrad",  CORES_PATH "/cap32_libretro.so"},
    {"amstradb", CORES_PATH "/cap32_libretro.so"},
    /* ZX Spectrum */
    {"spec",   CORES_PATH "/fuse_libretro.so"},
    {"zx81",   CORES_PATH "/81_libretro.so"},
    /* Coleco */
    {"col",    CORES_PATH "/gearcoleco_libretro.so"},
    /* Ports / games */
    {"Quake",  CORES_PATH "/tyrquake_libretro.so"},
    {"quake2", CORES_PATH "/vitaquake2_libretro.so"},
    {"scummvm", CORES_PATH "/scummvm_libretro.so"},
    {"outrun", CORES_PATH "/cannonball_libretro.so"},
    {"wolf3d", CORES_PATH "/ecwolf_libretro.so"},
    {"prboom", CORES_PATH "/prboom_libretro.so"},
    {"cavestory", CORES_PATH "/nxengine_libretro.so"},
    {"flashback", CORES_PATH "/reminiscence_libretro.so"},
    {"xrick",  CORES_PATH "/xrick_libretro.so"},
    {"gw",     CORES_PATH "/gw_libretro.so"},
    {"jnb",    CORES_PATH "/jumpnbump_libretro.so"},
    /* Misc */
    {"pico8",  CORES_PATH "/fake08_libretro.so"},   /* PICO-8 (fake08 core) */
    {"pico286", PICO286_BIN},                        /* DOS PC (standalone, launched directly) */
    {"lgpt",   LGPT_BIN},                            /* LittleGPTracker (standalone, launched directly) */
    {"rockbox", ROCKBOX_BIN},                        /* Rockbox music player (standalone) */
    {"Ebook",  EBOOK_BIN},                           /* ebook reader (epub/mobi/pdf, standalone) */
    {"ebook",  EBOOK_BIN},
    {"videos", VIDEO_BIN},                          /* MP4/MKV/AVI/etc. (standalone) */
    {"video",  VIDEO_BIN},
    {"music",  VIDEO_BIN},                          /* folder-based simple music player */
    {"audio",  VIDEO_BIN},
    {"frogshell", FROGSHELL_CORE},
    {"images", IMAGE_BIN},                          /* JPG/PNG/BMP/GIF/etc. (standalone) */
    {"photos", IMAGE_BIN},
    {"fake08", CORES_PATH "/fake08_libretro.so"},   /* legacy folder name */
    {"lowres-nx", CORES_PATH "/lowresnx_libretro.so"},
    {"tic80",  CORES_PATH "/tic80_libretro.so"},   /* TIC-80 fantasy console (.tic carts) */
    {"gme",    CORES_PATH "/gme_libretro.so"},
    {"m2k",    CORES_PATH "/mame2000_libretro.so"},
    {"arcade", CORES_PATH "/mame2000_libretro.so"},
    {"cps1",   CORES_PATH "/fbalpha2012_cps1_libretro.so"},    /* Capcom CPS-1 (FBA 2012) */
    {"cps2",   CORES_PATH "/fbalpha2012_cps2_libretro.so"},    /* Capcom CPS-2 (FBA 2012) */
    {"cps3",   CORES_PATH "/fbalpha2012_cps3_libretro.so"},    /* Capcom CPS-3 (FBA 2012, experimental: heavy) */
    {"neogeo", CORES_PATH "/fbalpha2012_neogeo_libretro.so"},  /* Neo Geo (FBA 2012) */
    {"pokem",  CORES_PATH "/pokemini_libretro.so"},
    {"int",    CORES_PATH "/freeintv_libretro.so"},
    {"fcf",    CORES_PATH "/freechaf_libretro.so"},
    {"cdg",    CORES_PATH "/pocketcdg_libretro.so"},
    {"chip8",  CORES_PATH "/jaxe_libretro.so"},
    {"retro8", CORES_PATH "/retro8_libretro.so"},
    {"arduboy",CORES_PATH "/ardens_libretro.so"},   /* default: Ardens (fast custom AVR core) */
    {"arduous",CORES_PATH "/arduous_libretro.so"},   /* alt: simavr-based arduous (cycle-accurate, slower) */
    {"vec",    CORES_PATH "/vecx_libretro.so"},
    {"thom",   CORES_PATH "/theodore_libretro.so"},
    {"o2em",   CORES_PATH "/o2em_libretro.so"},
    {"xmil",   CORES_PATH "/x68k_libretro.so"},
    {"geolith",CORES_PATH "/geolith_libretro.so"},
    {"gong",   CORES_PATH "/gong_libretro.so"},
    {"vapor",  CORES_PATH "/vaporspec_libretro.so"},
    {"amiga",  CORES_PATH "/uae_libretro.so"},
    {"atari-st", CORES_PATH "/castaway_libretro.so"},
    /* PlayStation: ps1/psx/PS run standalone PCSX4ALL (via is_ps1_folder);
     * these pcsx_rearmed entries are only the fallback if the pcsx4all binary
     * is missing.  ps1r runs the pcsx_rearmed libretro core directly. */
    {"ps1",    CORES_PATH "/pcsx_rearmed_libretro.so"},
    {"psx",    CORES_PATH "/pcsx_rearmed_libretro.so"},
    {"PS",     CORES_PATH "/pcsx_rearmed_libretro.so"},
    {"ps1r",   CORES_PATH "/pcsx_rearmed_libretro.so"},
    /* PSP: PPSSPP libretro core (software renderer, IR interpreter) */
    {"psp",    CORES_PATH "/ppsspp_libretro.so"},
    {"PSP",    CORES_PATH "/ppsspp_libretro.so"},
    {NULL, NULL}
};

static const char* get_core_for_folder(const char *folder) {
    if (!folder) return NULL;
    for (int i = 0; console_mappings[i].console_name; i++)
        if (strcasecmp(console_mappings[i].console_name, folder) == 0)
            return console_mappings[i].core_path;
    return NULL;
}

/* Extension fallback: when folder name doesn't match a console mapping,
 * pick a core based on the ROM file extension. */
typedef struct { const char *ext; const char *core_path; } ExtensionMapping;
static const ExtensionMapping ext_mappings[] = {
    {".nes",  CORES_PATH "/fceumm_libretro.so"},
    {".fds",  CORES_PATH "/fceumm_libretro.so"},
    {".unf",  CORES_PATH "/fceumm_libretro.so"},
    {".sfc",  CORES_PATH "/snes9x2005_plus_libretro.so"},
    {".smc",  CORES_PATH "/snes9x2005_plus_libretro.so"},
    {".gba",  CORES_PATH "/gpsp_libretro.so"},
    {".gb",   CORES_PATH "/gambatte_libretro.so"},
    {".gbc",  CORES_PATH "/gambatte_libretro.so"},
    {".md",   CORES_PATH "/picodrive_libretro.so"},
    {".smd",  CORES_PATH "/picodrive_libretro.so"},
    {".gen",  CORES_PATH "/picodrive_libretro.so"},
    {".sms",  CORES_PATH "/picodrive_libretro.so"},
    {".gg",   CORES_PATH "/gearsystem_libretro.so"},
    {".pce",  CORES_PATH "/mednafen_pce_fast_libretro.so"},
    {".sgx",  CORES_PATH "/mednafen_supergrafx_libretro.so"},
    {".lnx",  CORES_PATH "/handy_libretro.so"},
    {".lyx",  CORES_PATH "/handy_libretro.so"},
    {".ngp",  CORES_PATH "/race_libretro.so"},
    {".ngc",  CORES_PATH "/race_libretro.so"},
    {".ws",   CORES_PATH "/mednafen_wswan_libretro.so"},
    {".wsc",  CORES_PATH "/mednafen_wswan_libretro.so"},
    {".vb",   CORES_PATH "/mednafen_vb_libretro.so"},
    {".a26",  CORES_PATH "/stella2014_libretro.so"},
    {".a52",  CORES_PATH "/a5200_libretro.so"},
    {".a78",  CORES_PATH "/prosystem_libretro.so"},
    {".min",  CORES_PATH "/pokemini_libretro.so"},
    {".col",  CORES_PATH "/gearcoleco_libretro.so"},
    {".int",  CORES_PATH "/freeintv_libretro.so"},
    {".bin",  CORES_PATH "/freechaf_libretro.so"},
    {".sv",   CORES_PATH "/potator_libretro.so"},
    {".d64",  CORES_PATH "/vice_x64_libretro.so"},
    {".tap",  CORES_PATH "/fuse_libretro.so"},
    {".tzx",  CORES_PATH "/fuse_libretro.so"},
    {".dsk",  CORES_PATH "/cap32_libretro.so"},
    {".cdt",  CORES_PATH "/cap32_libretro.so"},
    {".cas",  CORES_PATH "/atari800_libretro.so"},
    {".xex",  CORES_PATH "/atari800_libretro.so"},
    {".atr",  CORES_PATH "/atari800_libretro.so"},
    {".vec",  CORES_PATH "/vecx_libretro.so"},
    {".rom",  CORES_PATH "/o2em_libretro.so"},
    {".adf",  CORES_PATH "/uae_libretro.so"},
    {".st",   CORES_PATH "/castaway_libretro.so"},
    {".msa",  CORES_PATH "/castaway_libretro.so"},
    {".cue",  CORES_PATH "/pcsx_rearmed_libretro.so"},
    {".iso",  CORES_PATH "/pcsx_rearmed_libretro.so"},
    {".mp4",  VIDEO_BIN},
    {".mkv",  VIDEO_BIN},
    {".avi",  VIDEO_BIN},
    {".mov",  VIDEO_BIN},
    {".m4v",  VIDEO_BIN},
    {".mpg",  VIDEO_BIN},
    {".mpeg", VIDEO_BIN},
    {".ts",   VIDEO_BIN},
    {".webm", VIDEO_BIN},
    {".wmv",  VIDEO_BIN},
    {".mp3",  VIDEO_BIN},
    {".m4a",  VIDEO_BIN},
    {".aac",  VIDEO_BIN},
    {".wav",  VIDEO_BIN},
    {".flac", VIDEO_BIN},
    {".ogg",  VIDEO_BIN},
    {".opus", VIDEO_BIN},
    {".jpg",  IMAGE_BIN},
    {".jpe",  IMAGE_BIN},
    {".jpeg", IMAGE_BIN},
    {".png",  IMAGE_BIN},
    {".bmp",  IMAGE_BIN},
    {".gif",  IMAGE_BIN},
    {".tga",  IMAGE_BIN},
    {".targa",IMAGE_BIN},
    {".ico",  IMAGE_BIN},
    {".webp", IMAGE_BIN},
    {".tif",  IMAGE_BIN},
    {".tiff", IMAGE_BIN},
    {NULL, NULL}
};

static const char* get_core_for_extension(const char *filename) {
    if (!filename) return NULL;
    const char *dot = strrchr(filename, '.');
    if (!dot) return NULL;
    for (int i = 0; ext_mappings[i].ext; i++)
        if (strcasecmp(ext_mappings[i].ext, dot) == 0)
            return ext_mappings[i].core_path;
    return NULL;
}

/* --- Available cores for the per-game / per-folder core picker ---
 * Deduped list built from console_mappings. Index 0 = "Default (auto)"
 * which clears any override and falls back to folder/extension mapping. */
typedef struct { char name[64]; const char *path; } CoreChoice;
static CoreChoice core_choices[160];
static int core_choice_count = 0;

/* Cores selectable in the per-game/per-folder picker but NOT auto-mapped to any
 * rom folder (no default wiring). Lets users opt into heavier modern cores
 * (FBNeo, mame2003-plus) per game without changing the lightweight defaults. */
static const char *extra_picker_cores[] = {
    CORES_PATH "/fbneo_libretro.so",
    CORES_PATH "/mame2003_plus_libretro.so",
    CORES_PATH "/snes9x2010_libretro.so",
    NULL,
};

static void add_core_choice(const char *p) {
    for (int j = 1; j < core_choice_count; j++)
        if (strcmp(core_choices[j].path, p) == 0) return;   /* dedup by path */
    if (core_choice_count >= (int)(sizeof(core_choices)/sizeof(core_choices[0])))
        return;
    const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
    char nm[64]; strncpy(nm, base, sizeof(nm)-1); nm[sizeof(nm)-1] = '\0';
    char *suf = strstr(nm, "_libretro.so"); if (suf) *suf = '\0';
    strncpy(core_choices[core_choice_count].name, nm, 63);
    core_choices[core_choice_count].name[63] = '\0';
    core_choices[core_choice_count].path = p;
    core_choice_count++;
}

static int core_choice_cmp(const void *a, const void *b) {
    return strcasecmp(((const CoreChoice *)a)->name, ((const CoreChoice *)b)->name);
}

static void build_core_choices(void) {
    strcpy(core_choices[0].name, "Default (auto)");
    core_choices[0].path = NULL;
    core_choice_count = 1;
    for (int i = 0; console_mappings[i].console_name; i++)
        add_core_choice(console_mappings[i].core_path);
    for (int i = 0; extra_picker_cores[i]; i++)
        add_core_choice(extra_picker_cores[i]);
    /* Dynamic: every *_libretro.so actually present in the cores dir is
     * selectable too, so a newly dropped-in core shows up in the picker
     * without a TreeFrogUI rebuild. (strdup'd once at init; add_core_choice
     * dedupes against the mapped cores by path.) */
    DIR *d = opendir(CORES_PATH);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *n = e->d_name;
            size_t l = strlen(n);
            if (l < 13 || strcmp(n + l - 12, "_libretro.so") != 0) continue;
            if (strcmp(n, "frogui_libretro.so") == 0) continue;  /* the menu itself */
            char full[512];
            snprintf(full, sizeof full, CORES_PATH "/%s", n);
            char *p = strdup(full);
            if (p) add_core_choice(p);
        }
        closedir(d);
    }
    /* Alphabetical, keeping "Default (auto)" pinned at index 0. */
    if (core_choice_count > 2)
        qsort(&core_choices[1], core_choice_count - 1, sizeof(CoreChoice), core_choice_cmp);
}

static int core_choice_index_for_path(const char *path) {
    if (!path) return 0;
    for (int i = 1; i < core_choice_count; i++)
        if (strcmp(core_choices[i].path, path) == 0) return i;
    return 0;
}

/* Libretro callbacks */
static retro_video_refresh_t     video_cb     = NULL;
static retro_environment_t       environ_cb   = NULL;
static retro_audio_sample_t      audio_cb     = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_input_poll_t        input_poll_cb = NULL;
static retro_input_state_t       input_state_cb = NULL;

/* App state */
#define MAX_PATH_LEN 512
#define INITIAL_ENTRIES_CAPACITY 64
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

typedef struct { char name[256]; int is_dir; } DirEntry;

static DirEntry *entries    = NULL;
static int entry_count      = 0;
static int entry_capacity   = 0;
static char current_path[MAX_PATH_LEN] = ROMS_PATH_DEFAULT;
static int selected_index   = 0;
static int scroll_offset    = 0;
static uint16_t *framebuffer = NULL;
static bool shutdown_requested = false;
static bool viewing_recents = false;
static bool viewing_favourites = false;
static bool viewing_apps = false;
static bool apps_browsing = false;
static bool viewing_activity = false;
static char activity_paths[128][MAX_PATH_LEN];
static long activity_seconds[128];
static long activity_runs[128];
static time_t activity_last[128];
static int activity_has_dates = 0;
static int activity_count = 0;
static char apps_root_path[MAX_PATH_LEN] = "";
static bool game_switcher_fullscreen = false;
enum { MAIN_TAB_RECENTS, MAIN_TAB_GAMES, MAIN_TAB_APPS, MAIN_TAB_SETTINGS };
/* Keep view changes nearly immediate. Three blended frames provide a short,
 * consistent crossfade without making tab navigation wait for a panel slide. */
#define VIEW_TRANSITION_FRAMES 2
static uint16_t *view_transition_old = NULL;
static uint16_t *view_transition_out = NULL;
static int view_transition_frame = VIEW_TRANSITION_FRAMES + 1;
static int is_app_folder_name(const char *name);
static bool is_psp_folder(const char *folder);
static char ui_toast_text[96] = "";
static int ui_toast_frames = 0;
/* The UI's idle path polls input at about 1 kHz.  These must not be frame
 * counters: a 240-frame confirmation used to disappear in a fraction of a
 * second, before a deliberately held face button had even passed debounce. */
static bool usb_mode_confirm_active = false;
static bool usb_mode_initiated_active = false;
static bool usb_mode_wait_for_release = false;
static long long usb_mode_initiated_at_ms = 0;
static uint32_t usb_mode_prev_raw = 0;
static void ui_toast_show(const char *text);
static void ui_transition_start(int direction);
static void system_labels_refresh(void);
#define SYSTEM_CAROUSEL_FRAMES 12
static int system_carousel_frame = SYSTEM_CAROUSEL_FRAMES + 1;
static float system_carousel_start_offset = 0.0f;
static float system_carousel_visible_offset = 0.0f;

/* Search (X button): on-screen keyboard → filtered results.
 * Scope = the folder you were in (ROMS_PATH root = search everything). */
static bool search_kbd_active = false;     /* typing the query */
static bool viewing_search    = false;     /* showing results list */
static char search_query[64]  = "";
static int  search_kbd_r = 0, search_kbd_c = 0;
static char search_scope[MAX_PATH_LEN] = "";
typedef struct { char name[256]; char path[MAX_PATH_LEN]; } SearchResult;
static SearchResult *search_results = NULL;
static int search_results_count = 0, search_results_cap = 0;

/* Per-game / per-folder core picker overlay (opened with SELECT) */
static bool core_picker_active = false;
static int  core_picker_idx = 0;
static int  core_picker_scroll = 0;
static int  core_picker_current = 0;   /* index of the ACTIVE core (marked ">>") */
static char core_picker_key[MAX_PATH_LEN];   /* ROM path (per-game) or folder path */
static char core_picker_title[160];          /* shown under header */
/* Extension-filter section of the picker (PS1 family). The picker shows two
 * collapsible sections, each with a selectable ">> " header:
 *   ">> Cores"              core rows (Default (auto), each core)
 *   ">> EXTENSIONS"         "Filter files: ON" master toggle + "[x] .cue"
 * LEFT/RIGHT (or A) on a header collapses/expands its section so long lists
 * can be skipped quickly. The active core keeps a "->" marker — ">> " is
 * reserved for section headers. */
static bool extf_in_picker = false;
static bool extf_cores_open = true;   /* ">> Cores" section expanded */
static bool extf_exts_open  = true;   /* ">> EXTENSIONS" section expanded */
static char extf_folder[32] = "";      /* system folder the picker filters */
static char extf_path[MAX_PATH_LEN];   /* full SD path of that folder */
static bool extf_inside = false;        /* picker opened on a file inside it */
static bool extf_dirty = false;        /* a filter edit needs a rescan */
/* Candidate extensions offered for whitelists. PS1 ships games as .cue +
 * .bin track blobs (and .chd/.pbp/... dumps): whitelisting the loadable
 * index formats makes each game appear once. */
static const char *extf_pool[] = {
    /* Common ROM/media extensions.  The picker is available for every
     * browsable folder; users can add anything else through the keyboard. */
    "zip", "7z", "cue", "bin", "m3u", "pbp", "iso", "chd",
    "img", "mdf", "sbi", "ccd", "ecm", "gba", "gb", "gbc", "nes",
    "smc", "sfc", "md", "gen", "sms", "gg", "pce", "a26", "a78",
    "lnx", "ws", "wsc", "nds", "cia", "love", "p8", "p8.png", NULL
};

static void dbg(const char *msg) {
    /* Mirror zhijack's opt-in diagnostics convention.  Writing every UI
     * startup step to FAT on normal boots is needless wear and noise. */
    static int enabled = -1;
    if (enabled < 0) enabled = access("/mnt/sdcard/log.txt", F_OK) == 0;
    if (!enabled) return;
    FILE *f = fopen("/mnt/sdcard/frogui_crash.log", "a");
    if (f) { fputs(msg, f); fputs("\n", f); fclose(f); }
    fprintf(stderr, "FROGUI_DBG: %s\n", msg);
}

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* USB mode is a modal dialog, so it must not inherit browser navigation,
 * per-game bindings, or the face-button filter intended to suppress right-stick
 * drift.  Its A/B controls are read from the physical bits selected in the
 * user's keymap and every other key is deliberately consumed by the modal. */
static bool usb_mode_raw_edge(FrogButton button, uint32_t raw) {
    int bit = input_get_raw_bit(button);
    if (bit < 0 || bit > 15) return false;
    uint32_t mask = 1u << bit;
    return (raw & mask) && !(usb_mode_prev_raw & mask);
}

/* Hide cubevol's battery glyph while leaving its centered volume popup intact.
 * fb1 is rotated and/or double-buffered on SF3500-class devices, so physical
 * top-right is not reliably memory top-right and the active page is not always
 * page zero. Clear every memory corner on every virtual page. */
static void fb1_clear_battery_zone(void) {
    /* Persistent mmap + cached geometry: open/mmap ONCE. The old per-frame
     * open+ioctl+mmap+munmap+close stalled the loop (visible input lag).
     * Do not latch a failed first attempt: early boot may reach FrogUI before
     * fb1 is ready, so retry later until the one-time mmap succeeds. */
    static unsigned char *mem = NULL; static int inited = 0;
    static size_t map_len = 0;
    static int pitch = 0, bpp = 4, vx = 0, vw = 0, vh = 0;
    static int corner_w = 0, corner_h = 0, mapped_rows = 0;
    if (!inited) {
        int fd = open("/dev/fb1", O_RDWR);
        if (fd < 0) return;
        struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0 &&
            ioctl(fd, FBIOGET_FSCREENINFO, &fi) == 0 && fi.smem_len > 0) {
            void *m = mmap(NULL, fi.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) {
                mem = m;
                map_len = fi.smem_len;
                bpp = vi.bits_per_pixel / 8; if (bpp < 1) bpp = 4;
                pitch = fi.line_length ? (int)fi.line_length : (int)vi.xres * bpp;
                vx = vi.xoffset;
                vw = vi.xres;
                vh = vi.yres;
                mapped_rows = pitch > 0 ? (int)(map_len / (size_t)pitch) : 0;
                /* Match the custom icon's exact body+nub footprint: 26x13
                 * baseline plus a 3px terminal, scaled with the UI. */
                corner_w = UI_S(29) + 30;
                corner_h = UI_S(13) + 30;
                inited = 1;
            }
        }
        close(fd);   /* mmap survives close */
    }
    if (!mem || pitch <= 0 || vh <= 0 || mapped_rows <= 0) return;

    int right_x = vx + vw - corner_w;
    if (right_x < 0) right_x = 0;
    size_t want_bytes = (size_t)corner_w * bpp;
    size_t right_off = (size_t)right_x * bpp;
    size_t right_bytes = right_off < (size_t)pitch
                       ? min(want_bytes, (size_t)pitch - right_off) : 0;

    /* R36SX's stock battery glyph is top-right on page zero. Do not clear
     * other corners/pages: those transparent writes survive a hard shutdown
     * and expose the stale TreeFrogUI frame under the stock logo. */
    int top_y = UI_S(10) - 15;
    if (top_y < 0) top_y = 0;
    for (int y = top_y; y < top_y + corner_h && y < vh; y++) {
        size_t ro = (size_t)y * pitch + right_off;
        if (right_bytes && ro + right_bytes <= map_len)
            memset(mem + ro, 0, right_bytes);
    }
}

/* Clear cubevol's entire overlay before handing control back to the stock
 * launcher.  The battery indicator is drawn on the main UI framebuffer, but
 * cubevol's fb1 plane can otherwise survive the handoff and show old corner
 * pixels over the shutdown/boot logo. */
static void fb1_clear_all(void) {
    int fd = open("/dev/fb1", O_RDWR);
    if (fd < 0) return;
    struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) == 0 && fi.smem_len > 0) {
        void *mem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mem != MAP_FAILED) {
            memset(mem, 0, fi.smem_len);
            munmap(mem, fi.smem_len);
        }
    }
    close(fd);
}

/* Cached battery % for the header (re-read every ~2s; ADC changes slowly). Also
 * clears cubevol's battery corner each call so its glyph stays hidden. Called by
 * render_header (every screen, every frame). */
static int raw_to_pct(int raw) {
    /* This ADC is too noisy and device-dependent for a truthful percentage.
     * Use three broad states; give the full state a deliberately wide range. */
    if (raw < 100) return 25;
    if (raw < 145) return 50;
    return 100;
}
/* Persistent ADC fds, opened O_RDWR ONCE like cubevol (battery_adc_init) - these
 * nodes are flaky when re-opened per poll, especially check_adc2. slot 0=adc1
 * (battery), 1=adc2 (charge). Read 1 byte from the kept-open fd each poll. */
static int read_adc(int slot) {
    static int fd[2] = { -2, -2 };
    static const char *node[2] = { "/dev/check_adc1", "/dev/check_adc5" };
    if (slot < 0 || slot > 1) return -1;
    if (fd[slot] == -2) fd[slot] = open(node[slot], O_RDWR);
    if (fd[slot] < 0) return -1;
    unsigned char b = 0;
    lseek(fd[slot], 0, SEEK_SET);
    int n = read(fd[slot], &b, 1);
    return (n == 1) ? b : -1;
}

static int g_batt_charging = 0;
int frogui_battery_charging(void) { return g_batt_charging; }
int frogui_battery_color_mode(void);   /* defined after settings_battery_color */
int frogui_battery_pct(void) {
    static int cached = -1, tick = 0;
    if (cached < 0 || (tick++ % 300) == 0) {
        int a1 = read_adc(0), a5 = read_adc(1);
        cached = (a1 >= 0) ? raw_to_pct(a1) : -1;
        g_batt_charging = (a5 >= 64) ? 1 : 0;
    }
    fb1_clear_battery_zone();
    return cached;
}

/* --- Input (via input.c / cubevol shmem) --- */

/* --- Settings ---
 * Stored at /mnt/sdcard/frogui/settings.txt, key=value format.
 * Two options: theme + font. */
#define SETTINGS_DIR  "/mnt/sdcard/frogui"
#define SETTINGS_FILE SETTINGS_DIR "/settings.txt"

/* Fonts are discovered at runtime by scanning the font directories for
 * .ttf/.otf files. font_files[] holds the on-disk filename (persisted in
 * settings + passed to the loader); font_disp[] is the extension-stripped
 * name shown in the menu. */
#define MAX_FONTS    32
#define FONT_STR_MAX 96
static char font_files[MAX_FONTS][FONT_STR_MAX];
static char font_disp[MAX_FONTS][FONT_STR_MAX];
static int  font_count = 0;

static int font_has_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0);
}

static void font_add(const char *fname) {
    if (font_count >= MAX_FONTS) return;
    for (int i = 0; i < font_count; i++)
        if (strcasecmp(font_files[i], fname) == 0) return;  /* dedup */
    strncpy(font_files[font_count], fname, FONT_STR_MAX - 1);
    font_files[font_count][FONT_STR_MAX - 1] = '\0';
    font_count++;
}

/* ---- Wallpaper: one image used across ALL views (index 0 = "None" = the normal
 * per-system background art). Scanned from frogui/wallpapers/. ------------- */
#define MAX_WALL 64
static char wallpaper_files[MAX_WALL][FONT_STR_MAX];
static char wallpaper_disp[MAX_WALL][FONT_STR_MAX];
static int  wallpaper_count = 0;         /* includes slot 0 = "None" */
static int  settings_wallpaper_idx = 0;  /* 0 = none/per-system */
static int  settings_wallpaper_fit = 2;  /* BANNER_FIT_* (Fill/Fit/Stretch/Center/Tile) */
static const char *wallpaper_fit_names[] = { "Fill", "Fit", "Stretch", "Center", "Tile" };
#define WALL_FIT_N 5

/* Optional background packs. Pack 0 is the shipped, flat frogui/ artwork;
 * additional packs live in frogui/theme-packs/<folder>/. Each pack contains
 * the same per-screen filenames (main.jpg, ps.jpg, settings.jpg, ...). */
#define MAX_THEME_PACKS 32
static char theme_pack_files[MAX_THEME_PACKS][FONT_STR_MAX];
static char theme_pack_disp[MAX_THEME_PACKS][FONT_STR_MAX];
static int theme_pack_count = 1;
static int settings_theme_pack_idx = 0;

/* System View icon packs are independent from background packs. Pack 0 keeps
 * frogui/system-icons; optional packs live in frogui/icon-packs/<folder>/. */
#define MAX_ICON_PACKS 32
static char icon_pack_files[MAX_ICON_PACKS][FONT_STR_MAX];
static char icon_pack_disp[MAX_ICON_PACKS][FONT_STR_MAX];
static int icon_pack_count = 1;
static int settings_icon_pack_idx = 0;

static void icon_pack_scan(void) {
    icon_pack_count = 1;
    strcpy(icon_pack_files[0], "");
    strcpy(icon_pack_disp[0], "Default (Cosy)");
    DIR *dp = opendir("/mnt/sdcard/frogui/icon-packs");
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) && icon_pack_count < MAX_ICON_PACKS) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type != DT_DIR) {
            char full[512]; struct stat st;
            snprintf(full, sizeof full, "/mnt/sdcard/frogui/icon-packs/%s", e->d_name);
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        }
        strncpy(icon_pack_files[icon_pack_count], e->d_name, FONT_STR_MAX - 1);
        icon_pack_files[icon_pack_count][FONT_STR_MAX - 1] = '\0';
        strncpy(icon_pack_disp[icon_pack_count], e->d_name, FONT_STR_MAX - 1);
        icon_pack_disp[icon_pack_count][FONT_STR_MAX - 1] = '\0';
        for (char *p = icon_pack_disp[icon_pack_count]; *p; p++)
            if (*p == '_') *p = ' ';
        icon_pack_count++;
    }
    closedir(dp);
    /* Stable alphabetical menu; slot 0 stays the default. */
    for (int i = 2; i < icon_pack_count; i++) {
        char kf[FONT_STR_MAX], kd[FONT_STR_MAX];
        strncpy(kf, icon_pack_files[i], FONT_STR_MAX);
        strncpy(kd, icon_pack_disp[i], FONT_STR_MAX);
        int j = i - 1;
        while (j >= 1 && strcasecmp(icon_pack_disp[j], kd) > 0) {
            strncpy(icon_pack_files[j + 1], icon_pack_files[j], FONT_STR_MAX);
            strncpy(icon_pack_disp[j + 1], icon_pack_disp[j], FONT_STR_MAX);
            j--;
        }
        strncpy(icon_pack_files[j + 1], kf, FONT_STR_MAX);
        strncpy(icon_pack_disp[j + 1], kd, FONT_STR_MAX);
    }
    if (settings_icon_pack_idx < 0 || settings_icon_pack_idx >= icon_pack_count)
        settings_icon_pack_idx = 0;
}

static void theme_pack_scan(void) {
    theme_pack_count = 1;
    strcpy(theme_pack_files[0], "");
    strcpy(theme_pack_disp[0], "Default");
    DIR *dp = opendir("/mnt/sdcard/frogui/theme-packs");
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) && theme_pack_count < MAX_THEME_PACKS) {
        if (e->d_name[0] == '.' || e->d_type == DT_REG) continue;
        int dup = 0;
        for (int i = 1; i < theme_pack_count; i++)
            if (strcasecmp(theme_pack_files[i], e->d_name) == 0) { dup = 1; break; }
        if (dup) continue;
        strncpy(theme_pack_files[theme_pack_count], e->d_name, FONT_STR_MAX - 1);
        theme_pack_files[theme_pack_count][FONT_STR_MAX - 1] = '\0';
        strncpy(theme_pack_disp[theme_pack_count], e->d_name, FONT_STR_MAX - 1);
        theme_pack_disp[theme_pack_count][FONT_STR_MAX - 1] = '\0';
        for (char *p = theme_pack_disp[theme_pack_count]; *p; p++)
            if (*p == '_' || *p == '-') *p = ' ';
        theme_pack_count++;
    }
    closedir(dp);
    if (settings_theme_pack_idx < 0 || settings_theme_pack_idx >= theme_pack_count)
        settings_theme_pack_idx = 0;
    /* New installations start with the credited NextUI background set. A
     * saved settings.txt selection still wins when settings are loaded later. */
    if (settings_theme_pack_idx == 0) {
        for (int i = 1; i < theme_pack_count; i++)
            if (strcasecmp(theme_pack_files[i], "Art_Book_NextUI") == 0) {
                settings_theme_pack_idx = i;
                break;
            }
    }
}

static int wall_has_ext(const char *n) {
    const char *d = strrchr(n, '.');
    return d && (strcasecmp(d, ".png") == 0 || strcasecmp(d, ".jpg") == 0 ||
                 strcasecmp(d, ".jpeg") == 0 || strcasecmp(d, ".bmp") == 0);
}
static void wallpaper_scan(void) {
    wallpaper_count = 0;
    strcpy(wallpaper_files[0], "");        /* slot 0 = None */
    strcpy(wallpaper_disp[0], "None");
    wallpaper_count = 1;
    DIR *dp = opendir("/mnt/sdcard/frogui/wallpapers");
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) && wallpaper_count < MAX_WALL) {
            if (e->d_name[0] == '.' || !wall_has_ext(e->d_name)) continue;
            int dup = 0;
            for (int i = 1; i < wallpaper_count; i++)
                if (strcasecmp(wallpaper_files[i], e->d_name) == 0) { dup = 1; break; }
            if (dup) continue;
            strncpy(wallpaper_files[wallpaper_count], e->d_name, FONT_STR_MAX - 1);
            wallpaper_files[wallpaper_count][FONT_STR_MAX - 1] = '\0';
            strncpy(wallpaper_disp[wallpaper_count], e->d_name, FONT_STR_MAX - 1);
            wallpaper_disp[wallpaper_count][FONT_STR_MAX - 1] = '\0';
            char *dot = strrchr(wallpaper_disp[wallpaper_count], '.');
            if (dot) *dot = '\0';
            wallpaper_count++;
        }
        closedir(dp);
    }
    if (settings_wallpaper_idx >= wallpaper_count) settings_wallpaper_idx = 0;
}

static void font_scan(void) {
    static const char *dirs[] = {
        "/mnt/sdcard/cubegm/fonts",
        "/mnt/sdcard/frogui/fonts",
        "fonts",
    };
    font_count = 0;
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            if (e->d_name[0] == '.') continue;
            if (font_has_ext(e->d_name)) font_add(e->d_name);
        }
        closedir(dp);
    }
    /* No fonts on disk: keep the built-in defaults as a safety net. */
    if (font_count == 0) {
        font_add("GamePocket-Regular-ZeroKern.ttf");
        font_add("monogram.ttf");
    }
    /* Sort filenames alphabetically (case-insensitive) for a stable list. */
    for (int i = 1; i < font_count; i++) {
        char key[FONT_STR_MAX];
        strncpy(key, font_files[i], FONT_STR_MAX);
        int j = i - 1;
        while (j >= 0 && strcasecmp(font_files[j], key) > 0) {
            strncpy(font_files[j + 1], font_files[j], FONT_STR_MAX);
            j--;
        }
        strncpy(font_files[j + 1], key, FONT_STR_MAX);
    }
    /* Build display names = filename minus extension. */
    for (int i = 0; i < font_count; i++) {
        strncpy(font_disp[i], font_files[i], FONT_STR_MAX - 1);
        font_disp[i][FONT_STR_MAX - 1] = '\0';
        char *dot = strrchr(font_disp[i], '.');
        if (dot) *dot = '\0';
    }
}

static bool settings_menu_active = false;
/* Bump this for a runtime change that alters rendered strings but not the
 * browser's directory state.  The idle renderer otherwise intentionally keeps
 * presenting its last frame. */
static int settings_menu_idx = 0;       /* row: 0=theme, 1=font, 2=brightness, 3=quick resume, 4=auto-save/auto-load, 5=animations... */
static int settings_theme_idx = 0;
static int settings_font_idx = 0;
static int settings_brightness = 75;    /* 0..100, step 5 */
/* Frames left to re-assert brightness after a cubevol (re)start. cubevol applies
 * its OWN stored brightness on start, DELAYED by panel/backlight-delay (to avoid
 * a boot flash) — which lands after our one-shot settings_apply and overrides it
 * (default brightness on cold boot / return from game). Re-asserting over a short
 * window outlasts that delayed apply; cubevol then only touches backlight on
 * hotkeys, so our value sticks. */
static int settings_bl_reassert = 0;
static int settings_audio_mute_reassert = 0;
static int settings_filter_idx = 1;     /* forced bilinear (option removed from menu) */
static int settings_filter_idx_on_enter = 0;  /* snapshot for restart-on-change */
static int settings_quick_resume = 0;      /* boot straight into last game: 0=off, 1=on. Settings key stays "auto_resume" for upgrade compat. */
static int settings_custom_aspect_ratios = 0; /* expose forced aspect-ratio choices in PicoArch */
static int settings_autosave_autoload = 1; /* auto-save on pause/quit + auto-load on any game launch (boot or manual pick): 0=off, 1=on */
static int settings_anim = 1;           /* UI animations: 0=off, 1=on */
static int settings_menu_sounds = 0;    /* short navigation tick: 0=off, 1=on */
enum { STYLE_VERTICAL, STYLE_HORIZONTAL, STYLE_SYSTEM, STYLE_COUNT };
static int settings_style = STYLE_VERTICAL;
static int settings_center_text = 0;   /* center labels in the vertical system list */
static int settings_friendly_names = 0; /* expand system folder codes in either style */
static int settings_hide_empty = 1;     /* hide rom folders with no games: 0=off, 1=on */
static int settings_hide_extensions = 1; /* hide file extensions in the browser: 0=off, 1=on */
static int settings_backgrounds = 1;     /* show per-system background images: 0=off (solid theme bg), 1=on */
static int settings_background_dim = 15; /* darken background artwork: 0=unchanged, 100=black */
static int settings_file_cache = 1;      /* cache folder listings (mtime-keyed) for fast nav: 0=off, 1=on */
static int settings_battery_color = 0;   /* "Nel Battery Mode": solid color light by level instead of fill bar */
enum { LANGUAGE_EN_US, LANGUAGE_PL_PL, LANGUAGE_ES_ES, LANGUAGE_PT_BR, LANGUAGE_JA_JP,
       LANGUAGE_RU_RU, LANGUAGE_ZH_CN, LANGUAGE_COUNT };
static int settings_language = LANGUAGE_EN_US;
static const char *language_codes[LANGUAGE_COUNT] = {
    "en_US", "pl_PL", "es_ES", "pt_BR", "ja_JP", "ru_RU", "zh_CN"
};
static const char *language_name_keys[LANGUAGE_COUNT] = {
    "language.en_US", "language.pl_PL", "language.es_ES", "language.pt_BR", "language.ja_JP",
    "language.ru_RU", "language.zh_CN"
};

/* Pastel themes are complete treatments, not palette-only options.  Pair
 * them with their matching artwork pack whenever the theme is applied. */
static void theme_sync_artwork_pack(void) {
    const char *wanted = NULL;
    const char *theme = theme_get_name(settings_theme_idx);
    if (strcmp(theme, "Catppuccin Mocha") == 0) wanted = "Catppuccin";
    else if (strcmp(theme, "Aura") == 0) wanted = "Aura";
    else if (strcmp(theme, "Canvas Pastel") == 0) wanted = "Canvas_Pastel";
    if (!wanted) return;
    for (int i = 0; i < theme_pack_count; i++) {
        if (strcasecmp(theme_pack_files[i], wanted) == 0 ||
            strcasecmp(theme_pack_disp[i], wanted) == 0) {
            settings_theme_pack_idx = i;
            return;
        }
    }
}

int frogui_battery_color_mode(void) { return settings_battery_color; }
static int settings_game_switcher = 1;  /* recents as box-art carousel: 0=off, 1=on */
static int settings_load_recents = 0;   /* start FrogUI in the recents view: 0=off, 1=on */
enum { ROM_SOURCE_SD, ROM_SOURCE_OTG, ROM_SOURCE_COUNT };
static int settings_rom_source = ROM_SOURCE_SD;
static const char *rom_source_names[ROM_SOURCE_COUNT] = {"SD", "OTG"};
static int settings_disable_sleep = 1;  /* live-patch cubevol to disable power sleep: 0=off, 1=on. zhijack reads this at boot; applies after restart. Default ON: R36SX/SF3500-class sleep isn't supported by TreeFrogUI, so ship with it disabled and point users at Quick Resume instead. */
static int settings_volume = 100;       /* global output volume 0..100 → cubegm/sndgain.txt */
static const char *filter_names[] = {"nearest", "bilinear"};
static const char *onoff_names[] = {"off", "on"};
static const char *style_names[STYLE_COUNT] = {"Vertical", "Horizontal", "System View"};
static const char *style_keys[STYLE_COUNT] = {"vertical", "horizontal", "system"};
#define FILTER_COUNT 2
#define SETTINGS_BRIGHTNESS_STEP 5
/* Filter option removed from the menu — always bilinear (HW path). */

/* Data-driven settings menu: rows are HEADERs (non-selectable dividers) or
 * options. Adding, reordering, or grouping = just editing this table; the nav,
 * adjust, and render code all iterate it. TOGGLE/RANGE point at their int; THEME
 * and FONT are special (dynamic option lists); ACTION opens the remap wizard. */
typedef enum { RT_HEADER, RT_INFO, RT_TOGGLE, RT_RANGE, RT_THEME, RT_STYLE, RT_FONT, RT_WALLPAPER,
               RT_WALLFIT, RT_THEME_PACK, RT_ICON_PACK, RT_ROM_SOURCE, RT_OTG_STATUS,
               RT_CACHE_REBUILD, RT_ACTION, RT_LANGUAGE } SRowType;
typedef struct {
    SRowType type;
    const char *label;
    int *val;               /* TOGGLE / RANGE target */
    int rmin, rmax, rstep;  /* RANGE bounds */
} SRow;

static const SRow settings_rows[] = {
    { RT_HEADER, "settings.appearance" }, { RT_THEME, "settings.theme" }, { RT_THEME_PACK, "settings.background_theme_pack" }, { RT_STYLE, "settings.style" }, { RT_ICON_PACK, "settings.icon_pack" }, { RT_TOGGLE, "settings.center_text", &settings_center_text }, { RT_TOGGLE, "settings.friendly_system_names", &settings_friendly_names }, { RT_FONT, "settings.font" }, { RT_TOGGLE, "settings.battery_colour_mode", &settings_battery_color }, { RT_TOGGLE, "settings.background_images", &settings_backgrounds }, { RT_RANGE, "settings.background_dim", &settings_background_dim, 0, 100, 5 }, { RT_WALLPAPER, "settings.wallpaper" }, { RT_WALLFIT, "settings.background_image_fit" },
    { RT_HEADER, "settings.general" }, { RT_LANGUAGE, "settings.language", &settings_language }, { RT_RANGE, "settings.brightness", &settings_brightness, 0, 100, SETTINGS_BRIGHTNESS_STEP }, { RT_TOGGLE, "settings.animations", &settings_anim }, { RT_TOGGLE, "settings.menu_sounds", &settings_menu_sounds }, { RT_TOGGLE, "settings.hide_extensions", &settings_hide_extensions }, { RT_TOGGLE, "settings.hide_empty_folders", &settings_hide_empty },
    { RT_HEADER, "settings.library" }, { RT_ROM_SOURCE, "settings.rom_source" }, { RT_OTG_STATUS, "settings.otg_storage" }, { RT_TOGGLE, "settings.game_switcher", &settings_game_switcher }, { RT_TOGGLE, "settings.start_in_recents", &settings_load_recents },
    { RT_HEADER, "settings.gameplay" }, { RT_TOGGLE, "settings.quick_resume", &settings_quick_resume }, { RT_TOGGLE, "settings.autosave_autoload", &settings_autosave_autoload }, { RT_TOGGLE, "settings.custom_aspect_ratios", &settings_custom_aspect_ratios },
    { RT_HEADER, "settings.system" }, { RT_RANGE, "settings.volume", &settings_volume, 0, 100, 5 }, { RT_TOGGLE, "settings.file_cache", &settings_file_cache }, { RT_CACHE_REBUILD, "settings.rebuild_file_cache" }, { RT_TOGGLE, "settings.disable_sleep", &settings_disable_sleep }, { RT_ACTION, "settings.button_mapping" }, { RT_INFO, "settings.version" },
};
#define SETTINGS_ROW_N ((int)(sizeof(settings_rows) / sizeof(settings_rows[0])))
static bool otg_roms_available(void);

/* A disabled dependent row stays visible (so the relationship is obvious),
 * but cannot be changed until its prerequisite is enabled. */
static bool settings_row_enabled(const SRow *r) {
    if (!r) return false;
    if (r->type == RT_THEME_PACK) return settings_backgrounds != 0;
    if (r->type == RT_ICON_PACK) return settings_style == STYLE_SYSTEM;
    return true;
}

/* ---- Collapsible settings sections ---------------------------------------
 * Headers stay dividers for rendering, but the cursor can land on them and
 * LEFT/RIGHT (or A) collapses/expands their section — same interaction as the
 * SELECT picker's ">> Cores" / ">> EXTENSIONS". The visible-row list below
 * is rebuilt whenever a section flips. */
static const char *settings_section_names[] = { "settings.appearance", "settings.general", "settings.library", "settings.gameplay", "settings.system", NULL };
#define SETTINGS_SECTION_N 5
static bool settings_section_open[SETTINGS_SECTION_N] = { true, true, true, true, true };

/* Map a settings_rows[] header index -> section id (index into
 * settings_section_names), or -1 when the row is not a known header. */
static int settings_section_of(int i) {
    if (i < 0 || i >= SETTINGS_ROW_N || settings_rows[i].type != RT_HEADER) return -1;
    for (int s = 0; settings_section_names[s]; s++)
        if (strcmp(settings_rows[i].label, settings_section_names[s]) == 0)
            return s;
    return -1;
}

/* ---- Collapsed-state persistence (review feedback) -----------------------
 * Each category's collapsed/expanded state is stored in the regular
 * settings file as one compact line ("sections=01011", one digit per
 * section, 0=collapsed 1=expanded). Default when absent: all expanded.
 * Saved immediately when the user toggles a section, so the layout comes
 * back exactly as it was left across reboots and updates. */
static void settings_sections_save(void) {
    /* Reuse the same file: read the current contents, replace/append the
     * sections line, write back. The file is tiny (a few hundred bytes). */
    char buf[2048];
    size_t len = 0;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (f) {
        len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[len] = '\0';

    char newline[32];
    snprintf(newline, sizeof newline, "sections=%d%d%d%d%d",
             settings_section_open[0] ? 1 : 0, settings_section_open[1] ? 1 : 0,
             settings_section_open[2] ? 1 : 0, settings_section_open[3] ? 1 : 0,
             settings_section_open[4] ? 1 : 0);

    FILE *out = fopen(SETTINGS_FILE, "w");
    if (!out) return;
    int wrote = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "sections=", 9) == 0) {
            fputs(newline, out); fputc('\n', out);
            wrote = 1;
        } else if (line[0] != '\0' || nl) {
            fputs(line, out); fputc('\n', out);
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!wrote) { fputs(newline, out); fputc('\n', out); }
    fclose(out);
}

static void settings_sections_load(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "sections=", 9) == 0) {
            const char *d = line + 9;
            for (int s = 0; s < SETTINGS_SECTION_N && d[s]; s++)
                settings_section_open[s] = (d[s] == '0') ? false : true;
            break;
        }
    }
    fclose(f);
}

/* Visible settings rows: every header plus, per open section, its option
 * rows. Built on demand; holds indices into settings_rows[]. */
static int settings_vis_rows[SETTINGS_ROW_N];
static int settings_vis_n = 0;

static void settings_build_vis_rows(void) {
    int n = 0;
    for (int i = 0; i < SETTINGS_ROW_N; i++) {
        int sec = settings_section_of(i);
        if (sec >= 0) {
            settings_vis_rows[n++] = i;                       /* header always visible */
            if (!settings_section_open[sec]) {
                /* skip the section's rows in one sweep */
                while (i + 1 < SETTINGS_ROW_N && settings_section_of(i + 1) < 0)
                    i++;
            }
        } else {
            settings_vis_rows[n++] = i;
        }
    }
    settings_vis_n = n;
}

/* Cursor position in the visible list; kept in sync with settings_menu_idx
 * (which stays an index into settings_rows[]). */
static int settings_vis_cursor(void) {
    for (int i = 0; i < settings_vis_n; i++)
        if (settings_vis_rows[i] == settings_menu_idx) return i;
    return 0;
}

static bool settings_row_visible(int i) {
    /* A row is visible unless it sits inside a COLLAPSED section: find the
     * nearest header above it; if that section is collapsed, the row is
     * hidden (headers themselves always stay visible). */
    if (i < 0 || i >= SETTINGS_ROW_N) return false;
    if (settings_section_of(i) >= 0) return true;   /* header */
    for (int s = i - 1; s >= 0; s--) {
        int sec = settings_section_of(s);
        if (sec >= 0)
            return settings_section_open[sec];
    }
    return true;   /* rows before any header: always visible */
}
static int settings_row_selectable(int i) {
    if (i < 0 || i >= SETTINGS_ROW_N) return 0;
    if (settings_rows[i].type == RT_ROM_SOURCE) return otg_roms_available();
    /* Headers are selectable now (they collapse/expand sections). */
    if (settings_rows[i].type == RT_HEADER) {
        int sec = settings_section_of(i);
        return sec >= 0;
    }
    return settings_rows[i].type != RT_INFO && settings_rows[i].type != RT_OTG_STATUS;
}

static const char *treefrogui_version(void) {
    static char version[32];
    static int loaded;
    if (!loaded) {
        FILE *f = fopen("/mnt/sdcard/cubegm/version.txt", "r");
        if (f) {
            if (!fgets(version, sizeof version, f)) version[0] = '\0';
            fclose(f);
            version[strcspn(version, "\r\n")] = '\0';
        }
        if (!version[0]) snprintf(version, sizeof version, "unknown");
        loaded = 1;
    }
    return version;
}

static bool remap_wizard_active = false;
static int  remap_step = 0;
static uint32_t remap_prev_raw = 0;

static void mkdir_p(const char *path) {
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    system(cmd);
}

static void settings_write_volume(void) {
    /* The shared system volume lives in cubevol's persistentmem slot (the
     * physical volume buttons' value); writing it also mirrors to the I2SO
     * hardware path and legacy sndgain.txt for standalone frontends.
     * Called on confirmed adjustments (menu exit / boot apply) AND as the
     * debounced commit of the live slider path (see settings_preview_row):
     * cube_pmem_volume_write itself skips the EEPROM write when the level
     * is unchanged, so repeated calls at the same value are free. */
    if (settings_volume < 0)   settings_volume = 0;
    if (settings_volume > 100) settings_volume = 100;
    if (cube_pmem_volume_write(settings_volume) < 0)
        dbg("volume persist failed (pmem); hw level applied, stored value unchanged");
}

static void settings_apply(void) {
    extern const int theme_count;
    if (settings_theme_idx < 0 || settings_theme_idx >= theme_count) settings_theme_idx = 0;
    if (settings_style < 0 || settings_style >= STYLE_COUNT) settings_style = STYLE_VERTICAL;
    if (settings_font_idx < 0 || settings_font_idx >= font_count) settings_font_idx = 0;
    if (settings_brightness < 0)   settings_brightness = 0;
    if (settings_brightness > 100) settings_brightness = 100;
    if (settings_background_dim < 0)   settings_background_dim = 0;
    if (settings_background_dim > 100) settings_background_dim = 100;
    theme_apply(settings_theme_idx);
    theme_sync_artwork_pack();
    if (font_count > 0)
        font_load_file(font_files[settings_font_idx]);
    font_sync_language_fallback();
    cube_set_backlight(settings_brightness);
    /* Keep cubevol's persistentmem value in sync so its delayed startup apply
     * shows the right brightness instead of flashing its stored default. */
    cube_pmem_backlight_sync(settings_brightness);
    banner_set_anim(settings_anim);
    banner_set_dim(settings_background_dim);
    /* Global output volume: there's no system mixer on this hardware, so every
     * frontend (picoarch, pcsx4all, lgpt) reads this percent from sndgain.txt and
     * scales its own audio. FrogUI is the single place to set it. */
    if (settings_volume < 0)   settings_volume = 0;
    if (settings_volume > 100) settings_volume = 100;
    settings_write_volume();
    /* FrogUI has no continuous audio unless menu sounds are explicitly enabled.
       Zeroed samples leave the DAC/amp live; mute its real I2SO output instead. */
    cube_set_i2so_output_muted(settings_menu_sounds ? 0 : 1);
}
/* Apply only the setting being previewed. The old generic settings_apply()
 * reloaded the current font and fsync'd sndgain.txt after every Left/Right press,
 * even for unrelated toggles. Persistent brightness/volume writes are deferred
 * until menu exit; their on-screen preview remains immediate. */
/* Live-volume debounce: the slider applies the level audibly on EVERY step
 * (hardware mirror), but commits the durable persistentmem write at most once
 * per VOLUME_COMMIT_MS while the user is still sliding, plus a final commit
 * shortly after the last step - so the physical volume meter follows the
 * slider in real time, without one EEPROM write per repeat step.  A step that
 * arrives later than the window commits immediately (slow adjustment = live
 * commit too).  The menu-exit path (settings_save_file) still writes the
 * final value unconditionally, covering any remaining edge. */
#define VOLUME_COMMIT_MS 500
static long long volume_last_commit_ms = 0;
static int volume_committed_level = -1;
static void settings_volume_commit_live(void) {
    long long now = monotonic_ms();
    if (settings_volume == volume_committed_level)
        return;   /* nothing new to persist */
    if (now - volume_last_commit_ms < VOLUME_COMMIT_MS &&
        volume_committed_level >= 0)
        return;   /* inside the debounce window; next step (or the final
                     commit below) will persist it */
    volume_last_commit_ms = now;
    volume_committed_level = settings_volume;
    settings_write_volume();
}
static void settings_volume_commit_final(void) {
    volume_committed_level = -1;   /* force a real commit check on next slide */
    volume_last_commit_ms = monotonic_ms();
}

static void settings_preview_row(const SRow *r) {
    if (!r) return;
    switch (r->type) {
    case RT_THEME:
        theme_apply(settings_theme_idx);
        theme_sync_artwork_pack();
        break;
    case RT_FONT:
        if (font_count > 0)
            font_load_file(font_files[settings_font_idx]);
        break;
    case RT_RANGE:
        if (r->val == &settings_brightness)
            cube_set_backlight(settings_brightness);
        else if (r->val == &settings_background_dim)
            banner_set_dim(settings_background_dim);
        else if (r->val == &settings_volume) {
            /* Live on EVERY step: the hardware mirror makes the change
             * audible immediately, and the debounced commit keeps the
             * stored value (physical meter) following the slider in
             * real time without per-step EEPROM writes. */
            cube_volume_preview(settings_volume);
            settings_volume_commit_live();
        }
        break;
    case RT_TOGGLE:
        if (r->val == &settings_anim) banner_set_anim(settings_anim);
        break;
    default:
        break;
    }
}

static void settings_load_file(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[256];
    extern const int theme_count;
    extern const Theme themes[];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        char *nl = strchr(val, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(val, '\r'); if (cr) *cr = '\0';
        if (strcmp(line, "theme") == 0) {
            /* v1.0.11 briefly shipped Horizontal as a colour theme. Preserve
             * that layout choice while migrating back to the default colours. */
            if (strcmp(val, "Horizontal") == 0) {
                settings_theme_idx = 0;
                settings_style = STYLE_HORIZONTAL;
                settings_friendly_names = 1;
            } else {
                for (int i = 0; i < theme_count; i++)
                    if (strcmp(themes[i].name, val) == 0) { settings_theme_idx = i; break; }
            }
        } else if (strcmp(line, "font") == 0) {
            for (int i = 0; i < font_count; i++)
                if (strcasecmp(font_files[i], val) == 0 ||
                    strcasecmp(font_disp[i], val) == 0) { settings_font_idx = i; break; }
        } else if (strcmp(line, "language") == 0) {
            for (int i = 0; i < LANGUAGE_COUNT; i++)
                if (strcasecmp(val, language_codes[i]) == 0) {
                    settings_language = i;
                    break;
                }
        } else if (strcmp(line, "wallpaper") == 0) {
            settings_wallpaper_idx = 0;   /* default None */
            for (int i = 1; i < wallpaper_count; i++)
                if (strcasecmp(wallpaper_files[i], val) == 0) { settings_wallpaper_idx = i; break; }
        } else if (strcmp(line, "wallpaper_fit") == 0) {
            for (int i = 0; i < WALL_FIT_N; i++)
                if (strcasecmp(wallpaper_fit_names[i], val) == 0) { settings_wallpaper_fit = i; break; }
        } else if (strcmp(line, "theme_pack") == 0) {
            for (int i = 0; i < theme_pack_count; i++)
                if (strcasecmp(theme_pack_files[i], val) == 0 ||
                    strcasecmp(theme_pack_disp[i], val) == 0) { settings_theme_pack_idx = i; break; }
        } else if (strcmp(line, "icon_pack") == 0) {
            settings_icon_pack_idx = 0;
            for (int i = 1; i < icon_pack_count; i++)
                if (strcasecmp(icon_pack_files[i], val) == 0 ||
                    strcasecmp(icon_pack_disp[i], val) == 0) { settings_icon_pack_idx = i; break; }
        } else if (strcmp(line, "brightness") == 0) {
            settings_brightness = atoi(val);
        } else if (strcmp(line, "volume") == 0) {
            settings_volume = atoi(val);
        } else if (strcmp(line, "filter") == 0) {
            for (int i = 0; i < FILTER_COUNT; i++)
                if (strcmp(filter_names[i], val) == 0) { settings_filter_idx = i; break; }
        } else if (strcmp(line, "animations") == 0) {
            settings_anim = (strcmp(val, "off") == 0) ? 0 : 1;
        } else if (strcmp(line, "menu_sounds") == 0) {
            settings_menu_sounds = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "style") == 0) {
            for (int i = 0; i < STYLE_COUNT; i++)
                if (strcasecmp(val, style_keys[i]) == 0) { settings_style = i; break; }
        } else if (strcmp(line, "center_text") == 0) {
            settings_center_text = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "horizontal_layout") == 0) {
            /* Migrate the short-lived toggle form without changing how that
             * user's carousel labels looked. */
            settings_style = (strcmp(val, "on") == 0)
                           ? STYLE_HORIZONTAL : STYLE_VERTICAL;
            if (settings_style == STYLE_HORIZONTAL)
                settings_friendly_names = 1;
        } else if (strcmp(line, "friendly_system_names") == 0) {
            settings_friendly_names = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "auto_resume") == 0) {
            settings_quick_resume = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "autosave_autoload") == 0) {
            settings_autosave_autoload = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "custom_aspect_ratios") == 0) {
            settings_custom_aspect_ratios = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "hide_empty") == 0) {
            settings_hide_empty = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "hide_extensions") == 0) {
            settings_hide_extensions = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "backgrounds") == 0) {
            settings_backgrounds = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "background_dim") == 0) {
            settings_background_dim = atoi(val);
        } else if (strcmp(line, "file_cache") == 0) {
            settings_file_cache = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "folder_cache") == 0) {
            /* Legacy alias: preserve the user's old cache preference. */
            settings_file_cache = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "battery_color") == 0) {
            settings_battery_color = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "disable_sleep") == 0) {
            settings_disable_sleep = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "game_switcher") == 0) {
            settings_game_switcher = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "load_recents") == 0) {
            settings_load_recents = (strcmp(val, "on") == 0) ? 1 : 0;
        } else if (strcmp(line, "rom_source") == 0) {
            settings_rom_source = (strcasecmp(val, "otg") == 0)
                                ? ROM_SOURCE_OTG : ROM_SOURCE_SD;
        }
    }
    fclose(f);
    /* The shared system volume (cubevol persistentmem - what the PHYSICAL
     * volume buttons last set) always wins over our saved copy: the user
     * may have changed it with the buttons since the last Settings visit. */
    int shared_vol = cube_pmem_volume_read();
    if (shared_vol >= 0 && shared_vol <= 100)
        settings_volume = shared_vol;
}

/* The rootfs mdev hook mounts a connected OTG drive at /media/hdd. Check both
 * the mount table and its roms directory so a stale mountpoint never moves the
 * library away from the internal SD. */
static bool otg_roms_available(void) {
    FILE *mounts = fopen("/proc/mounts", "r");
    char device[256], target[256], fstype[64], options[256];
    int mounted = 0;
    struct stat st;
    if (!mounts) return false;
    while (fscanf(mounts, "%255s %255s %63s %255s %*d %*d",
                  device, target, fstype, options) == 4) {
        if (strcmp(target, OTG_MOUNT_PATH) == 0) { mounted = 1; break; }
    }
    fclose(mounts);
    return mounted && stat(OTG_ROMS_PATH, &st) == 0 && S_ISDIR(st.st_mode);
}

/* FROGUI_ROMS_PATH is retained for development. Normal use selects the SD or
 * a connected OTG drive through Settings; a missing OTG drive safely falls
 * back to the internal SD and is never created or modified. */
static void resolve_roms_root(void) {
    const char *override = getenv("FROGUI_ROMS_PATH");
    struct stat st;
    if (override && *override && strlen(override) < sizeof(g_roms_path) &&
        stat(override, &st) == 0 && S_ISDIR(st.st_mode)) {
        strncpy(g_roms_path, override, sizeof(g_roms_path) - 1);
        g_roms_path[sizeof(g_roms_path) - 1] = '\0';
        while (strlen(g_roms_path) > 1 && g_roms_path[strlen(g_roms_path)-1] == '/')
            g_roms_path[strlen(g_roms_path)-1] = '\0';
        return;
    }
    if (settings_rom_source == ROM_SOURCE_OTG && otg_roms_available()) {
        strncpy(g_roms_path, OTG_ROMS_PATH, sizeof(g_roms_path) - 1);
        g_roms_path[sizeof(g_roms_path) - 1] = '\0';
        return;
    }
    strcpy(g_roms_path, ROMS_PATH_DEFAULT);
}

static void settings_save_file(void) {
    extern const Theme themes[];
    mkdir_p(SETTINGS_DIR);
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { dbg("settings save: fopen failed"); return; }
    fprintf(f, "theme=%s\n", themes[settings_theme_idx].name);
    fprintf(f, "font=%s\n", font_count > 0 ? font_files[settings_font_idx] : "");
    fprintf(f, "language=%s\n", i18n_current_language());
    fprintf(f, "wallpaper=%s\n",
            (settings_wallpaper_idx > 0 && settings_wallpaper_idx < wallpaper_count)
            ? wallpaper_files[settings_wallpaper_idx] : "none");
    fprintf(f, "wallpaper_fit=%s\n", wallpaper_fit_names[settings_wallpaper_fit]);
    fprintf(f, "theme_pack=%s\n", settings_theme_pack_idx > 0 ?
            theme_pack_files[settings_theme_pack_idx] : "default");
    fprintf(f, "icon_pack=%s\n", settings_icon_pack_idx > 0 ?
            icon_pack_files[settings_icon_pack_idx] : "default");
    fprintf(f, "brightness=%d\n", settings_brightness);
    fprintf(f, "volume=%d\n", settings_volume);
    fprintf(f, "filter=%s\n", filter_names[settings_filter_idx]);
    fprintf(f, "auto_resume=%s\n", onoff_names[settings_quick_resume]);
    fprintf(f, "autosave_autoload=%s\n", onoff_names[settings_autosave_autoload]);
    fprintf(f, "custom_aspect_ratios=%s\n", onoff_names[settings_custom_aspect_ratios]);
    fprintf(f, "animations=%s\n", onoff_names[settings_anim]);
    fprintf(f, "menu_sounds=%s\n", onoff_names[settings_menu_sounds]);
    fprintf(f, "style=%s\n", style_keys[settings_style]);
    fprintf(f, "center_text=%s\n", onoff_names[settings_center_text]);
    fprintf(f, "friendly_system_names=%s\n", onoff_names[settings_friendly_names]);
    fprintf(f, "hide_empty=%s\n", onoff_names[settings_hide_empty]);
    fprintf(f, "hide_extensions=%s\n", onoff_names[settings_hide_extensions]);
    fprintf(f, "backgrounds=%s\n", onoff_names[settings_backgrounds]);
    fprintf(f, "background_dim=%d\n", settings_background_dim);
    fprintf(f, "file_cache=%s\n", onoff_names[settings_file_cache]);
    fprintf(f, "battery_color=%s\n", onoff_names[settings_battery_color]);
    fprintf(f, "game_switcher=%s\n", onoff_names[settings_game_switcher]);
    fprintf(f, "load_recents=%s\n", onoff_names[settings_load_recents]);
    fprintf(f, "rom_source=%s\n", rom_source_names[settings_rom_source]);
    fprintf(f, "disable_sleep=%s\n", onoff_names[settings_disable_sleep]);
    fprintf(f, "sections=%d%d%d%d%d\n",
            settings_section_open[0] ? 1 : 0, settings_section_open[1] ? 1 : 0,
            settings_section_open[2] ? 1 : 0, settings_section_open[3] ? 1 : 0,
            settings_section_open[4] ? 1 : 0);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    cube_pmem_backlight_sync(settings_brightness);
    settings_write_volume();
    sync();  /* SD-card flush */
    { char buf[64]; snprintf(buf, sizeof(buf), "settings save: filter=%s idx=%d",
                                                filter_names[settings_filter_idx], settings_filter_idx);
      dbg(buf); }
}

static const char* get_basename(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b+1 : path;
}

static const char* get_console_folder(const char *path) {
    size_t roms_len = strlen(ROMS_PATH);
    if (strncmp(path, ROMS_PATH, roms_len) == 0) {
        const char *sub = path + roms_len;
        if (*sub == '/') {
            sub++;
        }
        static char console[64];
        int i = 0;
        while (sub[i] != '\0' && sub[i] != '/' && i < 63) {
            console[i] = sub[i];
            i++;
        }
        console[i] = '\0';
        return console;
    }
    return NULL;
}

/* Extension filters use the current folder name as their stable key.  ROM
 * paths already resolve through get_console_folder(); app/media folders live
 * elsewhere on the card, so fall back to the final path component. */
static const char* get_filter_folder(const char *path) {
    const char *folder = get_console_folder(path);
    if (folder && folder[0]) return folder;
    const char *base = get_basename(path);
    return (base && base[0]) ? base : NULL;
}

/* JPG/PNG normally mean frontend artwork and stay hidden from ROM listings.
 * Inside the dedicated image libraries they are content, including in nested
 * album/comic folders, so those paths must opt out of the artwork filter. */
static int is_image_library_path(const char *path) {
    const char *folder = get_console_folder(path);
    return folder && (!strcasecmp(folder, "images") ||
                      !strcasecmp(folder, "photos"));
}

#define SETTINGS_ENTRY_NAME    ">> Settings"
#define RECENTS_ENTRY_NAME     ">> Recents"
#define FAVOURITES_ENTRY_NAME  ">> Favourites"

typedef struct {
    const char *folder;
    const char *label_en;
    char label[96];
} SystemLabel;

/* Folder names remain the stable lookup key for cores and artwork. The
 * horizontal picker gets friendlier names without changing that contract. */
static SystemLabel system_labels[] = {
    {"a26", "Atari 2600"}, {"a5200", "Atari 5200"}, {"a78", "Atari 7800"},
    {"a800", "Atari 8-bit"}, {"lnx", "Atari Lynx"},
    {"fc", "Nintendo Entertainment System"},
    {"nes", "Nintendo Entertainment System"}, {"nesq", "NES - QuickNES"},
    {"nest", "NES - Nestopia"}, {"fds", "Famicom Disk System"},
    {"sfc", "Super Nintendo"}, {"snes", "Super Nintendo"},
    {"snes02", "SNES - Fast"},
    {"gb", "Game Boy"}, {"gbc", "Game Boy Color"},
    {"gbgb", "Game Boy - Gearboy"},
    {"gbb", "Game Boy - TGB Dual"}, {"dblcherrygb", "Double Cherry GB"},
    {"gba", "Game Boy Advance"}, {"gbac", "GBA - Multicore"},
    {"gbav", "GBA - VBA Next"}, {"gbaf", "GBA - mGBA"},
    {"mgba", "GBA - mGBA"},
    {"md", "Mega Drive / Genesis"}, {"sms", "Master System"},
    {"sega", "Mega Drive / Genesis"}, {"gg", "Game Gear"},
    {"gpgx", "Genesis Plus GX"}, {"segacd", "Sega CD"}, {"32x", "Sega 32X"},
    {"pce", "PC Engine"}, {"pcesgx", "PC Engine SuperGrafx"},
    {"ngpc", "Neo Geo Pocket"}, {"neogeo", "Neo Geo"},
    {"geolith", "Neo Geo"},
    {"wswan", "WonderSwan"}, {"wsv", "WonderSwan - Potator"},
    {"vb", "Virtual Boy"}, {"pcfx", "PC-FX"},
    {"ps", "PlayStation"}, {"psx", "PlayStation"},
    {"ps1", "PlayStation"}, {"ps1r", "PlayStation - ReARMed"},
    {"psp", "PlayStation Portable"},
    {"arcade", "Arcade"}, {"m2k", "Arcade - MAME 2000"}, {"cps1", "Arcade - Capcom CPS-1"},
    {"cps2", "Arcade - Capcom CPS-2"}, {"cps3", "Arcade - Capcom CPS-3"},
    {"c64", "Commodore 64"}, {"c64sc", "Commodore 64"},
    {"c64f", "Commodore 64 - Frodo"}, {"c64fc", "Commodore 64 - Frodo"},
    {"amiga", "Commodore Amiga"}, {"vic20", "Commodore VIC-20"},
    {"atari-st", "Atari ST"}, {"msx", "MSX"}, {"msx2", "MSX2"},
    {"spec", "ZX Spectrum"}, {"zx", "ZX Spectrum"}, {"zx81", "ZX81"},
    {"col", "ColecoVision"}, {"amstrad", "Amstrad CPC"},
    {"amstradb", "Amstrad CPC Plus"}, {"thom", "Thomson MO / TO"},
    {"xmil", "Sharp X68000"}, {"pico286", "DOS / PC"},
    {"dos", "DOS"}, {"prboom", "Doom"}, {"doom", "Doom"},
    {"quake", "Quake"}, {"quake2", "Quake II"}, {"wolf3d", "Wolfenstein 3D"},
    {"outrun", "Out Run"}, {"cavestory", "Cave Story"},
    {"flashback", "Flashback"}, {"xrick", "Rick Dangerous"},
    {"jnb", "Jump 'n Bump"}, {"gw", "Game & Watch"},
    {"tic80", "TIC-80"}, {"pico8", "PICO-8"}, {"fake08", "PICO-8"},
    {"retro8", "PICO-8 - Retro8"}, {"lowres-nx", "LowRes NX"},
    {"pokem", "Pokemon Mini"}, {"int", "Intellivision"},
    {"fcf", "Fairchild Channel F"}, {"cdg", "CD+G Karaoke"},
    {"chip8", "CHIP-8"}, {"arduboy", "Arduboy"},
    {"arduous", "Arduboy - Accurate"}, {"vec", "Vectrex"},
    {"o2em", "Odyssey 2"}, {"gme", "Game Music"},
    {"gong", "Pong"}, {"vapor", "VaporSpec"}, {"rockbox", "Rockbox"},
    {"music", "Music Player"}, {"audio", "Music Player"}, {"videos", "Videos"},
    {"video", "Videos"}, {"images", "Images"}, {"photos", "Images"}
};

/* Keep friendly labels as a stable snapshot for the active locale. This keeps
 * the Games hierarchy independent of the in-place JSON parser buffer and
 * makes a language switch an explicit update rather than a deferred lookup. */
static void system_labels_refresh(void) {
    for (size_t i = 0; i < sizeof(system_labels) / sizeof(system_labels[0]); i++) {
        char key[96];
        snprintf(key, sizeof(key), "system.%s", system_labels[i].folder);
        snprintf(system_labels[i].label, sizeof(system_labels[i].label), "%s",
                 tr_or(key, system_labels[i].label_en));
    }
}

static const char *system_display_name(const char *folder) {
    if (strcmp(folder, SETTINGS_ENTRY_NAME) == 0) return tr("folder.settings");
    if (strcmp(folder, RECENTS_ENTRY_NAME) == 0) return tr("folder.recents");
    if (strcmp(folder, FAVOURITES_ENTRY_NAME) == 0) return tr("folder.favourites");
    if (!settings_friendly_names) return folder;
    for (size_t i = 0; i < sizeof(system_labels) / sizeof(system_labels[0]); i++)
        if (strcasecmp(folder, system_labels[i].folder) == 0) {
            return system_labels[i].label[0] ? system_labels[i].label
                                              : system_labels[i].label_en;
        }
    return folder;
}

#define BANNER_DIR SDCARD_BASE "/frogui"

static void load_banner_for_view(const char *path, bool is_recents, bool is_favourites) {
    if (!settings_backgrounds) { banner_clear(); return; }   /* solid theme bg */
    /* Single wallpaper (if picked): same image in every view, overrides the
     * per-system art. */
    if (settings_wallpaper_idx > 0 && settings_wallpaper_idx < wallpaper_count) {
        char wp[512];
        snprintf(wp, sizeof wp, "/mnt/sdcard/frogui/wallpapers/%s",
                 wallpaper_files[settings_wallpaper_idx]);
        if (access(wp, R_OK) == 0) { banner_load_fit(wp, settings_wallpaper_fit, COLOR_BG); return; }
    }
    char img[512];
    char banner_dir[512];
    if (settings_theme_pack_idx > 0 && settings_theme_pack_idx < theme_pack_count)
        snprintf(banner_dir, sizeof banner_dir, BANNER_DIR "/theme-packs/%s",
                 theme_pack_files[settings_theme_pack_idx]);
    else
        snprintf(banner_dir, sizeof banner_dir, "%s", BANNER_DIR);
    const char *exts[] = { "png", "jpg", "jpeg", "bmp", NULL };
    if (is_recents) {
        for (int i = 0; exts[i]; i++) {
            snprintf(img, sizeof(img), "%s/recents.%s", banner_dir, exts[i]);
            if (access(img, R_OK) == 0) { banner_load_fit(img, settings_wallpaper_fit, COLOR_BG); return; }
        }
    } else if (is_favourites) {
        for (int i = 0; exts[i]; i++) {
            snprintf(img, sizeof(img), "%s/favourites.%s", banner_dir, exts[i]);
            if (access(img, R_OK) == 0) { banner_load_fit(img, settings_wallpaper_fit, COLOR_BG); return; }
        }
    } else {
        const char *base = (path && *path) ? strrchr(path, '/') : NULL;
        const char *name = base ? base + 1 : (path ? path : "main");
        if (!name || !*name) name = "main";
        /* Theme packs historically used m2k/gb names. Keep those assets
         * working for the canonical arcade and Game Boy Color folders too. */
        const char *names[] = { name,
            strcasecmp(name, "arcade") == 0 ? "m2k" : NULL,
            strcasecmp(name, "gbc") == 0 ? "gb" : NULL, NULL };
        for (int n = 0; names[n]; n++)
            for (int i = 0; exts[i]; i++) {
                snprintf(img, sizeof(img), "%s/%s.%s", banner_dir, names[n], exts[i]);
                if (access(img, R_OK) == 0) { banner_load_fit(img, settings_wallpaper_fit, COLOR_BG); return; }
            }
        /* Fallback: try main.png for any view that has no folder-specific image */
        for (int i = 0; exts[i]; i++) {
            snprintf(img, sizeof(img), "%s/main.%s", banner_dir, exts[i]);
            if (access(img, R_OK) == 0) { banner_load_fit(img, settings_wallpaper_fit, COLOR_BG); return; }
        }
    }
    banner_clear();
}

/* True if `path` contains at least one game (any file that isn't artwork/metadata),
 * recursing into subfolders. Used to hide empty rom folders. Bounded depth. */
/* Directory test without a stat() syscall where the filesystem already knows the
 * type (FAT/ext fill dirent.d_type). Falls back to stat() only for DT_UNKNOWN.
 * stat() per file was the main scan cost on cards with thousands of ROMs. */
static int dirent_is_dir(const char *parent, const struct dirent *e) {
#ifdef DT_DIR
    if (e->d_type == DT_DIR) return 1;
    if (e->d_type == DT_REG || e->d_type == DT_LNK) {
        if (e->d_type == DT_REG) return 0;   /* known file */
    }
    if (e->d_type != DT_UNKNOWN && e->d_type != DT_LNK) return 0;
#endif
    char full[MAX_PATH_LEN];
    snprintf(full, sizeof full, "%s/%s", parent, e->d_name);
    struct stat st;
    if (stat(full, &st) != 0) return -1;     /* unreadable: caller skips */
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* dirs first, then case-insensitive alpha. */
static int direntry_cmp(const void *a, const void *b) {
    const DirEntry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static int folder_has_games(const char *path, int depth) {
    if (depth > 3) return 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    /* get_console_folder returns a static buffer; copy it before recursing. */
    char sys_folder[64];
    const char *sf = get_filter_folder(path);
    if (sf) { strncpy(sys_folder, sf, sizeof(sys_folder) - 1); sys_folder[sizeof(sys_folder) - 1] = '\0'; }
    else sys_folder[0] = '\0';
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        int isdir = dirent_is_dir(path, e);
        if (isdir < 0) continue;
        if (isdir) {
            char full[MAX_PATH_LEN];
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            if (folder_has_games(full, depth + 1)) { found = 1; break; }
        } else {
            size_t nlen = strlen(e->d_name);
            int is_p8png = nlen >= 7 && strcasecmp(e->d_name + nlen - 7, ".p8.png") == 0;
            const char *ext = strrchr(e->d_name, '.');
            if (ext && !is_p8png && !is_image_library_path(path) &&
                (strcasecmp(ext,".csv")==0 || strcasecmp(ext,".txt")==0 ||
                 strcasecmp(ext,".xml")==0 || strcasecmp(ext,".jpg")==0 ||
                 strcasecmp(ext,".png")==0)) continue;
            /* Extension already isolated once above (review: normalize/resolve
             * per entry, not per API call) - pass it, or "" when there is none. */
            if (ext_filter_should_hide(sys_folder[0] ? sys_folder : NULL,
                                       ext ? ext + 1 : "")) continue;
            found = 1; break;
        }
    }
    closedir(d);
    return found;
}

/* ---- Folder listing cache (OnionOS-style, defensive) ----------------------
 * Skip the readdir + per-folder empty-check + sort on a repeat visit when the
 * directory hasn't changed. Keyed on the directory's mtime: adding/removing a
 * file bumps mtime, so a stale cache can't survive a real change. Every read is
 * bounds-checked; ANY inconsistency (bad magic, path mismatch, mtime/hide_empty
 * change, short/corrupt file, alloc fail) falls through to a full rescan, which
 * then rewrites the cache. Cache lives in frogui/.cache/, never in the rom dirs,
 * and the whole feature is behind settings_file_cache so it can be turned off. */
#define CACHE_DIR   SETTINGS_DIR "/.cache"
#define CACHE_MAGIC 0x33435546u    /* "FUC3" — bumped when the listing content
                                     * changes WITHOUT the dir mtime moving
                                     * (e.g. extension-whitelist edits). */
#define CACHE_MAX_ENTRIES 100000   /* sanity cap to reject a corrupt count */

static void cache_file_for(const char *path, char *out, size_t n) {
    /* djb2 hash of the path -> stable filename (exact path re-checked on load,
     * so a hash collision can't return the wrong listing). */
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        h = ((h << 5) + h) ^ *p;
    snprintf(out, n, CACHE_DIR "/%08lx.bin", h & 0xffffffffUL);
}

static void cache_clear(void) {
    DIR *dir = opendir(CACHE_DIR);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t len = strlen(e->d_name);
        if (len < 4 ||
            (strcasecmp(e->d_name + len - 4, ".bin") != 0 &&
             strcasecmp(e->d_name + len - 4, ".tmp") != 0))
            continue;
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof path, CACHE_DIR "/%s", e->d_name);
        unlink(path);
    }
    closedir(dir);
}

/* Fill entries[]/entry_count from cache if valid for (path, mtime, hide_empty).
 * Returns 1 on a good hit, 0 to force a rescan. */
static int cache_load(const char *path, uint64_t mtime, int at_root) {
    char cf[MAX_PATH_LEN];
    cache_file_for(path, cf, sizeof cf);
    FILE *f = fopen(cf, "rb");
    if (!f) return 0;
    int ok = 0;
    uint32_t magic = 0, npath = 0, hide = 0, count = 0;
    uint64_t m = 0;
    char pbuf[MAX_PATH_LEN];
    if (fread(&magic, 4, 1, f) != 1 || magic != CACHE_MAGIC) goto done;
    if (fread(&m, 8, 1, f) != 1 || m != mtime) goto done;
    if (fread(&hide, 4, 1, f) != 1) goto done;
    /* hide_empty only changes the ROOT listing; ignore it elsewhere. */
    if (at_root && hide != (uint32_t)(settings_hide_empty ? 1 : 0)) goto done;
    if (fread(&npath, 4, 1, f) != 1 || npath == 0 || npath >= sizeof pbuf) goto done;
    if (fread(pbuf, 1, npath, f) != npath) goto done;
    pbuf[npath] = '\0';
    if (strcmp(pbuf, path) != 0) goto done;              /* collision guard */
    if (fread(&count, 4, 1, f) != 1 || count > CACHE_MAX_ENTRIES) goto done;

    entry_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t isdir; uint16_t nlen;
        if (fread(&isdir, 1, 1, f) != 1) goto done;
        if (fread(&nlen, 2, 1, f) != 1 || nlen > 255) goto done;
        if (entry_count >= entry_capacity) {
            int nc = entry_capacity ? entry_capacity * 2 : INITIAL_ENTRIES_CAPACITY;
            DirEntry *ne = realloc(entries, nc * sizeof(DirEntry));
            if (!ne) goto done;
            entries = ne; entry_capacity = nc;
        }
        if (fread(entries[entry_count].name, 1, nlen, f) != nlen) goto done;
        entries[entry_count].name[nlen] = '\0';
        entries[entry_count].is_dir = isdir ? 1 : 0;
        entry_count++;
    }
    ok = 1;
done:
    fclose(f);
    if (!ok) entry_count = 0;   /* partial read: discard, caller rescans */
    return ok;
}

static void cache_store(const char *path, uint64_t mtime, int at_root) {
    mkdir(CACHE_DIR, 0755);
    char cf[MAX_PATH_LEN], tmp[MAX_PATH_LEN];
    cache_file_for(path, cf, sizeof cf);
    snprintf(tmp, sizeof tmp, "%s.tmp", cf);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;                 /* can't write cache: not fatal, just skip */
    uint32_t magic = CACHE_MAGIC, hide = at_root ? (settings_hide_empty ? 1 : 0) : 0;
    uint32_t npath = (uint32_t)strlen(path), count = (uint32_t)entry_count;
    int good = 1;
    good &= fwrite(&magic, 4, 1, f) == 1;
    good &= fwrite(&mtime, 8, 1, f) == 1;
    good &= fwrite(&hide, 4, 1, f) == 1;
    good &= fwrite(&npath, 4, 1, f) == 1;
    good &= fwrite(path, 1, npath, f) == npath;
    good &= fwrite(&count, 4, 1, f) == 1;
    for (int i = 0; good && i < entry_count; i++) {
        uint8_t isdir = entries[i].is_dir ? 1 : 0;
        uint16_t nlen = (uint16_t)strlen(entries[i].name);
        good &= fwrite(&isdir, 1, 1, f) == 1;
        good &= fwrite(&nlen, 2, 1, f) == 1;
        good &= fwrite(entries[i].name, 1, nlen, f) == nlen;
    }
    fclose(f);
    if (good) rename(tmp, cf);      /* atomic swap; a half-write never gets read */
    else      unlink(tmp);
}

static void scan_directory(const char *path) {
    /* An unopenable dir is NOT an early return: fall through with an empty
     * listing so the root still gets its Settings/Recents rows. The old
     * early-return left entries == NULL while the renderer drew the list →
     * NULL deref (SIGBUS addr=0) and a boot crash-loop on cards without roms/. */
    entry_count = 0;
    int at_root = strcmp(path, ROMS_PATH) == 0;

    /* Cache fast path: if the dir is unchanged (mtime match), load the sorted
     * listing from disk and skip readdir/stat/empty-check/sort entirely. */
    struct stat dst;
    int have_mtime = (stat(path, &dst) == 0);
    int cached = settings_file_cache && !is_image_library_path(path) && have_mtime &&
                 cache_load(path, (uint64_t)dst.st_mtime, at_root);

    if (!cached) {
        DIR *dir = opendir(path);
        struct dirent *e;
        /* Static buffer inside get_console_folder: copy before the loop — the
         * hide-empty branch below calls folder_has_games(), which reuses it. */
        char sys_folder[64];
        const char *sf = get_filter_folder(path);
        if (sf) { strncpy(sys_folder, sf, sizeof(sys_folder) - 1); sys_folder[sizeof(sys_folder) - 1] = '\0'; }
        else sys_folder[0] = '\0';
        int extf_on = ext_filter_folder_active(sys_folder[0] ? sys_folder : NULL);
        while (dir && (e = readdir(dir)) != NULL) {
            if (e->d_name[0] == '.') continue;
            int isdir = dirent_is_dir(path, e);   /* d_type, stat() only if unknown */
            if (isdir < 0) continue;
            if (!isdir) {
                const char *ext = strrchr(e->d_name, '.');
                /* PICO-8 carts are .p8.png — keep them; only skip plain .png artwork. */
                size_t nlen = strlen(e->d_name);
                int is_p8png = nlen >= 7 && strcasecmp(e->d_name + nlen - 7, ".p8.png") == 0;
                if (ext && !is_p8png && !is_image_library_path(path) &&
                           (strcasecmp(ext,".csv")==0 || strcasecmp(ext,".txt")==0 ||
                            strcasecmp(ext,".xml")==0 || strcasecmp(ext,".jpg")==0 ||
                            strcasecmp(ext,".png")==0)) continue;
                /* Per-system extension whitelist (PS1: keep .cue/.m3u/..., hide
                 * companion .bin track blobs). Only files are filtered; every
                 * other system shows everything as before. The extension was
                 * already isolated once above (review: normalize per entry,
                 * never per API call) - pass it, or "" when there is none. */
                if (ext_filter_should_hide(sys_folder[0] ? sys_folder : NULL,
                                           ext ? ext + 1 : "")) continue;
            }
            /* Always hide the internal "menu" folder at the root. */
            if (isdir && at_root && strcasecmp(e->d_name, "menu") == 0) continue;
            if (isdir && at_root && is_psp_folder(e->d_name)) continue;
            /* Media libraries live under Apps, not in the Games tab. */
            if (isdir && at_root && is_app_folder_name(e->d_name)) continue;
            /* Inside a filtered system, per-game folders with no whitelisted
             * file (e.g. a folder left behind after a dump was moved out) read
             * as broken rows; skip them for the same reason as hide-empty. */
            if (isdir && extf_on) {
                char full[MAX_PATH_LEN];
                snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
                if (!folder_has_games(full, 0)) continue;
            }
            /* Hide-empty-folders: at the root, skip rom folders with no games.
             * Only reached for the handful of root folders, never per-ROM. */
            if (isdir && settings_hide_empty && at_root) {
                char full[MAX_PATH_LEN];
                snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
                if (!folder_has_games(full, 0)) continue;
            }
            if (entry_count >= entry_capacity) {
                entry_capacity = entry_capacity ? entry_capacity*2 : INITIAL_ENTRIES_CAPACITY;
                entries = realloc(entries, entry_capacity * sizeof(DirEntry));
                if (!entries) { closedir(dir); return; }
            }
            strncpy(entries[entry_count].name, e->d_name, 255);
            entries[entry_count].name[255] = '\0';
            entries[entry_count].is_dir = isdir;
            entry_count++;
        }
        if (dir) closedir(dir);
        /* Sort: dirs first, then alpha. qsort (n log n) — the old n^2 bubble sort
         * made large folders (thousands of ROMs) crawl. */
        if (entry_count > 1)
            qsort(entries, entry_count, sizeof(DirEntry), direntry_cmp);
        /* Persist the freshly-scanned listing for next time. */
        if (settings_file_cache && have_mtime)
            cache_store(path, (uint64_t)dst.st_mtime, at_root);
    }
    /* Cached roots may predate the Apps split; apply the same exclusions after
     * loading a cache so media folders never leak back into Games. */
    if (at_root && entry_count > 0) {
        int write = 0;
        for (int read = 0; read < entry_count; read++) {
            if (entries[read].is_dir &&
                (strcasecmp(entries[read].name, "menu") == 0 ||
                 is_psp_folder(entries[read].name) ||
                 is_app_folder_name(entries[read].name))) continue;
            if (write != read) entries[write] = entries[read];
            write++;
        }
        entry_count = write;
    }
    /* Favourites remains part of the Games library. Recents and Settings are
     * real top-level tabs and must never appear as fake system folders. */
    if (strcmp(path, ROMS_PATH) == 0) {
        int has_favs    = favorites_get_count()    > 0 ? 1 : 0;
        int extras = has_favs;
        while (entry_count + extras > entry_capacity) {
            entry_capacity = entry_capacity ? entry_capacity*2 : INITIAL_ENTRIES_CAPACITY;
            entries = realloc(entries, entry_capacity * sizeof(DirEntry));
            if (!entries) goto done;
        }
        if (has_favs) {
            memmove(&entries[1], &entries[0], entry_count * sizeof(DirEntry));
            strncpy(entries[0].name, FAVOURITES_ENTRY_NAME, 255);
            entries[0].name[255] = '\0';
            entries[0].is_dir = 0;
            entry_count++;
        }
    }
done:
    if (!entries) entry_count = 0;   /* belt: renderer must never walk NULL */
    viewing_recents = false;
    viewing_favourites = false;
    viewing_apps = false;
    viewing_activity = false;
    selected_index = 0;
    scroll_offset  = 0;
}

/* Total seconds played for a game, from picoarch's playtime.txt. */
static long playtime_lookup(const char *path) {
    if (!path || !*path) return 0;
    FILE *f = fopen("/mnt/sdcard/frogui/playtime.txt", "r");
    if (!f) return 0;
    char line[1100]; long sec = 0;
    while (fgets(line, sizeof line, f)) {
        char *t = strchr(line, '\t'); if (!t) continue;
        *t = 0; char *p = t + 1; p[strcspn(p, "\r\n")] = 0;
        if (!strcmp(p, path)) { sec = atol(line); break; }
    }
    fclose(f);
    return sec;
}

/* ---------------- OnionOS-style game switcher (recents as box-art carousel) ----
 * Art = the newest gameplay screenshot picoarch wrote (.scr.bmp or .st<N>.bmp);
 * if no capture exists, fall back to box art/title artwork. Toggled by
 * settings_game_switcher. */
#include "stb_image.h"

/* Newest save-state screenshot for a game: /mnt/sdcard/picoarch/<tag>/<base>.st<N>.bmp */
static int switcher_savestate_bmp(const char *full_path, char *out, size_t n) {
    char dir[640]; strncpy(dir, full_path, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *sl = strrchr(dir, '/'); if (!sl) return 0;
    char base[256]; strncpy(base, sl + 1, sizeof base - 1); base[sizeof base - 1] = 0;
    *sl = 0;
    char *tagsl = strrchr(dir, '/'); const char *tag = tagsl ? tagsl + 1 : dir;
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;     /* strip extension */
    /* The ROM browser preserves the card's folder spelling (often NES),
     * while picoarch's config tag is normalized to lowercase (nes).  Probe
     * both forms; otherwise valid .scr.bmp files silently fall back to art. */
    char lower_tag[sizeof base];
    size_t tag_len = strlen(tag);
    if (tag_len >= sizeof lower_tag) tag_len = sizeof lower_tag - 1;
    for (size_t i = 0; i < tag_len; i++)
        lower_tag[i] = (char)tolower((unsigned char)tag[i]);
    lower_tag[tag_len] = '\0';
    const char *tags[2] = { tag, lower_tag };
    for (int ti = 0; ti < 2; ti++) {
        if (ti == 1 && !strcasecmp(tags[0], tags[1])) continue;
        /* dedicated per-game last-screen snapshot (written on menu entry) */
        snprintf(out, n, "/mnt/sdcard/picoarch/%s/%s.scr.bmp", tags[ti], base);
        if (access(out, F_OK) == 0) {
            dbg("recents: using gameplay screenshot");
            return 1;
        }
        for (int slot = 9; slot >= 0; slot--) {             /* then save states */
            snprintf(out, n, "/mnt/sdcard/picoarch/%s/%s.st%d.bmp", tags[ti], base, slot);
            if (access(out, F_OK) == 0) {
                dbg("recents: using save-state screenshot");
                return 1;
            }
        }
    }
    dbg("recents: no screenshot found");
    return 0;
}

static void switcher_blit565(uint16_t *fb, const uint16_t *src, const uint8_t *alpha,
                             int sw, int sh, int bx, int by, int bw, int bh) {
    if (!src || sw <= 0 || sh <= 0) return;
    int dw = bw, dh = sh * bw / sw;
    if (dh > bh) { dh = bh; dw = sw * bh / sh; }
    int ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        const uint16_t *r = src + (size_t)sy * sw;
        const uint8_t  *ar = alpha ? alpha + (size_t)sy * sw : NULL;
        uint16_t *d = fb + (size_t)(oy + y) * SCREEN_WIDTH + ox;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            unsigned a = ar ? ar[sx] : 255;
            if (a == 0) continue;
            uint16_t px = r[sx];
            if (a == 255) { d[x] = px; continue; }
            unsigned sr = (px  >> 11) & 0x1F, sg = (px  >> 5) & 0x3F, sb = px  & 0x1F;
            unsigned dr = (d[x]>> 11) & 0x1F, dg = (d[x]>> 5) & 0x3F, db = d[x] & 0x1F;
            d[x] = (uint16_t)((((sr*a + dr*(255-a))/255) << 11) |
                              (((sg*a + dg*(255-a))/255) << 5)  |
                               ((sb*a + db*(255-a))/255));
        }
    }
}

static void render_game_switcher_header(uint16_t *framebuffer, int barh) {
    render_fill_rect(framebuffer, 0, 0, SCREEN_WIDTH, barh, COLOR_LEGEND_BG);
    render_tabs(framebuffer, MAIN_TAB_RECENTS, COLOR_LEGEND_BG);
}

static void render_game_switcher(uint16_t *framebuffer) {
    const RecentGame *list = recent_games_get_list();
    int n = recent_games_get_count();
    int barh = UI_S(30);
    if (n <= 0) {
        render_game_switcher_header(framebuffer, barh);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, SCREEN_HEIGHT/2,
                       "No recent games yet", COLOR_TEXT);
        render_legend(framebuffer, LEGEND_X_NONE, 0, 0);
        return;
    }
    if (selected_index < 0) selected_index = 0;
    if (selected_index >= n) selected_index = n - 1;
    const RecentGame *g = &list[selected_index];

    /* Onion-style: keep the capture at full panel geometry in both modes and
     * paint UI on top. Resizing around the bars distorts gameplay captures. */
    int bx = 0, by = 0;
    int bw = SCREEN_WIDTH, bh = SCREEN_HEIGHT;
    /* Decoding PNG/JPG/BMP from FAT is the expensive part of entering Recents.
     * Compose the selected capture to panel-sized RGB565 once, then every
     * transition/redraw is only a memcpy. */
    static uint16_t *cached_frame = NULL;
    static int cached_pixels = 0, cached_drawn = 0;
    static char cached_game[MAX_PATH_LEN] = "";
    int pixels = bw * bh;
    if (!cached_frame || cached_pixels != pixels ||
        strcmp(cached_game, g->full_path) != 0) {
        if (cached_pixels != pixels) {
            free(cached_frame);
            cached_frame = (uint16_t *)malloc((size_t)pixels * sizeof(uint16_t));
            cached_pixels = cached_frame ? pixels : 0;
        }
        cached_drawn = 0;
        if (cached_frame) memset(cached_frame, 0, (size_t)pixels * sizeof(uint16_t));

        char path[1024];
        Thumbnail tb;
        /* A Recents card should show where the player left off. Only use
         * curated artwork when no gameplay capture exists yet. */
        if (cached_frame && switcher_savestate_bmp(g->full_path, path, sizeof path)) {
            int w, h, ch; unsigned char *img = stbi_load(path, &w, &h, &ch, 3);
            if (img) {
            /* Gameplay captures use the core's raw pixel geometry. PS1 in
             * particular can produce 256/320/368/512/640-wide modes whose
             * pixels are meant to be stretched by the display, not shown
             * square. Fill the switcher viewport just like live gameplay;
             * aspect-preserving fit made 640x240 captures look squashed into
             * a wide strip. Box art still uses switcher_blit565() above. */
            for (int y = 0; y < bh; y++) {
                const unsigned char *rr = img + (size_t)(y * h / bh) * w * 3;
                uint16_t *d = cached_frame + (size_t)(by + y) * SCREEN_WIDTH + bx;
                for (int x = 0; x < bw; x++) {
                    const unsigned char *p = rr + (size_t)(x * w / bw) * 3;
                    d[x] = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
                }
            }
                stbi_image_free(img);
                cached_drawn = 1;
            }
        }
        if (cached_frame && !cached_drawn &&
            load_game_artwork(g->full_path, ARTWORK_BOXART, &tb) && tb.data) {
            switcher_blit565(cached_frame, tb.data, tb.alpha,
                             tb.width, tb.height, bx, by, bw, bh);
            free_thumbnail(&tb);
            cached_drawn = 1;
        }
        if (cached_frame && !cached_drawn &&
            load_game_artwork(g->full_path, ARTWORK_TITLE_SCREEN, &tb) && tb.data) {
            switcher_blit565(cached_frame, tb.data, tb.alpha,
                             tb.width, tb.height, bx, by, bw, bh);
            free_thumbnail(&tb);
            cached_drawn = 1;
        }
        strncpy(cached_game, g->full_path, sizeof cached_game - 1);
        cached_game[sizeof cached_game - 1] = '\0';
    }
    if (cached_frame)
        memcpy(framebuffer, cached_frame, (size_t)pixels * sizeof(uint16_t));
    else
        render_fill_rect(framebuffer, bx, by, bw, bh, 0x0000);
    if (!cached_drawn)
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, bx + UI_S(16), by + bh/2,
                       "(no screenshot - open the in-game menu once)", COLOR_TEXT);

    /* Bottom overlay: detailed mode carries play info; fullscreen keeps only
     * the selected game and navigation, matching Onion's minimal view. */
    int byb = SCREEN_HEIGHT - barh;
    render_fill_rect(framebuffer, 0, byb, SCREEN_WIDTH, barh, COLOR_SELECT_BG);
    if (game_switcher_fullscreen) {
        int nw = font_measure_text(g->game_name);
        int nx = (SCREEN_WIDTH - nw) / 2;
        if (selected_index > 0)
            font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING,
                           byb + UI_S(8), "<", COLOR_SELECT_TEXT);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, nx,
                       byb + UI_S(8), g->game_name, COLOR_SELECT_TEXT);
        if (selected_index + 1 < n)
            font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                           SCREEN_WIDTH - PADDING - font_measure_text(">"),
                           byb + UI_S(8), ">", COLOR_SELECT_TEXT);
    } else {
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING,
                       byb + UI_S(8), g->game_name, COLOR_SELECT_TEXT);
        char info[96];
        long secs = playtime_lookup(g->full_path);
        if (secs >= 3600)
            snprintf(info, sizeof info, "Played %ldh %ldm   %d/%d", secs/3600, (secs%3600)/60, selected_index + 1, n);
        else if (secs >= 60)
            snprintf(info, sizeof info, "Played %ldm   %d/%d", secs/60, selected_index + 1, n);
        else if (secs > 0)
            snprintf(info, sizeof info, "Played %lds   %d/%d", secs, selected_index + 1, n);
        else
            snprintf(info, sizeof info, "%d/%d", selected_index + 1, n);
        int iw = font_measure_text(info);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       SCREEN_WIDTH - iw - PADDING, byb + UI_S(8),
                       info, COLOR_SELECT_TEXT);
        render_game_switcher_header(framebuffer, barh);
    }
}

/* Box-art side panel (Onion/muOS-style): right portion of the list view shows
 * the selected game's box art + name. Opaque card is drawn first so any
 * overflowing list-row text on the left is painted over. */
static void render_boxart_panel(uint16_t *fb, const char *full_path, const char *name) {
    int px   = SCREEN_WIDTH * 58 / 100;
    int top  = UI_S(44);
    int bot  = SCREEN_HEIGHT - UI_S(40);
    int pw   = SCREEN_WIDTH - px - UI_S(10);
    int ph   = bot - top;
    if (pw < UI_S(60) || ph < UI_S(60)) return;          /* too narrow to bother */

    /* Cache the decoded thumbnail; only re-decode when the selected game changes.
     * The old code ran stbi_load (PNG/JPG decode) EVERY frame -> scrolling crawled. */
    static char cached_path[1024] = "";
    static Thumbnail ctb; static int chas = 0;
    if (strcmp(full_path, cached_path) != 0) {
        if (chas) { free_thumbnail(&ctb); chas = 0; }
        if (load_game_artwork(full_path, ARTWORK_BOXART, &ctb) && ctb.data) chas = 1;
        /* Scrapers often provide a title screen rather than box art. */
        if (!chas && load_game_artwork(full_path, ARTWORK_TITLE_SCREEN, &ctb) && ctb.data)
            chas = 1;
        strncpy(cached_path, full_path, sizeof cached_path - 1);
        cached_path[sizeof cached_path - 1] = 0;
    }
    if (!chas) return;   /* no art → no panel */

    int nameh = UI_S(22);
    int aw = pw, ah = ph - nameh;
    /* Repaint the true background (banner slice or theme bg) instead of an
     * opaque card: covers list-text overflow, and transparent box art shows
     * the real background through it. */
    banner_fill_region(fb, px, top, pw, ph, COLOR_BG);
    switcher_blit565(fb, ctb.data, ctb.alpha, ctb.width, ctb.height, px, top, aw, ah);

    /* Name centered under the art, truncated to panel width. */
    char nm[64];
    int maxc = pw / UI_S(8); if (maxc > (int)sizeof(nm) - 1) maxc = sizeof(nm) - 1;
    int i = 0; for (; name[i] && i < maxc; i++) nm[i] = name[i];
    if (name[i]) { if (i > 2) i -= 2; nm[i++] = '.'; nm[i++] = '.'; }
    nm[i] = '\0';
    int tw = (int)strlen(nm) * UI_S(8);
    font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, px + (pw - tw) / 2, top + ah + UI_S(4),
                   nm, COLOR_TEXT);
}

/* Switch the browser into the recents view (used by the Recents entry and, when
 * "Start in Recents" is on, at startup). */
static void enter_recents_view(void) {
    const RecentGame *rg = recent_games_get_list();
    int rc = recent_games_get_count();
    while (rc > entry_capacity) {
        entry_capacity = entry_capacity ? entry_capacity*2 : INITIAL_ENTRIES_CAPACITY;
        DirEntry *ne = realloc(entries, entry_capacity * sizeof(DirEntry));
        if (!ne) return;
        entries = ne;
    }
    entry_count = 0;
    for (int i = 0; i < rc; i++) {
        strncpy(entries[entry_count].name, rg[i].game_name, 255);
        entries[entry_count].name[255] = '\0';
        entries[entry_count].is_dir = 0;
        entry_count++;
    }
    viewing_recents = true;
    game_switcher_fullscreen = false;
    selected_index = 0; scroll_offset = 0;
}

/* Activity Tracker reads the complete durable picoarch playtime ledger, so it
 * is not limited to the ten-item Recents list. */
static void enter_activity_view(void) {
	FILE *f = fopen("/mnt/sdcard/frogui/play_sessions.txt", "r");
	char line[MAX_PATH_LEN + 96];
	activity_count = 0;
	activity_has_dates = 0;
	while (f && fgets(line, sizeof line, f)) {
		char *a = strchr(line, '\t'); if (!a) continue;
		*a++ = '\0'; char *b = strchr(a, '\t'); if (!b) continue;
		*b++ = '\0'; b[strcspn(b, "\r\n")] = '\0';
		time_t when = (time_t)atol(line); long seconds = atol(a);
		/* RTC may be unset (epoch 0); duration and append order are still valid. */
		if (when < 0 || seconds <= 0 || !b[0]) continue;
		int found = -1;
		for (int i = 0; i < activity_count; i++)
			if (!strcmp(activity_paths[i], b)) { found = i; break; }
		if (found < 0 && activity_count < 128) {
			found = activity_count++;
			strncpy(activity_paths[found], b, MAX_PATH_LEN - 1);
			activity_paths[found][MAX_PATH_LEN - 1] = '\0';
		}
		if (found >= 0) {
			activity_seconds[found] += seconds; activity_runs[found]++;
			if (when > activity_last[found]) activity_last[found] = when;
		}
	}
	if (f) fclose(f);
	/* Backward compatibility: old installations have totals but no session dates. */
	if (activity_count == 0) {
		f = fopen("/mnt/sdcard/frogui/playtime.txt", "r");
		while (f && fgets(line, sizeof line, f) && activity_count < 128) {
			char *tab = strchr(line, '\t'); if (!tab) continue;
			*tab++ = '\0'; tab[strcspn(tab, "\r\n")] = '\0';
			long seconds = atol(line); if (seconds <= 0 || !tab[0]) continue;
			strncpy(activity_paths[activity_count], tab, MAX_PATH_LEN - 1);
			activity_paths[activity_count][MAX_PATH_LEN - 1] = '\0';
			activity_seconds[activity_count] = seconds; activity_runs[activity_count] = 1;
			activity_count++;
		}
		if (f) fclose(f);
	}
	/* Newest session first; without dates retain ledger order. */
	if (activity_has_dates || activity_count > 0) {
		for (int i = 0; i < activity_count; i++)
			if (activity_last[i] > 0) activity_has_dates = 1;
		if (activity_has_dates) for (int i = 0; i < activity_count; i++) {
			int best = i;
			for (int j = i + 1; j < activity_count; j++)
				if (activity_last[j] > activity_last[best]) best = j;
			if (best != i) {
				char p[MAX_PATH_LEN]; strcpy(p, activity_paths[i]); strcpy(activity_paths[i], activity_paths[best]); strcpy(activity_paths[best], p);
				long l = activity_seconds[i]; activity_seconds[i] = activity_seconds[best]; activity_seconds[best] = l;
				l = activity_runs[i]; activity_runs[i] = activity_runs[best]; activity_runs[best] = l;
				time_t d = activity_last[i]; activity_last[i] = activity_last[best]; activity_last[best] = d;
			}
		}
	}
    while (activity_count > entry_capacity) {
        entry_capacity = entry_capacity ? entry_capacity * 2 : INITIAL_ENTRIES_CAPACITY;
        DirEntry *ne = realloc(entries, (size_t)entry_capacity * sizeof(*entries));
        if (!ne) return;
        entries = ne;
    }
    entry_count = 0;
    for (int i = 0; i < activity_count; i++) {
        const char *base = strrchr(activity_paths[i], '/');
        base = base ? base + 1 : activity_paths[i];
        strncpy(entries[entry_count].name, base, 255);
        entries[entry_count].name[255] = '\0';
        char *dot = strrchr(entries[entry_count].name, '.'); if (dot) *dot = '\0';
        entries[entry_count].is_dir = 0;
        entry_count++;
    }
    viewing_activity = true; viewing_apps = false; apps_browsing = false;
    viewing_recents = false; viewing_favourites = false; viewing_search = false;
    selected_index = 0; scroll_offset = 0;
}

static void render_activity_graph(uint16_t *fb) {
    if (!fb || activity_count <= 0) return;
    int gx = SCREEN_WIDTH * 57 / 100;
    int gy = UI_S(48);
    int gw = SCREEN_WIDTH - gx - UI_S(16);
    int row = UI_S(38), max_rows = (SCREEN_HEIGHT - gy - UI_S(42)) / row;
    if (gw < UI_S(80) || max_rows <= 0) return;
    if (max_rows > 6) max_rows = 6;
    long max_seconds = 1;
    for (int i = 0; i < activity_count && i < max_rows; i++)
        if (activity_seconds[i] > max_seconds) max_seconds = activity_seconds[i];
    font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, gx, gy - UI_S(18),
                   "HOURS PLAYED", COLOR_HEADER);
    for (int i = 0; i < activity_count && i < max_rows; i++) {
        int y = gy + i * row;
        int bw = (int)((long)(gw - UI_S(8)) * activity_seconds[i] / max_seconds);
        if (bw < UI_S(3)) bw = UI_S(3);
        render_fill_rect(fb, gx, y, gw, UI_S(18), COLOR_LEGEND_BG);
        render_fill_rect(fb, gx, y, bw, UI_S(18), COLOR_SELECT_BG);
        char value[32];
        if (activity_seconds[i] >= 3600)
            snprintf(value, sizeof value, "%ldh %ldm", activity_seconds[i]/3600,
                     (activity_seconds[i]%3600)/60);
        else
            snprintf(value, sizeof value, "%ldm", activity_seconds[i]/60);
        font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, gx + UI_S(4), y + UI_S(3),
                       value, COLOR_SELECT_TEXT);
    }
}

static void render_activity_page(uint16_t *fb) {
    if (!fb) return;
    render_fill_rect(fb, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BG);
    render_tabs(fb, MAIN_TAB_APPS, COLOR_BG);
    long total = 0, runs = 0;
    for (int i = 0; i < activity_count; i++) { total += activity_seconds[i]; runs += activity_runs[i]; }
    char summary[128];
    snprintf(summary, sizeof summary, "%d GAMES   %ld RUNS   %ldH %02ldM TOTAL",
             activity_count, runs, total / 3600, (total % 3600) / 60);
    font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, UI_S(48), "PLAY ACTIVITY", COLOR_HEADER);
    font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, UI_S(70), summary, COLOR_TEXT);
    if (activity_count <= 0) {
        font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, UI_S(125),
                       "Play a game to start your activity history.", COLOR_TEXT);
        return;
    }
    long max_seconds = 1;
    for (int i = 0; i < activity_count; i++) if (activity_seconds[i] > max_seconds) max_seconds = activity_seconds[i];
    int row_h = UI_S(54), first = scroll_offset;
    int visible = (SCREEN_HEIGHT - UI_S(92) - UI_S(34)) / row_h;
    if (visible < 1) visible = 1;
    for (int n = 0; n < visible && first + n < activity_count; n++) {
        int i = first + n, y = UI_S(94) + n * row_h;
        if (i == selected_index) render_fill_rect(fb, 0, y - UI_S(4), SCREEN_WIDTH, row_h - UI_S(4), COLOR_LEGEND_BG);
        const char *base = strrchr(activity_paths[i], '/'); base = base ? base + 1 : activity_paths[i];
        char name[96]; strncpy(name, base, sizeof name - 1); name[sizeof name - 1] = '\0';
        char *dot = strrchr(name, '.'); if (dot) *dot = '\0';
        font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, name, COLOR_TEXT);
        char detail[96]; long s = activity_seconds[i];
        snprintf(detail, sizeof detail, "%ld runs   %ldh %02ldm total",
                 activity_runs[i], s / 3600, (s % 3600) / 60);
        font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y + UI_S(20), detail, COLOR_DISABLED);
        int bx = SCREEN_WIDTH * 52 / 100, bw = SCREEN_WIDTH - bx - PADDING;
        int fill = (int)((long)bw * s / max_seconds); if (fill < UI_S(3)) fill = UI_S(3);
        render_fill_rect(fb, bx, y + UI_S(9), bw, UI_S(12), COLOR_LEGEND_BG);
        render_fill_rect(fb, bx, y + UI_S(9), fill, UI_S(12), COLOR_SELECT_BG);
    }
    if (activity_count > visible)
        font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH - UI_S(92), SCREEN_HEIGHT - UI_S(42),
                       "UP/DOWN", COLOR_DISABLED);
}

static int games_tab_selected = 0, games_tab_scroll = 0;
static int recents_tab_selected = 0;
static int apps_tab_selected = 0;
static DirEntry *games_tab_entries = NULL;
static int games_tab_entry_count = 0;

/* Apps are virtual top-level entries. The on-card folder names remain stable
 * for compatibility, while the Apps tab presents friendly names and hides
 * media folders from the Games library. */
typedef struct { const char *key; const char *label_key; const char *label_en; const char *folder_a; const char *folder_b; const char *bin; } AppEntry;
static const AppEntry app_defs[] = {
    {"activity", "app.activity", "Activity Tracker", NULL, NULL, NULL},
    {"frogshell", "app.frogshell", "FrogShell", NULL, NULL, FROGSHELL_CORE},
    {"usbmode", "app.usb_mode", "USB mode", NULL, NULL, USB_MODE_BIN},
    {"ebook", "app.ebook_reader", "Ebook Reader", "Ebook", "ebooks", NULL},
    {"images", "app.image_viewer", "Image Viewer", "images", "photos", NULL},
    {"videos", "app.videos", "Videos", "videos", "video", NULL},
    {"music", "app.music_mp3", "Music / MP3", "music", "audio", NULL},
    {"rockbox", "app.rockbox", "Rockbox", "rockbox", NULL, NULL},
    {"shutdown", "app.shutdown", "Shutdown", NULL, NULL, SHUTDOWN_BIN},
};

static const char *app_folder_path(int index, char *out, size_t out_size) {
    if (index < 0 || index >= (int)(sizeof(app_defs) / sizeof(app_defs[0]))) return NULL;
    const AppEntry *a = &app_defs[index];
    if (!a->folder_a) return NULL;
    const char *candidates[3] = { a->folder_a, a->folder_b, NULL };
    for (int i = 0; candidates[i]; i++) {
        snprintf(out, out_size, "%s/%s", ROMS_PATH, candidates[i]);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return out;
    }
    return NULL;
}

static int is_app_folder_name(const char *name) {
    return name && (!strcasecmp(name, "rockbox") || !strcasecmp(name, "music") ||
                    !strcasecmp(name, "audio") || !strcasecmp(name, "videos") ||
                    !strcasecmp(name, "video") || !strcasecmp(name, "images") ||
                    !strcasecmp(name, "photos") || !strcasecmp(name, "ebook") ||
                    !strcasecmp(name, "ebooks"));
}

static void scan_apps_tab(void) {
    strncpy(current_path, ROMS_PATH, MAX_PATH_LEN - 1);
    current_path[MAX_PATH_LEN - 1] = '\0';
    apps_root_path[0] = '\0';
    entry_count = 0;
    const char *tf_device = getenv("TF_DEVICE");
    for (int i = 0; i < (int)(sizeof(app_defs) / sizeof(app_defs[0])); i++) {
        char path[MAX_PATH_LEN];
        /* SF3000 has a physical power toggle; expose software shutdown on
         * devices whose power control is momentary (SF3100/SF3500/R36*). */
        if (!strcmp(app_defs[i].key, "shutdown") && tf_device &&
            !strcasecmp(tf_device, "SF3000")) continue;
        if (!app_defs[i].bin && !strcmp(app_defs[i].key, "activity")) {
            if (access("/mnt/sdcard/frogui/playtime.txt", R_OK) != 0) continue;
        } else if (app_defs[i].bin) {
            if (access(app_defs[i].bin, X_OK) != 0) continue;
        } else if (!app_folder_path(i, path, sizeof path)) continue;
        if (entry_count >= entry_capacity) {
            entry_capacity = entry_capacity ? entry_capacity * 2 : INITIAL_ENTRIES_CAPACITY;
            entries = realloc(entries, (size_t)entry_capacity * sizeof(*entries));
            if (!entries) { entry_count = 0; return; }
        }
        strncpy(entries[entry_count].name, app_defs[i].key, sizeof(entries[entry_count].name) - 1);
        entries[entry_count].name[sizeof(entries[entry_count].name) - 1] = '\0';
        entries[entry_count].is_dir = 1;
        entry_count++;
    }
    viewing_apps = true;
    viewing_activity = false;
    apps_browsing = false;
    viewing_recents = false;
    viewing_favourites = false;
    viewing_search = false;
    selected_index = apps_tab_selected;
    if (selected_index >= entry_count) selected_index = entry_count > 0 ? entry_count - 1 : 0;
    if (selected_index < 0) selected_index = 0;
    scroll_offset = 0;
}

static const char *app_label(const char *key) {
    for (size_t i = 0; i < sizeof(app_defs) / sizeof(app_defs[0]); i++)
        if (strcasecmp(key, app_defs[i].key) == 0)
            return tr_or(app_defs[i].label_key, app_defs[i].label_en);
    return key;
}

static void save_games_tab_entries(void) {
    if (!entries || entry_count <= 0) {
        games_tab_entry_count = 0;
        return;
    }
    DirEntry *copy = realloc(games_tab_entries,
                             (size_t)entry_count * sizeof(*games_tab_entries));
    if (!copy) return;
    games_tab_entries = copy;
    memcpy(games_tab_entries, entries, (size_t)entry_count * sizeof(*entries));
    games_tab_entry_count = entry_count;
}

static int restore_games_tab_entries(void) {
    if (!games_tab_entries || games_tab_entry_count <= 0) return 0;
    if (games_tab_entry_count > entry_capacity) {
        DirEntry *grown = realloc(entries,
                                  (size_t)games_tab_entry_count * sizeof(*entries));
        if (!grown) return 0;
        entries = grown;
        entry_capacity = games_tab_entry_count;
    }
    memcpy(entries, games_tab_entries,
           (size_t)games_tab_entry_count * sizeof(*entries));
    entry_count = games_tab_entry_count;
    viewing_recents = false;
    viewing_favourites = false;
    viewing_search = false;
    return 1;
}

static int main_tab_active(void) {
    if (settings_menu_active) return MAIN_TAB_SETTINGS;
    if (viewing_recents) return MAIN_TAB_RECENTS;
    if (viewing_apps || apps_browsing || viewing_activity) return MAIN_TAB_APPS;
    return MAIN_TAB_GAMES;
}

static void switch_main_tab(int target) {
    int old = main_tab_active();
    if (target < MAIN_TAB_RECENTS || target > MAIN_TAB_SETTINGS || target == old)
        return;

    ui_transition_start(target > old ? 1 : -1);
    if (old == MAIN_TAB_GAMES) {
        games_tab_selected = selected_index;
        games_tab_scroll = scroll_offset;
        save_games_tab_entries();
    } else if (old == MAIN_TAB_RECENTS) {
        recents_tab_selected = selected_index;
    } else if (old == MAIN_TAB_APPS) {
        apps_tab_selected = selected_index;
    } else {
        settings_save_file();
        settings_menu_active = false;
    }

    if (target == MAIN_TAB_RECENTS) {
        settings_menu_active = false;
        viewing_apps = false;
        apps_browsing = false;
        viewing_activity = false;
        viewing_favourites = false;
        viewing_search = false;
        enter_recents_view();
        if (entry_count > 0) {
            selected_index = recents_tab_selected;
            if (selected_index >= entry_count) selected_index = entry_count - 1;
            if (selected_index < 0) selected_index = 0;
        }
    } else if (target == MAIN_TAB_GAMES) {
        settings_menu_active = false;
        viewing_recents = false;
        viewing_favourites = false;
        viewing_apps = false;
        apps_browsing = false;
        viewing_activity = false;
        viewing_search = false;
        strncpy(current_path, ROMS_PATH, MAX_PATH_LEN - 1);
        current_path[MAX_PATH_LEN - 1] = '\0';
        if (!restore_games_tab_entries())
            scan_directory(current_path);
        if (entry_count > 0) {
            selected_index = games_tab_selected;
            if (selected_index >= entry_count) selected_index = entry_count - 1;
            if (selected_index < 0) selected_index = 0;
            scroll_offset = games_tab_scroll;
            if (scroll_offset > selected_index) scroll_offset = selected_index;
            if (scroll_offset < 0) scroll_offset = 0;
        }
    } else if (target == MAIN_TAB_APPS) {
        settings_menu_active = false;
        scan_apps_tab();
        if (entry_count > 0) {
            selected_index = apps_tab_selected;
            if (selected_index >= entry_count) selected_index = entry_count - 1;
            if (selected_index < 0) selected_index = 0;
        }
    } else {
        viewing_recents = false;
        viewing_favourites = false;
        viewing_apps = false;
        apps_browsing = false;
        viewing_search = false;
        settings_menu_active = true;
        /* Headers are selectable now (they collapse/expand sections); just
         * make sure the cursor sits on a VISIBLE row. */
        settings_build_vis_rows();
        int found = 0;
        for (int i = 0; i < settings_vis_n; i++)
            if (settings_vis_rows[i] == settings_menu_idx) { found = 1; break; }
        if (!found) settings_menu_idx = settings_vis_rows[0];
        settings_filter_idx_on_enter = settings_filter_idx;
    }
}

/* Hand the active theme's colors to picoarch so its in-game menu matches FrogUI.
 * picoarch reads <exe_dir>/skin/skin.txt; its parser expects 24-bit RGB888 hex
 * (it converts to RGB565), so expand our RGB565 theme colors to RGB888. */
static unsigned rgb565_to_888(uint16_t c) {
    unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    unsigned r8 = (r5 << 3) | (r5 >> 2);
    unsigned g8 = (g6 << 2) | (g6 >> 4);
    unsigned b8 = (b5 << 3) | (b5 >> 2);
    return (r8 << 16) | (g8 << 8) | b8;
}
static void write_picoarch_skin(void) {
    extern uint16_t theme_text(void);
    extern uint16_t theme_select_text(void);
    extern uint16_t theme_select_bg(void);
    mkdir("/mnt/sdcard/cubegm/skin", 0777);
    FILE *s = fopen("/mnt/sdcard/cubegm/skin/skin.txt", "w");
    if (!s) return;
    /* Match FrogUI: normal rows in theme text colour, selected row = select-text
     * on the select-bg pill. */
    fprintf(s, "text_color=0x%06X\n",     rgb565_to_888(theme_text()));
    fprintf(s, "selection_color=0x%06X\n", rgb565_to_888(theme_select_bg()));
    fprintf(s, "sel_text_color=0x%06X\n",  rgb565_to_888(theme_select_text()));
    fprintf(s, "background_color=0x%06X\n", rgb565_to_888(theme_bg()));
    fclose(s);
}

static void request_game_launch(const char *core_path, const char *rom_path) {
    fb1_clear_all();
    cube_set_i2so_output_muted(0);  /* game owns the audio path after exec */
    write_picoarch_skin();
    FILE *f = fopen(LAUNCH_FILE, "w");
    if (!f) { dbg("failed to write launch file"); return; }
    fprintf(f, "%s\n%s\n", core_path, rom_path);
    fclose(f);
    /* Record in recent games history */
    const char *rom_base = strrchr(rom_path, '/');
    rom_base = rom_base ? rom_base + 1 : rom_path;
    char game_name[256];
    strncpy(game_name, rom_base, sizeof(game_name) - 1);
    game_name[sizeof(game_name) - 1] = '\0';
    char *dot = strrchr(game_name, '.');
    if (dot) *dot = '\0';
    recent_games_add(core_path, game_name, rom_path);
    sync(); /* flush FAT32 before exec */
    dbg("launch file written, requesting shutdown");
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

static void request_standalone_launch(const char *bin_path, const char *rom_path) {
    dbg("standalone_launch: start");
    fb1_clear_all();
    cube_set_i2so_output_muted(0);  /* standalone app owns the audio path */
    write_picoarch_skin();
    FILE *f = fopen(LAUNCH_FILE, "w");
    if (!f) { dbg("standalone_launch: fopen failed"); return; }
    fprintf(f, "standalone\n%s\n%s\n", bin_path, rom_path);
    fclose(f);
    dbg("standalone_launch: file written");
    const char *rom_base = strrchr(rom_path, '/');
    rom_base = rom_base ? rom_base + 1 : rom_path;
    char game_name[256];
    strncpy(game_name, rom_base, sizeof(game_name) - 1);
    game_name[sizeof(game_name) - 1] = '\0';
    char *dot = strrchr(game_name, '.');
    if (dot) *dot = '\0';
    /* Media playback is an app, not a game: keep MP3/video launches out of
     * Recents so that list remains useful for actual games. */
    if (strcmp(bin_path, ROCKBOX_BIN) != 0 && strcmp(bin_path, VIDEO_BIN) != 0 &&
        strcmp(bin_path, FROGSHELL_CORE) != 0) {
        dbg("standalone_launch: calling recent_games_add");
        recent_games_add(bin_path, game_name, rom_path);
    } else {
        dbg("standalone_launch: media app, skipping recents");
    }
    dbg("standalone_launch: calling environ_cb SHUTDOWN");
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    dbg("standalone_launch: after environ_cb");
}

/* Built-in utilities use the standalone exec contract but are not games and
 * therefore must not create Recents entries. */
static void request_builtin_launch(const char *bin_path) {
    dbg("builtin_launch: start");
    fb1_clear_all();
    cube_set_i2so_output_muted(0);  /* built-in standalone app may use audio */
    FILE *f = fopen(LAUNCH_FILE, "w");
    if (!f) { dbg("builtin_launch: fopen failed"); return; }
    fprintf(f, "standalone\n%s\n\n", bin_path);
    fclose(f);
    sync();
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

static bool is_ps1_folder(const char *folder) {
    if (!folder) return false;
    return strcasecmp(folder, "ps1") == 0 ||
           strcasecmp(folder, "psx") == 0 ||
           strcasecmp(folder, "PS")  == 0;
}

static bool is_pico286_folder(const char *folder) {
    return folder && strcasecmp(folder, "pico286") == 0;
}

static bool is_lgpt_folder(const char *folder) {
    return folder && strcasecmp(folder, "lgpt") == 0;
}

static bool is_psp_folder(const char *folder) {
    return folder && strcasecmp(folder, "psp") == 0;
}

/* A standalone-launched binary (run directly, not as a libretro core). */
static bool is_standalone_bin(const char *name) {
    return name && (strcmp(name, PCSX4ALL_BIN) == 0 ||
                    strcmp(name, PICO286_BIN)  == 0 ||
                    strcmp(name, LGPT_BIN)     == 0 ||
                    strcmp(name, ROCKBOX_BIN)  == 0 ||
                    strcmp(name, EBOOK_BIN)    == 0 ||
                    strcmp(name, VIDEO_BIN)    == 0 ||
                    strcmp(name, IMAGE_BIN)    == 0 ||
                    strcmp(name, PPSSPP_BIN)   == 0);
}

/* ----------------------------- Search (X button) ----------------------------- */

/* On-screen keyboard layout. Rows 0-3 are character keys; row 4 is special. */
static const char *KBD_ROWS[4] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
};
#define KBD_SPECIAL_ROW 4
#define KBD_NROWS       5
static const char *KBD_SPECIAL[3] = { "SPACE", "DEL", "GO" };

static int kbd_row_len(int r) {
    return (r == KBD_SPECIAL_ROW) ? 3 : (int)strlen(KBD_ROWS[r]);
}

static int str_icontains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

static void search_add_result(const char *name, const char *path) {
    if (search_results_count >= search_results_cap) {
        int nc = search_results_cap ? search_results_cap * 2 : 128;
        SearchResult *nr = realloc(search_results, nc * sizeof(SearchResult));
        if (!nr) return;
        search_results = nr; search_results_cap = nc;
    }
    SearchResult *r = &search_results[search_results_count];
    strncpy(r->name, name, sizeof(r->name) - 1); r->name[sizeof(r->name)-1] = '\0';
    strncpy(r->path, path, sizeof(r->path) - 1); r->path[sizeof(r->path)-1] = '\0';
    search_results_count++;
}

static void search_walk(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char sys_folder[64];
    const char *sf = get_console_folder(dir);
    if (sf) { strncpy(sys_folder, sf, sizeof(sys_folder) - 1); sys_folder[sizeof(sys_folder) - 1] = '\0'; }
    else sys_folder[0] = '\0';
    while ((e = readdir(d)) && search_results_count < 2000) {
        if (e->d_name[0] == '.') continue;
        char p[MAX_PATH_LEN];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!is_psp_folder(e->d_name) && depth < 3) search_walk(p, depth + 1);
        } else if (str_icontains(e->d_name, search_query)) {
            /* Same per-system whitelist as the browser, so search results never
             * resurrect the hidden companion files (PS1 .bin tracks). The
             * extension is isolated once here (review: normalize per entry). */
            const char *ext = strrchr(e->d_name, '.');
            if (ext_filter_should_hide(sys_folder[0] ? sys_folder : NULL,
                                       ext ? ext + 1 : "")) continue;
            search_add_result(e->d_name, p);
        }
    }
    closedir(d);
}

static void run_search(void) {
    search_results_count = 0;
    if (search_query[0])
        search_walk(search_scope[0] ? search_scope : ROMS_PATH, 0);

    /* mirror results into entries[] for the shared list renderer */
    while (search_results_count > entry_capacity) {
        entry_capacity = entry_capacity ? entry_capacity * 2 : INITIAL_ENTRIES_CAPACITY;
        entries = realloc(entries, entry_capacity * sizeof(DirEntry));
        if (!entries) return;
    }
    entry_count = 0;
    for (int i = 0; i < search_results_count; i++) {
        strncpy(entries[entry_count].name, search_results[i].name, 255);
        entries[entry_count].name[255] = '\0';
        entries[entry_count].is_dir = 0;
        entry_count++;
    }
    viewing_search = true;
    search_kbd_active = false;
    selected_index = 0; scroll_offset = 0;
}

/* Single launch path used by browse, search, favourites, and recents. Resolves
 * everything from the ROM path so all four agree — favourites/recents used to
 * launch the stored core_name directly and so missed the ps1-folder → standalone
 * pcsx4all preference (a ps1 favourite booted pcsx_rearmed instead). */
static void launch_by_path(const char *path) {
    const char *folder = get_console_folder(path);
    char dir[MAX_PATH_LEN];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    const char *ov = core_override_lookup(path, dir);
    const char *core = ov ? ov : get_core_for_folder(folder);
    if (!core) core = get_core_for_extension(path);
    if (ov)
        request_game_launch(ov, path);         /* explicit override → libretro */
    else if (core) {
        /* ps1 folder maps to pcsx_rearmed by default but we prefer the standalone
         * pcsx4all when present; every other standalone (pico286/lgpt/rockbox) is
         * caught generically by is_standalone_bin on the resolved core path. */
        if (is_ps1_folder(folder) && access(PCSX4ALL_BIN, F_OK) == 0)
            request_standalone_launch(PCSX4ALL_BIN, path);
        else if (is_psp_folder(folder) && access(PPSSPP_BIN, X_OK) == 0)
            request_standalone_launch(PPSSPP_BIN, path);
        else if (is_standalone_bin(core) && access(core, F_OK) == 0)
            request_standalone_launch(core, path);
        else
            request_game_launch(core, path);
    } else {
        dbg("launch: no core mapping for path");
    }
}

static void search_launch(int idx) {
    if (idx < 0 || idx >= search_results_count) return;
    launch_by_path(search_results[idx].path);
}

static void handle_search_kbd(void) {
    if (input_was_pressed(FROG_BTN_UP))    search_kbd_r = (search_kbd_r - 1 + KBD_NROWS) % KBD_NROWS;
    if (input_was_pressed(FROG_BTN_DOWN))  search_kbd_r = (search_kbd_r + 1) % KBD_NROWS;
    if (input_was_pressed(FROG_BTN_LEFT))  search_kbd_c--;
    if (input_was_pressed(FROG_BTN_RIGHT)) search_kbd_c++;
    { int rl = kbd_row_len(search_kbd_r);
      if (search_kbd_c < 0) search_kbd_c = rl - 1;
      if (search_kbd_c >= rl) search_kbd_c = 0; }
    if (input_was_pressed(FROG_BTN_A)) {
        int len = (int)strlen(search_query);
        if (search_kbd_r == KBD_SPECIAL_ROW) {
            if (search_kbd_c == 0) { if (len < (int)sizeof(search_query)-1) { search_query[len]=' '; search_query[len+1]='\0'; } }
            else if (search_kbd_c == 1) { if (len > 0) search_query[len-1] = '\0'; }
            else run_search();
        } else if (len < (int)sizeof(search_query)-1) {
            search_query[len] = KBD_ROWS[search_kbd_r][search_kbd_c];
            search_query[len+1] = '\0';
        }
    }
    if (input_was_pressed(FROG_BTN_Y)) {            /* quick backspace */
        int len = (int)strlen(search_query); if (len > 0) search_query[len-1] = '\0';
    }
    if (input_was_pressed(FROG_BTN_START)) run_search();
    if (input_was_pressed(FROG_BTN_B)) {           /* cancel → restore browser list */
        search_kbd_active = false;
        scan_directory(current_path);
        selected_index = 0; scroll_offset = 0;
    }
}

/* Extension-entry mode of the on-screen keyboard (review request): reuses
 * the search keyboard (same rows/keys) to let the user type a custom
 * extension for the folder's whitelist. START confirms (normalize lowercase
 * without dot, save via ext_filter_toggle_ext), B cancels back to the
 * picker with the cursor on the Add row. */
static bool extf_kbd_active = false;
static char extf_kbd_text[EXT_FILTER_EXT_LEN + 2];   /* room for dot typing */

/* Extension-entry keyboard (review request): same keys/rows as the search
 * keyboard. The special row's "search" slot acts as CONFIRM: normalize the
 * typed text to lowercase without the leading dot, add it to the folder's
 * whitelist (persisted), mark the picker dirty and drop back into it.
 * B cancels without saving. Text is capped at the extension length. */
static void handle_extf_kbd(void) {
    if (input_was_pressed(FROG_BTN_UP))    search_kbd_r = (search_kbd_r - 1 + KBD_NROWS) % KBD_NROWS;
    if (input_was_pressed(FROG_BTN_DOWN))  search_kbd_r = (search_kbd_r + 1) % KBD_NROWS;
    if (input_was_pressed(FROG_BTN_LEFT))  search_kbd_c--;
    if (input_was_pressed(FROG_BTN_RIGHT)) search_kbd_c++;
    { int rl = kbd_row_len(search_kbd_r);
      if (search_kbd_c < 0) search_kbd_c = rl - 1;
      if (search_kbd_c >= rl) search_kbd_c = 0; }
    int len = (int)strlen(extf_kbd_text);
    if (input_was_pressed(FROG_BTN_A)) {
        if (search_kbd_r == KBD_SPECIAL_ROW) {
            if (search_kbd_c == 0) {         /* space: ignore, no spaces in exts */
            } else if (search_kbd_c == 1) {  /* backspace */
                if (len > 0) extf_kbd_text[len-1] = '\0';
            } else {                         /* CONFIRM */
                if (ext_filter_toggle_ext(extf_folder, extf_kbd_text)) {
                    extf_dirty = true;
                    ui_toast_show("Extension added");
                } else {
                    ui_toast_show("Invalid or duplicate");
                }
                extf_kbd_active = false;
                return;
            }
        } else if (len < (int)sizeof(extf_kbd_text) - 1) {
            extf_kbd_text[len] = KBD_ROWS[search_kbd_r][search_kbd_c];
            extf_kbd_text[len+1] = '\0';
        }
    }
    if (input_was_pressed(FROG_BTN_Y)) {            /* quick backspace */
        if (len > 0) extf_kbd_text[len-1] = '\0';
    }
    if (input_was_pressed(FROG_BTN_START)) {        /* START also confirms */
        if (extf_kbd_text[0] && ext_filter_toggle_ext(extf_folder, extf_kbd_text)) {
            extf_dirty = true;
            ui_toast_show("Extension added");
        } else {
            ui_toast_show("Invalid or duplicate");
        }
        extf_kbd_active = false;
        return;
    }
    if (input_was_pressed(FROG_BTN_B))              /* cancel ??? back to picker */
        extf_kbd_active = false;
}

/* Rows the picker actually draws: one VISIBLE_ENTRIES slot is used by the
 * subtitle (see render_core_picker), so scroll math must match or the cursor
 * lands on an undrawn row and vanishes. */
#define PICKER_ROWS ((VISIBLE_ENTRIES - 1) < 1 ? 1 : (VISIBLE_ENTRIES - 1))
#define EXTF_POOL_N ((int)(sizeof(extf_pool) / sizeof(extf_pool[0])) - 1)

/* Flat model of a picker row, rebuilt on every open/section toggle. Both
 * section headers are selectable: LEFT/RIGHT (or A) on ">> Cores" / 
 * ">> EXTENSIONS" collapses/expands that section so long core lists can be
 * skipped with one press. */
enum {
    PR_CORE_HDR,   /* ">> Cores [-]" section header */
    PR_CORE,       /* "Default (auto)" / one core per row */
    PR_EXT_HDR,    /* ">> EXTENSIONS [-]" section header (filter folders only) */
    PR_EXT_TOGGLE, /* "Filter files: ON/OFF" */
    PR_EXT_BOX,    /* "[x] .cue" */
    PR_EXT_ADD     /* "Add extension..." (opens the on-screen keyboard) */
};
typedef struct { int type; int core; int ext; } PickerRow;
static PickerRow picker_rows_buf[256];
static int picker_rows_n = 0;

static void picker_rows_build(void) {
    int n = 0;
    picker_rows_buf[n].type = PR_CORE_HDR; picker_rows_buf[n].core = -1; picker_rows_buf[n].ext = -1; n++;
    if (extf_cores_open)
        for (int i = 0; i < core_choice_count; i++) {
            if (n >= (int)(sizeof(picker_rows_buf)/sizeof(picker_rows_buf[0]))) break;
            picker_rows_buf[n].type = PR_CORE; picker_rows_buf[n].core = i; picker_rows_buf[n].ext = -1; n++;
        }
    if (extf_in_picker) {
        if (n >= (int)(sizeof(picker_rows_buf)/sizeof(picker_rows_buf[0]))) { picker_rows_n = n; return; }
        picker_rows_buf[n].type = PR_EXT_HDR; picker_rows_buf[n].core = -1; picker_rows_buf[n].ext = -1; n++;
        if (extf_exts_open) {
            if (n >= (int)(sizeof(picker_rows_buf)/sizeof(picker_rows_buf[0]))) { picker_rows_n = n; return; }
            picker_rows_buf[n].type = PR_EXT_TOGGLE; picker_rows_buf[n].core = -1; picker_rows_buf[n].ext = -1; n++;
            for (int e = 0; e < EXTF_POOL_N; e++) {
                if (n >= (int)(sizeof(picker_rows_buf)/sizeof(picker_rows_buf[0]))) break;
                picker_rows_buf[n].type = PR_EXT_BOX; picker_rows_buf[n].core = -1; picker_rows_buf[n].ext = e; n++;
            }
            /* Custom extensions stored for this folder come after the pool. */
            int custom_n = ext_filter_ext_count(extf_folder);
            for (int e = 0; e < custom_n; e++) {
                if (n >= (int)(sizeof(picker_rows_buf)/sizeof(picker_rows_buf[0]))) break;
                picker_rows_buf[n].type = PR_EXT_BOX; picker_rows_buf[n].core = -1;
                picker_rows_buf[n].ext = EXTF_POOL_N + e;   /* stored-list index */
                n++;
            }
            if (n < (int)(sizeof(picker_rows_buf)/sizeof(picker_rows_buf[0]))) {
                picker_rows_buf[n].type = PR_EXT_ADD; picker_rows_buf[n].core = -1; picker_rows_buf[n].ext = -1; n++;
            }
        }
    }
    picker_rows_n = n;
}

/* Move the cursor onto the (first) row of the given type after a rebuild. */
static void picker_cursor_to(int type) {
    for (int i = 0; i < picker_rows_n; i++)
        if (picker_rows_buf[i].type == type) { core_picker_idx = i; break; }
}

static void handle_core_picker(void) {
    picker_rows_build();
    int total = picker_rows_n;
    if (input_was_pressed(FROG_BTN_UP)) {
        core_picker_idx = (core_picker_idx - 1 + total) % total;
    }
    if (input_was_pressed(FROG_BTN_DOWN)) {
        core_picker_idx = (core_picker_idx + 1) % total;
    }
    if (input_was_pressed(FROG_BTN_LEFT)) {
        int t = picker_rows_buf[core_picker_idx].type;
        if (t == PR_CORE_HDR || t == PR_EXT_HDR) {
            if (t == PR_CORE_HDR) extf_cores_open = !extf_cores_open;
            else                 extf_exts_open  = !extf_exts_open;
            picker_rows_build();
            total = picker_rows_n;
            picker_cursor_to(t);   /* stay on the header we just flipped */
        } else {
            core_picker_idx -= PICKER_ROWS;
            if (core_picker_idx < 0) core_picker_idx = 0;
        }
    }
    if (input_was_pressed(FROG_BTN_RIGHT)) {
        int t = picker_rows_buf[core_picker_idx].type;
        if (t == PR_CORE_HDR || t == PR_EXT_HDR) {
            if (t == PR_CORE_HDR) extf_cores_open = true;
            else                 extf_exts_open  = true;
            picker_rows_build();
            total = picker_rows_n;
        } else {
            core_picker_idx += PICKER_ROWS;
            if (core_picker_idx >= total) core_picker_idx = total - 1;
        }
    }
    if (core_picker_idx < core_picker_scroll)
        core_picker_scroll = core_picker_idx;
    if (core_picker_idx >= core_picker_scroll + PICKER_ROWS)
        core_picker_scroll = core_picker_idx - PICKER_ROWS + 1;
    if (input_was_pressed(FROG_BTN_A)) {
        int t = picker_rows_buf[core_picker_idx].type;
        if (t == PR_CORE_HDR || t == PR_EXT_HDR) {
            /* A on a header flips its section (same as LEFT). */
            if (t == PR_CORE_HDR) extf_cores_open = !extf_cores_open;
            else                 extf_exts_open  = !extf_exts_open;
            picker_rows_build();
            total = picker_rows_n;
            picker_cursor_to(t);
        } else if (t == PR_EXT_TOGGLE) {
            ext_filter_set_enabled(extf_folder, !ext_filter_get_enabled(extf_folder));
            extf_dirty = true;
        } else if (t == PR_EXT_BOX) {
            int ei = picker_rows_buf[core_picker_idx].ext;
            if (ei < EXTF_POOL_N)
                ext_filter_toggle_ext(extf_folder, extf_pool[ei]);
            else {
                /* stored (custom) extension: resolve by index */
                char ext[EXT_FILTER_EXT_LEN + 1];
                if (ext_filter_get_ext_at(extf_folder, ei - EXTF_POOL_N, ext, sizeof ext))
                    ext_filter_toggle_ext(extf_folder, ext);   /* toggling removes it */
            }
            extf_dirty = true;
        } else if (t == PR_EXT_ADD) {
            /* Open the on-screen keyboard in extension-entry mode. */
            extf_kbd_active = true;
            extf_kbd_text[0] = '\0';
            search_kbd_r = 0; search_kbd_c = 0;
        } else if (t == PR_CORE) {
            int cidx = picker_rows_buf[core_picker_idx].core;
            core_override_set(core_picker_key, core_choices[cidx].path);
            ui_toast_show(cidx == 0 ? "Core override cleared" : "Core saved for this item");
            core_picker_active = false;
        }
    }
    if (input_was_pressed(FROG_BTN_B)) {
        core_picker_active = false;
    }
    /* Leaving the picker (either button) after a filter edit: the dir mtime
     * didn't move, so drop the affected cached listings by hand and rescan
     * if we are sitting inside the filtered folder. The current view's cache
     * is dropped too: with hide-empty on, a root listing may need the folder
     * back (or gone) even when the folder's own mtime is unchanged. */
    if (!core_picker_active && extf_in_picker && extf_dirty) {
        char cf[MAX_PATH_LEN];
        cache_file_for(extf_path, cf, sizeof cf);
        unlink(cf);
        cache_file_for(current_path, cf, sizeof cf);
        unlink(cf);
        if (extf_inside)
            scan_directory(current_path);
        extf_dirty = false;
    }
}

/* Wizard covers every button except FN, which is only probed on devices that
 * have the physical button (device detection, see input_fn_available). FN is
 * the last enum entry, so on FN-less devices the wizard finishes right after
 * SELECT and never asks for a button that isn't there. */
static int remap_wizard_count(void) {
    return FROG_BTN_COUNT - (input_fn_available() ? 0 : 1);
}

static void handle_remap_wizard(void) {
    uint32_t raw   = input_get_raw_state();
    uint32_t risen = raw & ~remap_prev_raw;
    remap_prev_raw = raw;
    int b_bit = input_get_raw_bit(FROG_BTN_B);
    /* The wizard may be entered with an old or manually-edited keymap. A bad
     * B binding must not turn the shift below into undefined behaviour. */
    bool skip = b_bit >= 0 && b_bit <= FROG_RAW_MAX_BIT &&
                ((risen & (1u << b_bit)) != 0);
    int  pressed_bit = -1;
    if (!skip) {
        for (int bit = 0; bit < FROG_RAW_BIT_COUNT; bit++) {
            if ((risen >> bit) & 1) { pressed_bit = bit; break; }
        }
    }
    if (skip || pressed_bit >= 0) {
        if (!skip) input_set_raw_bit((FrogButton)remap_step, pressed_bit);
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "remap: step=%d button=%s raw=%d skip=%d",
                     remap_step, input_btn_name((FrogButton)remap_step),
                     pressed_bit, skip ? 1 : 0);
            dbg(msg);
        }
        remap_step++;
        if (remap_step >= remap_wizard_count()) {
            remap_wizard_active = false;
            if (input_save_remap(KEYMAP_FILE) == 0)
                ui_toast_show("Button mapping saved");
            else
                ui_toast_show("Could not save button mapping");
            /* Button Mapping belongs to Settings. Returning to its caller
             * avoids dropping the user into a partly stale Games/System view
             * after the final bind, which looked like empty icon tiles until
             * the next full UI reload. */
            settings_menu_active = true;
            settings_build_vis_rows();
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "remap: complete saved=%d return=settings style=%d icon_pack=%d",
                         access(KEYMAP_FILE, R_OK) == 0, settings_style,
                         settings_icon_pack_idx);
                dbg(msg);
            }
        }
    }
}

static void handle_settings_menu(void) {
    extern const int theme_count;
    /* Settings used to use edge-only presses while the browser used the
     * time-based repeat path. Keep both menus equally responsive, including
     * smooth scrolling/adjusting while a direction is held. */
    bool up    = input_repeat(FROG_BTN_UP);
    bool down  = input_repeat(FROG_BTN_DOWN);
    bool left  = input_repeat(FROG_BTN_LEFT);
    bool right = input_repeat(FROG_BTN_RIGHT);

    settings_build_vis_rows();
    /* Keep the cursor on a visible row (rows hidden by a collapsed section
     * can no longer be selected, so it can drift off the list). */
    int cur = settings_vis_cursor();
    if (settings_vis_rows[cur] != settings_menu_idx) {
        settings_menu_idx = settings_vis_rows[0];
        cur = 0;
    }
    if (up) {
        cur = (cur - 1 + settings_vis_n) % settings_vis_n;
        settings_menu_idx = settings_vis_rows[cur];
    }
    if (down) {
        cur = (cur + 1) % settings_vis_n;
        settings_menu_idx = settings_vis_rows[cur];
    }
    int sec = settings_section_of(settings_menu_idx);
    if ((left || right) && sec >= 0) {
        /* LEFT/RIGHT on a ">>" header collapses/expands its section, same
         * interaction as the SELECT picker sections. */
        settings_section_open[sec] = left ? !settings_section_open[sec] : true;
        settings_sections_save();   /* persist immediately (review request) */
        settings_build_vis_rows();
        /* cursor stays on the header we just flipped */
    } else if ((left || right) && settings_row_enabled(&settings_rows[settings_menu_idx])) {
        int delta = right ? 1 : -1;
        const SRow *r = &settings_rows[settings_menu_idx];
        switch (r->type) {
        case RT_THEME:
            settings_theme_idx = (settings_theme_idx + delta + theme_count) % theme_count;
            break;
        case RT_STYLE:
            settings_style = (settings_style + delta + STYLE_COUNT) % STYLE_COUNT;
            break;
        case RT_FONT:
            if (font_count > 0)
                settings_font_idx = (settings_font_idx + delta + font_count) % font_count;
            break;
        case RT_WALLPAPER:
            if (wallpaper_count > 0)
                settings_wallpaper_idx = (settings_wallpaper_idx + delta + wallpaper_count) % wallpaper_count;
            break;
        case RT_WALLFIT:
            settings_wallpaper_fit = (settings_wallpaper_fit + delta + WALL_FIT_N) % WALL_FIT_N;
            break;
        case RT_THEME_PACK:
            settings_theme_pack_idx = (settings_theme_pack_idx + delta + theme_pack_count) % theme_pack_count;
            break;
        case RT_ICON_PACK:
            settings_icon_pack_idx = (settings_icon_pack_idx + delta + icon_pack_count) % icon_pack_count;
            break;
        case RT_ROM_SOURCE:
            settings_rom_source = (settings_rom_source + delta + ROM_SOURCE_COUNT) % ROM_SOURCE_COUNT;
            break;
        case RT_LANGUAGE:
            settings_language = (settings_language + delta + LANGUAGE_COUNT) % LANGUAGE_COUNT;
            /* Locale changes need a fresh FrogUI core instance. Reinitializing
             * libretro from inside retro_run tears down picoarch itself, so
             * leave a one-shot boot flag and ask the normal launcher loop to
             * start us again. The new instance loads the saved locale before
             * scanning or rendering any system labels. */
            if (!i18n_init(language_codes[settings_language])) {
                settings_language = LANGUAGE_EN_US;
                i18n_init(language_codes[settings_language]);
            }
            settings_save_file();
            {
                FILE *flag = fopen(LANGUAGE_RESTART_FLAG, "w");
                if (flag) fclose(flag);
            }
            if (environ_cb)
                environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
            return;
        case RT_TOGGLE:
            *r->val = (*r->val + delta + 2) % 2;
            break;
        case RT_RANGE:
            *r->val += delta * r->rstep;
            if (*r->val < r->rmin) *r->val = r->rmin;
            if (*r->val > r->rmax) *r->val = r->rmax;
            break;
        default: break;
        }
        settings_preview_row(r);
    }
    if (input_was_pressed(FROG_BTN_A) && sec >= 0) {
        /* A on a header flips its section (same as LEFT) and stays in the
         * menu — it must not fall through to the save/close path below. */
        settings_section_open[sec] = !settings_section_open[sec];
        settings_sections_save();   /* persist immediately (review request) */
        settings_build_vis_rows();
        return;
    }
    if (input_was_pressed(FROG_BTN_A) &&
        settings_rows[settings_menu_idx].type == RT_CACHE_REBUILD) {
        /* Rebuilding opts in, removes every old/corrupt cache entry, and
         * immediately regenerates the root library index. System folders are
         * then refreshed lazily the first time they are opened. */
        settings_file_cache = 1;
        cache_clear();
        scan_directory(ROMS_PATH);
        settings_save_file();
        strncpy(current_path, ROMS_PATH, MAX_PATH_LEN-1);
        current_path[MAX_PATH_LEN-1] = '\0';
        selected_index = 0;
        scroll_offset = 0;
        settings_menu_active = false;
        save_games_tab_entries();
        ui_toast_show("File cache rebuilt");
        return;
    }
    if (input_was_pressed(FROG_BTN_A) && settings_rows[settings_menu_idx].type == RT_ACTION) {
        remap_step = 0;
        remap_prev_raw = input_get_raw_state();
        remap_wizard_active = true;
        settings_menu_active = false;
        dbg("remap: start from settings");
        return;
    }
    if ((input_was_pressed(FROG_BTN_A) && settings_rows[settings_menu_idx].type != RT_ACTION) ||
         input_was_pressed(FROG_BTN_B)) {
        settings_save_file();
        settings_volume_commit_final();   /* reset the live-commit debounce state */
        settings_menu_active = false;
        /* re-scan root so a hide-empty-folders change takes effect now */
        resolve_roms_root();
        scan_directory(ROMS_PATH);
        strncpy(current_path, ROMS_PATH, MAX_PATH_LEN-1);
        selected_index = 0; scroll_offset = 0;
        save_games_tab_entries();
    }
}

static bool horizontal_system_view(void) {
    return settings_style == STYLE_HORIZONTAL &&
           strcmp(current_path, ROMS_PATH) == 0 &&
           !viewing_recents && !viewing_favourites && !viewing_apps &&
           !apps_browsing && !viewing_search;
}

static bool icon_system_view(void) {
    return settings_style == STYLE_SYSTEM &&
           strcmp(current_path, ROMS_PATH) == 0 &&
           !viewing_recents && !viewing_favourites && !viewing_apps &&
           !apps_browsing && !viewing_search;
}

static void ui_toast_show(const char *text) {
    if (!text) return;
    strncpy(ui_toast_text, text, sizeof ui_toast_text - 1);
    ui_toast_text[sizeof ui_toast_text - 1] = '\0';
    ui_toast_frames = 90;   /* about 1.5 seconds at the presented 60 fps */
}

/* Measured on the R36SX amp: the speaker line needs several ms live before
 * the amp reproduces, and the SF-class frontend feeds ~100 ms of silence
 * runway before the first real chunk (see the picoarch speaker gate).  The
 * minimum AUDIBLE tick measured on-device was ~60 ms of content; the stock
 * firmware's own navigation blip is ~74 ms.  Anything shorter is swallowed
 * by the ramp-up; anything much longer stops reading as a discrete click.
 * Frames are derived from the core's ACTIVE sample rate so a different
 * audio backend/rate keeps the same 60 ms duration. */
#define UI_TICK_MS 60
/* Cap sized for the longest plausible UI tick (200 ms @ 48 kHz = 9600), not
 * the full second - static buffers are precious on these devices. */
#define UI_TICK_MAX_FRAMES 9600

static void ui_menu_tick(void) {
    if (!settings_menu_sounds) return;
    static int16_t tick[UI_TICK_MAX_FRAMES * 2];
    static int frames = 0;
    static int ready = 0;
    if (!ready || frames == 0) {
        frames = (int)((double)SAMPLE_RATE * UI_TICK_MS / 1000.0 + 0.5);
        if (frames < 16) frames = 16;             /* never degenerate */
        if (frames > UI_TICK_MAX_FRAMES) frames = UI_TICK_MAX_FRAMES;
        for (int i = 0; i < frames; i++) {
            int16_t s = ((i / 25) & 1) ? 900 : -900;
            int fade = (i < 200) ? i : (frames - i);
            if (fade > 2000) fade = 2000;
            if (fade < 1) fade = 1;
            s = (int16_t)((int)s * fade / 2000);
            tick[i * 2] = tick[i * 2 + 1] = s;
        }
        ready = 1;
    }
    if (audio_batch_cb) audio_batch_cb(tick, frames);
    else if (audio_cb)
        for (int i = 0; i < frames; i++) audio_cb(tick[i * 2], tick[i * 2 + 1]);
}

static void ui_transition_start(int direction) {
    (void)direction;
    if (!settings_anim || !framebuffer) return;
    size_t bytes = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t);
    if (!view_transition_old) view_transition_old = malloc(bytes);
    if (!view_transition_out) view_transition_out = malloc(bytes);
    if (!view_transition_old || !view_transition_out) return;
    memcpy(view_transition_old, framebuffer, bytes);
    view_transition_frame = 0;
}

static const uint16_t *ui_transition_compose(void) {
    if (!settings_anim || view_transition_frame > VIEW_TRANSITION_FRAMES ||
        !view_transition_old || !view_transition_out)
        return framebuffer;

    /* Blend at 25%, 50%, then 75% new view. Red/blue can be processed as one
     * packed field; green is separate. The denominator is four, so the hot
     * loop needs only multiplies, masks and a shift on the MIPS devices. */
    unsigned new_weight = (unsigned)view_transition_frame + 1u;
    unsigned old_weight = 4u - new_weight;
    size_t pixels = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT;
    for (size_t i = 0; i < pixels; i++) {
        uint32_t old = view_transition_old[i];
        uint32_t now = framebuffer[i];
        uint32_t rb = (((old & 0xf81fu) * old_weight +
                        (now & 0xf81fu) * new_weight) >> 2) & 0xf81fu;
        uint32_t g  = (((old & 0x07e0u) * old_weight +
                        (now & 0x07e0u) * new_weight) >> 2) & 0x07e0u;
        view_transition_out[i] = (uint16_t)(rb | g);
    }
    view_transition_frame++;
    return view_transition_out;
}

static bool system_carousel_is_animating(void) {
    return horizontal_system_view() && settings_anim &&
           system_carousel_frame <= SYSTEM_CAROUSEL_FRAMES;
}

static void system_carousel_start(int direction) {
    if (settings_anim) {
        /* selected_index has already moved. Carry over the current visual
         * offset so a second tap does not restart the labels from a whole slot
         * away and visibly jump. */
        system_carousel_start_offset =
            system_carousel_visible_offset + (float)direction;
        system_carousel_frame = 0;
    } else {
        system_carousel_start_offset = 0.0f;
        system_carousel_visible_offset = 0.0f;
        system_carousel_frame = SYSTEM_CAROUSEL_FRAMES + 1;
    }
}

static float system_carousel_ease(void) {
    if (!settings_anim || system_carousel_frame > SYSTEM_CAROUSEL_FRAMES)
        return 1.0f;
    float p = (float)system_carousel_frame / (float)SYSTEM_CAROUSEL_FRAMES;
    return p * p * (3.0f - 2.0f * p);
}

static int system_carousel_delta(int index) {
    int delta = index - selected_index;
    if (entry_count > 1) {
        while (delta > entry_count / 2) delta -= entry_count;
        while (delta < -(entry_count / 2)) delta += entry_count;
    }
    return delta;
}

static void fit_system_label(const char *source, char *dest, size_t size, int max_width) {
    size_t keep = strlen(source);
    if (size < 5) {
        if (size > 0) dest[0] = '\0';
        return;
    }
    if (keep > size - 4) keep = size - 4;
    snprintf(dest, size, "%s", source);
    while (keep > 1 && font_measure_text(dest) > max_width) {
        keep--;
        snprintf(dest, size, "%.*s...", (int)keep, source);
    }
}

static void render_system_carousel(uint16_t *fb) {
    render_tabs(fb, MAIN_TAB_GAMES, COLOR_BG);
    if (entry_count <= 0) {
        render_legend(fb, LEGEND_X_NONE, 0, 1);
        return;
    }

    int slot_width = SCREEN_WIDTH / 3;
    int center_x = SCREEN_WIDTH / 2;
    int text_y = SCREEN_HEIGHT * 3 / 5;
    float ease = system_carousel_ease();
    float offset = system_carousel_start_offset * (1.0f - ease);
    system_carousel_visible_offset = offset;

    for (int i = 0; i < entry_count; i++) {
        int delta = system_carousel_delta(i);
        float position = (float)delta + offset;
        if (position < -2.0f || position > 2.0f) continue;

        char label[96];
        fit_system_label(system_display_name(entries[i].name), label, sizeof label,
                         slot_width - UI_S(22));
        int text_width = font_measure_text(label);
        int x = center_x + (int)(position * slot_width) - text_width / 2;
        if (x + text_width < 0 || x >= SCREEN_WIDTH) continue;

        if (i == selected_index) {
            render_text_pillbox(fb, x, text_y, label,
                                COLOR_SELECT_BG, COLOR_SELECT_TEXT, UI_S(7));
        } else {
            render_text_pillbox(fb, x, text_y, label,
                                COLOR_LEGEND_BG, COLOR_DISABLED, UI_S(7));
        }
    }

    char count[32];
    snprintf(count, sizeof count, "%d / %d", selected_index + 1, entry_count);
    int count_x = (SCREEN_WIDTH - font_measure_text(count)) / 2;
    font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT, count_x,
                   text_y + ITEM_HEIGHT, count, COLOR_HEADER);

    int show_select =
        strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) != 0 &&
        strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) != 0 &&
        strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) != 0;
    render_legend(fb, LEGEND_X_NONE, show_select, 1);

    if (system_carousel_frame <= SYSTEM_CAROUSEL_FRAMES)
        system_carousel_frame++;
}

/* The system grid uses small cached icons independently from background packs.
 * Optional artwork comes from icon-packs/<pack>/<rom-folder>.png; the shared
 * frogui/system-icons folder is the fallback. A short LRU keeps SD reads out of
 * normal navigation without retaining every system logo on low-memory devices. */
#define SYSTEM_ICON_CACHE_N 12
#define SYSTEM_ICON_MAX_W   160
#define SYSTEM_ICON_MAX_H   160
typedef struct {
    char path[512];
    uint16_t *pixels;
    uint8_t *alpha;
    int width, height;
    uint8_t bright;
    unsigned used;
} SystemIcon;
static SystemIcon system_icons[SYSTEM_ICON_CACHE_N];
static unsigned system_icon_clock;
static int system_icon_pack = -1;
#define SYSTEM_ICON_MISSING_N 32
static char system_icon_missing[SYSTEM_ICON_MISSING_N][512];
static int system_icon_missing_count;

static SystemIcon *system_icon_cached(const char *path) {
    for (int i = 0; i < SYSTEM_ICON_CACHE_N; i++)
        if (system_icons[i].pixels && strcmp(system_icons[i].path, path) == 0)
            return &system_icons[i];
    return NULL;
}

static const char *system_icon_path(const char *folder, char *path, size_t size) {
    if (!folder || !*folder) return NULL;
    const char *key = folder;
    if (strcmp(folder, FAVOURITES_ENTRY_NAME) == 0) key = "favourites";
    else if (strcmp(folder, RECENTS_ENTRY_NAME) == 0) key = "recents";
    else if (strcmp(folder, SETTINGS_ENTRY_NAME) == 0) key = "settings";
    /* Master System is stored as SMS on some cards, while the icon packs use
     * the shared Game Gear/Master System artwork key (gg). */
    else if (strcasecmp(folder, "sms") == 0) key = "gg";
    if (settings_icon_pack_idx > 0 && settings_icon_pack_idx < icon_pack_count)
        snprintf(path, size, BANNER_DIR "/icon-packs/%s/%s.png",
                 icon_pack_files[settings_icon_pack_idx], key);
    else
        snprintf(path, size, BANNER_DIR "/system-icons/%s.png", key);
    if (system_icon_cached(path)) return path;
    for (int i = 0; i < system_icon_missing_count; i++)
        if (strcmp(system_icon_missing[i], path) == 0) return NULL;
    if (access(path, R_OK) == 0) return path;
    if (system_icon_missing_count < SYSTEM_ICON_MISSING_N) {
        strncpy(system_icon_missing[system_icon_missing_count], path, 511);
        system_icon_missing[system_icon_missing_count][511] = '\0';
        system_icon_missing_count++;
    }
    return NULL;
}

static SystemIcon *system_icon_get(const char *folder) {
    if (system_icon_pack != settings_icon_pack_idx) {
        for (int i = 0; i < SYSTEM_ICON_CACHE_N; i++) {
            free(system_icons[i].pixels);
            free(system_icons[i].alpha);
            memset(&system_icons[i], 0, sizeof system_icons[i]);
        }
        system_icon_missing_count = 0;
        system_icon_pack = settings_icon_pack_idx;
    }
    char path[512];
    if (!system_icon_path(folder, path, sizeof path)) return NULL;
    system_icon_clock++;
    SystemIcon *cached = system_icon_cached(path);
    if (cached) {
        cached->used = system_icon_clock;
        return cached;
    }

    int victim = 0;
    for (int i = 1; i < SYSTEM_ICON_CACHE_N; i++)
        if (!system_icons[i].pixels || system_icons[i].used < system_icons[victim].used)
            victim = i;

    int sw, sh, channels;
    unsigned char *rgba = stbi_load(path, &sw, &sh, &channels, 4);
    if (!rgba || sw <= 0 || sh <= 0) {
        if (rgba) stbi_image_free(rgba);
        return NULL;
    }
    int dw = sw, dh = sh;
    if (dw > SYSTEM_ICON_MAX_W) { dh = dh * SYSTEM_ICON_MAX_W / dw; dw = SYSTEM_ICON_MAX_W; }
    if (dh > SYSTEM_ICON_MAX_H) { dw = dw * SYSTEM_ICON_MAX_H / dh; dh = SYSTEM_ICON_MAX_H; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint16_t *pixels = malloc((size_t)dw * dh * sizeof(*pixels));
    uint8_t *alpha = malloc((size_t)dw * dh);
    if (!pixels || !alpha) {
        free(pixels); free(alpha); stbi_image_free(rgba); return NULL;
    }
    unsigned visible_pixels = 0;
    unsigned bright_pixels = 0;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            const unsigned char *p = rgba + ((size_t)sy * sw + sx) * 4;
            size_t dst = (size_t)y * dw + x;
            pixels[dst] = (uint16_t)(((p[0] >> 3) << 11) |
                                     ((p[1] >> 2) << 5) | (p[2] >> 3));
            alpha[dst] = p[3];
            /* Ignore transparent fringe pixels. A pack is considered bright
             * when most of its visible glyph is pale enough to disappear on
             * a light selection pill. */
            if (p[3] >= 64) {
                unsigned luma = (299u * p[0] + 587u * p[1] + 114u * p[2]) / 1000u;
                visible_pixels++;
                if (luma >= 184) bright_pixels++;
            }
        }
    }
    stbi_image_free(rgba);

    free(system_icons[victim].pixels);
    free(system_icons[victim].alpha);
    system_icons[victim].pixels = pixels;
    system_icons[victim].alpha = alpha;
    system_icons[victim].width = dw;
    system_icons[victim].height = dh;
    system_icons[victim].bright = visible_pixels > 0 &&
                                  bright_pixels * 100u >= visible_pixels * 60u;
    system_icons[victim].used = system_icon_clock;
    strncpy(system_icons[victim].path, path, sizeof system_icons[victim].path - 1);
    system_icons[victim].path[sizeof system_icons[victim].path - 1] = '\0';
    return &system_icons[victim];
}

static void system_icon_draw(uint16_t *fb, const SystemIcon *icon,
                             int x, int y, int w, int h) {
    if (!fb || !icon || !icon->pixels || !icon->alpha || w <= 0 || h <= 0) return;
    int dw = w, dh = icon->height * w / icon->width;
    if (dh > h) { dh = h; dw = icon->width * h / icon->height; }
    if (dw < 1 || dh < 1) return;
    int ox = x + (w - dw) / 2, oy = y + (h - dh) / 2;
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * icon->height / dh;
        uint16_t *dst = fb + (size_t)(oy + dy) * SCREEN_WIDTH + ox;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * icon->width / dw;
            size_t src = (size_t)sy * icon->width + sx;
            unsigned a = icon->alpha[src];
            if (!a) continue;
            uint16_t color = icon->pixels[src];
            if (a == 255) { dst[dx] = color; continue; }
            unsigned sr = (color >> 11) & 31, sg = (color >> 5) & 63, sb = color & 31;
            unsigned dr = (dst[dx] >> 11) & 31, dg = (dst[dx] >> 5) & 63, db = dst[dx] & 31;
            dst[dx] = (uint16_t)((((sr*a + dr*(255-a))/255) << 11) |
                                 (((sg*a + dg*(255-a))/255) << 5) |
                                  ((sb*a + db*(255-a))/255));
        }
    }
}

static void render_system_grid(uint16_t *fb) {
    render_tabs(fb, MAIN_TAB_GAMES, COLOR_BG);
    if (entry_count <= 0) {
        render_legend(fb, LEGEND_X_NONE, 0, 1);
        return;
    }

    enum { COLS = 4, ROWS = 2, PAGE_SIZE = COLS * ROWS };
    int page = selected_index / PAGE_SIZE;
    int pages = (entry_count + PAGE_SIZE - 1) / PAGE_SIZE;
    int first = page * PAGE_SIZE;
    int top = HEADER_HEIGHT + UI_S(8);
    int bottom = SCREEN_HEIGHT - ITEM_HEIGHT - UI_S(25);
    int grid_h = bottom - top;
    int gap = UI_S(8);
    int cell_w = (SCREEN_WIDTH - PADDING * 2 - gap * (COLS - 1)) / COLS;
    int cell_h = (grid_h - gap * (ROWS - 1)) / ROWS;

    for (int slot = 0; slot < PAGE_SIZE; slot++) {
        int idx = first + slot;
        if (idx >= entry_count) break;
        int col = slot % COLS, row = slot / COLS;
        int x = PADDING + col * (cell_w + gap);
        int y = top + row * (cell_h + gap);
        int active = idx == selected_index;

        SystemIcon *icon = system_icon_get(entries[idx].name);
        uint16_t tile_bg = COLOR_SELECT_BG;
        uint16_t tile_text = COLOR_SELECT_TEXT;
        if (active && icon && icon->bright) {
            unsigned bg_r = ((tile_bg >> 11) & 31u) * 255u / 31u;
            unsigned bg_g = ((tile_bg >> 5) & 63u) * 255u / 63u;
            unsigned bg_b = (tile_bg & 31u) * 255u / 31u;
            unsigned bg_luma = (299u * bg_r + 587u * bg_g + 114u * bg_b) / 1000u;
            if (bg_luma >= 160u) {
                /* Local contrast fix: invert only this selected system tile.
                 * Menus, legends and every other themed pill stay unchanged. */
                tile_bg = COLOR_SELECT_TEXT;
                tile_text = COLOR_SELECT_BG;
            }
        }
        if (active)
            render_rounded_rect(fb, x, y, cell_w, cell_h, UI_S(10), tile_bg);

        const char *shown = system_display_name(entries[idx].name);
        char label[96];
        fit_system_label(shown, label, sizeof label, cell_w - UI_S(12));
        int label_w = font_measure_text(label);
        int label_y = y + cell_h - ITEM_HEIGHT + UI_S(5);
        if (icon) {
            system_icon_draw(fb, icon, x + UI_S(12), y + UI_S(10),
                             cell_w - UI_S(24), cell_h - ITEM_HEIGHT - UI_S(8));
        } else {
            /* Custom folders remain useful even without an icon pack entry. */
            char initials[4] = {0};
            initials[0] = shown[0] ? shown[0] : '?';
            initials[1] = shown[1] ? shown[1] : '\0';
            for (int i = 0; initials[i]; i++)
                if (initials[i] >= 'a' && initials[i] <= 'z') initials[i] -= 32;
            int iw = font_measure_text(initials);
            font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT,
                           x + (cell_w - iw) / 2, y + cell_h / 2 - UI_S(8),
                           initials, active ? tile_text : COLOR_TEXT);
        }
        font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT,
                       x + (cell_w - label_w) / 2, label_y, label,
                       active ? tile_text : COLOR_TEXT);
    }

    char count[32];
    snprintf(count, sizeof count, "%d / %d", page + 1, pages);
    font_draw_text(fb, SCREEN_WIDTH, SCREEN_HEIGHT,
                   (SCREEN_WIDTH - font_measure_text(count)) / 2,
                   bottom + UI_S(4), count, COLOR_HEADER);
    int show_select =
        strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) != 0 &&
        strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) != 0 &&
        strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) != 0;
    render_legend(fb, LEGEND_X_NONE, show_select, 1);
}

static void handle_input(void) {
    input_update();

    /* A modal action must not fall through to the browser while its physical
     * button is still held.  In particular, B previously cancelled the modal
     * and then immediately performed a second, unrelated browser action. */
    if (usb_mode_wait_for_release) {
        if (input_get_raw_state() == 0)
            usb_mode_wait_for_release = false;
        else
            return;
    }

    /* USB mode gets its own confirmation page.  Keeping this modal prevents
     * the second A press from falling through into the browser (which used to
     * look like a UI restart) and gives B a reliable cancel path. */
    if (usb_mode_confirm_active || usb_mode_initiated_active) {
        uint32_t raw = input_get_raw_state();
        bool b_edge = usb_mode_raw_edge(FROG_BTN_B, raw);
        bool a_edge = usb_mode_raw_edge(FROG_BTN_A, raw);
        usb_mode_prev_raw = raw;

        /* B is identical in both phases.  The modal consumes every other
         * button, so browser navigation cannot accidentally close it. */
        if (b_edge) {
            usb_mode_confirm_active = false;
            usb_mode_initiated_active = false;
            usb_mode_wait_for_release = true;
            ui_toast_frames = 0;
            return;
        }
        if (usb_mode_initiated_active) {
            if (monotonic_ms() - usb_mode_initiated_at_ms >= 750) {
                usb_mode_initiated_active = false;
                request_builtin_launch(USB_MODE_BIN);
            }
            return;
        }
        if (a_edge) {
            usb_mode_confirm_active = false;
            usb_mode_initiated_active = true;
            usb_mode_initiated_at_ms = monotonic_ms();
        }
        return;
    }

    /* Search keyboard overlay */
    if (search_kbd_active) {
        handle_search_kbd();
        return;
    }

    /* Extension-entry keyboard overlay (SELECT picker ??? Add extension...) */
    if (extf_kbd_active) {
        handle_extf_kbd();
        return;
    }

    /* Core picker overlay: choose an override core for the current ROM/folder */
    if (core_picker_active) {
        handle_core_picker();
        return;
    }

    /* Remap wizard: detect raw rising edge on any bit; B = skip this step */
    if (remap_wizard_active) {
        handle_remap_wizard();
        return;
    }

    /* Top-level tabs use shoulders only. D-pad Left/Right belongs to the
     * current view (including Vertical) and must never change tabs. */
    {
        int tab = main_tab_active();
        bool at_tabs = settings_menu_active || viewing_recents || viewing_apps || apps_browsing ||
                       (!viewing_favourites && !viewing_search &&
                        strcmp(current_path, ROMS_PATH) == 0);
        bool prev = input_was_pressed(FROG_BTN_L1);
        bool next = input_was_pressed(FROG_BTN_R1);
        if (at_tabs && prev && tab > MAIN_TAB_RECENTS) {
            switch_main_tab(tab - 1);
            return;
        }
        if (at_tabs && next && tab < MAIN_TAB_SETTINGS) {
            switch_main_tab(tab + 1);
            return;
        }
    }

    if (settings_menu_active) {
        handle_settings_menu();
        return;
    }

    /* X: open search. Scope = current folder (ROMS root → search everything). */
    if (input_was_pressed(FROG_BTN_X) && !viewing_recents && !viewing_favourites && !viewing_search) {
        search_query[0] = '\0';
        search_kbd_r = 0; search_kbd_c = 0;
        strncpy(search_scope, current_path, sizeof(search_scope)-1);
        search_scope[sizeof(search_scope)-1] = '\0';
        search_kbd_active = true;
        goto input_done;
    }

    /* SELECT: open core picker. On a ROM file → per-game override; on a system
     * folder → per-folder override. Not available in recents/favourites views. */
    if (input_was_pressed(FROG_BTN_SELECT) && !viewing_recents && !viewing_favourites && !viewing_search &&
        selected_index < entry_count &&
        strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) != 0 &&
        strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) != 0 &&
        strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) != 0) {
        const char *cur;
        if (entries[selected_index].is_dir) {
            snprintf(core_picker_key, sizeof(core_picker_key), "%s/%s",
                     current_path, entries[selected_index].name);
            snprintf(core_picker_title, sizeof(core_picker_title), "Folder: %s",
                     entries[selected_index].name);
            cur = core_override_lookup(NULL, core_picker_key);
        } else {
            snprintf(core_picker_key, sizeof(core_picker_key), "%s/%s",
                     current_path, entries[selected_index].name);
            snprintf(core_picker_title, sizeof(core_picker_title), "Game: %s",
                     entries[selected_index].name);
            cur = core_override_lookup(core_picker_key, NULL);
        }
        /* Cursor lands on the active core's row (or the first core row when
         * the section is collapsed). */
        core_picker_idx = core_choice_index_for_path(cur);
        core_picker_current = core_picker_idx;   /* the active core, marked "* " */
        /* Keep the SELECT menu compact on entry.  Cores are the common
         * action; extension filtering remains available under its collapsible
         * section when needed. */
        extf_cores_open = true;
        extf_exts_open = false;
        core_picker_idx += 1;                    /* +1: ">> Cores" header row */
        core_picker_scroll = 0;
        if (core_picker_idx >= PICKER_ROWS)
            core_picker_scroll = core_picker_idx - PICKER_ROWS + 1;
        /* Extension whitelist block (PS1 family) sits ABOVE the core list.
         * Show it when SELECTing a filtered system folder itself, or when the
         * picker is opened on a ROM file inside one. */
        extf_in_picker = false;
        extf_inside = false;
        if (entries[selected_index].is_dir) {
            /* Every browsable folder gets the same extension-filter editor;
             * an empty list simply starts disabled and does not hide files. */
            {
                extf_in_picker = true;
                strncpy(extf_folder, entries[selected_index].name, sizeof(extf_folder) - 1);
                extf_folder[sizeof(extf_folder) - 1] = '\0';
                snprintf(extf_path, sizeof(extf_path), "%s/%s",
                         current_path, entries[selected_index].name);
            }
        } else {
            const char *cf = get_filter_folder(current_path);
            if (cf) {
                extf_in_picker = true;
                extf_inside = true;
                strncpy(extf_folder, cf, sizeof(extf_folder) - 1);
                extf_folder[sizeof(extf_folder) - 1] = '\0';
                strncpy(extf_path, current_path, sizeof(extf_path) - 1);
                extf_path[sizeof(extf_path) - 1] = '\0';
            }
        }
        extf_dirty = false;
        core_picker_active = true;
        return;
    }

    /* Y: Onion-style detailed/fullscreen toggle in GameSwitcher. Elsewhere it
     * keeps the existing favourite action. */
    if (input_was_pressed(FROG_BTN_Y) && viewing_recents && settings_game_switcher) {
        game_switcher_fullscreen = !game_switcher_fullscreen;
        goto input_done;
    }
    if (input_was_pressed(FROG_BTN_Y) && selected_index < entry_count) {
        if (viewing_favourites) {
            /* Remove selected favourite */
            favorites_remove_by_index(selected_index);
            ui_toast_show("Removed from Favourites");
            /* Refresh favourites view */
            const FavoriteGame *fl = favorites_get_list();
            int fc = favorites_get_count();
            entry_count = 0;
            for (int i = 0; i < fc; i++) {
                strncpy(entries[entry_count].name, fl[i].game_name, 255);
                entries[entry_count].name[255] = '\0';
                entries[entry_count].is_dir = 0;
                entry_count++;
            }
            if (selected_index >= entry_count && selected_index > 0)
                selected_index = entry_count - 1;
            if (entry_count == 0) {
                /* No more favourites — go back to root */
                viewing_favourites = false;
                scan_directory(ROMS_PATH);
                strncpy(current_path, ROMS_PATH, MAX_PATH_LEN-1);
            }
        } else if (!viewing_recents && !entries[selected_index].is_dir &&
                   strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) != 0 &&
                   strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) != 0 &&
                   strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) != 0) {
            /* Toggle favourite for current ROM */
            const char *folder = get_console_folder(current_path);
            const char *core   = get_core_for_folder(folder);
            if (!core) core = get_core_for_extension(entries[selected_index].name);
            char rom_path[MAX_PATH_LEN];
            snprintf(rom_path, sizeof(rom_path), "%s/%s", current_path, entries[selected_index].name);
            char game_name[256];
            strncpy(game_name, entries[selected_index].name, sizeof(game_name)-1);
            game_name[sizeof(game_name)-1] = '\0';
            char *dot = strrchr(game_name, '.');
            if (dot) *dot = '\0';
            if (core) {
                int was_favourite = favorites_is_favorited(core, game_name);
                favorites_toggle(core, game_name, rom_path);
                ui_toast_show(was_favourite ? "Removed from Favourites" : "Added to Favourites");
            }
        }
    }

    if (input_was_pressed(FROG_BTN_A) && selected_index < entry_count) {
        if (viewing_search) {
            search_launch(selected_index);
        } else if (viewing_favourites) {
            /* Launch from favourites list */
            const FavoriteGame *fl = favorites_get_list();
            int fc = favorites_get_count();
            if (selected_index < fc)
                launch_by_path(fl[selected_index].full_path);
        } else if (viewing_activity) {
            if (selected_index < activity_count)
                launch_by_path(activity_paths[selected_index]);
        } else if (viewing_recents) {
            /* Launch from recent games list */
            const RecentGame *rg = recent_games_get_list();
            int rc = recent_games_get_count();
            if (selected_index < rc)
                launch_by_path(rg[selected_index].full_path);
        } else if (viewing_apps) {
            /* Apps entries are virtual aliases for the on-card media folders. */
            char app_path[MAX_PATH_LEN];
            int app_index = -1;
            if (selected_index < entry_count) {
                for (int ai = 0; ai < (int)(sizeof(app_defs) / sizeof(app_defs[0])); ai++)
                    if (strcmp(entries[selected_index].name, app_defs[ai].key) == 0) { app_index = ai; break; }
            }
            if (app_index >= 0 && !strcmp(app_defs[app_index].key, "activity")) {
                ui_transition_start(1);
                enter_activity_view();
            } else if (app_index >= 0 && !strcmp(app_defs[app_index].key, "shutdown")) {
                request_builtin_launch(SHUTDOWN_BIN);
            } else if (app_index >= 0 && !strcmp(app_defs[app_index].key, "usbmode")) {
                if (access(USB_MODE_BIN, X_OK) != 0)
                    ui_toast_show("USB mode is unavailable");
                else {
                    ui_toast_frames = 0;
                    usb_mode_confirm_active = true;
                    /* Do not treat the A which opened the app as an A which
                     * confirms the modal. */
                    usb_mode_prev_raw = input_get_raw_state();
                }
            } else if (app_index >= 0 && app_defs[app_index].bin && access(app_defs[app_index].bin, X_OK) == 0) {
                if (!strcmp(app_defs[app_index].bin, FROGSHELL_CORE))
                    request_game_launch(FROGSHELL_CORE, FROGSHELL_CORE);
                else
                    request_standalone_launch(app_defs[app_index].bin, ROMS_PATH);
            } else if (app_index >= 0 && app_folder_path(app_index, app_path, sizeof app_path)) {
                ui_transition_start(1);
                strncpy(current_path, app_path, MAX_PATH_LEN - 1);
                current_path[MAX_PATH_LEN - 1] = '\0';
                strncpy(apps_root_path, app_path, MAX_PATH_LEN - 1);
                apps_root_path[MAX_PATH_LEN - 1] = '\0';
                viewing_apps = false;
                scan_directory(current_path);
                apps_browsing = true;
            }
        } else if (strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) == 0) {
            /* Enter favourites view */
            ui_transition_start(1);
            const FavoriteGame *fl = favorites_get_list();
            int fc = favorites_get_count();
            while (fc > entry_capacity) {
                entry_capacity = entry_capacity ? entry_capacity*2 : INITIAL_ENTRIES_CAPACITY;
                entries = realloc(entries, entry_capacity * sizeof(DirEntry));
                if (!entries) goto input_done;
            }
            entry_count = 0;
            for (int i = 0; i < fc; i++) {
                strncpy(entries[entry_count].name, fl[i].game_name, 255);
                entries[entry_count].name[255] = '\0';
                entries[entry_count].is_dir = 0;
                entry_count++;
            }
            viewing_favourites = true;
            selected_index = 0; scroll_offset = 0;
        } else if (strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) == 0) {
            ui_transition_start(1);
            enter_recents_view();
        } else if (entries[selected_index].is_dir) {
            ui_transition_start(1);
            char new_path[MAX_PATH_LEN];
            snprintf(new_path, sizeof(new_path), "%s/%s", current_path, entries[selected_index].name);
            strncpy(current_path, new_path, MAX_PATH_LEN-1);
            current_path[MAX_PATH_LEN-1] = '\0';
            scan_directory(current_path);
        } else {
            if (strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) == 0) {
                settings_menu_active = true;
                settings_menu_idx = 0;
                settings_filter_idx_on_enter = settings_filter_idx;
            } else {
                char rom_path[MAX_PATH_LEN];
                snprintf(rom_path, sizeof(rom_path), "%s/%s", current_path, entries[selected_index].name);
                launch_by_path(rom_path);
            }
        }
    }

    if (input_was_pressed(FROG_BTN_B)) {
        if (viewing_search) {
            /* back to the keyboard to refine the query */
            viewing_search = false;
            search_kbd_active = true;
        } else if (viewing_activity) {
            ui_transition_start(-1);
            scan_apps_tab();
            return;
        } else if (viewing_recents) {
            switch_main_tab(MAIN_TAB_GAMES);
            return;
        } else if (apps_browsing && strcmp(current_path, apps_root_path) == 0) {
            /* Return from a media folder to the Apps tab, not the Games root. */
            ui_transition_start(-1);
            strncpy(current_path, ROMS_PATH, MAX_PATH_LEN - 1);
            current_path[MAX_PATH_LEN - 1] = '\0';
            scan_apps_tab();
            return;
        } else if (viewing_favourites) {
            ui_transition_start(-1);
            viewing_favourites = false;
            scan_directory(ROMS_PATH);
            strncpy(current_path, ROMS_PATH, MAX_PATH_LEN-1);
        } else if (strcmp(current_path, ROMS_PATH) != 0) {
            ui_transition_start(-1);
            char *slash = strrchr(current_path, '/');
            if (slash) {
                /* scan_directory resets selection for a fresh listing. Keep
                 * the folder name so returning from a system restores the row
                 * we entered instead of jumping to the first item. */
                char child[MAX_PATH_LEN];
                strncpy(child, slash + 1, sizeof(child) - 1);
                child[sizeof(child) - 1] = '\0';
                *slash = '\0';
                scan_directory(current_path);
                for (int i = 0; i < entry_count; i++) {
                    if (strcmp(entries[i].name, child) == 0) {
                        selected_index = i;
                        scroll_offset = (i >= VISIBLE_ENTRIES)
                                      ? i - VISIBLE_ENTRIES + 1 : 0;
                        break;
                    }
                }
            }
        }
    }

    if (horizontal_system_view()) {
        scroll_offset = 0;
        if (input_repeat(FROG_BTN_LEFT) && entry_count > 0) {
            selected_index = (selected_index + entry_count - 1) % entry_count;
            system_carousel_start(-1);
        }
        if (input_repeat(FROG_BTN_RIGHT) && entry_count > 0) {
            selected_index = (selected_index + 1) % entry_count;
            system_carousel_start(1);
        }
    } else if (icon_system_view()) {
        enum { GRID_COLS = 4 };
        scroll_offset = 0;
        if (input_repeat(FROG_BTN_LEFT) && entry_count > 0)
            selected_index = (selected_index + entry_count - 1) % entry_count;
        if (input_repeat(FROG_BTN_RIGHT) && entry_count > 0)
            selected_index = (selected_index + 1) % entry_count;
        if (input_repeat(FROG_BTN_UP) && entry_count > 0) {
            int next = selected_index - GRID_COLS;
            if (next < 0) {
                int col = selected_index % GRID_COLS;
                int last_row = (entry_count - 1) / GRID_COLS;
                next = last_row * GRID_COLS + col;
                if (next >= entry_count) next = entry_count - 1;
            }
            selected_index = next;
        }
        if (input_repeat(FROG_BTN_DOWN) && entry_count > 0) {
            int next = selected_index + GRID_COLS;
            if (next >= entry_count) next = selected_index % GRID_COLS;
            if (next >= entry_count) next = entry_count - 1;
            selected_index = next;
        }
    } else {
        if (input_repeat(FROG_BTN_UP) && selected_index > 0) {
            selected_index--;
            if (selected_index < scroll_offset) scroll_offset = selected_index;
        }
        if (input_repeat(FROG_BTN_DOWN) && selected_index < entry_count-1) {
            selected_index++;
            if (selected_index >= scroll_offset + VISIBLE_ENTRIES)
                scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
        }
        /* In the game switcher (one game on screen at a time), Left/Right step
         * one game like Up/Down — no page jumping. */
        bool switcher = viewing_recents && settings_game_switcher;
        if (input_repeat(FROG_BTN_LEFT)) {
            if (switcher) {
                if (selected_index > 0) selected_index--;
            } else {
                selected_index = (selected_index >= VISIBLE_ENTRIES)
                               ? selected_index - VISIBLE_ENTRIES : 0;
                if (selected_index < scroll_offset) scroll_offset = selected_index;
            }
        }
        if (input_repeat(FROG_BTN_RIGHT)) {
            if (switcher) {
                if (selected_index < entry_count-1) selected_index++;
            } else {
                selected_index = (selected_index + VISIBLE_ENTRIES < entry_count)
                               ? selected_index + VISIBLE_ENTRIES : entry_count - 1;
                if (selected_index >= scroll_offset + VISIBLE_ENTRIES)
                    scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
            }
        }
    }

input_done:;
}

/* ---- libretro API ---- */

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "TreeFrogUI";
    info->library_version  = "1.0-sf3000";
    info->valid_extensions = "";
    info->need_fullpath    = false;
    info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = SCREEN_WIDTH;
    info->geometry.base_height  = SCREEN_HEIGHT;
    info->geometry.max_width    = SCREEN_WIDTH;
    info->geometry.max_height   = SCREEN_HEIGHT;
    info->geometry.aspect_ratio = (float)SCREEN_WIDTH / SCREEN_HEIGHT;
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = (double)SAMPLE_RATE;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    bool no_game = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

void retro_set_video_refresh(retro_video_refresh_t cb)          { video_cb      = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)            { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb){ audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)                { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)              { input_state_cb = cb; }


void retro_init(void) {
    dbg("retro_init start");
    /* Runtime panel geometry from picoarch (device-detected). Must run before
     * font_init / any layout use. Falls back to render.c defaults if env unset. */
    {
        const char *ew = getenv("TF_PANEL_W");
        const char *eh = getenv("TF_PANEL_H");
        const char *es = getenv("TF_UI_SCALE");
        int w = ew ? atoi(ew) : 0;
        int h = eh ? atoi(eh) : 0;
        int s = es ? atoi(es) : 0;
        render_set_geometry(w, h, s);
        dbg("geometry set");
    }
    input_init();
    input_load_remap(KEYMAP_FILE);
    i18n_init("en_US");
    font_init();
    dbg("font_init done");
    font_scan();
    dbg("font_scan done");
    wallpaper_scan();
    dbg("wallpaper_scan done");
    theme_pack_scan();
    dbg("theme_pack_scan done");
    icon_pack_scan();
    dbg("icon_pack_scan done");
    theme_init();
    dbg("theme_init done");
    settings_load_file();
    i18n_init(language_codes[settings_language]);
    system_labels_refresh();
    settings_sections_load();   /* restore the user's collapsed sections */
    /* A language change requests a normal launcher restart. Consume the flag
     * once and reopen the same Settings row so the user can continue without
     * hunting through the menu. */
    if (access(LANGUAGE_RESTART_FLAG, F_OK) == 0) {
        unlink(LANGUAGE_RESTART_FLAG);
        settings_menu_active = true;
        settings_section_open[1] = true; /* General contains Language. */
        for (int i = 0; i < SETTINGS_ROW_N; i++)
            if (settings_rows[i].type == RT_LANGUAGE) {
                settings_menu_idx = i;
                break;
            }
        settings_build_vis_rows();
    }
    /* Sync cubevol's stored backlight before checking the daemon. If it had
     * genuinely died, the replacement reads the right value immediately. */
    cube_pmem_backlight_sync(settings_brightness);
    fb1_set_visible(1);   /* leave the live volume/input daemon untouched */
    settings_apply();
    settings_bl_reassert = 120; /* re-assert safety net past cubevol's delayed apply */
    settings_audio_mute_reassert = settings_menu_sounds ? 0 : 120;
    dbg("settings loaded");
    recent_games_init();
    dbg("recent_games_init done");
    favorites_init();
    dbg("favorites_init done");
    core_override_load();
    ext_filter_load();
    build_core_choices();
    dbg("core overrides loaded");

    framebuffer = calloc(SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(uint16_t));
    dbg("calloc done");
    render_init(framebuffer);
    dbg("render_init done");
    /* Resolve the roms root before the first scan (see g_roms_path). */
    resolve_roms_root();
    if (strcmp(g_roms_path, ROMS_PATH_DEFAULT) == 0) {
        DIR *d = opendir(g_roms_path);
        if (d) closedir(d);
        else {
            DIR *alt = opendir(SDCARD_BASE "/ROMS");
            if (alt) { closedir(alt); strcpy(g_roms_path, SDCARD_BASE "/ROMS"); }
            else mkdir(g_roms_path, 0777);
        }
    }
    strncpy(current_path, g_roms_path, MAX_PATH_LEN - 1);
    current_path[MAX_PATH_LEN - 1] = '\0';
    scan_directory(current_path);
    dbg("scan_directory done");
    /* Recents reuses entries[] for its own rows. Keep the freshly-built Games
     * root in RAM so switching tabs never re-walks the FAT filesystem. */
    save_games_tab_entries();
    if (settings_load_recents && recent_games_get_count() > 0)
        enter_recents_view();   /* "Start in Recents" setting */
    shutdown_requested = false;
    dbg("retro_init complete");
}

void retro_deinit(void) {
    for (int i = 0; i < SYSTEM_ICON_CACHE_N; i++) {
        free(system_icons[i].pixels);
        free(system_icons[i].alpha);
        system_icons[i].pixels = NULL;
        system_icons[i].alpha = NULL;
    }
    system_icon_pack = -1;
    system_icon_missing_count = 0;
    free(framebuffer); framebuffer = NULL;
    free(view_transition_old); view_transition_old = NULL;
    free(view_transition_out); view_transition_out = NULL;
    free(games_tab_entries); games_tab_entries = NULL;
    games_tab_entry_count = 0;
    free(entries);     entries = NULL;
    entry_count = entry_capacity = 0;
}

bool retro_load_game(const struct retro_game_info *info) {
    (void)info;
    return true;  /* supports no-game */
}

void retro_unload_game(void) {}

static void render_settings_menu(void) {
    extern const Theme themes[];
    if (banner_is_loaded())
        banner_render(framebuffer);
    else
        render_clear_screen(framebuffer);
    render_tabs(framebuffer, MAIN_TAB_SETTINGS, COLOR_BG);

    char line[128];
    /* Scroll window over the VISIBLE rows (collapsed sections shrink the
     * list) so the selected row always stays on screen. */
    settings_build_vis_rows();
    int cur = settings_vis_cursor();
    static int soff = 0;
    if (cur < soff) soff = cur;
    if (cur >= soff + VISIBLE_ENTRIES) soff = cur - VISIBLE_ENTRIES + 1;
    if (soff > settings_vis_n - VISIBLE_ENTRIES) soff = settings_vis_n - VISIBLE_ENTRIES;
    if (soff < 0) soff = 0;

    int vis = settings_vis_n - soff;
    if (vis > VISIBLE_ENTRIES) vis = VISIBLE_ENTRIES;
    for (int i = 0; i < vis; i++) {
        int idx = settings_vis_rows[soff + i];
        const SRow *r = &settings_rows[idx];
        int y = START_Y + i * ITEM_HEIGHT;
        if (r->type == RT_HEADER) {
            /* Category divider: TreeFrogUI ">> " marker, accent color, no
             * pillbox when idle ??? but selectable, so pillbox under cursor. */
            snprintf(line, sizeof line, ">> %s", tr(r->label));
            if (settings_menu_idx == idx)
                render_text_pillbox(framebuffer, PADDING, y, line, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
            else
                font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, line, COLOR_SELECT_BG);
            continue;
        }
        switch (r->type) {
        case RT_INFO:   snprintf(line, sizeof line, "%s: %s", tr(r->label), treefrogui_version()); break;
        case RT_THEME:  snprintf(line, sizeof line, "%s: < %s >", tr(r->label), themes[settings_theme_idx].name); break;
        case RT_STYLE:  snprintf(line, sizeof line, "%s: < %s >", tr(r->label), style_names[settings_style]); break;
        case RT_FONT:   snprintf(line, sizeof line, "%s: < %s >", tr(r->label), font_count > 0 ? font_disp[settings_font_idx] : "(none)"); break;
        case RT_WALLPAPER: snprintf(line, sizeof line, "%s: < %s >", tr(r->label), wallpaper_disp[settings_wallpaper_idx]); break;
        case RT_WALLFIT:   snprintf(line, sizeof line, "%s: < %s >", tr(r->label), wallpaper_fit_names[settings_wallpaper_fit]); break;
        case RT_THEME_PACK: snprintf(line, sizeof line, "%s: < %s >", tr(r->label), theme_pack_disp[settings_theme_pack_idx]); break;
        case RT_ICON_PACK: snprintf(line, sizeof line, "%s: < %s >", tr(r->label), icon_pack_disp[settings_icon_pack_idx]); break;
        case RT_LANGUAGE: snprintf(line, sizeof line, "%s: < %s >", tr(r->label),
                                   tr(language_name_keys[settings_language])); break;
        case RT_ROM_SOURCE:
            if (otg_roms_available())
                snprintf(line, sizeof line, "%s: < %s >", tr(r->label), rom_source_names[settings_rom_source]);
            else
                snprintf(line, sizeof line, "%s: %s", tr(r->label), rom_source_names[ROM_SOURCE_SD]);
            break;
        case RT_OTG_STATUS:
            snprintf(line, sizeof line, "%s: %s", tr(r->label),
                     tr(otg_roms_available() ? "value.connected" : "value.not_connected"));
            break;
        case RT_TOGGLE: snprintf(line, sizeof line, "%s: < %s >", tr(r->label), tr(*r->val ? "value.on" : "value.off")); break;
        case RT_RANGE:  snprintf(line, sizeof line, "%s: < %d%% >", tr(r->label), *r->val); break;
        default:        snprintf(line, sizeof line, "%s", tr(r->label)); break;   /* RT_ACTION */
        }
        /* Options sit indented under their ">> HEADER" so the grouping reads
         * clearly. Headers stay flush at PADDING. */
        int ix = PADDING + UI_S(16);
        if (settings_menu_idx == idx && settings_row_enabled(r))
            render_text_pillbox(framebuffer, ix, y, line, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
        else {
            int color = (!settings_row_enabled(r) || r->type == RT_OTG_STATUS ||
                         (r->type == RT_ROM_SOURCE && !otg_roms_available()))
                      ? COLOR_DISABLED : COLOR_TEXT;
            font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, ix, y, line, color);
        }
    }
    render_scroll_indicator(framebuffer, settings_vis_n, cur, VISIBLE_ENTRIES);
    render_legend(framebuffer, LEGEND_X_NONE, 0, 0);
}

static void render_remap_wizard(void) {
    if (banner_is_loaded())
        banner_render(framebuffer);
    else
        render_clear_screen(framebuffer);
    render_header(framebuffer, tr("header.button_mapping"));

    char line[128];
    int y = START_Y;
    snprintf(line, sizeof(line), "Press  %s  (%d / %d)", input_btn_name((FrogButton)remap_step), remap_step + 1, remap_wizard_count());
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, line, COLOR_TEXT);

    y += ITEM_HEIGHT;
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, tr("remap.skip"), COLOR_TEXT);
}

static void render_core_picker(void) {
    if (banner_is_loaded())
        banner_render(framebuffer);
    else
        render_clear_screen(framebuffer);
    render_header(framebuffer, tr("header.select_core"));

    int y = START_Y;
    /* subtitle: which game/folder we're overriding */
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, core_picker_title, COLOR_TEXT);
    y += ITEM_HEIGHT;

    /* Draw the window of rows [scroll, scroll+PICKER_ROWS). Section headers
     * draw like Settings dividers but stay selectable: the pillbox cursor
     * lands on them, LEFT/RIGHT/A collapse or expand their section. */
    int total = picker_rows_n;
    for (int i = 0; i < PICKER_ROWS; i++) {
        int idx = core_picker_scroll + i;
        if (idx >= total) break;
        const PickerRow *r = &picker_rows_buf[idx];
        char line[96];
        switch (r->type) {
        case PR_CORE_HDR:
            snprintf(line, sizeof(line), ">> %s", tr("picker.cores"));
            break;
        case PR_EXT_HDR:
            snprintf(line, sizeof(line), ">> %s", tr("picker.extensions"));
            break;
        case PR_EXT_TOGGLE:
            snprintf(line, sizeof(line), "   %s: %s", tr("picker.filter_files"),
                     tr(ext_filter_get_enabled(extf_folder) ? "value.on" : "value.off"));
            break;
        case PR_EXT_BOX: {
            char ext[EXT_FILTER_EXT_LEN + 1];
            const char *extp;
            if (r->ext < EXTF_POOL_N)
                extp = extf_pool[r->ext];
            else if (ext_filter_get_ext_at(extf_folder, r->ext - EXTF_POOL_N,
                                          ext, sizeof ext))
                extp = ext;
            else
                extp = "?";
            snprintf(line, sizeof(line), "   [%c] .%s",
                     ext_filter_has_ext(extf_folder, extp) ? 'x' : ' ', extp);
            break;
        }
        case PR_EXT_ADD:
            snprintf(line, sizeof(line), "   %s", tr("picker.add_extension"));
            break;
        case PR_CORE:
            /* "->" marks the currently-active core; ">> " is reserved for
             * the section headers above. */
            snprintf(line, sizeof(line), "%s%s",
                     r->core == core_picker_current ? "-> " : "   ",
                     core_choices[r->core].name);
            break;
        default:
            line[0] = '\0';
            break;
        }
        int ry = y + i * ITEM_HEIGHT;
        if (r->type == PR_CORE_HDR || r->type == PR_EXT_HDR) {
            /* headers: accent color; pillbox when the cursor is on them */
            if (idx == core_picker_idx)
                render_text_pillbox(framebuffer, PADDING, ry, line, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
            else
                font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, ry,
                               line, COLOR_SELECT_BG);
        } else if (idx == core_picker_idx) {
            render_text_pillbox(framebuffer, PADDING, ry, line, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
        } else {
            font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, ry, line, COLOR_TEXT);
        }
    }
    render_scroll_indicator(framebuffer, total, core_picker_idx, PICKER_ROWS);
    render_legend(framebuffer, LEGEND_X_NONE, 0, 0);
}

static void render_search_kbd(void) {
    render_clear_screen(framebuffer);
    render_header(framebuffer, tr("header.search"));

    int y = START_Y;
    char q[96];
    snprintf(q, sizeof(q), "> %s_", search_query);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, q, COLOR_TEXT);
    y += ITEM_HEIGHT + UI_S(8);

    int cw = UI_S(26), ch = ITEM_HEIGHT;
    for (int r = 0; r < KBD_NROWS; r++) {
        int rl = kbd_row_len(r);
        int ry = y + r * ch;
        for (int c = 0; c < rl; c++) {
            char lbl[8];
            int cx;
            if (r == KBD_SPECIAL_ROW) { snprintf(lbl, sizeof(lbl), "%s", KBD_SPECIAL[c]); cx = PADDING + c * (cw * 3); }
            else { lbl[0] = KBD_ROWS[r][c]; lbl[1] = '\0'; cx = PADDING + c * cw; }
            if (r == search_kbd_r && c == search_kbd_c)
                render_text_pillbox(framebuffer, cx, ry, lbl, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
            else
                font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, cx, ry, lbl, COLOR_TEXT);
        }
    }
    render_legend(framebuffer, LEGEND_X_NONE, 0, 0);
}

/* Extension-entry keyboard: the search keyboard with an extension header and
 * the typed text shown as ".<text>" (the dot is implied and stripped). */
static void render_extf_kbd(void) {
    render_clear_screen(framebuffer);
    render_header(framebuffer, tr("header.add_extension"));

    int y = START_Y;
    char q[96];
    snprintf(q, sizeof(q), ".%s_", extf_kbd_text);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y, q, COLOR_TEXT);
    y += ITEM_HEIGHT + UI_S(8);

    int cw = UI_S(26), ch = ITEM_HEIGHT;
    for (int r = 0; r < KBD_NROWS; r++) {
        int rl = kbd_row_len(r);
        int ry = y + r * ch;
        for (int c = 0; c < rl; c++) {
            char lbl[8];
            int cx;
            if (r == KBD_SPECIAL_ROW) { snprintf(lbl, sizeof(lbl), "%s", KBD_SPECIAL[c]); cx = PADDING + c * (cw * 3); }
            else { lbl[0] = KBD_ROWS[r][c]; lbl[1] = '\0'; cx = PADDING + c * cw; }
            if (r == search_kbd_r && c == search_kbd_c)
                render_text_pillbox(framebuffer, cx, ry, lbl, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
            else
                font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, cx, ry, lbl, COLOR_TEXT);
        }
    }
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING,
                   y + KBD_NROWS * ch + UI_S(6),
                   tr("keyboard.ok_cancel"), COLOR_TEXT);
    render_legend(framebuffer, LEGEND_X_NONE, 0, 0);
}

static void render_usb_confirm(void) {
    render_clear_screen(framebuffer);
    render_header(framebuffer, tr("header.usb_mode"));
    const char *a = usb_mode_initiated_active
                  ? tr("usb.initiated")
                  : tr("usb.connect_instruction");
    const char *b = usb_mode_initiated_active
                  ? tr("usb.back")
                  : tr("usb.connect_back");
    if (!usb_mode_initiated_active)
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       (SCREEN_WIDTH - font_measure_text(a)) / 2,
                       SCREEN_HEIGHT / 2 - UI_S(18), a, COLOR_TEXT);
    const char *pill = usb_mode_initiated_active ? tr("usb.initiated") : tr("usb.ready");
    render_text_pillbox(framebuffer, (SCREEN_WIDTH - font_measure_text(pill)) / 2,
                        SCREEN_HEIGHT / 2 - UI_S(48), pill,
                        /* Keep the selected-theme contrast pair inside the
                         * pillbox; COLOR_TEXT can equal the pill background
                         * on light themes. */
                        COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                   (SCREEN_WIDTH - font_measure_text(b)) / 2,
                   SCREEN_HEIGHT / 2 + UI_S(12), b, COLOR_TEXT);
    if (usb_mode_initiated_active) {
        const char *warn = tr("usb.transfer_warning");
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       (SCREEN_WIDTH - font_measure_text(warn)) / 2,
                       SCREEN_HEIGHT / 2 + UI_S(42), warn, COLOR_TEXT);
    }
}

void retro_run(void) {
    /* Take input from the libretro frontend (rkgame feeds cores this way; the
     * cubevol joy_key shm isn't updated when rkgame owns evdev). Build FrogUI's
     * raw bit layout from the joypad → input_set_ext_raw (OR'd with shm). */
    if (input_state_cb) {
        if (input_poll_cb) input_poll_cb();
        uint32_t raw = 0;
        #define RB(id, bit) if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, (id))) raw |= (1u << (bit))
        RB(RETRO_DEVICE_ID_JOYPAD_UP, 4);    RB(RETRO_DEVICE_ID_JOYPAD_DOWN, 6);
        RB(RETRO_DEVICE_ID_JOYPAD_LEFT, 7);  RB(RETRO_DEVICE_ID_JOYPAD_RIGHT, 5);
        RB(RETRO_DEVICE_ID_JOYPAD_A, 13);    RB(RETRO_DEVICE_ID_JOYPAD_B, 14);
        RB(RETRO_DEVICE_ID_JOYPAD_X, 12);    RB(RETRO_DEVICE_ID_JOYPAD_Y, 15);
        RB(RETRO_DEVICE_ID_JOYPAD_L, 10);    RB(RETRO_DEVICE_ID_JOYPAD_R, 11);
        RB(RETRO_DEVICE_ID_JOYPAD_START, 3); RB(RETRO_DEVICE_ID_JOYPAD_SELECT, 0);
        #undef RB
        input_set_ext_raw(raw);
    }
    int old_selected = selected_index;
    int old_settings_row = settings_menu_idx;
    bool old_settings_active = settings_menu_active;
    bool old_recents = viewing_recents, old_favourites = viewing_favourites;
    char old_path[MAX_PATH_LEN];
    strncpy(old_path, current_path, sizeof old_path - 1);
    old_path[sizeof old_path - 1] = '\0';
    handle_input();
    if (old_selected != selected_index || old_settings_row != settings_menu_idx ||
        old_settings_active != settings_menu_active || old_recents != viewing_recents ||
        old_favourites != viewing_favourites || strcmp(old_path, current_path) != 0)
        ui_menu_tick();

    /* Entering the systems screen can also happen after a folder, Settings or
     * Recents re-scan changed selected_index. Start that screen settled rather
     * than carrying a half-finished offset from its previous visit. */
    {
        static bool carousel_view_last = false;
        bool carousel_view_now = horizontal_system_view();
        if (carousel_view_now && !carousel_view_last) {
            system_carousel_frame = SYSTEM_CAROUSEL_FRAMES + 1;
            system_carousel_start_offset = 0.0f;
            system_carousel_visible_offset = 0.0f;
        }
        carousel_view_last = carousel_view_now;
    }

    /* Re-assert brightness every frame for a short window after boot so
     * cubevol's delayed startup apply of its own (default) stored value is
     * overwritten within one frame instead of flashing visibly. */
    if (settings_bl_reassert > 0) {
        cube_set_backlight(settings_brightness);
        settings_bl_reassert--;
    }
    /* Live-volume tail commit: the debounced slider path (500 ms) can leave
     * the newest level applied to the hardware mirror but not yet persisted
     * when the user stops sliding.  Commit it once the window has passed, so
     * the stored value (physical meter, next boot) always catches up without
     * waiting for a menu exit. */
    if (settings_volume != volume_committed_level &&
        monotonic_ms() - volume_last_commit_ms >= VOLUME_COMMIT_MS) {
        volume_committed_level = -1;   /* force the real write below */
        settings_volume_commit_live();
    }
    /* Reverse link: the physical volume buttons (cubevol) change the stored
     * pmem value out from under us at any moment.  Poll it while the
     * Settings menu is open so the slider DISPLAY follows the buttons in
     * real time too - one value everywhere, whichever side changed it.
     * (picoarch does the same for the in-game path; this covers the UI.)
     * OWN-LAG GUARD: while the debounced slider path is catching up, pmem
     * still holds OUR last committed level - reading it back would bounce
     * the slider to stale values mid-adjustment (seen on-device as
     * 50-45-45-45-40-45-40 when sliding fast).  A read that matches our own
     * last commit is our lag, not a physical press: only adopt values that
     * differ from BOTH the slider and our last commit. */
    if (settings_menu_active) {
        static long long volume_poll_ms = 0;
        long long now = monotonic_ms();
        if (now - volume_poll_ms >= 250) {
            volume_poll_ms = now;
            int shared = cube_pmem_volume_read();
            if (shared >= 0 && shared <= 100 &&
                shared != settings_volume && shared != volume_committed_level) {
                settings_volume = shared;
                volume_committed_level = shared;   /* it is already stored */
            }
        }
    }
    /* cubevol can restore its stored I2SO level shortly after boot.  Keep the
       idle launcher mute asserted through that window, without changing the
       stored level used for the next game launch. */
    if (settings_audio_mute_reassert > 0) {
        cube_set_i2so_output_muted(1);
        settings_audio_mute_reassert--;
    }

    /* Reload banner when view, path, or selection changes.
     * On the main SYSTEMS menu, preview the highlighted folder's banner. */
    static char banner_last_path[MAX_PATH_LEN] = "";
    static bool banner_last_recents = false;
    static bool banner_last_favourites = false;
    static int  banner_last_sel = -1;
    static int  banner_last_bg = -1;    /* reload when the backgrounds toggle flips */
    static int  banner_last_wp = -1;    /* reload when the wallpaper choice changes */
    static int  banner_last_wpfit = -1; /* reload when the wallpaper fit mode changes */
    static int  banner_last_pack = -1;  /* reload when the background pack changes */
    static int  banner_last_dim = -1;   /* reload when background dim changes */
    {
        const char *banner_path = current_path;
        char sel_path[MAX_PATH_LEN];
        if (settings_menu_active) {
            banner_path = "settings";
        } else if (!viewing_recents && !viewing_favourites &&
                   selected_index >= 0 && selected_index < entry_count) {
            if (strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) == 0) {
                banner_path = "settings";
            } else if (strcmp(current_path, ROMS_PATH) == 0 &&
                       strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) == 0) {
                banner_path = "recents";
            } else if (strcmp(current_path, ROMS_PATH) == 0 &&
                       strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) == 0) {
                banner_path = "favourites";
            } else if (entries[selected_index].is_dir && strcmp(current_path, ROMS_PATH) == 0) {
                snprintf(sel_path, sizeof(sel_path), "%s/%s",
                         current_path, entries[selected_index].name);
                banner_path = sel_path;
            }
        }
        /* Reload ONLY when the background actually changes.
         * - A single WALLPAPER is the same image in every view: decode it ONCE
         *   and never reload on folder/view/selection change (only when the
         *   wallpaper choice, fit, or backgrounds toggle changes). This was the
         *   scroll killer - the wallpaper was re-decoded + re-scaled on every
         *   folder change.
         * - Otherwise the per-system art depends on banner_path (which already
         *   encodes root folder-preview + special entries), never selected_index
         *   directly. */
        (void)banner_last_sel;
        int wp_active = settings_backgrounds && settings_wallpaper_idx > 0;
        int need_reload;
        if (wp_active) {
            need_reload = (settings_wallpaper_idx != banner_last_wp ||
                           settings_wallpaper_fit != banner_last_wpfit ||
                           settings_theme_pack_idx != banner_last_pack ||
                           settings_background_dim != banner_last_dim ||
                           settings_backgrounds  != banner_last_bg);
        } else {
            need_reload = (viewing_recents != banner_last_recents ||
                           viewing_favourites != banner_last_favourites ||
                           strcmp(banner_path, banner_last_path) != 0 ||
                           settings_backgrounds != banner_last_bg ||
                           settings_wallpaper_idx != banner_last_wp ||
                           settings_wallpaper_fit != banner_last_wpfit ||
                           settings_theme_pack_idx != banner_last_pack ||
                           settings_background_dim != banner_last_dim);
        }
        if (need_reload && !(viewing_recents && settings_game_switcher)) {
            /* GameSwitcher paints its own full panel. Leave the Games banner
             * and its cache keys untouched while Recents is open, so returning
             * to Games does not decode the same background from SD again. */
            load_banner_for_view(banner_path, viewing_recents, viewing_favourites);
            banner_last_recents = viewing_recents;
            banner_last_favourites = viewing_favourites;
            banner_last_sel = selected_index;
            banner_last_bg = settings_backgrounds;
            banner_last_wp = settings_wallpaper_idx;
            banner_last_wpfit = settings_wallpaper_fit;
            banner_last_pack = settings_theme_pack_idx;
            banner_last_dim = settings_background_dim;
            strncpy(banner_last_path, banner_path, MAX_PATH_LEN - 1);
            banner_last_path[MAX_PATH_LEN - 1] = '\0';
        }
    }

    /* Redraw-only-when-dirty for the browser and Settings. Most calls have no
     * visual change,
     * so neither recompose nor re-present the old frame: R36SX's presenter waits
     * for vsync (~16.7ms), and calling it while idle needlessly limits input
     * polling to 60Hz. A short sleep keeps the idle loop cheap while still
     * sampling input at roughly 1kHz. Search/picker/remap overlays remain
     * continuously drawn for now. */
    int can_skip_idle = !(search_kbd_active || core_picker_active ||
                          remap_wizard_active);
    int redraw = 1;
    if (can_skip_idle) {
        static unsigned last_sig = 0; static int first = 1;
        unsigned sig = 5381u;
        sig = sig*33u + (unsigned)ui_toast_frames;
        sig = sig*33u + (unsigned)view_transition_frame;
        /* Include modal state in the presentation signature.  Without this,
         * B correctly cancelled USB mode but the compositor skipped the next
         * frame, leaving the old "USB MTP READY" framebuffer on screen until
         * an unrelated navigation key happened to invalidate it. */
        sig = sig*33u + (unsigned)usb_mode_confirm_active;
        sig = sig*33u + (unsigned)usb_mode_initiated_active;
        sig = sig*33u + (unsigned)settings_menu_active;
        if (settings_menu_active) {
            sig = sig*33u + (unsigned)settings_menu_idx;
            /* Collapsed/expanded section state: pressing A on a header changes
             * NO row value and NO cursor index ??? without these bits in the
             * signature the screen would wait for the next cursor move. */
            for (int s = 0; s < (int)(sizeof(settings_section_open)/sizeof(settings_section_open[0])); s++)
                sig = sig*33u + (settings_section_open[s] ? 1u : 0u);
            sig = sig*33u + (unsigned)settings_theme_idx;
            sig = sig*33u + (unsigned)settings_style;
            sig = sig*33u + (unsigned)settings_icon_pack_idx;
            sig = sig*33u + (unsigned)settings_font_idx;
            sig = sig*33u + (unsigned)settings_wallpaper_idx;
            sig = sig*33u + (unsigned)settings_wallpaper_fit;
            for (int i = 0; i < SETTINGS_ROW_N; i++)
                if (settings_rows[i].val)
                    sig = sig*33u + (unsigned)*settings_rows[i].val;
        } else {
            sig = sig*33u + (unsigned)selected_index;
            sig = sig*33u + (unsigned)scroll_offset;
            sig = sig*33u + (unsigned)(viewing_recents*16 + viewing_favourites*8 + viewing_apps*4 + apps_browsing*2 + viewing_activity + viewing_search*32);
            sig = sig*33u + (unsigned)game_switcher_fullscreen;
            for (const char *p = current_path; *p; p++) sig = sig*33u + (unsigned char)*p;
        }
        redraw = first || sig != last_sig || banner_is_animating() ||
                 system_carousel_is_animating() ||
                 view_transition_frame <= VIEW_TRANSITION_FRAMES ||
                 ui_toast_frames > 0 || usb_mode_confirm_active || usb_mode_initiated_active;
        last_sig = sig; first = 0;
    }
    if (!redraw) {
        /* Catch cubevol's asynchronous charging repaint within roughly one
         * display frame. The mmap and geometry stay cached. */
        static unsigned fb1_idle_ticks = 0;
        if (++fb1_idle_ticks >= 16) {
            fb1_idle_ticks = 0;
            fb1_clear_battery_zone();
        }
        usleep(1000);
        return;
    }
    /* Clear immediately on the first carousel frame and every real redraw.
     * Only the top-right battery zone is touched; cubevol's centered volume
     * popup remains available. */
    fb1_clear_battery_zone();

    if (usb_mode_confirm_active || usb_mode_initiated_active) {
        render_usb_confirm();
    } else if (search_kbd_active) {
        render_search_kbd();
    } else if (extf_kbd_active) {
        render_extf_kbd();
    } else if (core_picker_active) {
        render_core_picker();
    } else if (remap_wizard_active) {
        render_remap_wizard();
    } else if (settings_menu_active) {
        render_settings_menu();
    } else if (viewing_recents && settings_game_switcher) {
        /* Do not paint or discard the cached Games banner behind GameSwitcher. */
        render_clear_screen(framebuffer);
        render_game_switcher(framebuffer);
    } else {
        if (banner_is_loaded())
            banner_render(framebuffer);
        else
            render_clear_screen(framebuffer);
        if (horizontal_system_view()) {
            render_system_carousel(framebuffer);
        } else if (icon_system_view()) {
            render_system_grid(framebuffer);
        } else {
        static char search_title[96];
        const char *title;
        if (viewing_search) {
            snprintf(search_title, sizeof(search_title), "SEARCH: %s (%d)", search_query, entry_count);
            title = search_title;
        } else {
            title = viewing_activity   ? app_label("activity") :
                    viewing_recents    ? "RECENT GAMES" :
                    viewing_apps       ? "APPS" :
                    viewing_favourites ? "FAVOURITES" :
                    (strcmp(current_path, ROMS_PATH) == 0)
                    ? "TREEFROGUI: SYSTEMS" : system_display_name(get_basename(current_path));
        }
        if (viewing_activity)
            render_tabs(framebuffer, MAIN_TAB_APPS, COLOR_BG);
        else if (viewing_recents)
            render_tabs(framebuffer, MAIN_TAB_RECENTS, COLOR_BG);
        else if (viewing_apps)
            render_tabs(framebuffer, MAIN_TAB_APPS, COLOR_BG);
        else if (strcmp(current_path, ROMS_PATH) == 0 && !viewing_favourites && !viewing_search)
            render_tabs(framebuffer, MAIN_TAB_GAMES, COLOR_BG);
        else
            render_header(framebuffer, title);
        {
            int visible = min(entry_count - scroll_offset, VISIBLE_ENTRIES);
            for (int i = 0; i < visible; i++) {
                int idx = scroll_offset + i;
                const char *shown = entries[idx].name;
                char disp[256];
                if (viewing_apps && entries[idx].is_dir)
                    shown = app_label(entries[idx].name);
                else if (entries[idx].is_dir && strcmp(current_path, ROMS_PATH) == 0)
                    shown = system_display_name(entries[idx].name);
                /* Hide file extension (.gb/.gba/...) when enabled — display only,
                 * the real name in entries[] is still used to launch. Files only. */
                if (settings_hide_extensions && !entries[idx].is_dir) {
                    strncpy(disp, entries[idx].name, sizeof disp - 1);
                    disp[sizeof disp - 1] = '\0';
                    char *dot = strrchr(disp, '.');
                    if (dot && dot != disp) { *dot = '\0'; shown = disp; }
                }
                if (settings_center_text && settings_style == STYLE_VERTICAL &&
                    strcmp(current_path, ROMS_PATH) == 0)
                    render_menu_item_centered(framebuffer, idx, shown, entries[idx].is_dir,
                                              (idx == selected_index), scroll_offset);
                else
                    render_menu_item(framebuffer, idx, shown, entries[idx].is_dir,
                                     (idx == selected_index), scroll_offset, 0);
            }
            render_scroll_indicator(framebuffer, entry_count, selected_index, VISIBLE_ENTRIES);
        }
        /* Box-art panel for the selected game (normal browsing only). */
        if (!viewing_recents && !viewing_favourites && !viewing_search &&
            selected_index >= 0 && selected_index < entry_count &&
            !entries[selected_index].is_dir &&
            strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) != 0 &&
            strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) != 0 &&
            strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) != 0) {
            char fp[1024];
            snprintf(fp, sizeof fp, "%s/%s", current_path, entries[selected_index].name);
            render_boxart_panel(framebuffer, fp, entries[selected_index].name);
        }

        /* Compute Y-button legend mode for current selection */
        int legend_mode = LEGEND_X_NONE;
        if (viewing_favourites) {
            legend_mode = LEGEND_X_REMOVE;
        } else if (!viewing_recents && selected_index < entry_count &&
                   !entries[selected_index].is_dir &&
                   strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) != 0 &&
                   strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) != 0 &&
                   strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) != 0) {
            const char *folder = get_console_folder(current_path);
            const char *core   = get_core_for_folder(folder);
            if (!core) core = get_core_for_extension(entries[selected_index].name);
            if (core) {
                char game_name[256];
                strncpy(game_name, entries[selected_index].name, sizeof(game_name)-1);
                game_name[sizeof(game_name)-1] = '\0';
                char *dot = strrchr(game_name, '.');
                if (dot) *dot = '\0';
                legend_mode = favorites_is_favorited(core, game_name)
                              ? LEGEND_X_REMOVE : LEGEND_X_FAVOURITE;
            }
        }
        /* SELECT opens the core picker on folders + real ROMs (matches the SELECT
         * handler guard): show the "SEL-OPTIONS" hint there. */
        int show_select = (!viewing_recents && !viewing_favourites && !viewing_search &&
                           selected_index < entry_count &&
                           strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) != 0 &&
                           strcmp(entries[selected_index].name, RECENTS_ENTRY_NAME) != 0 &&
                           strcmp(entries[selected_index].name, FAVOURITES_ENTRY_NAME) != 0);
        /* X opens search in the normal browser (systems root + any game folder). */
        int show_search = (!viewing_recents && !viewing_favourites && !viewing_search);

        /* Play-time for the selected game (Recents list only). */
        if (viewing_recents && selected_index < entry_count && selected_index < recent_games_get_count()) {
            const RecentGame *rg = recent_games_get_list();
            long s = playtime_lookup(rg[selected_index].full_path);
            if (s > 0) {
                char t[64]; long h = s/3600, m = (s%3600)/60;
                if (h)          snprintf(t, sizeof t, "Played %ldh %ldm", h, m);
                else if (s >= 60) snprintf(t, sizeof t, "Played %ldm", m);
                else            snprintf(t, sizeof t, "Played %lds", s);
                font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING,
                               SCREEN_HEIGHT - 56, t, COLOR_TEXT);
            }
        }
        if (viewing_activity && selected_index < activity_count) {
            long s = activity_seconds[selected_index]; char t[128];
            long h = s / 3600, m = (s % 3600) / 60;
            if (h) snprintf(t, sizeof t, "%ld runs  %ldh %ldm", activity_runs[selected_index], h, m);
            else if (s >= 60) snprintf(t, sizeof t, "%ld runs  %ldm", activity_runs[selected_index], s/60);
            else snprintf(t, sizeof t, "%ld runs  %lds", activity_runs[selected_index], s);
            if (activity_last[selected_index] > 0) {
                char date[32]; struct tm tmv;
                localtime_r(&activity_last[selected_index], &tmv);
                strftime(date, sizeof date, "%Y-%m-%d", &tmv);
                size_t used = strlen(t); snprintf(t + used, sizeof t - used, "  last %s", date);
            } else {
                size_t used = strlen(t); snprintf(t + used, sizeof t - used, "  all time");
            }
            font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, SCREEN_HEIGHT - 56, t, COLOR_TEXT);
        }
        if (viewing_activity)
            render_activity_page(framebuffer);

        render_legend(framebuffer, legend_mode, show_select, show_search);
        }
    }

    if (ui_toast_frames > 0) {
        render_toast(framebuffer, ui_toast_text);
        ui_toast_frames--;
    }

    const uint16_t *present = ui_transition_compose();
    if (video_cb)
        video_cb(present, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
}

void retro_reset(void) { scan_directory(ROMS_PATH); strncpy(current_path, ROMS_PATH, MAX_PATH_LEN-1); }

unsigned retro_get_region(void)                                    { return RETRO_REGION_NTSC; }
size_t   retro_serialize_size(void)                                { return 0; }
bool     retro_serialize(void *d, size_t s)                        { (void)d;(void)s; return false; }
bool     retro_unserialize(const void *d, size_t s)                { (void)d;(void)s; return false; }
void     retro_cheat_reset(void)                                   {}
void     retro_cheat_set(unsigned i, bool e, const char *c)        { (void)i;(void)e;(void)c; }
void    *retro_get_memory_data(unsigned id)                        { (void)id; return NULL; }
size_t   retro_get_memory_size(unsigned id)                        { (void)id; return 0; }
bool     retro_load_game_special(unsigned t,
             const struct retro_game_info *i, size_t n)            { (void)t;(void)i;(void)n; return false; }
