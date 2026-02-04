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
 * - Supports a two-level menu: main quick menu and "Game Options" submenu
 *
 * Scaling:
 * - Uses RetroArch's DPI-aware scaling via gfx_display_get_dpi_scale()
 * - This provides intelligent scaling based on display size and viewing distance
 * - Respects user's menu_scale_factor preference from settings
 *
 * Navigation State Machine:
 * - is_quick_menu: true when viewing the custom quick menu (not RA settings)
 * - in_settings_submenu: true when in "Game Options" submenu
 * - return_to_settings_submenu: flag to return to submenu after backing out of RA menu
 * - Back button in main quick menu closes menu and resumes content
 * - Back button in Game Options submenu returns to main quick menu
 */

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
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
#include "../../gfx/font_driver.h"
#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../runloop.h"
#include "../../paths.h"
#include <file/file_path.h>

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
static uint32_t cannoli_color_text       = 0xFFFFFFFF;  /* White (RGBA packed) */
static uint32_t cannoli_color_text_dark  = 0x000000FF;  /* Black (RGBA packed) */

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
 * Sentinel value used to identify the "Game Options" menu entry.
 * When a menu item has this value as its action, selecting it opens
 * the custom settings submenu instead of triggering a RetroArch action.
 * The value 0xCAFE is arbitrary, chosen to not conflict with any
 * MENU_ENUM_LABEL_* values.
 */
#define CANNOLI_SETTINGS_SUBMENU_MARKER 0xCAFE

/* Main custom quick menu */
static const cannoli_quick_item_t cannoli_quick_menu_items[] = {
   { "Resume",        MENU_ENUM_LABEL_RESUME_CONTENT },
   { "Restart",       MENU_ENUM_LABEL_RESTART_CONTENT },
   { "Save State",    MENU_ENUM_LABEL_SAVE_STATE },
   { "Load State",    MENU_ENUM_LABEL_LOAD_STATE },
   { "Game Options",  CANNOLI_SETTINGS_SUBMENU_MARKER },  /* Opens custom settings submenu */
   { "Advanced",      MENU_ENUM_LABEL_SETTINGS },         /* Opens full RA settings */
   { "Quit",          MENU_ENUM_LABEL_QUIT_RETROARCH },
   { NULL, 0 }
};

/* Custom settings submenu items */
static const cannoli_quick_item_t cannoli_settings_menu_items[] = {
   { "State Slot",    MENU_ENUM_LABEL_STATE_SLOT },
   { "Core Options",  MENU_ENUM_LABEL_CORE_OPTIONS },
   { "Controls",      MENU_ENUM_LABEL_CORE_INPUT_REMAPPING_OPTIONS },
   { "Shaders",       MENU_ENUM_LABEL_SHADER_OPTIONS },
   { "Overrides",     MENU_ENUM_LABEL_QUICK_MENU_OVERRIDE_OPTIONS },
   { "Cheats",        MENU_ENUM_LABEL_CORE_CHEAT_OPTIONS },
   { "Disk Control",  MENU_ENUM_LABEL_DISK_OPTIONS },
   { "Screenshot",    MENU_ENUM_LABEL_TAKE_SCREENSHOT },
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
   float font_size;
   float font_size_small;
   float font_size_title;

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
   bool return_to_settings_submenu;  /* Track if we should return to Game Options submenu */
   size_t saved_quick_menu_selection; /* Remember position in main quick menu */
} cannoli_t;

/* ======================================================================
 * DRAWING FUNCTIONS
 * ====================================================================== */

/* Forward declarations */
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
   if (cannoli->in_settings_submenu)
   {
      strlcpy(title_buf, "Game Options", sizeof(title_buf));
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
   }

   /* Draw title */
   cannoli_draw_title(cannoli, p_disp, video_width, video_height,
         cannoli->margin_x, cannoli->margin_y + (int)(cannoli->font_size_title * 0.9f),
         title_buf, cannoli_color_text);

   /* Calculate scroll */
   if (selection >= max_visible)
      start_idx = selection - max_visible + 1;
   else
      start_idx = 0;

   /* Draw menu entries - tight spacing below title */
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

      /* For custom menus, prefer path (our custom label) over rich_label (RA's label) */
      if (cannoli->is_quick_menu || cannoli->in_settings_submenu)
         entry_label = entry.path;
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

      /* Check if value is "..." - if so, don't display it */
      bool show_value = !string_is_empty(entry.value) && !string_is_equal(entry.value, "...");

      if (is_selected)
      {
         int pill_width;
         int text_width = cannoli_get_text_width(cannoli, display_label, false);

         /*
          * Pill width calculation:
          * - With value: spans from label to value (full content width + padding)
          * - Without value: fits snugly around label text with symmetric padding
          */
         if (show_value)
            pill_width = video_width - cannoli->margin_x * 2 + cannoli->pill_padding * 2;
         else
            pill_width = text_width + cannoli->pill_padding * 2;

         cannoli_draw_rounded_pill(cannoli, p_disp, userdata,
               cannoli->margin_x - cannoli->pill_padding, pill_y,
               pill_width, pill_height,
               video_width, video_height, cannoli_color_selection);

         cannoli_draw_text(cannoli, p_disp, video_width, video_height,
               cannoli->margin_x, text_y,
               display_label, cannoli_color_text_dark, false);

         /* Value stays on the right, truncated to max width */
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
                  truncated_value, cannoli_color_text_dark, false);
         }
      }
      else
      {
         cannoli_draw_text(cannoli, p_disp, video_width, video_height,
               cannoli->margin_x, text_y,
               display_label, cannoli_color_text, false);

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
   cannoli->font_size_title = CANNOLI_BASE_FONT_SIZE * scale_factor * 1.4f;

   /* Clamp font sizes to ensure readability */
   if (cannoli->font_size < CANNOLI_MIN_FONT_SIZE)
      cannoli->font_size = CANNOLI_MIN_FONT_SIZE;
   if (cannoli->font_size_small < CANNOLI_MIN_FONT_SIZE - 2)
      cannoli->font_size_small = CANNOLI_MIN_FONT_SIZE - 2;

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

   cannoli_draw_bg(cannoli, p_disp, userdata, video_width, video_height);
   cannoli_render_menu(cannoli, p_disp, userdata, video_width, video_height);

   if (cannoli->font.font)
      font_flush(video_width, video_height, &cannoli->font);
   if (cannoli->font_small.font)
      font_flush(video_width, video_height, &cannoli->font_small);
   if (cannoli->font_title.font)
      font_flush(video_width, video_height, &cannoli->font_title);
}

static void cannoli_populate_entries(void *data,
      const char *path, const char *label, unsigned k)
{
   cannoli_t *cannoli = (cannoli_t*)data;
   const char *content_settings_label = msg_hash_to_str(MENU_ENUM_LABEL_CONTENT_SETTINGS);
   bool is_content_settings = false;

   if (!cannoli)
      return;

   /* Check if this is the quick menu (content_settings) */
   if (label)
   {
      if (content_settings_label && string_is_equal(label, content_settings_label))
         is_content_settings = true;

      /* Also check enum_idx from the menu stack */
      if (!is_content_settings)
      {
         struct menu_state *menu_st = menu_state_get_ptr();
         if (menu_st && menu_st->entries.list)
         {
            enum msg_hash_enums enum_idx = MSG_UNKNOWN;
            menu_entries_get_last_stack(NULL, NULL, NULL, &enum_idx, NULL);
            if (enum_idx == MENU_ENUM_LABEL_CONTENT_SETTINGS)
               is_content_settings = true;
         }
      }

      if (is_content_settings)
      {
         /* Check if we should return to the Game Options submenu */
         if (cannoli->return_to_settings_submenu)
         {
            cannoli_populate_menu_items(cannoli_settings_menu_items);
            cannoli->in_settings_submenu = true;
            cannoli->return_to_settings_submenu = false;
         }
         else
         {
            cannoli_populate_menu_items(cannoli_quick_menu_items);
            cannoli->in_settings_submenu = false;
         }
         cannoli->is_quick_menu = true;
      }
      else
      {
         /* Don't reset return_to_settings_submenu here - we need it when coming back */
         cannoli->is_quick_menu = false;
         cannoli->in_settings_submenu = false;
      }
   }
   else
   {
      /* Don't reset return_to_settings_submenu here - we need it when coming back */
      cannoli->is_quick_menu = false;
      cannoli->in_settings_submenu = false;
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
 *         +----(B: back)----+          [Game Options]
 *                           |                  |
 *                           |                  v
 *                           +--------  [RA Settings Screen]
 *                                             |
 *                                      (B: back to Game Options)
 *
 * Key behaviors:
 * - B in main quick menu: closes menu and resumes game
 * - B in Game Options submenu: returns to main quick menu
 * - B in RA settings (entered from Game Options): returns to Game Options
 * - A on "Game Options": enters the custom settings submenu
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
      /* Back button in Game Options submenu: return to main quick menu */
      if (action == MENU_ACTION_CANCEL && cannoli->in_settings_submenu)
      {
         cannoli->in_settings_submenu = false;
         cannoli->return_to_settings_submenu = false;
         cannoli_populate_menu_items(cannoli_quick_menu_items);
         menu_st->selection_ptr = cannoli->saved_quick_menu_selection;
         return 0;
      }

      /* Back button in main quick menu: close menu entirely and resume game */
      if (action == MENU_ACTION_CANCEL && !cannoli->in_settings_submenu)
      {
         command_event(CMD_EVENT_MENU_TOGGLE, NULL);
         return 0;
      }

      /* Select "Game Options" entry: enter the settings submenu */
      if (action == MENU_ACTION_OK && entry && !cannoli->in_settings_submenu)
      {
         const char *entry_label = NULL;

         if (!string_is_empty(entry->rich_label))
            entry_label = entry->rich_label;
         else if (!string_is_empty(entry->path))
            entry_label = entry->path;

         if (entry_label && string_is_equal(entry_label, "Game Options"))
         {
            cannoli->saved_quick_menu_selection = menu_st->selection_ptr;
            cannoli->in_settings_submenu = true;
            cannoli->return_to_settings_submenu = false;
            cannoli_populate_menu_items(cannoli_settings_menu_items);
            menu_st->selection_ptr = 0;
            return 0;
         }
      }

      /*
       * When entering an RA settings screen from Game Options submenu,
       * set flag to return to Game Options (not main menu) when backing out.
       */
      if (action == MENU_ACTION_OK && cannoli->in_settings_submenu)
      {
         cannoli->return_to_settings_submenu = true;
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
