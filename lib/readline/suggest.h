/* suggest.h -- inline suggestion support for readline. */

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

#if !defined (_RL_SUGGEST_H_)
#define _RL_SUGGEST_H_

/* Update the current suggestion based on line buffer state.
   Called before each redisplay. */
extern void _rl_suggestion_update (void);

/* Clear any active suggestion. */
extern void _rl_suggestion_clear (void);

/* Accessors for the current suggestion text. */
extern const char *_rl_suggestion_get (void);
extern int _rl_suggestion_get_len (void);

/* Returns non-zero if the last action was a suggestion acceptance. */
extern int _rl_suggestion_was_accepted (void);

/* Undo the last suggestion acceptance.  Returns non-zero on success. */
extern int _rl_suggestion_undo_accept (void);

/* Returns non-zero if the current suggestion is a full-line replacement
   (from substring matching) rather than a suffix append. */
extern int _rl_suggestion_is_full_replacement (void);

/* Returns the full replacement text for substring matches (or NULL). */
extern const char *_rl_suggestion_get_replacement (void);

/* Bindable commands for accepting suggestions. */
extern int rl_accept_suggestion (int, int);
extern int rl_accept_suggestion_word (int, int);

/* Bindable command: dismiss the current suggestion with animation. */
extern int rl_dismiss_suggestion (int, int);

/* Bindable commands for cycling through matching suggestions. */
extern int rl_suggestion_cycle_previous (int, int);
extern int rl_suggestion_cycle_next (int, int);

#endif /* _RL_SUGGEST_H_ */
