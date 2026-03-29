/* frecency.c -- directory frecency tracking for inline suggestions.

   Maintains an in-memory database of visited directories scored by
   a combination of frequency and recency.  Persisted to ~/.bash_frecency.

   Score = frequency / (1 + hours_since_last_visit / 4)

   This gives high scores to directories visited both frequently and
   recently, with natural decay for unused directories.  */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#if defined (HAVE_STDLIB_H)
#  include <stdlib.h>
#else
#  include "ansi_stdlib.h"
#endif

#include <string.h>
#include <stdio.h>
#include <time.h>

#if defined (HAVE_UNISTD_H)
#  include <unistd.h>
#endif

#include "shell.h"
#include "frecency.h"

/* Maximum number of entries to keep in the database. */
#define FRECENCY_MAX_ENTRIES 500

/* A single frecency entry. */
typedef struct {
  char *path;
  int frequency;
  time_t last_access;
} frecency_entry_t;

/* In-memory database. */
static frecency_entry_t *frecency_db = (frecency_entry_t *)NULL;
static int frecency_count = 0;
static int frecency_capacity = 0;
static int frecency_initialized = 0;

/* Path to the frecency data file. */
static char *
frecency_file_path (void)
{
  const char *home;
  char *path;
  size_t len;

  home = get_string_value ("HOME");
  if (home == NULL)
    return (char *)NULL;

  len = strlen (home) + sizeof ("/.bash_frecency");
  path = (char *)xmalloc (len);
  snprintf (path, len, "%s/.bash_frecency", home);
  return path;
}

/* Compute the frecency score for an entry. */
static double
frecency_score (frecency_entry_t *entry)
{
  time_t now;
  double hours;

  now = time (NULL);
  hours = difftime (now, entry->last_access) / 3600.0;
  if (hours < 0)
    hours = 0;

  return (double)entry->frequency / (1.0 + hours / 4.0);
}

/* Load the database from disk. */
void
frecency_init (void)
{
  char *fpath, *line, *p;
  FILE *fp;
  char buf[PATH_MAX + 64];

  if (frecency_initialized)
    return;

  frecency_initialized = 1;
  frecency_capacity = 64;
  frecency_db = (frecency_entry_t *)xmalloc (frecency_capacity * sizeof (frecency_entry_t));
  frecency_count = 0;

  fpath = frecency_file_path ();
  if (fpath == NULL)
    return;

  fp = fopen (fpath, "r");
  xfree (fpath);
  if (fp == NULL)
    return;

  while (fgets (buf, sizeof (buf), fp) != NULL)
    {
      /* Format: path|frequency|timestamp */
      line = buf;

      /* Strip trailing newline */
      p = strchr (line, '\n');
      if (p)
	*p = '\0';

      /* Parse path */
      p = strchr (line, '|');
      if (p == NULL)
	continue;
      *p = '\0';

      if (frecency_count >= frecency_capacity)
	{
	  frecency_capacity *= 2;
	  if (frecency_capacity > FRECENCY_MAX_ENTRIES)
	    frecency_capacity = FRECENCY_MAX_ENTRIES;
	  frecency_db = (frecency_entry_t *)xrealloc (frecency_db,
	      frecency_capacity * sizeof (frecency_entry_t));
	}

      frecency_db[frecency_count].path = savestring (line);
      frecency_db[frecency_count].frequency = atoi (p + 1);

      /* Parse timestamp */
      p = strchr (p + 1, '|');
      frecency_db[frecency_count].last_access = p ? (time_t)strtol (p + 1, NULL, 10) : time (NULL);
      frecency_count++;

      if (frecency_count >= FRECENCY_MAX_ENTRIES)
	break;
    }

  fclose (fp);
}

/* Save the database to disk. */
static void
frecency_save (void)
{
  char *fpath;
  FILE *fp;
  int i;

  fpath = frecency_file_path ();
  if (fpath == NULL)
    return;

  fp = fopen (fpath, "w");
  xfree (fpath);
  if (fp == NULL)
    return;

  for (i = 0; i < frecency_count; i++)
    fprintf (fp, "%s|%d|%ld\n",
	     frecency_db[i].path,
	     frecency_db[i].frequency,
	     (long)frecency_db[i].last_access);

  fclose (fp);
}

/* Evict the lowest-scoring entry when the database is full. */
static void
frecency_evict_lowest (void)
{
  int i, worst;
  double worst_score, s;

  if (frecency_count <= 0)
    return;

  worst = 0;
  worst_score = frecency_score (&frecency_db[0]);

  for (i = 1; i < frecency_count; i++)
    {
      s = frecency_score (&frecency_db[i]);
      if (s < worst_score)
	{
	  worst_score = s;
	  worst = i;
	}
    }

  xfree (frecency_db[worst].path);
  /* Move last entry into the evicted slot */
  if (worst < frecency_count - 1)
    frecency_db[worst] = frecency_db[frecency_count - 1];
  frecency_count--;
}

/* Record a directory visit. */
void
frecency_update (const char *path)
{
  int i;

  if (path == NULL || *path == '\0')
    return;

  if (!frecency_initialized)
    frecency_init ();

  /* Look for existing entry */
  for (i = 0; i < frecency_count; i++)
    {
      if (strcmp (frecency_db[i].path, path) == 0)
	{
	  frecency_db[i].frequency++;
	  frecency_db[i].last_access = time (NULL);
	  frecency_save ();
	  return;
	}
    }

  /* New entry */
  if (frecency_count >= FRECENCY_MAX_ENTRIES)
    frecency_evict_lowest ();

  if (frecency_count >= frecency_capacity)
    {
      frecency_capacity *= 2;
      if (frecency_capacity > FRECENCY_MAX_ENTRIES)
	frecency_capacity = FRECENCY_MAX_ENTRIES;
      frecency_db = (frecency_entry_t *)xrealloc (frecency_db,
	  frecency_capacity * sizeof (frecency_entry_t));
    }

  frecency_db[frecency_count].path = savestring (path);
  frecency_db[frecency_count].frequency = 1;
  frecency_db[frecency_count].last_access = time (NULL);
  frecency_count++;

  frecency_save ();
}

/* Find the best frecency match for a partial directory query.
   Matches against the basename of stored paths.
   Returns a malloc'd full path, or NULL. */
char *
frecency_find (const char *query, int query_len)
{
  int i;
  double best_score, s;
  char *best_path;
  const char *basename;

  if (query == NULL || query_len <= 0 || frecency_count == 0)
    return (char *)NULL;

  if (!frecency_initialized)
    frecency_init ();

  best_score = -1.0;
  best_path = (char *)NULL;

  for (i = 0; i < frecency_count; i++)
    {
      /* Match against basename of the stored path */
      basename = strrchr (frecency_db[i].path, '/');
      basename = basename ? basename + 1 : frecency_db[i].path;

      if (strncmp (basename, query, query_len) == 0)
	{
	  s = frecency_score (&frecency_db[i]);
	  if (s > best_score)
	    {
	      best_score = s;
	      best_path = frecency_db[i].path;
	    }
	}
    }

  return best_path ? savestring (best_path) : (char *)NULL;
}

/* Find the Nth best frecency match (0-indexed) for a partial query.
   Returns a malloc'd full path, or NULL if fewer than N+1 matches. */
char *
frecency_find_nth (const char *query, int query_len, int n)
{
  int i, j, match_count;
  const char *basename;
  double s;

  /* Temporary arrays to hold matching indices and scores. */
  int *match_idx;
  double *match_scores;

  if (query == NULL || query_len <= 0 || frecency_count == 0 || n < 0)
    return (char *)NULL;

  if (!frecency_initialized)
    frecency_init ();

  /* First pass: count matches */
  match_count = 0;
  for (i = 0; i < frecency_count; i++)
    {
      basename = strrchr (frecency_db[i].path, '/');
      basename = basename ? basename + 1 : frecency_db[i].path;
      if (strncmp (basename, query, query_len) == 0)
	match_count++;
    }

  if (n >= match_count)
    return (char *)NULL;

  /* Second pass: collect matches with scores */
  match_idx = (int *)xmalloc (match_count * sizeof (int));
  match_scores = (double *)xmalloc (match_count * sizeof (double));

  j = 0;
  for (i = 0; i < frecency_count; i++)
    {
      basename = strrchr (frecency_db[i].path, '/');
      basename = basename ? basename + 1 : frecency_db[i].path;
      if (strncmp (basename, query, query_len) == 0)
	{
	  match_idx[j] = i;
	  match_scores[j] = frecency_score (&frecency_db[i]);
	  j++;
	}
    }

  /* Simple selection sort to find the Nth highest score.
     For small match sets this is fine. */
  for (i = 0; i <= n; i++)
    {
      int best = i;
      for (j = i + 1; j < match_count; j++)
	if (match_scores[j] > match_scores[best])
	  best = j;
      if (best != i)
	{
	  double tmp_s = match_scores[i];
	  int tmp_i = match_idx[i];
	  match_scores[i] = match_scores[best];
	  match_idx[i] = match_idx[best];
	  match_scores[best] = tmp_s;
	  match_idx[best] = tmp_i;
	}
    }

  {
    char *result = savestring (frecency_db[match_idx[n]].path);
    xfree (match_idx);
    xfree (match_scores);
    return result;
  }
}
