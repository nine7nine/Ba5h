/* menucomplete.h -- visual completion menu for readline. */

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

#if !defined (_RL_MENUCOMPLETE_H_)
#define _RL_MENUCOMPLETE_H_

/* Display hook: renders a navigable completion menu below the prompt.
   Registered as rl_completion_display_matches_hook when enabled. */
extern void _rl_menu_complete_display_hook (char **matches, int len, int max);

#endif /* _RL_MENUCOMPLETE_H_ */
