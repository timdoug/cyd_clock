/*
 * glob() stub for chrony on ESP-IDF: there is no filesystem, so every
 * pattern ("confdir", "sourcedir", "include") matches nothing.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHRONY_ESP_GLOB_H
#define CHRONY_ESP_GLOB_H

#include <stddef.h>

typedef struct {
  size_t gl_pathc;
  char **gl_pathv;
} glob_t;

#define GLOB_ERR     (1 << 0)
#define GLOB_NOSORT  (1 << 1)
#define GLOB_APPEND  (1 << 2)
#define GLOB_NOMATCH 3

static inline int glob(const char *pattern, int flags,
                       int (*errfunc)(const char *, int), glob_t *pglob)
{
  (void)pattern;
  (void)errfunc;
  if (!(flags & GLOB_APPEND)) {
    pglob->gl_pathc = 0;
    pglob->gl_pathv = NULL;
  }
  return GLOB_NOMATCH;
}

static inline void globfree(glob_t *pglob)
{
  (void)pglob;
}

#endif
