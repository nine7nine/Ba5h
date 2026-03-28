/* suggest.c -- inline suggestion support for readline. */

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

#include <sys/types.h>

#if defined (HAVE_STDLIB_H)
#  include <stdlib.h>
#else
#  include "ansi_stdlib.h"
#endif

#include <stdio.h>
#include <string.h>

#if defined (HAVE_UNISTD_H)
#  include <unistd.h>
#endif

#if defined (HAVE_SYS_TIME_H)
#  include <sys/time.h>
#endif

#include "rldefs.h"
#include "rlmbutil.h"
#include "readline.h"
#include "history.h"
#include "rlprivate.h"
#include "xmalloc.h"
#include "suggest.h"

/* The current suggestion text (the suffix beyond what's typed). */
static char *_rl_suggestion_text = (char *)NULL;
static int _rl_suggestion_len = 0;

/* Cache: the line content that produced the current suggestion. */
static char *_rl_suggestion_cached_line = (char *)NULL;
static int _rl_suggestion_cached_len = 0;

/* Tracks whether the last action was a suggestion acceptance, so that
   left arrow can undo it.  Cleared on any other editing action. */
static int _rl_suggestion_just_accepted = 0;

/* Non-zero when the user has explicitly dismissed the current suggestion
   via rl_dismiss_suggestion.  Prevents re-suggesting until the line
   content changes. */
static int _rl_suggestion_dismissed = 0;

/* Non-zero when the current suggestion is a full-line replacement from
   substring matching, rather than a suffix append from prefix matching. */
static int _rl_suggestion_is_replacement = 0;

/* When _rl_suggestion_is_replacement is set, this holds the full line to
   replace the buffer with on acceptance.  _rl_suggestion_text holds the
   display suffix (portion after the matched substring). */
static char *_rl_suggestion_replacement = (char *)NULL;

/* History index of the entry that produced the current suggestion.
   -1 means no match / initial state.  Used for cycling. */
static int _rl_suggestion_match_index = -1;

/* ---- Predictor registry ---- */

/* A registered prediction source. */
typedef struct _rl_predictor_entry {
  char *name;
  rl_predictor_func_t *func;
  int priority;
  struct _rl_predictor_entry *next;
} rl_predictor_entry_t;

static rl_predictor_entry_t *_rl_predictors = (rl_predictor_entry_t *)NULL;

/* Clear any active suggestion and free memory. */
void
_rl_suggestion_clear (void)
{
  if (_rl_suggestion_text)
    {
      xfree (_rl_suggestion_text);
      _rl_suggestion_text = (char *)NULL;
    }
  _rl_suggestion_len = 0;

  if (_rl_suggestion_cached_line)
    {
      xfree (_rl_suggestion_cached_line);
      _rl_suggestion_cached_line = (char *)NULL;
    }
  _rl_suggestion_cached_len = 0;
  _rl_suggestion_just_accepted = 0;

  if (_rl_suggestion_replacement)
    {
      xfree (_rl_suggestion_replacement);
      _rl_suggestion_replacement = (char *)NULL;
    }

  _rl_suggestion_match_index = -1;
}

/* Returns non-zero if the last action was a suggestion acceptance. */
int
_rl_suggestion_was_accepted (void)
{
  return _rl_suggestion_just_accepted;
}

/* Undo the last suggestion acceptance. Returns non-zero on success. */
int
_rl_suggestion_undo_accept (void)
{
  if (_rl_suggestion_just_accepted == 0)
    return 0;

  _rl_suggestion_just_accepted = 0;
  /* The acceptance was wrapped in an undo group, so a single undo
     reverts the entire insertion. */
  rl_do_undo ();
  return 1;
}

/* Accessors for display.c */
const char *
_rl_suggestion_get (void)
{
  return _rl_suggestion_text;
}

int
_rl_suggestion_get_len (void)
{
  return _rl_suggestion_len;
}

/* Returns non-zero if current suggestion is a full-line replacement
   (from substring matching) rather than a suffix append. */
int
_rl_suggestion_is_full_replacement (void)
{
  return _rl_suggestion_is_replacement;
}

/* Returns the full replacement text for substring matches, or NULL. */
const char *
_rl_suggestion_get_replacement (void)
{
  return _rl_suggestion_replacement;
}

/* Search history for a matching entry starting at index START and moving
   in DIRECTION (-1 = older, +1 = newer).  For each entry, tries prefix
   match first, then substring match (if strategy allows).

   On success: sets _rl_suggestion_match_index, _rl_suggestion_is_replacement,
   and _rl_suggestion_replacement.  Returns newly-allocated suggestion text.
   On failure: returns NULL, leaves state unchanged. */
static char *
suggestion_from_history_starting (int start, int direction)
{
  HIST_ENTRY **hist;
  int i, hlen, len;
  const char *line;

  if (rl_end == 0)
    return (char *)NULL;

  hist = history_list ();
  if (hist == NULL)
    return (char *)NULL;

  len = rl_end;

  for (hlen = 0; hist[hlen]; hlen++)
    ;

  if (start < 0)
    start = 0;
  if (start >= hlen)
    start = hlen - 1;

  for (i = start; i >= 0 && i < hlen; i += direction)
    {
      line = hist[i]->line;
      if (line == NULL)
	continue;

      /* Prefix match (always tried first) */
      if (strncmp (line, rl_line_buffer, len) == 0 && line[len] != '\0')
	{
	  _rl_suggestion_is_replacement = 0;
	  _rl_suggestion_match_index = i;
	  if (_rl_suggestion_replacement)
	    {
	      xfree (_rl_suggestion_replacement);
	      _rl_suggestion_replacement = (char *)NULL;
	    }
	  return savestring (line + len);
	}

      /* Substring match (only if strategy is "substring") */
      if (_rl_suggestion_strategy == 1)
	{
	  const char *match;

	  /* Skip entries identical to what's typed */
	  if (strcmp (line, rl_line_buffer) == 0)
	    continue;

	  match = strstr (line, rl_line_buffer);
	  if (match != NULL)
	    {
	      _rl_suggestion_is_replacement = 1;
	      _rl_suggestion_match_index = i;
	      if (_rl_suggestion_replacement)
		xfree (_rl_suggestion_replacement);
	      _rl_suggestion_replacement = savestring (line);
	      return savestring (match + len);
	    }
	}
    }

  return (char *)NULL;
}

/* Search history backwards from the most recent entry.  Wrapper around
   suggestion_from_history_starting() for the initial suggestion. */
static char *
suggestion_from_history (void)
{
  HIST_ENTRY **hist;
  int hlen;

  hist = history_list ();
  if (hist == NULL)
    return (char *)NULL;

  for (hlen = 0; hist[hlen]; hlen++)
    ;

  _rl_suggestion_match_index = -1;
  return suggestion_from_history_starting (hlen - 1, -1);
}

/* ---- Predictor registry implementation ---- */

/* Register a predictor.  Lower PRIORITY values are queried first.
   Returns 0 on success, -1 if NAME already exists or on error. */
int
rl_add_predictor (const char *name, rl_predictor_func_t *func, int priority)
{
  rl_predictor_entry_t *entry, *prev, *cur;

  if (name == NULL || func == NULL)
    return -1;

  /* Reject duplicate names */
  for (cur = _rl_predictors; cur; cur = cur->next)
    if (strcmp (cur->name, name) == 0)
      return -1;

  entry = (rl_predictor_entry_t *)xmalloc (sizeof (rl_predictor_entry_t));
  entry->name = savestring (name);
  entry->func = func;
  entry->priority = priority;
  entry->next = (rl_predictor_entry_t *)NULL;

  /* Insert sorted by priority (lower = first) */
  prev = (rl_predictor_entry_t *)NULL;
  cur = _rl_predictors;
  while (cur && cur->priority <= priority)
    {
      prev = cur;
      cur = cur->next;
    }
  entry->next = cur;
  if (prev)
    prev->next = entry;
  else
    _rl_predictors = entry;

  return 0;
}

/* Remove a predictor by name.  Returns 0 on success, -1 if not found. */
int
rl_remove_predictor (const char *name)
{
  rl_predictor_entry_t *prev, *cur;

  if (name == NULL)
    return -1;

  prev = (rl_predictor_entry_t *)NULL;
  for (cur = _rl_predictors; cur; prev = cur, cur = cur->next)
    {
      if (strcmp (cur->name, name) == 0)
	{
	  if (prev)
	    prev->next = cur->next;
	  else
	    _rl_predictors = cur->next;
	  xfree (cur->name);
	  xfree (cur);
	  return 0;
	}
    }
  return -1;
}

/* Built-in history predictor with the standard predictor signature.
   Wraps suggestion_from_history(). */
char *
_rl_history_predictor (const char *line, int len, int *is_substring, char **replacement)
{
  char *result;

  *is_substring = 0;
  *replacement = (char *)NULL;

  result = suggestion_from_history ();
  if (result)
    {
      /* Copy match state out through the predictor interface.
	 suggestion_from_history() sets these module-level statics;
	 transfer ownership to the caller via output params and clear
	 the statics so dispatch can set them cleanly. */
      *is_substring = _rl_suggestion_is_replacement;
      if (_rl_suggestion_replacement)
	{
	  *replacement = _rl_suggestion_replacement;
	  _rl_suggestion_replacement = (char *)NULL;
	}
      _rl_suggestion_is_replacement = 0;
    }
  return result;
}

/* Walk the predictor list, calling each in priority order.
   Returns the first non-NULL result. */
static char *
_rl_predictor_dispatch (int *is_substring_out, char **replacement_out)
{
  rl_predictor_entry_t *p;
  char *result;

  *is_substring_out = 0;
  *replacement_out = (char *)NULL;

  for (p = _rl_predictors; p; p = p->next)
    {
      result = (*p->func) (rl_line_buffer, rl_end, is_substring_out, replacement_out);
      if (result != NULL)
	return result;
      /* Reset for next predictor */
      *is_substring_out = 0;
      *replacement_out = (char *)NULL;
    }
  return (char *)NULL;
}

/* Update the suggestion state. Called before each redisplay from
   _rl_internal_char_cleanup(). */
void
_rl_suggestion_update (void)
{
  char *suggestion;

  /* Gate: feature must be enabled */
  if (_rl_enable_inline_suggestions == 0)
    {
      if (_rl_suggestion_text)
	_rl_suggestion_clear ();
      return;
    }

  /* Gate: cursor must be at end of line */
  if (rl_point != rl_end)
    {
      if (_rl_suggestion_text)
	_rl_suggestion_clear ();
      return;
    }

  /* Gate: not during special states */
  if (RL_ISSTATE (RL_STATE_ISEARCH) ||
      RL_ISSTATE (RL_STATE_NSEARCH) ||
      RL_ISSTATE (RL_STATE_COMPLETING) ||
      RL_ISSTATE (RL_STATE_MACRODEF) ||
      RL_ISSTATE (RL_STATE_READSTR))
    {
      if (_rl_suggestion_text)
	_rl_suggestion_clear ();
      return;
    }

  /* Gate: not in vi command mode */
#if defined (VI_MODE)
  if (VI_COMMAND_MODE ())
    {
      if (_rl_suggestion_text)
	_rl_suggestion_clear ();
      return;
    }
#endif

  /* Gate: horizontal scroll mode not supported */
  if (_rl_horizontal_scroll_mode)
    {
      if (_rl_suggestion_text)
	_rl_suggestion_clear ();
      return;
    }

  /* Gate: empty line - no suggestion */
  if (rl_end == 0)
    {
      if (_rl_suggestion_text)
	_rl_suggestion_clear ();
      return;
    }

  /* If suggestion was dismissed, suppress re-suggestion until the line
     content actually changes. */
  if (_rl_suggestion_dismissed)
    {
      if (_rl_suggestion_cached_line &&
	  _rl_suggestion_cached_len == rl_end &&
	  strncmp (_rl_suggestion_cached_line, rl_line_buffer, rl_end) == 0)
	return;		/* still dismissed, line unchanged */
      /* Line content changed -- clear the dismissed flag and proceed */
      _rl_suggestion_dismissed = 0;
    }

  /* Cache check: if line content hasn't changed, keep current suggestion */
  if (_rl_suggestion_cached_line &&
      _rl_suggestion_cached_len == rl_end &&
      strncmp (_rl_suggestion_cached_line, rl_line_buffer, rl_end) == 0)
    {
      /* Line unchanged -- if last command was not an accept, clear flag */
      if (rl_last_func != rl_accept_suggestion &&
	  rl_last_func != rl_accept_suggestion_word)
	_rl_suggestion_just_accepted = 0;
      return;
    }

  /* Line content changed.  If this was not from an acceptance, clear
     the acceptance flag so left arrow won't undo an old acceptance. */
  if (rl_last_func != rl_accept_suggestion &&
      rl_last_func != rl_accept_suggestion_word)
    _rl_suggestion_just_accepted = 0;

  /* Line content changed - search for a new suggestion */
  if (_rl_suggestion_text)
    {
      xfree (_rl_suggestion_text);
      _rl_suggestion_text = (char *)NULL;
      _rl_suggestion_len = 0;
    }

  if (_rl_suggestion_replacement)
    {
      xfree (_rl_suggestion_replacement);
      _rl_suggestion_replacement = (char *)NULL;
    }

  if (_rl_suggestion_cached_line)
    {
      xfree (_rl_suggestion_cached_line);
      _rl_suggestion_cached_line = (char *)NULL;
    }

  if (_rl_predictors)
    {
      int is_sub = 0;
      char *repl = (char *)NULL;

      suggestion = _rl_predictor_dispatch (&is_sub, &repl);
      if (suggestion)
	{
	  _rl_suggestion_text = suggestion;
	  _rl_suggestion_len = strlen (suggestion);
	  _rl_suggestion_is_replacement = is_sub;
	  _rl_suggestion_replacement = repl;
	}
    }
  else
    {
      /* Fallback: no predictors registered, use built-in history search */
      suggestion = suggestion_from_history ();
      if (suggestion)
	{
	  _rl_suggestion_text = suggestion;
	  _rl_suggestion_len = strlen (suggestion);
	}
    }

  /* Update cache */
  _rl_suggestion_cached_line = savestring (rl_line_buffer);
  _rl_suggestion_cached_len = rl_end;
}

/* Bindable command: accept the entire current suggestion. */
int
rl_accept_suggestion (int count, int key)
{
  int r;

  if (_rl_suggestion_text == NULL || _rl_suggestion_len == 0)
    return 0;

  rl_begin_undo_group ();
  if (_rl_suggestion_is_replacement && _rl_suggestion_replacement)
    {
      /* Substring match: replace the entire line with the full match */
      rl_delete_text (0, rl_end);
      rl_point = 0;
      r = rl_insert_text (_rl_suggestion_replacement);
    }
  else
    {
      /* Prefix match: append the suggestion suffix */
      r = rl_insert_text (_rl_suggestion_text);
    }
  rl_end_undo_group ();
  _rl_suggestion_just_accepted = 1;
  _rl_suggestion_clear ();
  return r;
}

/* Bindable command: accept the next word from the current suggestion. */
int
rl_accept_suggestion_word (int count, int key)
{
  int i, len;
  char *word;
  int r;

  if (_rl_suggestion_text == NULL || _rl_suggestion_len == 0)
    return 0;

  if (_rl_suggestion_is_replacement)
    {
      /* For substring matches, accept-word replaces line with the full
	 suggestion (word-by-word doesn't make sense for replacements). */
      return rl_accept_suggestion (count, key);
    }

  len = _rl_suggestion_len;
  i = 0;

  /* Skip leading whitespace in suggestion */
  while (i < len && whitespace (_rl_suggestion_text[i]))
    i++;

  /* Find end of word */
  while (i < len && !whitespace (_rl_suggestion_text[i]))
    i++;

  if (i == 0)
    return 0;

  /* Insert the word portion */
  word = (char *)xmalloc (i + 1);
  strncpy (word, _rl_suggestion_text, i);
  word[i] = '\0';

  rl_begin_undo_group ();
  r = rl_insert_text (word);
  rl_end_undo_group ();
  xfree (word);

  _rl_suggestion_just_accepted = 1;
  /* The suggestion will be updated on next _rl_suggestion_update() call */
  _rl_suggestion_clear ();

  return r;
}

/* Portable millisecond sleep using select(). */
static void
_rl_sleep_ms (int ms)
{
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  select (0, NULL, NULL, NULL, &tv);
}

/* Compute the display width (in screen columns) of the first NBYTES
   bytes of text S, capped to MAX_COLS.  Handles multibyte characters. */
static int
_rl_text_display_width (const char *s, int nbytes, int max_cols)
{
  int cols, si;

  if (s == NULL || nbytes <= 0 || max_cols <= 0)
    return 0;

  cols = 0;
  si = 0;
  while (si < nbytes && cols < max_cols)
    {
#if defined (HANDLE_MULTIBYTE)
      if (MB_CUR_MAX > 1 && rl_byte_oriented == 0)
	{
	  WCHAR_T wc;
	  mbstate_t ps;
	  size_t bytes;
	  int w;

	  memset (&ps, 0, sizeof (mbstate_t));
	  bytes = MBRTOWC (&wc, s + si, nbytes - si, &ps);
	  if (MB_INVALIDCH (bytes) || MB_NULLWCH (bytes))
	    break;
	  w = WCWIDTH (wc);
	  if (w < 0)
	    w = 1;
	  if (cols + w > max_cols)
	    break;
	  si += bytes;
	  cols += w;
	}
      else
#endif
	{
	  si++;
	  cols++;
	}
    }
  return cols;
}

/* Animate the suggestion disappearing column-by-column from right to left.

   Prefix match:  cursor is at end of typed text; ghost suffix follows.
     Animation erases the ghost suffix only.  Typed text remains.

   Substring match:  the entire line area shows the full replacement
     (ghost prefix + typed portion + ghost suffix).  Animation erases the
     whole replacement and the line buffer is cleared by the caller. */
static void
_rl_suggestion_dismiss_animated (void)
{
  int suffix_width, erase_width, right_move, col, delay_ms;

  /* Width of the ghost suffix that sits past the cursor */
  suffix_width = _rl_text_display_width (
      _rl_suggestion_text, _rl_suggestion_len,
      _rl_screenwidth - _rl_last_c_pos);

  if (_rl_suggestion_is_replacement && _rl_suggestion_replacement)
    {
      /* Substring match: compute the width of the full replacement that
	 was rendered.  The display code renders from the line-content
	 start, so we compute the full replacement display width and the
	 prefix+typed portion (everything left of the cursor). */
      const char *repl = _rl_suggestion_replacement;
      const char *match_pos = strstr (repl, rl_line_buffer);
      int match_end = match_pos
	  ? (int)(match_pos - repl) + rl_end
	  : rl_end;
      int prefix_typed_width = _rl_text_display_width (
	  repl, match_end, _rl_screenwidth);

      erase_width = prefix_typed_width + suffix_width;
      right_move = suffix_width;
    }
  else
    {
      /* Prefix match: only the ghost suffix after the cursor */
      erase_width = suffix_width;
      right_move = suffix_width;
    }

  if (erase_width <= 0)
    return;

  /* Adaptive delay: target ~400ms total, clamped to [5, 25] ms/column */
  delay_ms = 400 / erase_width;
  if (delay_ms < 5)
    delay_ms = 5;
  if (delay_ms > 25)
    delay_ms = 25;

  /* Move cursor to the rightmost column of the displayed text */
  if (right_move > 0)
    fprintf (rl_outstream, "\033[%dC", right_move);

  /* Erase one column at a time, sweeping left */
  for (col = erase_width; col > 0; col--)
    {
      fprintf (rl_outstream, "\033[D\033[K");
      fflush (rl_outstream);
      _rl_sleep_ms (delay_ms);
    }

  fflush (rl_outstream);
}

/* Internal dismiss: clear suggestion text and set the dismissed flag so
   _rl_suggestion_update() won't re-suggest until the line changes. */
static void
_rl_suggestion_dismiss (void)
{
  if (_rl_suggestion_text)
    {
      xfree (_rl_suggestion_text);
      _rl_suggestion_text = (char *)NULL;
    }
  _rl_suggestion_len = 0;
  _rl_suggestion_just_accepted = 0;

  if (_rl_suggestion_replacement)
    {
      xfree (_rl_suggestion_replacement);
      _rl_suggestion_replacement = (char *)NULL;
    }
  _rl_suggestion_is_replacement = 0;

  /* Keep _rl_suggestion_cached_line intact so the dismissed-flag check
     in _rl_suggestion_update() can detect that the line is unchanged. */
  _rl_suggestion_dismissed = 1;
}

/* Bindable command: dismiss the current suggestion with a fade-back
   animation.  For substring matches the entire visual replacement is
   erased and the line buffer is cleared; for prefix matches only the
   ghost suffix is erased and the typed text is preserved. */
int
rl_dismiss_suggestion (int count, int key)
{
  int was_replacement;

  if (_rl_suggestion_text == NULL || _rl_suggestion_len == 0)
    return 0;

  was_replacement = _rl_suggestion_is_replacement;

  _rl_suggestion_dismiss_animated ();
  _rl_suggestion_dismiss ();

  /* For substring matches the animation erased the full visual line
     (ghost prefix + typed text + ghost suffix).  Clear the actual line
     buffer so the prompt matches the now-empty display area. */
  if (was_replacement && rl_end > 0)
    {
      rl_delete_text (0, rl_end);
      rl_point = 0;
    }

  return 1;
}

/* Helper: replace the current suggestion text with a new one (used by
   the cycle functions).  Does NOT touch the cache or dismissed flag. */
static void
_rl_suggestion_set (char *text)
{
  if (_rl_suggestion_text)
    xfree (_rl_suggestion_text);
  _rl_suggestion_text = text;
  _rl_suggestion_len = text ? (int)strlen (text) : 0;
  _rl_suggestion_just_accepted = 0;
}

/* Bindable command: cycle to the previous (older) matching history entry
   and display it as the inline suggestion.  Dings if already at the
   oldest match. */
int
rl_suggestion_cycle_previous (int count, int key)
{
  char *suggestion;

  if (_rl_suggestion_text == NULL || _rl_suggestion_len == 0)
    return 0;

  if (_rl_suggestion_match_index <= 0)
    {
      rl_ding ();
      return 0;
    }

  suggestion = suggestion_from_history_starting (
      _rl_suggestion_match_index - 1, -1);

  if (suggestion == NULL)
    {
      rl_ding ();
      return 0;
    }

  _rl_suggestion_set (suggestion);
  return 0;
}

/* Bindable command: cycle to the next (newer) matching history entry.
   If already at the newest match (or no newer match exists), dismiss
   the suggestion with animation. */
int
rl_suggestion_cycle_next (int count, int key)
{
  HIST_ENTRY **hist;
  int hlen;
  char *suggestion;

  if (_rl_suggestion_text == NULL || _rl_suggestion_len == 0)
    return 0;

  hist = history_list ();
  if (hist == NULL)
    return rl_dismiss_suggestion (count, key);

  for (hlen = 0; hist[hlen]; hlen++)
    ;

  if (_rl_suggestion_match_index >= hlen - 1)
    return rl_dismiss_suggestion (count, key);

  suggestion = suggestion_from_history_starting (
      _rl_suggestion_match_index + 1, +1);

  if (suggestion == NULL)
    return rl_dismiss_suggestion (count, key);

  _rl_suggestion_set (suggestion);
  return 0;
}
