/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2024 - RetroArch
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Streamlined Menu Driver
 *
 * A minimal menu driver for RetroArch.
 * Uses standard RetroArch menu navigation with custom in-game quick menu.
 *
 * Architecture Overview:
 * - Renders a simple list-based menu with rounded pill selection indicators, heavily inspired by MinUI
 * - Replaces RetroArch's default quick menu with a custom simplified version
 * - Supports a two-level menu: main quick menu and "Advanced" settings submenu
 *
 * Scaling:
 * - Uses RetroArch's DPI-aware scaling via gfx_display_get_dpi_scale()
 * - This provides intelligent scaling based on display size and viewing distance
 * - Respects user's menu_scale_factor preference from settings
 *
 * Navigation State Machine:
 * - is_quick_menu: true when viewing the custom quick menu (not RA settings)
 * - in_settings_submenu: true when in "Advanced" submenu
 * - return_to_settings_submenu: flag to return to submenu after backing out of RA menu
 * - Back button in main quick menu closes menu and resumes content
 * - Back button in Advanced submenu returns to main quick menu
 */

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#include <string/stdstring.h>
#include <lists/file_list.h>
#include <compat/strl.h>
#include <retro_inline.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../menu_driver.h"
#include "../menu_entries.h"
#include "../menu_setting.h"
#include "../menu_cbs.h"
#include "../../msg_hash.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/gfx_animation.h"
#include "../../gfx/gfx_thumbnail.h"
#include "../../gfx/font_driver.h"
#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../runloop.h"
#include "../../paths.h"
#include "../../disk_control_interface.h"
#include "../../tasks/task_content.h"
#include "../../core_info.h"
#include "../../file_path_special.h"
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <streams/file_stream.h>
#include <formats/m3u_file.h>
#include <playlists/label_sanitization.h>
#include "../../content.h"
#include "../../verbosity.h"
#include "../../defaults.h"
#include "../../playlist.h"

#if TARGET_OS_TV
#include <CoreText/CoreText.h>
#include "../../ui/drivers/cocoa/apple_platform.h"
#endif

/* Forward declarations for helpers defined later in the file */
static const char *streamlined_strip_sort_prefix(const char *name);
static void streamlined_get_display_name(
      const char *path, char *out, size_t out_size,
      bool is_m3u_folder);
static int streamlined_entry_cmp(const void *a, const void *b);

/* ======================================================================
 * CONFIGURATION
 * ====================================================================== */

/*
 * Color array format for gfx_display_draw_quad():
 * RetroArch's quad rendering expects 16 floats representing RGBA values
 * for each of the 4 vertices of the quad (top-left, top-right, bottom-left,
 * bottom-right). For solid colors, all 4 vertices use the same RGBA values.
 *
 * Layout: [R0,G0,B0,A0, R1,G1,B1,A1, R2,G2,B2,A2, R3,G3,B3,A3]
 */
#define STREAMLINED_SOLID_COLOR(r, g, b, a) \
   { r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a }

static float streamlined_color_bg[16]        = STREAMLINED_SOLID_COLOR(0.0f, 0.0f, 0.0f, 0.85f);
static float streamlined_color_selection[16] = STREAMLINED_SOLID_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
static float streamlined_color_accent[16]    = STREAMLINED_SOLID_COLOR(0.18f, 0.55f, 0.53f, 1.0f);
static float streamlined_color_black[16]     = STREAMLINED_SOLID_COLOR(0.0f, 0.0f, 0.0f, 1.0f);
static uint32_t streamlined_color_text        = 0xFFFFFFFF;  /* White (RGBA packed) */
static uint32_t streamlined_color_text_dark   = 0x000000FF;  /* Black (RGBA packed) */
static uint32_t streamlined_color_text_muted  = 0xAAAAAAFF;  /* Light gray (RGBA packed) */
static uint32_t streamlined_color_text_accent = 0x2E8C87FF;  /* Teal (RGBA packed) */

/* Layout constants - base sizes at 1.0x scale factor */
#define STREAMLINED_BASE_FONT_SIZE     32    /* Base font size in pixels */
#define STREAMLINED_MARGIN_RATIO       0.03f /* Screen edge margin as ratio of dimension */
#define STREAMLINED_LINE_HEIGHT        1.8f  /* Line height multiplier for menu items */
#define STREAMLINED_PILL_PADDING_RATIO 0.375f /* Horizontal padding as ratio of font size */
#define STREAMLINED_MIN_FONT_SIZE      12    /* Minimum font size to ensure readability */

/* ======================================================================
 * CUSTOM QUICK MENU - Modify this to change quick menu items
 * ====================================================================== */

typedef struct
{
   const char *label;
   enum msg_hash_enums action;
} streamlined_quick_item_t;

/*
 * Sentinel value used to identify the "Advanced" menu entry.
 * When a menu item has this value as its action, selecting it opens
 * the custom settings submenu instead of triggering a RetroArch action.
 * The value 0xCAFE is arbitrary, chosen to not conflict with any
 * MENU_ENUM_LABEL_* values.
 */
#define STREAMLINED_SETTINGS_SUBMENU_MARKER 0xCAFE

/* Marker for conditional exit entry - shows "Exit" or "Quit" based on CLI launch */
#define STREAMLINED_EXIT_MARKER 0xCAFF

/* Marker constants for Game List Options menu entries */
#define STREAMLINED_OPTIONS_RESET_GAME     0xCB00
#define STREAMLINED_OPTIONS_SEARCH         0xCB01
#define STREAMLINED_OPTIONS_RANDOM_GAME    0xCB02
#define STREAMLINED_OPTIONS_DELETE_SAVE    0xCB03
#define STREAMLINED_OPTIONS_SET_FOLDER_CORE 0xCB04
#define STREAMLINED_OPTIONS_SET_GAME_CORE   0xCB05
#define STREAMLINED_OPTIONS_CLEAR_GAME_CORE 0xCB06
#define STREAMLINED_OPTIONS_ADD_FAVORITE    0xCB07
#define STREAMLINED_OPTIONS_REMOVE_FAVORITE 0xCB08
#define STREAMLINED_FAVORITES_ENTRY         0xCB09
#define STREAMLINED_GAME_SWITCHER_ENTRY    0xCB0A
#define STREAMLINED_OPTIONS_REMOVE_FROM_SWITCHER 0xCB0B
#define STREAMLINED_OPTIONS_DELETE_GAME     0xCB0C

/* Main custom quick menu
 * NOTE: Exit/Quit handled dynamically - see streamlined_populate_quick_menu() */
static const streamlined_quick_item_t streamlined_quick_menu_items[] = {
   { "Resume",        MENU_ENUM_LABEL_RESUME_CONTENT },
   { "Save",          MENU_ENUM_LABEL_SAVE_STATE },
   { "Load",          MENU_ENUM_LABEL_LOAD_STATE },
   { "Advanced",      STREAMLINED_SETTINGS_SUBMENU_MARKER },  /* Opens combined settings submenu */
   { "Reset",         MENU_ENUM_LABEL_RESTART_CONTENT },
   { NULL,            STREAMLINED_EXIT_MARKER },  /* Dynamic: "Exit" or "Quit" based on CLI */
   { NULL, 0 }
};

/* Combined settings submenu items for quick menu (alphabetized)
 * NOTE: Disc Control is handled dynamically - see streamlined_populate_settings_submenu() */
static const streamlined_quick_item_t streamlined_settings_menu_items[] = {
   { "Achievements",     MENU_ENUM_LABEL_ACHIEVEMENT_LIST },
   { "Audio",            MENU_ENUM_LABEL_AUDIO_SETTINGS },
   { "Cheats",           MENU_ENUM_LABEL_CORE_CHEAT_OPTIONS },
   { "Controls",         MENU_ENUM_LABEL_CORE_INPUT_REMAPPING_OPTIONS },
   { "Core Options",     MENU_ENUM_LABEL_CORE_OPTIONS },
   { "Disc Control",     MENU_ENUM_LABEL_DISK_OPTIONS },  /* Conditionally shown */
   { "Information",      MENU_ENUM_LABEL_INFORMATION },
   { "Input",            MENU_ENUM_LABEL_INPUT_SETTINGS },
   { "Latency",          MENU_ENUM_LABEL_LATENCY_SETTINGS },
   { "Onscreen Overlay", MENU_ENUM_LABEL_ONSCREEN_OVERLAY_SETTINGS },
   { "Overrides",        MENU_ENUM_LABEL_QUICK_MENU_OVERRIDE_OPTIONS },
   { "Recording",        MENU_ENUM_LABEL_RECORDING_SETTINGS },
   { "Rewind",           MENU_ENUM_LABEL_REWIND_SETTINGS },
   { "Saving",           MENU_ENUM_LABEL_SAVING_SETTINGS },
   { "Screenshot",       MENU_ENUM_LABEL_TAKE_SCREENSHOT },
   { "Shaders",          MENU_ENUM_LABEL_SHADER_OPTIONS },
   { "Video",            MENU_ENUM_LABEL_VIDEO_SETTINGS },
   { NULL, 0 }
};

/* Main menu settings submenu items - all alphabetized */
static const streamlined_quick_item_t streamlined_main_settings_items[] = {
   { "Accessibility",    MENU_ENUM_LABEL_ACCESSIBILITY_SETTINGS },
   { "Achievements",     MENU_ENUM_LABEL_RETRO_ACHIEVEMENTS_SETTINGS },
   { "Audio",            MENU_ENUM_LABEL_AUDIO_SETTINGS },
   { "Cheats",           MENU_ENUM_LABEL_CORE_CHEAT_OPTIONS },
   { "Configuration",    MENU_ENUM_LABEL_CONFIGURATION_SETTINGS },
   { "Controls",         MENU_ENUM_LABEL_CORE_INPUT_REMAPPING_OPTIONS },
   { "Core",             MENU_ENUM_LABEL_CORE_SETTINGS },
   { "Core Options",     MENU_ENUM_LABEL_CORE_OPTIONS },
   { "Directory",        MENU_ENUM_LABEL_DIRECTORY_SETTINGS },
   { "Drivers",          MENU_ENUM_LABEL_DRIVER_SETTINGS },
   { "Frame Throttle",   MENU_ENUM_LABEL_FRAME_THROTTLE_SETTINGS },
   { "Information",      MENU_ENUM_LABEL_INFORMATION_LIST },
   { "Input",            MENU_ENUM_LABEL_INPUT_SETTINGS },
   { "Latency",          MENU_ENUM_LABEL_LATENCY_SETTINGS },
   { "Logging",          MENU_ENUM_LABEL_LOGGING_SETTINGS },
   { "Network",          MENU_ENUM_LABEL_NETWORK_SETTINGS },
   { "On-Screen Overlay", MENU_ENUM_LABEL_ONSCREEN_OVERLAY_SETTINGS },
   { "Online Updater",   MENU_ENUM_LABEL_ONLINE_UPDATER },
   { "Overrides",        MENU_ENUM_LABEL_QUICK_MENU_OVERRIDE_OPTIONS },
   { "Playlists",        MENU_ENUM_LABEL_PLAYLIST_SETTINGS },
   { "Power Management", MENU_ENUM_LABEL_POWER_MANAGEMENT_SETTINGS },
   { "Recording",        MENU_ENUM_LABEL_RECORDING_SETTINGS },
   { "Rewind",           MENU_ENUM_LABEL_REWIND_SETTINGS },
   { "Saving",           MENU_ENUM_LABEL_SAVING_SETTINGS },
   { "Shaders",          MENU_ENUM_LABEL_SHADER_OPTIONS },
   { "Start Recording",  MENU_ENUM_LABEL_QUICK_MENU_START_RECORDING },
   { "Start Streaming",  MENU_ENUM_LABEL_QUICK_MENU_START_STREAMING },
   { "Take Screenshot",  MENU_ENUM_LABEL_TAKE_SCREENSHOT },
   { "User",             MENU_ENUM_LABEL_USER_SETTINGS },
   { "User Interface",   MENU_ENUM_LABEL_USER_INTERFACE_SETTINGS },
   { "Video",            MENU_ENUM_LABEL_VIDEO_SETTINGS },
   { NULL, 0 }
};

/* ======================================================================
 * DRIVER STATE
 * ====================================================================== */

typedef struct
{
   /* Font */
   font_data_impl_t font;
   font_data_impl_t font_small;
   font_data_impl_t font_title;
   font_data_impl_t font_tiny;   /* For slot indicators */
   float font_size;
   float font_size_small;
   float font_size_title;
   float font_size_tiny;

   /* Layout */
   unsigned width;
   unsigned height;
   int margin_x;
   int margin_y;
   int pill_padding;
   float scale_factor;

   /* State */
   bool is_quick_menu;
   bool in_settings_submenu;
   bool return_to_settings_submenu;  /* Track if we should return to Advanced submenu */
   size_t saved_quick_menu_selection; /* Remember position in main quick menu */
   size_t saved_advanced_selection;   /* Remember position in Advanced submenu */

   /* Save slot selector */
   gfx_thumbnail_t savestate_thumbnail;
   char savestate_thumbnail_path[PATH_MAX_LENGTH];
   int preview_slot;              /* Currently previewed slot (0-7) */
   bool show_slot_selector;       /* True when on Save/Load State entry */
   size_t last_selection;         /* Track selection changes */

   /* Ticker for text scrolling (uses RetroArch's built-in animation system) */
   uint64_t ticker_idx;           /* Incremented each frame for ticker animation */
   uint64_t item_ticker_start;    /* ticker_idx when current item was selected */
   size_t item_ticker_selection;  /* Track which item is being ticker-scrolled */

   /* Custom main menu (launcher mode) */
   bool is_custom_main_menu;      /* True when displaying custom launcher menu */
   bool in_folder;                /* True when inside a folder (blocks going up) */
   char current_folder_path[PATH_MAX_LENGTH]; /* Path of current folder */
   char folder_core_path[PATH_MAX_LENGTH];    /* Core path for current folder (from core.txt) */
   size_t main_menu_selection;    /* Remember selection in main menu when entering folder */
   size_t folder_selection;       /* Remember selection in folder when launching game */
   char last_launched_folder[PATH_MAX_LENGTH]; /* Folder from which game was launched */
   char last_folder_core_path[PATH_MAX_LENGTH]; /* Core path for last launched folder */
   bool return_to_folder;         /* Flag to return to folder after game exit */
   bool return_to_top_level;      /* Flag to return to top level after game exit */
   size_t top_level_selection;    /* Remember selection at top level when launching game */

   /* ROM thumbnail display */
   gfx_thumbnail_t rom_thumbnail;
   char rom_thumbnail_path[PATH_MAX_LENGTH];
   size_t rom_thumbnail_selection;


   /* Core selection mode */
   bool selecting_core;           /* True when showing core selection list */
   char pending_content_path[PATH_MAX_LENGTH]; /* Content path waiting for core selection */

   /* Main menu settings submenu */
   bool in_main_settings_submenu; /* True when in Settings submenu from main menu */
   bool return_to_main_settings_submenu; /* Flag to return to settings submenu after backing out */
   size_t saved_main_menu_selection; /* Remember position in main menu when entering settings */
   size_t saved_settings_selection; /* Remember position in settings submenu */

   /* Save state detection for resume */
   bool selected_is_file;         /* True when selection is a ROM (not folder) */
   bool selected_has_savestate;   /* True when selected ROM has a save state */

   /* Options menu state */
   bool in_options_menu;
   char options_game_path[PATH_MAX_LENGTH];
   char options_folder_path[PATH_MAX_LENGTH];
   char options_core_path[PATH_MAX_LENGTH];
   bool options_game_has_savestate;
   size_t options_saved_selection;
   bool options_was_in_folder;
   bool selecting_core_for_folder;
   bool selecting_core_for_game;
   char options_game_core_path[PATH_MAX_LENGTH];
   bool in_favorites;
   size_t favorites_saved_selection;
   bool return_to_options;         /* Deferred transition from search back to options */
   bool enter_search_deferred;    /* Deferred transition from options into search */
   unsigned cancel_ignore_frames; /* Block cancel for N frames after state transition */

   /* Search mode state */
   bool in_search_mode;
   char search_query[256];
   size_t search_query_len;
   bool search_keyboard_done;
#if TARGET_OS_TV
   char *search_kb_buffer_ptr;
   size_t search_kb_buffer_size;
   size_t search_kb_buffer_offset;
#else
   int search_kb_row;
   int search_kb_col;
   bool search_focus_list;
#endif
   size_t search_list_selection;
   struct string_list *search_all_entries;
   char search_prev_query[256];

   /* Random game preview state */
   bool in_random_preview;
   char random_game_path[PATH_MAX_LENGTH];
   char random_display_name[256];
   bool random_has_savestate;
   bool random_has_thumbnail;
   bool random_show_text;          /* false = thumbnail view, true = text view */
   gfx_thumbnail_t random_thumbnail;
   char random_thumbnail_path[PATH_MAX_LENGTH];

   /* Game Switcher state */
   bool in_game_switcher;
   bool game_switcher_in_glo;
   size_t game_switcher_index;
   gfx_thumbnail_t game_switcher_thumbnail;
   char game_switcher_thumbnail_path[PATH_MAX_LENGTH];
   char game_switcher_display_name[256];
   char game_switcher_content_path[PATH_MAX_LENGTH];
   char game_switcher_core_path[PATH_MAX_LENGTH];
   bool game_switcher_has_savestate;
   bool game_switcher_has_thumbnail;
   size_t game_switcher_saved_selection;

   /* Delete autosave confirmation/result state */
   bool in_delete_confirm;
   bool delete_done;
   uint64_t delete_done_start;     /* ticker_idx when deletion completed */
   bool delete_is_game;           /* in_delete_confirm context: true = game, false = autosave */
   bool delete_was_game;          /* Preserved past confirm for result-screen navigation */
   char delete_done_label[64];    /* "Game Deleted" or "Deleted Autosave" */

   /* Loading screen state */
   bool loading_pending;          /* Waiting for loading screen to render */
   bool loading_triggered;        /* Loading screen rendered, ready to execute load */
   bool loading_is_resume;        /* Y-button resume: load auto-save state after launch */
   char loading_core_path[PATH_MAX_LENGTH];
   char loading_content_path[PATH_MAX_LENGTH];

   /* Exiting screen state */
   bool exiting_pending;          /* Waiting for exiting screen to render */
   bool exiting_triggered;        /* Exiting screen rendered, ready to execute quit */
   bool exiting_is_cli;           /* True = quit app (CLI), false = return to main menu */

} streamlined_t;

/* Number of save slots to display (Auto + slots 0-7) */
#define STREAMLINED_NUM_SLOTS 9
#define STREAMLINED_AUTO_SLOT_INDEX 0  /* First dot is the auto slot (state_slot -1) */

/* QWERTY keyboard layout for search (non-tvOS platforms) */
#if !TARGET_OS_TV
static const char *streamlined_kb_rows[] = {
   "1234567890",
   "QWERTYUIOP",
   "ASDFGHJKL",
   "ZXCVBNM\x08",    /* \x08 = backspace */
   " "
};
static const int streamlined_kb_row_lens[] = { 10, 10, 9, 8, 1 };
static const float streamlined_kb_row_offsets[] = { 0.0f, 0.0f, 0.5f, 1.0f, 0.0f };
#define STREAMLINED_KB_NUM_ROWS 5
#endif

/* ======================================================================
 * DRAWING FUNCTIONS
 * ====================================================================== */

/* Forward declarations */
static const char *streamlined_strip_sort_prefix(const char *name);
static void streamlined_build_savestate_base_path(
      const char *rom_path, const char *core_path,
      char *out, size_t out_size);
static bool streamlined_check_savestate(
      const char *rom_path, const char *core_path);
static void streamlined_build_autosave_path(
      const char *rom_path, const char *core_path,
      const char *suffix,
      char *out, size_t out_size);
static bool streamlined_find_autosave_path(
      const char *rom_path, const char *core_path,
      const char *suffix,
      char *out, size_t out_size);
static bool streamlined_resolve_m3u_content(const char *m3u_path,
      char *content_path_out, size_t content_path_size);
static bool streamlined_read_folder_core(
      const char *folder_path, char *core_path_out, size_t core_path_size);
static bool streamlined_check_m3u_folder(const char *dir_path,
      char *m3u_path_out, size_t m3u_path_size);
static bool streamlined_find_m3u_for_content(const char *content_path,
      char *m3u_out, size_t m3u_size);
static void streamlined_populate_search_results(streamlined_t *strm);
static void streamlined_populate_options_menu(streamlined_t *strm, bool in_favorites, bool in_game_switcher);
static void streamlined_populate_favorites_menu(streamlined_t *strm);
static void streamlined_strip_trailing_slash(char *path);
static void streamlined_get_content_folder_path(const char *game_path, char *folder_out, size_t folder_size);
static bool streamlined_resolve_content_core(const char *game_path, char *core_out, size_t core_size, char *game_core_out, size_t game_core_size, playlist_t *playlist);
static void streamlined_populate_core_selection(streamlined_t *strm, const char *content_path);
static const char *streamlined_get_core_display_name(const char *core_path);
static const char *streamlined_effective_core(streamlined_t *strm);
static void streamlined_get_game_core_name(
      const char *game_path, const char *folder_path,
      char *out, size_t out_size);
static bool streamlined_read_game_core(const char *folder_path,
      const char *game_name, char *core_path_out, size_t size);
static void streamlined_select_options_entry(unsigned target_enum);
static void streamlined_delete_game_files(const char *game_path);
static void streamlined_remove_from_all_playlists(const char *game_path);

enum streamlined_font_type { FONT_NORMAL, FONT_SMALL, FONT_TINY };

static void streamlined_draw_text(streamlined_t *strm,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color, bool small_font);
static int streamlined_get_text_width(streamlined_t *strm, const char *text,
      enum streamlined_font_type font_type);

static void streamlined_draw_bg(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   if (!p_disp || video_width == 0 || video_height == 0)
      return;

   gfx_display_draw_quad(p_disp, userdata,
         video_width, video_height,
         0, 0, video_width, video_height,
         video_width, video_height,
         streamlined_color_bg, NULL);
}

static void streamlined_draw_filled_circle(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      int cx, int cy, int radius,
      unsigned video_width, unsigned video_height,
      float *color)
{
   int y;
   float r_sq = (float)(radius * radius);

   /* Draw circle as horizontal spans - much faster than per-pixel */
   for (y = -radius; y <= radius; y++)
   {
      float y_sq = (float)(y * y);
      float x_span = sqrtf(r_sq - y_sq);
      int x_start = (int)(-x_span + 0.5f);
      int x_end = (int)(x_span + 0.5f);
      int span_width = x_end - x_start;

      if (span_width > 0)
      {
         gfx_display_draw_quad(p_disp, userdata, video_width, video_height,
               cx + x_start, cy + y, span_width, 1,
               video_width, video_height, color, NULL);
      }
   }
}

/*
 * Draw a rounded pill shape (stadium/discorectangle).
 * Composed of: left semicircle + center rectangle + right semicircle.
 * The radius is derived from height/2, creating perfect semicircles at ends.
 *
 *   ╭───────────────────╮
 *   │ O               O │  <- semicircles at each end
 *   ╰───────────────────╯
 */
static void streamlined_draw_rounded_pill(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      int x, int y, int width, int height,
      unsigned video_width, unsigned video_height,
      float *color)
{
   int radius = height / 2;
   int rect_x = x + radius;
   int rect_width = width - height;

   /* Left semicircle */
   streamlined_draw_filled_circle(strm, p_disp, userdata,
         x + radius, y + radius, radius,
         video_width, video_height, color);

   /* Right semicircle */
   streamlined_draw_filled_circle(strm, p_disp, userdata,
         x + width - radius, y + radius, radius,
         video_width, video_height, color);

   /* Center rectangle (only if pill is wide enough) */
   if (rect_width > 0)
   {
      gfx_display_draw_quad(p_disp, userdata, video_width, video_height,
            rect_x, y, rect_width, height,
            video_width, video_height, color, NULL);
   }
}

static void streamlined_draw_text(streamlined_t *strm,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color, bool small_font)
{
   font_data_t *font = small_font ? strm->font_small.font : strm->font.font;
   if (font && text)
   {
      gfx_display_draw_text(font, text, x, y,
            video_width, video_height, color,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
   }
}

static void streamlined_draw_title(streamlined_t *strm,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color)
{
   font_data_t *font = strm->font_title.font ? strm->font_title.font : strm->font.font;
   if (font && text)
   {
      gfx_display_draw_text(font, text, x, y,
            video_width, video_height, color,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
   }
}

static int streamlined_get_text_width(streamlined_t *strm, const char *text,
      enum streamlined_font_type font_type)
{
   font_data_t *font;
   switch (font_type)
   {
      case FONT_TINY:
         font = strm->font_tiny.font ? strm->font_tiny.font : strm->font_small.font;
         break;
      case FONT_SMALL:
         font = strm->font_small.font;
         break;
      default:
         font = strm->font.font;
         break;
   }
   if (font && text)
      return font_driver_get_message_width(font, text, strlen(text), 1.0f);
   return 0;
}

static void streamlined_draw_text_tiny(streamlined_t *strm,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color)
{
   font_data_t *font = strm->font_tiny.font ? strm->font_tiny.font : strm->font_small.font;
   if (font && text)
   {
      gfx_display_draw_text(font, text, x, y,
            video_width, video_height, color,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
   }
}

static int streamlined_get_text_width_tiny(streamlined_t *strm, const char *text)
{
   font_data_t *font = strm->font_tiny.font ? strm->font_tiny.font : strm->font_small.font;
   if (font && text)
      return font_driver_get_message_width(font, text, strlen(text), 1.0f);
   return 0;
}

static int streamlined_get_title_width(streamlined_t *strm, const char *text)
{
   font_data_t *font = strm->font_title.font ? strm->font_title.font : strm->font.font;
   if (font && text)
      return font_driver_get_message_width(font, text, strlen(text), 1.0f);
   return 0;
}

/*
 * Check if entry value indicates a directory and add slash prefix.
 * Returns true if entry is a directory.
 */
static bool streamlined_process_entry_type(const char *value, char *label, size_t label_size)
{
   char temp[256];

   if (string_is_equal(value, "(DIR)"))
   {
      /* Add leading slash to indicate directory */
      snprintf(temp, sizeof(temp), "/%s", label);
      strlcpy(label, temp, label_size);
      return true;
   }
   return false;
}

/*
 * Check if value should be hidden (file type indicators).
 * These are hardcoded English strings in RetroArch, not translated.
 */
static bool streamlined_should_hide_value(const char *value)
{
   return string_is_equal(value, "(FILE)")
       || string_is_equal(value, "(DIR)")
       || string_is_equal(value, "(IMAGE)")
       || string_is_equal(value, "(MOVIE)")
       || string_is_equal(value, "(MUSIC)")
       || string_is_equal(value, "(COMP)")
       || string_is_equal(value, "(CORE)")
       || string_is_equal(value, "(SHADER)")
       || string_is_equal(value, "(PRESET)")
       || string_is_equal(value, "(RDB)")
       || string_is_equal(value, "(CURSOR)")
       || string_is_equal(value, "(CFILE)");
}

/* Truncate text to fit within max_width, adding ellipsis if needed */
static void streamlined_truncate_text(streamlined_t *strm, const char *text,
      char *out, size_t out_size, int max_width,
      enum streamlined_font_type font_type)
{
   int text_width;
   size_t len;

   if (!text || !out || out_size == 0)
      return;

   strlcpy(out, text, out_size);
   text_width = streamlined_get_text_width(strm, out, font_type);

   if (text_width <= max_width)
      return;

   /* Truncate and add ellipsis */
   len = strlen(out);
   while (len > 3 && text_width > max_width)
   {
      out[len - 1] = '\0';
      len--;
      /* Make room for ellipsis */
      if (len > 3)
      {
         out[len - 1] = '.';
         out[len - 2] = '.';
         out[len - 3] = '.';
      }
      text_width = streamlined_get_text_width(strm, out, font_type);
   }
}

/* ======================================================================
 * SAVE SLOT SELECTOR
 * ====================================================================== */

/*
 * Load the thumbnail for a specific save slot.
 * Constructs the path and requests async thumbnail load.
 *
 * @param preview_slot: 0 = Auto (state_slot -1), 1-8 = state_slot 0-7
 */
static void streamlined_load_slot_thumbnail(streamlined_t *strm, int preview_slot)
{
   char state_path[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();
   int state_slot = preview_slot - 1;  /* Convert preview_slot to state_slot */

   /* Get savestate path for this slot */
   if (!runloop_get_savestate_path(state_path, sizeof(state_path), state_slot))
      return;

   /* Append .png extension for thumbnail */
   strlcat(state_path, ".png", sizeof(state_path));

   /* Request thumbnail if path changed or status is unknown */
   if (   (strm->savestate_thumbnail.status == GFX_THUMBNAIL_STATUS_UNKNOWN)
       || !string_is_equal(state_path, strm->savestate_thumbnail_path))
   {
      strlcpy(strm->savestate_thumbnail_path, state_path,
            sizeof(strm->savestate_thumbnail_path));

      /* Free old texture before requesting new one */
      gfx_thumbnail_reset(&strm->savestate_thumbnail);

      /* Request new thumbnail - the thumbnail system handles missing files */
      gfx_thumbnail_request_file(state_path, &strm->savestate_thumbnail,
            settings->uints.gfx_thumbnail_upscale_threshold);

      /* Use core aspect ratio for proper rendering */
      strm->savestate_thumbnail.flags |= GFX_THUMB_FLAG_CORE_ASPECT;
   }

   strm->preview_slot = preview_slot;
}

/*
 * Load a ROM thumbnail from the .media folder relative to the ROM's directory.
 * Path: {current_folder}/.media/{type}/{rom_basename_without_ext}.png
 * Type mapping: 1->Screenshot, 2->Title, 3->Boxart (0 = off)
 */
static void streamlined_load_rom_thumbnail(streamlined_t *strm, const char *rom_file_path)
{
   char thumb_path[PATH_MAX_LENGTH];
   char name_buf[PATH_MAX_LENGTH];
   const char *type_folder;
   const char *base_name;
   char *ext;
   settings_t *settings = config_get_ptr();

   if (!strm || string_is_empty(rom_file_path))
      return;

   /* Check thumbnail type setting */
   if (settings->uints.gfx_thumbnails == 0)
   {
      gfx_thumbnail_reset(&strm->rom_thumbnail);
      strm->rom_thumbnail_path[0] = '\0';
      return;
   }

   /* Map type: 1->Screenshot, 2->Title, 3->Boxart */
   switch (settings->uints.gfx_thumbnails)
   {
      case 1:  type_folder = "Screenshot"; break;
      case 2:  type_folder = "Title";      break;
      case 3:  type_folder = "Boxart";     break;
      default: return;
   }

   /* Get ROM base filename and strip extension */
   base_name = path_basename(rom_file_path);
   if (string_is_empty(base_name))
      return;

   strlcpy(name_buf, base_name, sizeof(name_buf));
   ext = strrchr(name_buf, '.');
   if (ext)
      *ext = '\0';

   /* Build path: {current_folder}/.media/{type}/{name}.png */
   fill_pathname_join_special(thumb_path, strm->current_folder_path,
         ".media", sizeof(thumb_path));
   fill_pathname_join(thumb_path, thumb_path,
         type_folder, sizeof(thumb_path));
   fill_pathname_join(thumb_path, thumb_path,
         name_buf, sizeof(thumb_path));
   strlcat(thumb_path, ".png", sizeof(thumb_path));

   /* Skip if same path already loaded */
   if (string_is_equal(thumb_path, strm->rom_thumbnail_path))
      return;

   strlcpy(strm->rom_thumbnail_path, thumb_path,
         sizeof(strm->rom_thumbnail_path));

   gfx_thumbnail_reset(&strm->rom_thumbnail);
   gfx_thumbnail_request_file(thumb_path, &strm->rom_thumbnail,
         settings->uints.gfx_thumbnail_upscale_threshold);

}

/*
 * Load a directory thumbnail from the parent folder's .media folder.
 * Path: {parent_path}/.media/{folder_name}.png
 * No type subfolder needed for directories.
 */
static void streamlined_load_dir_thumbnail(streamlined_t *strm,
      const char *dir_path, const char *parent_path)
{
   char thumb_path[PATH_MAX_LENGTH];
   const char *dir_name;
   settings_t *settings = config_get_ptr();

   if (!strm || string_is_empty(dir_path) || string_is_empty(parent_path))
      return;

   /* Check folder thumbnails setting */
   if (!settings->bools.menu_streamlined_show_folder_thumbnails)
   {
      gfx_thumbnail_reset(&strm->rom_thumbnail);
      strm->rom_thumbnail_path[0] = '\0';
      return;
   }

   /* Get directory basename */
   dir_name = path_basename(dir_path);
   if (string_is_empty(dir_name))
      return;

   /* Build path: {parent_path}/.media/{dirname}.png */
   fill_pathname_join_special(thumb_path, parent_path,
         ".media", sizeof(thumb_path));
   fill_pathname_join(thumb_path, thumb_path,
         dir_name, sizeof(thumb_path));
   strlcat(thumb_path, ".png", sizeof(thumb_path));

   /* Skip if same path already loaded */
   if (string_is_equal(thumb_path, strm->rom_thumbnail_path))
      return;

   strlcpy(strm->rom_thumbnail_path, thumb_path,
         sizeof(strm->rom_thumbnail_path));

   gfx_thumbnail_reset(&strm->rom_thumbnail);
   gfx_thumbnail_request_file(thumb_path, &strm->rom_thumbnail,
         settings->uints.gfx_thumbnail_upscale_threshold);

}

/*
 * Draw the save slot selector UI: thumbnail preview with polaroid frame and dot indicators.
 * Positioned on the right side of the screen, vertically centered.
 */
static void streamlined_draw_slot_selector(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   int i;
   /* Larger thumbnail - 45% of screen height, maintain 4:3 aspect for frame */
   int thumb_max_height = (int)(video_height * 0.45f);
   int thumb_max_width  = (int)(thumb_max_height * 4.0f / 3.0f);

   /* Polaroid frame dimensions */
   int frame_border     = (int)(5 * strm->scale_factor);   /* Side/top border */
   int frame_bottom     = (int)(28 * strm->scale_factor);  /* Thicker bottom chin for dots */
   int frame_width      = thumb_max_width + frame_border * 2;
   int frame_height     = thumb_max_height + frame_border + frame_bottom;

   int frame_x, frame_y;
   int thumb_x, thumb_y;
   int dot_y, dot_spacing, dot_radius;
   int total_dots_width;
   int dots_start_x;

   /* Calculate dot dimensions first (needed for vertical centering) */
   dot_radius  = (int)(4 * strm->scale_factor);
   dot_spacing = (int)(16 * strm->scale_factor);

   /* Position frame on right side, vertically centered with dots below */
   frame_x = video_width - strm->margin_x - frame_width;
   frame_y = (video_height - frame_height - dot_radius * 2 - (int)(16 * strm->scale_factor)) / 2;

   /* Thumbnail position inside frame */
   thumb_x = frame_x + frame_border;
   thumb_y = frame_y + frame_border;

   /* Draw polaroid frame (white background) */
   gfx_display_draw_quad(p_disp, userdata, video_width, video_height,
         frame_x, frame_y, frame_width, frame_height,
         video_width, video_height, streamlined_color_selection, NULL);

   /* Draw thumbnail if available */
   if (strm->savestate_thumbnail.status == GFX_THUMBNAIL_STATUS_AVAILABLE)
   {
      float draw_width, draw_height;

      /* Calculate aspect-correct dimensions */
      gfx_thumbnail_get_draw_dimensions(
            &strm->savestate_thumbnail,
            thumb_max_width, thumb_max_height, 1.0f,
            &draw_width, &draw_height);

      /* Center within thumbnail area */
      {
         int offset_x = (thumb_max_width - (int)draw_width) / 2;
         int offset_y = (thumb_max_height - (int)draw_height) / 2;

         gfx_thumbnail_draw(userdata, video_width, video_height,
               &strm->savestate_thumbnail,
               (float)(thumb_x + offset_x), (float)(thumb_y + offset_y),
               (unsigned)draw_width, (unsigned)draw_height,
               GFX_THUMBNAIL_ALIGN_CENTRE, 1.0f, 1.0f, NULL);
      }
   }
   else if (strm->savestate_thumbnail.status == GFX_THUMBNAIL_STATUS_MISSING)
   {
      /* Only show placeholder when we know the thumbnail is missing (not while loading) */
      const char *placeholder;
      char state_path[PATH_MAX_LENGTH];
      settings_t *settings = config_get_ptr();

      /* Check if save state exists (without .png) to determine message */
      if (runloop_get_savestate_path(state_path, sizeof(state_path), settings->ints.state_slot)
            && path_is_valid(state_path))
         placeholder = "No Screenshot";
      else
         placeholder = "Empty";

      /* Draw placeholder text centered in thumbnail area */
      {
         int text_width = streamlined_get_text_width(strm, placeholder, FONT_NORMAL);
         int text_x = thumb_x + (thumb_max_width - text_width) / 2;
         /* Center vertically: account for font baseline by adding ~1/3 of font size */
         int text_y = thumb_y + thumb_max_height / 2 + (int)(strm->font_size * 0.35f);

         streamlined_draw_text(strm, p_disp, video_width, video_height,
               text_x, text_y,
               placeholder, streamlined_color_text_dark, false);
      }
   }

   /* Draw slot indicators in the polaroid chin: 'A' for auto, dots for 0-7 */
   total_dots_width = STREAMLINED_NUM_SLOTS * (dot_radius * 2)
         + (STREAMLINED_NUM_SLOTS - 1) * (dot_spacing - dot_radius * 2);
   dot_y = thumb_y + thumb_max_height + (frame_bottom - dot_radius * 2) / 2;
   dots_start_x = frame_x + (frame_width - total_dots_width) / 2;

   for (i = 0; i < STREAMLINED_NUM_SLOTS; i++)
   {
      int dot_cx = dots_start_x + i * dot_spacing + dot_radius;
      bool is_selected = (i == strm->preview_slot);
      float *color = is_selected ? streamlined_color_accent : streamlined_color_black;
      int r = is_selected ? dot_radius : (int)(dot_radius * 0.6f);
      /* Adjust y position to keep dots vertically centered regardless of size */
      int cy = dot_y + dot_radius;

      if (i == 0)
      {
         /* Draw 'A' for Auto slot using tiny font, centered on dot line */
         int text_width = streamlined_get_text_width_tiny(strm, "A");
         int text_x = dot_cx - text_width / 2;
         /* Center 'A' vertically: baseline + 0.35*font_size ≈ visual center */
         int text_y = cy + (int)(strm->font_size_tiny * 0.35f);
         streamlined_draw_text_tiny(strm, p_disp, video_width, video_height,
               text_x, text_y, "A",
               is_selected ? streamlined_color_text_accent : streamlined_color_text_dark);
      }
      else
      {
         streamlined_draw_filled_circle(strm, p_disp, userdata,
               dot_cx, cy, r,
               video_width, video_height, color);
      }
   }
}

/*
 * Draw the ROM/directory thumbnail on the right side of the screen.
 * Aspect-correct, max 50% width, padded from edges, positioned below title.
 */
static void streamlined_draw_rom_thumbnail(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   float draw_width, draw_height;
   int max_w, max_h, avail_top, avail_bottom, avail_h;
   int draw_x, draw_y;

   /* Max width: 50% of screen */
   max_w = (int)(video_width * 0.5f) - strm->margin_x;

   /* Available vertical area: below title, above footer */
   avail_top    = strm->margin_y + (int)(strm->font_size_title * 1.4f);
   avail_bottom = (int)(video_height - 78.0f * strm->scale_factor);
   avail_h      = avail_bottom - avail_top - strm->margin_y * 2;

   if (avail_h <= 0 || max_w <= 0)
      return;

   max_h = avail_h;

   /* Calculate aspect-correct dimensions */
   gfx_thumbnail_get_draw_dimensions(
         &strm->rom_thumbnail,
         max_w, max_h, 1.0f,
         &draw_width, &draw_height);

   /* Position: right-aligned with margin, vertically centered in available area */
   draw_x = video_width - strm->margin_x - (int)draw_width;
   draw_y = avail_top + strm->margin_y + (avail_h - (int)draw_height) / 2;

   gfx_thumbnail_draw(userdata, video_width, video_height,
         &strm->rom_thumbnail,
         (float)draw_x, (float)draw_y,
         (unsigned)draw_width, (unsigned)draw_height,
         GFX_THUMBNAIL_ALIGN_CENTRE, 1.0f, 1.0f, NULL);
}

/* ======================================================================
 * MENU STACK SYNC (tvOS back button support)
 * ====================================================================== */

static void streamlined_sync_menu_stack(streamlined_t *strm)
{
   struct menu_state *menu_st;
   menu_list_t *menu_list;
   file_list_t *menu_stack;
   bool in_submenu;

   if (!strm || !strm->is_custom_main_menu)
      return;

   menu_st = menu_state_get_ptr();
   if (!menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list || !menu_list->menu_stack[0])
      return;

   menu_stack = menu_list->menu_stack[0];
   in_submenu = strm->in_folder
      || strm->in_favorites
      || strm->in_main_settings_submenu
      || strm->selecting_core
      || strm->selecting_core_for_folder
      || strm->selecting_core_for_game
      || strm->in_options_menu
      || strm->in_search_mode
      || strm->in_random_preview
      || strm->in_game_switcher
      || strm->in_delete_confirm
      || strm->delete_done;

   if (in_submenu && menu_stack->size == 1)
   {
      /* Push marker so tvOS menuIsAtTop() sees size > 1 */
      file_list_append(menu_stack, "",
            msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU), 0, 0, 0);
   }
   else if (!in_submenu && menu_stack->size > 1)
   {
      /* Pop marker — back at top level */
      file_list_pop(menu_stack, NULL);
   }
}

/* ======================================================================
 * RANDOM GAME PREVIEW RENDERING
 * ====================================================================== */

/*
 * Load the thumbnail for a random game preview.
 * Uses the same .media folder structure as the game list browser.
 */
static void streamlined_load_random_thumbnail(streamlined_t *strm)
{
   char thumb_path[PATH_MAX_LENGTH];
   char name_buf[PATH_MAX_LENGTH];
   const char *type_folder;
   const char *base_name;
   char *ext;
   settings_t *settings = config_get_ptr();

   strm->random_has_thumbnail = false;

   if (string_is_empty(strm->random_game_path))
      return;

   if (settings->uints.gfx_thumbnails == 0)
      return;

   switch (settings->uints.gfx_thumbnails)
   {
      case 1:  type_folder = "Screenshot"; break;
      case 2:  type_folder = "Title";      break;
      case 3:  type_folder = "Boxart";     break;
      default: return;
   }

   base_name = path_basename(strm->random_game_path);
   if (string_is_empty(base_name))
      return;

   strlcpy(name_buf, base_name, sizeof(name_buf));
   ext = strrchr(name_buf, '.');
   if (ext)
      *ext = '\0';

   fill_pathname_join_special(thumb_path, strm->options_folder_path,
         ".media", sizeof(thumb_path));
   fill_pathname_join(thumb_path, thumb_path,
         type_folder, sizeof(thumb_path));
   fill_pathname_join(thumb_path, thumb_path,
         name_buf, sizeof(thumb_path));
   strlcat(thumb_path, ".png", sizeof(thumb_path));

   if (!path_is_valid(thumb_path))
   {
      strm->random_has_thumbnail = false;
      strm->random_show_text = true;
      return;
   }

   strlcpy(strm->random_thumbnail_path, thumb_path,
         sizeof(strm->random_thumbnail_path));
   gfx_thumbnail_reset(&strm->random_thumbnail);
   gfx_thumbnail_request_file(thumb_path, &strm->random_thumbnail,
         settings->uints.gfx_thumbnail_upscale_threshold);
   strm->random_has_thumbnail = true;
}

/* ======================================================================
 * GAME SWITCHER HELPERS
 * ====================================================================== */

/*
 * Load the thumbnail for the current game switcher entry.
 * Priority: 1. Autosave screenshot (.auto.png)
 *           2. Primary thumbnail from .media folder
 */
static void streamlined_load_game_switcher_thumbnail(streamlined_t *strm)
{
   char thumb_path[PATH_MAX_LENGTH];
   char name_buf[PATH_MAX_LENGTH];
   char parent_dir[PATH_MAX_LENGTH];
   const char *type_folder;
   const char *base_name;
   char *ext;
   settings_t *settings = config_get_ptr();

   strm->game_switcher_has_thumbnail = false;
   gfx_thumbnail_reset(&strm->game_switcher_thumbnail);
   strm->game_switcher_thumbnail_path[0] = '\0';

   if (string_is_empty(strm->game_switcher_content_path))
      return;

   /* Tier 1: Autosave screenshot (.auto.png) */
   if (!string_is_empty(strm->game_switcher_core_path)
         && streamlined_find_autosave_path(
               strm->game_switcher_content_path,
               strm->game_switcher_core_path,
               ".auto.png", thumb_path, sizeof(thumb_path)))
   {
      strlcpy(strm->game_switcher_thumbnail_path, thumb_path,
            sizeof(strm->game_switcher_thumbnail_path));
      gfx_thumbnail_request_file(thumb_path, &strm->game_switcher_thumbnail,
            settings->uints.gfx_thumbnail_upscale_threshold);
      strm->game_switcher_thumbnail.flags |= GFX_THUMB_FLAG_CORE_ASPECT;
      strm->game_switcher_has_thumbnail = true;
      return;
   }

   /* Tier 2: Primary thumbnail from .media folder */
   if (settings->uints.gfx_thumbnails == 0)
      return;

   switch (settings->uints.gfx_thumbnails)
   {
      case 1:  type_folder = "Screenshot"; break;
      case 2:  type_folder = "Title";      break;
      case 3:  type_folder = "Boxart";     break;
      default: return;
   }

   {
      char m3u_path[PATH_MAX_LENGTH];
      const char *thumb_content = strm->game_switcher_content_path;

      if (streamlined_find_m3u_for_content(strm->game_switcher_content_path,
            m3u_path, sizeof(m3u_path)))
         thumb_content = m3u_path;

      base_name = path_basename(thumb_content);
      if (string_is_empty(base_name))
         return;

      strlcpy(name_buf, base_name, sizeof(name_buf));
      ext = strrchr(name_buf, '.');
      if (ext)
         *ext = '\0';

      streamlined_get_content_folder_path(thumb_content,
            parent_dir, sizeof(parent_dir));
   }

   fill_pathname_join_special(thumb_path, parent_dir,
         ".media", sizeof(thumb_path));
   fill_pathname_join(thumb_path, thumb_path,
         type_folder, sizeof(thumb_path));
   fill_pathname_join(thumb_path, thumb_path,
         name_buf, sizeof(thumb_path));
   strlcat(thumb_path, ".png", sizeof(thumb_path));

   if (!path_is_valid(thumb_path))
      return;

   strlcpy(strm->game_switcher_thumbnail_path, thumb_path,
         sizeof(strm->game_switcher_thumbnail_path));
   gfx_thumbnail_request_file(thumb_path, &strm->game_switcher_thumbnail,
         settings->uints.gfx_thumbnail_upscale_threshold);
   strm->game_switcher_has_thumbnail = true;
}

/*
 * Load the game switcher entry at the current index from the history playlist.
 * Resolves paths, builds display name, checks savestate, loads thumbnail.
 */
static void streamlined_load_game_switcher_entry(streamlined_t *strm)
{
   playlist_t *history = g_defaults.content_history;
   const struct playlist_entry *pl_entry = NULL;
   char resolved_path[PATH_MAX_LENGTH];
   char parent_dir[PATH_MAX_LENGTH];

   strm->game_switcher_display_name[0] = '\0';
   strm->game_switcher_content_path[0] = '\0';
   strm->game_switcher_core_path[0] = '\0';
   strm->game_switcher_has_savestate = false;

   if (!history || playlist_size(history) == 0)
      return;

   if (strm->game_switcher_index >= playlist_size(history))
      strm->game_switcher_index = 0;

   playlist_get_index(history, strm->game_switcher_index, &pl_entry);
   if (!pl_entry || string_is_empty(pl_entry->path))
      return;

   /* Resolve path (iOS abbreviation fix) */
   strlcpy(resolved_path, pl_entry->path, sizeof(resolved_path));
   playlist_resolve_path(PLAYLIST_LOAD, false,
         resolved_path, sizeof(resolved_path));

   strlcpy(strm->game_switcher_content_path, resolved_path,
         sizeof(strm->game_switcher_content_path));

   /* Resolve core path */
   streamlined_resolve_content_core(resolved_path,
         strm->game_switcher_core_path,
         sizeof(strm->game_switcher_core_path),
         NULL, 0,
         g_defaults.content_history);

   /* Build display name */
   {
      char m3u_path[PATH_MAX_LENGTH];
      if (streamlined_find_m3u_for_content(resolved_path,
            m3u_path, sizeof(m3u_path)))
      {
         fill_pathname_parent_dir(parent_dir, m3u_path, sizeof(parent_dir));
         streamlined_strip_trailing_slash(parent_dir);
         streamlined_get_display_name(parent_dir,
               strm->game_switcher_display_name,
               sizeof(strm->game_switcher_display_name), true);
      }
      else if (!string_is_empty(pl_entry->label))
         strlcpy(strm->game_switcher_display_name, pl_entry->label,
               sizeof(strm->game_switcher_display_name));
      else
         streamlined_get_display_name(resolved_path,
               strm->game_switcher_display_name,
               sizeof(strm->game_switcher_display_name), false);
   }

   /* Check savestate */
   if (!string_is_empty(strm->game_switcher_core_path))
      strm->game_switcher_has_savestate = streamlined_check_savestate(
            resolved_path, strm->game_switcher_core_path);

   /* Load thumbnail */
   streamlined_load_game_switcher_thumbnail(strm);
}

/*
 * Populate the menu list with history entries (for Text view).
 */
static void streamlined_populate_game_switcher_menu(streamlined_t *strm)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   playlist_t *history;
   size_t pl_size, j;

   if (!menu_st || !strm)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   menu_entries_clear(list);

   history = g_defaults.content_history;
   if (!history || playlist_size(history) == 0)
   {
      menu_entries_append(list,
            "No recent games", "",
            MSG_UNKNOWN, FILE_TYPE_NONE,
            0, 0, NULL);
      return;
   }

   pl_size = playlist_size(history);
   for (j = 0; j < pl_size; j++)
   {
      const struct playlist_entry *pl_entry = NULL;
      char resolved_path[PATH_MAX_LENGTH];
      char display_name[256];

      playlist_get_index(history, j, &pl_entry);
      if (!pl_entry || string_is_empty(pl_entry->path))
         continue;

      strlcpy(resolved_path, pl_entry->path, sizeof(resolved_path));
      playlist_resolve_path(PLAYLIST_LOAD, false,
            resolved_path, sizeof(resolved_path));

      {
         char m3u_path[PATH_MAX_LENGTH];
         if (streamlined_find_m3u_for_content(resolved_path,
               m3u_path, sizeof(m3u_path)))
         {
            char m3u_parent[PATH_MAX_LENGTH];
            fill_pathname_parent_dir(m3u_parent, m3u_path, sizeof(m3u_parent));
            streamlined_strip_trailing_slash(m3u_parent);
            streamlined_get_display_name(m3u_parent,
                  display_name, sizeof(display_name), true);
         }
         else if (!string_is_empty(pl_entry->label))
            strlcpy(display_name, pl_entry->label, sizeof(display_name));
         else
            streamlined_get_display_name(resolved_path,
                  display_name, sizeof(display_name), false);
      }

      menu_entries_append(list,
            display_name, resolved_path,
            MSG_UNKNOWN, FILE_TYPE_PLAIN,
            0, 0, NULL);
   }
}

/*
 * Render the game switcher (Image-centric view).
 * Full-screen thumbnail with game name in footer. L/R arrows shown.
 */
static void streamlined_render_game_switcher(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   settings_t *settings = config_get_ptr();
   float scale         = strm->scale_factor;
   float footer_height = 78.0f * scale;
   int avail_top, avail_bottom;

   streamlined_sync_menu_stack(strm);

   /* Image-centric view only */
   if (settings && settings->uints.menu_streamlined_game_switcher_view == 1)
      return;

   /* Check for empty history */
   {
      playlist_t *history = g_defaults.content_history;
      if (!history || playlist_size(history) == 0)
      {
         /* "No recent games" centered */
         const char *msg = "No recent games";
         int msg_w = streamlined_get_text_width(strm, msg, FONT_NORMAL);
         int msg_x = ((int)video_width - msg_w) / 2;
         int msg_y = (int)(video_height / 2) + (int)(strm->font_size * 0.35f);
         streamlined_draw_text(strm, p_disp, video_width, video_height,
               msg_x, msg_y, msg, streamlined_color_text, false);
         return;
      }
   }

   avail_top = strm->margin_y;
   avail_bottom = (int)(video_height - footer_height);

   /* Draw thumbnail or "No Artwork" */
   if (strm->game_switcher_has_thumbnail
         && strm->game_switcher_thumbnail.status == GFX_THUMBNAIL_STATUS_AVAILABLE)
   {
      float draw_width, draw_height;
      int max_w = (int)video_width - strm->margin_x * 2;
      int max_h = avail_bottom - avail_top - strm->margin_y;
      int draw_x, draw_y;

      gfx_thumbnail_get_draw_dimensions(
            &strm->game_switcher_thumbnail,
            max_w, max_h, 1.0f,
            &draw_width, &draw_height);

      draw_x = ((int)video_width - (int)draw_width) / 2;
      draw_y = avail_top + (avail_bottom - avail_top - (int)draw_height) / 2;

      gfx_thumbnail_draw(userdata, video_width, video_height,
            &strm->game_switcher_thumbnail,
            (float)draw_x, (float)draw_y,
            (unsigned)draw_width, (unsigned)draw_height,
            GFX_THUMBNAIL_ALIGN_CENTRE, 1.0f, 1.0f, NULL);
   }
   else
   {
      /* "No Artwork" centered */
      const char *no_art = "No Artwork";
      int text_w = streamlined_get_text_width(strm, no_art, FONT_NORMAL);
      int text_x = ((int)video_width - text_w) / 2;
      int center_y = (avail_top + avail_bottom) / 2
            + (int)(strm->font_size * 0.35f);

      streamlined_draw_text(strm, p_disp, video_width, video_height,
            text_x, center_y, no_art, streamlined_color_text_muted, false);
   }

   /* Footer: ◀ Game Name ▶ */
   {
      float footer_center_y = (float)video_height - (footer_height / 2.0f);
      float text_y = footer_center_y + (strm->font_size * 0.35f);
      int max_name_w = (int)video_width - strm->margin_x * 4;
      char truncated[256];
      int name_w, arrow_w, total_w, start_x;
      const char *left_arrow = "\xe2\x97\x80";   /* ◀ UTF-8 */
      const char *right_arrow = "\xe2\x96\xb6";  /* ▶ UTF-8 */
      int arrow_gap = (int)(12 * scale);

      streamlined_truncate_text(strm, strm->game_switcher_display_name,
            truncated, sizeof(truncated), max_name_w, FONT_NORMAL);

      name_w = streamlined_get_text_width(strm, truncated, FONT_NORMAL);
      arrow_w = streamlined_get_text_width(strm, left_arrow, FONT_NORMAL);
      total_w = arrow_w + arrow_gap + name_w + arrow_gap + arrow_w;
      start_x = ((int)video_width - total_w) / 2;

      streamlined_draw_text(strm, p_disp, video_width, video_height,
            start_x, (int)text_y,
            left_arrow, streamlined_color_text_muted, false);
      streamlined_draw_text(strm, p_disp, video_width, video_height,
            start_x + arrow_w + arrow_gap, (int)text_y,
            truncated, streamlined_color_text, false);
      streamlined_draw_text(strm, p_disp, video_width, video_height,
            start_x + arrow_w + arrow_gap + name_w + arrow_gap, (int)text_y,
            right_arrow, streamlined_color_text_muted, false);
   }
}

/*
 * Enter the game switcher from the main menu.
 * Saves current selection, sets flags, and branches on gs_view to either
 * load the image-centric entry or populate the text-view menu list.
 */
static void streamlined_enter_game_switcher(streamlined_t *strm)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   settings_t *gs_settings    = config_get_ptr();
   unsigned gs_view = gs_settings
         ? gs_settings->uints.menu_streamlined_game_switcher_view : 0;

   strm->game_switcher_saved_selection = menu_st->selection_ptr;
   strm->game_switcher_index = 0;
   strm->in_game_switcher = true;
   strm->game_switcher_in_glo = false;

   if (gs_view == 0)
      streamlined_load_game_switcher_entry(strm);
   else
   {
      streamlined_populate_game_switcher_menu(strm);
      gfx_thumbnail_reset(&strm->rom_thumbnail);
      strm->rom_thumbnail_path[0] = '\0';
      strm->rom_thumbnail_selection = (size_t)-1;
      menu_st->selection_ptr = 0;
   }
}

/*
 * Refresh the game switcher view after a state change (e.g. returning from
 * GLO, adding/removing favorites, deleting autosave).
 * Branches on gs_view to reload the image entry or repopulate the text list.
 */
static void streamlined_refresh_game_switcher_view(streamlined_t *strm)
{
   settings_t *gs_settings = config_get_ptr();
   unsigned gs_view = gs_settings
         ? gs_settings->uints.menu_streamlined_game_switcher_view : 0;

   if (gs_view == 0)
      streamlined_load_game_switcher_entry(strm);
   else
   {
      streamlined_populate_game_switcher_menu(strm);
      menu_state_get_ptr()->selection_ptr = strm->game_switcher_index;
   }
}

/*
 * Render the random game preview screen.
 * Thumbnail View: centered thumbnail with game-browsing hint bar.
 * Text View: centered "Random Game" header + display name + hint bar.
 * Footer pills drawn after content (same order as streamlined_render_menu).
 */
static void streamlined_render_random_preview(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   float scale            = strm->scale_factor;
   float footer_height    = 78.0f * scale;
   float footer_margin    = 40.0f * scale;
   float pill_h           = strm->font_size_small + 8.0f * scale;
   float pill_pad         = 10.0f * scale;
   float pill_text_gap    = 8.0f * scale;
   float btn_spacing      = 20.0f * scale;
   float footer_center_y  = (float)video_height - (footer_height / 2.0f);
   float pill_y           = footer_center_y - (pill_h / 2.0f);
   float text_y           = footer_center_y + (strm->font_size_small * 0.35f);

   /* Draw content first */
   if (strm->random_show_text || !strm->random_has_thumbnail)
   {
      /* Text View: header + centered game name */
      const char *header = "Random Game";
      int header_w = streamlined_get_title_width(strm, header);
      int header_x = ((int)video_width - header_w) / 2;
      int header_y = strm->margin_y + (int)(strm->font_size_title * 0.9f);

      streamlined_draw_title(strm, p_disp, video_width, video_height,
            header_x, header_y, header, streamlined_color_text);

      {
         int avail_top = strm->margin_y + (int)(strm->font_size_title * 1.4f);
         int avail_bottom = (int)(video_height - footer_height);
         int center_y = (avail_top + avail_bottom) / 2
               + (int)(strm->font_size * 0.35f);
         int max_w = (int)video_width - strm->margin_x * 2;
         char truncated[256];

         streamlined_truncate_text(strm, strm->random_display_name,
               truncated, sizeof(truncated), max_w, FONT_NORMAL);

         {
            int name_w = streamlined_get_text_width(strm, truncated, FONT_NORMAL);
            int name_x = ((int)video_width - name_w) / 2;

            streamlined_draw_text(strm, p_disp, video_width, video_height,
                  name_x, center_y, truncated, streamlined_color_text, false);
         }
      }
   }
   else
   {
      /* Thumbnail View: centered thumbnail */
      if (strm->random_thumbnail.status == GFX_THUMBNAIL_STATUS_AVAILABLE)
      {
         float draw_width, draw_height;
         int avail_top = strm->margin_y;
         int avail_bottom = (int)(video_height - footer_height);
         int max_w = (int)video_width - strm->margin_x * 2;
         int max_h = avail_bottom - avail_top - strm->margin_y;
         int draw_x, draw_y;

         gfx_thumbnail_get_draw_dimensions(
               &strm->random_thumbnail,
               max_w, max_h, 1.0f,
               &draw_width, &draw_height);

         draw_x = ((int)video_width - (int)draw_width) / 2;
         draw_y = avail_top + (avail_bottom - avail_top - (int)draw_height) / 2;

         gfx_thumbnail_draw(userdata, video_width, video_height,
               &strm->random_thumbnail,
               (float)draw_x, (float)draw_y,
               (unsigned)draw_width, (unsigned)draw_height,
               GFX_THUMBNAIL_ALIGN_CENTRE, 1.0f, 1.0f, NULL);
      }
      else
      {
         int center_y = (int)(video_height - footer_height) / 2
               + (int)(strm->font_size * 0.35f);
         int name_w = streamlined_get_text_width(strm,
               strm->random_display_name, FONT_NORMAL);
         int name_x = ((int)video_width - name_w) / 2;

         streamlined_draw_text(strm, p_disp, video_width, video_height,
               name_x, center_y,
               strm->random_display_name, streamlined_color_text, false);
      }
   }

   /* Footer: pills + text */
   {
      const char *back_key   = "B";
      const char *ok_key     = "A";
      const char *back_str   = msg_hash_to_str(
            MENU_ENUM_LABEL_VALUE_BASIC_MENU_CONTROLS_BACK);
      const char *ok_str     = "Play";

      int back_key_w  = font_driver_get_message_width(
            strm->font_small.font, back_key, strlen(back_key), 1.0f);
      int ok_key_w    = font_driver_get_message_width(
            strm->font_small.font, ok_key, strlen(ok_key), 1.0f);
      int back_pill_w = back_key_w + (int)(pill_pad * 2.0f);
      int ok_pill_w   = ok_key_w + (int)(pill_pad * 2.0f);
      int ok_label_w  = font_driver_get_message_width(
            strm->font_small.font, ok_str, strlen(ok_str), 1.0f);
      float right_x   = (float)video_width - footer_margin;
      float ok_pill_x  = right_x - (float)ok_label_w - pill_text_gap - (float)ok_pill_w;

      /* Flush stale GPU pipeline state after thumbnail/content drawing */
      gfx_display_draw_text(strm->font_small.font,
            back_key,
            (int)(footer_margin + pill_pad),
            (int)text_y,
            video_width, video_height,
            streamlined_color_text_dark,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);

      /* Left: [B] Back */
      streamlined_draw_rounded_pill(strm, p_disp, userdata,
            (int)footer_margin, (int)pill_y, back_pill_w, (int)pill_h,
            video_width, video_height, streamlined_color_selection);
      gfx_display_draw_text(strm->font_small.font,
            back_key,
            (int)(footer_margin + pill_pad),
            (int)text_y,
            video_width, video_height,
            streamlined_color_text_dark,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
      gfx_display_draw_text(strm->font_small.font,
            back_str,
            (int)(footer_margin + (float)back_pill_w + pill_text_gap),
            (int)text_y,
            video_width, video_height,
            streamlined_color_text,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);

      /* Right: [A] Play */
      streamlined_draw_rounded_pill(strm, p_disp, userdata,
            (int)ok_pill_x, (int)pill_y, ok_pill_w, (int)pill_h,
            video_width, video_height, streamlined_color_selection);
      gfx_display_draw_text(strm->font_small.font,
            ok_key,
            (int)(ok_pill_x + pill_pad),
            (int)text_y,
            video_width, video_height,
            streamlined_color_text_dark,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
      gfx_display_draw_text(strm->font_small.font,
            ok_str,
            (int)(ok_pill_x + (float)ok_pill_w + pill_text_gap),
            (int)text_y,
            video_width, video_height,
            streamlined_color_text,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);

      /* [X] Resume — only if savestate exists */
      if (strm->random_has_savestate)
      {
         const char *resume_key = "X";
         const char *resume_str = "Resume";
         int resume_key_w   = font_driver_get_message_width(
               strm->font_small.font, resume_key, strlen(resume_key), 1.0f);
         int resume_pill_w  = resume_key_w + (int)(pill_pad * 2.0f);
         int resume_label_w = font_driver_get_message_width(
               strm->font_small.font, resume_str, strlen(resume_str), 1.0f);
         float resume_pill_x = ok_pill_x - btn_spacing
               - (float)resume_label_w - pill_text_gap - (float)resume_pill_w;

         streamlined_draw_rounded_pill(strm, p_disp, userdata,
               (int)resume_pill_x, (int)pill_y, resume_pill_w, (int)pill_h,
               video_width, video_height, streamlined_color_selection);
         gfx_display_draw_text(strm->font_small.font,
               resume_key,
               (int)(resume_pill_x + pill_pad),
               (int)text_y,
               video_width, video_height,
               streamlined_color_text_dark,
               TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
         gfx_display_draw_text(strm->font_small.font,
               resume_str,
               (int)(resume_pill_x + (float)resume_pill_w + pill_text_gap),
               (int)text_y,
               video_width, video_height,
               streamlined_color_text,
               TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
      }
   }
}

/* ======================================================================
 * SEARCH KEYBOARD RENDERING (non-tvOS)
 * ====================================================================== */

#if !TARGET_OS_TV
/*
 * Render the search bar and QWERTY keyboard for search mode.
 * Returns the total height consumed so menu items can be offset.
 */
static int streamlined_render_search_keyboard(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   int row, col;
   int key_w = (int)(strm->font_size * 1.8f);
   int key_h = (int)(strm->font_size * 1.5f);
   int key_gap = (int)(4 * strm->scale_factor);
   int kb_start_y = strm->margin_y + (int)(strm->font_size_title * 1.4f);
   int search_bar_h = (int)(strm->font_size * 1.8f);
   int search_bar_y = kb_start_y;
   int kb_y = search_bar_y + search_bar_h + key_gap * 2;
   int max_row_width = 10 * key_w + 9 * key_gap;
   int kb_start_x = (video_width - max_row_width) / 2;

   /* Draw search bar */
   {
      char search_display[260];
      int bar_width = max_row_width;
      int bar_x = kb_start_x;
      int text_y = search_bar_y + search_bar_h / 2 + (int)(strm->font_size * 0.30f);

      streamlined_draw_rounded_pill(strm, p_disp, userdata,
            bar_x, search_bar_y, bar_width, search_bar_h,
            video_width, video_height, streamlined_color_selection);

      if (strm->search_query[0] != '\0')
         snprintf(search_display, sizeof(search_display), "%s|", strm->search_query);
      else
         strlcpy(search_display, "|", sizeof(search_display));

      streamlined_draw_text(strm, p_disp, video_width, video_height,
            bar_x + strm->pill_padding, text_y,
            search_display, streamlined_color_text_dark, false);
   }

   /* Draw keyboard rows */
   for (row = 0; row < STREAMLINED_KB_NUM_ROWS; row++)
   {
      int row_len = streamlined_kb_row_lens[row];
      float offset = streamlined_kb_row_offsets[row];
      int row_y = kb_y + row * (key_h + key_gap);

      if (row == STREAMLINED_KB_NUM_ROWS - 1)
      {
         /* Space bar - wide centered key */
         int space_w = key_w * 5 + key_gap * 4;
         int space_x = kb_start_x + (max_row_width - space_w) / 2;
         bool is_selected = !strm->search_focus_list
               && strm->search_kb_row == row && strm->search_kb_col == 0;
         float *bg_color = is_selected ? streamlined_color_selection : streamlined_color_bg;
         uint32_t text_color = is_selected
               ? streamlined_color_text_dark : streamlined_color_text;
         int text_y = row_y + key_h / 2 + (int)(strm->font_size * 0.30f);
         const char *space_label = "SPACE";
         int label_w = streamlined_get_text_width(strm, space_label, FONT_NORMAL);

         streamlined_draw_rounded_pill(strm, p_disp, userdata,
               space_x, row_y, space_w, key_h,
               video_width, video_height, bg_color);
         streamlined_draw_text(strm, p_disp, video_width, video_height,
               space_x + (space_w - label_w) / 2, text_y,
               space_label, text_color, false);
      }
      else
      {
         int row_x = kb_start_x + (int)(offset * (float)(key_w + key_gap));
         for (col = 0; col < row_len; col++)
         {
            char ch = streamlined_kb_rows[row][col];
            bool is_selected = !strm->search_focus_list
                  && strm->search_kb_row == row && strm->search_kb_col == col;
            float *bg_color = is_selected
                  ? streamlined_color_selection : streamlined_color_bg;
            uint32_t text_color = is_selected
                  ? streamlined_color_text_dark : streamlined_color_text;
            int key_x = row_x + col * (key_w + key_gap);
            int text_y = row_y + key_h / 2 + (int)(strm->font_size * 0.30f);
            char key_str[4];
            int char_w;

            if (ch == '\x08')
               strlcpy(key_str, "\xe2\x8c\xab", sizeof(key_str)); /* ⌫ UTF-8 */
            else
            {
               key_str[0] = ch;
               key_str[1] = '\0';
            }

            streamlined_draw_rounded_pill(strm, p_disp, userdata,
                  key_x, row_y, key_w, key_h,
                  video_width, video_height, bg_color);
            char_w = streamlined_get_text_width(strm, key_str, FONT_NORMAL);
            streamlined_draw_text(strm, p_disp, video_width, video_height,
                  key_x + (key_w - char_w) / 2, text_y,
                  key_str, text_color, false);
         }
      }
   }

   return search_bar_h + key_gap * 2
         + STREAMLINED_KB_NUM_ROWS * (key_h + key_gap);
}
#endif

/* ======================================================================
 * MENU RENDERING
 * ====================================================================== */

static void streamlined_render_menu(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   size_t list_size, selection, i, start_idx, max_visible;
   int y, item_height;
   char title_buf[256];
   char selected_sublabel[512];
   bool show_sublabel;

   if (!strm->font.font || !p_disp || !menu_st)
      return;

   /* Keep menu stack in sync for tvOS back button handling */
   streamlined_sync_menu_stack(strm);

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list || list->size == 0)
      return;

   list_size = list->size;
   selection = menu_st->selection_ptr;
   selected_sublabel[0] = '\0';
   show_sublabel = strm->in_settings_submenu
                || strm->in_main_settings_submenu
                || strm->return_to_settings_submenu
                || strm->return_to_main_settings_submenu;
   item_height = strm->font.line_height;
   if (item_height <= 0)
      item_height = 20;

   /*
    * Detect if on Save (index 1) or Load (index 2) in main quick menu.
    * Show the slot selector UI when these entries are selected.
    */
   {
      bool was_showing = strm->show_slot_selector;
      strm->show_slot_selector = false;

      if (strm->is_quick_menu && !strm->in_settings_submenu)
      {
         if (selection == 1 || selection == 2)
            strm->show_slot_selector = true;
      }

      /* When selection changes to save/load, load the current slot's thumbnail */
      if (strm->show_slot_selector && (!was_showing || selection != strm->last_selection))
      {
         settings_t *settings = config_get_ptr();
         int state_slot = settings->ints.state_slot;

         /* Convert state_slot to preview_slot: state_slot -1 = preview 0 (Auto) */
         /* Clamp state_slot to -1 to 7 range */
         if (state_slot < -1)
            state_slot = -1;
         else if (state_slot > 7)
            state_slot = 7;
         settings->ints.state_slot = state_slot;

         /* preview_slot = state_slot + 1 */
         strm->preview_slot = state_slot + 1;
         streamlined_load_slot_thumbnail(strm, strm->preview_slot);
      }

      strm->last_selection = selection;
   }

   /* Load ROM/directory thumbnail on selection change in custom main menu */
   if (strm->is_custom_main_menu && selection != strm->rom_thumbnail_selection)
   {
      strm->rom_thumbnail_selection = selection;

      if (selection < list_size)
      {
         menu_entry_t entry;
         MENU_ENTRY_INITIALIZE(entry);
         entry.flags |= MENU_ENTRY_FLAG_RICH_LABEL_ENABLED
                      | MENU_ENTRY_FLAG_VALUE_ENABLED
                      | MENU_ENTRY_FLAG_LABEL_ENABLED;
         menu_entry_get(&entry, 0, (unsigned)selection, NULL, true);

         if (entry.type == FILE_TYPE_DIRECTORY
               && !string_is_empty(entry.label))
         {
            streamlined_load_dir_thumbnail(strm, entry.label,
                  strm->current_folder_path);
         }
         else if (!string_is_empty(entry.label)
               && path_is_valid(entry.label))
         {
            if (strm->in_favorites || strm->in_game_switcher)
            {
               /* Temporarily set current_folder_path for thumbnail lookup */
               char saved_folder[PATH_MAX_LENGTH];
               char m3u_path[PATH_MAX_LENGTH];
               char content_folder[PATH_MAX_LENGTH];
               const char *thumb_content = entry.label;

               strlcpy(saved_folder, strm->current_folder_path, sizeof(saved_folder));

               if (streamlined_find_m3u_for_content(entry.label,
                     m3u_path, sizeof(m3u_path)))
                  thumb_content = m3u_path;

               streamlined_get_content_folder_path(thumb_content,
                     content_folder, sizeof(content_folder));
               strlcpy(strm->current_folder_path, content_folder,
                     sizeof(strm->current_folder_path));

               streamlined_load_rom_thumbnail(strm, thumb_content);
               strlcpy(strm->current_folder_path, saved_folder,
                     sizeof(strm->current_folder_path));
            }
            else
               streamlined_load_rom_thumbnail(strm, entry.label);
         }
         else
         {
            gfx_thumbnail_reset(&strm->rom_thumbnail);
            strm->rom_thumbnail_path[0] = '\0';
         }

         /* Check if selected ROM has a save state (only in folder with core.txt) */
         strm->selected_is_file = false;
         strm->selected_has_savestate = false;

         if (entry.type == FILE_TYPE_PLAIN && !string_is_empty(entry.label))
         {
            strm->selected_is_file = true;
            if (strm->in_favorites)
            {
               /* Resolve core for favorites entry */
               char fav_core[PATH_MAX_LENGTH];
               if (streamlined_resolve_content_core(entry.label,
                     fav_core, sizeof(fav_core),
                     NULL, 0,
                     g_defaults.content_favorites))
                  strm->selected_has_savestate = streamlined_check_savestate(
                        entry.label, fav_core);
            }
            else if (strm->in_game_switcher)
            {
               char gs_core[PATH_MAX_LENGTH];
               if (streamlined_resolve_content_core(entry.label,
                     gs_core, sizeof(gs_core),
                     NULL, 0,
                     g_defaults.content_history))
                  strm->selected_has_savestate = streamlined_check_savestate(
                        entry.label, gs_core);
            }
            else if (!string_is_empty(strm->folder_core_path))
            {
               strm->selected_has_savestate = streamlined_check_savestate(
                     entry.label, strm->folder_core_path);
            }
            else if (strm->in_search_mode && !string_is_empty(strm->options_core_path))
            {
               strm->selected_has_savestate = streamlined_check_savestate(
                     entry.label, strm->options_core_path);
            }
            else
            {
               char temp_core[PATH_MAX_LENGTH];
               char parent_dir[PATH_MAX_LENGTH];
               fill_pathname_parent_dir(parent_dir, entry.label, sizeof(parent_dir));
               if (streamlined_read_folder_core(parent_dir, temp_core, sizeof(temp_core)))
                  strm->selected_has_savestate = streamlined_check_savestate(
                        entry.label, temp_core);
            }
         }
         else if (entry.type == FILE_TYPE_PLAIN)
            strm->selected_is_file = true;
      }
   }

   /* Calculate visible items: screen height minus title area and button legend area */
   {
      int title_area = strm->margin_y + (int)(strm->font_size_title * 1.4f);
      int bottom_area = (int)(78.0f * strm->scale_factor);
      int search_kb_area = 0;
#if !TARGET_OS_TV
      if (strm->in_search_mode)
      {
         int _key_h = (int)(strm->font_size * 1.5f);
         int _key_gap = (int)(4 * strm->scale_factor);
         int _bar_h = (int)(strm->font_size * 1.8f);
         search_kb_area = _bar_h + _key_gap * 2
               + STREAMLINED_KB_NUM_ROWS * (_key_h + _key_gap);
      }
#endif
      max_visible = (video_height - title_area - bottom_area - search_kb_area) / item_height;
   }
   if (max_visible == 0)
      max_visible = 1;

   /* Get title - show game name for quick menu, "Settings" for submenu */
   title_buf[0] = '\0';
   if (strm->in_search_mode)
   {
      if (strm->in_favorites)
         strlcpy(title_buf, "Search: Favorites", sizeof(title_buf));
      else
      {
         const char *folder_name = path_basename(strm->options_folder_path);
         const char *clean_name = folder_name
               ? streamlined_strip_sort_prefix(folder_name) : "All";
         snprintf(title_buf, sizeof(title_buf), "Search: %s", clean_name);
      }
   }
   else if (strm->in_options_menu)
   {
      strlcpy(title_buf, "Game List Options", sizeof(title_buf));
   }
   else if (strm->selecting_core_for_folder)
   {
      strlcpy(title_buf, "Set Folder Core", sizeof(title_buf));
   }
   else if (strm->selecting_core_for_game)
   {
      strlcpy(title_buf, "Set Game Core", sizeof(title_buf));
   }
   else if (strm->selecting_core)
   {
      strlcpy(title_buf, "Select Core", sizeof(title_buf));
   }
   else if (strm->in_settings_submenu)
   {
      strlcpy(title_buf, "Advanced", sizeof(title_buf));
   }
   else if (strm->in_main_settings_submenu)
   {
      strlcpy(title_buf, "Settings", sizeof(title_buf));
   }
   else if (strm->in_game_switcher && strm->game_switcher_in_glo)
   {
      strlcpy(title_buf, "Game List Options", sizeof(title_buf));
   }
   else if (strm->in_game_switcher)
   {
      strlcpy(title_buf, "Game Switcher", sizeof(title_buf));
   }
   else if (strm->is_custom_main_menu && strm->in_favorites)
   {
      strlcpy(title_buf, "Favorites", sizeof(title_buf));
   }
   else if (strm->is_custom_main_menu && strm->in_folder)
   {
      /* Show folder/platform name as title */
      const char *folder_name = path_basename(strm->current_folder_path);
      if (!string_is_empty(folder_name))
      {
         /* Strip sort prefix (e.g., "1) Game Boy" -> "Game Boy") */
         const char *clean_name = streamlined_strip_sort_prefix(folder_name);
         strlcpy(title_buf, clean_name, sizeof(title_buf));
      }
   }
   else if (strm->is_quick_menu)
   {
      /* Custom quick menu - show game name */
      const char *content_path = path_get(RARCH_PATH_CONTENT);
      if (!string_is_empty(content_path))
      {
         /* Check if content lives in an M3U game folder —
          * if so, use the folder name as the game title */
         char parent_dir[PATH_MAX_LENGTH];
         char m3u_path[PATH_MAX_LENGTH];
         fill_pathname_parent_dir(parent_dir, content_path, sizeof(parent_dir));

         /* Remove trailing slash so path_basename returns the folder name */
         {
            size_t len = strlen(parent_dir);
            if (len > 0 && parent_dir[len - 1] == '/')
               parent_dir[len - 1] = '\0';
         }

         if (streamlined_check_m3u_folder(parent_dir, m3u_path, sizeof(m3u_path)))
            streamlined_get_display_name(parent_dir,
                  title_buf, sizeof(title_buf), true);
         else
            streamlined_get_display_name(content_path,
                  title_buf, sizeof(title_buf), false);
      }
      if (title_buf[0] == '\0')
         strlcpy(title_buf, "Quick Menu", sizeof(title_buf));
   }
   else
   {
      menu_entries_get_title(title_buf, sizeof(title_buf));

      /* Strip "Select File: " prefix (localized) from file browser titles */
      {
         const char *select_file = msg_hash_to_str(MENU_ENUM_LABEL_VALUE_SELECT_FILE);
         size_t prefix_len = strlen(select_file);

         /* Check for "Select File: " pattern (translated string + ": ") */
         if (strncmp(title_buf, select_file, prefix_len) == 0
               && title_buf[prefix_len] == ':'
               && title_buf[prefix_len + 1] == ' ')
         {
            memmove(title_buf, title_buf + prefix_len + 2,
                  strlen(title_buf + prefix_len + 2) + 1);
         }
         else
         {
            strlcpy(title_buf, "Main Menu", sizeof(title_buf));
         }
      }
   }

   /* Increment ticker for this frame */
   strm->ticker_idx++;

   /* Draw title with ticker-based scrolling for long titles */
   {
      int max_title_width = video_width - strm->margin_x * 2;
      int title_y = strm->margin_y + (int)(strm->font_size_title * 0.9f);
      char title_ticker[256];
      unsigned x_offset = 0;
      font_data_t *title_font = strm->font_title.font
            ? strm->font_title.font : strm->font.font;

      gfx_animation_ctx_ticker_smooth_t ticker;
      ticker.idx           = strm->ticker_idx;
      ticker.src_str       = title_buf;
      ticker.spacer        = NULL;
      ticker.dst_str       = title_ticker;
      ticker.dst_str_width = NULL;
      ticker.x_offset      = &x_offset;
      ticker.font          = title_font;
      ticker.dst_str_len   = sizeof(title_ticker);
      ticker.glyph_width   = (unsigned)strm->font_size_title;
      ticker.field_width   = (unsigned)max_title_width;
      ticker.font_scale    = 1.0f;
      ticker.type_enum     = TICKER_TYPE_BOUNCE;
      ticker.selected      = true;

      gfx_animation_ticker_smooth(&ticker);

      streamlined_draw_title(strm, p_disp, video_width, video_height,
            strm->margin_x + (int)x_offset, title_y,
            title_ticker, streamlined_color_text);
   }

   /* Search mode: poll keyboard buffer and re-filter on query change */
   if (strm->in_search_mode)
   {
#if TARGET_OS_TV
      if (!strm->search_keyboard_done && strm->search_kb_buffer_ptr)
         strlcpy(strm->search_query, strm->search_kb_buffer_ptr,
               sizeof(strm->search_query));
#endif
      if (!string_is_equal(strm->search_query, strm->search_prev_query))
      {
         strlcpy(strm->search_prev_query, strm->search_query,
               sizeof(strm->search_prev_query));
         streamlined_populate_search_results(strm);
         menu_st->selection_ptr = 0;
         selection = 0;
         list = MENU_LIST_GET_SELECTION(menu_list, 0);
         list_size = list ? list->size : 0;
      }
   }

   /* Calculate scroll */
   if (selection >= max_visible)
      start_idx = selection - max_visible + 1;
   else
      start_idx = 0;

   /* Draw menu entries */
   y = strm->margin_y + (int)(strm->font_size_title * 1.4f);

#if !TARGET_OS_TV
   /* Draw search keyboard and offset menu items below it */
   if (strm->in_search_mode)
      y += streamlined_render_search_keyboard(strm, p_disp, userdata,
            video_width, video_height);
#endif

   /* Reserve space on the right for thumbnail when one is visible */
   {
      int thumb_reserve = 0;
      if (strm->is_custom_main_menu
          && strm->rom_thumbnail.status == GFX_THUMBNAIL_STATUS_AVAILABLE)
      {
         float tw, th;
         int max_tw = (int)(video_width * 0.5f) - strm->margin_x;
         int avail_top = strm->margin_y + (int)(strm->font_size_title * 1.4f);
         int avail_bot = (int)(video_height - 78.0f * strm->scale_factor);
         int max_th = avail_bot - avail_top - strm->margin_y * 2;
         if (max_tw > 0 && max_th > 0)
         {
            gfx_thumbnail_get_draw_dimensions(
                  &strm->rom_thumbnail, max_tw, max_th, 1.0f, &tw, &th);
            thumb_reserve = (int)tw + strm->margin_x;
         }
      }

   for (i = 0; i < max_visible && (start_idx + i) < list_size; i++)
   {
      menu_entry_t entry;
      const char *entry_label;
      char display_label[256];
      char *ptr;
      bool is_selected = ((start_idx + i) == selection);

      /* Calculate consistent text position */
      int pill_height = (int)(strm->font_size * 1.5f);
      int pill_y = y + (item_height - pill_height) / 2;
      int text_y = pill_y + pill_height / 2 + (int)(strm->font_size * 0.30f);
      int content_width = video_width - strm->margin_x * 2 - thumb_reserve;

      MENU_ENTRY_INITIALIZE(entry);
      entry.flags |= MENU_ENTRY_FLAG_RICH_LABEL_ENABLED
                   | MENU_ENTRY_FLAG_VALUE_ENABLED;
      if (is_selected && show_sublabel)
         entry.flags |= MENU_ENTRY_FLAG_SUBLABEL_ENABLED;
      menu_entry_get(&entry, 0, (unsigned)(start_idx + i), NULL, true);
      if (is_selected && show_sublabel)
         strlcpy(selected_sublabel, entry.sublabel, sizeof(selected_sublabel));

      /* For custom menus, prefer path (our custom label) over rich_label (RA's label)
       * For custom main menu, use label (display name) since path contains full file path
       * For core selection, use path (display name) since label contains core path
       * For main settings submenu, use path (our custom label) */
      if (strm->is_quick_menu || strm->in_settings_submenu || strm->selecting_core
            || strm->selecting_core_for_folder || strm->selecting_core_for_game
            || strm->in_main_settings_submenu)
      {
         entry_label = entry.path;
         /* Core Options has empty path - use rich_label instead */
         if (string_is_empty(entry_label) && !string_is_empty(entry.rich_label))
            entry_label = entry.rich_label;
      }
      else if (strm->is_custom_main_menu)
      {
         if (!string_is_empty(entry.label))
            entry_label = entry.label;
         else
         {
            const char *raw_path = list->list[start_idx + i].path;
            entry_label = !string_is_empty(raw_path) ? raw_path : entry.path;
         }
      }
      else if (!string_is_empty(entry.rich_label))
         entry_label = entry.rich_label;
      else
         entry_label = entry.path;

      if (string_is_empty(entry_label))
      {
         y += item_height;
         continue;
      }

      /* Copy label for display */
      strlcpy(display_label, entry_label, sizeof(display_label));

      /* Process entry type - adds slash prefix for directories
       * Skip for top-level custom main menu (already handled in populate) */
      if (!(strm->is_custom_main_menu && !strm->in_folder && !strm->in_main_settings_submenu))
         streamlined_process_entry_type(entry.value, display_label, sizeof(display_label));

      /* Check if value should be displayed */
      bool show_value = !string_is_empty(entry.value)
                     && !string_is_equal(entry.value, "...")
                     && !streamlined_should_hide_value(entry.value);

      if (is_selected)
      {
         int pill_width;
         int text_width = streamlined_get_text_width(strm, display_label, FONT_NORMAL);
         int max_value_width = content_width * 45 / 100;
         int value_gap = (int)(16 * strm->scale_factor);
         int max_label_width = show_value
               ? (content_width - max_value_width - value_gap)
               : content_width;

         /*
          * Pill width calculation:
          * - With value: spans from label to value (full content width + padding)
          * - Without value: fits snugly around label text with symmetric padding
          */
         if (show_value)
            pill_width = content_width + strm->pill_padding * 2;
         else
            pill_width = (text_width > max_label_width ? max_label_width : text_width)
                  + strm->pill_padding * 2;

         streamlined_draw_rounded_pill(strm, p_disp, userdata,
               strm->margin_x - strm->pill_padding, pill_y,
               pill_width, pill_height,
               video_width, video_height, streamlined_color_selection);

         /* Draw label with ticker scrolling if too long */
         {
            char label_ticker[256];
            unsigned x_offset = 0;
            uint64_t item_idx;

            /* Reset ticker when selection changes so scrolling starts from left */
            if (selection != strm->item_ticker_selection)
            {
               strm->item_ticker_selection = selection;
               strm->item_ticker_start = strm->ticker_idx;
            }
            item_idx = strm->ticker_idx - strm->item_ticker_start;

            gfx_animation_ctx_ticker_smooth_t ticker;
            ticker.idx           = item_idx;
            ticker.src_str       = display_label;
            ticker.spacer        = NULL;
            ticker.dst_str       = label_ticker;
            ticker.dst_str_width = NULL;
            ticker.x_offset      = &x_offset;
            ticker.font          = strm->font.font;
            ticker.dst_str_len   = sizeof(label_ticker);
            ticker.glyph_width   = (unsigned)strm->font_size;
            ticker.field_width   = (unsigned)max_label_width;
            ticker.font_scale    = 1.0f;
            ticker.type_enum     = TICKER_TYPE_BOUNCE;
            ticker.selected      = true;

            gfx_animation_ticker_smooth(&ticker);

            streamlined_draw_text(strm, p_disp, video_width, video_height,
                  strm->margin_x + (int)x_offset, text_y,
                  label_ticker, streamlined_color_text_dark, false);
         }

         /* Value stays on the right, truncated to max width */
         if (show_value)
         {
            char truncated_value[256];
            int value_width;
            int value_right_edge = strm->margin_x + content_width;

            streamlined_truncate_text(strm, entry.value, truncated_value,
                  sizeof(truncated_value), max_value_width, FONT_NORMAL);
            value_width = streamlined_get_text_width(strm, truncated_value, FONT_NORMAL);

            streamlined_draw_text(strm, p_disp, video_width, video_height,
                  value_right_edge - value_width, text_y,
                  truncated_value, streamlined_color_text_dark, false);
         }
      }
      else
      {
         /* Non-selected: truncate long labels */
         int max_value_width = content_width * 45 / 100;
         int value_gap = (int)(16 * strm->scale_factor);
         int max_label_width = show_value
               ? (content_width - max_value_width - value_gap)
               : content_width;
         char truncated_label[256];

         streamlined_truncate_text(strm, display_label, truncated_label,
               sizeof(truncated_label), max_label_width, FONT_NORMAL);

         streamlined_draw_text(strm, p_disp, video_width, video_height,
               strm->margin_x, text_y,
               truncated_label, streamlined_color_text, false);

         if (show_value)
         {
            char truncated_value[256];
            int max_value_width = content_width * 45 / 100;
            int value_width;
            int value_right_edge = strm->margin_x + content_width;

            streamlined_truncate_text(strm, entry.value, truncated_value,
                  sizeof(truncated_value), max_value_width, FONT_NORMAL);
            value_width = streamlined_get_text_width(strm, truncated_value, FONT_NORMAL);

            streamlined_draw_text(strm, p_disp, video_width, video_height,
                  value_right_edge - value_width, text_y,
                  truncated_value, streamlined_color_text, false);
         }
      }

      y += item_height;
   }
   } /* end thumb_reserve block */

   /* Draw save slot selector if on Save/Load State entry */
   if (strm->show_slot_selector)
      streamlined_draw_slot_selector(strm, p_disp, userdata, video_width, video_height);

   /* Draw ROM/directory thumbnail if available in custom main menu */
   if (strm->is_custom_main_menu
       && strm->rom_thumbnail.status == GFX_THUMBNAIL_STATUS_AVAILABLE)
      streamlined_draw_rom_thumbnail(strm, p_disp, userdata, video_width, video_height);

   /* Footer - Back on left, OK/Play on right, optionally Resume before Play */
   {
      float scale            = strm->scale_factor;
      float footer_height    = 78.0f * scale;
      float footer_margin    = 40.0f * scale;
      float pill_h           = strm->font_size_small + 8.0f * scale;
      float pill_pad         = 10.0f * scale;
      float pill_text_gap    = 8.0f * scale;
      float btn_spacing      = 20.0f * scale;

      float footer_center_y  = (float)video_height - (footer_height / 2.0f);
      float pill_y           = footer_center_y - (pill_h / 2.0f);
      float text_y           = footer_center_y + (strm->font_size_small * 0.35f);

      int back_key_w, ok_key_w;
      int back_pill_w, ok_pill_w;
      const char *back_key   = "B";
      const char *ok_key     = "A";
      const char *back_str   = msg_hash_to_str(
            MENU_ENUM_LABEL_VALUE_BASIC_MENU_CONTROLS_BACK);
      const char *ok_str     = (strm->selected_is_file && !strm->is_quick_menu)
                               ? "Play"
                               : msg_hash_to_str(
                                    MENU_ENUM_LABEL_VALUE_BASIC_MENU_CONTROLS_OK);

      back_key_w  = font_driver_get_message_width(
            strm->font_small.font, back_key, strlen(back_key), 1.0f);
      ok_key_w    = font_driver_get_message_width(
            strm->font_small.font, ok_key, strlen(ok_key), 1.0f);
      back_pill_w = back_key_w + (int)(pill_pad * 2.0f);
      ok_pill_w   = ok_key_w + (int)(pill_pad * 2.0f);

      /* Left side: [margin] [B pill] [gap] Back */
      streamlined_draw_rounded_pill(strm, p_disp, userdata,
            (int)footer_margin, (int)pill_y, back_pill_w, (int)pill_h,
            video_width, video_height, streamlined_color_selection);
      gfx_display_draw_text(strm->font_small.font,
            back_key,
            (int)(footer_margin + pill_pad),
            (int)text_y,
            video_width, video_height,
            streamlined_color_text_dark,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
      gfx_display_draw_text(strm->font_small.font,
            back_str,
            (int)(footer_margin + (float)back_pill_w + pill_text_gap),
            (int)text_y,
            video_width, video_height,
            streamlined_color_text,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);

      /* Right side: optionally [X pill] [gap] Resume [spacing] then [A pill] [gap] Play/OK [margin] */
      {
         int ok_label_w = font_driver_get_message_width(
               strm->font_small.font, ok_str, strlen(ok_str), 1.0f);
         float right_x  = (float)video_width - footer_margin;

         /* Position A pill + label from right edge */
         float ok_pill_x = right_x - (float)ok_label_w - pill_text_gap - (float)ok_pill_w;

         streamlined_draw_rounded_pill(strm, p_disp, userdata,
               (int)ok_pill_x, (int)pill_y, ok_pill_w, (int)pill_h,
               video_width, video_height, streamlined_color_selection);
         gfx_display_draw_text(strm->font_small.font,
               ok_key,
               (int)(ok_pill_x + pill_pad),
               (int)text_y,
               video_width, video_height,
               streamlined_color_text_dark,
               TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
         gfx_display_draw_text(strm->font_small.font,
               ok_str,
               (int)(ok_pill_x + (float)ok_pill_w + pill_text_gap),
               (int)text_y,
               video_width, video_height,
               streamlined_color_text,
               TEXT_ALIGN_LEFT, 1.0f, false, 0, false);

         /* Draw [X] Resume pill to the left of [A] Play when save state exists */
         if (strm->selected_has_savestate && !strm->is_quick_menu)
         {
            const char *resume_key = "X";
            const char *resume_str = "Resume";
            int resume_key_w   = font_driver_get_message_width(
                  strm->font_small.font, resume_key, strlen(resume_key), 1.0f);
            int resume_pill_w  = resume_key_w + (int)(pill_pad * 2.0f);
            int resume_label_w = font_driver_get_message_width(
                  strm->font_small.font, resume_str, strlen(resume_str), 1.0f);
            float resume_pill_x = ok_pill_x - btn_spacing
                  - (float)resume_label_w - pill_text_gap - (float)resume_pill_w;

            streamlined_draw_rounded_pill(strm, p_disp, userdata,
                  (int)resume_pill_x, (int)pill_y, resume_pill_w, (int)pill_h,
                  video_width, video_height, streamlined_color_selection);
            gfx_display_draw_text(strm->font_small.font,
                  resume_key,
                  (int)(resume_pill_x + pill_pad),
                  (int)text_y,
                  video_width, video_height,
                  streamlined_color_text_dark,
                  TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
            gfx_display_draw_text(strm->font_small.font,
                  resume_str,
                  (int)(resume_pill_x + (float)resume_pill_w + pill_text_gap),
                  (int)text_y,
                  video_width, video_height,
                  streamlined_color_text,
                  TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
         }

         /* Draw sublabel text between Back and rightmost button group */
         if (show_sublabel && selected_sublabel[0] != '\0')
         {
            int back_str_w = font_driver_get_message_width(
                  strm->font_small.font, back_str, strlen(back_str), 1.0f);
            float sublabel_left = footer_margin + (float)back_pill_w
                  + pill_text_gap + (float)back_str_w + btn_spacing;
            float sublabel_right = strm->selected_has_savestate
                  ? (ok_pill_x - btn_spacing
                     - (float)font_driver_get_message_width(
                        strm->font_small.font, "Resume", 6, 1.0f)
                     - pill_text_gap
                     - (float)(font_driver_get_message_width(
                        strm->font_small.font, "X", 1, 1.0f)
                        + (int)(pill_pad * 2.0f))
                     - btn_spacing)
                  : ok_pill_x - btn_spacing;
            int sublabel_avail = (int)(sublabel_right - sublabel_left);

            if (sublabel_avail > 0)
            {
               char sublabel_buf[512];
               int sublabel_w;
               int sublabel_x;
               float sublabel_text_y = text_y;

               sublabel_w = streamlined_get_text_width(strm,
                     selected_sublabel, FONT_SMALL);

               if (sublabel_w > sublabel_avail)
               {
                  int tiny_w = streamlined_get_text_width(strm,
                        selected_sublabel, FONT_TINY);

                  if (tiny_w > sublabel_avail)
                  {
                     /* Word-wrap into two lines */
                     char line1[512];
                     char line2[512];
                     font_data_t *tiny_font = strm->font_tiny.font
                           ? strm->font_tiny.font : strm->font_small.font;
                     size_t len         = strlen(selected_sublabel);
                     size_t last_space  = 0;
                     size_t break_pos   = len;
                     size_t i;
                     float line_spacing = strm->font_size_tiny * 1.2f;
                     float baseline_off = strm->font_size_tiny * 0.35f;
                     float line1_y, line2_y;
                     int line1_w, line2_w, line1_x, line2_x;

                     /* Find word-wrap break point */
                     for (i = 1; i <= len; i++)
                     {
                        if (selected_sublabel[i - 1] == ' ')
                           last_space = i - 1;
                        if (font_driver_get_message_width(tiny_font,
                              selected_sublabel, i, 1.0f) > sublabel_avail)
                        {
                           break_pos = (last_space > 0) ? last_space : i;
                           break;
                        }
                     }

                     /* Line 1: text up to break point */
                     strlcpy(line1, selected_sublabel, sizeof(line1));
                     line1[break_pos] = '\0';

                     /* Line 2: remainder, skip space at break */
                     if (break_pos < len
                           && selected_sublabel[break_pos] == ' ')
                        strlcpy(line2, selected_sublabel + break_pos + 1,
                              sizeof(line2));
                     else
                        strlcpy(line2, selected_sublabel + break_pos,
                              sizeof(line2));

                     /* Truncate line 2 if too wide */
                     streamlined_truncate_text(strm, line2, sublabel_buf,
                           sizeof(sublabel_buf), sublabel_avail, FONT_TINY);
                     strlcpy(line2, sublabel_buf, sizeof(line2));

                     /* Vertical positioning: center both lines */
                     line1_y = footer_center_y
                           - (line_spacing * 0.5f) + baseline_off;
                     line2_y = footer_center_y
                           + (line_spacing * 0.5f) + baseline_off;

                     /* Center each line horizontally */
                     line1_w = streamlined_get_text_width(strm,
                           line1, FONT_TINY);
                     line2_w = streamlined_get_text_width(strm,
                           line2, FONT_TINY);
                     line1_x = (int)(sublabel_left
                           + ((float)sublabel_avail - (float)line1_w)
                           / 2.0f);
                     line2_x = (int)(sublabel_left
                           + ((float)sublabel_avail - (float)line2_w)
                           / 2.0f);

                     /* Draw both lines */
                     streamlined_draw_text_tiny(strm, p_disp,
                           video_width, video_height,
                           line1_x, (int)line1_y,
                           line1, streamlined_color_text_muted);
                     streamlined_draw_text_tiny(strm, p_disp,
                           video_width, video_height,
                           line2_x, (int)line2_y,
                           line2, streamlined_color_text_muted);
                  }
                  else
                  {
                     /* Fits on single FONT_TINY line */
                     strlcpy(sublabel_buf, selected_sublabel,
                           sizeof(sublabel_buf));
                     sublabel_w   = tiny_w;
                     sublabel_text_y = footer_center_y
                           + (strm->font_size_tiny * 0.35f);
                     sublabel_x = (int)(sublabel_left
                           + ((float)sublabel_avail - (float)sublabel_w)
                           / 2.0f);
                     streamlined_draw_text_tiny(strm, p_disp,
                           video_width, video_height,
                           sublabel_x, (int)sublabel_text_y,
                           sublabel_buf, streamlined_color_text_muted);
                  }
               }
               else
               {
                  strlcpy(sublabel_buf, selected_sublabel,
                        sizeof(sublabel_buf));
                  sublabel_x = (int)(sublabel_left
                        + ((float)sublabel_avail - (float)sublabel_w)
                        / 2.0f);
                  gfx_display_draw_text(strm->font_small.font,
                        sublabel_buf,
                        sublabel_x, (int)sublabel_text_y,
                        video_width, video_height,
                        streamlined_color_text_muted,
                        TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
               }
            }
         }
      }
   }
}

/* ======================================================================
 * QUICK MENU CUSTOMIZATION
 * ====================================================================== */

static void streamlined_populate_menu_items(const streamlined_quick_item_t *items)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   size_t i;

   if (!menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   /* Clear and repopulate with custom items */
   menu_entries_clear(list);

   for (i = 0; items[i].label != NULL; i++)
   {
      const streamlined_quick_item_t *item = &items[i];
      const char *action_label = msg_hash_to_str(item->action);

      /* Use proper internal label for callbacks, but set alt for display */
      menu_entries_append(list,
            item->label,
            action_label ? action_label : item->label,
            item->action,
            MENU_SETTING_ACTION,
            0, 0, NULL);
   }
}

/* Check if content was launched from command line */
static bool streamlined_is_launched_from_cli(void)
{
   global_t *global = global_get_ptr();
   if (!global)
      return false;
   return (global->flags & GLOB_FLG_LAUNCHED_FROM_CLI) != 0;
}

/* Check if disc control is available for current core */
static bool streamlined_is_disc_control_available(void)
{
   rarch_system_info_t *sys_info = &runloop_state_get_ptr()->system;
   if (!sys_info)
      return false;
   return disk_control_enabled(&sys_info->disk_control);
}

/* Populate quick menu, with dynamic Exit/Quit based on CLI launch */
static void streamlined_populate_quick_menu(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   const streamlined_quick_item_t *item;
   bool from_cli = streamlined_is_launched_from_cli();

   if (!menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   /* Clear and repopulate with custom items */
   menu_entries_clear(list);

   for (item = streamlined_quick_menu_items; item->label != NULL || item->action == STREAMLINED_EXIT_MARKER; item++)
   {
      const char *label;
      const char *action_label;
      enum msg_hash_enums action;

      /* Handle dynamic Quit entry - quits if CLI, exits to menu if not */
      if (item->action == STREAMLINED_EXIT_MARKER)
      {
         settings_t *settings = config_get_ptr();
         bool auto_save = settings && settings->bools.savestate_auto_save;

         label = auto_save ? "Save and Quit" : "Quit";
         action = from_cli ? MENU_ENUM_LABEL_QUIT_RETROARCH : MENU_ENUM_LABEL_CLOSE_CONTENT;
         action_label = msg_hash_to_str(action);
      }
      else
      {
         label = item->label;
         action = item->action;
         action_label = msg_hash_to_str(action);
      }

      menu_entries_append(list,
            label,
            action_label ? action_label : label,
            action,
            MENU_SETTING_ACTION,
            0, 0, NULL);
   }
}

/* Populate settings submenu, conditionally including Disc Control */
static void streamlined_populate_settings_submenu(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   const streamlined_quick_item_t *item;
   bool show_disc_control = streamlined_is_disc_control_available();

   if (!menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   /* Clear and repopulate with custom items */
   menu_entries_clear(list);

   for (item = streamlined_settings_menu_items; item->label != NULL; item++)
   {
      const char *action_label;

      /* Skip Disc Control if not available */
      if (item->action == MENU_ENUM_LABEL_DISK_OPTIONS && !show_disc_control)
         continue;

      /* Skip Achievements if not compiled in */
#ifndef HAVE_CHEEVOS
      if (item->action == MENU_ENUM_LABEL_ACHIEVEMENT_LIST)
         continue;
#endif

      /* Skip Cheats if not compiled in */
#ifndef HAVE_CHEATS
      if (item->action == MENU_ENUM_LABEL_CORE_CHEAT_OPTIONS)
         continue;
#endif

      /* Skip Rewind if not compiled in */
#ifndef HAVE_REWIND
      if (item->action == MENU_ENUM_LABEL_REWIND_SETTINGS)
         continue;
#endif

      /* Core Options needs empty path and special type to work properly */
      if (item->action == MENU_ENUM_LABEL_CORE_OPTIONS)
      {
         menu_entries_append(list,
               "",  /* Empty path required for core options */
               msg_hash_to_str(MENU_ENUM_LABEL_CORE_OPTIONS),
               MENU_ENUM_LABEL_CORE_OPTIONS,
               MENU_SETTING_ACTION_CORE_OPTIONS,
               0, 0, NULL);
         continue;
      }

      action_label = msg_hash_to_str(item->action);

      menu_entries_append(list,
            item->label,
            action_label ? action_label : item->label,
            item->action,
            MENU_SETTING_ACTION,
            0, 0, NULL);
   }
}

/* Populate main menu settings submenu (Settings categories + main menu items) */
static void streamlined_populate_main_settings_submenu(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   const streamlined_quick_item_t *item;

   if (!menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   /* Clear and repopulate with custom items */
   menu_entries_clear(list);

   for (item = streamlined_main_settings_items; item->label != NULL; item++)
   {
      const char *action_label;

      /* Skip Achievements if not compiled in */
#ifndef HAVE_CHEEVOS
      if (item->action == MENU_ENUM_LABEL_RETRO_ACHIEVEMENTS_SETTINGS)
         continue;
#endif

      /* Skip Online Updater if not compiled in */
#ifndef HAVE_ONLINE_UPDATER
      if (item->action == MENU_ENUM_LABEL_ONLINE_UPDATER)
         continue;
#endif

      /* Skip Network if not compiled in */
#ifndef HAVE_NETWORKING
      if (item->action == MENU_ENUM_LABEL_NETWORK_SETTINGS)
         continue;
#endif

      /* Skip Cheats if not compiled in */
#ifndef HAVE_CHEATS
      if (item->action == MENU_ENUM_LABEL_CORE_CHEAT_OPTIONS)
         continue;
#endif

      /* Skip Rewind if not compiled in */
#ifndef HAVE_REWIND
      if (item->action == MENU_ENUM_LABEL_REWIND_SETTINGS)
         continue;
#endif

      /* Skip Core Options if no core is loaded */
      if (item->action == MENU_ENUM_LABEL_CORE_OPTIONS)
      {
         if (retroarch_ctl(RARCH_CTL_IS_DUMMY_CORE, NULL))
            continue;
         /* Core Options needs empty path and special type to work properly */
         menu_entries_append(list,
               "",  /* Empty path required for core options */
               msg_hash_to_str(MENU_ENUM_LABEL_CORE_OPTIONS),
               MENU_ENUM_LABEL_CORE_OPTIONS,
               MENU_SETTING_ACTION_CORE_OPTIONS,
               0, 0, NULL);
         continue;
      }

      action_label = msg_hash_to_str(item->action);

      menu_entries_append(list,
            item->label,
            action_label ? action_label : item->label,
            item->action,
            MENU_SETTING_ACTION,
            0, 0, NULL);
   }
}

/*
 * Build the base save state path for a ROM, replicating RetroArch's
 * directory logic (savestates_in_content_dir, sort_savestates_by_content,
 * sort_savestates). Result is like: {dir}/{rom_basename}.state
 */
static void streamlined_build_savestate_base_path(
      const char *rom_path, const char *core_path,
      char *out, size_t out_size)
{
   settings_t *settings          = config_get_ptr();
   const char *savestate_dir     = dir_get_ptr(RARCH_DIR_SAVESTATE);
   bool savestates_in_content    = settings->bools.savestates_in_content_dir;
   bool sort_by_content          = settings->bools.sort_savestates_by_content_enable;
   bool sort_by_core             = settings->bools.sort_savestates_enable;
   char dir[DIR_MAX_LENGTH];

   dir[0] = '\0';

   /* Step 1: Base directory */
   if (string_is_empty(savestate_dir) || savestates_in_content)
      fill_pathname_basedir(dir, rom_path, sizeof(dir));
   else
      strlcpy(dir, savestate_dir, sizeof(dir));

   /* Step 2: Append content parent dir name if sort_by_content */
   if (sort_by_content)
   {
      char content_dir_name[DIR_MAX_LENGTH];
      content_dir_name[0] = '\0';
      fill_pathname_parent_dir_name(content_dir_name, rom_path,
            sizeof(content_dir_name));
      if (!string_is_empty(content_dir_name))
      {
         char tmp[DIR_MAX_LENGTH];
         fill_pathname_join_special(tmp, dir, content_dir_name, sizeof(tmp));
         strlcpy(dir, tmp, sizeof(dir));
      }
   }

   /* Step 3: Append core library_name if sort_by_core */
   if (sort_by_core && !string_is_empty(core_path))
   {
      core_info_t *info = NULL;
      if (core_info_find(core_path, &info) && info && info->core_name)
      {
         char tmp[DIR_MAX_LENGTH];
         fill_pathname_join(tmp, dir, info->core_name, sizeof(tmp));
         strlcpy(dir, tmp, sizeof(dir));
      }
   }

   /* Step 4: Build final path: dir/rom_basename.state
    * Use fill_pathname to strip the ROM extension first,
    * matching standard RetroArch savestate naming. */
   {
      char tmp[PATH_MAX_LENGTH];
      fill_pathname_join_special(tmp, dir, path_basename(rom_path), sizeof(tmp));
      fill_pathname(out, tmp, FILE_PATH_STATE_EXTENSION, out_size);
   }
}

/*
 * Build an autosave path by appending a suffix (e.g. ".auto" or ".auto.png")
 * to the savestate base path.
 */
static void streamlined_build_autosave_path(
      const char *rom_path, const char *core_path,
      const char *suffix,
      char *out, size_t out_size)
{
   char base_path[PATH_MAX_LENGTH];
   base_path[0] = '\0';
   streamlined_build_savestate_base_path(rom_path, core_path,
         base_path, sizeof(base_path));
   if (string_is_empty(base_path))
   {
      out[0] = '\0';
      return;
   }
   snprintf(out, out_size, "%s%s", base_path, suffix);
}

/*
 * Find an existing autosave file, with M3U fallback.
 * Tries the given path first, then for M3U files tries the resolved
 * first disc file (since non-M3U-supporting cores create autosaves
 * from the disc path).
 */
static bool streamlined_find_autosave_path(
      const char *rom_path, const char *core_path,
      const char *suffix,
      char *out, size_t out_size)
{
   /* Try with the given path */
   streamlined_build_autosave_path(rom_path, core_path, suffix,
         out, out_size);
   if (!string_is_empty(out) && path_is_valid(out))
      return true;

   /* For M3U: also try with the resolved first disc file */
   if (m3u_file_is_m3u(rom_path))
   {
      char resolved[PATH_MAX_LENGTH];
      if (streamlined_resolve_m3u_content(rom_path,
            resolved, sizeof(resolved)))
      {
         streamlined_build_autosave_path(resolved, core_path, suffix,
               out, out_size);
         if (!string_is_empty(out) && path_is_valid(out))
            return true;
      }
   }

   out[0] = '\0';
   return false;
}

/*
 * Check if an auto-save state exists for the given ROM.
 * Returns true if an auto-save state file exists.
 */
static bool streamlined_check_savestate(
      const char *rom_path, const char *core_path)
{
   char path[PATH_MAX_LENGTH];
   return streamlined_find_autosave_path(rom_path, core_path,
         ".auto", path, sizeof(path));
}

/*
 * Delete the auto-save state file for a given ROM.
 * Constructs the path using streamlined_build_savestate_base_path()
 * and appends ".auto", then deletes the file.
 */
static void streamlined_delete_autosave_file(
      const char *rom_path, const char *core_path)
{
   char auto_path[PATH_MAX_LENGTH];

   /* Delete autosave for the given path */
   streamlined_build_autosave_path(rom_path, core_path, ".auto",
         auto_path, sizeof(auto_path));
   if (!string_is_empty(auto_path))
      filestream_delete(auto_path);

   /* For M3U: also delete the autosave for the resolved first disc file */
   if (m3u_file_is_m3u(rom_path))
   {
      char resolved[PATH_MAX_LENGTH];
      if (streamlined_resolve_m3u_content(rom_path,
            resolved, sizeof(resolved)))
      {
         streamlined_build_autosave_path(resolved, core_path, ".auto",
               auto_path, sizeof(auto_path));
         if (!string_is_empty(auto_path))
            filestream_delete(auto_path);
      }
   }
}

/*
 * Delete game files from disk.
 * For M3U multi-disc games, deletes the entire subfolder.
 * For single-file games, deletes just the file.
 */
static void streamlined_delete_game_files(const char *game_path)
{
   char m3u_path[PATH_MAX_LENGTH];

   if (string_is_empty(game_path))
      return;

   if (streamlined_find_m3u_for_content(game_path,
         m3u_path, sizeof(m3u_path)))
   {
      /* M3U game: delete entire subfolder */
      char parent_dir[PATH_MAX_LENGTH];
      fill_pathname_basedir(parent_dir, m3u_path, sizeof(parent_dir));
      streamlined_strip_trailing_slash(parent_dir);

      {
         struct string_list *file_list = dir_list_new(
               parent_dir, NULL, false, true, true, true);
         if (file_list)
         {
            size_t j;
            for (j = 0; j < file_list->size; j++)
               filestream_delete(file_list->elems[j].data);
            string_list_free(file_list);
         }
      }
      /* Remove the now-empty directory */
      filestream_delete(parent_dir);
   }
   else
   {
      /* Single file game */
      filestream_delete(game_path);
   }
}

/*
 * Remove a game from history, favorites, and all .lpl playlist files.
 */
static void streamlined_remove_from_all_playlists(const char *game_path)
{
   settings_t *settings;

   if (string_is_empty(game_path))
      return;

   /* Remove from history */
   if (g_defaults.content_history)
   {
      playlist_delete_by_path(g_defaults.content_history, game_path);
      playlist_write_file(g_defaults.content_history);
   }

   /* Remove from favorites */
   if (g_defaults.content_favorites)
   {
      playlist_delete_by_path(g_defaults.content_favorites, game_path);
      playlist_write_file(g_defaults.content_favorites);
   }

   /* Scan playlist directory for .lpl files */
   settings = config_get_ptr();
   if (settings && !string_is_empty(settings->paths.directory_playlist))
   {
      struct string_list *lpl_list = dir_list_new(
            settings->paths.directory_playlist,
            "lpl", false, false, false, false);
      if (lpl_list)
      {
         size_t j;
         for (j = 0; j < lpl_list->size; j++)
         {
            playlist_config_t pl_config;
            playlist_t *pl;
            memset(&pl_config, 0, sizeof(pl_config));
            pl_config.capacity = COLLECTION_SIZE;
            strlcpy(pl_config.path, lpl_list->elems[j].data,
                  sizeof(pl_config.path));
            pl = playlist_init(&pl_config);
            if (pl)
            {
               if (playlist_entry_exists(pl, game_path))
               {
                  playlist_delete_by_path(pl, game_path);
                  playlist_write_file(pl);
               }
               playlist_free(pl);
            }
         }
         string_list_free(lpl_list);
      }
   }

   /* For M3U games: also remove M3U path and first disc path */
   {
      char m3u_path[PATH_MAX_LENGTH];
      if (streamlined_find_m3u_for_content(game_path,
            m3u_path, sizeof(m3u_path)))
      {
         char resolved[PATH_MAX_LENGTH];
         /* Remove M3U path if different from game_path */
         if (!string_is_equal(game_path, m3u_path))
         {
            if (g_defaults.content_history)
            {
               playlist_delete_by_path(g_defaults.content_history, m3u_path);
               playlist_write_file(g_defaults.content_history);
            }
            if (g_defaults.content_favorites)
            {
               playlist_delete_by_path(g_defaults.content_favorites, m3u_path);
               playlist_write_file(g_defaults.content_favorites);
            }
         }
         /* Remove first disc path if different from game_path */
         if (streamlined_resolve_m3u_content(m3u_path,
               resolved, sizeof(resolved))
               && !string_is_equal(game_path, resolved))
         {
            if (g_defaults.content_history)
            {
               playlist_delete_by_path(g_defaults.content_history, resolved);
               playlist_write_file(g_defaults.content_history);
            }
            if (g_defaults.content_favorites)
            {
               playlist_delete_by_path(g_defaults.content_favorites, resolved);
               playlist_write_file(g_defaults.content_favorites);
            }
         }
      }
   }
}

/*
 * Populate the Game List Options menu.
 * Entries are conditional on whether the selected game has a save state.
 */
static void streamlined_populate_options_menu(streamlined_t *strm, bool in_favorites, bool in_game_switcher)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   char label[256];
   const char *display_name;

   if (!menu_st || !strm)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   menu_entries_clear(list);

   if (strm->options_game_has_savestate)
   {
      menu_entries_append(list,
            "Reset Game", "", STREAMLINED_OPTIONS_RESET_GAME,
            MENU_SETTING_ACTION, 0, 0, NULL);
   }

   /* Set Folder Core (not available for favorites or game switcher) */
   if (!in_favorites && !in_game_switcher)
   {
      display_name = streamlined_get_core_display_name(strm->options_core_path);
      snprintf(label, sizeof(label), "Set Folder Core: %s", display_name);
      menu_entries_append(list,
            label, "", STREAMLINED_OPTIONS_SET_FOLDER_CORE,
            MENU_SETTING_ACTION, 0, 0, NULL);
   }

   /* Set Game Core */
   if (!string_is_empty(strm->options_game_core_path))
      display_name = streamlined_get_core_display_name(strm->options_game_core_path);
   else
      display_name = streamlined_get_core_display_name(strm->options_core_path);
   snprintf(label, sizeof(label), "Set Game Core: %s", display_name);
   menu_entries_append(list,
         label, "", STREAMLINED_OPTIONS_SET_GAME_CORE,
         MENU_SETTING_ACTION, 0, 0, NULL);

   /* Search and Random Game (not available in game switcher) */
   if (!in_game_switcher)
   {
      menu_entries_append(list,
            "Search", "", STREAMLINED_OPTIONS_SEARCH,
            MENU_SETTING_ACTION, 0, 0, NULL);

      menu_entries_append(list,
            "Random Game", "", STREAMLINED_OPTIONS_RANDOM_GAME,
            MENU_SETTING_ACTION, 0, 0, NULL);
   }

   if (strm->options_game_has_savestate)
   {
      menu_entries_append(list,
            "Delete Autosave", "", STREAMLINED_OPTIONS_DELETE_SAVE,
            MENU_SETTING_ACTION, 0, 0, NULL);
   }

   /* Favorites: always show Remove; normal: check Add/Remove */
   if (in_favorites)
   {
      menu_entries_append(list,
            "Remove from Favorites", "", STREAMLINED_OPTIONS_REMOVE_FAVORITE,
            MENU_SETTING_ACTION, 0, 0, NULL);
   }
   else
   {
      playlist_t *favorites = g_defaults.content_favorites;
      bool is_favorite = favorites && playlist_entry_exists(favorites, strm->options_game_path);
      if (is_favorite)
         menu_entries_append(list,
               "Remove from Favorites", "", STREAMLINED_OPTIONS_REMOVE_FAVORITE,
               MENU_SETTING_ACTION, 0, 0, NULL);
      else
         menu_entries_append(list,
               "Add to Favorites", "", STREAMLINED_OPTIONS_ADD_FAVORITE,
               MENU_SETTING_ACTION, 0, 0, NULL);
   }

   /* Remove from Game Switcher (only in game switcher context) */
   if (in_game_switcher)
   {
      menu_entries_append(list,
            "Remove from Game Switcher", "", STREAMLINED_OPTIONS_REMOVE_FROM_SWITCHER,
            MENU_SETTING_ACTION, 0, 0, NULL);
   }

   /* Delete Game (last item — hidden when game is currently running) */
   {
      bool hide_delete = false;
      if (in_game_switcher && strm->game_switcher_index == 0)
      {
         const char *content_path = path_get(RARCH_PATH_CONTENT);
         if (!string_is_empty(content_path))
            hide_delete = true;
      }
      if (!hide_delete)
         menu_entries_append(list,
               "Delete Game", "", STREAMLINED_OPTIONS_DELETE_GAME,
               MENU_SETTING_ACTION, 0, 0, NULL);
   }
}

/*
 * Fuzzy string match: normalize both strings (lowercase, alphanumeric only)
 * and check if the normalized query is a substring of the normalized name.
 * Example: "super mario" -> "supermario" matches "Super Mario Bros. 3" -> "supermariobros3"
 */
static bool streamlined_fuzzy_match(const char *query, const char *name)
{
   char q_norm[256], n_norm[256];
   size_t qi = 0, ni = 0, i;

   if (string_is_empty(query))
      return true;
   if (string_is_empty(name))
      return false;

   for (i = 0; query[i] && qi < sizeof(q_norm) - 1; i++)
   {
      if (isalnum((unsigned char)query[i]))
         q_norm[qi++] = tolower((unsigned char)query[i]);
   }
   q_norm[qi] = '\0';

   for (i = 0; name[i] && ni < sizeof(n_norm) - 1; i++)
   {
      if (isalnum((unsigned char)name[i]))
         n_norm[ni++] = tolower((unsigned char)name[i]);
   }
   n_norm[ni] = '\0';

   return strstr(n_norm, q_norm) != NULL;
}

/*
 * Populate the menu list with search results filtered by the current query.
 * Iterates the full directory listing (search_all_entries) and adds
 * matching entries to the menu list.
 */
static void streamlined_populate_search_results(streamlined_t *strm)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   unsigned i;

   if (!menu_st || !strm || !strm->search_all_entries)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   menu_entries_clear(list);

   for (i = 0; i < strm->search_all_entries->size; i++)
   {
      const char *path = strm->search_all_entries->elems[i].data;
      unsigned attr = strm->search_all_entries->elems[i].attr.i;
      const char *name = path_basename(path);
      char display_name[256];

      if (!name || name[0] == '.')
         continue;

      if (attr == RARCH_DIRECTORY)
      {
         char m3u_path[PATH_MAX_LENGTH];
         if (!streamlined_check_m3u_folder(path, m3u_path, sizeof(m3u_path)))
            continue;

         streamlined_get_display_name(path,
               display_name, sizeof(display_name), true);
         if (!streamlined_fuzzy_match(strm->search_query, display_name))
            continue;

         menu_entries_append(list,
               display_name, m3u_path,
               MSG_UNKNOWN, FILE_TYPE_PLAIN,
               0, 0, NULL);
      }
      else
      {
         streamlined_get_display_name(path,
               display_name, sizeof(display_name), false);

         if (!streamlined_fuzzy_match(strm->search_query, display_name))
            continue;

         menu_entries_append(list,
               display_name, path,
               MSG_UNKNOWN, FILE_TYPE_PLAIN,
               0, 0, NULL);
      }
   }

   /* Re-sort by normalized display names when enabled */
   {
      settings_t *settings = config_get_ptr();
      if (settings->bools.menu_streamlined_normalize_rom_names
            && list->size > 1)
         qsort(list->list, list->size,
               sizeof(struct item_file), streamlined_entry_cmp);
   }

   if (list->size == 0 && !string_is_empty(strm->search_query))
   {
      menu_entries_append(list,
            "No results", "",
            MSG_UNKNOWN, FILE_TYPE_NONE,
            0, 0, NULL);
   }
}

#if TARGET_OS_TV
static void streamlined_search_keyboard_cb(void *userdata, const char *line)
{
   streamlined_t *strm = (streamlined_t *)userdata;
   if (!strm)
      return;
   if (line)
      strlcpy(strm->search_query, line, sizeof(strm->search_query));
   strm->search_keyboard_done = true;
}

static void streamlined_delete_confirm_cb(void *userdata, bool confirmed)
{
   streamlined_t *strm = (streamlined_t *)userdata;
   if (!strm)
      return;

   strm->in_delete_confirm = false;

   if (confirmed)
   {
      if (strm->delete_is_game)
      {
         streamlined_delete_game_files(strm->options_game_path);
         streamlined_remove_from_all_playlists(strm->options_game_path);
         strlcpy(strm->delete_done_label, "Game Deleted",
               sizeof(strm->delete_done_label));
         strm->delete_was_game = true;
      }
      else
      {
         streamlined_delete_autosave_file(
               strm->options_game_path, streamlined_effective_core(strm));
         strlcpy(strm->delete_done_label, "Deleted Autosave",
               sizeof(strm->delete_done_label));
         strm->delete_was_game = false;
      }
      strm->in_options_menu = false;
      strm->delete_done = true;
      strm->delete_done_start = strm->ticker_idx;
   }
   else
   {
      /* Cancelled — return to options menu */
      strm->in_options_menu = true;
      strm->cancel_ignore_frames = 3;
      streamlined_populate_options_menu(strm, strm->in_favorites,
            strm->in_game_switcher);
      streamlined_select_options_entry(
            strm->delete_is_game ? STREAMLINED_OPTIONS_DELETE_GAME
                                 : STREAMLINED_OPTIONS_DELETE_SAVE);
   }
}
#endif

/*
 * Clean up search mode state. If clear_query is false, the search query
 * persists so re-entering search remembers what was previously typed.
 */
static void streamlined_cleanup_search(streamlined_t *strm, bool clear_query)
{
   if (!strm)
      return;
   if (strm->search_all_entries)
   {
      string_list_free(strm->search_all_entries);
      strm->search_all_entries = NULL;
   }
#if TARGET_OS_TV
   if (strm->search_kb_buffer_ptr)
   {
      free(strm->search_kb_buffer_ptr);
      strm->search_kb_buffer_ptr = NULL;
   }
#endif
   if (clear_query)
   {
      strm->search_query[0] = '\0';
      strm->search_query_len = 0;
   }
   strm->search_prev_query[0] = '\0';
   strm->in_search_mode = false;
}

/*
 * Try to find a core by name from the core info list.
 * Matches against core_name, display_name, or filename (without _libretro suffix).
 * Returns true if found and writes path to core_path_out.
 */
static bool streamlined_find_core_by_name(const char *name, char *core_path_out, size_t core_path_size)
{
   core_info_list_t *core_info_list = NULL;
   size_t i;

   if (string_is_empty(name))
      return false;

   /* If it's already a full path and exists, use it directly */
   if (path_is_absolute(name) && path_is_valid(name))
   {
      strlcpy(core_path_out, name, core_path_size);
      return true;
   }

   core_info_get_list(&core_info_list);
   if (!core_info_list)
      return false;

   /* Search for a core matching the given name */
   for (i = 0; i < core_info_list->count; i++)
   {
      const core_info_t *info = &core_info_list->list[i];

      if (!info || string_is_empty(info->path))
         continue;

      /* Check if core_name matches (case insensitive) */
      if (info->core_name && strcasecmp(name, info->core_name) == 0)
      {
         strlcpy(core_path_out, info->path, core_path_size);
         return true;
      }

      /* Also try matching against display_name */
      if (info->display_name && strcasecmp(name, info->display_name) == 0)
      {
         strlcpy(core_path_out, info->path, core_path_size);
         return true;
      }

      /* Also try matching against the filename without extension */
      {
         char basename[256];
         const char *filename = path_basename(info->path);
         if (filename)
         {
            strlcpy(basename, filename, sizeof(basename));
            path_remove_extension(basename);
            /* Remove _libretro suffix if present */
            {
               char *suffix = strstr(basename, "_libretro");
               if (suffix)
                  *suffix = '\0';
            }
            if (strcasecmp(name, basename) == 0)
            {
               strlcpy(core_path_out, info->path, core_path_size);
               return true;
            }
         }
      }
   }

   return false;
}

/*
 * Read core path from a folder's core.txt file.
 * The file can contain multiple lines, each with a core name in order of preference.
 * The first installed core found will be used.
 *
 * Example core.txt:
 *   snes9x
 *   bsnes
 *   mesen-s
 *
 * Each line can be:
 *   - Full path to core (e.g., /path/to/cores/snes9x_libretro.dylib)
 *   - Core name (e.g., snes9x, Snes9x, SNES9x) - case insensitive
 *   - Core filename without _libretro suffix (e.g., snes9x)
 *
 * Returns true if a valid core path was found and written to core_path_out.
 */
static bool streamlined_read_folder_core(const char *folder_path, char *core_path_out, size_t core_path_size)
{
   char core_file_path[PATH_MAX_LENGTH];
   RFILE *file;
   char line[PATH_MAX_LENGTH];

   if (string_is_empty(folder_path))
      return false;

   /* Build path to core.txt */
   fill_pathname_join_special(core_file_path, folder_path, ".core.txt", sizeof(core_file_path));

   /* Check if file exists */
   if (!path_is_valid(core_file_path))
      return false;

   /* Read the core path from the file */
   file = filestream_open(core_file_path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return false;

   /* Read each line and try to find an installed core */
   while (filestream_gets(file, line, sizeof(line)))
   {
      /* Trim trailing newline/whitespace */
      string_trim_whitespace(line);

      if (string_is_empty(line))
         continue;

      /* Skip comment lines */
      if (line[0] == '#')
         continue;

      /* Try to find this core */
      if (streamlined_find_core_by_name(line, core_path_out, core_path_size))
      {
         filestream_close(file);
         return true;
      }
   }

   filestream_close(file);
   return false;
}

/*
 * Save the selected core to the folder's core.txt file.
 * Uses the core's base name (without _libretro suffix) for portability.
 */
static bool streamlined_save_folder_core(const char *folder_path, const char *core_path)
{
   char core_file_path[PATH_MAX_LENGTH];
   char core_name[256];
   RFILE *file;
   const char *filename;
   char *suffix;

   if (string_is_empty(folder_path) || string_is_empty(core_path))
      return false;

   /* Extract the core name from the path */
   filename = path_basename(core_path);
   if (!filename)
      return false;

   strlcpy(core_name, filename, sizeof(core_name));
   path_remove_extension(core_name);

   /* Remove _libretro suffix if present */
   suffix = strstr(core_name, "_libretro");
   if (suffix)
      *suffix = '\0';

   /* Build path to core.txt */
   fill_pathname_join_special(core_file_path, folder_path, ".core.txt", sizeof(core_file_path));

   /* Write the core name to the file */
   file = filestream_open(core_file_path, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return false;

   filestream_printf(file, "%s\n", core_name);
   filestream_close(file);

   return true;
}

/*
 * Derive the game identifier used for per-game core override files.
 * If the game's parent directory differs from folder_path (M3U subfolder case),
 * use the subfolder name. Otherwise use the game's basename with extension removed.
 */
static void streamlined_get_game_core_name(
      const char *game_path, const char *folder_path,
      char *out, size_t out_size)
{
   char parent_dir[PATH_MAX_LENGTH];
   char folder_clean[PATH_MAX_LENGTH];
   const char *base;

   out[0] = '\0';
   if (string_is_empty(game_path))
      return;

   fill_pathname_parent_dir(parent_dir, game_path, sizeof(parent_dir));
   /* Remove trailing slash for comparison */
   {
      size_t len = strlen(parent_dir);
      if (len > 0 && parent_dir[len - 1] == '/')
         parent_dir[len - 1] = '\0';
   }

   strlcpy(folder_clean, folder_path, sizeof(folder_clean));
   {
      size_t len = strlen(folder_clean);
      if (len > 0 && folder_clean[len - 1] == '/')
         folder_clean[len - 1] = '\0';
   }

   /* M3U subfolder: game lives in a subdirectory of the folder */
   if (!string_is_empty(folder_path)
         && strcmp(parent_dir, folder_clean) != 0)
   {
      base = path_basename(parent_dir);
      if (!string_is_empty(base))
      {
         strlcpy(out, base, out_size);
         return;
      }
   }

   /* Normal case: use game filename without extension */
   base = path_basename(game_path);
   if (!string_is_empty(base))
   {
      char *ext;
      strlcpy(out, base, out_size);
      ext = strrchr(out, '.');
      if (ext)
         *ext = '\0';
   }
}

/*
 * Read core path from a per-file core override file (.core.$GAME.txt).
 * Uses the same multi-line format and lookup logic as streamlined_read_folder_core.
 */
static bool streamlined_read_core_file(const char *core_file_path,
      char *core_path_out, size_t size)
{
   RFILE *file;
   char line[PATH_MAX_LENGTH];

   if (string_is_empty(core_file_path) || !path_is_valid(core_file_path))
      return false;

   file = filestream_open(core_file_path,
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return false;

   while (filestream_gets(file, line, sizeof(line)))
   {
      string_trim_whitespace(line);
      if (string_is_empty(line) || line[0] == '#')
         continue;
      if (streamlined_find_core_by_name(line, core_path_out, size))
      {
         filestream_close(file);
         return true;
      }
   }

   filestream_close(file);
   return false;
}

/*
 * Read per-game core override from .core.$GAME.txt in the given folder.
 */
static bool streamlined_read_game_core(const char *folder_path,
      const char *game_name, char *core_path_out, size_t size)
{
   char core_file_path[PATH_MAX_LENGTH];
   char filename[PATH_MAX_LENGTH];

   if (string_is_empty(folder_path) || string_is_empty(game_name))
      return false;

   snprintf(filename, sizeof(filename), ".core.%s.txt", game_name);
   fill_pathname_join_special(core_file_path, folder_path, filename,
         sizeof(core_file_path));

   return streamlined_read_core_file(core_file_path, core_path_out, size);
}

/*
 * Save per-game core override to .core.$GAME.txt.
 */
static bool streamlined_save_game_core(const char *folder_path,
      const char *game_name, const char *core_path)
{
   char core_file_path[PATH_MAX_LENGTH];
   char filename[PATH_MAX_LENGTH];
   char core_name[256];
   RFILE *file;
   const char *basename;
   char *suffix;

   if (string_is_empty(folder_path) || string_is_empty(game_name)
         || string_is_empty(core_path))
      return false;

   basename = path_basename(core_path);
   if (!basename)
      return false;

   strlcpy(core_name, basename, sizeof(core_name));
   path_remove_extension(core_name);
   suffix = strstr(core_name, "_libretro");
   if (suffix)
      *suffix = '\0';

   snprintf(filename, sizeof(filename), ".core.%s.txt", game_name);
   fill_pathname_join_special(core_file_path, folder_path, filename,
         sizeof(core_file_path));

   file = filestream_open(core_file_path,
         RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return false;

   filestream_printf(file, "%s\n", core_name);
   filestream_close(file);
   return true;
}

/*
 * Delete per-game core override file .core.$GAME.txt.
 */
static void streamlined_delete_game_core(const char *folder_path,
      const char *game_name)
{
   char core_file_path[PATH_MAX_LENGTH];
   char filename[PATH_MAX_LENGTH];

   if (string_is_empty(folder_path) || string_is_empty(game_name))
      return;

   snprintf(filename, sizeof(filename), ".core.%s.txt", game_name);
   fill_pathname_join_special(core_file_path, folder_path, filename,
         sizeof(core_file_path));

   filestream_delete(core_file_path);
}

/*
 * Return a display name for a core given its path.
 * Uses core_info to find a friendly name, or returns "Not Set" if empty.
 */
static const char *streamlined_get_core_display_name(const char *core_path)
{
   core_info_t *info = NULL;

   if (string_is_empty(core_path))
      return "Not Set";

   if (core_info_find(core_path, &info) && info)
   {
      if (!string_is_empty(info->display_name))
         return info->display_name;
      if (!string_is_empty(info->core_name))
         return info->core_name;
   }

   return "Not Set";
}

/*
 * Return the effective core path: per-game override if set, else folder core.
 */
static const char *streamlined_effective_core(streamlined_t *strm)
{
   if (!string_is_empty(strm->options_game_core_path))
      return strm->options_game_core_path;
   return strm->options_core_path;
}

/* Remove trailing path separator from a directory path in-place */
static void streamlined_strip_trailing_slash(char *path)
{
   size_t len = strlen(path);
   if (len > 0 && path[len - 1] == '/')
      path[len - 1] = '\0';
}

/*
 * Derive the original folder path for a favorites game entry.
 * Normal ROM: parent directory.
 * M3U file: grandparent directory (M3U lives inside game subfolder).
 */
static void streamlined_get_content_folder_path(const char *game_path,
      char *folder_out, size_t folder_size)
{
   char parent_dir[PATH_MAX_LENGTH];

   folder_out[0] = '\0';
   if (string_is_empty(game_path))
      return;

   fill_pathname_parent_dir(parent_dir, game_path, sizeof(parent_dir));
   streamlined_strip_trailing_slash(parent_dir);

   if (m3u_file_is_m3u(game_path))
   {
      /* M3U lives in game subfolder — grandparent is the platform folder */
      fill_pathname_parent_dir(folder_out, parent_dir, folder_size);
      streamlined_strip_trailing_slash(folder_out);
   }
   else
      strlcpy(folder_out, parent_dir, folder_size);
}

/*
 * Resolve core for a content entry (favorites or history). Priority:
 * 1. Per-game override (.core.$GAME.txt in content folder)
 * 2. Folder core (.core.txt in content folder)
 * 3. For M3U: parent dir's .core.txt (harmless no-op for non-M3U)
 * 4. Core stored in playlist entry
 * Returns false if no core found.
 */
static bool streamlined_resolve_content_core(const char *game_path,
      char *core_out, size_t core_size,
      char *game_core_out, size_t game_core_size,
      playlist_t *playlist)
{
   char folder_path[PATH_MAX_LENGTH];
   char game_name[PATH_MAX_LENGTH];

   core_out[0] = '\0';
   if (string_is_empty(game_path))
      return false;

   streamlined_get_content_folder_path(game_path, folder_path, sizeof(folder_path));

   /* 1. Per-game core override */
   streamlined_get_game_core_name(game_path, folder_path,
         game_name, sizeof(game_name));
   if (streamlined_read_game_core(folder_path, game_name,
         core_out, core_size))
   {
      if (game_core_out)
         strlcpy(game_core_out, core_out, game_core_size);
      return true;
   }
   if (game_core_out)
      game_core_out[0] = '\0';

   /* 2. Folder core */
   if (streamlined_read_folder_core(folder_path, core_out, core_size))
      return true;

   /* 3. For M3U, try parent directory's .core.txt */
   if (m3u_file_is_m3u(game_path))
   {
      char parent_dir[PATH_MAX_LENGTH];
      fill_pathname_parent_dir(parent_dir, game_path, sizeof(parent_dir));
      streamlined_strip_trailing_slash(parent_dir);
      if (streamlined_read_folder_core(parent_dir, core_out, core_size))
         return true;
   }

   /* 4. Core stored in playlist entry */
   if (playlist)
   {
      const struct playlist_entry *pl_entry = NULL;
      playlist_get_index_by_path(playlist, game_path, &pl_entry);
      if (pl_entry && !string_is_empty(pl_entry->core_path)
            && !string_is_equal(pl_entry->core_path, "DETECT"))
      {
         char resolved_core[PATH_MAX_LENGTH];
         strlcpy(resolved_core, pl_entry->core_path, sizeof(resolved_core));
         playlist_resolve_path(PLAYLIST_LOAD, true,
               resolved_core, sizeof(resolved_core));
         if (path_is_valid(resolved_core))
         {
            strlcpy(core_out, resolved_core, core_size);
            return true;
         }
      }
   }

   return false;
}

/*
 * Add a game to the favorites playlist.
 */
static void streamlined_add_to_favorites(const char *game_path,
      const char *core_path)
{
   playlist_t *favorites = g_defaults.content_favorites;
   struct playlist_entry entry;
   char display_name[256];
   core_info_t *core_info = NULL;
   const char *core_name = "DETECT";

   if (!favorites || string_is_empty(game_path))
      return;

   /* Check duplicate */
   if (playlist_entry_exists(favorites, game_path))
      return;

   /* Check capacity */
   if (playlist_size(favorites) >= playlist_capacity(favorites))
      return;

   /* Derive display name */
   {
      bool is_m3u = m3u_file_is_m3u(game_path);
      if (is_m3u)
      {
         /* For M3U, use parent folder name */
         char parent_dir[PATH_MAX_LENGTH];
         fill_pathname_parent_dir(parent_dir, game_path, sizeof(parent_dir));
         streamlined_strip_trailing_slash(parent_dir);
         streamlined_get_display_name(parent_dir,
               display_name, sizeof(display_name), true);
      }
      else
         streamlined_get_display_name(game_path,
               display_name, sizeof(display_name), false);
   }

   /* Resolve core name */
   if (!string_is_empty(core_path)
         && core_info_find(core_path, &core_info) && core_info)
      core_name = core_info->core_name ? core_info->core_name : "DETECT";

   memset(&entry, 0, sizeof(entry));
   entry.path      = (char*)game_path;
   entry.label     = display_name;
   entry.core_path = (char*)(string_is_empty(core_path) ? "DETECT" : core_path);
   entry.core_name = (char*)core_name;

   playlist_push(favorites, &entry);
   playlist_qsort(favorites);
   playlist_write_file(favorites);
}

/*
 * Remove a game from the favorites playlist.
 */
static void streamlined_remove_from_favorites(const char *game_path)
{
   playlist_t *favorites = g_defaults.content_favorites;

   if (!favorites || string_is_empty(game_path))
      return;

   playlist_delete_by_path(favorites, game_path);
   playlist_write_file(favorites);
}

/*
 * Set state flags to return from favorites to the top-level menu.
 */
static void streamlined_exit_favorites(streamlined_t *strm)
{
   strm->return_to_top_level = true;
   strm->top_level_selection = strm->favorites_saved_selection;
   strm->in_favorites = false;
}

/*
 * Populate the menu with favorites playlist entries.
 */
static void streamlined_populate_favorites_menu(streamlined_t *strm)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   playlist_t *favorites;
   size_t pl_size, j;

   if (!menu_st || !strm)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   menu_entries_clear(list);

   favorites = g_defaults.content_favorites;
   if (!favorites)
   {
      menu_entries_append(list,
            "No favorites", "",
            MSG_UNKNOWN, FILE_TYPE_NONE,
            0, 0, NULL);
      return;
   }

   pl_size = playlist_size(favorites);
   if (pl_size == 0)
   {
      menu_entries_append(list,
            "No favorites", "",
            MSG_UNKNOWN, FILE_TYPE_NONE,
            0, 0, NULL);
      return;
   }

   for (j = 0; j < pl_size; j++)
   {
      const struct playlist_entry *pl_entry = NULL;
      char resolved_path[PATH_MAX_LENGTH];

      playlist_get_index(favorites, j, &pl_entry);
      if (!pl_entry || string_is_empty(pl_entry->path))
         continue;

      /* Resolve path (playlist_push abbreviates on iOS) */
      strlcpy(resolved_path, pl_entry->path, sizeof(resolved_path));
      playlist_resolve_path(PLAYLIST_LOAD, false,
            resolved_path, sizeof(resolved_path));

      {
         char m3u_path[PATH_MAX_LENGTH];
         if (streamlined_find_m3u_for_content(resolved_path,
               m3u_path, sizeof(m3u_path)))
         {
            char name_buf[256];
            char m3u_parent[PATH_MAX_LENGTH];
            fill_pathname_parent_dir(m3u_parent, m3u_path, sizeof(m3u_parent));
            streamlined_strip_trailing_slash(m3u_parent);
            streamlined_get_display_name(m3u_parent,
                  name_buf, sizeof(name_buf), true);
            menu_entries_append(list,
                  name_buf, resolved_path,
                  MSG_UNKNOWN, FILE_TYPE_PLAIN,
                  0, 0, NULL);
         }
         else if (!string_is_empty(pl_entry->label))
         {
            menu_entries_append(list,
                  pl_entry->label, resolved_path,
                  MSG_UNKNOWN, FILE_TYPE_PLAIN,
                  0, 0, NULL);
         }
         else
         {
            char name_buf[256];
            streamlined_get_display_name(resolved_path,
                  name_buf, sizeof(name_buf), false);
            menu_entries_append(list,
                  name_buf, resolved_path,
                  MSG_UNKNOWN, FILE_TYPE_PLAIN,
                  0, 0, NULL);
         }
      }
   }
}

/*
 * Populate the menu with a list of all installed cores.
 * Used when the user needs to select which core to use for a folder.
 */
static void streamlined_populate_core_selection(streamlined_t *strm, const char *content_path)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   core_info_list_t *core_info_list = NULL;
   size_t i;

   if (!menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   /* Clear existing entries */
   menu_entries_clear(list);

   /* Get list of all installed cores */
   core_info_get_list(&core_info_list);
   core_info_qsort(core_info_list, CORE_INFO_LIST_SORT_DISPLAY_NAME);

   if (!core_info_list || core_info_list->count == 0)
   {
      menu_entries_append(list,
            "No cores installed",
            "",
            MSG_UNKNOWN,
            FILE_TYPE_NONE,
            0, 0, NULL);
      return;
   }

   /* Add each installed core to the list */
   for (i = 0; i < core_info_list->count; i++)
   {
      const core_info_t *info = &core_info_list->list[i];
      const char *display_name = info->display_name ? info->display_name : info->core_name;

      if (string_is_empty(info->path) || string_is_empty(display_name))
         continue;

      menu_entries_append(list,
            display_name,       /* Display name */
            info->path,         /* Core path in label for selection */
            MSG_UNKNOWN,
            FILE_TYPE_CORE,
            0, 0, NULL);
   }
}

/*
 * Move trailing article (", The" / ", A" / ", An") to the front of the title.
 * Only considers the portion before the first " - " subtitle separator.
 * e.g. "Legend of Zelda, The - A Link to the Past"
 *   -> "The Legend of Zelda - A Link to the Past"
 */
static void streamlined_fix_article(char *name, size_t name_size)
{
   static const char *articles[] = { ", The", ", A", ", An" };
   char *subtitle;
   size_t main_len;
   unsigned i;

   if (!name || name[0] == '\0')
      return;

   /* Find first " - " subtitle separator */
   subtitle = strstr(name, " - ");
   main_len = subtitle ? (size_t)(subtitle - name) : strlen(name);

   for (i = 0; i < sizeof(articles) / sizeof(articles[0]); i++)
   {
      size_t art_len = strlen(articles[i]);
      if (main_len > art_len)
      {
         /* Check if main title ends with this article */
         const char *pos = name + main_len - art_len;
         if (strncmp(pos, articles[i], art_len) == 0)
         {
            /* articles[i] is ", The" — skip the ", " to get just the article word */
            const char *word = articles[i] + 2;
            size_t word_len = art_len - 2;
            size_t base_len = main_len - art_len; /* title without article */
            char tmp[512];

            /* Build: "The " + base title + subtitle */
            snprintf(tmp, sizeof(tmp), "%.*s %.*s%s",
                  (int)word_len, word,
                  (int)base_len, name,
                  subtitle ? subtitle : "");
            strlcpy(name, tmp, name_size);
            return;
         }
      }
   }
}

/*
 * Normalize a ROM display name:
 * 1. Strip parenthesized/bracketed tags (No-Intro/GoodTools)
 * 2. Fix trailing article placement
 */
static void streamlined_normalize_rom_name(char *name, size_t name_size)
{
   if (!name || name[0] == '\0')
      return;
   label_remove_parens_and_brackets(name);
   /* Trim trailing whitespace left after tag removal */
   {
      size_t len = strlen(name);
      while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t'))
         len--;
      name[len] = '\0';
   }
   streamlined_fix_article(name, name_size);
}

/*
 * Central display-name pipeline: extract basename, strip extension (for ROMs)
 * or sort prefix (for M3U folders), then optionally normalize.
 */
static void streamlined_get_display_name(
      const char *path, char *out, size_t out_size,
      bool is_m3u_folder)
{
   settings_t *settings = config_get_ptr();
   const char *name = path_basename(path);

   if (is_m3u_folder)
   {
      const char *clean = streamlined_strip_sort_prefix(name);
      strlcpy(out, clean, out_size);
   }
   else
   {
      strlcpy(out, name, out_size);
      path_remove_extension(out);
   }

   if (settings->bools.menu_streamlined_normalize_rom_names)
      streamlined_normalize_rom_name(out, out_size);
}

/*
 * Comparator for file_list_t entries used with qsort.
 * Sorts FILE_TYPE_DIRECTORY first, then alphabetically (case-insensitive)
 * by the display name stored in entry->path.
 */
static int streamlined_entry_cmp(const void *a, const void *b)
{
   const struct item_file *ea = (const struct item_file *)a;
   const struct item_file *eb = (const struct item_file *)b;
   bool dir_a = (ea->type == FILE_TYPE_DIRECTORY);
   bool dir_b = (eb->type == FILE_TYPE_DIRECTORY);

   if (dir_a != dir_b)
      return dir_a ? -1 : 1;

   return strcasecmp(ea->path, eb->path);
}

/*
 * Strip leading sort prefix from folder name (e.g., "1) Game Boy" -> "Game Boy")
 * Pattern: one or more digits followed by ") "
 * Returns pointer to the start of the actual name (within the same string)
 */
static const char *streamlined_strip_sort_prefix(const char *name)
{
   const char *p = name;

   /* Check for leading digits */
   if (!p || !isdigit((unsigned char)*p))
      return name;

   /* Skip all digits */
   while (*p && isdigit((unsigned char)*p))
      p++;

   /* Check for ") " after digits */
   if (p[0] == ')' && p[1] == ' ')
      return p + 2;

   /* No valid prefix found, return original */
   return name;
}

/* Check if a directory contains an M3U file whose name matches the folder name */
static bool streamlined_check_m3u_folder(const char *dir_path,
      char *m3u_path_out, size_t m3u_path_size)
{
   char m3u_file[PATH_MAX_LENGTH];
   const char *dir_name = path_basename(dir_path);
   if (string_is_empty(dir_name))
      return false;
   fill_pathname_join_special(m3u_file, dir_path, dir_name, sizeof(m3u_file));
   strlcat(m3u_file, ".m3u", sizeof(m3u_file));
   if (path_is_valid(m3u_file))
   {
      strlcpy(m3u_path_out, m3u_file, m3u_path_size);
      return true;
   }
   return false;
}

/*
 * Given any content path, find the M3U file if it belongs to an M3U game.
 * Handles both: content IS an M3U file, or content is a disc file inside
 * an M3U folder (e.g., PSX/Game/disc1.bin -> finds PSX/Game/Game.m3u).
 */
static bool streamlined_find_m3u_for_content(const char *content_path,
      char *m3u_out, size_t m3u_size)
{
   char parent_dir[PATH_MAX_LENGTH];
   if (string_is_empty(content_path))
      return false;
   if (m3u_file_is_m3u(content_path))
   {
      strlcpy(m3u_out, content_path, m3u_size);
      return true;
   }
   fill_pathname_parent_dir(parent_dir, content_path, sizeof(parent_dir));
   streamlined_strip_trailing_slash(parent_dir);
   return streamlined_check_m3u_folder(parent_dir, m3u_out, m3u_size);
}

/* Check if a core supports M3U files natively */
static bool streamlined_core_supports_m3u(const char *core_path)
{
   core_info_t *info = NULL;
   if (!core_info_find(core_path, &info) || !info || !info->supported_extensions_list)
      return false;
   return string_list_find_elem_prefix(info->supported_extensions_list, ".", "m3u");
}

/* Parse M3U and return the first entry's full path */
static bool streamlined_resolve_m3u_content(const char *m3u_path,
      char *content_path_out, size_t content_path_size)
{
   m3u_file_t *m3u       = m3u_file_init(m3u_path);
   m3u_file_entry_t *entry = NULL;
   if (!m3u)
      return false;
   if (m3u_file_get_size(m3u) == 0 || !m3u_file_get_entry(m3u, 0, &entry))
   {
      m3u_file_free(m3u);
      return false;
   }
   strlcpy(content_path_out, entry->full_path, content_path_size);
   m3u_file_free(m3u);
   return true;
}

static void streamlined_request_loading(
      streamlined_t *strm,
      const char *core_path, const char *content_path,
      bool is_resume)
{
   char resolved[PATH_MAX_LENGTH];
   const char *final_content = content_path;

   if (m3u_file_is_m3u(content_path)
         && !streamlined_core_supports_m3u(core_path))
   {
      if (streamlined_resolve_m3u_content(content_path,
            resolved, sizeof(resolved)))
         final_content = resolved;
   }

   strlcpy(strm->loading_core_path, core_path, sizeof(strm->loading_core_path));
   strlcpy(strm->loading_content_path, final_content, sizeof(strm->loading_content_path));
   strm->loading_is_resume  = is_resume;
   strm->loading_pending    = true;
   strm->loading_triggered  = false;
}

static void streamlined_execute_deferred_load(streamlined_t *strm)
{
   content_ctx_info_t content_info;
   content_info.argc        = 0;
   content_info.argv        = NULL;
   content_info.args        = NULL;
   content_info.environ_get = NULL;

   strm->is_custom_main_menu = false;
   strm->in_folder           = false;
   strm->selecting_core      = false;
   runloop_state_get_ptr()->entry_state_slot = -1;

   command_event(CMD_EVENT_MENU_TOGGLE, NULL);

   task_push_load_content_with_new_core_from_menu(
         strm->loading_core_path,
         strm->loading_content_path,
         &content_info,
         CORE_TYPE_PLAIN, NULL, NULL);

   if (strm->loading_is_resume)
   {
      settings_t *settings       = config_get_ptr();
      runloop_state_t *runloop_st = runloop_state_get_ptr();
      if (!settings->bools.savestate_auto_load)
      {
         char auto_path[PATH_MAX_LENGTH];
         size_t _len = strlcpy(auto_path, runloop_st->name.savestate,
               sizeof(auto_path));
         strlcpy(auto_path + _len, ".auto", sizeof(auto_path) - _len);
         if (path_is_valid(auto_path))
            content_load_state(auto_path, false, true);
      }
   }

   strm->loading_pending         = false;
   strm->loading_triggered       = false;
   strm->loading_core_path[0]    = '\0';
   strm->loading_content_path[0] = '\0';
   strm->pending_content_path[0] = '\0';
}

static void streamlined_execute_deferred_exit(streamlined_t *strm)
{
   strm->exiting_pending   = false;
   strm->exiting_triggered = false;

   if (strm->exiting_is_cli)
   {
      command_event(CMD_EVENT_QUIT, NULL);
   }
   else
   {
      strm->is_quick_menu = false;
      strm->in_settings_submenu = false;
      strm->return_to_settings_submenu = false;

      command_event(CMD_EVENT_UNLOAD_CORE, NULL);
      menu_entries_flush_stack(msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU), 0);
   }
}

/* Populate custom main menu with folders and files from the specified directory
 * show_folder_slash: if true, prefix folder names with "/" */
static void streamlined_populate_folder_menu(streamlined_t *strm, const char *directory, bool show_folder_slash)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   settings_t *settings = config_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   struct string_list *str_list;
   unsigned i;

   if (!menu_st || !directory || directory[0] == '\0')
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   /* Clear existing entries */
   menu_entries_clear(list);

   /* Scan directory for folders and files */
   str_list = dir_list_new(directory, NULL, true,
         settings->bools.show_hidden_files, true, false);

   if (str_list && str_list->size)
   {
      /* Sort alphabetically with directories first */
      dir_list_sort(str_list, true);

      for (i = 0; i < str_list->size; i++)
      {
         const char *path = str_list->elems[i].data;
         unsigned attr = str_list->elems[i].attr.i;
         const char *name = path_basename(path);

         /* Skip hidden items (starting with .) */
         if (!name || name[0] == '.')
            continue;

         if (attr == RARCH_DIRECTORY)
         {
            char m3u_path[PATH_MAX_LENGTH];

            if (streamlined_check_m3u_folder(path, m3u_path, sizeof(m3u_path)))
            {
               /* M3U game folder — show as launchable game */
               char display_name[256];
               streamlined_get_display_name(path,
                     display_name, sizeof(display_name), true);

               menu_entries_append(list,
                     display_name,     /* folder name = game title */
                     m3u_path,         /* M3U file path for loading */
                     MSG_UNKNOWN,
                     FILE_TYPE_PLAIN,
                     0, 0, NULL);
            }
            else
            {
               /* Show directories, optionally with leading slash
                * Strip sort prefix (e.g., "1) Game Boy" -> "Game Boy") */
               char display_name[256];
               const char *clean_name = streamlined_strip_sort_prefix(name);

               if (show_folder_slash)
                  snprintf(display_name, sizeof(display_name), "/%s", clean_name);
               else
                  strlcpy(display_name, clean_name, sizeof(display_name));

               menu_entries_append(list,
                     display_name,     /* Display name (entry->path for rendering) */
                     path,             /* Full path (entry->label for navigation) */
                     MSG_UNKNOWN,
                     FILE_TYPE_DIRECTORY,
                     0, 0, NULL);
            }
         }
         else
         {
            /* Show files (ROMs) - strip extension for cleaner display */
            char display_name[256];
            streamlined_get_display_name(path,
                  display_name, sizeof(display_name), false);

            menu_entries_append(list,
                  display_name,     /* Display name (entry->path for rendering) */
                  path,             /* Full path (entry->label for loading) */
                  MSG_UNKNOWN,
                  FILE_TYPE_PLAIN,
                  0, 0, NULL);
         }
      }

      /* Re-sort by normalized display names when enabled */
      if (settings->bools.menu_streamlined_normalize_rom_names
            && list->size > 1)
         qsort(list->list, list->size,
               sizeof(struct item_file), streamlined_entry_cmp);
   }

   if (str_list)
      string_list_free(str_list);

   /* Show empty message if folder has no content (only for platform folders, not top level) */
   if (show_folder_slash && list->size == 0)
   {
      menu_entries_append(list,
            "No games found",
            "",
            MSG_UNKNOWN,
            FILE_TYPE_NONE,
            0, 0, NULL);
   }

   /* Add Game Switcher entry at top level when history non-empty and enabled */
   if (!show_folder_slash)
   {
      settings_t *hist_settings = config_get_ptr();
      playlist_t *history = g_defaults.content_history;
      if (hist_settings && hist_settings->bools.menu_content_show_history
            && history && playlist_size(history) > 0)
      {
         menu_entries_prepend(list,
               "Game Switcher", "streamlined_game_switcher",
               STREAMLINED_GAME_SWITCHER_ENTRY,
               MENU_SETTING_ACTION, 0, 0);
      }
   }

   /* Add Favorites entry at top level when non-empty and enabled */
   if (!show_folder_slash)
   {
      settings_t *fav_settings = config_get_ptr();
      playlist_t *favorites = g_defaults.content_favorites;
      if (fav_settings && fav_settings->bools.menu_content_show_favorites
            && favorites && playlist_size(favorites) > 0)
      {
         menu_entries_prepend(list,
               "Favorites", "streamlined_favorites",
               STREAMLINED_FAVORITES_ENTRY,
               MENU_SETTING_ACTION, 0, 0);
      }
   }

   /* Add Settings and Quit options at the bottom (only at top level) */
   if (!show_folder_slash)
   {
      menu_entries_append(list,
            "Settings",
            "streamlined_main_settings",
            MENU_ENUM_LABEL_SETTINGS,
            MENU_SETTING_ACTION,
            0, 0, NULL);

#if !TARGET_OS_TV
      menu_entries_append(list,
            "Quit",
            msg_hash_to_str(MENU_ENUM_LABEL_QUIT_RETROARCH),
            MENU_ENUM_LABEL_QUIT_RETROARCH,
            MENU_SETTING_ACTION,
            0, 0, NULL);
#endif
   }
}

/* ======================================================================
 * MENU DRIVER INTERFACE
 * ====================================================================== */

#if TARGET_OS_TV
/* Resolve the system UI font file path on tvOS using CoreText.
 * Uses CTFontCreateUIFontForLanguage which returns the actual system
 * font regardless of its internal name across tvOS versions. */
static bool streamlined_get_system_font_path(char *buf, size_t buf_size)
{
   bool result = false;
   CTFontRef font = CTFontCreateUIFontForLanguage(
         kCTFontUIFontSystem, 12.0, NULL);
   if (font)
   {
      CFURLRef url = (CFURLRef)CTFontCopyAttribute(font, kCTFontURLAttribute);
      if (url)
      {
         CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
         if (path)
         {
            result = CFStringGetCString(path, buf, (CFIndex)buf_size,
                  kCFStringEncodingUTF8);
            CFRelease(path);
         }
         CFRelease(url);
      }
      CFRelease(font);
   }
   return result;
}
#endif

/*
 * Try to load a font from the given path within the assets directory.
 * Returns the loaded font or NULL if not found.
 */
static font_data_t *streamlined_try_load_font(gfx_display_t *p_disp,
      const char *assets_dir, const char *font_subpath,
      float font_size, char *fontpath_out, size_t fontpath_size,
      bool is_threaded)
{
   font_data_t *font = NULL;

   if (assets_dir[0] == '\0')
      return NULL;

   fill_pathname_join_special(fontpath_out, assets_dir, font_subpath, fontpath_size);
   font = gfx_display_font_file(p_disp, fontpath_out, font_size, is_threaded);

   return font;
}

static void *streamlined_init(void **userdata, bool video_is_threaded)
{
   gfx_display_t *p_disp = disp_get_ptr();
   streamlined_t *strm = (streamlined_t*)calloc(1, sizeof(*strm));
   menu_handle_t *menu = (menu_handle_t*)calloc(1, sizeof(*menu));

   if (!strm || !menu)
   {
      if (strm) free(strm);
      if (menu) free(menu);
      return NULL;
   }

   *userdata = strm;

   srand((unsigned)time(NULL));

   p_disp->framebuf_width = 0;
   p_disp->framebuf_height = 0;

   return menu;
}

static void streamlined_free(void *data)
{
   streamlined_t *strm = (streamlined_t*)data;
   if (strm)
   {
      strm->font.font = NULL;
      strm->font_small.font = NULL;
      strm->font_title.font = NULL;
      if (strm->search_all_entries)
      {
         string_list_free(strm->search_all_entries);
         strm->search_all_entries = NULL;
      }
#if TARGET_OS_TV
      if (strm->search_kb_buffer_ptr)
      {
         free(strm->search_kb_buffer_ptr);
         strm->search_kb_buffer_ptr = NULL;
      }
#endif
   }
}

static void streamlined_context_reset(void *data, bool is_threaded)
{
   unsigned video_width, video_height;
   streamlined_t *strm = (streamlined_t*)data;
   gfx_display_t *p_disp = disp_get_ptr();
   settings_t *settings = config_get_ptr();
   char fontpath[PATH_MAX_LENGTH];
   float scale_factor;

   if (!strm)
      return;

   /* Get actual video dimensions (following Ozone/MaterialUI pattern).
    * p_disp->framebuf_width/height may be 0 at this point since
    * streamlined doesn't use a software framebuffer. */
   video_driver_get_size(&video_width, &video_height);

   /* gfx_display_get_dpi_scale() returns a DPI-aware scale factor
    * that already incorporates settings->floats.menu_scale_factor.
    * On macOS (where DPI queries crash), it falls back to a
    * pixel-diagonal-based formula. */
   scale_factor = gfx_display_get_dpi_scale(p_disp,
         settings,
         video_width,
         video_height,
         settings->bools.video_fullscreen,
         false);

   /* Ensure a minimum scale to prevent unusably small UI */
   if (scale_factor < 0.5f)
      scale_factor = 0.5f;

   strm->scale_factor = scale_factor;
   strm->font_size = STREAMLINED_BASE_FONT_SIZE * scale_factor;
   strm->font_size_small = STREAMLINED_BASE_FONT_SIZE * scale_factor * 0.75f;
   strm->font_size_title = STREAMLINED_BASE_FONT_SIZE * scale_factor * 1.1f;
   /* Tiny font sized to match dot indicators (dot_radius * 2 is diameter) */
   strm->font_size_tiny = 4 * scale_factor * 2.5f;

   /* Clamp font sizes to ensure readability */
   if (strm->font_size < STREAMLINED_MIN_FONT_SIZE)
      strm->font_size = STREAMLINED_MIN_FONT_SIZE;
   if (strm->font_size_small < STREAMLINED_MIN_FONT_SIZE - 2)
      strm->font_size_small = STREAMLINED_MIN_FONT_SIZE - 2;
   if (strm->font_size_tiny < 8)
      strm->font_size_tiny = 8;

   /* Pill padding scales with font size so it stays proportional with different fonts */
   strm->pill_padding = (int)(strm->font_size * STREAMLINED_PILL_PADDING_RATIO);

   /* Free existing fonts before reloading */
   if (strm->font.font)
   {
      font_driver_free(strm->font.font);
      strm->font.font = NULL;
   }
   if (strm->font_small.font)
   {
      font_driver_free(strm->font_small.font);
      strm->font_small.font = NULL;
   }
   if (strm->font_title.font)
   {
      font_driver_free(strm->font_title.font);
      strm->font_title.font = NULL;
   }
   if (strm->font_tiny.font)
   {
      font_driver_free(strm->font_tiny.font);
      strm->font_tiny.font = NULL;
   }

   fontpath[0] = '\0';

   /*
    * Font loading priority:
    * 1. Streamlined font (assets/streamlined/font.ttf) for custom styling
    * 2. XMB font (assets/xmb/monochrome/font.ttf) commonly available
    * 3. Ozone font (assets/ozone/regular.ttf) as final fallback
    */
#if TARGET_OS_TV
   /* Prefer system font on tvOS for native feel */
   if (!strm->font.font && streamlined_get_system_font_path(fontpath, sizeof(fontpath)))
      strm->font.font = gfx_display_font_file(p_disp, fontpath, strm->font_size, is_threaded);
#endif
   /* Existing fallback chain follows... */
   if (!strm->font.font)
      strm->font.font = streamlined_try_load_font(p_disp,
            settings->paths.directory_assets, "streamlined/font.ttf",
            strm->font_size, fontpath, sizeof(fontpath), is_threaded);

   if (!strm->font.font)
      strm->font.font = streamlined_try_load_font(p_disp,
            settings->paths.directory_assets, "xmb/monochrome/font.ttf",
            strm->font_size, fontpath, sizeof(fontpath), is_threaded);

   if (!strm->font.font)
      strm->font.font = streamlined_try_load_font(p_disp,
            settings->paths.directory_assets, "ozone/regular.ttf",
            strm->font_size, fontpath, sizeof(fontpath), is_threaded);

   /* Load additional font sizes using the same font file that worked */
   if (strm->font.font && fontpath[0] != '\0')
   {
      strm->font_small.font = gfx_display_font_file(p_disp, fontpath, strm->font_size_small, is_threaded);
      strm->font_title.font = gfx_display_font_file(p_disp, fontpath, strm->font_size_title, is_threaded);
      strm->font_tiny.font = gfx_display_font_file(p_disp, fontpath, strm->font_size_tiny, is_threaded);
   }

   strm->font.line_height = (int)(strm->font_size * STREAMLINED_LINE_HEIGHT);
   strm->font.glyph_width = (int)(strm->font_size * 0.6f);
   strm->font_small.line_height = (int)(strm->font_size_small * STREAMLINED_LINE_HEIGHT);
   strm->font_small.glyph_width = (int)(strm->font_size_small * 0.6f);
   strm->font_title.line_height = (int)(strm->font_size_title * STREAMLINED_LINE_HEIGHT);
   strm->font_title.glyph_width = (int)(strm->font_size_title * 0.6f);

   if (strm->font.line_height < 20)
      strm->font.line_height = 20;
   if (strm->font_small.line_height < 15)
      strm->font_small.line_height = 15;

   /* Initialize save slot selector state */
   gfx_thumbnail_reset(&strm->savestate_thumbnail);
   strm->savestate_thumbnail_path[0] = '\0';
   strm->preview_slot = 0;
   strm->show_slot_selector = false;
   strm->last_selection = 0;

   /* Initialize ticker for text scrolling */
   strm->ticker_idx = 0;
   strm->item_ticker_start = 0;
   strm->item_ticker_selection = (size_t)-1;

   gfx_display_init_white_texture();

}

static void streamlined_context_destroy(void *data)
{
   streamlined_t *strm = (streamlined_t*)data;

   if (strm)
   {
      if (strm->font.font)
      {
         font_driver_free(strm->font.font);
         strm->font.font = NULL;
      }
      if (strm->font_small.font)
      {
         font_driver_free(strm->font_small.font);
         strm->font_small.font = NULL;
      }
      if (strm->font_title.font)
      {
         font_driver_free(strm->font_title.font);
         strm->font_title.font = NULL;
      }
      if (strm->font_tiny.font)
      {
         font_driver_free(strm->font_tiny.font);
         strm->font_tiny.font = NULL;
      }

      /* Clean up save slot thumbnail */
      gfx_thumbnail_reset(&strm->savestate_thumbnail);

      /* Clean up ROM/directory thumbnail */
      gfx_thumbnail_reset(&strm->rom_thumbnail);

      /* Clean up random game preview thumbnail */
      gfx_thumbnail_reset(&strm->random_thumbnail);

      /* Clean up game switcher thumbnail */
      gfx_thumbnail_reset(&strm->game_switcher_thumbnail);

   }

   gfx_display_deinit_white_texture();
}

static void streamlined_render(void *data, unsigned width, unsigned height, bool is_idle)
{
   streamlined_t *strm = (streamlined_t*)data;

   if (!strm)
      return;

   if (strm->loading_triggered)
   {
      streamlined_execute_deferred_load(strm);
      return;
   }

   if (strm->exiting_triggered)
   {
      streamlined_execute_deferred_exit(strm);
      return;
   }

   if (strm->width != width || strm->height != height)
   {
      strm->width = width;
      strm->height = height;
      strm->margin_x = (int)(width * STREAMLINED_MARGIN_RATIO);
      strm->margin_y = (int)(height * STREAMLINED_MARGIN_RATIO);
   }
}

static void streamlined_frame(void *data, video_frame_info_t *video_info)
{
   streamlined_t *strm = (streamlined_t*)data;
   gfx_display_t *p_disp = disp_get_ptr();
   void *userdata;
   unsigned video_width, video_height;

   if (!strm || !p_disp || !video_info)
      return;

   userdata = video_info->userdata;
   video_width = video_info->width;
   video_height = video_info->height;

   if (video_width == 0 || video_height == 0)
      return;

#if TARGET_OS_TV
   /* Fade to black during deferred search/keyboard transitions (tvOS only) */
   if (strm->enter_search_deferred || strm->return_to_options)
   {
      gfx_display_draw_quad(p_disp, userdata,
            video_width, video_height,
            0, 0, video_width, video_height,
            video_width, video_height,
            streamlined_color_black, NULL);
      return;
   }
#endif

   /* Loading screen: full black background + centered "Loading..." */
   if (strm->loading_pending)
   {
      gfx_display_draw_quad(p_disp, userdata,
            video_width, video_height,
            0, 0, video_width, video_height,
            video_width, video_height,
            streamlined_color_black, NULL);

      if (strm->font.font)
      {
         font_bind(&strm->font);
         gfx_display_draw_text(strm->font.font,
               "Loading...",
               (int)(video_width / 2),
               (int)(video_height / 2 + strm->font_size * 0.35f),
               video_width, video_height,
               streamlined_color_text,
               TEXT_ALIGN_CENTER, 1.0f, false, 0, false);
         font_flush(video_width, video_height, &strm->font);
      }

      strm->loading_triggered = true;
      return;
   }

   /* Exiting screen: full black background + centered "Exiting..." */
   if (strm->exiting_pending)
   {
      gfx_display_draw_quad(p_disp, userdata,
            video_width, video_height,
            0, 0, video_width, video_height,
            video_width, video_height,
            streamlined_color_black, NULL);

      if (strm->font.font)
      {
         settings_t *settings = config_get_ptr();
         bool auto_save       = settings && settings->bools.savestate_auto_save;

         font_bind(&strm->font);
         gfx_display_draw_text(strm->font.font,
               auto_save ? "Saving and Exiting..." : "Exiting...",
               (int)(video_width / 2),
               (int)(video_height / 2 + strm->font_size * 0.35f),
               video_width, video_height,
               streamlined_color_text,
               TEXT_ALIGN_CENTER, 1.0f, false, 0, false);
         font_flush(video_width, video_height, &strm->font);
      }

      strm->exiting_triggered = true;
      return;
   }

   /* Delete result screen — shown for ~3 seconds */
   if (strm->delete_done)
   {
      gfx_display_draw_quad(p_disp, userdata,
            video_width, video_height,
            0, 0, video_width, video_height,
            video_width, video_height,
            streamlined_color_black, NULL);

      if (strm->font.font)
      {
         font_bind(&strm->font);
         gfx_display_draw_text(strm->font.font,
               strm->delete_done_label,
               (int)(video_width / 2),
               (int)(video_height / 2 + strm->font_size * 0.35f),
               video_width, video_height,
               streamlined_color_text,
               TEXT_ALIGN_CENTER, 1.0f, false, 0, false);
         font_flush(video_width, video_height, &strm->font);
      }

      /* Advance ticker (normally done in render_menu which is skipped here) */
      strm->ticker_idx++;

      /* After ~3 seconds (180 frames at 60fps), return to game list */
      if (strm->ticker_idx - strm->delete_done_start > 180)
      {
         strm->delete_done = false;

         if (strm->delete_was_game)
         {
            /* Game was deleted — navigate based on context */
            if (strm->in_game_switcher)
            {
               playlist_t *history = g_defaults.content_history;
               if (!history || playlist_size(history) == 0)
               {
                  /* History empty — exit game switcher to top level */
                  strm->in_game_switcher = false;
                  {
                     settings_t *settings = config_get_ptr();
                     streamlined_populate_folder_menu(strm,
                           settings->paths.directory_menu_content, false);
                  }
                  if (strm->in_folder)
                     menu_state_get_ptr()->selection_ptr = strm->main_menu_selection;
                  else if (strm->in_favorites)
                     menu_state_get_ptr()->selection_ptr = strm->favorites_saved_selection;
                  else
                     menu_state_get_ptr()->selection_ptr = strm->game_switcher_saved_selection;
                  strm->in_folder = false;
                  strm->in_favorites = false;
                  strm->selected_has_savestate = false;
                  strm->selected_is_file = false;
                  gfx_thumbnail_reset(&strm->rom_thumbnail);
                  strm->rom_thumbnail_path[0] = '\0';
                  strm->rom_thumbnail_selection = (size_t)-1;
               }
               else
               {
                  if (strm->game_switcher_index >= playlist_size(history))
                     strm->game_switcher_index = playlist_size(history) - 1;
                  streamlined_refresh_game_switcher_view(strm);
               }
            }
            else if (strm->in_favorites)
            {
               playlist_t *fav = g_defaults.content_favorites;
               if (!fav || playlist_size(fav) == 0)
               {
                  /* Last favorite deleted — return to top level */
                  settings_t *settings = config_get_ptr();
                  strm->in_favorites = false;
                  strm->selected_has_savestate = false;
                  strm->selected_is_file = false;
                  gfx_thumbnail_reset(&strm->rom_thumbnail);
                  strm->rom_thumbnail_path[0] = '\0';
                  strm->rom_thumbnail_selection = (size_t)-1;
                  streamlined_populate_folder_menu(strm,
                        settings->paths.directory_menu_content, false);
                  menu_state_get_ptr()->selection_ptr = 0;
               }
               else
               {
                  streamlined_populate_favorites_menu(strm);
                  strm->rom_thumbnail_selection = (size_t)-1;
                  if (strm->options_saved_selection >= playlist_size(fav))
                     menu_state_get_ptr()->selection_ptr = playlist_size(fav) - 1;
                  else
                     menu_state_get_ptr()->selection_ptr = strm->options_saved_selection;
               }
            }
            else if (strm->options_was_in_folder)
            {
               streamlined_populate_folder_menu(strm, strm->options_folder_path, true);
               {
                  struct menu_state *ms = menu_state_get_ptr();
                  menu_list_t *ml = ms->entries.list;
                  file_list_t *fl = ml ? MENU_LIST_GET_SELECTION(ml, 0) : NULL;
                  size_t count = fl ? fl->size : 0;
                  if (strm->options_saved_selection >= count && count > 0)
                     ms->selection_ptr = count - 1;
                  else
                     ms->selection_ptr = strm->options_saved_selection;
               }
            }
            else
            {
               settings_t *settings = config_get_ptr();
               streamlined_populate_folder_menu(strm, settings->paths.directory_menu_content, false);
               {
                  struct menu_state *ms = menu_state_get_ptr();
                  menu_list_t *ml = ms->entries.list;
                  file_list_t *fl = ml ? MENU_LIST_GET_SELECTION(ml, 0) : NULL;
                  size_t count = fl ? fl->size : 0;
                  if (strm->options_saved_selection >= count && count > 0)
                     ms->selection_ptr = count - 1;
                  else
                     ms->selection_ptr = strm->options_saved_selection;
               }
            }
         }
         else
         {
            /* Autosave was deleted — existing behavior */
            if (strm->in_game_switcher)
            {
               strm->game_switcher_has_savestate = false;
               streamlined_refresh_game_switcher_view(strm);
            }
            else if (strm->in_favorites)
               streamlined_populate_favorites_menu(strm);
            else if (strm->options_was_in_folder)
               streamlined_populate_folder_menu(strm, strm->options_folder_path, true);
            else
            {
               settings_t *settings = config_get_ptr();
               streamlined_populate_folder_menu(strm, settings->paths.directory_menu_content, false);
            }
            if (!strm->in_game_switcher)
               menu_state_get_ptr()->selection_ptr = strm->options_saved_selection;
         }
      }
      return;
   }

   if (!strm->font.font)
      return;

   if (strm->margin_x == 0 || strm->margin_y == 0)
   {
      strm->margin_x = (int)(video_width * STREAMLINED_MARGIN_RATIO);
      strm->margin_y = (int)(video_height * STREAMLINED_MARGIN_RATIO);
      if (strm->margin_x < 10) strm->margin_x = 10;
      if (strm->margin_y < 10) strm->margin_y = 10;
   }

   font_bind(&strm->font);
   if (strm->font_small.font)
      font_bind(&strm->font_small);
   if (strm->font_title.font)
      font_bind(&strm->font_title);
   if (strm->font_tiny.font)
      font_bind(&strm->font_tiny);

   streamlined_draw_bg(strm, p_disp, userdata, video_width, video_height);

   if (strm->in_delete_confirm)
   {
#if !TARGET_OS_TV
      /* Confirmation view: centered text + footer with Back/Delete */
      const char *header = strm->delete_is_game ? "Delete Game" : "Delete Autosave";
      int header_w = streamlined_get_title_width(strm, header);
      int header_x = ((int)video_width - header_w) / 2;
      int header_y = strm->margin_y + (int)(strm->font_size_title * 0.9f);

      streamlined_draw_title(strm, p_disp, video_width, video_height,
            header_x, header_y, header, streamlined_color_text);

      /* Game name centered */
      {
         float footer_height = 78.0f * strm->scale_factor;
         int avail_top = strm->margin_y + (int)(strm->font_size_title * 1.4f);
         int avail_bottom = (int)(video_height - footer_height);
         int center_y = (avail_top + avail_bottom) / 2
               + (int)(strm->font_size * 0.35f);
         int max_w = (int)video_width - strm->margin_x * 2;
         char truncated[256];
         char display_name[256];

         streamlined_get_display_name(strm->options_game_path,
               display_name, sizeof(display_name), false);

         streamlined_truncate_text(strm, display_name,
               truncated, sizeof(truncated), max_w, FONT_NORMAL);

         {
            int name_w = streamlined_get_text_width(strm, truncated, FONT_NORMAL);
            int name_x = ((int)video_width - name_w) / 2;
            streamlined_draw_text(strm, p_disp, video_width, video_height,
                  name_x, center_y, truncated, streamlined_color_text, false);
         }

         /* Preservation note for game deletion */
         if (strm->delete_is_game && strm->font_small.font)
         {
            const char *note = "Saves, save states, and thumbnails will not be deleted.";
            int note_w = font_driver_get_message_width(
                  strm->font_small.font, note, strlen(note), 1.0f);
            int note_x = ((int)video_width - note_w) / 2;
            int note_y = center_y + (int)(strm->font_size * 1.2f);
            gfx_display_draw_text(strm->font_small.font,
                  note, note_x, note_y,
                  video_width, video_height, streamlined_color_text_muted,
                  TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
         }
      }

      /* Footer: [B] Back ... [A] Delete */
      {
         float scale = strm->scale_factor;
         float footer_height = 78.0f * scale;
         float footer_margin = 40.0f * scale;
         float pill_h = strm->font_size_small + 8.0f * scale;
         float pill_pad = 10.0f * scale;
         float pill_text_gap = 8.0f * scale;
         float footer_center_y = (float)video_height - (footer_height / 2.0f);
         float pill_y = footer_center_y - (pill_h / 2.0f);
         float text_y = footer_center_y + (strm->font_size_small * 0.35f);

         const char *back_key = "B";
         const char *ok_key = "A";
         const char *back_str = msg_hash_to_str(
               MENU_ENUM_LABEL_VALUE_BASIC_MENU_CONTROLS_BACK);
         const char *ok_str = "Delete";

         int back_key_w = font_driver_get_message_width(
               strm->font_small.font, back_key, strlen(back_key), 1.0f);
         int ok_key_w = font_driver_get_message_width(
               strm->font_small.font, ok_key, strlen(ok_key), 1.0f);
         int back_pill_w = back_key_w + (int)(pill_pad * 2.0f);
         int ok_pill_w = ok_key_w + (int)(pill_pad * 2.0f);
         int ok_label_w = font_driver_get_message_width(
               strm->font_small.font, ok_str, strlen(ok_str), 1.0f);
         float right_x = (float)video_width - footer_margin;
         float ok_pill_x = right_x - (float)ok_label_w - pill_text_gap - (float)ok_pill_w;

         /* Left: [B] Back */
         streamlined_draw_rounded_pill(strm, p_disp, userdata,
               (int)footer_margin, (int)pill_y, back_pill_w, (int)pill_h,
               video_width, video_height, streamlined_color_selection);
         gfx_display_draw_text(strm->font_small.font,
               back_key, (int)(footer_margin + pill_pad), (int)text_y,
               video_width, video_height, streamlined_color_text_dark,
               TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
         gfx_display_draw_text(strm->font_small.font,
               back_str,
               (int)(footer_margin + (float)back_pill_w + pill_text_gap),
               (int)text_y,
               video_width, video_height, streamlined_color_text,
               TEXT_ALIGN_LEFT, 1.0f, false, 0, false);

         /* Right: [A] Delete */
         streamlined_draw_rounded_pill(strm, p_disp, userdata,
               (int)ok_pill_x, (int)pill_y, ok_pill_w, (int)pill_h,
               video_width, video_height, streamlined_color_selection);
         gfx_display_draw_text(strm->font_small.font,
               ok_key, (int)(ok_pill_x + pill_pad), (int)text_y,
               video_width, video_height, streamlined_color_text_dark,
               TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
         gfx_display_draw_text(strm->font_small.font,
               ok_str,
               (int)(ok_pill_x + (float)ok_pill_w + pill_text_gap),
               (int)text_y,
               video_width, video_height, streamlined_color_text,
               TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
      }
#endif
   }
   else if (strm->in_game_switcher && !strm->game_switcher_in_glo)
      streamlined_render_game_switcher(strm, p_disp, userdata,
            video_width, video_height);
   else if (strm->in_random_preview)
      streamlined_render_random_preview(strm, p_disp, userdata,
            video_width, video_height);
   else
      streamlined_render_menu(strm, p_disp, userdata, video_width, video_height);

   if (strm->font.font)
      font_flush(video_width, video_height, &strm->font);
   if (strm->font_small.font)
      font_flush(video_width, video_height, &strm->font_small);
   if (strm->font_title.font)
      font_flush(video_width, video_height, &strm->font_title);
   if (strm->font_tiny.font)
      font_flush(video_width, video_height, &strm->font_tiny);
}

static void streamlined_populate_entries(void *data,
      const char *path, const char *label, unsigned k)
{
   streamlined_t *strm = (streamlined_t*)data;
   const char *content_settings_label = msg_hash_to_str(MENU_ENUM_LABEL_CONTENT_SETTINGS);
   const char *main_menu_label = msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU);
   bool is_content_settings = false;
   bool is_main_menu = false;

   if (!strm)
      return;

   /* Check what menu we're in */
   if (label)
   {
      if (content_settings_label && string_is_equal(label, content_settings_label))
         is_content_settings = true;
      else if (main_menu_label && string_is_equal(label, main_menu_label))
         is_main_menu = true;

      /* Also check enum_idx from the menu stack */
      if (!is_content_settings && !is_main_menu
            && !strm->return_to_main_settings_submenu)
      {
         struct menu_state *menu_st = menu_state_get_ptr();
         if (menu_st && menu_st->entries.list)
         {
            enum msg_hash_enums enum_idx = MSG_UNKNOWN;
            menu_entries_get_last_stack(NULL, NULL, NULL, &enum_idx, NULL);
            if (enum_idx == MENU_ENUM_LABEL_CONTENT_SETTINGS)
               is_content_settings = true;
            else if (enum_idx == MENU_ENUM_LABEL_MAIN_MENU)
               is_main_menu = true;
         }
      }

      /* Handle main menu - show folders from start directory */
      if (is_main_menu)
      {
         settings_t *settings = config_get_ptr();
         const char *start_dir = settings->paths.directory_menu_content;
         struct menu_state *menu_st_local = menu_state_get_ptr();

         if (!string_is_empty(start_dir))
         {
            /* Check if returning to main settings submenu */
            if (strm->return_to_main_settings_submenu)
            {
               streamlined_populate_main_settings_submenu();
               strm->is_custom_main_menu = true;
               strm->in_main_settings_submenu = true;
               strm->return_to_main_settings_submenu = false;
               strm->in_folder = false;
               /* Restore selection in settings submenu */
               if (menu_st_local)
                  menu_st_local->selection_ptr = strm->saved_settings_selection;
            }
            /* Check if returning from a top-level M3U game */
            else if (strm->return_to_top_level)
            {
               streamlined_populate_folder_menu(strm, start_dir, false);
               strlcpy(strm->current_folder_path, start_dir,
                     sizeof(strm->current_folder_path));
               strm->is_custom_main_menu = true;
               strm->in_folder = false;
               strm->return_to_top_level = false;
               gfx_thumbnail_reset(&strm->rom_thumbnail);
               strm->rom_thumbnail_path[0] = '\0';
               strm->rom_thumbnail_selection = (size_t)-1;
               if (menu_st_local)
                  menu_st_local->selection_ptr = strm->top_level_selection;
            }
            /* Check if returning from a game - restore folder state */
            else if (strm->return_to_folder && !string_is_empty(strm->last_launched_folder))
            {
               streamlined_populate_folder_menu(strm, strm->last_launched_folder, true);
               strlcpy(strm->current_folder_path, strm->last_launched_folder,
                     sizeof(strm->current_folder_path));
               /* Restore the folder's core path */
               strlcpy(strm->folder_core_path, strm->last_folder_core_path,
                     sizeof(strm->folder_core_path));
               strm->is_custom_main_menu = true;
               strm->in_folder = true;
               strm->return_to_folder = false;
               gfx_thumbnail_reset(&strm->rom_thumbnail);
               strm->rom_thumbnail_path[0] = '\0';
               strm->rom_thumbnail_selection = (size_t)-1;
               /* Restore selection to the game that was played */
               if (menu_st_local)
                  menu_st_local->selection_ptr = strm->folder_selection;

            }
            else
            {
               streamlined_populate_folder_menu(strm, start_dir, false);  /* Top level - no slash */
               strlcpy(strm->current_folder_path, start_dir,
                     sizeof(strm->current_folder_path));
               strm->is_custom_main_menu = true;
               strm->in_folder = false;
               strm->rom_thumbnail_selection = (size_t)-1;
            }
         }
         strm->is_quick_menu = false;
         strm->in_settings_submenu = false;
         return;
      }

      if (is_content_settings)
      {
         /* Check if we should return to the Advanced settings submenu */
         if (strm->return_to_settings_submenu)
         {
            struct menu_state *menu_st = menu_state_get_ptr();
            streamlined_populate_settings_submenu();
            strm->in_settings_submenu = true;
            strm->return_to_settings_submenu = false;
            if (menu_st)
               menu_st->selection_ptr = strm->saved_advanced_selection;
         }
         else
         {
            streamlined_populate_quick_menu();
            strm->in_settings_submenu = false;
         }
         strm->is_quick_menu = true;
         strm->is_custom_main_menu = false;

         /*
          * Reset thumbnail state when entering quick menu so it reloads.
          * This ensures the thumbnail is refreshed (e.g., if a new screenshot
          * was taken since last viewing).
          */
         gfx_thumbnail_reset(&strm->savestate_thumbnail);
         strm->savestate_thumbnail_path[0] = '\0';
         strm->last_selection = (size_t)-1;  /* Force reload on next render */
      }
      else
      {
         /* Don't reset return_to_settings_submenu here - we need it when coming back */
         strm->is_quick_menu = false;
         strm->in_settings_submenu = false;
         strm->is_custom_main_menu = false;
      }
   }
   else
   {
      /* Don't reset return_to_settings_submenu here - we need it when coming back */
      strm->is_quick_menu = false;
      strm->in_settings_submenu = false;
      strm->is_custom_main_menu = false;
   }
}

static void streamlined_navigation_set(void *data, bool scroll) { }
static void streamlined_navigation_clear(void *data, bool pending_push) { }
static void streamlined_navigation_set_last(void *data) { }

static int streamlined_pointer_up(void *data,
      unsigned x, unsigned y, unsigned ptr,
      enum menu_input_pointer_gesture gesture,
      menu_file_list_cbs_t *cbs,
      menu_entry_t *entry, unsigned action)
{
   return 0;
}

static int streamlined_environ(enum menu_environ_cb type, void *data, void *userdata)
{
   return -1;
}

/*
 * Find a menu entry by enum_idx and set the selection pointer to it.
 */
static void streamlined_select_options_entry(unsigned target_enum)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   file_list_t *slist;
   size_t idx = 0;

   if (!menu_st)
      return;

   slist = MENU_LIST_GET_SELECTION(menu_st->entries.list, 0);
   if (slist)
   {
      size_t j;
      for (j = 0; j < slist->size; j++)
      {
         menu_entry_t e;
         MENU_ENTRY_INITIALIZE(e);
         menu_entry_get(&e, 0, (unsigned)j, NULL, true);
         if (e.enum_idx == target_enum)
         {
            idx = j;
            break;
         }
      }
   }
   menu_st->selection_ptr = idx;
}

/*
 * Custom input handler for Cannoli menu navigation.
 *
 * Navigation state machine:
 *
 *   [Game Running] ---(menu button)---> [Main Quick Menu]
 *         ^                                    |
 *         |                                    v
 *         +----(B: back)----+          [Advanced]
 *                           |                  |
 *                           |                  v
 *                           +--------  [RA Settings Screen]
 *                                             |
 *                                      (B: back to Advanced)
 *
 * Key behaviors:
 * - B in main quick menu: closes menu and resumes game
 * - B in Advanced submenu: returns to main quick menu
 * - B in RA settings (entered from Advanced): returns to Advanced
 * - A on "Advanced": enters the custom settings submenu
 * - A on any other item: executes the associated RetroArch action
 */
static int streamlined_entry_action(void *userdata, menu_entry_t *entry,
      size_t i, enum menu_action action)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   streamlined_t *strm = NULL;

   if (menu_st)
      strm = (streamlined_t*)menu_st->userdata;

   /* Absorb lingering cancel events after state transitions */
   if (strm && strm->cancel_ignore_frames > 0)
   {
      strm->cancel_ignore_frames--;
      if (action == MENU_ACTION_CANCEL)
         return 0;
   }

   /* Block all input while loading/exiting screen is showing */
   if (strm && (strm->loading_pending || strm->exiting_pending))
      return 0;

   /* ================================================================
    * GAME SWITCHER INPUT HANDLING
    * ================================================================ */

   /* Game Switcher GLO menu */
   if (strm && strm->in_game_switcher && strm->game_switcher_in_glo)
   {
      settings_t *gs_settings = config_get_ptr();
      unsigned gs_view = gs_settings ? gs_settings->uints.menu_streamlined_game_switcher_view : 0;

      if (action == MENU_ACTION_CANCEL)
      {
         strm->game_switcher_in_glo = false;
         strm->in_options_menu = false;
         streamlined_refresh_game_switcher_view(strm);
         strm->cancel_ignore_frames = 3;
         return 0;
      }

      if (action == MENU_ACTION_OK && entry)
      {
         if (entry->enum_idx == STREAMLINED_OPTIONS_REMOVE_FROM_SWITCHER)
         {
            playlist_t *history = g_defaults.content_history;
            if (history)
            {
               playlist_delete_index(history, strm->game_switcher_index);
               playlist_write_file(history);
            }
            strm->game_switcher_in_glo = false;
            strm->in_options_menu = false;

            /* Check if history is now empty */
            if (!history || playlist_size(history) == 0)
            {
               strm->in_game_switcher = false;
               {
                  settings_t *settings = config_get_ptr();
                  streamlined_populate_folder_menu(strm,
                        settings->paths.directory_menu_content, false);
               }
               if (strm->in_folder)
                  menu_st->selection_ptr = strm->main_menu_selection;
               else if (strm->in_favorites)
                  menu_st->selection_ptr = strm->favorites_saved_selection;
               else
                  menu_st->selection_ptr = strm->game_switcher_saved_selection;
               strm->in_folder = false;
               strm->in_favorites = false;
               strm->selected_has_savestate = false;
               strm->selected_is_file = false;
               gfx_thumbnail_reset(&strm->rom_thumbnail);
               strm->rom_thumbnail_path[0] = '\0';
               strm->rom_thumbnail_selection = (size_t)-1;
               return 0;
            }

            /* Adjust index if past end */
            if (strm->game_switcher_index >= playlist_size(history))
               strm->game_switcher_index = playlist_size(history) - 1;

            if (gs_view == 0)
               streamlined_load_game_switcher_entry(strm);
            else
            {
               streamlined_populate_game_switcher_menu(strm);
               if (strm->game_switcher_index < playlist_size(history))
                  menu_st->selection_ptr = strm->game_switcher_index;
               else
                  menu_st->selection_ptr = 0;
            }
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_RESET_GAME)
         {
            const char *eff_core = streamlined_effective_core(strm);
            streamlined_delete_autosave_file(
                  strm->options_game_path, eff_core);
            strm->game_switcher_in_glo = false;
            strm->in_options_menu = false;
            strm->in_game_switcher = false;

            strm->return_to_top_level = true;
            strm->top_level_selection = strm->game_switcher_saved_selection;

            streamlined_request_loading(strm,
                  eff_core, strm->options_game_path, false);
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_DELETE_SAVE)
         {
            strm->in_delete_confirm = true;
            strm->delete_is_game = false;
            strm->game_switcher_in_glo = false;
            strm->in_options_menu = false;
#if TARGET_OS_TV
            {
               char display_name[256];
               streamlined_get_display_name(strm->options_game_path,
                     display_name, sizeof(display_name), false);
               ios_show_confirm_dialog(
                     "Delete Autosave", display_name, "Delete",
                     streamlined_delete_confirm_cb, strm);
            }
#endif
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_DELETE_GAME)
         {
            strm->in_delete_confirm = true;
            strm->delete_is_game = true;
            strm->game_switcher_in_glo = false;
            strm->in_options_menu = false;
#if TARGET_OS_TV
            {
               char display_name[256];
               char dialog_msg[512];
               streamlined_get_display_name(strm->options_game_path,
                     display_name, sizeof(display_name), false);
               snprintf(dialog_msg, sizeof(dialog_msg),
                     "%s\n\nSaves, save states, and thumbnails will not be deleted.",
                     display_name);
               ios_show_confirm_dialog(
                     "Delete Game", dialog_msg, "Delete",
                     streamlined_delete_confirm_cb, strm);
            }
#endif
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_SET_GAME_CORE)
         {
            strm->selecting_core_for_game = true;
            strm->game_switcher_in_glo = false;
            strm->in_options_menu = false;
            streamlined_populate_core_selection(strm, strm->options_game_path);

            if (!string_is_empty(strm->options_game_core_path))
            {
               file_list_t *clist = MENU_LIST_GET_SELECTION(menu_st->entries.list, 0);
               if (clist)
                  menu_entries_prepend(clist,
                        "Use Folder Default", "",
                        STREAMLINED_OPTIONS_CLEAR_GAME_CORE,
                        MENU_SETTING_ACTION, 0, 0);
            }
            menu_st->selection_ptr = 0;
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_ADD_FAVORITE)
         {
            const char *eff_core = streamlined_effective_core(strm);
            streamlined_add_to_favorites(strm->options_game_path, eff_core);
            strm->game_switcher_in_glo = false;
            strm->in_options_menu = false;
            streamlined_refresh_game_switcher_view(strm);
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_REMOVE_FAVORITE)
         {
            streamlined_remove_from_favorites(strm->options_game_path);
            strm->game_switcher_in_glo = false;
            strm->in_options_menu = false;
            streamlined_refresh_game_switcher_view(strm);
            return 0;
         }
      }

      /* Allow navigation in GLO */
      if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN
            || action == MENU_ACTION_SCROLL_UP || action == MENU_ACTION_SCROLL_DOWN)
         return generic_menu_entry_action(userdata, entry, i, action);

      return 0;
   }

   /* Image-centric Game Switcher */
   if (strm && strm->in_game_switcher && !strm->game_switcher_in_glo)
   {
      settings_t *gs_settings = config_get_ptr();
      unsigned gs_view = gs_settings ? gs_settings->uints.menu_streamlined_game_switcher_view : 0;

      if (gs_view == 0)
      {
         /* Image-centric view input */
         playlist_t *history = g_defaults.content_history;
         size_t hist_size = history ? playlist_size(history) : 0;

         if (action == MENU_ACTION_CANCEL || action == MENU_ACTION_INFO)
         {
            gfx_thumbnail_reset(&strm->game_switcher_thumbnail);
            strm->in_game_switcher = false;
            {
               settings_t *settings = config_get_ptr();
               streamlined_populate_folder_menu(strm,
                     settings->paths.directory_menu_content, false);
            }
            if (strm->in_folder)
               menu_st->selection_ptr = strm->main_menu_selection;
            else if (strm->in_favorites)
               menu_st->selection_ptr = strm->favorites_saved_selection;
            else
               menu_st->selection_ptr = strm->game_switcher_saved_selection;
            strm->in_folder = false;
            strm->in_favorites = false;
            strm->selected_has_savestate = false;
            strm->selected_is_file = false;
            gfx_thumbnail_reset(&strm->rom_thumbnail);
            strm->rom_thumbnail_path[0] = '\0';
            strm->rom_thumbnail_selection = (size_t)-1;
            return 0;
         }

         if (action == MENU_ACTION_LEFT && hist_size > 0)
         {
            if (strm->game_switcher_index == 0)
               strm->game_switcher_index = hist_size - 1;
            else
               strm->game_switcher_index--;
            streamlined_load_game_switcher_entry(strm);
            return 0;
         }

         if (action == MENU_ACTION_RIGHT && hist_size > 0)
         {
            strm->game_switcher_index++;
            if (strm->game_switcher_index >= hist_size)
               strm->game_switcher_index = 0;
            streamlined_load_game_switcher_entry(strm);
            return 0;
         }

         if (action == MENU_ACTION_OK && hist_size > 0)
         {
            if (!string_is_empty(strm->game_switcher_core_path)
                  && !string_is_empty(strm->game_switcher_content_path))
            {
               gfx_thumbnail_reset(&strm->game_switcher_thumbnail);
               strm->in_game_switcher = false;
               strm->return_to_top_level = true;
               strm->top_level_selection = strm->game_switcher_saved_selection;

               streamlined_request_loading(strm,
                     strm->game_switcher_core_path,
                     strm->game_switcher_content_path,
                     strm->game_switcher_has_savestate);
            }
            return 0;
         }

         /* Y = Open GLO */
         if (action == MENU_ACTION_SEARCH && hist_size > 0)
         {
            char folder_path[PATH_MAX_LENGTH];

            /* Set up options fields from game switcher state */
            strlcpy(strm->options_game_path, strm->game_switcher_content_path,
                  sizeof(strm->options_game_path));
            strm->options_game_has_savestate = strm->game_switcher_has_savestate;
            strm->options_was_in_folder = false;

            /* Resolve core and per-game core override */
            streamlined_resolve_content_core(strm->game_switcher_content_path,
                  strm->options_core_path, sizeof(strm->options_core_path),
                  strm->options_game_core_path, sizeof(strm->options_game_core_path),
                  g_defaults.content_history);

            streamlined_get_content_folder_path(strm->game_switcher_content_path,
                  folder_path, sizeof(folder_path));
            strlcpy(strm->options_folder_path, folder_path,
                  sizeof(strm->options_folder_path));

            strm->game_switcher_in_glo = true;
            strm->in_options_menu = true;
            streamlined_populate_options_menu(strm, false, true);
            menu_st->selection_ptr = 0;
            return 0;
         }

         return 0;
      }
      else
      {
         /* Text view input */
         if (action == MENU_ACTION_CANCEL)
         {
            strm->in_game_switcher = false;
            {
               settings_t *settings = config_get_ptr();
               streamlined_populate_folder_menu(strm,
                     settings->paths.directory_menu_content, false);
            }
            if (strm->in_folder)
               menu_st->selection_ptr = strm->main_menu_selection;
            else if (strm->in_favorites)
               menu_st->selection_ptr = strm->favorites_saved_selection;
            else
               menu_st->selection_ptr = strm->game_switcher_saved_selection;
            strm->in_folder = false;
            strm->in_favorites = false;
            strm->selected_has_savestate = false;
            strm->selected_is_file = false;
            gfx_thumbnail_reset(&strm->rom_thumbnail);
            strm->rom_thumbnail_path[0] = '\0';
            strm->rom_thumbnail_selection = (size_t)-1;
            return 0;
         }

         if (action == MENU_ACTION_OK && entry)
         {
            const char *item_path = entry->label;
            if (!string_is_empty(item_path) && path_is_valid(item_path))
            {
               char gs_core[PATH_MAX_LENGTH];
               bool has_core = streamlined_resolve_content_core(
                     item_path,
                     gs_core, sizeof(gs_core),
                     NULL, 0,
                     g_defaults.content_history);

               if (has_core)
               {
                  bool has_save = streamlined_check_savestate(item_path, gs_core);
                  strm->in_game_switcher = false;
                  strm->return_to_top_level = true;
                  strm->top_level_selection = strm->game_switcher_saved_selection;
                  streamlined_request_loading(strm,
                        gs_core, item_path, has_save);
               }
            }
            return 0;
         }

         /* Y = Open GLO with current entry's data */
         if (action == MENU_ACTION_SEARCH && entry)
         {
            const char *item_path = entry->label;
            if (!string_is_empty(item_path) && path_is_valid(item_path))
            {
               char folder_path[PATH_MAX_LENGTH];

               strlcpy(strm->options_game_path, item_path,
                     sizeof(strm->options_game_path));
               strm->options_was_in_folder = false;

               /* Resolve core and per-game core override */
               streamlined_resolve_content_core(item_path,
                     strm->options_core_path, sizeof(strm->options_core_path),
                     strm->options_game_core_path, sizeof(strm->options_game_core_path),
                     g_defaults.content_history);

               streamlined_get_content_folder_path(item_path,
                     folder_path, sizeof(folder_path));
               strlcpy(strm->options_folder_path, folder_path,
                     sizeof(strm->options_folder_path));

               strm->options_game_has_savestate = !string_is_empty(strm->options_core_path)
                     && streamlined_check_savestate(item_path, strm->options_core_path);

               strm->game_switcher_index = menu_st->selection_ptr;
               strm->game_switcher_in_glo = true;
               strm->in_options_menu = true;
               streamlined_populate_options_menu(strm, false, true);
               menu_st->selection_ptr = 0;
            }
            return 0;
         }

         /* Allow normal list navigation */
         if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN
               || action == MENU_ACTION_SCROLL_UP || action == MENU_ACTION_SCROLL_DOWN)
            return generic_menu_entry_action(userdata, entry, i, action);

         return 0;
      }
   }

   if (strm && strm->is_quick_menu)
   {
      /* Handle input for save slot selection when on Save/Load entry */
      if (strm->show_slot_selector)
      {
         settings_t *settings = config_get_ptr();
         struct menu_state *menu_state = menu_state_get_ptr();
         size_t selection = menu_state ? menu_state->selection_ptr : 0;

         /*
          * Slot mapping: preview_slot 0 = Auto (state_slot -1)
          *               preview_slot 1-8 = state_slot 0-7
          * So: state_slot = preview_slot - 1
          */
         if (action == MENU_ACTION_LEFT)
         {
            strm->preview_slot--;
            if (strm->preview_slot < 0)
               strm->preview_slot = STREAMLINED_NUM_SLOTS - 1;
            settings->ints.state_slot = strm->preview_slot - 1;
            streamlined_load_slot_thumbnail(strm, strm->preview_slot);
            return 0;  /* Consume input */
         }

         if (action == MENU_ACTION_RIGHT)
         {
            strm->preview_slot++;
            if (strm->preview_slot >= STREAMLINED_NUM_SLOTS)
               strm->preview_slot = 0;
            settings->ints.state_slot = strm->preview_slot - 1;
            streamlined_load_slot_thumbnail(strm, strm->preview_slot);
            return 0;  /* Consume input */
         }

         /* A or Start button executes save/load action and closes menu */
         if (action == MENU_ACTION_OK || action == MENU_ACTION_START)
         {
            /* selection 1 = Save, selection 2 = Load */
            if (selection == 1)
               command_event(CMD_EVENT_SAVE_STATE, NULL);
            else if (selection == 2)
               command_event(CMD_EVENT_LOAD_STATE, NULL);
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            return 0;
         }
      }

      /* Back button in Advanced submenu: return to main quick menu */
      if (action == MENU_ACTION_CANCEL && strm->in_settings_submenu)
      {
         strm->in_settings_submenu = false;
         strm->return_to_settings_submenu = false;
         streamlined_populate_quick_menu();
         menu_st->selection_ptr = strm->saved_quick_menu_selection;
         return 0;
      }

      /* Back button in main quick menu: close menu entirely and resume game */
      if (action == MENU_ACTION_CANCEL && !strm->in_settings_submenu)
      {
         command_event(CMD_EVENT_MENU_TOGGLE, NULL);
         return 0;
      }

      /* Select "Advanced" entry: enter the settings submenu */
      if (action == MENU_ACTION_OK && entry && !strm->in_settings_submenu)
      {
         const char *entry_label = NULL;

         if (!string_is_empty(entry->rich_label))
            entry_label = entry->rich_label;
         else if (!string_is_empty(entry->path))
            entry_label = entry->path;

         if (entry_label && string_is_equal(entry_label, "Advanced"))
         {
            strm->saved_quick_menu_selection = menu_st->selection_ptr;
            strm->in_settings_submenu = true;
            strm->return_to_settings_submenu = false;
            streamlined_populate_settings_submenu();
            menu_st->selection_ptr = 0;
            return 0;
         }

         /* Handle Quit / Save and Quit - show exiting screen, then quit or return to main menu */
         if (entry->enum_idx == MENU_ENUM_LABEL_QUIT_RETROARCH
               || entry->enum_idx == MENU_ENUM_LABEL_CLOSE_CONTENT)
         {
            strm->exiting_is_cli = streamlined_is_launched_from_cli();
            strm->exiting_pending = true;
            strm->exiting_triggered = false;
            return 0;
         }
      }

      /*
       * When entering an RA settings screen from Advanced submenu,
       * set flag to return to Advanced (not main menu) when backing out.
       */
      if (action == MENU_ACTION_OK && strm->in_settings_submenu)
      {
         strm->saved_advanced_selection = menu_st->selection_ptr;
         strm->return_to_settings_submenu = true;
      }

      /* Block X button in quick menu - Resume doesn't apply here */
      if (action == MENU_ACTION_SCAN)
         return 0;
   }

   /* Handle core selection mode */
   if (strm && strm->selecting_core)
   {
      /* Cancel core selection - go back to folder or favorites */
      if (action == MENU_ACTION_CANCEL)
      {
         strm->selecting_core = false;
         strm->pending_content_path[0] = '\0';
         if (strm->in_favorites)
         {
            strm->rom_thumbnail_selection = (size_t)-1;
            streamlined_populate_favorites_menu(strm);
         }
         else
            streamlined_populate_folder_menu(strm, strm->current_folder_path, true);
         return 0;
      }

      /* Core selected - save to .core.txt and launch content */
      if (action == MENU_ACTION_OK && entry)
      {
         const char *selected_core = entry->label;  /* Core path is in label */

         if (!string_is_empty(selected_core) && path_is_valid(selected_core))
         {
            if (strm->in_favorites)
            {
               /* From favorites — just launch, don't save folder core */
               streamlined_exit_favorites(strm);
               streamlined_request_loading(strm,
                     selected_core, strm->pending_content_path, false);
               return 0;
            }

            /* Save the selected core to .core.txt for this folder */
            streamlined_save_folder_core(strm->current_folder_path, selected_core);

            /* Update the folder's core path */
            strlcpy(strm->folder_core_path, selected_core,
                  sizeof(strm->folder_core_path));

            /* Save folder state so we can return after quitting */
            strm->folder_selection = menu_st->selection_ptr;
            strlcpy(strm->last_launched_folder, strm->current_folder_path,
                  sizeof(strm->last_launched_folder));
            strlcpy(strm->last_folder_core_path, strm->folder_core_path,
                  sizeof(strm->last_folder_core_path));
            strm->return_to_folder = true;

            streamlined_request_loading(strm,
                  selected_core, strm->pending_content_path, false);

            return 0;
         }
      }

      /* Allow navigation (up/down) - pass to generic handler */
      if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN ||
          action == MENU_ACTION_SCROLL_UP || action == MENU_ACTION_SCROLL_DOWN)
      {
         return generic_menu_entry_action(userdata, entry, i, action);
      }

      return 0;  /* Block other actions during core selection */
   }

   /* Handle "Set Folder Core" selection */
   if (strm && strm->selecting_core_for_folder)
   {
      if (action == MENU_ACTION_CANCEL)
      {
         strm->selecting_core_for_folder = false;
         strm->in_options_menu = true;
         strm->cancel_ignore_frames = 3;
         streamlined_populate_options_menu(strm, strm->in_favorites, false);
         streamlined_select_options_entry(STREAMLINED_OPTIONS_SET_FOLDER_CORE);
         return 0;
      }

      if (action == MENU_ACTION_OK && entry)
      {
         const char *selected_core = entry->label;
         if (!string_is_empty(selected_core) && path_is_valid(selected_core))
         {
            char core_folder[PATH_MAX_LENGTH];

            if (strm->options_was_in_folder)
               strlcpy(core_folder, strm->options_folder_path, sizeof(core_folder));
            else
               fill_pathname_parent_dir(core_folder, strm->options_game_path, sizeof(core_folder));

            streamlined_save_folder_core(core_folder, selected_core);
            strlcpy(strm->options_core_path, selected_core,
                  sizeof(strm->options_core_path));
            strlcpy(strm->folder_core_path, selected_core,
                  sizeof(strm->folder_core_path));

            strm->selecting_core_for_folder = false;
            strm->in_options_menu = true;
            strm->cancel_ignore_frames = 3;
            streamlined_populate_options_menu(strm, strm->in_favorites, false);
            streamlined_select_options_entry(STREAMLINED_OPTIONS_SET_FOLDER_CORE);
            return 0;
         }
      }

      if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN
            || action == MENU_ACTION_SCROLL_UP || action == MENU_ACTION_SCROLL_DOWN)
         return generic_menu_entry_action(userdata, entry, i, action);

      return 0;
   }

   /* Handle "Set Game Core" selection */
   if (strm && strm->selecting_core_for_game)
   {
      if (action == MENU_ACTION_CANCEL)
      {
         strm->selecting_core_for_game = false;
         strm->in_options_menu = true;
         strm->cancel_ignore_frames = 3;
         streamlined_populate_options_menu(strm, strm->in_favorites, false);
         streamlined_select_options_entry(STREAMLINED_OPTIONS_SET_GAME_CORE);
         return 0;
      }

      if (action == MENU_ACTION_OK && entry)
      {
         /* "Use Folder Default" clears the per-game override */
         if (entry->enum_idx == STREAMLINED_OPTIONS_CLEAR_GAME_CORE)
         {
            char core_folder[PATH_MAX_LENGTH];
            char game_name[PATH_MAX_LENGTH];

            if (strm->options_was_in_folder)
               strlcpy(core_folder, strm->options_folder_path, sizeof(core_folder));
            else
               fill_pathname_parent_dir(core_folder, strm->options_game_path, sizeof(core_folder));

            streamlined_get_game_core_name(strm->options_game_path, core_folder,
                  game_name, sizeof(game_name));
            streamlined_delete_game_core(core_folder, game_name);
            strm->options_game_core_path[0] = '\0';

            strm->selecting_core_for_game = false;
            strm->in_options_menu = true;
            strm->cancel_ignore_frames = 3;
            streamlined_populate_options_menu(strm, strm->in_favorites, false);
            streamlined_select_options_entry(STREAMLINED_OPTIONS_SET_GAME_CORE);
            return 0;
         }

         /* Normal core selected */
         {
            const char *selected_core = entry->label;
            if (!string_is_empty(selected_core) && path_is_valid(selected_core))
            {
               char core_folder[PATH_MAX_LENGTH];
               char game_name[PATH_MAX_LENGTH];

               if (strm->options_was_in_folder)
                  strlcpy(core_folder, strm->options_folder_path, sizeof(core_folder));
               else
                  fill_pathname_parent_dir(core_folder, strm->options_game_path, sizeof(core_folder));

               streamlined_get_game_core_name(strm->options_game_path, core_folder,
                     game_name, sizeof(game_name));
               streamlined_save_game_core(core_folder, game_name, selected_core);
               strlcpy(strm->options_game_core_path, selected_core,
                     sizeof(strm->options_game_core_path));

               strm->selecting_core_for_game = false;
               strm->in_options_menu = true;
               strm->cancel_ignore_frames = 3;
               streamlined_populate_options_menu(strm, strm->in_favorites, false);
               streamlined_select_options_entry(STREAMLINED_OPTIONS_SET_GAME_CORE);
               return 0;
            }
         }
      }

      if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN
            || action == MENU_ACTION_SCROLL_UP || action == MENU_ACTION_SCROLL_DOWN)
         return generic_menu_entry_action(userdata, entry, i, action);

      return 0;
   }

   /* Block input during delete result screen */
   if (strm && strm->delete_done)
      return 0;

   /* Handle delete confirmation input */
   if (strm && strm->in_delete_confirm)
   {
      if (action == MENU_ACTION_CANCEL)
      {
         strm->in_delete_confirm = false;
         strm->in_options_menu = true;
         strm->cancel_ignore_frames = 3;
         streamlined_populate_options_menu(strm, strm->in_favorites,
               strm->in_game_switcher);
         streamlined_select_options_entry(
               strm->delete_is_game ? STREAMLINED_OPTIONS_DELETE_GAME
                                    : STREAMLINED_OPTIONS_DELETE_SAVE);
         return 0;
      }

      if (action == MENU_ACTION_OK)
      {
         if (strm->delete_is_game)
         {
            streamlined_delete_game_files(strm->options_game_path);
            streamlined_remove_from_all_playlists(strm->options_game_path);
            strlcpy(strm->delete_done_label, "Game Deleted",
                  sizeof(strm->delete_done_label));
            strm->delete_was_game = true;
         }
         else
         {
            streamlined_delete_autosave_file(
                  strm->options_game_path, streamlined_effective_core(strm));
            strlcpy(strm->delete_done_label, "Deleted Autosave",
                  sizeof(strm->delete_done_label));
            strm->delete_was_game = false;
         }
         strm->in_delete_confirm = false;
         strm->in_options_menu = false;
         strm->delete_done = true;
         strm->delete_done_start = strm->ticker_idx;
         return 0;
      }

      return 0;  /* Block all other actions */
   }

   /* Handle random game preview input */
   if (strm && strm->in_random_preview)
   {
      if (action == MENU_ACTION_CANCEL)
      {
         /* Return to options menu with Random Game selected */
         gfx_thumbnail_reset(&strm->random_thumbnail);
         strm->random_thumbnail_path[0] = '\0';
         strm->in_random_preview = false;
         strm->in_options_menu = true;
         strm->cancel_ignore_frames = 3;
         streamlined_populate_options_menu(strm, strm->in_favorites, false);
         streamlined_select_options_entry(STREAMLINED_OPTIONS_RANDOM_GAME);
         return 0;
      }

      /* A = Play (start fresh) */
      if (action == MENU_ACTION_OK)
      {
         const char *launch_core = strm->options_core_path;
         char fav_core[PATH_MAX_LENGTH];

         if (strm->in_favorites
               && streamlined_resolve_content_core(strm->random_game_path,
                     fav_core, sizeof(fav_core),
                     NULL, 0,
                     g_defaults.content_favorites))
            launch_core = fav_core;

         strm->folder_selection = strm->options_saved_selection;
         if (strm->in_favorites)
            streamlined_exit_favorites(strm);
         else if (strm->options_was_in_folder)
         {
            strlcpy(strm->last_launched_folder, strm->options_folder_path,
                  sizeof(strm->last_launched_folder));
            strlcpy(strm->last_folder_core_path, strm->options_core_path,
                  sizeof(strm->last_folder_core_path));
            strm->return_to_folder = true;
         }
         else
         {
            strm->return_to_top_level = true;
            strm->top_level_selection = strm->options_saved_selection;
         }

         gfx_thumbnail_reset(&strm->random_thumbnail);
         strm->in_random_preview = false;
         strm->in_options_menu = false;
         streamlined_request_loading(strm,
               launch_core, strm->random_game_path, false);
         return 0;
      }

      /* X = Resume (load autosave) */
      if (action == MENU_ACTION_SCAN && strm->random_has_savestate)
      {
         const char *launch_core = strm->options_core_path;
         char fav_core[PATH_MAX_LENGTH];

         if (strm->in_favorites
               && streamlined_resolve_content_core(strm->random_game_path,
                     fav_core, sizeof(fav_core),
                     NULL, 0,
                     g_defaults.content_favorites))
            launch_core = fav_core;

         strm->folder_selection = strm->options_saved_selection;
         if (strm->in_favorites)
            streamlined_exit_favorites(strm);
         else if (strm->options_was_in_folder)
         {
            strlcpy(strm->last_launched_folder, strm->options_folder_path,
                  sizeof(strm->last_launched_folder));
            strlcpy(strm->last_folder_core_path, strm->options_core_path,
                  sizeof(strm->last_folder_core_path));
            strm->return_to_folder = true;
         }
         else
         {
            strm->return_to_top_level = true;
            strm->top_level_selection = strm->options_saved_selection;
         }

         gfx_thumbnail_reset(&strm->random_thumbnail);
         strm->in_random_preview = false;
         strm->in_options_menu = false;
         streamlined_request_loading(strm,
               launch_core, strm->random_game_path, true);
         return 0;
      }

      /* Y = Toggle thumbnail/text view (only if thumbnail exists) */
      if (action == MENU_ACTION_SEARCH && strm->random_has_thumbnail)
      {
         strm->random_show_text = !strm->random_show_text;
         return 0;
      }

      return 0;  /* Block all other actions */
   }

   /* Deferred transition: options -> search (absorbs lingering OK press) */
   if (strm && strm->enter_search_deferred)
   {
      settings_t *settings = config_get_ptr();
      strm->enter_search_deferred = false;
      strm->in_search_mode = true;
      strm->search_query_len = strlen(strm->search_query);
      strm->search_prev_query[0] = '\0';
      strm->search_keyboard_done = false;
      strm->search_list_selection = 0;

      if (strm->in_favorites)
      {
         /* Build search entries from favorites playlist */
         playlist_t *fav = g_defaults.content_favorites;
         size_t fav_size = fav ? playlist_size(fav) : 0;
         if (fav && fav_size > 0)
            strm->search_all_entries = string_list_new();
         if (strm->search_all_entries)
         {
            size_t j;
            for (j = 0; j < fav_size; j++)
            {
               const struct playlist_entry *pl_entry = NULL;
               union string_list_elem_attr attr;
               char resolved_path[PATH_MAX_LENGTH];
               playlist_get_index(fav, j, &pl_entry);
               if (!pl_entry || string_is_empty(pl_entry->path))
                  continue;
               strlcpy(resolved_path, pl_entry->path, sizeof(resolved_path));
               playlist_resolve_path(PLAYLIST_LOAD, false,
                     resolved_path, sizeof(resolved_path));
               attr.i = RARCH_PLAIN_FILE;
               string_list_append(strm->search_all_entries,
                     resolved_path, attr);
            }
         }
      }
      else
      {
         strm->search_all_entries = dir_list_new(
               strm->options_folder_path, NULL, true,
               settings->bools.show_hidden_files, true, false);
         if (strm->search_all_entries)
            dir_list_sort(strm->search_all_entries, true);
      }

      streamlined_populate_search_results(strm);
      menu_st->selection_ptr = 0;

#if TARGET_OS_TV
      if (strm->search_query[0] != '\0')
         strm->search_kb_buffer_ptr = strdup(strm->search_query);
      else
         strm->search_kb_buffer_ptr = NULL;
      strm->search_kb_buffer_size = strm->search_query_len;
      strm->search_kb_buffer_offset = strm->search_query_len;
      {
         char *prev_buf = strm->search_kb_buffer_ptr;
         ios_keyboard_start(
               &strm->search_kb_buffer_ptr,
               &strm->search_kb_buffer_size,
               &strm->search_kb_buffer_offset,
               "Search",
               streamlined_search_keyboard_cb,
               strm);
         free(prev_buf);
      }
#else
      strm->search_kb_row = 0;
      strm->search_kb_col = 0;
      strm->search_focus_list = false;
#endif
      return 0;
   }

   /* Handle search mode input */
   if (strm && strm->in_search_mode)
   {
      if (action == MENU_ACTION_CANCEL)
      {
#if TARGET_OS_TV
         if (ios_keyboard_active())
         {
            /* Keyboard active — dismiss and exit search */
            ios_keyboard_end();
            /* Fall through to exit search below */
         }
         else if (strm->search_keyboard_done)
         {
            /* Browsing results — re-open keyboard */
            char *prev_buf = strm->search_query[0]
                  ? strdup(strm->search_query) : NULL;
            strm->search_kb_buffer_ptr = prev_buf;
            strm->search_kb_buffer_size = strm->search_query_len;
            strm->search_kb_buffer_offset = strm->search_query_len;
            strm->search_keyboard_done = false;
            {
               char *old = strm->search_kb_buffer_ptr;
               ios_keyboard_start(
                     &strm->search_kb_buffer_ptr,
                     &strm->search_kb_buffer_size,
                     &strm->search_kb_buffer_offset,
                     "Search",
                     streamlined_search_keyboard_cb,
                     strm);
               free(old);
            }
            return 0;
         }
#else
         if (strm->search_focus_list)
         {
            /* Browsing results — return focus to keyboard */
            strm->search_focus_list = false;
            return 0;
         }
#endif
         /* Defer transition to options menu (next frame) to absorb lingering cancel */
         streamlined_cleanup_search(strm, false);
         strm->return_to_options = true;
         return 0;
      }

#if TARGET_OS_TV
      if (strm->search_keyboard_done)
      {
#endif
         /* Handle game list navigation (tvOS: after keyboard done, non-tvOS: when list focused) */
#if !TARGET_OS_TV
         if (!strm->search_focus_list)
         {
            /* Keyboard grid navigation */
            if (action == MENU_ACTION_UP)
            {
               strm->search_kb_row--;
               if (strm->search_kb_row < 0)
                  strm->search_kb_row = STREAMLINED_KB_NUM_ROWS - 1;
               if (strm->search_kb_col >= streamlined_kb_row_lens[strm->search_kb_row])
                  strm->search_kb_col = streamlined_kb_row_lens[strm->search_kb_row] - 1;
               return 0;
            }
            if (action == MENU_ACTION_DOWN)
            {
               strm->search_kb_row++;
               if (strm->search_kb_row >= STREAMLINED_KB_NUM_ROWS)
               {
                  strm->search_focus_list = true;
                  strm->search_kb_row = STREAMLINED_KB_NUM_ROWS - 1;
                  menu_st->selection_ptr = 0;
                  return 0;
               }
               if (strm->search_kb_col >= streamlined_kb_row_lens[strm->search_kb_row])
                  strm->search_kb_col = streamlined_kb_row_lens[strm->search_kb_row] - 1;
               return 0;
            }
            if (action == MENU_ACTION_LEFT)
            {
               strm->search_kb_col--;
               if (strm->search_kb_col < 0)
                  strm->search_kb_col = streamlined_kb_row_lens[strm->search_kb_row] - 1;
               return 0;
            }
            if (action == MENU_ACTION_RIGHT)
            {
               strm->search_kb_col++;
               if (strm->search_kb_col >= streamlined_kb_row_lens[strm->search_kb_row])
                  strm->search_kb_col = 0;
               return 0;
            }
            if (action == MENU_ACTION_OK)
            {
               char ch = streamlined_kb_rows[strm->search_kb_row][strm->search_kb_col];
               if (ch == '\x08')
               {
                  if (strm->search_query_len > 0)
                  {
                     strm->search_query_len--;
                     strm->search_query[strm->search_query_len] = '\0';
                  }
               }
               else if (strm->search_query_len < sizeof(strm->search_query) - 1)
               {
                  strm->search_query[strm->search_query_len++] = ch;
                  strm->search_query[strm->search_query_len] = '\0';
               }
               return 0;
            }
            return 0;
         }
         else
         {
            /* Game list has focus */
            if (action == MENU_ACTION_UP && menu_st->selection_ptr == 0)
            {
               strm->search_focus_list = false;
               return 0;
            }
#endif
            if (action == MENU_ACTION_OK && entry)
            {
               const char *item_path = entry->label;
               if (!string_is_empty(item_path) && path_is_valid(item_path))
               {
                  const char *launch_core = strm->options_core_path;
                  char fav_core[PATH_MAX_LENGTH];

                  if (strm->in_favorites)
                  {
                     if (!streamlined_resolve_content_core(item_path,
                           fav_core, sizeof(fav_core),
                           NULL, 0,
                           g_defaults.content_favorites))
                     {
                        /* No core found — show core selection */
                        strlcpy(strm->pending_content_path, item_path,
                              sizeof(strm->pending_content_path));
                        streamlined_cleanup_search(strm, true);
                        strm->selecting_core = true;
                        streamlined_populate_core_selection(strm, item_path);
                        menu_st->selection_ptr = 0;
                        return 0;
                     }
                     launch_core = fav_core;
                  }

                  if (!string_is_empty(launch_core))
                  {
                     strm->folder_selection = strm->options_saved_selection;
                     if (strm->in_favorites)
                        streamlined_exit_favorites(strm);
                     else if (strm->options_was_in_folder)
                     {
                        strlcpy(strm->last_launched_folder, strm->options_folder_path,
                              sizeof(strm->last_launched_folder));
                        strlcpy(strm->last_folder_core_path, strm->options_core_path,
                              sizeof(strm->last_folder_core_path));
                        strm->return_to_folder = true;
                     }
                     else
                     {
                        strm->return_to_top_level = true;
                        strm->top_level_selection = strm->options_saved_selection;
                     }

                     strm->in_options_menu = false;
                     streamlined_cleanup_search(strm, true);
                     streamlined_request_loading(strm,
                           launch_core, item_path, false);
                     return 0;
                  }
               }
            }

            /* X button = resume with save state */
            if (action == MENU_ACTION_SCAN && entry)
            {
               const char *item_path = entry->label;
               if (!string_is_empty(item_path) && path_is_valid(item_path))
               {
                  const char *launch_core = strm->options_core_path;
                  char fav_core[PATH_MAX_LENGTH];

                  if (strm->in_favorites
                        && streamlined_resolve_content_core(item_path,
                              fav_core, sizeof(fav_core),
                              NULL, 0,
                              g_defaults.content_favorites))
                     launch_core = fav_core;

                  if (!string_is_empty(launch_core))
                  {
                     strm->folder_selection = strm->options_saved_selection;
                     if (strm->in_favorites)
                        streamlined_exit_favorites(strm);
                     else if (strm->options_was_in_folder)
                     {
                        strlcpy(strm->last_launched_folder, strm->options_folder_path,
                              sizeof(strm->last_launched_folder));
                        strlcpy(strm->last_folder_core_path, strm->options_core_path,
                              sizeof(strm->last_folder_core_path));
                        strm->return_to_folder = true;
                     }
                     else
                     {
                        strm->return_to_top_level = true;
                        strm->top_level_selection = strm->options_saved_selection;
                     }

                     strm->in_options_menu = false;
                     streamlined_cleanup_search(strm, true);
                     streamlined_request_loading(strm,
                           launch_core, item_path, true);
                     return 0;
                  }
               }
            }

            /* Let generic handler do up/down navigation in the list */
            if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN
                || action == MENU_ACTION_SCROLL_UP || action == MENU_ACTION_SCROLL_DOWN)
               return generic_menu_entry_action(userdata, entry, i, action);

#if !TARGET_OS_TV
            return 0;
         }  /* end search_focus_list else */
#endif

#if TARGET_OS_TV
      }  /* end search_keyboard_done */
#endif
      return 0;
   }

   /* Deferred transition: search -> options (absorbs lingering cancel) */
   if (strm && strm->return_to_options)
   {
      strm->return_to_options = false;
      strm->in_options_menu = true;
      strm->cancel_ignore_frames = 3;
      streamlined_populate_options_menu(strm, strm->in_favorites, false);
      streamlined_select_options_entry(STREAMLINED_OPTIONS_SEARCH);
      return 0;
   }

   /* Handle Game List Options menu */
   if (strm && strm->in_options_menu)
   {
      if (action == MENU_ACTION_CANCEL)
      {
         strm->in_options_menu = false;
         strm->rom_thumbnail_selection = (size_t)-1;

         /* Return to game switcher if we came from there */
         if (strm->in_game_switcher)
         {
            strm->game_switcher_in_glo = false;
            streamlined_refresh_game_switcher_view(strm);
            return 0;
         }

         if (strm->in_favorites)
            streamlined_populate_favorites_menu(strm);
         else if (strm->options_was_in_folder)
            streamlined_populate_folder_menu(strm, strm->options_folder_path, true);
         else
         {
            settings_t *settings = config_get_ptr();
            streamlined_populate_folder_menu(strm, settings->paths.directory_menu_content, false);
         }
         menu_st->selection_ptr = strm->options_saved_selection;
         return 0;
      }

      if (action == MENU_ACTION_OK && entry)
      {
         if (entry->enum_idx == STREAMLINED_OPTIONS_ADD_FAVORITE)
         {
            const char *eff_core = streamlined_effective_core(strm);
            streamlined_add_to_favorites(strm->options_game_path, eff_core);
            strm->in_options_menu = false;
            strm->rom_thumbnail_selection = (size_t)-1;
            /* Return to game list */
            if (strm->options_was_in_folder)
               streamlined_populate_folder_menu(strm, strm->options_folder_path, true);
            else
            {
               settings_t *settings = config_get_ptr();
               streamlined_populate_folder_menu(strm, settings->paths.directory_menu_content, false);
            }
            menu_st->selection_ptr = strm->options_saved_selection;
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_REMOVE_FAVORITE)
         {
            streamlined_remove_from_favorites(strm->options_game_path);
            strm->in_options_menu = false;
            if (strm->in_favorites)
            {
               playlist_t *fav = g_defaults.content_favorites;
               if (!fav || playlist_size(fav) == 0)
               {
                  /* Last favorite removed — return to top level */
                  settings_t *settings = config_get_ptr();
                  strm->in_favorites = false;
                  strm->selected_has_savestate = false;
                  strm->selected_is_file = false;
                  gfx_thumbnail_reset(&strm->rom_thumbnail);
                  strm->rom_thumbnail_path[0] = '\0';
                  strm->rom_thumbnail_selection = (size_t)-1;
                  streamlined_populate_folder_menu(strm,
                        settings->paths.directory_menu_content, false);
                  menu_st->selection_ptr = 0;
               }
               else
               {
                  streamlined_populate_favorites_menu(strm);
                  strm->rom_thumbnail_selection = (size_t)-1;
                  if (strm->options_saved_selection >= playlist_size(fav))
                     menu_st->selection_ptr = playlist_size(fav) - 1;
                  else
                     menu_st->selection_ptr = strm->options_saved_selection;
               }
            }
            else
            {
               if (strm->options_was_in_folder)
                  streamlined_populate_folder_menu(strm, strm->options_folder_path, true);
               else
               {
                  settings_t *settings = config_get_ptr();
                  streamlined_populate_folder_menu(strm, settings->paths.directory_menu_content, false);
               }
               menu_st->selection_ptr = strm->options_saved_selection;
               strm->rom_thumbnail_selection = (size_t)-1;
            }
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_SEARCH)
         {
            strm->enter_search_deferred = true;
            strm->in_options_menu = false;
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_RESET_GAME)
         {
            /* Delete autosave and launch fresh */
            const char *eff_core = streamlined_effective_core(strm);
            streamlined_delete_autosave_file(
                  strm->options_game_path, eff_core);

            /* Save return state */
            strm->folder_selection = strm->options_saved_selection;
            if (strm->in_favorites)
               streamlined_exit_favorites(strm);
            else if (strm->options_was_in_folder)
            {
               strlcpy(strm->last_launched_folder, strm->options_folder_path,
                     sizeof(strm->last_launched_folder));
               strlcpy(strm->last_folder_core_path, strm->options_core_path,
                     sizeof(strm->last_folder_core_path));
               strm->return_to_folder = true;
            }
            else
            {
               strm->return_to_top_level = true;
               strm->top_level_selection = strm->options_saved_selection;
            }

            strm->in_options_menu = false;
            streamlined_request_loading(strm,
                  eff_core, strm->options_game_path, false);
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_DELETE_SAVE)
         {
            strm->in_delete_confirm = true;
            strm->delete_is_game = false;
            strm->in_options_menu = false;
#if TARGET_OS_TV
            {
               char display_name[256];
               streamlined_get_display_name(strm->options_game_path,
                     display_name, sizeof(display_name), false);
               ios_show_confirm_dialog(
                     "Delete Autosave", display_name, "Delete",
                     streamlined_delete_confirm_cb, strm);
            }
#endif
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_DELETE_GAME)
         {
            strm->in_delete_confirm = true;
            strm->delete_is_game = true;
            strm->in_options_menu = false;
#if TARGET_OS_TV
            {
               char display_name[256];
               char dialog_msg[512];
               streamlined_get_display_name(strm->options_game_path,
                     display_name, sizeof(display_name), false);
               snprintf(dialog_msg, sizeof(dialog_msg),
                     "%s\n\nSaves, save states, and thumbnails will not be deleted.",
                     display_name);
               ios_show_confirm_dialog(
                     "Delete Game", dialog_msg, "Delete",
                     streamlined_delete_confirm_cb, strm);
            }
#endif
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_SET_FOLDER_CORE)
         {
            strm->selecting_core_for_folder = true;
            strm->in_options_menu = false;
            streamlined_populate_core_selection(strm, strm->options_game_path);
            menu_st->selection_ptr = 0;
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_SET_GAME_CORE)
         {
            strm->selecting_core_for_game = true;
            strm->in_options_menu = false;
            streamlined_populate_core_selection(strm, strm->options_game_path);

            /* If game already has a per-game override, prepend "Use Folder Default" */
            if (!string_is_empty(strm->options_game_core_path))
            {
               file_list_t *clist = MENU_LIST_GET_SELECTION(menu_st->entries.list, 0);
               if (clist)
               {
                  /* Insert at position 0 */
                  menu_entries_prepend(clist,
                        "Use Folder Default", "",
                        STREAMLINED_OPTIONS_CLEAR_GAME_CORE,
                        MENU_SETTING_ACTION, 0, 0);
               }
            }
            menu_st->selection_ptr = 0;
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_RANDOM_GAME && strm->in_favorites)
         {
            playlist_t *fav = g_defaults.content_favorites;
            size_t fav_size = fav ? playlist_size(fav) : 0;
            if (fav_size > 0)
            {
               size_t pick = (size_t)(rand() % fav_size);
               const struct playlist_entry *pl_entry = NULL;
               char fav_folder[PATH_MAX_LENGTH];

               playlist_get_index(fav, pick, &pl_entry);
               if (pl_entry && !string_is_empty(pl_entry->path))
               {
                  char resolved_path[PATH_MAX_LENGTH];
                  strlcpy(resolved_path, pl_entry->path, sizeof(resolved_path));
                  playlist_resolve_path(PLAYLIST_LOAD, false,
                        resolved_path, sizeof(resolved_path));

                  strlcpy(strm->random_game_path, resolved_path,
                        sizeof(strm->random_game_path));

                  /* Display name */
                  if (!string_is_empty(pl_entry->label))
                     strlcpy(strm->random_display_name, pl_entry->label,
                           sizeof(strm->random_display_name));
                  else
                     streamlined_get_display_name(resolved_path,
                           strm->random_display_name,
                           sizeof(strm->random_display_name), false);

                  /* Resolve core for savestate check */
                  {
                     char fav_core[PATH_MAX_LENGTH];
                     if (streamlined_resolve_content_core(resolved_path,
                           fav_core, sizeof(fav_core),
                           NULL, 0,
                           g_defaults.content_favorites))
                        strm->random_has_savestate = streamlined_check_savestate(
                              resolved_path, fav_core);
                     else
                        strm->random_has_savestate = false;
                  }

                  /* Set folder path for thumbnail loading */
                  streamlined_get_content_folder_path(resolved_path,
                        fav_folder, sizeof(fav_folder));
                  strlcpy(strm->options_folder_path, fav_folder,
                        sizeof(strm->options_folder_path));

                  strm->random_show_text = false;
                  streamlined_load_random_thumbnail(strm);
                  strm->in_random_preview = true;
                  strm->in_options_menu = false;
               }
            }
            return 0;
         }

         if (entry->enum_idx == STREAMLINED_OPTIONS_RANDOM_GAME)
         {
            settings_t *settings = config_get_ptr();
            struct string_list *file_list = dir_list_new(
                  strm->options_folder_path, NULL, true,
                  settings->bools.show_hidden_files, true, false);
            if (file_list && file_list->size > 0)
            {
               size_t j, file_count = 0;
               size_t *file_indices = (size_t*)calloc(file_list->size, sizeof(size_t));
               if (file_indices)
               {
                  for (j = 0; j < file_list->size; j++)
                  {
                     const char *path = file_list->elems[j].data;
                     unsigned attr = file_list->elems[j].attr.i;
                     const char *name = path_basename(path);
                     if (!name || name[0] == '.')
                        continue;
                     if (attr == RARCH_DIRECTORY)
                     {
                        char m3u_path[PATH_MAX_LENGTH];
                        if (streamlined_check_m3u_folder(path, m3u_path, sizeof(m3u_path)))
                           file_indices[file_count++] = j;
                     }
                     else
                        file_indices[file_count++] = j;
                  }

                  if (file_count > 0)
                  {
                     size_t pick = (size_t)(rand() % file_count);
                     size_t idx = file_indices[pick];
                     const char *picked_path = file_list->elems[idx].data;
                     unsigned picked_attr = file_list->elems[idx].attr.i;

                     /* Resolve launch path for M3U game folders */
                     if (picked_attr == RARCH_DIRECTORY)
                     {
                        char m3u_path[PATH_MAX_LENGTH];
                        streamlined_check_m3u_folder(picked_path, m3u_path, sizeof(m3u_path));
                        strlcpy(strm->random_game_path, m3u_path,
                              sizeof(strm->random_game_path));
                     }
                     else
                     {
                        strlcpy(strm->random_game_path, picked_path,
                              sizeof(strm->random_game_path));
                     }

                     /* Build display name */
                     streamlined_get_display_name(picked_path,
                           strm->random_display_name,
                           sizeof(strm->random_display_name),
                           picked_attr == RARCH_DIRECTORY);

                     /* Check savestate */
                     strm->random_has_savestate = streamlined_check_savestate(
                           strm->random_game_path, strm->options_core_path);

                     /* Load thumbnail and determine initial view */
                     strm->random_show_text = false;
                     streamlined_load_random_thumbnail(strm);

                     strm->in_random_preview = true;
                     strm->in_options_menu = false;
                  }
                  free(file_indices);
               }
            }
            if (file_list)
               string_list_free(file_list);
            return 0;
         }
      }

      /* Block non-navigation actions */
      if (action == MENU_ACTION_SCAN
          || action == MENU_ACTION_SEARCH
          || action == MENU_ACTION_INFO)
         return 0;
   }

   /* Handle cancel from RA settings to return to Main Settings submenu */
   if (strm && strm->return_to_main_settings_submenu && action == MENU_ACTION_CANCEL)
   {
      strm->return_to_main_settings_submenu = false;
      strm->is_custom_main_menu = true;
      strm->in_main_settings_submenu = true;
      strm->in_folder = false;
      streamlined_populate_main_settings_submenu();
      if (menu_st)
         menu_st->selection_ptr = strm->saved_settings_selection;
      return 0;
   }

   /* Handle custom main menu (launcher mode) navigation */
   if (strm && strm->is_custom_main_menu)
   {
      /* Select button — open Game Switcher */
      if (action == MENU_ACTION_INFO
            && !strm->in_main_settings_submenu
            && !strm->in_options_menu
            && !strm->in_search_mode)
      {
         playlist_t *history = g_defaults.content_history;
         if (history && playlist_size(history) > 0)
         {
            streamlined_enter_game_switcher(strm);
            return 0;
         }
      }

      /* Back from favorites — return to top level */
      if (action == MENU_ACTION_CANCEL && strm->in_favorites)
      {
         settings_t *settings = config_get_ptr();
         const char *start_dir = settings->paths.directory_menu_content;

         strm->in_favorites = false;
         strm->selected_has_savestate = false;
         strm->selected_is_file = false;
         gfx_thumbnail_reset(&strm->rom_thumbnail);
         strm->rom_thumbnail_path[0] = '\0';
         strm->rom_thumbnail_selection = (size_t)-1;
         if (!string_is_empty(start_dir))
            streamlined_populate_folder_menu(strm, start_dir, false);
         menu_st->selection_ptr = strm->favorites_saved_selection;
         return 0;
      }

      /* Block back navigation when inside a folder */
      if (action == MENU_ACTION_CANCEL && strm->in_folder)
      {
         /* Go back to top-level folder listing */
         settings_t *settings = config_get_ptr();
         const char *start_dir = settings->paths.directory_menu_content;

         if (!string_is_empty(start_dir))
         {
            streamlined_populate_folder_menu(strm, start_dir, false);  /* Back to top - no slash */
            strlcpy(strm->current_folder_path, start_dir,
                  sizeof(strm->current_folder_path));
            strm->in_folder = false;
            strm->selected_has_savestate = false;
            strm->selected_is_file = false;
            gfx_thumbnail_reset(&strm->rom_thumbnail);
            strm->rom_thumbnail_path[0] = '\0';
            strm->rom_thumbnail_selection = (size_t)-1;
            /* Restore saved main menu selection */
            menu_st->selection_ptr = strm->main_menu_selection;
         }
         return 0;
      }

      /* Back button in main settings submenu: return to main menu */
      if (action == MENU_ACTION_CANCEL && strm->in_main_settings_submenu)
      {
         settings_t *settings = config_get_ptr();
         const char *start_dir = settings->paths.directory_menu_content;

         strm->in_main_settings_submenu = false;
         strm->return_to_main_settings_submenu = false;
         if (!string_is_empty(start_dir))
         {
            streamlined_populate_folder_menu(strm, start_dir, false);
            strlcpy(strm->current_folder_path, start_dir,
                  sizeof(strm->current_folder_path));
         }
         /* Restore saved main menu selection */
         menu_st->selection_ptr = strm->saved_main_menu_selection;
         return 0;
      }

      /* Block back navigation at top level (nowhere to go) */
      if (action == MENU_ACTION_CANCEL && !strm->in_folder && !strm->in_main_settings_submenu)
      {
         return 0;  /* Do nothing - can't go up from top level */
      }

      /* Handle folder/file/settings selection */
      if (action == MENU_ACTION_OK && entry)
      {
         /* Check for Settings entry - show custom settings submenu */
         if (entry->enum_idx == MENU_ENUM_LABEL_SETTINGS && !strm->in_main_settings_submenu)
         {
            strm->saved_main_menu_selection = menu_st->selection_ptr;
            strm->in_main_settings_submenu = true;
            streamlined_populate_main_settings_submenu();
            menu_st->selection_ptr = 0;
            return 0;
         }

         /* Handle selection within main settings submenu */
         if (strm->in_main_settings_submenu)
         {
            /* Save selection so we can return to same position */
            strm->saved_settings_selection = menu_st->selection_ptr;
            /* Set flag to return to settings submenu when backing out */
            strm->return_to_main_settings_submenu = true;
            /* Let generic handler process the RA menu item */
            strm->is_custom_main_menu = false;
            strm->in_main_settings_submenu = false;
            return generic_menu_entry_action(userdata, entry, i, action);
         }

         /* Favorites entry — enter favorites list */
         if (entry->enum_idx == STREAMLINED_FAVORITES_ENTRY)
         {
            strm->favorites_saved_selection = menu_st->selection_ptr;
            strm->in_favorites = true;
            strm->in_folder = false;
            strm->folder_core_path[0] = '\0';
            streamlined_populate_favorites_menu(strm);
            gfx_thumbnail_reset(&strm->rom_thumbnail);
            strm->rom_thumbnail_path[0] = '\0';
            strm->rom_thumbnail_selection = (size_t)-1;
            menu_st->selection_ptr = 0;
            return 0;
         }

         /* Game Switcher entry — enter configured view */
         if (entry->enum_idx == STREAMLINED_GAME_SWITCHER_ENTRY)
         {
            streamlined_enter_game_switcher(strm);
            return 0;
         }

         /* For custom main menu entries, the full path is in entry->label
          * (entry->path contains the display name without path/extension) */
         const char *item_path = entry->label;

         if (!string_is_empty(item_path))
         {
            if (path_is_directory(item_path))
            {
               /* Save current selection before entering folder */
               strm->main_menu_selection = menu_st->selection_ptr;

               /* Enter the selected folder */
               streamlined_populate_folder_menu(strm, item_path, true);  /* Inside folder - show slash */
               strlcpy(strm->current_folder_path, item_path,
                     sizeof(strm->current_folder_path));

               /* Try to read folder's core.txt */
               if (!streamlined_read_folder_core(item_path, strm->folder_core_path,
                     sizeof(strm->folder_core_path)))
                  strm->folder_core_path[0] = '\0';  /* No core.txt found */

               strm->in_folder = true;
               strm->selected_has_savestate = false;
               strm->selected_is_file = false;
               gfx_thumbnail_reset(&strm->rom_thumbnail);
               strm->rom_thumbnail_path[0] = '\0';
               strm->rom_thumbnail_selection = (size_t)-1;
               menu_st->selection_ptr = 0;
               return 0;
            }
            else if (strm->in_favorites && path_is_valid(item_path))
            {
               /* Launch from favorites */
               char fav_core[PATH_MAX_LENGTH];
               if (streamlined_resolve_content_core(item_path,
                     fav_core, sizeof(fav_core),
                     NULL, 0,
                     g_defaults.content_favorites))
               {
                  streamlined_exit_favorites(strm);
                  streamlined_request_loading(strm, fav_core, item_path, false);
                  return 0;
               }
               /* No core → show core selection screen */
               strlcpy(strm->pending_content_path, item_path,
                     sizeof(strm->pending_content_path));
               strm->selecting_core = true;
               streamlined_populate_core_selection(strm, item_path);
               menu_st->selection_ptr = 0;
               return 0;
            }
            else if (path_is_valid(item_path))
            {
               /* Launch the selected file (ROM) */
               const char *core_path = NULL;
               char resolved_core[PATH_MAX_LENGTH];

               if (streamlined_resolve_content_core(item_path,
                     resolved_core, sizeof(resolved_core),
                     NULL, 0, NULL))
               {
                  core_path = resolved_core;
               }
               else
               {
                  /* No core found — show core selection screen */
                  strlcpy(strm->pending_content_path, item_path,
                        sizeof(strm->pending_content_path));
                  strm->selecting_core = true;
                  streamlined_populate_core_selection(strm, item_path);
                  menu_st->selection_ptr = 0;
                  return 0;
               }

               if (core_path && path_is_valid(core_path))
               {
                  /* Save state so we can return after quitting */
                  strm->folder_selection = menu_st->selection_ptr;
                  if (strm->in_folder)
                  {
                     strlcpy(strm->last_launched_folder, strm->current_folder_path,
                           sizeof(strm->last_launched_folder));
                     strlcpy(strm->last_folder_core_path, strm->folder_core_path,
                           sizeof(strm->last_folder_core_path));
                     strm->return_to_folder = true;
                  }
                  else
                  {
                     strm->return_to_top_level = true;
                     strm->top_level_selection = menu_st->selection_ptr;
                  }

                  streamlined_request_loading(strm, core_path, item_path, false);

                  return 0;
               }
               /* No compatible core found - do nothing for now */
               return 0;
            }
         }
      }

      /* X button = Resume from Favorites */
      if (action == MENU_ACTION_SCAN
            && strm->in_favorites
            && strm->selected_has_savestate
            && entry)
      {
         const char *item_path = entry->label;
         char fav_core[PATH_MAX_LENGTH];
         if (!string_is_empty(item_path) && path_is_valid(item_path)
               && streamlined_resolve_content_core(item_path,
                     fav_core, sizeof(fav_core),
                     NULL, 0,
                     g_defaults.content_favorites))
         {
            streamlined_exit_favorites(strm);
            streamlined_request_loading(strm, fav_core, item_path, true);
            return 0;
         }
      }

      /* X button = Resume with save state */
      if (action == MENU_ACTION_SCAN
            && strm->selected_has_savestate
            && entry)
      {
         const char *item_path = entry->label;
         const char *core_path = strm->folder_core_path;
         char resolved_core[PATH_MAX_LENGTH];

         if (streamlined_resolve_content_core(item_path,
               resolved_core, sizeof(resolved_core),
               NULL, 0, NULL))
            core_path = resolved_core;

         if (!string_is_empty(item_path) && path_is_valid(item_path)
               && !string_is_empty(core_path) && path_is_valid(core_path))
         {
            /* Save state for return */
            strm->folder_selection = menu_st->selection_ptr;
            if (strm->in_folder)
            {
               strlcpy(strm->last_launched_folder, strm->current_folder_path,
                     sizeof(strm->last_launched_folder));
               strlcpy(strm->last_folder_core_path, strm->folder_core_path,
                     sizeof(strm->last_folder_core_path));
               strm->return_to_folder = true;
            }
            else
            {
               strm->return_to_top_level = true;
               strm->top_level_selection = menu_st->selection_ptr;
            }

            streamlined_request_loading(strm, core_path, item_path, true);

            return 0;
         }
      }

      /* Y button = Open Game List Options menu (Favorites) */
      if (action == MENU_ACTION_SEARCH && strm->in_favorites
            && strm->selected_is_file && entry)
      {
         char fav_folder[PATH_MAX_LENGTH];

         strm->options_saved_selection = menu_st->selection_ptr;
         strlcpy(strm->options_game_path, entry->label,
               sizeof(strm->options_game_path));
         strm->options_game_has_savestate = strm->selected_has_savestate;
         strm->options_was_in_folder = false;

         /* Set folder path from game's original location */
         streamlined_get_content_folder_path(entry->label,
               fav_folder, sizeof(fav_folder));
         strlcpy(strm->options_folder_path, fav_folder,
               sizeof(strm->options_folder_path));

         /* Resolve core and per-game core override */
         streamlined_resolve_content_core(entry->label,
               strm->options_core_path, sizeof(strm->options_core_path),
               strm->options_game_core_path, sizeof(strm->options_game_core_path),
               g_defaults.content_favorites);

         strm->in_options_menu = true;
         gfx_thumbnail_reset(&strm->rom_thumbnail);
         strm->rom_thumbnail_path[0] = '\0';
         streamlined_populate_options_menu(strm, strm->in_favorites, false);
         menu_st->selection_ptr = 0;
         return 0;
      }

      /* Y button = Open Game List Options menu */
      if (action == MENU_ACTION_SEARCH && strm->selected_is_file && entry)
      {
         strm->options_saved_selection = menu_st->selection_ptr;
         strlcpy(strm->options_game_path, entry->label,
               sizeof(strm->options_game_path));
         strlcpy(strm->options_folder_path, strm->current_folder_path,
               sizeof(strm->options_folder_path));
         strm->options_game_has_savestate = strm->selected_has_savestate;
         strm->options_was_in_folder = strm->in_folder;

         /* Resolve core and per-game core override */
         streamlined_resolve_content_core(entry->label,
               strm->options_core_path, sizeof(strm->options_core_path),
               strm->options_game_core_path, sizeof(strm->options_game_core_path),
               NULL);

         strm->in_options_menu = true;
         gfx_thumbnail_reset(&strm->rom_thumbnail);
         strm->rom_thumbnail_path[0] = '\0';
         streamlined_populate_options_menu(strm, strm->in_favorites, false);
         menu_st->selection_ptr = 0;
         return 0;
      }

      /* Block non-navigation actions (SCAN, SEARCH, INFO, etc.)
       * from reaching the generic handler - custom menu entries
       * lack the path setup that those handlers expect */
      if (   action == MENU_ACTION_SCAN
          || action == MENU_ACTION_SEARCH
          || action == MENU_ACTION_INFO)
         return 0;
   }

   /* Delegate all other input to RetroArch's generic menu handler */
   return generic_menu_entry_action(userdata, entry, i, action);
}

menu_ctx_driver_t menu_ctx_streamlined = {
   NULL,                         /* set_texture */
   NULL,                         /* render_messagebox */
   streamlined_render,
   streamlined_frame,
   streamlined_init,
   streamlined_free,
   streamlined_context_reset,
   streamlined_context_destroy,
   streamlined_populate_entries,
   NULL,                         /* toggle */
   streamlined_navigation_clear,
   NULL,                         /* navigation_decrement */
   NULL,                         /* navigation_increment */
   streamlined_navigation_set,
   streamlined_navigation_set_last,
   NULL,                         /* navigation_descend_alphabet */
   NULL,                         /* navigation_ascend_alphabet */
   NULL,                         /* lists_init */
   NULL,                         /* list_insert */
   NULL,                         /* list_prepend */
   NULL,                         /* list_delete */
   NULL,                         /* list_clear */
   NULL,                         /* list_cache */
   NULL,                         /* list_push */
   NULL,                         /* list_get_selection */
   NULL,                         /* list_get_size */
   NULL,                         /* list_get_entry */
   NULL,                         /* list_set_selection */
   NULL,                         /* bind_init */
   NULL,                         /* load_image */
   "streamlined",
   streamlined_environ,
   NULL,                         /* update_thumbnail_path */
   NULL,                         /* update_thumbnail_image */
   NULL,                         /* refresh_thumbnail_image */
   NULL,                         /* set_thumbnail_content */
   NULL,                         /* osk_ptr_at_pos */
   NULL,                         /* update_savestate_thumbnail_path */
   NULL,                         /* update_savestate_thumbnail_image */
   NULL,                         /* pointer_down */
   streamlined_pointer_up,
   streamlined_entry_action
};
