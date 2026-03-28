/* syntax_highlight.h -- live syntax highlighting for the bash command line. */

#ifndef _SYNTAX_HIGHLIGHT_H_
#define _SYNTAX_HIGHLIGHT_H_

/* Callback function for rl_syntax_highlight_func.
   Tokenizes LINE (of LEN bytes) and fills FACES with per-character
   face values suitable for readline's display system. */
extern void bash_syntax_highlight (const char *line, int len, char *faces);

#endif /* _SYNTAX_HIGHLIGHT_H_ */
