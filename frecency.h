/* frecency.h -- directory frecency tracking for inline suggestions. */

#ifndef _FRECENCY_H_
#define _FRECENCY_H_

/* Initialize the frecency database (load from disk). */
extern void frecency_init (void);

/* Record a visit to PATH (called after successful cd). */
extern void frecency_update (const char *path);

/* Find the best frecency match for a partial directory name.
   Returns a malloc'd full path, or NULL if no match.
   QUERY is the partial text after "cd ". */
extern char *frecency_find (const char *query, int query_len);

/* Find the Nth best match (0 = best).  Returns malloc'd path or NULL. */
extern char *frecency_find_nth (const char *query, int query_len, int n);

#endif /* _FRECENCY_H_ */
