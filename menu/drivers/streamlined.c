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
 * Navigation:
 * - Uses a view stack (streamlined_view_stack_t) for hierarchical navigation
 * - Push a view to navigate deeper, pop to go back
 * - View types: MAIN_MENU, FOLDER, CORE_SELECT, QUICK_MENU, ADVANCED,
 *   MAIN_SETTINGS, RA_SETTINGS
 * - Resume state persists across content load/unload for folder return
 */

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

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
#include "../../content.h"
#include "../../core_info.h"
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <streams/file_stream.h>

#if TARGET_OS_TV
#include <CoreText/CoreText.h>
#endif

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
#define STREAMLINED_BASE_FONT_SIZE     32     /* Base font size in pixels */
#define STREAMLINED_MARGIN_RATIO       0.03f  /* Screen edge margin as ratio of dimension */
#define STREAMLINED_LINE_HEIGHT        1.8f   /* Line height multiplier for menu items */
#define STREAMLINED_PILL_PADDING_RATIO 0.375f /* Horizontal padding as ratio of font size */
#define STREAMLINED_PILL_HEIGHT_RATIO  1.5f   /* Pill height as ratio of font size */
#define STREAMLINED_MIN_FONT_SIZE      12     /* Minimum font size to ensure readability */

/* Font scale ratios relative to base font size */
#define STREAMLINED_FONT_SMALL_RATIO   0.75f  /* Small font scale */
#define STREAMLINED_FONT_TITLE_RATIO   1.1f   /* Title font scale */
#define STREAMLINED_FONT_TINY_RATIO    2.5f   /* Tiny font scale (dot_radius * this) */
#define STREAMLINED_GLYPH_WIDTH_RATIO  0.6f   /* Estimated glyph width ratio */

/* Text layout */
#define STREAMLINED_VALUE_WIDTH_PCT    45      /* Max value width as percentage of content area */
#define STREAMLINED_TITLE_AREA_RATIO   1.4f    /* Title area height as ratio of title font size */
#define STREAMLINED_TEXT_VCENTER       0.35f   /* Vertical centering offset for font baseline */
#define STREAMLINED_TEXT_VCENTER_PILL  0.30f   /* Vertical centering offset inside pills */

/* Save slot thumbnail/polaroid frame constants (base sizes before scaling) */
#define STREAMLINED_THUMB_HEIGHT_RATIO 0.45f   /* Thumbnail height as ratio of screen height */
#define STREAMLINED_THUMB_ASPECT_RATIO (4.0f / 3.0f) /* Thumbnail aspect ratio */
#define STREAMLINED_FRAME_BORDER_BASE  5       /* Frame side/top border in base pixels */
#define STREAMLINED_FRAME_CHIN_BASE    28      /* Frame bottom chin in base pixels */
#define STREAMLINED_DOT_RADIUS_BASE    4       /* Dot indicator radius in base pixels */
#define STREAMLINED_DOT_SPACING_BASE   16      /* Dot indicator spacing in base pixels */

/* Footer area constants (base sizes before scaling) */
#define STREAMLINED_FOOTER_HEIGHT_BASE 78.0f   /* Footer area height */
#define STREAMLINED_FOOTER_MARGIN_BASE 40.0f   /* Footer edge margin */
#define STREAMLINED_FOOTER_PILL_PAD    10.0f   /* Footer pill internal padding */
#define STREAMLINED_FOOTER_GAP         8.0f    /* Gap between pill and label text */

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

/* Marker for "Save and Quit" entry - only shown when savestate_auto_save is on */
#define STREAMLINED_SAVE_AND_QUIT_MARKER 0xCB00

/* Main custom quick menu
 * NOTE: Exit/Quit handled dynamically - see streamlined_populate_quick_menu() */
static const streamlined_quick_item_t streamlined_quick_menu_items[] = {
   { "Resume",        MENU_ENUM_LABEL_RESUME_CONTENT },
   { "Save",          MENU_ENUM_LABEL_SAVE_STATE },
   { "Load",          MENU_ENUM_LABEL_LOAD_STATE },
   { "Advanced",      STREAMLINED_SETTINGS_SUBMENU_MARKER },  /* Opens combined settings submenu */
   { "Reset",         MENU_ENUM_LABEL_RESTART_CONTENT },
   { NULL,            STREAMLINED_EXIT_MARKER },           /* Dynamic: "Exit" or "Quit" based on CLI */
   { NULL,            STREAMLINED_SAVE_AND_QUIT_MARKER },  /* Dynamic: "Save and Quit" (auto_save only) */
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

/* View types for the navigation stack */
typedef enum
{
   STREAMLINED_VIEW_MAIN_MENU,      /* Top-level folder list */
   STREAMLINED_VIEW_FOLDER,         /* Inside a folder (games list) */
   STREAMLINED_VIEW_CORE_SELECT,    /* Core selection screen */
   STREAMLINED_VIEW_QUICK_MENU,     /* In-game pause menu */
   STREAMLINED_VIEW_ADVANCED,       /* Quick menu Advanced submenu */
   STREAMLINED_VIEW_MAIN_SETTINGS,  /* Main menu Settings submenu */
   STREAMLINED_VIEW_RA_SETTINGS     /* Delegated to RA generic handler */
} streamlined_view_type_t;

/* Per-view data stored in a tagged union */
typedef struct
{
   streamlined_view_type_t type;
   size_t saved_selection;
   union {
      struct { char folder_path[PATH_MAX_LENGTH]; } main_menu;
      struct { char folder_path[PATH_MAX_LENGTH]; char core_path[PATH_MAX_LENGTH]; } folder;
      struct { char content_path[PATH_MAX_LENGTH]; } core_select;
   } data;
} streamlined_view_t;

#define STREAMLINED_VIEW_STACK_MAX 8

typedef struct
{
   streamlined_view_t entries[STREAMLINED_VIEW_STACK_MAX];
   int top;  /* Index of current top (-1 = empty) */
} streamlined_view_stack_t;

/* Interstitial screen types */
typedef enum
{
   STREAMLINED_INTERSTITIAL_NONE,
   STREAMLINED_INTERSTITIAL_LOADING,
   STREAMLINED_INTERSTITIAL_EXITING,
   STREAMLINED_INTERSTITIAL_SAVING_AND_EXITING
} streamlined_interstitial_t;

/* Auto savestate cache: tracks whether the selected game has an auto save */
typedef struct
{
   bool has_auto_save;   /* Does the selected entry have an auto savestate? */
   size_t selection;     /* Selection index when cache was last computed */
} streamlined_auto_save_cache_t;

/* Resume state: persists across content load/unload to restore folder view */
typedef struct
{
   bool active;
   char folder_path[PATH_MAX_LENGTH];
   size_t selection;
} streamlined_resume_t;

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

   /* Navigation state */
   streamlined_view_stack_t view_stack;
   streamlined_resume_t resume;

   /* Save slot selector */
   gfx_thumbnail_t savestate_thumbnail;
   char savestate_thumbnail_path[PATH_MAX_LENGTH];
   int preview_slot;              /* Currently previewed slot (0-7) */
   bool show_slot_selector;       /* True when on Save/Load State entry */
   size_t last_selection;         /* Track selection changes */

   /* Auto savestate detection */
   streamlined_auto_save_cache_t auto_save_cache;

   /* Ticker for text scrolling (uses RetroArch's built-in animation system) */
   uint64_t ticker_idx;           /* Incremented each frame for ticker animation */
   uint64_t item_ticker_start;    /* ticker_idx when current item was selected */
   size_t item_ticker_selection;  /* Track which item is being ticker-scrolled */

   /* Interstitial screen state */
   streamlined_interstitial_t interstitial;
   bool interstitial_triggered;
   /* Deferred loading parameters */
   char loading_core_path[PATH_MAX_LENGTH];
   char loading_content_path[PATH_MAX_LENGTH];
   bool loading_is_resume;

} streamlined_t;

/* Number of save slots to display (Auto + slots 0-7) */
#define STREAMLINED_NUM_SLOTS 9
#define STREAMLINED_AUTO_SLOT_INDEX 0  /* First dot is the auto slot (state_slot -1) */

/* View stack operations */
static streamlined_view_t *streamlined_view_current(
      streamlined_view_stack_t *stack)
{
   if (stack->top < 0)
      return NULL;
   return &stack->entries[stack->top];
}

static streamlined_view_t *streamlined_view_push(
      streamlined_view_stack_t *stack, streamlined_view_type_t type)
{
   if (stack->top >= STREAMLINED_VIEW_STACK_MAX - 1)
      return NULL;
   stack->top++;
   memset(&stack->entries[stack->top], 0, sizeof(streamlined_view_t));
   stack->entries[stack->top].type = type;
   return &stack->entries[stack->top];
}

static streamlined_view_t *streamlined_view_pop(
      streamlined_view_stack_t *stack)
{
   if (stack->top <= 0)
      return NULL;  /* Don't pop the root view */
   stack->top--;
   return &stack->entries[stack->top];
}

/*
 * Push/pop a navigation marker on the RA menu stack.
 * On tvOS, cocoa_common.m's menuIsAtTop checks menu_stack size to decide
 * whether the Menu button should background the app. Since the streamlined
 * driver manages its own view stack, the RA stack stays at size 1.
 * Pushing a marker makes size > 1, preventing the system from backgrounding.
 */
static void streamlined_push_nav_marker(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   if (menu_st && menu_st->entries.list)
   {
      file_list_t *stack = MENU_LIST_GET(menu_st->entries.list, 0);
      if (stack)
         file_list_append(stack,
               msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU),
               msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU),
               MENU_ENUM_LABEL_MAIN_MENU, 0, 0);
   }
}

static void streamlined_pop_nav_marker(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   if (menu_st && menu_st->entries.list)
   {
      file_list_t *stack = MENU_LIST_GET(menu_st->entries.list, 0);
      if (stack && stack->size > 1)
         file_list_pop(stack, NULL);
   }
}

/* ======================================================================
 * DRAWING FUNCTIONS
 * ====================================================================== */

/* Forward declarations */
static const char *streamlined_strip_sort_prefix(const char *name);
static bool streamlined_get_auto_savestate_path(
      const char *content_path, const char *core_path,
      char *out_path, size_t out_size);
static void streamlined_launch_content(streamlined_t *strm,
      struct menu_state *menu_st,
      const char *core_path, const char *content_path,
      bool load_auto_savestate);
static bool streamlined_read_game_core(
      const char *content_path, char *core_path_out,
      size_t core_path_size);
static bool streamlined_resolve_core_for_content(
      const char *content_path, const char *folder_core_path,
      char *core_path_out, size_t core_path_size);

static void streamlined_draw_text(streamlined_t *strm,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color, bool small_font);
static int streamlined_get_text_width(streamlined_t *strm, const char *text, bool small_font);

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

static int streamlined_get_text_width(streamlined_t *strm, const char *text, bool small_font)
{
   font_data_t *font = small_font ? strm->font_small.font : strm->font.font;
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
   if (string_is_equal(value, "(DIR)"))
   {
      /* Add leading slash to indicate directory */
      size_t len = strlen(label);
      if (len + 1 < label_size)
      {
         memmove(label + 1, label, len + 1);
         label[0] = '/';
      }
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
      char *out, size_t out_size, int max_width, bool small_font)
{
   int text_width;
   size_t len;

   if (!text || !out || out_size == 0)
      return;

   strlcpy(out, text, out_size);
   text_width = streamlined_get_text_width(strm, out, small_font);

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
      text_width = streamlined_get_text_width(strm, out, small_font);
   }
}

/*
 * Split sublabel text into up to two balanced lines for footer display.
 * If the text fits within max_width, line1 gets the full text and line2 is empty.
 * If not, splits at a word boundary near the middle for balanced legibility.
 * Lines that still exceed max_width are truncated with ellipsis.
 */
static void streamlined_sublabel_wrap(
      streamlined_t *strm,
      const char *text,
      int max_width,
      char *line1, size_t line1_size,
      char *line2, size_t line2_size)
{
   int full_width;
   size_t len, half_len;
   size_t split_pos;
   int left_dist, right_dist;
   size_t left_space, right_space;
   bool found_left, found_right;

   line1[0] = '\0';
   line2[0] = '\0';

   if (!text || !text[0] || !strm->font_small.font)
      return;

   full_width = font_driver_get_message_width(
         strm->font_small.font, text, strlen(text), 1.0f);

   /* Fits on one line */
   if (full_width <= max_width)
   {
      strlcpy(line1, text, line1_size);
      return;
   }

   /* Find a space near the middle of the string */
   len = strlen(text);
   half_len = len / 2;

   found_left  = false;
   found_right = false;
   left_space  = 0;
   right_space = 0;

   /* Search left from middle */
   {
      size_t j = half_len;
      while (j > 0)
      {
         if (text[j] == ' ')
         {
            left_space = j;
            found_left = true;
            break;
         }
         j--;
      }
   }

   /* Search right from middle */
   {
      size_t j = half_len + 1;
      while (j < len)
      {
         if (text[j] == ' ')
         {
            right_space = j;
            found_right = true;
            break;
         }
         j++;
      }
   }

   /* Pick the closer space to the middle */
   if (found_left && found_right)
   {
      left_dist  = (int)(half_len - left_space);
      right_dist = (int)(right_space - half_len);
      split_pos  = (left_dist <= right_dist) ? left_space : right_space;
   }
   else if (found_left)
      split_pos = left_space;
   else if (found_right)
      split_pos = right_space;
   else
   {
      /* No space found at all - single line, truncated */
      streamlined_truncate_text(strm, text, line1, line1_size, max_width, true);
      return;
   }

   /* Split into two lines at the space */
   if (split_pos < line1_size)
   {
      memcpy(line1, text, split_pos);
      line1[split_pos] = '\0';
   }
   else
      strlcpy(line1, text, line1_size);

   strlcpy(line2, text + split_pos + 1, line2_size);

   /* Truncate either line if it exceeds max_width */
   {
      int w1 = font_driver_get_message_width(
            strm->font_small.font, line1, strlen(line1), 1.0f);
      if (w1 > max_width)
         streamlined_truncate_text(strm, line1, line1, line1_size, max_width, true);
   }
   {
      int w2 = font_driver_get_message_width(
            strm->font_small.font, line2, strlen(line2), 1.0f);
      if (w2 > max_width)
         streamlined_truncate_text(strm, line2, line2, line2_size, max_width, true);
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
 * Draw the save slot selector UI: thumbnail preview with polaroid frame and dot indicators.
 * Positioned on the right side of the screen, vertically centered.
 */
static void streamlined_draw_slot_selector(streamlined_t *strm,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   int i;
   int thumb_max_height = (int)(video_height * STREAMLINED_THUMB_HEIGHT_RATIO);
   int thumb_max_width  = (int)(thumb_max_height * STREAMLINED_THUMB_ASPECT_RATIO);

   /* Polaroid frame dimensions */
   int frame_border     = (int)(STREAMLINED_FRAME_BORDER_BASE * strm->scale_factor);
   int frame_bottom     = (int)(STREAMLINED_FRAME_CHIN_BASE * strm->scale_factor);
   int frame_width      = thumb_max_width + frame_border * 2;
   int frame_height     = thumb_max_height + frame_border + frame_bottom;

   int frame_x, frame_y;
   int thumb_x, thumb_y;
   int dot_y, dot_spacing, dot_radius;
   int total_dots_width;
   int dots_start_x;

   /* Calculate dot dimensions first (needed for vertical centering) */
   dot_radius  = (int)(STREAMLINED_DOT_RADIUS_BASE * strm->scale_factor);
   dot_spacing = (int)(STREAMLINED_DOT_SPACING_BASE * strm->scale_factor);

   /* Position frame on right side, vertically centered with dots below */
   frame_x = video_width - strm->margin_x - frame_width;
   frame_y = (video_height - frame_height - dot_radius * 2 - (int)(STREAMLINED_DOT_SPACING_BASE * strm->scale_factor)) / 2;

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
      int preview_state_slot = strm->preview_slot - 1;

      /* Check if save state exists (without .png) to determine message */
      if (runloop_get_savestate_path(state_path, sizeof(state_path), preview_state_slot)
            && path_is_valid(state_path))
         placeholder = "No Screenshot";
      else
         placeholder = "Empty";

      /* Draw placeholder text centered in thumbnail area */
      {
         int text_width = streamlined_get_text_width(strm, placeholder, false);
         int text_x = thumb_x + (thumb_max_width - text_width) / 2;
         /* Center vertically: account for font baseline by adding ~1/3 of font size */
         int text_y = thumb_y + thumb_max_height / 2 + (int)(strm->font_size * STREAMLINED_TEXT_VCENTER);

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
         int text_y = cy + (int)(strm->font_size_tiny * STREAMLINED_TEXT_VCENTER);
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
   streamlined_view_t *view;
   streamlined_view_type_t vtype;

   if (!strm->font.font || !p_disp || !menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list || list->size == 0)
      return;

   list_size = list->size;
   selection = menu_st->selection_ptr;
   item_height = strm->font.line_height;
   if (item_height <= 0)
      item_height = 20;

   view = streamlined_view_current(&strm->view_stack);
   vtype = view ? view->type : STREAMLINED_VIEW_RA_SETTINGS;

   /*
    * Detect if on Save (index 1) or Load (index 2) in main quick menu.
    * Show the slot selector UI when these entries are selected.
    */
   {
      bool was_showing = strm->show_slot_selector;
      strm->show_slot_selector = false;

      if (vtype == STREAMLINED_VIEW_QUICK_MENU)
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

   /* Detect auto savestate for current selection in FOLDER views */
   if (vtype == STREAMLINED_VIEW_FOLDER && view)
   {
      if (selection != strm->auto_save_cache.selection)
      {
         strm->auto_save_cache.selection = selection;
         strm->auto_save_cache.has_auto_save = false;

         {
            char resolved_core[PATH_MAX_LENGTH];
            menu_entry_t check_entry;
            MENU_ENTRY_INITIALIZE(check_entry);
            check_entry.flags |= MENU_ENTRY_FLAG_LABEL_ENABLED;
            menu_entry_get(&check_entry, 0, (unsigned)selection, NULL, true);

            if (!string_is_empty(check_entry.label)
                  && path_is_valid(check_entry.label)
                  && !path_is_directory(check_entry.label)
                  && streamlined_resolve_core_for_content(
                        check_entry.label,
                        view->data.folder.core_path,
                        resolved_core, sizeof(resolved_core)))
            {
               char auto_state_path[PATH_MAX_LENGTH];
               if (streamlined_get_auto_savestate_path(
                        check_entry.label,
                        resolved_core,
                        auto_state_path,
                        sizeof(auto_state_path)))
                  strm->auto_save_cache.has_auto_save =
                        path_is_valid(auto_state_path);
            }
         }
      }
   }
   else
   {
      strm->auto_save_cache.selection = (size_t)-1;
      strm->auto_save_cache.has_auto_save = false;
   }

   /* Calculate visible items: screen height minus title area and button legend area */
   {
      int title_area = strm->margin_y + (int)(strm->font_size_title * STREAMLINED_TITLE_AREA_RATIO);
      int bottom_area = (int)(STREAMLINED_FOOTER_HEIGHT_BASE * strm->scale_factor);
      max_visible = (video_height - title_area - bottom_area) / item_height;
   }
   if (max_visible == 0)
      max_visible = 1;

   /* Get title based on current view */
   title_buf[0] = '\0';
   switch (vtype)
   {
      case STREAMLINED_VIEW_CORE_SELECT:
         strlcpy(title_buf, "Select Core", sizeof(title_buf));
         break;
      case STREAMLINED_VIEW_ADVANCED:
         strlcpy(title_buf, "Advanced", sizeof(title_buf));
         break;
      case STREAMLINED_VIEW_MAIN_SETTINGS:
         strlcpy(title_buf, "Settings", sizeof(title_buf));
         break;
      case STREAMLINED_VIEW_FOLDER:
      {
         const char *folder_name = path_basename(view->data.folder.folder_path);
         if (!string_is_empty(folder_name))
         {
            const char *clean_name = streamlined_strip_sort_prefix(folder_name);
            strlcpy(title_buf, clean_name, sizeof(title_buf));
         }
         break;
      }
      case STREAMLINED_VIEW_MAIN_MENU:
         strlcpy(title_buf, "RetroArch", sizeof(title_buf));
         break;
      case STREAMLINED_VIEW_QUICK_MENU:
      {
         const char *content_path = path_get(RARCH_PATH_CONTENT);
         if (!string_is_empty(content_path))
         {
            const char *game_name = path_basename(content_path);
            if (!string_is_empty(game_name))
            {
               char *ext;
               strlcpy(title_buf, game_name, sizeof(title_buf));
               ext = strrchr(title_buf, '.');
               if (ext)
                  *ext = '\0';
            }
         }
         if (title_buf[0] == '\0')
            strlcpy(title_buf, "Quick Menu", sizeof(title_buf));
         break;
      }
      default:
      {
         menu_entries_get_title(title_buf, sizeof(title_buf));

         /* Strip "Select File: " prefix (localized) from file browser titles */
         {
            const char *select_file = msg_hash_to_str(MENU_ENUM_LABEL_VALUE_SELECT_FILE);
            size_t prefix_len = strlen(select_file);

            if (strncmp(title_buf, select_file, prefix_len) == 0
                  && title_buf[prefix_len] == ':'
                  && title_buf[prefix_len + 1] == ' ')
            {
               memmove(title_buf, title_buf + prefix_len + 2,
                     strlen(title_buf + prefix_len + 2) + 1);
            }
         }
         break;
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

   /* Calculate scroll */
   if (selection >= max_visible)
      start_idx = selection - max_visible + 1;
   else
      start_idx = 0;

   /* Draw menu entries - tight spacing below title */
   y = strm->margin_y + (int)(strm->font_size_title * STREAMLINED_TITLE_AREA_RATIO);

   for (i = 0; i < max_visible && (start_idx + i) < list_size; i++)
   {
      menu_entry_t entry;
      const char *entry_label;
      char display_label[256];
      char *ptr;
      bool is_selected = ((start_idx + i) == selection);

      /* Calculate consistent text position */
      int pill_height = (int)(strm->font_size * STREAMLINED_PILL_HEIGHT_RATIO);
      int pill_y = y + (item_height - pill_height) / 2;
      int text_y = pill_y + pill_height / 2 + (int)(strm->font_size * STREAMLINED_TEXT_VCENTER_PILL);

      MENU_ENTRY_INITIALIZE(entry);
      entry.flags |= MENU_ENTRY_FLAG_RICH_LABEL_ENABLED
                   | MENU_ENTRY_FLAG_VALUE_ENABLED;
      menu_entry_get(&entry, 0, (unsigned)(start_idx + i), NULL, true);

      /* For custom menus, prefer path (our custom label) over rich_label (RA's label)
       * For custom main menu, use label (display name) since path contains full file path
       * For core selection, use path (display name) since label contains core path
       * For main settings submenu, use path (our custom label) */
      if (vtype == STREAMLINED_VIEW_QUICK_MENU
            || vtype == STREAMLINED_VIEW_ADVANCED
            || vtype == STREAMLINED_VIEW_CORE_SELECT
            || vtype == STREAMLINED_VIEW_MAIN_SETTINGS)
      {
         entry_label = entry.path;
         /* Core Options has empty path - use rich_label instead */
         if (string_is_empty(entry_label) && !string_is_empty(entry.rich_label))
            entry_label = entry.rich_label;
      }
      else if ((vtype == STREAMLINED_VIEW_MAIN_MENU
            || vtype == STREAMLINED_VIEW_FOLDER)
            && !string_is_empty(entry.label))
         entry_label = entry.label;
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
      if (vtype != STREAMLINED_VIEW_MAIN_MENU)
         streamlined_process_entry_type(entry.value, display_label, sizeof(display_label));

      /* Check if value should be displayed */
      {
         bool show_value;
         int max_value_width;
         int value_gap;
         int max_label_width;

         show_value = !string_is_empty(entry.value)
                        && !string_is_equal(entry.value, "...")
                        && !streamlined_should_hide_value(entry.value);
         max_value_width = (video_width - strm->margin_x * 2) * STREAMLINED_VALUE_WIDTH_PCT / 100;
         value_gap = (int)(STREAMLINED_DOT_SPACING_BASE * strm->scale_factor);
         max_label_width = show_value
               ? (video_width - strm->margin_x * 2 - max_value_width - value_gap)
               : (video_width - strm->margin_x * 2);

      if (is_selected)
      {
         int pill_width;
         int text_width = streamlined_get_text_width(strm, display_label, false);

         /*
          * Pill width calculation:
          * - With value: spans from label to value (full content width + padding)
          * - Without value: fits snugly around label text with symmetric padding
          */
         if (show_value)
            pill_width = video_width - strm->margin_x * 2 + strm->pill_padding * 2;
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
            gfx_animation_ctx_ticker_smooth_t ticker;

            /* Reset ticker when selection changes so scrolling starts from left */
            if (selection != strm->item_ticker_selection)
            {
               strm->item_ticker_selection = selection;
               strm->item_ticker_start = strm->ticker_idx;
            }
            item_idx = strm->ticker_idx - strm->item_ticker_start;

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

            streamlined_truncate_text(strm, entry.value, truncated_value,
                  sizeof(truncated_value), max_value_width, false);
            value_width = streamlined_get_text_width(strm, truncated_value, false);

            streamlined_draw_text(strm, p_disp, video_width, video_height,
                  video_width - strm->margin_x - value_width, text_y,
                  truncated_value, streamlined_color_text_dark, false);
         }
      }
      else
      {
         /* Non-selected: truncate long labels */
         char truncated_label[256];

         streamlined_truncate_text(strm, display_label, truncated_label,
               sizeof(truncated_label), max_label_width, false);

         streamlined_draw_text(strm, p_disp, video_width, video_height,
               strm->margin_x, text_y,
               truncated_label, streamlined_color_text, false);

         if (show_value)
         {
            char truncated_value[256];
            int value_width;

            streamlined_truncate_text(strm, entry.value, truncated_value,
                  sizeof(truncated_value), max_value_width, false);
            value_width = streamlined_get_text_width(strm, truncated_value, false);

            streamlined_draw_text(strm, p_disp, video_width, video_height,
                  video_width - strm->margin_x - value_width, text_y,
                  truncated_value, streamlined_color_text, false);
         }
      }
      } /* end show_value scope */

      y += item_height;
   }

   /* Draw save slot selector if on Save/Load State entry */
   if (strm->show_slot_selector)
      streamlined_draw_slot_selector(strm, p_disp, userdata, video_width, video_height);

   /* Footer - Back on left, OK on right, white pills with black letter + white label */
   {
      float scale            = strm->scale_factor;
      float footer_height    = STREAMLINED_FOOTER_HEIGHT_BASE * scale;
      float footer_margin    = STREAMLINED_FOOTER_MARGIN_BASE * scale;
      float pill_h           = strm->font_size_small + STREAMLINED_FOOTER_GAP * scale;
      float pill_pad         = STREAMLINED_FOOTER_PILL_PAD * scale;
      float pill_text_gap    = STREAMLINED_FOOTER_GAP * scale;

      float footer_center_y  = (float)video_height - (footer_height / 2.0f);
      float pill_y           = footer_center_y - (pill_h / 2.0f);
      float text_y           = footer_center_y + (strm->font_size_small * STREAMLINED_TEXT_VCENTER);

      int back_key_w, ok_key_w;
      int back_pill_w, ok_pill_w;
      float ok_pill_x        = 0;
      float left_end         = 0;
      const char *back_key   = "B";
      const char *ok_key;
      const char *back_str   = msg_hash_to_str(
            MENU_ENUM_LABEL_VALUE_BASIC_MENU_CONTROLS_BACK);
      const char *ok_str;

      /* Determine right-side hint(s) based on auto save state:
       * - auto_load ON + auto save: (X) Resume only (A also resumes via runloop)
       * - auto_load OFF + auto save: (X) Resume AND (A) Play
       * - no auto save: (A) Play for game files, (A) OK otherwise */
      {
         bool show_resume = strm->auto_save_cache.has_auto_save
               && vtype == STREAMLINED_VIEW_FOLDER;
         settings_t *footer_settings = show_resume ? config_get_ptr() : NULL;
         bool auto_load_on = footer_settings
               && footer_settings->bools.savestate_auto_load;

         if (show_resume && auto_load_on)
         {
            ok_key = "X";
            ok_str = "Resume";
         }
         else
         {
            ok_key = "A";
            if ((vtype == STREAMLINED_VIEW_MAIN_MENU
                  || vtype == STREAMLINED_VIEW_FOLDER)
                  && selection < list_size
                  && list->list[selection].type == FILE_TYPE_PLAIN)
               ok_str = "Play";
            else
               ok_str = msg_hash_to_str(
                     MENU_ENUM_LABEL_VALUE_BASIC_MENU_CONTROLS_OK);
         }
      }

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

      /* Compute where the left hint group ends (for sublabel centering) */
      {
         int back_label_w = font_driver_get_message_width(
               strm->font_small.font, back_str, strlen(back_str), 1.0f);
         left_end = footer_margin + (float)back_pill_w
               + pill_text_gap + (float)back_label_w;
      }

      /* Right side hint(s) */
      {
         bool show_resume_hint = strm->auto_save_cache.has_auto_save
               && vtype == STREAMLINED_VIEW_FOLDER;
         int ok_label_w = font_driver_get_message_width(
               strm->font_small.font, ok_str, strlen(ok_str), 1.0f);
         ok_pill_x = (float)video_width - footer_margin
               - (float)ok_label_w - pill_text_gap - (float)ok_pill_w;

         /* Draw the primary right-side hint: either (X) Resume or (A) Play/OK */
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

         /* When auto_load is OFF and auto save exists, draw (X) Resume
          * to the left of (A) Play */
         if (show_resume_hint && !string_is_equal(ok_key, "X"))
         {
            const char *resume_key = "X";
            const char *resume_str = "Resume";
            int resume_key_w = font_driver_get_message_width(
                  strm->font_small.font, resume_key, strlen(resume_key), 1.0f);
            int resume_label_w = font_driver_get_message_width(
                  strm->font_small.font, resume_str, strlen(resume_str), 1.0f);
            int resume_pill_w = resume_key_w + (int)(pill_pad * 2.0f);
            float resume_pill_x = ok_pill_x - pill_text_gap
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

      /* Sublabel hint for settings views - centered between button hints */
      if (vtype == STREAMLINED_VIEW_MAIN_SETTINGS
            || vtype == STREAMLINED_VIEW_ADVANCED
            || vtype == STREAMLINED_VIEW_RA_SETTINGS)
      {
         menu_entry_t sublabel_entry;
         MENU_ENTRY_INITIALIZE(sublabel_entry);
         sublabel_entry.flags |= MENU_ENTRY_FLAG_SUBLABEL_ENABLED;
         menu_entry_get(&sublabel_entry, 0, (unsigned)selection, NULL, true);

         if (sublabel_entry.sublabel[0] != '\0')
         {
            int sublabel_avail = (int)(ok_pill_x - left_end
                  - pill_text_gap * 2.0f);

            if (sublabel_avail > (int)(strm->font_size_small * 3))
            {
               char sl_line1[512];
               char sl_line2[512];
               int sl_w1;
               float center_x;

               sl_line1[0] = '\0';
               sl_line2[0] = '\0';

               streamlined_sublabel_wrap(strm, sublabel_entry.sublabel,
                     sublabel_avail,
                     sl_line1, sizeof(sl_line1),
                     sl_line2, sizeof(sl_line2));

               center_x = left_end + (ok_pill_x - left_end) / 2.0f;

               if (sl_line2[0] != '\0')
               {
                  /* Two lines: center the pair vertically in footer */
                  float line_spacing = strm->font_size_small * 1.2f;
                  float sl_y1 = footer_center_y - (line_spacing / 2.0f)
                        + (strm->font_size_small * STREAMLINED_TEXT_VCENTER);
                  float sl_y2 = sl_y1 + line_spacing;
                  int sl_w2;

                  sl_w1 = font_driver_get_message_width(
                        strm->font_small.font,
                        sl_line1, strlen(sl_line1), 1.0f);
                  gfx_display_draw_text(strm->font_small.font,
                        sl_line1,
                        (int)(center_x - (float)sl_w1 / 2.0f),
                        (int)sl_y1,
                        video_width, video_height,
                        streamlined_color_text_muted,
                        TEXT_ALIGN_LEFT, 1.0f, false, 0, false);

                  sl_w2 = font_driver_get_message_width(
                        strm->font_small.font,
                        sl_line2, strlen(sl_line2), 1.0f);
                  gfx_display_draw_text(strm->font_small.font,
                        sl_line2,
                        (int)(center_x - (float)sl_w2 / 2.0f),
                        (int)sl_y2,
                        video_width, video_height,
                        streamlined_color_text_muted,
                        TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
               }
               else
               {
                  /* Single line: same baseline as button hints */
                  sl_w1 = font_driver_get_message_width(
                        strm->font_small.font,
                        sl_line1, strlen(sl_line1), 1.0f);
                  gfx_display_draw_text(strm->font_small.font,
                        sl_line1,
                        (int)(center_x - (float)sl_w1 / 2.0f),
                        (int)text_y,
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

/*
 * Get the current menu selection list and clear it.
 * Returns the cleared file_list_t* or NULL on failure.
 */
static file_list_t *streamlined_get_and_clear_menu_list(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;

   if (!menu_st)
      return NULL;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return NULL;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return NULL;

   menu_entries_clear(list);
   return list;
}

static void streamlined_populate_menu_items(const streamlined_quick_item_t *items)
{
   file_list_t *list = streamlined_get_and_clear_menu_list();
   size_t i;

   if (!list)
      return;

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

/* Populate quick menu, with dynamic Exit/Quit and conditional Save and Quit */
static void streamlined_populate_quick_menu(void)
{
   file_list_t *list = streamlined_get_and_clear_menu_list();
   const streamlined_quick_item_t *item;
   bool from_cli = streamlined_is_launched_from_cli();
   settings_t *settings = config_get_ptr();
   bool auto_save = settings->bools.savestate_auto_save;

   if (!list)
      return;

   for (item = streamlined_quick_menu_items;
        item->label != NULL
           || item->action == STREAMLINED_EXIT_MARKER
           || item->action == STREAMLINED_SAVE_AND_QUIT_MARKER;
        item++)
   {
      const char *label;
      const char *action_label;
      enum msg_hash_enums action;

      /* Handle dynamic Quit entry - quits if CLI, exits to menu if not */
      if (item->action == STREAMLINED_EXIT_MARKER)
      {
         label = "Quit";
         action = from_cli ? MENU_ENUM_LABEL_QUIT_RETROARCH : MENU_ENUM_LABEL_CLOSE_CONTENT;
         action_label = msg_hash_to_str(action);
      }
      /* Handle "Save and Quit" - only shown when savestate_auto_save is on */
      else if (item->action == STREAMLINED_SAVE_AND_QUIT_MARKER)
      {
         if (!auto_save)
            continue;
         label = "Save and Quit";
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
   file_list_t *list = streamlined_get_and_clear_menu_list();
   const streamlined_quick_item_t *item;
   bool show_disc_control = streamlined_is_disc_control_available();

   if (!list)
      return;

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
   file_list_t *list = streamlined_get_and_clear_menu_list();
   const streamlined_quick_item_t *item;

   if (!list)
      return;

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
 * Read core path from a game-specific core override file.
 * Looks for .core.<filename>.txt in the same directory as the game.
 * Uses the same line-by-line parsing as streamlined_read_folder_core().
 *
 * Example: for /roms/SNES/Super Mario World.sfc,
 *          reads /roms/SNES/.core.Super Mario World.sfc.txt
 *
 * Returns true if a valid core path was found and written to core_path_out.
 */
static bool streamlined_read_game_core(const char *content_path, char *core_path_out, size_t core_path_size)
{
   char dir[DIR_MAX_LENGTH];
   char game_core_filename[PATH_MAX_LENGTH];
   char game_core_path[PATH_MAX_LENGTH];
   const char *basename;
   RFILE *file;
   char line[PATH_MAX_LENGTH];

   if (string_is_empty(content_path))
      return false;

   basename = path_basename(content_path);
   if (string_is_empty(basename))
      return false;

   /* Build path: <dir>/.core.<filename>.txt */
   fill_pathname_basedir(dir, content_path, sizeof(dir));
   snprintf(game_core_filename, sizeof(game_core_filename),
         ".core.%s.txt", basename);
   fill_pathname_join_special(game_core_path, dir,
         game_core_filename, sizeof(game_core_path));

   if (!path_is_valid(game_core_path))
      return false;

   file = filestream_open(game_core_path,
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return false;

   while (filestream_gets(file, line, sizeof(line)))
   {
      string_trim_whitespace(line);

      if (string_is_empty(line))
         continue;

      if (line[0] == '#')
         continue;

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
 * Resolve the effective core for a content file.
 * Checks for a game-specific core override first (.core.<filename>.txt),
 * then falls back to the folder's core path.
 *
 * Returns true if a core was resolved and written to core_path_out.
 */
static bool streamlined_resolve_core_for_content(
      const char *content_path, const char *folder_core_path,
      char *core_path_out, size_t core_path_size)
{
   /* Try game-specific core first */
   if (streamlined_read_game_core(content_path, core_path_out, core_path_size))
      return true;

   /* Fall back to folder core */
   if (!string_is_empty(folder_core_path))
   {
      strlcpy(core_path_out, folder_core_path, core_path_size);
      return true;
   }

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
 * Compute the auto savestate path for a content file and core path.
 * Mirrors the logic in runloop_path_set_redirect() for savestate directory
 * resolution, but works before content is loaded by using core_info
 * instead of the loaded core's system info.
 *
 * Respects: savestates_in_content_dir, sort_savestates_enable,
 *           sort_savestates_by_content_enable, directory_savestate
 *
 * Returns true if path was computed successfully.
 */
static bool streamlined_get_auto_savestate_path(
      const char *content_path, const char *core_path,
      char *out_path, size_t out_size)
{
   settings_t *settings           = config_get_ptr();
   bool savestates_in_content_dir = settings->bools.savestates_in_content_dir;
   bool sort_savestates           = settings->bools.sort_savestates_enable;
   bool sort_savestates_by_content = settings->bools.sort_savestates_by_content_enable;
   char savestate_dir[DIR_MAX_LENGTH];
   char content_no_ext[PATH_MAX_LENGTH];

   if (string_is_empty(content_path))
      return false;

   /* Strip extension from content path (mirroring runtime_content_path_basename) */
   strlcpy(content_no_ext, content_path, sizeof(content_no_ext));
   path_remove_extension(content_no_ext);

   /* Start with configured savestate directory */
   {
      const char *configured_dir = dir_get_ptr(RARCH_DIR_SAVESTATE);
      if (!string_is_empty(configured_dir))
         strlcpy(savestate_dir, configured_dir, sizeof(savestate_dir));
      else
         savestate_dir[0] = '\0';
   }

   /* If savestates_in_content_dir or directory is empty, use content's directory */
   if (string_is_empty(savestate_dir) || savestates_in_content_dir)
      fill_pathname_basedir(savestate_dir, content_path, sizeof(savestate_dir));

   /* Per-content-directory sorting: append content's parent dir name */
   if (sort_savestates_by_content && !string_is_empty(content_no_ext))
   {
      char content_dir_name[DIR_MAX_LENGTH];
      content_dir_name[0] = '\0';
      fill_pathname_parent_dir_name(content_dir_name, content_no_ext,
            sizeof(content_dir_name));
      if (!string_is_empty(content_dir_name))
         fill_pathname_join_special(savestate_dir, savestate_dir,
               content_dir_name, sizeof(savestate_dir));
   }

   /* Per-core sorting: append core's library name */
   if (sort_savestates && !string_is_empty(core_path))
   {
      core_info_t *core_info = NULL;
      if (core_info_find(core_path, &core_info)
            && core_info && !string_is_empty(core_info->core_name))
         fill_pathname_join(savestate_dir, savestate_dir,
               core_info->core_name, sizeof(savestate_dir));
   }

   /* Build the savestate path */
   if (path_is_directory(savestate_dir))
   {
      /* Redirect into the computed directory */
      strlcpy(out_path, savestate_dir, out_size);
      fill_pathname_dir(out_path, content_no_ext,
            ".state", out_size);
   }
   else
   {
      /* Use content's directory with extension replaced */
      fill_pathname(out_path, content_no_ext,
            ".state", out_size);
   }

   /* Append .auto for the auto save slot */
   strlcat(out_path, ".auto", out_size);

   return true;
}

/*
 * Populate the menu with a list of all installed cores.
 * Used when the user needs to select which core to use for a folder.
 */
static void streamlined_populate_core_selection(streamlined_t *strm, const char *content_path)
{
   file_list_t *list = streamlined_get_and_clear_menu_list();
   core_info_list_t *core_info_list = NULL;
   size_t i;

   if (!list)
      return;

   /* Get list of all installed cores */
   core_info_get_list(&core_info_list);

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

   /* Sort cores alphabetically by display name */
   core_info_qsort(core_info_list, CORE_INFO_LIST_SORT_DISPLAY_NAME);

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

/* Populate custom main menu with folders and files from the specified directory
 * show_folder_slash: if true, prefix folder names with "/" */
static void streamlined_populate_folder_menu(streamlined_t *strm, const char *directory, bool show_folder_slash)
{
   settings_t *settings = config_get_ptr();
   file_list_t *list;
   struct string_list *str_list;
   unsigned i;

   if (!directory || directory[0] == '\0')
      return;

   list = streamlined_get_and_clear_menu_list();
   if (!list)
      return;

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
         else
         {
            /* Show files (ROMs) - strip extension for cleaner display */
            char display_name[256];
            strlcpy(display_name, name, sizeof(display_name));
            path_remove_extension(display_name);

            menu_entries_append(list,
                  display_name,     /* Display name (entry->path for rendering) */
                  path,             /* Full path (entry->label for loading) */
                  MSG_UNKNOWN,
                  FILE_TYPE_PLAIN,
                  0, 0, NULL);
         }
      }
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

/* Free all loaded fonts and NULL their pointers */
static void streamlined_free_fonts(streamlined_t *strm)
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
}

#if TARGET_OS_TV
/*
 * Resolve the tvOS system font file path at runtime.
 * Uses CTFontCreateUIFontForLanguage to get the system font, then
 * extracts its on-disk path. This avoids hardcoding a path that may
 * change across tvOS versions.
 */
static bool streamlined_get_system_font_path(char *out, size_t out_size)
{
   CTFontRef font;
   CFURLRef  url;
   Boolean   ok;

   font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 0, NULL);
   if (!font)
      return false;

   url = (CFURLRef)CTFontCopyAttribute(font, kCTFontURLAttribute);
   CFRelease(font);
   if (!url)
      return false;

   ok = CFURLGetFileSystemRepresentation(url, true, (UInt8 *)out, (CFIndex)out_size);
   CFRelease(url);
   return (bool)ok;
}
#endif

/*
 * Try to load a font from the given path within the assets directory.
 * Returns the loaded font or NULL if not found.
 */
static font_data_t *streamlined_try_load_font(gfx_display_t *p_disp,
      const char *assets_dir, const char *font_subpath,
      float font_size, bool is_threaded, char *fontpath_out, size_t fontpath_size)
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
   strm->view_stack.top = -1;

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
   strm->font_size_small = STREAMLINED_BASE_FONT_SIZE * scale_factor * STREAMLINED_FONT_SMALL_RATIO;
   strm->font_size_title = STREAMLINED_BASE_FONT_SIZE * scale_factor * STREAMLINED_FONT_TITLE_RATIO;
   /* Tiny font sized to match dot indicators (dot_radius * 2 is diameter) */
   strm->font_size_tiny = STREAMLINED_DOT_RADIUS_BASE * scale_factor * STREAMLINED_FONT_TINY_RATIO;

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
   streamlined_free_fonts(strm);

   fontpath[0] = '\0';

   /*
    * Font loading priority:
    * tvOS: System font via CoreText (native look, path-independent across OS versions)
    * Fallback: asset fonts (streamlined → xmb → ozone)
    */
#if TARGET_OS_TV
   {
      char sys_fontpath[PATH_MAX_LENGTH];
      if (streamlined_get_system_font_path(sys_fontpath, sizeof(sys_fontpath)))
      {
         strm->font.font = gfx_display_font_file(p_disp, sys_fontpath,
               strm->font_size, is_threaded);
         if (strm->font.font)
            strlcpy(fontpath, sys_fontpath, sizeof(fontpath));
      }
   }
#endif

   if (!strm->font.font)
      strm->font.font = streamlined_try_load_font(p_disp,
            settings->paths.directory_assets, "streamlined/font.ttf",
            strm->font_size, is_threaded, fontpath, sizeof(fontpath));

   if (!strm->font.font)
      strm->font.font = streamlined_try_load_font(p_disp,
            settings->paths.directory_assets, "xmb/monochrome/font.ttf",
            strm->font_size, is_threaded, fontpath, sizeof(fontpath));

   if (!strm->font.font)
      strm->font.font = streamlined_try_load_font(p_disp,
            settings->paths.directory_assets, "ozone/regular.ttf",
            strm->font_size, is_threaded, fontpath, sizeof(fontpath));

   /* Load additional font sizes using the same font file that worked */
   if (strm->font.font && fontpath[0] != '\0')
   {
      strm->font_small.font = gfx_display_font_file(p_disp, fontpath, strm->font_size_small, is_threaded);
      strm->font_title.font = gfx_display_font_file(p_disp, fontpath, strm->font_size_title, is_threaded);
      strm->font_tiny.font = gfx_display_font_file(p_disp, fontpath, strm->font_size_tiny, is_threaded);
   }

   strm->font.line_height = (int)(strm->font_size * STREAMLINED_LINE_HEIGHT);
   strm->font.glyph_width = (int)(strm->font_size * STREAMLINED_GLYPH_WIDTH_RATIO);
   strm->font_small.line_height = (int)(strm->font_size_small * STREAMLINED_LINE_HEIGHT);
   strm->font_small.glyph_width = (int)(strm->font_size_small * STREAMLINED_GLYPH_WIDTH_RATIO);
   strm->font_title.line_height = (int)(strm->font_size_title * STREAMLINED_LINE_HEIGHT);
   strm->font_title.glyph_width = (int)(strm->font_size_title * STREAMLINED_GLYPH_WIDTH_RATIO);

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

   /* Initialize auto savestate cache */
   strm->auto_save_cache.has_auto_save = false;
   strm->auto_save_cache.selection = (size_t)-1;

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
      streamlined_free_fonts(strm);

      /* Clean up save slot thumbnail */
      gfx_thumbnail_reset(&strm->savestate_thumbnail);

   }

   gfx_display_deinit_white_texture();
}

/*
 * Execute the deferred action after the interstitial has rendered one frame.
 * Called from streamlined_render on the frame after the interstitial was drawn.
 */
static void streamlined_execute_deferred_action(streamlined_t *strm)
{
   streamlined_interstitial_t action = strm->interstitial;

   strm->interstitial           = STREAMLINED_INTERSTITIAL_NONE;
   strm->interstitial_triggered = false;

   switch (action)
   {
      case STREAMLINED_INTERSTITIAL_LOADING:
      {
         struct menu_state *menu_st = menu_state_get_ptr();
         streamlined_launch_content(strm, menu_st,
               strm->loading_core_path,
               strm->loading_content_path,
               strm->loading_is_resume);
         strm->loading_core_path[0]    = '\0';
         strm->loading_content_path[0] = '\0';
         break;
      }

      case STREAMLINED_INTERSTITIAL_EXITING:
      {
         settings_t *settings = config_get_ptr();
         if (streamlined_is_launched_from_cli())
         {
            /* CLI: quit app (auto_save on skips UNLOAD_CORE save,
             * auto_save off uses CMD_EVENT_QUIT directly) */
            if (settings->bools.savestate_auto_save)
               command_event(CMD_EVENT_CLOSE_CONTENT, NULL);
            else
               command_event(CMD_EVENT_QUIT, NULL);
         }
         else
         {
            strm->view_stack.top = -1;
            if (settings->bools.savestate_auto_save)
            {
               bool orig = settings->bools.savestate_auto_save;
               settings->bools.savestate_auto_save = false;
               command_event(CMD_EVENT_UNLOAD_CORE, NULL);
               settings->bools.savestate_auto_save = orig;
            }
            else
               command_event(CMD_EVENT_UNLOAD_CORE, NULL);
            menu_entries_flush_stack(
                  msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU), 0);
         }
         break;
      }

      case STREAMLINED_INTERSTITIAL_SAVING_AND_EXITING:
      {
         if (streamlined_is_launched_from_cli())
         {
            command_event(CMD_EVENT_QUIT, NULL);
         }
         else
         {
            strm->view_stack.top = -1;
            command_event(CMD_EVENT_UNLOAD_CORE, NULL);
            menu_entries_flush_stack(
                  msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU), 0);
         }
         break;
      }

      default:
         break;
   }
}

static void streamlined_render(void *data, unsigned width, unsigned height, bool is_idle)
{
   streamlined_t *strm = (streamlined_t*)data;

   if (!strm)
      return;

   /* Execute deferred action on the frame after interstitial was drawn */
   if (strm->interstitial_triggered)
   {
      streamlined_execute_deferred_action(strm);
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

   /* Interstitial screen: black background + centered text */
   if (strm->interstitial != STREAMLINED_INTERSTITIAL_NONE)
   {
      const char *text;
      font_data_t *ifont;
      float isize;

      switch (strm->interstitial)
      {
         case STREAMLINED_INTERSTITIAL_LOADING:
            text = "Loading";
            break;
         case STREAMLINED_INTERSTITIAL_EXITING:
            text = "Exiting";
            break;
         case STREAMLINED_INTERSTITIAL_SAVING_AND_EXITING:
            text = "Saving and Exiting";
            break;
         default:
            text = "";
            break;
      }

      gfx_display_draw_quad(p_disp, userdata,
            video_width, video_height,
            0, 0, video_width, video_height,
            video_width, video_height,
            streamlined_color_black, NULL);

      ifont = strm->font_title.font ? strm->font_title.font : strm->font.font;
      isize = strm->font_title.font ? strm->font_size_title : strm->font_size;
      gfx_display_draw_text(ifont, text,
            (int)(video_width / 2),
            (int)(video_height / 2 + isize * STREAMLINED_TEXT_VCENTER),
            video_width, video_height,
            streamlined_color_text,
            TEXT_ALIGN_CENTER, 1.0f, false, 0, false);

      if (strm->font.font)
         font_flush(video_width, video_height, &strm->font);
      if (strm->font_title.font)
         font_flush(video_width, video_height, &strm->font_title);

      strm->interstitial_triggered = true;
      return;
   }

   streamlined_draw_bg(strm, p_disp, userdata, video_width, video_height);
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
   streamlined_view_t *view;

   if (!strm)
      return;

   /* Check what menu we're in */
   if (!label)
      return;

   if (content_settings_label && string_is_equal(label, content_settings_label))
      is_content_settings = true;
   else if (main_menu_label && string_is_equal(label, main_menu_label))
      is_main_menu = true;

   /* Also check enum_idx from the menu stack */
   if (!is_content_settings && !is_main_menu)
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
         view = streamlined_view_current(&strm->view_stack);

         /* Pop RA_SETTINGS if returning from an RA settings screen */
         if (view && view->type == STREAMLINED_VIEW_RA_SETTINGS)
         {
            streamlined_view_pop(&strm->view_stack);
            view = streamlined_view_current(&strm->view_stack);
         }

         if (strm->resume.active)
         {
            /* Returning from game - rebuild stack with folder state */
            streamlined_view_t *v;
            strm->view_stack.top = -1;
            v = streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_MAIN_MENU);
            if (v)
               strlcpy(v->data.main_menu.folder_path, start_dir,
                     sizeof(v->data.main_menu.folder_path));
            v = streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_FOLDER);
            if (v)
            {
               strlcpy(v->data.folder.folder_path, strm->resume.folder_path,
                     sizeof(v->data.folder.folder_path));
               if (!streamlined_read_folder_core(strm->resume.folder_path,
                     v->data.folder.core_path, sizeof(v->data.folder.core_path)))
                  v->data.folder.core_path[0] = '\0';
            }
            streamlined_populate_folder_menu(strm, strm->resume.folder_path, true);
            if (menu_st_local)
               menu_st_local->selection_ptr = strm->resume.selection;
            strm->resume.active = false;
         }
         else if (view && view->type == STREAMLINED_VIEW_MAIN_SETTINGS)
         {
            /* Returning to main settings submenu - re-push nav marker */
            streamlined_push_nav_marker();
            streamlined_populate_main_settings_submenu();
            if (menu_st_local)
               menu_st_local->selection_ptr = view->saved_selection;
         }
         else if (view && view->type == STREAMLINED_VIEW_MAIN_MENU)
         {
            /* Already at main menu - just ensure list is populated */
            streamlined_populate_folder_menu(strm, view->data.main_menu.folder_path, false);
         }
         else if (view && view->type == STREAMLINED_VIEW_FOLDER)
         {
            /* Already in a folder - just ensure list is populated */
            streamlined_populate_folder_menu(strm, view->data.folder.folder_path, true);
         }
         else
         {
            /* Fresh main menu - reset stack */
            streamlined_view_t *v;
            strm->view_stack.top = -1;
            v = streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_MAIN_MENU);
            if (v)
               strlcpy(v->data.main_menu.folder_path, start_dir,
                     sizeof(v->data.main_menu.folder_path));
            streamlined_populate_folder_menu(strm, start_dir, false);
         }
      }
      return;
   }

   if (is_content_settings)
   {
      struct menu_state *menu_st_local = menu_state_get_ptr();
      view = streamlined_view_current(&strm->view_stack);

      /* Pop RA_SETTINGS if returning from an RA settings screen */
      if (view && view->type == STREAMLINED_VIEW_RA_SETTINGS)
      {
         streamlined_view_pop(&strm->view_stack);
         view = streamlined_view_current(&strm->view_stack);
      }

      if (!view || (view->type != STREAMLINED_VIEW_QUICK_MENU
                  && view->type != STREAMLINED_VIEW_ADVANCED))
      {
         /* Fresh entry into quick menu - reset stack */
         strm->view_stack.top = -1;
         streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_QUICK_MENU);
         view = streamlined_view_current(&strm->view_stack);
      }

      if (view && view->type == STREAMLINED_VIEW_ADVANCED)
      {
         streamlined_populate_settings_submenu();
         if (menu_st_local)
            menu_st_local->selection_ptr = view->saved_selection;
      }
      else
      {
         streamlined_populate_quick_menu();
      }

      /*
       * Reset thumbnail state when entering quick menu so it reloads.
       * This ensures the thumbnail is refreshed (e.g., if a new screenshot
       * was taken since last viewing).
       */
      gfx_thumbnail_reset(&strm->savestate_thumbnail);
      strm->savestate_thumbnail_path[0] = '\0';
      strm->last_selection = (size_t)-1;  /* Force reload on next render */
   }
   /* else: in an RA settings screen - don't modify the stack */
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
 * Request a deferred content load with an interstitial screen.
 * Saves the parameters and sets interstitial to LOADING so that
 * streamlined_frame draws the "Loading" screen for one frame,
 * then streamlined_render executes the actual load on the next frame.
 */
static void streamlined_request_loading(
      streamlined_t *strm,
      const char *core_path, const char *content_path,
      bool is_resume)
{
   strlcpy(strm->loading_core_path, core_path,
         sizeof(strm->loading_core_path));
   strlcpy(strm->loading_content_path, content_path,
         sizeof(strm->loading_content_path));
   strm->loading_is_resume      = is_resume;
   strm->interstitial           = STREAMLINED_INTERSTITIAL_LOADING;
   strm->interstitial_triggered = false;
}

/*
 * Save folder state and launch content with the given core.
 * Common code for both direct launch (from .core.txt) and core selection.
 */
static void streamlined_launch_content(streamlined_t *strm,
      struct menu_state *menu_st,
      const char *core_path, const char *content_path,
      bool load_auto_savestate)
{
   content_ctx_info_t content_info;
   streamlined_view_t *folder_view = NULL;
   int idx;

   content_info.argc        = 0;
   content_info.argv        = NULL;
   content_info.args        = NULL;
   content_info.environ_get = NULL;

   /* Find the nearest FOLDER view on the stack to save resume state */
   for (idx = strm->view_stack.top; idx >= 0; idx--)
   {
      if (strm->view_stack.entries[idx].type == STREAMLINED_VIEW_FOLDER)
      {
         folder_view = &strm->view_stack.entries[idx];
         break;
      }
   }

   if (folder_view)
   {
      strm->resume.active = true;
      strlcpy(strm->resume.folder_path, folder_view->data.folder.folder_path,
            sizeof(strm->resume.folder_path));
      /* If launching from folder directly, use current selection.
       * If launching via core selection, use the folder's saved selection. */
      if (streamlined_view_current(&strm->view_stack) == folder_view)
         strm->resume.selection = menu_st->selection_ptr;
      else
         strm->resume.selection = folder_view->saved_selection;
   }

   /* Clear the view stack */
   strm->view_stack.top = -1;

   /* Close menu before loading content */
   command_event(CMD_EVENT_MENU_TOGGLE, NULL);

   task_push_load_content_with_new_core_from_menu(
         core_path, content_path,
         &content_info,
         CORE_TYPE_PLAIN, NULL, NULL);

   /* If requested and savestate_auto_load is off, explicitly load the auto
    * savestate. When savestate_auto_load is on, the runloop already handles
    * this via command_event_load_auto_state(). */
   if (load_auto_savestate)
   {
      settings_t *settings = config_get_ptr();
      if (!settings->bools.savestate_auto_load)
      {
         char auto_path[PATH_MAX_LENGTH];
         if (streamlined_get_auto_savestate_path(content_path, core_path,
               auto_path, sizeof(auto_path)))
            content_load_state(auto_path, false, true);
      }
   }
}

/*
 * Custom input handler for streamlined menu navigation.
 * Uses the view stack to determine behavior based on the current view type.
 */
static int streamlined_entry_action(void *userdata, menu_entry_t *entry,
      size_t i, enum menu_action action)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   streamlined_t *strm = NULL;
   streamlined_view_t *view;

   if (menu_st)
      strm = (streamlined_t*)menu_st->userdata;

   if (!strm)
      return generic_menu_entry_action(userdata, entry, i, action);

   /* Block all input while interstitial screen is showing */
   if (strm->interstitial != STREAMLINED_INTERSTITIAL_NONE)
      return 0;

   view = streamlined_view_current(&strm->view_stack);
   if (!view)
      return generic_menu_entry_action(userdata, entry, i, action);

   switch (view->type)
   {
      case STREAMLINED_VIEW_QUICK_MENU:
      {
         /* Handle input for save slot selection when on Save/Load entry */
         if (strm->show_slot_selector)
         {
            settings_t *settings = config_get_ptr();
            size_t selection = menu_st->selection_ptr;

            if (action == MENU_ACTION_LEFT)
            {
               strm->preview_slot--;
               if (strm->preview_slot < 0)
                  strm->preview_slot = STREAMLINED_NUM_SLOTS - 1;
               settings->ints.state_slot = strm->preview_slot - 1;
               streamlined_load_slot_thumbnail(strm, strm->preview_slot);
               return 0;
            }

            if (action == MENU_ACTION_RIGHT)
            {
               strm->preview_slot++;
               if (strm->preview_slot >= STREAMLINED_NUM_SLOTS)
                  strm->preview_slot = 0;
               settings->ints.state_slot = strm->preview_slot - 1;
               streamlined_load_slot_thumbnail(strm, strm->preview_slot);
               return 0;
            }

            if (action == MENU_ACTION_OK || action == MENU_ACTION_START)
            {
               if (selection == 1)
                  command_event(CMD_EVENT_SAVE_STATE, NULL);
               else if (selection == 2)
                  command_event(CMD_EVENT_LOAD_STATE, NULL);
               command_event(CMD_EVENT_MENU_TOGGLE, NULL);
               return 0;
            }
         }

         /* Back button: close menu and resume game */
         if (action == MENU_ACTION_CANCEL)
         {
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            return 0;
         }

         /* OK: check for special entries */
         if (action == MENU_ACTION_OK && entry)
         {
            const char *entry_label = NULL;

            if (!string_is_empty(entry->rich_label))
               entry_label = entry->rich_label;
            else if (!string_is_empty(entry->path))
               entry_label = entry->path;

            /* Select "Advanced": push ADVANCED view */
            if (entry_label && string_is_equal(entry_label, "Advanced"))
            {
               view->saved_selection = menu_st->selection_ptr;
               streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_ADVANCED);
               streamlined_populate_settings_submenu();
               menu_st->selection_ptr = 0;
               return 0;
            }

            /* Handle "Quit" - show "Exiting" interstitial, defer action */
            if (entry_label && string_is_equal(entry_label, "Quit"))
            {
               strm->interstitial           = STREAMLINED_INTERSTITIAL_EXITING;
               strm->interstitial_triggered = false;
               return 0;
            }

            /* Handle "Save and Quit" - show "Saving and Exiting" interstitial */
            if (entry_label && string_is_equal(entry_label, "Save and Quit"))
            {
               strm->interstitial           = STREAMLINED_INTERSTITIAL_SAVING_AND_EXITING;
               strm->interstitial_triggered = false;
               return 0;
            }
         }
         break;
      }

      case STREAMLINED_VIEW_ADVANCED:
      {
         /* Back: return to quick menu */
         if (action == MENU_ACTION_CANCEL)
         {
            streamlined_view_pop(&strm->view_stack);
            view = streamlined_view_current(&strm->view_stack);
            streamlined_populate_quick_menu();
            if (view)
               menu_st->selection_ptr = view->saved_selection;
            return 0;
         }

         /* OK: entering an RA settings screen */
         if (action == MENU_ACTION_OK)
         {
            view->saved_selection = menu_st->selection_ptr;
            streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_RA_SETTINGS);
         }
         break;
      }

      case STREAMLINED_VIEW_CORE_SELECT:
      {
         /* Cancel: go back to parent view */
         if (action == MENU_ACTION_CANCEL)
         {
            streamlined_view_pop(&strm->view_stack);
            view = streamlined_view_current(&strm->view_stack);
            if (view && view->type == STREAMLINED_VIEW_FOLDER)
               streamlined_populate_folder_menu(strm, view->data.folder.folder_path, true);
            else if (view && view->type == STREAMLINED_VIEW_MAIN_MENU)
               streamlined_populate_folder_menu(strm, view->data.main_menu.folder_path, false);
            if (view)
               menu_st->selection_ptr = view->saved_selection;
            return 0;
         }

         /* Core selected: save and launch */
         if (action == MENU_ACTION_OK && entry)
         {
            const char *selected_core = entry->label;
            if (!string_is_empty(selected_core) && path_is_valid(selected_core))
            {
               /* Find the parent FOLDER view to save core */
               streamlined_view_t *parent = (strm->view_stack.top > 0)
                  ? &strm->view_stack.entries[strm->view_stack.top - 1] : NULL;

               if (parent && parent->type == STREAMLINED_VIEW_FOLDER)
               {
                  streamlined_save_folder_core(parent->data.folder.folder_path, selected_core);
                  strlcpy(parent->data.folder.core_path, selected_core,
                        sizeof(parent->data.folder.core_path));
               }

               streamlined_request_loading(strm,
                     selected_core, view->data.core_select.content_path,
                     false);
               return 0;
            }
         }

         /* Allow navigation (up/down) */
         if (action == MENU_ACTION_UP || action == MENU_ACTION_DOWN ||
             action == MENU_ACTION_SCROLL_UP || action == MENU_ACTION_SCROLL_DOWN)
         {
            return generic_menu_entry_action(userdata, entry, i, action);
         }

         return 0;  /* Block other actions */
      }

      case STREAMLINED_VIEW_MAIN_MENU:
      {
         /* Cancel at top level: reset selection so next Menu press can background */
         if (action == MENU_ACTION_CANCEL)
         {
            menu_st->selection_ptr = 0;
            return 0;
         }

         if (action == MENU_ACTION_OK && entry)
         {
            /* Settings entry */
            if (entry->enum_idx == MENU_ENUM_LABEL_SETTINGS)
            {
               view->saved_selection = menu_st->selection_ptr;
               streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_MAIN_SETTINGS);
               streamlined_push_nav_marker();
               streamlined_populate_main_settings_submenu();
               menu_st->selection_ptr = 0;
               return 0;
            }

            /* Folder/file selection */
            {
               const char *item_path = entry->label;
               if (!string_is_empty(item_path))
               {
                  if (path_is_directory(item_path))
                  {
                     streamlined_view_t *v;
                     view->saved_selection = menu_st->selection_ptr;
                     v = streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_FOLDER);
                     if (v)
                     {
                        strlcpy(v->data.folder.folder_path, item_path,
                              sizeof(v->data.folder.folder_path));
                        if (!streamlined_read_folder_core(item_path, v->data.folder.core_path,
                              sizeof(v->data.folder.core_path)))
                           v->data.folder.core_path[0] = '\0';
                     }
                     streamlined_push_nav_marker();
                     streamlined_populate_folder_menu(strm, item_path, true);
                     menu_st->selection_ptr = 0;
                     return 0;
                  }
                  else if (path_is_valid(item_path))
                  {
                     /* No core at top level - show core selection */
                     streamlined_view_t *v;
                     view->saved_selection = menu_st->selection_ptr;
                     v = streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_CORE_SELECT);
                     if (v)
                        strlcpy(v->data.core_select.content_path, item_path,
                              sizeof(v->data.core_select.content_path));
                     streamlined_push_nav_marker();
                     streamlined_populate_core_selection(strm, item_path);
                     menu_st->selection_ptr = 0;
                     return 0;
                  }
               }
            }
         }

         /* Block non-navigation actions */
         if (action == MENU_ACTION_SCAN
               || action == MENU_ACTION_SEARCH
               || action == MENU_ACTION_INFO)
            return 0;

         break;
      }

      case STREAMLINED_VIEW_FOLDER:
      {
         /* Cancel: pop to parent view */
         if (action == MENU_ACTION_CANCEL)
         {
            streamlined_view_pop(&strm->view_stack);
            view = streamlined_view_current(&strm->view_stack);
            if (view)
            {
               if (view->type == STREAMLINED_VIEW_FOLDER)
                  streamlined_populate_folder_menu(strm, view->data.folder.folder_path, true);
               else if (view->type == STREAMLINED_VIEW_MAIN_MENU)
               {
                  streamlined_pop_nav_marker();
                  streamlined_populate_folder_menu(strm, view->data.main_menu.folder_path, false);
               }
               menu_st->selection_ptr = view->saved_selection;
            }
            return 0;
         }

         if (action == MENU_ACTION_OK && entry)
         {
            const char *item_path = entry->label;
            if (!string_is_empty(item_path))
            {
               if (path_is_directory(item_path))
               {
                  /* Enter subfolder - push new FOLDER view */
                  streamlined_view_t *v;
                  view->saved_selection = menu_st->selection_ptr;
                  v = streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_FOLDER);
                  if (v)
                  {
                     strlcpy(v->data.folder.folder_path, item_path,
                           sizeof(v->data.folder.folder_path));
                     if (!streamlined_read_folder_core(item_path, v->data.folder.core_path,
                           sizeof(v->data.folder.core_path)))
                        v->data.folder.core_path[0] = '\0';
                  }
                  streamlined_populate_folder_menu(strm, item_path, true);
                  menu_st->selection_ptr = 0;
                  return 0;
               }
               else if (path_is_valid(item_path))
               {
                  /* Launch file - resolve game-specific or folder core */
                  {
                     char resolved_core[PATH_MAX_LENGTH];
                     if (streamlined_resolve_core_for_content(
                              item_path, view->data.folder.core_path,
                              resolved_core, sizeof(resolved_core))
                           && path_is_valid(resolved_core))
                     {
                        settings_t *settings = config_get_ptr();
                        streamlined_request_loading(strm,
                              resolved_core, item_path,
                              strm->auto_save_cache.has_auto_save
                                    && settings->bools.savestate_auto_load);
                        return 0;
                     }
                     else
                     {
                        /* No core resolved - show core selection */
                        streamlined_view_t *v;
                        view->saved_selection = menu_st->selection_ptr;
                        v = streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_CORE_SELECT);
                        if (v)
                           strlcpy(v->data.core_select.content_path, item_path,
                                 sizeof(v->data.core_select.content_path));
                        streamlined_populate_core_selection(strm, item_path);
                        menu_st->selection_ptr = 0;
                        return 0;
                     }
                  }
               }
            }
         }

         /* X button (MENU_ACTION_SCAN): launch with auto savestate (Resume) */
         if (action == MENU_ACTION_SCAN && entry)
         {
            if (strm->auto_save_cache.has_auto_save)
            {
               const char *item_path = entry->label;
               if (!string_is_empty(item_path) && path_is_valid(item_path)
                     && !path_is_directory(item_path))
               {
                  char resolved_core[PATH_MAX_LENGTH];
                  if (streamlined_resolve_core_for_content(
                           item_path, view->data.folder.core_path,
                           resolved_core, sizeof(resolved_core))
                        && path_is_valid(resolved_core))
                  {
                     streamlined_request_loading(strm,
                           resolved_core, item_path, true);
                     return 0;
                  }
               }
            }
            return 0;
         }

         /* Block non-navigation actions */
         if (action == MENU_ACTION_SEARCH
               || action == MENU_ACTION_INFO)
            return 0;

         break;
      }

      case STREAMLINED_VIEW_MAIN_SETTINGS:
      {
         /* Back: return to main menu */
         if (action == MENU_ACTION_CANCEL)
         {
            streamlined_view_pop(&strm->view_stack);
            view = streamlined_view_current(&strm->view_stack);
            if (view && view->type == STREAMLINED_VIEW_MAIN_MENU)
            {
               streamlined_pop_nav_marker();
               streamlined_populate_folder_menu(strm, view->data.main_menu.folder_path, false);
            }
            if (view)
               menu_st->selection_ptr = view->saved_selection;
            return 0;
         }

         /* OK: entering RA settings */
         if (action == MENU_ACTION_OK)
         {
            view->saved_selection = menu_st->selection_ptr;
            streamlined_view_push(&strm->view_stack, STREAMLINED_VIEW_RA_SETTINGS);
            streamlined_pop_nav_marker();  /* Remove marker before RA pushes its entries */
            return generic_menu_entry_action(userdata, entry, i, action);
         }
         break;
      }

      case STREAMLINED_VIEW_RA_SETTINGS:
         /* All input handled by generic handler */
         return generic_menu_entry_action(userdata, entry, i, action);

      default:
         break;
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
