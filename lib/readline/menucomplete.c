/* menucomplete.c -- visual completion menu for readline.

   Renders a navigable grid of completion matches below the prompt.
   Modal: arrow keys navigate, Enter/Tab accept, Escape dismisses.
   Modelled after PowerShell's MenuComplete (Ctrl+Space). */

/* Copyright (C) 2025 Free Software Foundation, Inc.

   This file is part of the GNU Readline Library (Readline), a library
   for reading lines of text with interactive input and history editing.

   Readline is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Readline is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Readline.  If not, see <http://www.gnu.org/licenses/>.
*/

#define READLINE_LIBRARY

#if defined (HAVE_CONFIG_H)
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined (HAVE_UNISTD_H)
#  include <unistd.h>
#endif

#include "rldefs.h"
#include "rlmbutil.h"
#include "readline.h"
#include "rlprivate.h"
#include "xmalloc.h"
#include "suggest.h"
#include "menucomplete.h"

/* ------------------------------------------------------------------ */
/* ANSI escape helpers                                                 */
/* ------------------------------------------------------------------ */

/* Selected item: bold bright white. */
#define MC_SELECTED_ON    "\033[1;37m"
#define MC_SELECTED_OFF   "\033[0m"

/* ------------------------------------------------------------------ */
/* Menu state                                                          */
/* ------------------------------------------------------------------ */

/* Saved state from the display hook so the modal loop can use it. */
static char **_mc_matches;	/* matches[0]=LCD, matches[1..len]=items */
static int    _mc_len;		/* number of actual matches (excl LCD) */
static int    _mc_max;		/* max printable width of any match */

static int    _mc_selected;	/* 0-based index into matches[1..len] */
static int    _mc_columns;	/* number of columns in the grid */
static int    _mc_rows;		/* total rows in the grid */
static int    _mc_visible;	/* rows visible at a time */
static int    _mc_scroll;	/* first visible row (0-based) */
static int    _mc_col_width;	/* column width (max + padding) */
static int    _mc_active;	/* non-zero while menu is displayed */

/* Saved line state for restoring on dismiss. */
static char  *_mc_saved_text;
static int    _mc_saved_point;

/* The completion word boundaries (set by readline before the hook). */
static int    _mc_word_start;
static int    _mc_word_end;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* Get the printable part of a match (basename for filenames). */
static const char *
mc_printable (const char *match)
{
  const char *p;

  if (rl_filename_completion_desired == 0)
    return match;
  p = strrchr (match, '/');
  if (p && p[1])
    return p + 1;
  return match;
}

/* Compute display width of a string, handling multibyte. */
static int
mc_strwidth (const char *s)
{
#if defined (HANDLE_MULTIBYTE)
  if (MB_CUR_MAX > 1 && rl_byte_oriented == 0)
    {
      int w = 0;
      mbstate_t ps;
      size_t bytes;
      WCHAR_T wc;
      int cw;

      memset (&ps, 0, sizeof (ps));
      while (*s)
	{
	  bytes = MBRTOWC (&wc, s, MB_CUR_MAX, &ps);
	  if (MB_INVALIDCH (bytes) || MB_NULLWCH (bytes))
	    break;
	  cw = WCWIDTH (wc);
	  w += (cw > 0) ? cw : 1;
	  s += bytes;
	}
      return w;
    }
#endif
  return (int)strlen (s);
}

/* Draw one row of the menu grid starting at grid row `row`.
   Clears to end of line after the last column. */
static void
mc_draw_row (int row)
{
  int col, idx;
  const char *text;
  int tw, pad;

  fprintf (rl_outstream, "\r\033[K");

  for (col = 0; col < _mc_columns; col++)
    {
      idx = row * _mc_columns + col;
      if (idx >= _mc_len)
	break;

      text = mc_printable (_mc_matches[idx + 1]); /* +1: skip LCD */
      tw = mc_strwidth (text);
      pad = _mc_col_width - tw;
      if (pad < 0)
	pad = 0;

      if (idx == _mc_selected)
	fprintf (rl_outstream, MC_SELECTED_ON "%s" MC_SELECTED_OFF, text);
      else
	{
	  /* Unselected items use the inline suggestion/ghost text color. */
	  _rl_suggestion_color_on ();
	  fprintf (rl_outstream, "%s", text);
	  _rl_suggestion_color_off ();
	}

      /* Column padding */
      if (col < _mc_columns - 1 && pad > 0)
	fprintf (rl_outstream, "%*s", pad, "");
    }
}

/* Draw a status bar showing match count and position. */
static void
mc_draw_status (void)
{
  fprintf (rl_outstream, "\r\033[K");
  _rl_suggestion_color_on ();
  fprintf (rl_outstream, "(%d/%d)", _mc_selected + 1, _mc_len);
  _rl_suggestion_color_off ();
}

/* Adjust _mc_scroll so the selected row is visible. */
static void
mc_adjust_scroll (void)
{
  int sel_row = _mc_selected / _mc_columns;

  if (sel_row < _mc_scroll)
    _mc_scroll = sel_row;
  else if (sel_row >= _mc_scroll + _mc_visible)
    _mc_scroll = sel_row - _mc_visible + 1;
}

/* Render the full menu below the prompt. */
static void
mc_render (void)
{
  int r, draw_rows;

  mc_adjust_scroll ();

  /* Move to the line below the prompt. */
  _rl_move_vert (_rl_vis_botlin);

  draw_rows = _mc_rows - _mc_scroll;
  if (draw_rows > _mc_visible)
    draw_rows = _mc_visible;

  for (r = 0; r < draw_rows; r++)
    {
      rl_crlf ();
      mc_draw_row (_mc_scroll + r);
    }

  /* Status bar */
  rl_crlf ();
  mc_draw_status ();

  /* Clear any leftover lines below from previous render. */
  {
    int total_drawn = draw_rows + 1; /* rows + status */
    int avail = _rl_screenheight - _rl_vis_botlin - 1;
    int extra = avail - total_drawn;
    int i;

    for (i = 0; i < extra && i < 5; i++)
      {
	rl_crlf ();
	fprintf (rl_outstream, "\r\033[K");
      }
  }

  /* Move cursor back up to the prompt line. */
  {
    int lines_below = draw_rows + 1;
    int extra_cleared = 0;
    int avail = _rl_screenheight - _rl_vis_botlin - 1;
    int extra = avail - (draw_rows + 1);
    if (extra > 5)
      extra = 5;
    if (extra > 0)
      extra_cleared = extra;

    if (lines_below + extra_cleared > 0)
      fprintf (rl_outstream, "\033[%dA", lines_below + extra_cleared);
  }

  /* Restore cursor to the prompt position. */
  _rl_last_v_pos = _rl_vis_botlin;
  fprintf (rl_outstream, "\r");
  _rl_last_c_pos = 0;
  rl_redisplay ();
  fflush (rl_outstream);
}

/* Erase the menu area and restore the display. */
static void
mc_erase (void)
{
  int r, total;

  _rl_move_vert (_rl_vis_botlin);

  total = _mc_visible + 1; /* grid rows + status bar */
  for (r = 0; r < total + 5; r++) /* +5 for any extras we cleared */
    {
      rl_crlf ();
      fprintf (rl_outstream, "\r\033[K");
    }

  /* Move back up. */
  if (total + 5 > 0)
    fprintf (rl_outstream, "\033[%dA", total + 5);

  _rl_last_v_pos = _rl_vis_botlin;
  fprintf (rl_outstream, "\r");
  _rl_last_c_pos = 0;

  fflush (rl_outstream);
}

/* Replace the completion word with the selected match. */
static void
mc_preview_selection (void)
{
  const char *match;
  int new_end;

  if (_mc_selected < 0 || _mc_selected >= _mc_len)
    return;

  match = _mc_matches[_mc_selected + 1]; /* +1: skip LCD */

  /* Delete old completion word and insert the selected match. */
  rl_delete_text (_mc_word_start, rl_point);
  rl_point = _mc_word_start;
  rl_insert_text (match);

  /* If it's a directory, append a slash. */
  if (rl_filename_completion_desired)
    {
      new_end = rl_point;
      if (new_end > 0 && rl_line_buffer[new_end - 1] != '/')
	{
	  struct stat sb;
	  if (stat (match, &sb) == 0 && S_ISDIR (sb.st_mode))
	    rl_insert_text ("/");
	}
    }
}

/* Restore the original text that was present before any preview. */
static void
mc_restore_original (void)
{
  if (_mc_saved_text)
    {
      rl_delete_text (0, rl_end);
      rl_point = 0;
      rl_insert_text (_mc_saved_text);
      rl_point = _mc_saved_point;
    }
}

/* Read escape sequences for arrow keys.  Returns:
   'A' = up, 'B' = down, 'C' = right, 'D' = left,
   'Z' = shift-tab (CSI Z), or 0 if not recognized. */
static int
mc_read_escape_seq (void)
{
  int c;

  c = rl_read_key ();
  if (c == '[' || c == 'O')
    {
      c = rl_read_key ();
      switch (c)
	{
	case 'A': return 'A'; /* up */
	case 'B': return 'B'; /* down */
	case 'C': return 'C'; /* right */
	case 'D': return 'D'; /* left */
	case 'Z': return 'Z'; /* shift-tab */
	}
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Modal key loop                                                      */
/* ------------------------------------------------------------------ */

/* Run the modal menu loop.  Returns when the user accepts or dismisses. */
static void
mc_modal_loop (void)
{
  int c, seq, done;

  done = 0;
  while (!done)
    {
      mc_render ();

      c = rl_read_key ();

      switch (c)
	{
	case '\033': /* Escape — could be dismiss or arrow key */
	  seq = mc_read_escape_seq ();
	  switch (seq)
	    {
	    case 'A': /* Up */
	      if (_mc_selected >= _mc_columns)
		_mc_selected -= _mc_columns;
	      mc_preview_selection ();
	      break;
	    case 'B': /* Down */
	      if (_mc_selected + _mc_columns < _mc_len)
		_mc_selected += _mc_columns;
	      mc_preview_selection ();
	      break;
	    case 'C': /* Right */
	      if (_mc_selected < _mc_len - 1)
		_mc_selected++;
	      mc_preview_selection ();
	      break;
	    case 'D': /* Left */
	      if (_mc_selected > 0)
		_mc_selected--;
	      mc_preview_selection ();
	      break;
	    case 'Z': /* Shift-Tab: previous */
	      _mc_selected--;
	      if (_mc_selected < 0)
		_mc_selected = _mc_len - 1;
	      mc_preview_selection ();
	      break;
	    case 0: /* bare Escape — dismiss */
	      mc_erase ();
	      mc_restore_original ();
	      done = 1;
	      break;
	    }
	  break;

	case '\t': /* Tab: next match (wrapping) */
	  _mc_selected++;
	  if (_mc_selected >= _mc_len)
	    _mc_selected = 0;
	  mc_preview_selection ();
	  break;

	case '\r': /* Enter: accept current selection */
	case '\n':
	  mc_erase ();
	  /* Selection is already in the line buffer from preview. */
	  done = 1;
	  break;

	case CTRL ('C'): /* Ctrl-C: dismiss */
	case CTRL ('G'): /* Ctrl-G: dismiss */
	  mc_erase ();
	  mc_restore_original ();
	  done = 1;
	  break;

	default:
	  /* Any other key: dismiss menu and replay the key. */
	  mc_erase ();
	  mc_restore_original ();
	  rl_stuff_char (c);
	  done = 1;
	  break;
	}
    }
}

/* ------------------------------------------------------------------ */
/* Display hook (public API)                                           */
/* ------------------------------------------------------------------ */

/* Called by readline's display_matches() via rl_completion_display_matches_hook.
   matches[0] = LCD (longest common denominator), matches[1..len] = items.
   len = number of actual matches, max = widest printable width. */
void
_rl_menu_complete_display_hook (char **matches, int len, int max)
{
  int avail_rows, padding;

  if (len <= 0)
    return;

  /* Gate: fall through to default display when feature is disabled. */
  if (_rl_enable_menu_complete_list == 0)
    {
      rl_display_match_list (matches, len, max);
      rl_forced_update_display ();
      rl_display_fixed = 1;
      return;
    }

  /* Clear any active inline suggestion — the menu replaces it. */
  if (_rl_suggestion_get_len () > 0)
    _rl_suggestion_clear ();

  /* Store match data. */
  _mc_matches = matches;
  _mc_len = len;
  _mc_max = max;
  _mc_selected = 0;

  /* Layout: compute grid dimensions. */
  padding = 2;
  _mc_col_width = max + padding;
  if (_mc_col_width < 1)
    _mc_col_width = 1;

  _mc_columns = _rl_screenwidth / _mc_col_width;
  if (_mc_columns < 1)
    _mc_columns = 1;

  _mc_rows = (len + _mc_columns - 1) / _mc_columns;

  /* Visible rows: leave space for prompt + status bar.
     Reserve at least 2 lines for prompt area. */
  avail_rows = _rl_screenheight - _rl_vis_botlin - 3; /* -1 prompt, -1 status, -1 margin */
  if (avail_rows < 1)
    avail_rows = 1;
  if (avail_rows > _mc_rows)
    avail_rows = _mc_rows;

  _mc_visible = avail_rows;
  _mc_scroll = 0;

  /* Save current line state for restore on dismiss. */
  if (_mc_saved_text)
    {
      xfree (_mc_saved_text);
      _mc_saved_text = (char *)NULL;
    }
  _mc_saved_text = savestring (rl_line_buffer);
  _mc_saved_point = rl_point;

  /* Determine the completion word boundaries.
     rl_point is at the end of the word being completed. */
  _mc_word_end = rl_point;
  /* Walk back to find the start of the completion word. */
  {
    int i = rl_point - 1;
    while (i >= 0 && rl_line_buffer[i] != ' ' && rl_line_buffer[i] != '\t'
	   && rl_line_buffer[i] != '/' && rl_line_buffer[i] != '=')
      i--;
    _mc_word_start = i + 1;
  }

  /* Preview the first match. */
  mc_preview_selection ();

  _mc_active = 1;

  /* Enter modal loop. */
  mc_modal_loop ();

  _mc_active = 0;

  /* Clean up saved state. */
  if (_mc_saved_text)
    {
      xfree (_mc_saved_text);
      _mc_saved_text = (char *)NULL;
    }

  /* Tell readline to redraw. */
  rl_forced_update_display ();
  rl_display_fixed = 1;
}
