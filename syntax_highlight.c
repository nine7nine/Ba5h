/* syntax_highlight.c -- live syntax highlighting for the bash command line.

   Single-pass state machine that classifies each byte position in the
   editing buffer into a readline face type for coloring:

   '0' NORMAL    - default text (arguments, filenames)
   '3' KEYWORD   - shell reserved words (if, then, for, while, ...)
   '4' BUILTIN   - shell builtins (cd, echo, export, ...)
   '5' STRING    - quoted text ('...' "..." $'...')
   '6' VARIABLE  - variable references ($var, ${...})
   '7' COMMENT   - comments (# to end of line)
   '8' OPERATOR  - control operators (; | && || & ( ))
   '9' REDIRECT  - redirection operators (> >> < << &> etc.)
   ':' ERROR     - syntax errors (unclosed quotes at EOL, etc.)
*/

#include "config.h"

#include <sys/types.h>

#if defined (HAVE_STDLIB_H)
#  include <stdlib.h>
#else
#  include "ansi_stdlib.h"
#endif

#include <string.h>

#include "shell.h"
#include "syntax.h"
#include "input.h"
#include "builtins/common.h"
#include "syntax_highlight.h"

/* Face values -- must match lib/readline/display.c */
#define F_NORMAL   '0'
#define F_KEYWORD  '3'
#define F_BUILTIN  '4'
#define F_STRING   '5'
#define F_VARIABLE '6'
#define F_COMMENT  '7'
#define F_OPERATOR '8'
#define F_REDIRECT '9'
#define F_ERROR    ':'

/* Cache: avoid re-tokenizing when the line hasn't changed.
   cached_cap tracks allocated capacity for grow-only buffer reuse. */
static char *cached_line = NULL;
static int   cached_len = 0;
static int   cached_cap = 0;
static char *cached_faces = NULL;

/* Classify a completed word in command position.  Returns the face char. */
static char
classify_word (const char *word, int len, int is_cmd_pos)
{
  char saved, face;
  char *buf;

  if (len <= 0)
    return F_NORMAL;

  /* Need a null-terminated copy for the lookup functions. */
  buf = (char *)alloca (len + 1);
  memcpy (buf, word, len);
  buf[len] = '\0';

  /* Keywords always get keyword face, regardless of position. */
  if (find_reserved_word (buf) >= 0)
    return F_KEYWORD;

  /* In command position, check for builtins. */
  if (is_cmd_pos && find_shell_builtin (buf) != NULL)
    return F_BUILTIN;

  return F_NORMAL;
}

/* Core tokenizer: single-pass state machine. */
void
bash_syntax_highlight (const char *line, int len, char *faces)
{
  int i, word_start, in_cmd_pos;
  char face;

  if (len <= 0)
    return;

  /* Cache check. */
  if (cached_line && cached_len == len &&
      memcmp (cached_line, line, len) == 0 && cached_faces)
    {
      memcpy (faces, cached_faces, len);
      return;
    }

  /* Initialize: everything starts as NORMAL. */
  memset (faces, F_NORMAL, len);

  i = 0;
  in_cmd_pos = 1;	/* start of line is a command position */
  word_start = -1;

  while (i < len)
    {
      unsigned char c = (unsigned char)line[i];

      /* ---- Comments ---- */
      if (c == '#' && in_cmd_pos)
	{
	  /* # at the start of a word in command position is a comment.
	     Actually, # is a comment when it's the first char of a word. */
	  memset (faces + i, F_COMMENT, len - i);
	  break;
	}

      /* ---- Single-quoted strings ---- */
      if (c == '\'')
	{
	  int start = i;
	  faces[i++] = F_STRING;
	  while (i < len && line[i] != '\'')
	    faces[i++] = F_STRING;
	  if (i < len)
	    faces[i++] = F_STRING;	/* closing quote */
	  else
	    /* Unclosed quote -- mark opening quote as error */
	    faces[start] = F_ERROR;
	  in_cmd_pos = 0;
	  word_start = -1;
	  continue;
	}

      /* ---- Double-quoted strings ---- */
      if (c == '"')
	{
	  int start = i;
	  faces[i++] = F_STRING;
	  while (i < len && line[i] != '"')
	    {
	      if (line[i] == '\\' && i + 1 < len)
		{
		  faces[i] = F_STRING;
		  i++;
		  faces[i] = F_STRING;
		  i++;
		}
	      else if (line[i] == '$')
		{
		  /* Variable inside double quotes */
		  faces[i++] = F_VARIABLE;
		  if (i < len && line[i] == '{')
		    {
		      faces[i++] = F_VARIABLE;
		      while (i < len && line[i] != '}')
			faces[i++] = F_VARIABLE;
		      if (i < len)
			faces[i++] = F_VARIABLE; /* closing } */
		    }
		  else if (i < len && line[i] == '(')
		    {
		      /* Command substitution $(...) -- color as operator */
		      int depth = 1;
		      faces[i++] = F_OPERATOR;
		      while (i < len && depth > 0)
			{
			  if (line[i] == '(')
			    depth++;
			  else if (line[i] == ')')
			    depth--;
			  if (depth > 0)
			    faces[i++] = F_NORMAL;
			  else
			    faces[i++] = F_OPERATOR;
			}
		    }
		  else
		    {
		      /* $VARNAME */
		      while (i < len && line[i] != '"' &&
			     (line[i] == '_' ||
			      (line[i] >= 'a' && line[i] <= 'z') ||
			      (line[i] >= 'A' && line[i] <= 'Z') ||
			      (line[i] >= '0' && line[i] <= '9')))
			faces[i++] = F_VARIABLE;
		    }
		}
	      else
		{
		  faces[i] = F_STRING;
		  i++;
		}
	    }
	  if (i < len)
	    faces[i++] = F_STRING;	/* closing quote */
	  else
	    faces[start] = F_ERROR;
	  in_cmd_pos = 0;
	  word_start = -1;
	  continue;
	}

      /* ---- $'...' ANSI-C strings ---- */
      if (c == '$' && i + 1 < len && line[i + 1] == '\'')
	{
	  int start = i;
	  faces[i++] = F_STRING;	/* $ */
	  faces[i++] = F_STRING;	/* ' */
	  while (i < len && line[i] != '\'')
	    {
	      if (line[i] == '\\' && i + 1 < len)
		{
		  faces[i] = F_STRING;
		  i++;
		}
	      faces[i] = F_STRING;
	      i++;
	    }
	  if (i < len)
	    faces[i++] = F_STRING;	/* closing quote */
	  else
	    faces[start] = F_ERROR;
	  in_cmd_pos = 0;
	  word_start = -1;
	  continue;
	}

      /* ---- Variables ($var, ${var}, special vars) ---- */
      if (c == '$')
	{
	  faces[i++] = F_VARIABLE;
	  if (i < len && line[i] == '{')
	    {
	      faces[i++] = F_VARIABLE;
	      while (i < len && line[i] != '}')
		faces[i++] = F_VARIABLE;
	      if (i < len)
		faces[i++] = F_VARIABLE;	/* closing } */
	    }
	  else if (i < len && line[i] == '(')
	    {
	      /* Command substitution $(...) */
	      int depth = 1;
	      faces[i++] = F_OPERATOR;
	      while (i < len && depth > 0)
		{
		  if (line[i] == '(')
		    depth++;
		  else if (line[i] == ')')
		    depth--;
		  if (depth > 0)
		    faces[i++] = F_NORMAL;
		  else
		    faces[i++] = F_OPERATOR;
		}
	    }
	  else
	    {
	      /* Special single-char vars: $?, $!, $#, $@, $*, $0-$9, $- */
	      if (i < len && (line[i] == '?' || line[i] == '!' ||
			      line[i] == '#' || line[i] == '@' ||
			      line[i] == '*' || line[i] == '-' ||
			      (line[i] >= '0' && line[i] <= '9')))
		{
		  faces[i++] = F_VARIABLE;
		}
	      else
		{
		  /* Regular variable name */
		  while (i < len &&
			 (line[i] == '_' ||
			  (line[i] >= 'a' && line[i] <= 'z') ||
			  (line[i] >= 'A' && line[i] <= 'Z') ||
			  (line[i] >= '0' && line[i] <= '9')))
		    faces[i++] = F_VARIABLE;
		}
	    }
	  in_cmd_pos = 0;
	  word_start = -1;
	  continue;
	}

      /* ---- Backslash escape ---- */
      if (c == '\\' && i + 1 < len)
	{
	  /* Escaped character -- both chars get normal face */
	  i += 2;
	  in_cmd_pos = 0;
	  continue;
	}

      /* ---- Control operators ---- */
      if (c == ';' || c == '&' || c == '|' || c == '(' || c == ')')
	{
	  /* Flush any pending word first */
	  if (word_start >= 0)
	    {
	      face = classify_word (line + word_start, i - word_start, in_cmd_pos);
	      if (face != F_NORMAL)
		memset (faces + word_start, face, i - word_start);
	      word_start = -1;
	    }

	  faces[i] = F_OPERATOR;
	  /* Handle two-character operators: && || ;; */
	  if (i + 1 < len &&
	      ((c == '&' && line[i + 1] == '&') ||
	       (c == '|' && line[i + 1] == '|') ||
	       (c == ';' && line[i + 1] == ';')))
	    {
	      faces[i + 1] = F_OPERATOR;
	      i += 2;
	    }
	  else
	    i++;

	  in_cmd_pos = 1;	/* next word is a command */
	  continue;
	}

      /* ---- Redirection operators ---- */
      if (c == '>' || c == '<')
	{
	  /* Flush any pending word first */
	  if (word_start >= 0)
	    {
	      face = classify_word (line + word_start, i - word_start, in_cmd_pos);
	      if (face != F_NORMAL)
		memset (faces + word_start, face, i - word_start);
	      word_start = -1;
	    }

	  faces[i] = F_REDIRECT;
	  /* Handle multi-character redirections: >> << >& <& &> etc. */
	  if (i + 1 < len &&
	      (line[i + 1] == '>' || line[i + 1] == '<' ||
	       line[i + 1] == '&'))
	    {
	      faces[i + 1] = F_REDIRECT;
	      i += 2;
	    }
	  else
	    i++;

	  /* A FD number just before a redirect is part of the redirect */
	  /* After a redirect, next word is a filename, not a command */
	  in_cmd_pos = 0;
	  continue;
	}

      /* ---- Whitespace ---- */
      if (c == ' ' || c == '\t')
	{
	  /* Flush pending word */
	  if (word_start >= 0)
	    {
	      face = classify_word (line + word_start, i - word_start, in_cmd_pos);
	      if (face != F_NORMAL)
		memset (faces + word_start, face, i - word_start);
	      word_start = -1;
	      in_cmd_pos = 0;  /* we saw a word, next is not cmd pos */
	    }
	  i++;
	  continue;
	}

      /* ---- Backtick command substitution ---- */
      if (c == '`')
	{
	  /* Flush pending word */
	  if (word_start >= 0)
	    {
	      face = classify_word (line + word_start, i - word_start, in_cmd_pos);
	      if (face != F_NORMAL)
		memset (faces + word_start, face, i - word_start);
	      word_start = -1;
	    }

	  faces[i++] = F_OPERATOR;
	  while (i < len && line[i] != '`')
	    {
	      if (line[i] == '\\' && i + 1 < len)
		{
		  i++;  /* skip escaped char */
		  i++;
		}
	      else
		i++;
	    }
	  if (i < len)
	    faces[i++] = F_OPERATOR;

	  in_cmd_pos = 0;
	  continue;
	}

      /* ---- Regular word characters ---- */
      if (word_start < 0)
	word_start = i;
      i++;
    }

  /* Flush final pending word */
  if (word_start >= 0)
    {
      face = classify_word (line + word_start, i - word_start, in_cmd_pos);
      if (face != F_NORMAL)
	memset (faces + word_start, face, i - word_start);
    }

  /* Update cache -- grow-only realloc to avoid malloc/free per keystroke */
  if (len >= cached_cap)
    {
      cached_cap = len + 64;
      cached_line = (char *)realloc (cached_line, cached_cap + 1);
      cached_faces = (char *)realloc (cached_faces, cached_cap + 1);
    }
  if (cached_line && cached_faces)
    {
      memcpy (cached_line, line, len);
      cached_line[len] = '\0';
      cached_len = len;
      memcpy (cached_faces, faces, len);
      cached_faces[len] = '\0';
    }
}
