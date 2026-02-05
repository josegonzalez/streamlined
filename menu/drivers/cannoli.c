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
 * Cannoli Menu Driver
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
#include <file/file_path.h>
#include <lists/dir_list.h>
#include <streams/file_stream.h>

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
#define CANNOLI_SOLID_COLOR(r, g, b, a) \
   { r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a }

static float cannoli_color_bg[16]        = CANNOLI_SOLID_COLOR(0.0f, 0.0f, 0.0f, 0.85f);
static float cannoli_color_selection[16] = CANNOLI_SOLID_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
static float cannoli_color_accent[16]    = CANNOLI_SOLID_COLOR(0.18f, 0.55f, 0.53f, 1.0f);
static float cannoli_color_black[16]     = CANNOLI_SOLID_COLOR(0.0f, 0.0f, 0.0f, 1.0f);
static uint32_t cannoli_color_text        = 0xFFFFFFFF;  /* White (RGBA packed) */
static uint32_t cannoli_color_text_dark   = 0x000000FF;  /* Black (RGBA packed) */
static uint32_t cannoli_color_text_accent = 0x2E8C87FF;  /* Teal (RGBA packed) */

/* Layout constants - base sizes at 1.0x scale factor */
#define CANNOLI_BASE_FONT_SIZE     32    /* Base font size in pixels */
#define CANNOLI_MARGIN_RATIO       0.03f /* Screen edge margin as ratio of dimension */
#define CANNOLI_LINE_HEIGHT        1.8f  /* Line height multiplier for menu items */
#define CANNOLI_PILL_PADDING_RATIO 0.375f /* Horizontal padding as ratio of font size */
#define CANNOLI_BUTTON_CIRCLE_SIZE 32    /* Size of button circles in legend */
#define CANNOLI_MIN_FONT_SIZE      12    /* Minimum font size to ensure readability */

/* ======================================================================
 * CUSTOM QUICK MENU - Modify this to change quick menu items
 * ====================================================================== */

typedef struct
{
   const char *label;
   enum msg_hash_enums action;
} cannoli_quick_item_t;

/*
 * Sentinel value used to identify the "Advanced" menu entry.
 * When a menu item has this value as its action, selecting it opens
 * the custom settings submenu instead of triggering a RetroArch action.
 * The value 0xCAFE is arbitrary, chosen to not conflict with any
 * MENU_ENUM_LABEL_* values.
 */
#define CANNOLI_SETTINGS_SUBMENU_MARKER 0xCAFE

/* Marker for conditional exit entry - shows "Exit" or "Quit" based on CLI launch */
#define CANNOLI_EXIT_MARKER 0xCAFF

/* Main custom quick menu
 * NOTE: Exit/Quit handled dynamically - see cannoli_populate_quick_menu() */
static const cannoli_quick_item_t cannoli_quick_menu_items[] = {
   { "Resume",        MENU_ENUM_LABEL_RESUME_CONTENT },
   { "Save",          MENU_ENUM_LABEL_SAVE_STATE },
   { "Load",          MENU_ENUM_LABEL_LOAD_STATE },
   { "Advanced",      CANNOLI_SETTINGS_SUBMENU_MARKER },  /* Opens combined settings submenu */
   { "Reset",         MENU_ENUM_LABEL_RESTART_CONTENT },
   { NULL,            CANNOLI_EXIT_MARKER },  /* Dynamic: "Exit" or "Quit" based on CLI */
   { NULL, 0 }
};

/* Combined settings submenu items for quick menu (alphabetized)
 * NOTE: Disc Control is handled dynamically - see cannoli_populate_settings_submenu() */
static const cannoli_quick_item_t cannoli_settings_menu_items[] = {
   { "Achievements",     MENU_ENUM_LABEL_RETRO_ACHIEVEMENTS_SETTINGS },
   { "Audio",            MENU_ENUM_LABEL_AUDIO_SETTINGS },
   { "Cheats",           MENU_ENUM_LABEL_CORE_CHEAT_OPTIONS },
   { "Controls",         MENU_ENUM_LABEL_CORE_INPUT_REMAPPING_OPTIONS },
   { "Core Options",     MENU_ENUM_LABEL_CORE_OPTIONS },
   { "Disc Control",     MENU_ENUM_LABEL_DISK_OPTIONS },  /* Conditionally shown */
   { "Input",            MENU_ENUM_LABEL_INPUT_SETTINGS },
   { "Overrides",        MENU_ENUM_LABEL_QUICK_MENU_OVERRIDE_OPTIONS },
   { "Saving",           MENU_ENUM_LABEL_SAVING_SETTINGS },
   { "Screenshot",       MENU_ENUM_LABEL_TAKE_SCREENSHOT },
   { "Shaders",          MENU_ENUM_LABEL_SHADER_OPTIONS },
   { "Video",            MENU_ENUM_LABEL_VIDEO_SETTINGS },
   { NULL, 0 }
};

/* Main menu settings submenu items - all alphabetized */
static const cannoli_quick_item_t cannoli_main_settings_items[] = {
   { "Accessibility",    MENU_ENUM_LABEL_ACCESSIBILITY_SETTINGS },
   { "Achievements",     MENU_ENUM_LABEL_RETRO_ACHIEVEMENTS_SETTINGS },
   { "Audio",            MENU_ENUM_LABEL_AUDIO_SETTINGS },
   { "Configuration",    MENU_ENUM_LABEL_CONFIGURATION_SETTINGS },
   { "Core",             MENU_ENUM_LABEL_CORE_SETTINGS },
   { "Directory",        MENU_ENUM_LABEL_DIRECTORY_SETTINGS },
   { "Drivers",          MENU_ENUM_LABEL_DRIVER_SETTINGS },
   { "Frame Throttle",   MENU_ENUM_LABEL_FRAME_THROTTLE_SETTINGS },
   { "Help",             MENU_ENUM_LABEL_HELP_LIST },
   { "Information",      MENU_ENUM_LABEL_INFORMATION_LIST },
   { "Input",            MENU_ENUM_LABEL_INPUT_SETTINGS },
   { "Latency",          MENU_ENUM_LABEL_LATENCY_SETTINGS },
   { "Logging",          MENU_ENUM_LABEL_LOGGING_SETTINGS },
   { "Network",          MENU_ENUM_LABEL_NETWORK_SETTINGS },
   { "Online Updater",   MENU_ENUM_LABEL_ONLINE_UPDATER },
   { "Playlists",        MENU_ENUM_LABEL_PLAYLIST_SETTINGS },
   { "Power Management", MENU_ENUM_LABEL_POWER_MANAGEMENT_SETTINGS },
   { "Recording",        MENU_ENUM_LABEL_RECORDING_SETTINGS },
   { "Saving",           MENU_ENUM_LABEL_SAVING_SETTINGS },
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
   int button_size;
   float scale_factor;

   /* State */
   bool is_quick_menu;
   bool in_settings_submenu;
   bool return_to_settings_submenu;  /* Track if we should return to Advanced submenu */
   size_t saved_quick_menu_selection; /* Remember position in main quick menu */

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

   /* Core selection mode */
   bool selecting_core;           /* True when showing core selection list */
   char pending_content_path[PATH_MAX_LENGTH]; /* Content path waiting for core selection */

   /* Main menu settings submenu */
   bool in_main_settings_submenu; /* True when in Settings submenu from main menu */
   bool return_to_main_settings_submenu; /* Flag to return to settings submenu after backing out */
   size_t saved_main_menu_selection; /* Remember position in main menu when entering settings */
   size_t saved_settings_selection; /* Remember position in settings submenu */
} cannoli_t;

/* Number of save slots to display (Auto + slots 0-7) */
#define CANNOLI_NUM_SLOTS 9
#define CANNOLI_AUTO_SLOT_INDEX 0  /* First dot is the auto slot (state_slot -1) */

/* ======================================================================
 * DRAWING FUNCTIONS
 * ====================================================================== */

/* Forward declarations */
static const char *cannoli_strip_sort_prefix(const char *name);

static void cannoli_draw_text(cannoli_t *cannoli,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color, bool small_font);
static int cannoli_get_text_width(cannoli_t *cannoli, const char *text, bool small_font);

static void cannoli_draw_bg(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   if (!p_disp || video_width == 0 || video_height == 0)
      return;

   gfx_display_draw_quad(p_disp, userdata,
         video_width, video_height,
         0, 0, video_width, video_height,
         video_width, video_height,
         cannoli_color_bg, NULL);
}

static void cannoli_draw_filled_circle(cannoli_t *cannoli,
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
static void cannoli_draw_rounded_pill(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      int x, int y, int width, int height,
      unsigned video_width, unsigned video_height,
      float *color)
{
   int radius = height / 2;
   int rect_x = x + radius;
   int rect_width = width - height;

   /* Left semicircle */
   cannoli_draw_filled_circle(cannoli, p_disp, userdata,
         x + radius, y + radius, radius,
         video_width, video_height, color);

   /* Right semicircle */
   cannoli_draw_filled_circle(cannoli, p_disp, userdata,
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

static void cannoli_draw_button_legend(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      int x, int y, const char *button, const char *label,
      unsigned video_width, unsigned video_height)
{
   int circle_size = cannoli->button_size;
   int circle_radius = circle_size / 2;
   int label_width = cannoli_get_text_width(cannoli, label, true);
   int pill_padding = (int)(6 * cannoli->scale_factor);
   int inner_padding = (int)(6 * cannoli->scale_factor);
   /* Layout: [padding][circle][inner_padding][label][padding + small extra] */
   int pill_width = pill_padding * 2 + circle_size + inner_padding + label_width
         + (int)(2 * cannoli->scale_factor);
   int pill_height = circle_size + pill_padding * 2;
   int pill_y = y - pill_padding;
   int text_baseline = pill_y + pill_height / 2 + (int)(cannoli->font_size_small * 0.20f);

   /* Draw rounded teal pill background */
   cannoli_draw_rounded_pill(cannoli, p_disp, userdata,
         x, pill_y, pill_width, pill_height,
         video_width, video_height, cannoli_color_accent);

   /* Draw white circle inside pill */
   cannoli_draw_filled_circle(cannoli, p_disp, userdata,
         x + pill_padding + circle_radius, pill_y + pill_height / 2, circle_radius,
         video_width, video_height, cannoli_color_selection);

   /* Draw button letter centered in white circle */
   {
      int letter_width = cannoli_get_text_width(cannoli, button, true);
      int letter_x = x + pill_padding + circle_radius - letter_width / 2;
      cannoli_draw_text(cannoli, p_disp, video_width, video_height,
            letter_x, text_baseline, button, cannoli_color_text_dark, true);
   }

   /* Draw label text to the right of circle */
   {
      int text_x = x + pill_padding + circle_size + inner_padding;
      cannoli_draw_text(cannoli, p_disp, video_width, video_height,
            text_x, text_baseline, label, cannoli_color_text_dark, true);
   }
}

static void cannoli_draw_text(cannoli_t *cannoli,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color, bool small_font)
{
   font_data_t *font = small_font ? cannoli->font_small.font : cannoli->font.font;
   if (font && text)
   {
      gfx_display_draw_text(font, text, x, y,
            video_width, video_height, color,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
   }
}

static void cannoli_draw_title(cannoli_t *cannoli,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color)
{
   font_data_t *font = cannoli->font_title.font ? cannoli->font_title.font : cannoli->font.font;
   if (font && text)
   {
      gfx_display_draw_text(font, text, x, y,
            video_width, video_height, color,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
   }
}

static int cannoli_get_text_width(cannoli_t *cannoli, const char *text, bool small_font)
{
   font_data_t *font = small_font ? cannoli->font_small.font : cannoli->font.font;
   if (font && text)
      return font_driver_get_message_width(font, text, strlen(text), 1.0f);
   return 0;
}

static void cannoli_draw_text_tiny(cannoli_t *cannoli,
      gfx_display_t *p_disp,
      unsigned video_width, unsigned video_height,
      int x, int y,
      const char *text, uint32_t color)
{
   font_data_t *font = cannoli->font_tiny.font ? cannoli->font_tiny.font : cannoli->font_small.font;
   if (font && text)
   {
      gfx_display_draw_text(font, text, x, y,
            video_width, video_height, color,
            TEXT_ALIGN_LEFT, 1.0f, false, 0, false);
   }
}

static int cannoli_get_text_width_tiny(cannoli_t *cannoli, const char *text)
{
   font_data_t *font = cannoli->font_tiny.font ? cannoli->font_tiny.font : cannoli->font_small.font;
   if (font && text)
      return font_driver_get_message_width(font, text, strlen(text), 1.0f);
   return 0;
}

static int cannoli_get_title_width(cannoli_t *cannoli, const char *text)
{
   font_data_t *font = cannoli->font_title.font ? cannoli->font_title.font : cannoli->font.font;
   if (font && text)
      return font_driver_get_message_width(font, text, strlen(text), 1.0f);
   return 0;
}

/*
 * Check if entry value indicates a directory and add slash prefix.
 * Returns true if entry is a directory.
 */
static bool cannoli_process_entry_type(const char *value, char *label, size_t label_size)
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
static bool cannoli_should_hide_value(const char *value)
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
static void cannoli_truncate_text(cannoli_t *cannoli, const char *text,
      char *out, size_t out_size, int max_width, bool small_font)
{
   int text_width;
   size_t len;

   if (!text || !out || out_size == 0)
      return;

   strlcpy(out, text, out_size);
   text_width = cannoli_get_text_width(cannoli, out, small_font);

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
      text_width = cannoli_get_text_width(cannoli, out, small_font);
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
static void cannoli_load_slot_thumbnail(cannoli_t *cannoli, int preview_slot)
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
   if (   (cannoli->savestate_thumbnail.status == GFX_THUMBNAIL_STATUS_UNKNOWN)
       || !string_is_equal(state_path, cannoli->savestate_thumbnail_path))
   {
      strlcpy(cannoli->savestate_thumbnail_path, state_path,
            sizeof(cannoli->savestate_thumbnail_path));

      /* Free old texture before requesting new one */
      gfx_thumbnail_reset(&cannoli->savestate_thumbnail);

      /* Request new thumbnail - the thumbnail system handles missing files */
      gfx_thumbnail_request_file(state_path, &cannoli->savestate_thumbnail,
            settings->uints.gfx_thumbnail_upscale_threshold);

      /* Use core aspect ratio for proper rendering */
      cannoli->savestate_thumbnail.flags |= GFX_THUMB_FLAG_CORE_ASPECT;
   }

   cannoli->preview_slot = preview_slot;
}

/*
 * Draw the save slot selector UI: thumbnail preview with polaroid frame and dot indicators.
 * Positioned on the right side of the screen, vertically centered.
 */
static void cannoli_draw_slot_selector(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   int i;
   /* Larger thumbnail - 45% of screen height, maintain 4:3 aspect for frame */
   int thumb_max_height = (int)(video_height * 0.45f);
   int thumb_max_width  = (int)(thumb_max_height * 4.0f / 3.0f);

   /* Polaroid frame dimensions */
   int frame_border     = (int)(5 * cannoli->scale_factor);   /* Side/top border */
   int frame_bottom     = (int)(28 * cannoli->scale_factor);  /* Thicker bottom chin for dots */
   int frame_width      = thumb_max_width + frame_border * 2;
   int frame_height     = thumb_max_height + frame_border + frame_bottom;

   int frame_x, frame_y;
   int thumb_x, thumb_y;
   int dot_y, dot_spacing, dot_radius;
   int total_dots_width;
   int dots_start_x;

   /* Calculate dot dimensions first (needed for vertical centering) */
   dot_radius  = (int)(4 * cannoli->scale_factor);
   dot_spacing = (int)(16 * cannoli->scale_factor);

   /* Position frame on right side, vertically centered with dots below */
   frame_x = video_width - cannoli->margin_x - frame_width;
   frame_y = (video_height - frame_height - dot_radius * 2 - (int)(16 * cannoli->scale_factor)) / 2;

   /* Thumbnail position inside frame */
   thumb_x = frame_x + frame_border;
   thumb_y = frame_y + frame_border;

   /* Draw polaroid frame (white background) */
   gfx_display_draw_quad(p_disp, userdata, video_width, video_height,
         frame_x, frame_y, frame_width, frame_height,
         video_width, video_height, cannoli_color_selection, NULL);

   /* Draw thumbnail if available */
   if (cannoli->savestate_thumbnail.status == GFX_THUMBNAIL_STATUS_AVAILABLE)
   {
      float draw_width, draw_height;

      /* Calculate aspect-correct dimensions */
      gfx_thumbnail_get_draw_dimensions(
            &cannoli->savestate_thumbnail,
            thumb_max_width, thumb_max_height, 1.0f,
            &draw_width, &draw_height);

      /* Center within thumbnail area */
      {
         int offset_x = (thumb_max_width - (int)draw_width) / 2;
         int offset_y = (thumb_max_height - (int)draw_height) / 2;

         gfx_thumbnail_draw(userdata, video_width, video_height,
               &cannoli->savestate_thumbnail,
               (float)(thumb_x + offset_x), (float)(thumb_y + offset_y),
               (unsigned)draw_width, (unsigned)draw_height,
               GFX_THUMBNAIL_ALIGN_CENTRE, 1.0f, 1.0f, NULL);
      }
   }
   else if (cannoli->savestate_thumbnail.status == GFX_THUMBNAIL_STATUS_MISSING)
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
         int text_width = cannoli_get_text_width(cannoli, placeholder, false);
         int text_x = thumb_x + (thumb_max_width - text_width) / 2;
         /* Center vertically: account for font baseline by adding ~1/3 of font size */
         int text_y = thumb_y + thumb_max_height / 2 + (int)(cannoli->font_size * 0.35f);

         cannoli_draw_text(cannoli, p_disp, video_width, video_height,
               text_x, text_y,
               placeholder, cannoli_color_text_dark, false);
      }
   }

   /* Draw slot indicators in the polaroid chin: 'A' for auto, dots for 0-7 */
   total_dots_width = CANNOLI_NUM_SLOTS * (dot_radius * 2)
         + (CANNOLI_NUM_SLOTS - 1) * (dot_spacing - dot_radius * 2);
   dot_y = thumb_y + thumb_max_height + (frame_bottom - dot_radius * 2) / 2;
   dots_start_x = frame_x + (frame_width - total_dots_width) / 2;

   for (i = 0; i < CANNOLI_NUM_SLOTS; i++)
   {
      int dot_cx = dots_start_x + i * dot_spacing + dot_radius;
      bool is_selected = (i == cannoli->preview_slot);
      float *color = is_selected ? cannoli_color_accent : cannoli_color_black;
      int r = is_selected ? dot_radius : (int)(dot_radius * 0.6f);
      /* Adjust y position to keep dots vertically centered regardless of size */
      int cy = dot_y + dot_radius;

      if (i == 0)
      {
         /* Draw 'A' for Auto slot using tiny font, centered on dot line */
         int text_width = cannoli_get_text_width_tiny(cannoli, "A");
         int text_x = dot_cx - text_width / 2;
         /* Center 'A' vertically: baseline + 0.35*font_size ≈ visual center */
         int text_y = cy + (int)(cannoli->font_size_tiny * 0.35f);
         cannoli_draw_text_tiny(cannoli, p_disp, video_width, video_height,
               text_x, text_y, "A",
               is_selected ? cannoli_color_text_accent : cannoli_color_text_dark);
      }
      else
      {
         cannoli_draw_filled_circle(cannoli, p_disp, userdata,
               dot_cx, cy, r,
               video_width, video_height, color);
      }
   }
}

/* ======================================================================
 * MENU RENDERING
 * ====================================================================== */

static void cannoli_render_menu(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   size_t list_size, selection, i, start_idx, max_visible;
   int y, item_height, button_legend_y;
   char title_buf[256];

   if (!cannoli->font.font || !p_disp || !menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list || list->size == 0)
      return;

   list_size = list->size;
   selection = menu_st->selection_ptr;
   item_height = cannoli->font.line_height;
   if (item_height <= 0)
      item_height = 20;

   /*
    * Detect if on Save (index 1) or Load (index 2) in main quick menu.
    * Show the slot selector UI when these entries are selected.
    */
   {
      bool was_showing = cannoli->show_slot_selector;
      cannoli->show_slot_selector = false;

      if (cannoli->is_quick_menu && !cannoli->in_settings_submenu)
      {
         if (selection == 1 || selection == 2)
            cannoli->show_slot_selector = true;
      }

      /* When selection changes to save/load, load the current slot's thumbnail */
      if (cannoli->show_slot_selector && (!was_showing || selection != cannoli->last_selection))
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
         cannoli->preview_slot = state_slot + 1;
         cannoli_load_slot_thumbnail(cannoli, cannoli->preview_slot);
      }

      cannoli->last_selection = selection;
   }

   /* Calculate visible items: screen height minus title area and button legend area */
   {
      int title_area = cannoli->margin_y + (int)(cannoli->font_size_title * 1.4f);
      int bottom_area = cannoli->margin_y + cannoli->button_size + (int)(20 * cannoli->scale_factor);
      max_visible = (video_height - title_area - bottom_area) / item_height;
   }
   if (max_visible == 0)
      max_visible = 1;

   /* Get title - show game name for quick menu, "Settings" for submenu */
   title_buf[0] = '\0';
   if (cannoli->selecting_core)
   {
      strlcpy(title_buf, "Select Core", sizeof(title_buf));
   }
   else if (cannoli->in_settings_submenu)
   {
      strlcpy(title_buf, "Advanced", sizeof(title_buf));
   }
   else if (cannoli->in_main_settings_submenu)
   {
      strlcpy(title_buf, "Settings", sizeof(title_buf));
   }
   else if (cannoli->is_custom_main_menu && cannoli->in_folder)
   {
      /* Show folder/platform name as title */
      const char *folder_name = path_basename(cannoli->current_folder_path);
      if (!string_is_empty(folder_name))
      {
         /* Strip sort prefix (e.g., "1) Game Boy" -> "Game Boy") */
         const char *clean_name = cannoli_strip_sort_prefix(folder_name);
         strlcpy(title_buf, clean_name, sizeof(title_buf));
      }
   }
   else if (cannoli->is_quick_menu)
   {
      /* Custom quick menu - show game name */
      const char *content_path = path_get(RARCH_PATH_CONTENT);
      if (!string_is_empty(content_path))
      {
         const char *game_name = path_basename(content_path);
         if (!string_is_empty(game_name))
         {
            /* Copy and remove extension */
            char *ext;
            strlcpy(title_buf, game_name, sizeof(title_buf));
            ext = strrchr(title_buf, '.');
            if (ext)
               *ext = '\0';
         }
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
      }
   }

   /* Increment ticker for this frame */
   cannoli->ticker_idx++;

   /* Draw title with ticker-based scrolling for long titles
    * Skip title only for top-level custom main menu (show for platform screens and settings submenu) */
   if (!(cannoli->is_custom_main_menu && !cannoli->in_folder && !cannoli->in_main_settings_submenu))
   {
      int max_title_width = video_width - cannoli->margin_x * 2;
      int title_y = cannoli->margin_y + (int)(cannoli->font_size_title * 0.9f);
      char title_ticker[256];
      unsigned x_offset = 0;
      font_data_t *title_font = cannoli->font_title.font
            ? cannoli->font_title.font : cannoli->font.font;

      gfx_animation_ctx_ticker_smooth_t ticker;
      ticker.idx           = cannoli->ticker_idx;
      ticker.src_str       = title_buf;
      ticker.spacer        = NULL;
      ticker.dst_str       = title_ticker;
      ticker.dst_str_width = NULL;
      ticker.x_offset      = &x_offset;
      ticker.font          = title_font;
      ticker.dst_str_len   = sizeof(title_ticker);
      ticker.glyph_width   = (unsigned)cannoli->font_size_title;
      ticker.field_width   = (unsigned)max_title_width;
      ticker.font_scale    = 1.0f;
      ticker.type_enum     = TICKER_TYPE_BOUNCE;
      ticker.selected      = true;

      gfx_animation_ticker_smooth(&ticker);

      cannoli_draw_title(cannoli, p_disp, video_width, video_height,
            cannoli->margin_x + (int)x_offset, title_y,
            title_ticker, cannoli_color_text);
   }

   /* Calculate scroll */
   if (selection >= max_visible)
      start_idx = selection - max_visible + 1;
   else
      start_idx = 0;

   /* Draw menu entries - tight spacing below title, or from top for top-level main menu */
   if (cannoli->is_custom_main_menu && !cannoli->in_folder && !cannoli->in_main_settings_submenu)
      y = cannoli->margin_y;
   else
      y = cannoli->margin_y + (int)(cannoli->font_size_title * 1.4f);

   for (i = 0; i < max_visible && (start_idx + i) < list_size; i++)
   {
      menu_entry_t entry;
      const char *entry_label;
      char display_label[256];
      char *ptr;
      bool is_selected = ((start_idx + i) == selection);

      /* Calculate consistent text position */
      int pill_height = (int)(cannoli->font_size * 1.5f);
      int pill_y = y + (item_height - pill_height) / 2;
      int text_y = pill_y + pill_height / 2 + (int)(cannoli->font_size * 0.30f);

      MENU_ENTRY_INITIALIZE(entry);
      entry.flags |= MENU_ENTRY_FLAG_RICH_LABEL_ENABLED
                   | MENU_ENTRY_FLAG_VALUE_ENABLED;
      menu_entry_get(&entry, 0, (unsigned)(start_idx + i), NULL, true);

      /* For custom menus, prefer path (our custom label) over rich_label (RA's label)
       * For custom main menu, use label (display name) since path contains full file path
       * For core selection, use path (display name) since label contains core path
       * For main settings submenu, use path (our custom label) */
      if (cannoli->is_quick_menu || cannoli->in_settings_submenu || cannoli->selecting_core
            || cannoli->in_main_settings_submenu)
         entry_label = entry.path;
      else if (cannoli->is_custom_main_menu && !string_is_empty(entry.label))
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
      if (!(cannoli->is_custom_main_menu && !cannoli->in_folder && !cannoli->in_main_settings_submenu))
         cannoli_process_entry_type(entry.value, display_label, sizeof(display_label));

      /* Check if value should be displayed */
      bool show_value = !string_is_empty(entry.value)
                     && !string_is_equal(entry.value, "...")
                     && !cannoli_should_hide_value(entry.value);

      if (is_selected)
      {
         int pill_width;
         int text_width = cannoli_get_text_width(cannoli, display_label, false);
         int max_value_width = (video_width - cannoli->margin_x * 2) * 45 / 100;
         int value_gap = (int)(16 * cannoli->scale_factor);
         int max_label_width = show_value
               ? (video_width - cannoli->margin_x * 2 - max_value_width - value_gap)
               : (video_width - cannoli->margin_x * 2);

         /*
          * Pill width calculation:
          * - With value: spans from label to value (full content width + padding)
          * - Without value: fits snugly around label text with symmetric padding
          */
         if (show_value)
            pill_width = video_width - cannoli->margin_x * 2 + cannoli->pill_padding * 2;
         else
            pill_width = (text_width > max_label_width ? max_label_width : text_width)
                  + cannoli->pill_padding * 2;

         cannoli_draw_rounded_pill(cannoli, p_disp, userdata,
               cannoli->margin_x - cannoli->pill_padding, pill_y,
               pill_width, pill_height,
               video_width, video_height, cannoli_color_selection);

         /* Draw label with ticker scrolling if too long */
         {
            char label_ticker[256];
            unsigned x_offset = 0;
            uint64_t item_idx;

            /* Reset ticker when selection changes so scrolling starts from left */
            if (selection != cannoli->item_ticker_selection)
            {
               cannoli->item_ticker_selection = selection;
               cannoli->item_ticker_start = cannoli->ticker_idx;
            }
            item_idx = cannoli->ticker_idx - cannoli->item_ticker_start;

            gfx_animation_ctx_ticker_smooth_t ticker;
            ticker.idx           = item_idx;
            ticker.src_str       = display_label;
            ticker.spacer        = NULL;
            ticker.dst_str       = label_ticker;
            ticker.dst_str_width = NULL;
            ticker.x_offset      = &x_offset;
            ticker.font          = cannoli->font.font;
            ticker.dst_str_len   = sizeof(label_ticker);
            ticker.glyph_width   = (unsigned)cannoli->font_size;
            ticker.field_width   = (unsigned)max_label_width;
            ticker.font_scale    = 1.0f;
            ticker.type_enum     = TICKER_TYPE_BOUNCE;
            ticker.selected      = true;

            gfx_animation_ticker_smooth(&ticker);

            cannoli_draw_text(cannoli, p_disp, video_width, video_height,
                  cannoli->margin_x + (int)x_offset, text_y,
                  label_ticker, cannoli_color_text_dark, false);
         }

         /* Value stays on the right, truncated to max width */
         if (show_value)
         {
            char truncated_value[256];
            int value_width;

            cannoli_truncate_text(cannoli, entry.value, truncated_value,
                  sizeof(truncated_value), max_value_width, false);
            value_width = cannoli_get_text_width(cannoli, truncated_value, false);

            cannoli_draw_text(cannoli, p_disp, video_width, video_height,
                  video_width - cannoli->margin_x - value_width, text_y,
                  truncated_value, cannoli_color_text_dark, false);
         }
      }
      else
      {
         /* Non-selected: truncate long labels */
         int max_value_width = (video_width - cannoli->margin_x * 2) * 45 / 100;
         int value_gap = (int)(16 * cannoli->scale_factor);
         int max_label_width = show_value
               ? (video_width - cannoli->margin_x * 2 - max_value_width - value_gap)
               : (video_width - cannoli->margin_x * 2);
         char truncated_label[256];

         cannoli_truncate_text(cannoli, display_label, truncated_label,
               sizeof(truncated_label), max_label_width, false);

         cannoli_draw_text(cannoli, p_disp, video_width, video_height,
               cannoli->margin_x, text_y,
               truncated_label, cannoli_color_text, false);

         if (show_value)
         {
            char truncated_value[256];
            int max_value_width = (video_width - cannoli->margin_x * 2) * 45 / 100;
            int value_width;

            cannoli_truncate_text(cannoli, entry.value, truncated_value,
                  sizeof(truncated_value), max_value_width, false);
            value_width = cannoli_get_text_width(cannoli, truncated_value, false);

            cannoli_draw_text(cannoli, p_disp, video_width, video_height,
                  video_width - cannoli->margin_x - value_width, text_y,
                  truncated_value, cannoli_color_text, false);
         }
      }

      y += item_height;
   }

   /* Draw save slot selector if on Save/Load State entry */
   if (cannoli->show_slot_selector)
      cannoli_draw_slot_selector(cannoli, p_disp, userdata, video_width, video_height);

   /* Button legends */
   button_legend_y = video_height - cannoli->margin_y - cannoli->button_size;

   cannoli_draw_button_legend(cannoli, p_disp, userdata,
         cannoli->margin_x, button_legend_y,
         "B", "Back",
         video_width, video_height);

   {
      int pill_padding = (int)(6 * cannoli->scale_factor);
      int inner_padding = (int)(6 * cannoli->scale_factor);
      int label_width = cannoli_get_text_width(cannoli, "Select", true);
      int pill_width = pill_padding * 2 + cannoli->button_size + inner_padding + label_width
            + (int)(2 * cannoli->scale_factor);
      int legend_x = video_width - cannoli->margin_x - pill_width;

      cannoli_draw_button_legend(cannoli, p_disp, userdata,
            legend_x, button_legend_y,
            "A", "Select",
            video_width, video_height);
   }
}

/* ======================================================================
 * QUICK MENU CUSTOMIZATION
 * ====================================================================== */

static void cannoli_populate_menu_items(const cannoli_quick_item_t *items)
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
      const cannoli_quick_item_t *item = &items[i];
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
static bool cannoli_is_launched_from_cli(void)
{
   global_t *global = global_get_ptr();
   if (!global)
      return false;
   return (global->flags & GLOB_FLG_LAUNCHED_FROM_CLI) != 0;
}

/* Check if disc control is available for current core */
static bool cannoli_is_disc_control_available(void)
{
   rarch_system_info_t *sys_info = &runloop_state_get_ptr()->system;
   if (!sys_info)
      return false;
   return disk_control_enabled(&sys_info->disk_control);
}

/* Populate quick menu, with dynamic Exit/Quit based on CLI launch */
static void cannoli_populate_quick_menu(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   const cannoli_quick_item_t *item;
   bool from_cli = cannoli_is_launched_from_cli();

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

   for (item = cannoli_quick_menu_items; item->label != NULL || item->action == CANNOLI_EXIT_MARKER; item++)
   {
      const char *label;
      const char *action_label;
      enum msg_hash_enums action;

      /* Handle dynamic Quit entry - quits if CLI, exits to menu if not */
      if (item->action == CANNOLI_EXIT_MARKER)
      {
         label = "Quit";
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
static void cannoli_populate_settings_submenu(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   const cannoli_quick_item_t *item;
   bool show_disc_control = cannoli_is_disc_control_available();

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

   for (item = cannoli_settings_menu_items; item->label != NULL; item++)
   {
      const char *action_label;

      /* Skip Disc Control if not available */
      if (item->action == MENU_ENUM_LABEL_DISK_OPTIONS && !show_disc_control)
         continue;

      /* Skip Achievements if not compiled in */
#ifndef HAVE_CHEEVOS
      if (item->action == MENU_ENUM_LABEL_RETRO_ACHIEVEMENTS_SETTINGS)
         continue;
#endif

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
static void cannoli_populate_main_settings_submenu(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   const cannoli_quick_item_t *item;

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

   for (item = cannoli_main_settings_items; item->label != NULL; item++)
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
static bool cannoli_find_core_by_name(const char *name, char *core_path_out, size_t core_path_size)
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
static bool cannoli_read_folder_core(const char *folder_path, char *core_path_out, size_t core_path_size)
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
      if (cannoli_find_core_by_name(line, core_path_out, core_path_size))
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
static bool cannoli_save_folder_core(const char *folder_path, const char *core_path)
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
 * Populate the menu with a list of all installed cores.
 * Used when the user needs to select which core to use for a folder.
 */
static void cannoli_populate_core_selection(cannoli_t *cannoli, const char *content_path)
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
 * Strip leading sort prefix from folder name (e.g., "1) Game Boy" -> "Game Boy")
 * Pattern: one or more digits followed by ") "
 * Returns pointer to the start of the actual name (within the same string)
 */
static const char *cannoli_strip_sort_prefix(const char *name)
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
static void cannoli_populate_folder_menu(cannoli_t *cannoli, const char *directory, bool show_folder_slash)
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
            /* Show directories, optionally with leading slash
             * Strip sort prefix (e.g., "1) Game Boy" -> "Game Boy") */
            char display_name[256];
            const char *clean_name = cannoli_strip_sort_prefix(name);

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

   /* Add Settings option at the bottom (only at top level) */
   if (!show_folder_slash)
   {
      menu_entries_append(list,
            "Settings",
            "cannoli_main_settings",
            MENU_ENUM_LABEL_SETTINGS,
            MENU_SETTING_ACTION,
            0, 0, NULL);
   }
}

/* ======================================================================
 * MENU DRIVER INTERFACE
 * ====================================================================== */

/*
 * Try to load a font from the given path within the assets directory.
 * Returns the loaded font or NULL if not found.
 */
static font_data_t *cannoli_try_load_font(gfx_display_t *p_disp,
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

static void *cannoli_init(void **userdata, bool video_is_threaded)
{
   gfx_display_t *p_disp = disp_get_ptr();
   cannoli_t *cannoli = (cannoli_t*)calloc(1, sizeof(*cannoli));
   menu_handle_t *menu = (menu_handle_t*)calloc(1, sizeof(*menu));

   if (!cannoli || !menu)
   {
      if (cannoli) free(cannoli);
      if (menu) free(menu);
      return NULL;
   }

   *userdata = cannoli;

   p_disp->framebuf_width = 0;
   p_disp->framebuf_height = 0;

   return menu;
}

static void cannoli_free(void *data)
{
   cannoli_t *cannoli = (cannoli_t*)data;
   if (cannoli)
   {
      cannoli->font.font = NULL;
      cannoli->font_small.font = NULL;
      cannoli->font_title.font = NULL;
   }
}

static void cannoli_context_reset(void *data, bool is_threaded)
{
   cannoli_t *cannoli = (cannoli_t*)data;
   gfx_display_t *p_disp = disp_get_ptr();
   settings_t *settings = config_get_ptr();
   char fontpath[PATH_MAX_LENGTH];
   float scale_factor;

   if (!cannoli)
      return;

   /*
    * Use RetroArch's DPI-aware scaling system instead of simple pixel ratio.
    * gfx_display_get_dpi_scale() intelligently handles:
    * - Actual display DPI when available from the OS
    * - Smart blending based on screen size (TVs get larger UI for couch viewing)
    * - Fallback to pixel-based scaling when DPI info unavailable
    *
    * Parameters:
    * - width/height: current framebuffer dimensions
    * - fullscreen: affects DPI detection on some platforms
    * - false: don't use widget scale (we're a menu driver, not widgets)
    */
   scale_factor = gfx_display_get_dpi_scale(p_disp,
         settings,
         p_disp->framebuf_width,
         p_disp->framebuf_height,
         settings->bools.video_fullscreen,
         false);

   /* Apply user's menu scale preference from Settings > User Interface */
   scale_factor *= settings->floats.menu_scale_factor;

   /* Ensure a minimum scale to prevent unusably small UI */
   if (scale_factor < 0.5f)
      scale_factor = 0.5f;

   cannoli->scale_factor = scale_factor;
   cannoli->font_size = CANNOLI_BASE_FONT_SIZE * scale_factor;
   cannoli->font_size_small = CANNOLI_BASE_FONT_SIZE * scale_factor * 0.75f;
   cannoli->font_size_title = CANNOLI_BASE_FONT_SIZE * scale_factor * 1.1f;
   /* Tiny font sized to match dot indicators (dot_radius * 2 is diameter) */
   cannoli->font_size_tiny = 4 * scale_factor * 2.5f;

   /* Clamp font sizes to ensure readability */
   if (cannoli->font_size < CANNOLI_MIN_FONT_SIZE)
      cannoli->font_size = CANNOLI_MIN_FONT_SIZE;
   if (cannoli->font_size_small < CANNOLI_MIN_FONT_SIZE - 2)
      cannoli->font_size_small = CANNOLI_MIN_FONT_SIZE - 2;
   if (cannoli->font_size_tiny < 8)
      cannoli->font_size_tiny = 8;

   /* Pill padding scales with font size so it stays proportional with different fonts */
   cannoli->pill_padding = (int)(cannoli->font_size * CANNOLI_PILL_PADDING_RATIO);
   cannoli->button_size = (int)(CANNOLI_BUTTON_CIRCLE_SIZE * scale_factor);

   /* Free existing fonts before reloading */
   if (cannoli->font.font)
   {
      font_driver_free(cannoli->font.font);
      cannoli->font.font = NULL;
   }
   if (cannoli->font_small.font)
   {
      font_driver_free(cannoli->font_small.font);
      cannoli->font_small.font = NULL;
   }
   if (cannoli->font_title.font)
   {
      font_driver_free(cannoli->font_title.font);
      cannoli->font_title.font = NULL;
   }
   if (cannoli->font_tiny.font)
   {
      font_driver_free(cannoli->font_tiny.font);
      cannoli->font_tiny.font = NULL;
   }

   fontpath[0] = '\0';

   /*
    * Font loading priority:
    * 1. Cannoli-specific font (assets/cannoli/font.ttf) for custom styling
    * 2. XMB font (assets/xmb/monochrome/font.ttf) commonly available
    * 3. Ozone font (assets/ozone/regular.ttf) as final fallback
    */
   cannoli->font.font = cannoli_try_load_font(p_disp,
         settings->paths.directory_assets, "cannoli/font.ttf",
         cannoli->font_size, is_threaded, fontpath, sizeof(fontpath));

   if (!cannoli->font.font)
      cannoli->font.font = cannoli_try_load_font(p_disp,
            settings->paths.directory_assets, "xmb/monochrome/font.ttf",
            cannoli->font_size, is_threaded, fontpath, sizeof(fontpath));

   if (!cannoli->font.font)
      cannoli->font.font = cannoli_try_load_font(p_disp,
            settings->paths.directory_assets, "ozone/regular.ttf",
            cannoli->font_size, is_threaded, fontpath, sizeof(fontpath));

   /* Load additional font sizes using the same font file that worked */
   if (cannoli->font.font && fontpath[0] != '\0')
   {
      cannoli->font_small.font = gfx_display_font_file(p_disp, fontpath, cannoli->font_size_small, is_threaded);
      cannoli->font_title.font = gfx_display_font_file(p_disp, fontpath, cannoli->font_size_title, is_threaded);
      cannoli->font_tiny.font = gfx_display_font_file(p_disp, fontpath, cannoli->font_size_tiny, is_threaded);
   }

   cannoli->font.line_height = (int)(cannoli->font_size * CANNOLI_LINE_HEIGHT);
   cannoli->font.glyph_width = (int)(cannoli->font_size * 0.6f);
   cannoli->font_small.line_height = (int)(cannoli->font_size_small * CANNOLI_LINE_HEIGHT);
   cannoli->font_small.glyph_width = (int)(cannoli->font_size_small * 0.6f);
   cannoli->font_title.line_height = (int)(cannoli->font_size_title * CANNOLI_LINE_HEIGHT);
   cannoli->font_title.glyph_width = (int)(cannoli->font_size_title * 0.6f);

   if (cannoli->font.line_height < 20)
      cannoli->font.line_height = 20;
   if (cannoli->font_small.line_height < 15)
      cannoli->font_small.line_height = 15;

   /* Initialize save slot selector state */
   gfx_thumbnail_reset(&cannoli->savestate_thumbnail);
   cannoli->savestate_thumbnail_path[0] = '\0';
   cannoli->preview_slot = 0;
   cannoli->show_slot_selector = false;
   cannoli->last_selection = 0;

   /* Initialize ticker for text scrolling */
   cannoli->ticker_idx = 0;
   cannoli->item_ticker_start = 0;
   cannoli->item_ticker_selection = (size_t)-1;

   gfx_display_init_white_texture();
}

static void cannoli_context_destroy(void *data)
{
   cannoli_t *cannoli = (cannoli_t*)data;

   if (cannoli)
   {
      if (cannoli->font.font)
      {
         font_driver_free(cannoli->font.font);
         cannoli->font.font = NULL;
      }
      if (cannoli->font_small.font)
      {
         font_driver_free(cannoli->font_small.font);
         cannoli->font_small.font = NULL;
      }
      if (cannoli->font_title.font)
      {
         font_driver_free(cannoli->font_title.font);
         cannoli->font_title.font = NULL;
      }
      if (cannoli->font_tiny.font)
      {
         font_driver_free(cannoli->font_tiny.font);
         cannoli->font_tiny.font = NULL;
      }

      /* Clean up save slot thumbnail */
      gfx_thumbnail_reset(&cannoli->savestate_thumbnail);
   }

   gfx_display_deinit_white_texture();
}

static void cannoli_render(void *data, unsigned width, unsigned height, bool is_idle)
{
   cannoli_t *cannoli = (cannoli_t*)data;

   if (!cannoli)
      return;

   if (cannoli->width != width || cannoli->height != height)
   {
      cannoli->width = width;
      cannoli->height = height;
      cannoli->margin_x = (int)(width * CANNOLI_MARGIN_RATIO);
      cannoli->margin_y = (int)(height * CANNOLI_MARGIN_RATIO);
   }
}

static void cannoli_frame(void *data, video_frame_info_t *video_info)
{
   cannoli_t *cannoli = (cannoli_t*)data;
   gfx_display_t *p_disp = disp_get_ptr();
   void *userdata;
   unsigned video_width, video_height;

   if (!cannoli || !p_disp || !video_info)
      return;

   userdata = video_info->userdata;
   video_width = video_info->width;
   video_height = video_info->height;

   if (video_width == 0 || video_height == 0)
      return;

   if (!cannoli->font.font)
      return;

   if (cannoli->margin_x == 0 || cannoli->margin_y == 0)
   {
      cannoli->margin_x = (int)(video_width * CANNOLI_MARGIN_RATIO);
      cannoli->margin_y = (int)(video_height * CANNOLI_MARGIN_RATIO);
      if (cannoli->margin_x < 10) cannoli->margin_x = 10;
      if (cannoli->margin_y < 10) cannoli->margin_y = 10;
   }

   font_bind(&cannoli->font);
   if (cannoli->font_small.font)
      font_bind(&cannoli->font_small);
   if (cannoli->font_title.font)
      font_bind(&cannoli->font_title);
   if (cannoli->font_tiny.font)
      font_bind(&cannoli->font_tiny);

   cannoli_draw_bg(cannoli, p_disp, userdata, video_width, video_height);
   cannoli_render_menu(cannoli, p_disp, userdata, video_width, video_height);

   if (cannoli->font.font)
      font_flush(video_width, video_height, &cannoli->font);
   if (cannoli->font_small.font)
      font_flush(video_width, video_height, &cannoli->font_small);
   if (cannoli->font_title.font)
      font_flush(video_width, video_height, &cannoli->font_title);
   if (cannoli->font_tiny.font)
      font_flush(video_width, video_height, &cannoli->font_tiny);
}

static void cannoli_populate_entries(void *data,
      const char *path, const char *label, unsigned k)
{
   cannoli_t *cannoli = (cannoli_t*)data;
   const char *content_settings_label = msg_hash_to_str(MENU_ENUM_LABEL_CONTENT_SETTINGS);
   const char *main_menu_label = msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU);
   bool is_content_settings = false;
   bool is_main_menu = false;

   if (!cannoli)
      return;

   /* Check what menu we're in */
   if (label)
   {
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
            /* Check if returning to main settings submenu */
            if (cannoli->return_to_main_settings_submenu)
            {
               cannoli_populate_main_settings_submenu();
               cannoli->is_custom_main_menu = true;
               cannoli->in_main_settings_submenu = true;
               cannoli->return_to_main_settings_submenu = false;
               cannoli->in_folder = false;
               /* Restore selection in settings submenu */
               if (menu_st_local)
                  menu_st_local->selection_ptr = cannoli->saved_settings_selection;
            }
            /* Check if returning from a game - restore folder state */
            else if (cannoli->return_to_folder && !string_is_empty(cannoli->last_launched_folder))
            {
               cannoli_populate_folder_menu(cannoli, cannoli->last_launched_folder, true);
               strlcpy(cannoli->current_folder_path, cannoli->last_launched_folder,
                     sizeof(cannoli->current_folder_path));
               /* Restore the folder's core path */
               strlcpy(cannoli->folder_core_path, cannoli->last_folder_core_path,
                     sizeof(cannoli->folder_core_path));
               cannoli->is_custom_main_menu = true;
               cannoli->in_folder = true;
               cannoli->return_to_folder = false;
               /* Restore selection to the game that was played */
               if (menu_st_local)
                  menu_st_local->selection_ptr = cannoli->folder_selection;
            }
            else
            {
               cannoli_populate_folder_menu(cannoli, start_dir, false);  /* Top level - no slash */
               strlcpy(cannoli->current_folder_path, start_dir,
                     sizeof(cannoli->current_folder_path));
               cannoli->is_custom_main_menu = true;
               cannoli->in_folder = false;
            }
         }
         cannoli->is_quick_menu = false;
         cannoli->in_settings_submenu = false;
         return;
      }

      if (is_content_settings)
      {
         /* Check if we should return to the Advanced settings submenu */
         if (cannoli->return_to_settings_submenu)
         {
            cannoli_populate_settings_submenu();
            cannoli->in_settings_submenu = true;
            cannoli->return_to_settings_submenu = false;
         }
         else
         {
            cannoli_populate_quick_menu();
            cannoli->in_settings_submenu = false;
         }
         cannoli->is_quick_menu = true;
         cannoli->is_custom_main_menu = false;

         /*
          * Reset thumbnail state when entering quick menu so it reloads.
          * This ensures the thumbnail is refreshed (e.g., if a new screenshot
          * was taken since last viewing).
          */
         gfx_thumbnail_reset(&cannoli->savestate_thumbnail);
         cannoli->savestate_thumbnail_path[0] = '\0';
         cannoli->last_selection = (size_t)-1;  /* Force reload on next render */
      }
      else
      {
         /* Don't reset return_to_settings_submenu here - we need it when coming back */
         cannoli->is_quick_menu = false;
         cannoli->in_settings_submenu = false;
         cannoli->is_custom_main_menu = false;
      }
   }
   else
   {
      /* Don't reset return_to_settings_submenu here - we need it when coming back */
      cannoli->is_quick_menu = false;
      cannoli->in_settings_submenu = false;
      cannoli->is_custom_main_menu = false;
   }
}

static void cannoli_navigation_set(void *data, bool scroll) { }
static void cannoli_navigation_clear(void *data, bool pending_push) { }
static void cannoli_navigation_set_last(void *data) { }

static int cannoli_pointer_up(void *data,
      unsigned x, unsigned y, unsigned ptr,
      enum menu_input_pointer_gesture gesture,
      menu_file_list_cbs_t *cbs,
      menu_entry_t *entry, unsigned action)
{
   return 0;
}

static int cannoli_environ(enum menu_environ_cb type, void *data, void *userdata)
{
   return -1;
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
static int cannoli_entry_action(void *userdata, menu_entry_t *entry,
      size_t i, enum menu_action action)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   cannoli_t *cannoli = NULL;

   if (menu_st)
      cannoli = (cannoli_t*)menu_st->userdata;

   if (cannoli && cannoli->is_quick_menu)
   {
      /* Handle input for save slot selection when on Save/Load entry */
      if (cannoli->show_slot_selector)
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
            cannoli->preview_slot--;
            if (cannoli->preview_slot < 0)
               cannoli->preview_slot = CANNOLI_NUM_SLOTS - 1;
            settings->ints.state_slot = cannoli->preview_slot - 1;
            cannoli_load_slot_thumbnail(cannoli, cannoli->preview_slot);
            return 0;  /* Consume input */
         }

         if (action == MENU_ACTION_RIGHT)
         {
            cannoli->preview_slot++;
            if (cannoli->preview_slot >= CANNOLI_NUM_SLOTS)
               cannoli->preview_slot = 0;
            settings->ints.state_slot = cannoli->preview_slot - 1;
            cannoli_load_slot_thumbnail(cannoli, cannoli->preview_slot);
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
      if (action == MENU_ACTION_CANCEL && cannoli->in_settings_submenu)
      {
         cannoli->in_settings_submenu = false;
         cannoli->return_to_settings_submenu = false;
         cannoli_populate_quick_menu();
         menu_st->selection_ptr = cannoli->saved_quick_menu_selection;
         return 0;
      }

      /* Back button in main quick menu: close menu entirely and resume game */
      if (action == MENU_ACTION_CANCEL && !cannoli->in_settings_submenu)
      {
         command_event(CMD_EVENT_MENU_TOGGLE, NULL);
         return 0;
      }

      /* Select "Advanced" entry: enter the settings submenu */
      if (action == MENU_ACTION_OK && entry && !cannoli->in_settings_submenu)
      {
         const char *entry_label = NULL;

         if (!string_is_empty(entry->rich_label))
            entry_label = entry->rich_label;
         else if (!string_is_empty(entry->path))
            entry_label = entry->path;

         if (entry_label && string_is_equal(entry_label, "Advanced"))
         {
            cannoli->saved_quick_menu_selection = menu_st->selection_ptr;
            cannoli->in_settings_submenu = true;
            cannoli->return_to_settings_submenu = false;
            cannoli_populate_settings_submenu();
            menu_st->selection_ptr = 0;
            return 0;
         }

         /* Handle "Quit" - if not CLI, close content and go to main menu */
         if (entry_label && string_is_equal(entry_label, "Quit"))
         {
            if (!cannoli_is_launched_from_cli())
            {
               /* Reset cannoli state */
               cannoli->is_quick_menu = false;
               cannoli->in_settings_submenu = false;
               cannoli->return_to_settings_submenu = false;

               /* Unload core and flush to main menu */
               command_event(CMD_EVENT_UNLOAD_CORE, NULL);
               menu_entries_flush_stack(msg_hash_to_str(MENU_ENUM_LABEL_MAIN_MENU), 0);
               return 0;
            }
            /* If CLI, let the default handler quit RetroArch */
         }
      }

      /*
       * When entering an RA settings screen from Advanced submenu,
       * set flag to return to Advanced (not main menu) when backing out.
       */
      if (action == MENU_ACTION_OK && cannoli->in_settings_submenu)
      {
         cannoli->return_to_settings_submenu = true;
      }
   }

   /* Handle core selection mode */
   if (cannoli && cannoli->selecting_core)
   {
      /* Cancel core selection - go back to folder */
      if (action == MENU_ACTION_CANCEL)
      {
         cannoli->selecting_core = false;
         cannoli->pending_content_path[0] = '\0';
         cannoli_populate_folder_menu(cannoli, cannoli->current_folder_path, true);
         return 0;
      }

      /* Core selected - save to .core.txt and launch content */
      if (action == MENU_ACTION_OK && entry)
      {
         const char *selected_core = entry->label;  /* Core path is in label */

         if (!string_is_empty(selected_core) && path_is_valid(selected_core))
         {
            content_ctx_info_t content_info;

            /* Save the selected core to .core.txt for this folder */
            cannoli_save_folder_core(cannoli->current_folder_path, selected_core);

            /* Update the folder's core path */
            strlcpy(cannoli->folder_core_path, selected_core,
                  sizeof(cannoli->folder_core_path));

            content_info.argc        = 0;
            content_info.argv        = NULL;
            content_info.args        = NULL;
            content_info.environ_get = NULL;

            /* Save folder state so we can return after quitting */
            cannoli->folder_selection = menu_st->selection_ptr;
            strlcpy(cannoli->last_launched_folder, cannoli->current_folder_path,
                  sizeof(cannoli->last_launched_folder));
            strlcpy(cannoli->last_folder_core_path, cannoli->folder_core_path,
                  sizeof(cannoli->last_folder_core_path));
            cannoli->return_to_folder = true;

            cannoli->selecting_core = false;
            cannoli->is_custom_main_menu = false;
            cannoli->in_folder = false;

            /* Close menu before loading content */
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);

            task_push_load_content_with_new_core_from_menu(
                  selected_core,
                  cannoli->pending_content_path,
                  &content_info,
                  CORE_TYPE_PLAIN, NULL, NULL);

            cannoli->pending_content_path[0] = '\0';
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

   /* Handle custom main menu (launcher mode) navigation */
   if (cannoli && cannoli->is_custom_main_menu)
   {
      /* Block back navigation when inside a folder */
      if (action == MENU_ACTION_CANCEL && cannoli->in_folder)
      {
         /* Go back to top-level folder listing */
         settings_t *settings = config_get_ptr();
         const char *start_dir = settings->paths.directory_menu_content;

         if (!string_is_empty(start_dir))
         {
            cannoli_populate_folder_menu(cannoli, start_dir, false);  /* Back to top - no slash */
            strlcpy(cannoli->current_folder_path, start_dir,
                  sizeof(cannoli->current_folder_path));
            cannoli->in_folder = false;
            /* Restore saved main menu selection */
            menu_st->selection_ptr = cannoli->main_menu_selection;
         }
         return 0;
      }

      /* Back button in main settings submenu: return to main menu */
      if (action == MENU_ACTION_CANCEL && cannoli->in_main_settings_submenu)
      {
         settings_t *settings = config_get_ptr();
         const char *start_dir = settings->paths.directory_menu_content;

         cannoli->in_main_settings_submenu = false;
         cannoli->return_to_main_settings_submenu = false;
         if (!string_is_empty(start_dir))
         {
            cannoli_populate_folder_menu(cannoli, start_dir, false);
            strlcpy(cannoli->current_folder_path, start_dir,
                  sizeof(cannoli->current_folder_path));
         }
         /* Restore saved main menu selection */
         menu_st->selection_ptr = cannoli->saved_main_menu_selection;
         return 0;
      }

      /* Block back navigation at top level (nowhere to go) */
      if (action == MENU_ACTION_CANCEL && !cannoli->in_folder && !cannoli->in_main_settings_submenu)
      {
         return 0;  /* Do nothing - can't go up from top level */
      }

      /* Handle folder/file/settings selection */
      if (action == MENU_ACTION_OK && entry)
      {
         /* Check for Settings entry - show custom settings submenu */
         if (entry->enum_idx == MENU_ENUM_LABEL_SETTINGS && !cannoli->in_main_settings_submenu)
         {
            cannoli->saved_main_menu_selection = menu_st->selection_ptr;
            cannoli->in_main_settings_submenu = true;
            cannoli_populate_main_settings_submenu();
            menu_st->selection_ptr = 0;
            return 0;
         }

         /* Handle selection within main settings submenu */
         if (cannoli->in_main_settings_submenu)
         {
            /* Save selection so we can return to same position */
            cannoli->saved_settings_selection = menu_st->selection_ptr;
            /* Set flag to return to settings submenu when backing out */
            cannoli->return_to_main_settings_submenu = true;
            /* Let generic handler process the RA menu item */
            cannoli->is_custom_main_menu = false;
            cannoli->in_main_settings_submenu = false;
            return generic_menu_entry_action(userdata, entry, i, action);
         }

         /* For custom main menu entries, the full path is in entry->label
          * (entry->path contains the display name without path/extension) */
         const char *item_path = entry->label;

         if (!string_is_empty(item_path))
         {
            if (path_is_directory(item_path))
            {
               /* Save current selection before entering folder */
               cannoli->main_menu_selection = menu_st->selection_ptr;

               /* Enter the selected folder */
               cannoli_populate_folder_menu(cannoli, item_path, true);  /* Inside folder - show slash */
               strlcpy(cannoli->current_folder_path, item_path,
                     sizeof(cannoli->current_folder_path));

               /* Try to read folder's core.txt */
               if (!cannoli_read_folder_core(item_path, cannoli->folder_core_path,
                     sizeof(cannoli->folder_core_path)))
                  cannoli->folder_core_path[0] = '\0';  /* No core.txt found */

               cannoli->in_folder = true;
               menu_st->selection_ptr = 0;
               return 0;
            }
            else if (path_is_valid(item_path))
            {
               /* Launch the selected file (ROM) */
               const char *core_path = NULL;
               content_ctx_info_t content_info;

               /* Check if folder has a specific core assigned via .core.txt */
               if (!string_is_empty(cannoli->folder_core_path))
               {
                  core_path = cannoli->folder_core_path;
               }
               else
               {
                  /* No .core.txt - show core selection screen */
                  strlcpy(cannoli->pending_content_path, item_path,
                        sizeof(cannoli->pending_content_path));
                  cannoli->selecting_core = true;
                  cannoli_populate_core_selection(cannoli, item_path);
                  menu_st->selection_ptr = 0;
                  return 0;
               }

               if (core_path && path_is_valid(core_path))
               {
                  content_info.argc        = 0;
                  content_info.argv        = NULL;
                  content_info.args        = NULL;
                  content_info.environ_get = NULL;

                  /* Save folder state so we can return after quitting */
                  cannoli->folder_selection = menu_st->selection_ptr;
                  strlcpy(cannoli->last_launched_folder, cannoli->current_folder_path,
                        sizeof(cannoli->last_launched_folder));
                  strlcpy(cannoli->last_folder_core_path, cannoli->folder_core_path,
                        sizeof(cannoli->last_folder_core_path));
                  cannoli->return_to_folder = true;

                  cannoli->is_custom_main_menu = false;
                  cannoli->in_folder = false;

                  /* Close menu before loading content */
                  command_event(CMD_EVENT_MENU_TOGGLE, NULL);

                  task_push_load_content_with_new_core_from_menu(
                        core_path,           /* Core to use */
                        item_path,           /* Content path */
                        &content_info,
                        CORE_TYPE_PLAIN, NULL, NULL);

                  return 0;
               }
               /* No compatible core found - do nothing for now */
               return 0;
            }
         }
      }
   }

   /* Delegate all other input to RetroArch's generic menu handler */
   return generic_menu_entry_action(userdata, entry, i, action);
}

menu_ctx_driver_t menu_ctx_cannoli = {
   NULL,                         /* set_texture */
   NULL,                         /* render_messagebox */
   cannoli_render,
   cannoli_frame,
   cannoli_init,
   cannoli_free,
   cannoli_context_reset,
   cannoli_context_destroy,
   cannoli_populate_entries,
   NULL,                         /* toggle */
   cannoli_navigation_clear,
   NULL,                         /* navigation_decrement */
   NULL,                         /* navigation_increment */
   cannoli_navigation_set,
   cannoli_navigation_set_last,
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
   "cannoli",
   cannoli_environ,
   NULL,                         /* update_thumbnail_path */
   NULL,                         /* update_thumbnail_image */
   NULL,                         /* refresh_thumbnail_image */
   NULL,                         /* set_thumbnail_content */
   NULL,                         /* osk_ptr_at_pos */
   NULL,                         /* update_savestate_thumbnail_path */
   NULL,                         /* update_savestate_thumbnail_image */
   NULL,                         /* pointer_down */
   cannoli_pointer_up,
   cannoli_entry_action
};
