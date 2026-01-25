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
 * A minimal, clean GPU-rendered menu driver.
 * Features custom menu organization that's easy to modify.
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
#include "../menu_displaylist.h"
#include "../menu_cbs.h"
#include "../../msg_hash.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/gfx_animation.h"
#include "../../gfx/font_driver.h"
#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../command.h"
#include "../../runloop.h"

/* ======================================================================
 * CONFIGURATION
 * ====================================================================== */

/* Colors (RGBA float format, 0.0-1.0) */
static float cannoli_color_bg[16]        = { 0.0f, 0.0f, 0.0f, 1.0f,
                                              0.0f, 0.0f, 0.0f, 1.0f,
                                              0.0f, 0.0f, 0.0f, 1.0f,
                                              0.0f, 0.0f, 0.0f, 1.0f };
static float cannoli_color_selection[16] = { 1.0f, 1.0f, 1.0f, 1.0f,
                                              1.0f, 1.0f, 1.0f, 1.0f,
                                              1.0f, 1.0f, 1.0f, 1.0f,
                                              1.0f, 1.0f, 1.0f, 1.0f };
static float cannoli_color_accent[16]    = { 0.18f, 0.55f, 0.53f, 1.0f,
                                              0.18f, 0.55f, 0.53f, 1.0f,
                                              0.18f, 0.55f, 0.53f, 1.0f,
                                              0.18f, 0.55f, 0.53f, 1.0f };
static uint32_t cannoli_color_text       = 0xFFFFFFFF;  /* White (RGBA) */
static uint32_t cannoli_color_text_dark  = 0x000000FF;  /* Black (RGBA) */

/* Layout constants (will be scaled based on screen size) */
#define CANNOLI_BASE_FONT_SIZE   24
#define CANNOLI_MARGIN_RATIO     0.05f   /* 5% of screen width */
#define CANNOLI_LINE_HEIGHT      1.8f    /* Line height multiplier */
#define CANNOLI_PILL_PADDING_X   20
#define CANNOLI_PILL_PADDING_Y   6
#define CANNOLI_PILL_RADIUS      16
#define CANNOLI_PILL_STROKE      3       /* Outline stroke width */
#define CANNOLI_BUTTON_CIRCLE_SIZE 28    /* Button legend circle diameter */

/* Maximum items per menu */
#define CANNOLI_MAX_ITEMS        32
#define CANNOLI_MAX_MENUS        16

/* ======================================================================
 * CUSTOM MENU DEFINITIONS
 * Modify these to change menu organization
 * ====================================================================== */

typedef struct
{
   const char *label;           /* Display text */
   enum msg_hash_enums action;  /* RetroArch action (0 for submenu) */
   const char *submenu_id;      /* Submenu ID (NULL for actions) */
   bool requires_content;       /* Only show when content is loaded */
} cannoli_item_t;

typedef struct
{
   const char *id;              /* Menu identifier */
   const char *title;           /* Display title */
   cannoli_item_t items[CANNOLI_MAX_ITEMS];
} cannoli_menu_def_t;

/* Main Menu */
static const cannoli_menu_def_t cannoli_menus[] = {
   { "main", "RetroArch", {
      { "Games",       0, "games",    false },
      { "Settings",    0, "settings", false },
      { "Information", MENU_ENUM_LABEL_INFORMATION, NULL, false },
      { "Quit",        MENU_ENUM_LABEL_QUIT_RETROARCH,   NULL, false },
      { NULL }
   }},
   { "games", "Games", {
      { "Load Content",    MENU_ENUM_LABEL_LOAD_CONTENT_LIST,    NULL, false },
      { "Playlists",       MENU_ENUM_LABEL_PLAYLISTS_TAB,        NULL, false },
      { "History",         MENU_ENUM_LABEL_LOAD_CONTENT_HISTORY, NULL, false },
      { "Favorites",       MENU_ENUM_LABEL_GOTO_FAVORITES,       NULL, false },
      { NULL }
   }},
   { "settings", "Settings", {
      { "Video",     MENU_ENUM_LABEL_VIDEO_SETTINGS,     NULL, false },
      { "Audio",     MENU_ENUM_LABEL_AUDIO_SETTINGS,     NULL, false },
      { "Input",     MENU_ENUM_LABEL_INPUT_SETTINGS,     NULL, false },
      { "Core",      MENU_ENUM_LABEL_CORE_SETTINGS,      NULL, false },
      { "Saving",    MENU_ENUM_LABEL_SAVING_SETTINGS,    NULL, false },
      { "Directory", MENU_ENUM_LABEL_DIRECTORY_SETTINGS, NULL, false },
      { NULL }
   }},
   { NULL }
};

/* Quick Menu (shown when content is running) */
static const cannoli_menu_def_t cannoli_quick_menu = {
   "quick", "Quick Menu", {
      { "Resume",       MENU_ENUM_LABEL_RESUME_CONTENT,    NULL, true },
      { "Restart",      MENU_ENUM_LABEL_RESTART_CONTENT,   NULL, true },
      { "Save State",   MENU_ENUM_LABEL_SAVE_STATE,        NULL, true },
      { "Load State",   MENU_ENUM_LABEL_LOAD_STATE,        NULL, true },
      { "Options",      MENU_ENUM_LABEL_CORE_OPTIONS,      NULL, true },
      { "Close Content",MENU_ENUM_LABEL_CLOSE_CONTENT,     NULL, true },
      { NULL }
   }
};

/* ======================================================================
 * DRIVER STATE
 * ====================================================================== */

#define CANNOLI_MENU_STACK_SIZE 8

typedef struct
{
   const cannoli_menu_def_t *menu;
   size_t selection;
} cannoli_stack_entry_t;

typedef struct
{
   /* Font */
   font_data_impl_t font;
   font_data_impl_t font_small;
   float font_size;
   float font_size_small;

   /* Layout (recalculated on resize) */
   unsigned width;
   unsigned height;
   int margin_x;
   int margin_y;

   /* Navigation - custom menus */
   const cannoli_menu_def_t *current_menu;
   size_t selection;
   size_t scroll_offset;
   size_t visible_items;

   /* Menu stack for back navigation */
   cannoli_stack_entry_t menu_stack[CANNOLI_MENU_STACK_SIZE];
   size_t stack_depth;

   /* State flags */
   bool force_redraw;
   bool content_running;
   bool in_standard_menu;
   bool skip_render_frame;
   size_t standard_menu_stack_base;
   size_t saved_selection;
} cannoli_t;

/* ======================================================================
 * HELPER FUNCTIONS
 * ====================================================================== */

static const cannoli_menu_def_t *cannoli_find_menu(const char *id)
{
   size_t i;
   if (!id)
      return NULL;

   for (i = 0; cannoli_menus[i].id != NULL; i++)
   {
      if (string_is_equal(cannoli_menus[i].id, id))
         return &cannoli_menus[i];
   }
   return NULL;
}

static size_t cannoli_count_visible_items(const cannoli_menu_def_t *menu, bool content_running)
{
   size_t count = 0;
   size_t i;

   if (!menu)
      return 0;

   for (i = 0; menu->items[i].label != NULL; i++)
   {
      if (menu->items[i].requires_content && !content_running)
         continue;
      count++;
   }
   return count;
}

static const cannoli_item_t *cannoli_get_visible_item(
      const cannoli_menu_def_t *menu, size_t visible_idx, bool content_running)
{
   size_t count = 0;
   size_t i;

   if (!menu)
      return NULL;

   for (i = 0; menu->items[i].label != NULL; i++)
   {
      if (menu->items[i].requires_content && !content_running)
         continue;
      if (count == visible_idx)
         return &menu->items[i];
      count++;
   }
   return NULL;
}

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
static void cannoli_draw_rounded_pill(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      int x, int y, int width, int height,
      unsigned video_width, unsigned video_height,
      float *color);

static void cannoli_draw_bg(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   if (!p_disp || video_width == 0 || video_height == 0)
      return;

   gfx_display_draw_quad(
         p_disp,
         userdata,
         video_width, video_height,
         0, 0,
         video_width, video_height,
         video_width, video_height,
         cannoli_color_bg,
         NULL);
}

static void cannoli_draw_selection(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      int x, int y, int width, int height,
      unsigned video_width, unsigned video_height)
{
   /* Draw filled white rounded pill selection (MinUI style) */
   cannoli_draw_rounded_pill(cannoli, p_disp, userdata,
         x, y, width, height,
         video_width, video_height, cannoli_color_selection);
}

static void cannoli_draw_filled_circle(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      int cx, int cy, int radius,
      unsigned video_width, unsigned video_height,
      float *color)
{
   /* Draw a filled circle using small quads (approximation) */
   int x, y;
   for (y = -radius; y <= radius; y++)
   {
      for (x = -radius; x <= radius; x++)
      {
         if (x * x + y * y <= radius * radius)
         {
            gfx_display_draw_quad(p_disp, userdata, video_width, video_height,
                  cx + x, cy + y, 1, 1,
                  video_width, video_height, color, NULL);
         }
      }
   }
}

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

   /* Center rectangle */
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
   int circle_size = CANNOLI_BUTTON_CIRCLE_SIZE;
   int circle_radius = circle_size / 2;
   int label_width = cannoli_get_text_width(cannoli, label, true);
   int pill_padding = 6;
   int inner_padding = 6;
   int pill_width = pill_padding + circle_size + inner_padding + label_width + pill_padding;
   int pill_height = circle_size + pill_padding * 2;
   int pill_y = y - pill_padding;
   int text_baseline = pill_y + pill_height / 2 + cannoli->font_small.line_height / 4;

   /* Draw rounded teal pill background */
   cannoli_draw_rounded_pill(cannoli, p_disp, userdata,
         x, pill_y, pill_width, pill_height,
         video_width, video_height, cannoli_color_accent);

   /* Draw white circle inside pill */
   cannoli_draw_filled_circle(cannoli, p_disp, userdata,
         x + pill_padding + circle_radius, pill_y + pill_height / 2, circle_radius,
         video_width, video_height, cannoli_color_selection);

   /* Draw button letter centered in white circle (black text) */
   {
      int letter_width = cannoli_get_text_width(cannoli, button, true);
      int letter_x = x + pill_padding + circle_radius - letter_width / 2;
      cannoli_draw_text(cannoli, p_disp, video_width, video_height,
            letter_x, text_baseline, button, cannoli_color_text_dark, true);
   }

   /* Draw label text to the right of circle (black text) */
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

static int cannoli_get_text_width(cannoli_t *cannoli, const char *text, bool small_font)
{
   font_data_t *font = small_font ? cannoli->font_small.font : cannoli->font.font;
   if (font && text)
      return font_driver_get_message_width(font, text, strlen(text), 1.0f);
   return 0;
}

/* ======================================================================
 * RENDERING
 * ====================================================================== */

static void cannoli_render_custom_menu(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   const cannoli_menu_def_t *menu = cannoli->current_menu;
   size_t visible_count;
   size_t max_visible;
   size_t i;
   int y;
   int item_height;
   int button_legend_y;

   if (!menu || !cannoli->font.font || !p_disp)
      return;

   if (video_width == 0 || video_height == 0)
      return;

   visible_count = cannoli_count_visible_items(menu, cannoli->content_running);
   item_height = cannoli->font.line_height;
   max_visible = (video_height - cannoli->margin_y * 3 - cannoli->font.line_height - item_height) / item_height;
   if (max_visible > visible_count)
      max_visible = visible_count;

   /* Adjust scroll offset */
   if (cannoli->selection >= cannoli->scroll_offset + max_visible)
      cannoli->scroll_offset = cannoli->selection - max_visible + 1;
   if (cannoli->selection < cannoli->scroll_offset)
      cannoli->scroll_offset = cannoli->selection;

   /* Draw title */
   cannoli_draw_text(cannoli, p_disp, video_width, video_height,
         cannoli->margin_x, cannoli->margin_y + cannoli->font.line_height,
         menu->title, cannoli_color_text, false);

   /* Draw menu items */
   y = cannoli->margin_y + cannoli->font.line_height * 2;

   for (i = 0; i < max_visible && (i + cannoli->scroll_offset) < visible_count; i++)
   {
      const cannoli_item_t *item = cannoli_get_visible_item(menu,
            i + cannoli->scroll_offset, cannoli->content_running);
      bool is_selected = (i + cannoli->scroll_offset) == cannoli->selection;

      /* Calculate consistent text position for all items */
      int pill_height = (int)(cannoli->font_size * 1.3f);
      int pill_y = y + (item_height - pill_height) / 2;
      int text_y = pill_y + pill_height / 2 + (int)(cannoli->font_size * 0.35f);

      if (!item || !item->label)
         continue;

      if (is_selected)
      {
         /* Draw selection pill */
         int text_width = cannoli_get_text_width(cannoli, item->label, false);
         int pill_width = text_width + CANNOLI_PILL_PADDING_X * 2;

         cannoli_draw_selection(cannoli, p_disp, userdata,
               cannoli->margin_x - CANNOLI_PILL_PADDING_X, pill_y,
               pill_width, pill_height,
               video_width, video_height);

         /* Draw text centered in pill */
         cannoli_draw_text(cannoli, p_disp, video_width, video_height,
               cannoli->margin_x, text_y,
               item->label, cannoli_color_text_dark, false);
      }
      else
      {
         cannoli_draw_text(cannoli, p_disp, video_width, video_height,
               cannoli->margin_x, text_y,
               item->label, cannoli_color_text, false);
      }

      y += item_height;
   }

   /* Draw scroll indicators */
   if (cannoli->scroll_offset > 0)
   {
      cannoli_draw_text(cannoli, p_disp, video_width, video_height,
            video_width - cannoli->margin_x - 20, cannoli->margin_y + cannoli->font.line_height * 2,
            "^", cannoli_color_text, false);
   }

   if (cannoli->scroll_offset + max_visible < visible_count)
   {
      int arrow_y = y + cannoli->font.line_height;
      cannoli_draw_text(cannoli, p_disp, video_width, video_height,
            video_width - cannoli->margin_x - 20, arrow_y,
            "v", cannoli_color_text, false);
   }

   /* Draw button legends at bottom */
   button_legend_y = video_height - cannoli->margin_y - CANNOLI_BUTTON_CIRCLE_SIZE;

   /* Back button (if not at root) */
   if (cannoli->stack_depth > 0)
   {
      cannoli_draw_button_legend(cannoli, p_disp, userdata,
            cannoli->margin_x, button_legend_y,
            "B", "Back",
            video_width, video_height);
   }

   /* Select button */
   {
      int pill_padding = 6;
      int inner_padding = 6;
      int label_width = cannoli_get_text_width(cannoli, "Select", true);
      int pill_width = pill_padding + CANNOLI_BUTTON_CIRCLE_SIZE + inner_padding + label_width + pill_padding;
      int legend_x = video_width - cannoli->margin_x - pill_width;

      cannoli_draw_button_legend(cannoli, p_disp, userdata,
            legend_x, button_legend_y,
            "A", "Select",
            video_width, video_height);
   }
}

static void cannoli_render_standard_menu(cannoli_t *cannoli,
      gfx_display_t *p_disp, void *userdata,
      unsigned video_width, unsigned video_height)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *list;
   size_t list_size;
   size_t selection;
   size_t max_visible;
   size_t i;
   size_t start_idx;
   int y;
   int item_height;
   int button_legend_y;
   char title_buf[256];

   if (!cannoli->font.font || !p_disp)
      return;

   if (video_width == 0 || video_height == 0)
      return;

   if (!menu_st)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   list = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!list)
      return;

   list_size = MENU_LIST_GET_SELECTION(menu_list, 0)->size;
   if (list_size == 0)
      return;  /* No entries yet, skip rendering */

   selection = menu_st->selection_ptr;
   item_height = cannoli->font.line_height;
   if (item_height <= 0)
      item_height = 20;  /* Safety default */

   max_visible = (video_height - cannoli->margin_y * 3 - cannoli->font.line_height - item_height) / item_height;
   if (max_visible == 0)
      max_visible = 1;

   /* Get title from menu stack */
   title_buf[0] = '\0';
   {
      menu_entries_get_title(title_buf, sizeof(title_buf));
   }

   /* Draw title */
   cannoli_draw_text(cannoli, p_disp, video_width, video_height,
         cannoli->margin_x, cannoli->margin_y + cannoli->font.line_height,
         title_buf, cannoli_color_text, false);

   /* Calculate start index for scrolling */
   if (selection >= max_visible)
      start_idx = selection - max_visible + 1;
   else
      start_idx = 0;

   /* Draw menu entries */
   y = cannoli->margin_y + cannoli->font.line_height * 2;
   button_legend_y = video_height - cannoli->margin_y;

   for (i = 0; i < max_visible && (start_idx + i) < list_size; i++)
   {
      menu_entry_t entry;
      const char *entry_label;
      bool is_selected = ((start_idx + i) == selection);

      /* Calculate consistent text position for all items */
      int pill_height = (int)(cannoli->font_size * 1.3f);
      int pill_y = y + (item_height - pill_height) / 2;
      int text_y = pill_y + pill_height / 2 + (int)(cannoli->font_size * 0.35f);

      MENU_ENTRY_INITIALIZE(entry);
      entry.flags |= MENU_ENTRY_FLAG_RICH_LABEL_ENABLED
                   | MENU_ENTRY_FLAG_VALUE_ENABLED;
      menu_entry_get(&entry, 0, (unsigned)(start_idx + i), NULL, true);

      /* Get label to display */
      if (!string_is_empty(entry.rich_label))
         entry_label = entry.rich_label;
      else
         entry_label = entry.path;

      /* Skip if no label */
      if (string_is_empty(entry_label))
      {
         y += item_height;
         continue;
      }

      if (is_selected)
      {
         /* Draw selection pill */
         int text_width = cannoli_get_text_width(cannoli, entry_label, false);
         int value_width = 0;
         int pill_width;

         if (!string_is_empty(entry.value))
            value_width = cannoli_get_text_width(cannoli, entry.value, false) + CANNOLI_PILL_PADDING_X;

         pill_width = text_width + value_width + CANNOLI_PILL_PADDING_X * 2;

         cannoli_draw_selection(cannoli, p_disp, userdata,
               cannoli->margin_x - CANNOLI_PILL_PADDING_X, pill_y,
               pill_width, pill_height,
               video_width, video_height);

         /* Draw text centered in pill */
         cannoli_draw_text(cannoli, p_disp, video_width, video_height,
               cannoli->margin_x, text_y,
               entry_label, cannoli_color_text_dark, false);

         /* Draw value if present */
         if (!string_is_empty(entry.value))
         {
            int value_x = cannoli->margin_x + text_width + CANNOLI_PILL_PADDING_X;
            cannoli_draw_text(cannoli, p_disp, video_width, video_height,
                  value_x, text_y,
                  entry.value, cannoli_color_text_dark, false);
         }
      }
      else
      {
         /* Draw normal text */
         cannoli_draw_text(cannoli, p_disp, video_width, video_height,
               cannoli->margin_x, text_y,
               entry_label, cannoli_color_text, false);

         /* Draw value if present */
         if (!string_is_empty(entry.value))
         {
            int value_x = video_width - cannoli->margin_x -
                          cannoli_get_text_width(cannoli, entry.value, false);
            cannoli_draw_text(cannoli, p_disp, video_width, video_height,
                  value_x, text_y,
                  entry.value, cannoli_color_text, false);
         }
      }

      y += item_height;
   }

   /* Draw scroll indicators */
   if (start_idx > 0)
   {
      cannoli_draw_text(cannoli, p_disp, video_width, video_height,
            video_width - cannoli->margin_x - 20, cannoli->margin_y + cannoli->font.line_height * 2,
            "^", cannoli_color_text, false);
   }

   if (start_idx + max_visible < list_size)
   {
      int arrow_y = y + cannoli->font.line_height;
      cannoli_draw_text(cannoli, p_disp, video_width, video_height,
            video_width - cannoli->margin_x - 20, arrow_y,
            "v", cannoli_color_text, false);
   }

   /* Draw button legends */
   button_legend_y = video_height - cannoli->margin_y - CANNOLI_BUTTON_CIRCLE_SIZE;

   /* Back button */
   cannoli_draw_button_legend(cannoli, p_disp, userdata,
         cannoli->margin_x, button_legend_y,
         "B", "Back",
         video_width, video_height);

   /* Select button */
   {
      int pill_padding = 6;
      int inner_padding = 6;
      int label_width = cannoli_get_text_width(cannoli, "Select", true);
      int pill_width = pill_padding + CANNOLI_BUTTON_CIRCLE_SIZE + inner_padding + label_width + pill_padding;
      int legend_x = video_width - cannoli->margin_x - pill_width;

      cannoli_draw_button_legend(cannoli, p_disp, userdata,
            legend_x, button_legend_y,
            "A", "Select",
            video_width, video_height);
   }
}

/* ======================================================================
 * ACTION HANDLING
 * ====================================================================== */

/* Forward declaration */
static void cannoli_populate_custom_menu(cannoli_t *cannoli);

static int cannoli_handle_custom_action(cannoli_t *cannoli, enum menu_action action)
{
   const cannoli_menu_def_t *menu = cannoli->current_menu;
   size_t visible_count;

   if (!menu)
      return 0;

   visible_count = cannoli_count_visible_items(menu, cannoli->content_running);

   switch (action)
   {
      case MENU_ACTION_UP:
         if (cannoli->selection > 0)
            cannoli->selection--;
         else
            cannoli->selection = visible_count - 1;
         /* Sync with menu system */
         {
            struct menu_state *menu_st = menu_state_get_ptr();
            menu_st->selection_ptr = cannoli->selection;
         }
         break;

      case MENU_ACTION_DOWN:
         if (cannoli->selection < visible_count - 1)
            cannoli->selection++;
         else
            cannoli->selection = 0;
         /* Sync with menu system */
         {
            struct menu_state *menu_st = menu_state_get_ptr();
            menu_st->selection_ptr = cannoli->selection;
         }
         break;

      case MENU_ACTION_OK:
         {
            const cannoli_item_t *item = cannoli_get_visible_item(menu,
                  cannoli->selection, cannoli->content_running);

            if (item)
            {
               if (item->submenu_id)
               {
                  /* Navigate to custom submenu */
                  const cannoli_menu_def_t *submenu = cannoli_find_menu(item->submenu_id);
                  if (submenu)
                  {
                     /* Push current menu to stack */
                     if (cannoli->stack_depth < CANNOLI_MENU_STACK_SIZE)
                     {
                        cannoli->menu_stack[cannoli->stack_depth].menu = cannoli->current_menu;
                        cannoli->menu_stack[cannoli->stack_depth].selection = cannoli->selection;
                        cannoli->stack_depth++;
                     }
                     cannoli->current_menu = submenu;
                     cannoli->selection = 0;
                     cannoli->scroll_offset = 0;
                     /* Repopulate entries for new menu */
                     cannoli_populate_custom_menu(cannoli);
                  }
               }
               else if (item->action)
               {
                  /* Handle quit specially */
                  if (item->action == MENU_ENUM_LABEL_QUIT_RETROARCH)
                  {
                     runloop_state_t *runloop_st = runloop_state_get_ptr();
                     runloop_st->flags |= RUNLOOP_FLAG_SHUTDOWN_INITIATED;
                  }
                  /* Handle information specially - push displaylist directly */
                  else if (item->action == MENU_ENUM_LABEL_INFORMATION)
                  {
                     struct menu_state *menu_st = menu_state_get_ptr();
                     menu_list_t *menu_list = menu_st->entries.list;
                     settings_t *settings = config_get_ptr();
                     menu_displaylist_info_t info;

                     /* Mark that we're entering standard menu mode */
                     cannoli->saved_selection = cannoli->selection;
                     cannoli->in_standard_menu = true;
                     cannoli->standard_menu_stack_base = MENU_LIST_GET_STACK_SIZE(menu_list, 0);

                     /* Push information list displaylist */
                     menu_displaylist_info_init(&info);
                     info.list = MENU_LIST_GET_SELECTION(menu_list, 0);
                     info.type = MENU_SETTING_ACTION;
                     info.enum_idx = MENU_ENUM_LABEL_INFORMATION_LIST;
                     strlcpy(info.label, msg_hash_to_str(MENU_ENUM_LABEL_INFORMATION_LIST), sizeof(info.label));
                     if (menu_displaylist_ctl(DISPLAYLIST_INFORMATION_LIST, &info, settings))
                        menu_displaylist_process(&info);
                     menu_displaylist_info_free(&info);
                  }
                  else
                  {
                     /* Use the standard menu entry callback system */
                     struct menu_state *menu_st = menu_state_get_ptr();
                     menu_list_t *menu_list = menu_st->entries.list;
                     file_list_t *selection_buf = MENU_LIST_GET_SELECTION(menu_list, 0);
                     menu_file_list_cbs_t *cbs = NULL;
                     menu_entry_t entry;

                     if (selection_buf && cannoli->selection < selection_buf->size)
                        cbs = (menu_file_list_cbs_t*)selection_buf->list[cannoli->selection].actiondata;

                     /* Mark that we're entering standard menu mode */
                     cannoli->saved_selection = cannoli->selection;
                     cannoli->in_standard_menu = true;
                     cannoli->standard_menu_stack_base = MENU_LIST_GET_STACK_SIZE(menu_list, 0);

                     /* Get entry info */
                     MENU_ENTRY_INITIALIZE(entry);
                     menu_entry_get(&entry, 0, (unsigned)cannoli->selection, NULL, true);

                     /* Directly call the action_ok callback to avoid recursion */
                     if (cbs && cbs->action_ok)
                        cbs->action_ok(entry.path, entry.label, entry.type,
                                       cannoli->selection, entry.entry_idx);
                  }
               }
            }
         }
         break;

      case MENU_ACTION_CANCEL:
         if (cannoli->stack_depth > 0)
         {
            /* Pop from stack */
            cannoli->stack_depth--;
            cannoli->current_menu = cannoli->menu_stack[cannoli->stack_depth].menu;
            cannoli->selection = cannoli->menu_stack[cannoli->stack_depth].selection;
            cannoli->scroll_offset = 0;
            /* Repopulate entries for restored menu */
            cannoli_populate_custom_menu(cannoli);
         }
         break;

      default:
         break;
   }

   return 0;
}

static int cannoli_handle_standard_action(cannoli_t *cannoli, enum menu_action action)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list = menu_st->entries.list;
   size_t stack_size = MENU_LIST_GET_STACK_SIZE(menu_list, 0);
   menu_entry_t entry;

   if (action == MENU_ACTION_CANCEL)
   {
      if (stack_size <= cannoli->standard_menu_stack_base + 1)
      {
         /* Return to custom menu */
         cannoli->in_standard_menu = false;
         cannoli->selection = cannoli->saved_selection;
         /* Repopulate custom menu entries */
         cannoli_populate_custom_menu(cannoli);
         return 0;
      }
   }

   /* Use generic handler to avoid recursion through our entry_action */
   MENU_ENTRY_INITIALIZE(entry);
   menu_entry_get(&entry, 0, (unsigned)menu_st->selection_ptr, NULL, true);
   return generic_menu_entry_action(cannoli, &entry, menu_st->selection_ptr, action);
}

/* ======================================================================
 * MENU DRIVER INTERFACE
 * ====================================================================== */

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

   *userdata = cannoli;  /* This is what gets passed to frame() */

   /* Start at main menu */
   cannoli->current_menu = &cannoli_menus[0];
   cannoli->selection = 0;
   cannoli->stack_depth = 0;
   cannoli->in_standard_menu = false;

   /* Fonts will be initialized in context_reset */

   p_disp->framebuf_width = 0;
   p_disp->framebuf_height = 0;

   return menu;
}

static void cannoli_free(void *data)
{
   cannoli_t *cannoli = (cannoli_t*)data;

   if (cannoli)
   {
      /* Fonts are freed by the font system */
      cannoli->font.font = NULL;
      cannoli->font_small.font = NULL;
      /* Don't free cannoli here - the menu system does it */
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

   /* Calculate scale based on screen height */
   if (p_disp->framebuf_height > 0)
      scale_factor = (float)p_disp->framebuf_height / 480.0f;
   else
      scale_factor = 1.0f;

   if (scale_factor < 1.0f)
      scale_factor = 1.0f;

   cannoli->font_size = CANNOLI_BASE_FONT_SIZE * scale_factor;
   cannoli->font_size_small = CANNOLI_BASE_FONT_SIZE * scale_factor * 0.75f;

   /* Free existing fonts */
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

   /* Load fonts - try multiple paths */
   fontpath[0] = '\0';

   /* Try XMB monochrome font first */
   if (settings->paths.directory_assets[0] != '\0')
   {
      fill_pathname_join_special(fontpath, settings->paths.directory_assets,
            "xmb/monochrome/font.ttf", sizeof(fontpath));
      cannoli->font.font = gfx_display_font_file(p_disp, fontpath, cannoli->font_size, is_threaded);
   }

   /* Try ozone font as fallback */
   if (!cannoli->font.font && settings->paths.directory_assets[0] != '\0')
   {
      fill_pathname_join_special(fontpath, settings->paths.directory_assets,
            "ozone/regular.ttf", sizeof(fontpath));
      cannoli->font.font = gfx_display_font_file(p_disp, fontpath, cannoli->font_size, is_threaded);
   }

   /* Load small font if main font loaded */
   if (cannoli->font.font && fontpath[0] != '\0')
      cannoli->font_small.font = gfx_display_font_file(p_disp, fontpath, cannoli->font_size_small, is_threaded);

   /* Initialize font_data_impl_t fields */
   cannoli->font.line_height = (int)(cannoli->font_size * CANNOLI_LINE_HEIGHT);
   cannoli->font.glyph_width = (int)(cannoli->font_size * 0.6f);
   cannoli->font_small.line_height = (int)(cannoli->font_size_small * CANNOLI_LINE_HEIGHT);
   cannoli->font_small.glyph_width = (int)(cannoli->font_size_small * 0.6f);

   if (cannoli->font.line_height < 20)
      cannoli->font.line_height = 20;
   if (cannoli->font_small.line_height < 15)
      cannoli->font_small.line_height = 15;

   /* Initialize white texture for quads */
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
   }

   gfx_display_deinit_white_texture();
}

static void cannoli_render(void *data, unsigned width, unsigned height, bool is_idle)
{
   cannoli_t *cannoli = (cannoli_t*)data;
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   struct menu_state *menu_st;
   menu_list_t *menu_list;
   file_list_t *selection_buf;

   if (!cannoli)
      return;

   /* Update state */
   cannoli->content_running = (runloop_st->flags & RUNLOOP_FLAG_CORE_RUNNING) ? true : false;

   /* Ensure entries are populated for custom menu */
   if (!cannoli->in_standard_menu)
   {
      menu_st = menu_state_get_ptr();
      if (menu_st)
      {
         menu_list = menu_st->entries.list;
         if (menu_list)
         {
            selection_buf = MENU_LIST_GET_SELECTION(menu_list, 0);
            if (selection_buf && selection_buf->size == 0)
               cannoli_populate_custom_menu(cannoli);
         }
      }
   }

   /* Update layout on resize */
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
   void *userdata = video_info ? video_info->userdata : NULL;
   unsigned video_width, video_height;

   if (!cannoli || !p_disp || !video_info)
      return;

   video_width = video_info->width;
   video_height = video_info->height;

   /* Don't render if dimensions are invalid */
   if (video_width == 0 || video_height == 0)
      return;

   /* Skip frame after menu transition */
   if (cannoli->skip_render_frame)
   {
      cannoli->skip_render_frame = false;
      return;
   }

   /* Make sure fonts are loaded */
   if (!cannoli->font.font)
      return;

   /* Update layout if needed */
   if (cannoli->margin_x == 0 || cannoli->margin_y == 0)
   {
      cannoli->margin_x = (int)(video_width * CANNOLI_MARGIN_RATIO);
      cannoli->margin_y = (int)(video_height * CANNOLI_MARGIN_RATIO);
      if (cannoli->margin_x < 10) cannoli->margin_x = 10;
      if (cannoli->margin_y < 10) cannoli->margin_y = 10;
   }

   /* Bind fonts for rendering */
   font_bind(&cannoli->font);
   if (cannoli->font_small.font)
      font_bind(&cannoli->font_small);

   /* Draw background */
   cannoli_draw_bg(cannoli, p_disp, userdata, video_width, video_height);

   /* Draw appropriate menu */
   if (cannoli->in_standard_menu)
      cannoli_render_standard_menu(cannoli, p_disp, userdata, video_width, video_height);
   else
      cannoli_render_custom_menu(cannoli, p_disp, userdata, video_width, video_height);

   /* Flush text */
   if (cannoli->font.font)
      font_flush(video_width, video_height, &cannoli->font);
   if (cannoli->font_small.font)
      font_flush(video_width, video_height, &cannoli->font_small);
}

static void cannoli_navigation_set(void *data, bool scroll)
{
   cannoli_t *cannoli = (cannoli_t*)data;

   if (!cannoli)
      return;

   if (!cannoli->in_standard_menu)
   {
      struct menu_state *menu_st = menu_state_get_ptr();
      cannoli->selection = menu_st->selection_ptr;
   }
}

static void cannoli_navigation_clear(void *data, bool pending_push)
{
   cannoli_t *cannoli = (cannoli_t*)data;

   if (!cannoli)
      return;

   if (!cannoli->in_standard_menu)
   {
      cannoli->selection = 0;
      cannoli->scroll_offset = 0;
   }
}

static void cannoli_navigation_set_last(void *data)
{
   cannoli_t *cannoli = (cannoli_t*)data;

   if (!cannoli)
      return;

   if (!cannoli->in_standard_menu)
   {
      size_t visible_count = cannoli_count_visible_items(cannoli->current_menu, cannoli->content_running);
      if (visible_count > 0)
         cannoli->selection = visible_count - 1;
   }
}

static void cannoli_populate_custom_menu(cannoli_t *cannoli)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_list_t *menu_list;
   file_list_t *selection_buf;
   const cannoli_menu_def_t *menu;
   size_t i;

   if (!cannoli || !menu_st)
      return;

   menu = cannoli->current_menu;
   if (!menu)
      return;

   menu_list = menu_st->entries.list;
   if (!menu_list)
      return;

   selection_buf = MENU_LIST_GET_SELECTION(menu_list, 0);
   if (!selection_buf)
      return;

   /* Clear existing entries */
   menu_entries_clear(selection_buf);

   /* Add entries from our custom menu */
   for (i = 0; menu->items[i].label != NULL; i++)
   {
      const cannoli_item_t *item = &menu->items[i];
      const char *entry_label;
      enum msg_hash_enums enum_idx;

      /* Skip items that require content when none is running */
      if (item->requires_content && !cannoli->content_running)
         continue;

      /* Use proper label for items that trigger standard menus */
      if (item->action != 0 && !item->submenu_id)
      {
         entry_label = msg_hash_to_str(item->action);
         enum_idx = item->action;
      }
      else
      {
         entry_label = item->label;
         enum_idx = MSG_UNKNOWN;
      }

      menu_entries_append(selection_buf,
            item->label,      /* path (display text) */
            entry_label,      /* label (for callback lookup) */
            enum_idx,         /* enum_idx */
            MENU_SETTING_ACTION, /* type */
            0,                /* directory_ptr */
            0,                /* entry_idx */
            NULL);            /* setting */
   }

   /* Set the selection pointer */
   menu_st->selection_ptr = cannoli->selection;
}

static void cannoli_populate_entries(void *data,
      const char *path, const char *label, unsigned k)
{
   cannoli_t *cannoli = (cannoli_t*)data;

   if (!cannoli)
      return;

   /* Only populate custom menu entries when not in standard menu mode */
   if (!cannoli->in_standard_menu)
      cannoli_populate_custom_menu(cannoli);
}

static int cannoli_entry_action(void *userdata, menu_entry_t *entry,
      size_t i, enum menu_action action)
{
   cannoli_t *cannoli = (cannoli_t*)userdata;

   if (!cannoli)
      return -1;

   if (cannoli->in_standard_menu)
      return cannoli_handle_standard_action(cannoli, action);
   else
      return cannoli_handle_custom_action(cannoli, action);
}

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
   switch (type)
   {
      case MENU_ENVIRON_RESET_HORIZONTAL_LIST:
      case MENU_ENVIRON_ENABLE_MOUSE_CURSOR:
      case MENU_ENVIRON_DISABLE_MOUSE_CURSOR:
      default:
         break;
   }
   return -1;
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
