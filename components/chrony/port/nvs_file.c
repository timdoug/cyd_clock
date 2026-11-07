/*
 * Small files for chronyd stored as NVS blobs, mounted at CES_FILE_ROOT
 * through the ESP-IDF VFS. The device has no filesystem partition; this
 * gives chrony's "driftfile" (written through UTI_OpenFile() and
 * UTI_RenameTempFile() in util.c) the operations it uses: open with
 * O_CREAT|O_EXCL, read, write, lseek, fstat, close, stat, unlink and
 * rename. A file is held in RAM while open and written to NVS on close.
 * Only the chronyd task uses these files.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "nvs.h"

#include "nvs_file.h"

#define NAMESPACE "chronyfs"
#define MAX_OPEN  2
#define MAX_SIZE  1024

typedef struct {
  int used;
  int writable;
  char key[NVS_KEY_NAME_MAX_SIZE];
  char *data;
  size_t size;
  size_t pos;
} OpenFile;

static nvs_handle_t handle;
static OpenFile files[MAX_OPEN];

/* ================================================== */

/* "/name" -> "name"; NVS keys are at most 15 characters */
static int
path_to_key(const char *path, char *key)
{
  if (*path == '/')
    path++;
  if (!*path || strchr(path, '/') || strlen(path) >= NVS_KEY_NAME_MAX_SIZE) {
    errno = ENAMETOOLONG;
    return 0;
  }
  strcpy(key, path);
  return 1;
}

/* ================================================== */

/* Load a blob into a new heap buffer; returns 0 with errno set */
static int
load(const char *key, char **data, size_t *size)
{
  size_t len = 0;

  if (nvs_get_blob(handle, key, NULL, &len) != ESP_OK) {
    errno = ENOENT;
    return 0;
  }
  *data = malloc(len ? len : 1);
  if (!*data) {
    errno = ENOMEM;
    return 0;
  }
  if (nvs_get_blob(handle, key, *data, &len) != ESP_OK) {
    free(*data);
    errno = EIO;
    return 0;
  }
  *size = len;
  return 1;
}

/* ================================================== */

static int
store(const char *key, const char *data, size_t size)
{
  if (nvs_set_blob(handle, key, data, size) != ESP_OK ||
      nvs_commit(handle) != ESP_OK) {
    errno = EIO;
    return 0;
  }
  return 1;
}

/* ================================================== */

static int
exists(const char *key)
{
  size_t len = 0;

  return nvs_get_blob(handle, key, NULL, &len) == ESP_OK;
}

/* ================================================== */

static int
file_open(void *ctx, const char *path, int flags, int mode)
{
  OpenFile *f = NULL;
  char key[NVS_KEY_NAME_MAX_SIZE];
  int fd, acc = flags & O_ACCMODE;

  if (!path_to_key(path, key))
    return -1;

  for (fd = 0; fd < MAX_OPEN; fd++) {
    if (!files[fd].used) {
      f = &files[fd];
      break;
    }
  }
  if (!f) {
    errno = ENFILE;
    return -1;
  }
  memset(f, 0, sizeof (*f));
  strcpy(f->key, key);

  if (acc == O_RDONLY) {
    if (!load(key, &f->data, &f->size))
      return -1;
  } else {
    if ((flags & O_CREAT) && (flags & O_EXCL) && exists(key)) {
      errno = EEXIST;
      return -1;
    }
    if (!(flags & O_CREAT) && !exists(key)) {
      errno = ENOENT;
      return -1;
    }
    if (!(flags & O_TRUNC) && exists(key) && !load(key, &f->data, &f->size))
      return -1;
    if (flags & O_APPEND)
      f->pos = f->size;
    f->writable = 1;
  }

  f->used = 1;
  return fd;
}

/* ================================================== */

static ssize_t
file_read(void *ctx, int fd, void *dst, size_t size)
{
  OpenFile *f = &files[fd];
  size_t n;

  if (f->pos >= f->size)
    return 0;
  n = f->size - f->pos;
  if (n > size)
    n = size;
  memcpy(dst, f->data + f->pos, n);
  f->pos += n;
  return n;
}

/* ================================================== */

static ssize_t
file_write(void *ctx, int fd, const void *src, size_t size)
{
  OpenFile *f = &files[fd];
  char *data;

  if (!f->writable) {
    errno = EBADF;
    return -1;
  }
  if (f->pos + size > MAX_SIZE) {
    errno = EFBIG;
    return -1;
  }
  if (f->pos + size > f->size) {
    data = realloc(f->data, f->pos + size);
    if (!data) {
      errno = ENOMEM;
      return -1;
    }
    f->data = data;
    f->size = f->pos + size;
  }
  memcpy(f->data + f->pos, src, size);
  f->pos += size;
  return size;
}

/* ================================================== */

static off_t
file_lseek(void *ctx, int fd, off_t offset, int whence)
{
  OpenFile *f = &files[fd];
  off_t base;

  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = f->pos; break;
    case SEEK_END: base = f->size; break;
    default:
      errno = EINVAL;
      return -1;
  }
  if (base + offset < 0 || base + offset > MAX_SIZE) {
    errno = EINVAL;
    return -1;
  }
  f->pos = base + offset;
  return f->pos;
}

/* ================================================== */

static int
file_fstat(void *ctx, int fd, struct stat *st)
{
  memset(st, 0, sizeof (*st));
  st->st_mode = S_IFREG | 0644;
  st->st_size = files[fd].size;
  return 0;
}

/* ================================================== */

static int
file_close(void *ctx, int fd)
{
  OpenFile *f = &files[fd];
  int r = 0;

  if (f->writable && !store(f->key, f->data ? f->data : "", f->size))
    r = -1;
  free(f->data);
  memset(f, 0, sizeof (*f));
  return r;
}

/* ================================================== */

static int
file_stat(void *ctx, const char *path, struct stat *st)
{
  char key[NVS_KEY_NAME_MAX_SIZE];
  size_t len = 0;

  if (!path_to_key(path, key))
    return -1;
  if (nvs_get_blob(handle, key, NULL, &len) != ESP_OK) {
    errno = ENOENT;
    return -1;
  }
  memset(st, 0, sizeof (*st));
  st->st_mode = S_IFREG | 0644;
  st->st_size = len;
  return 0;
}

/* ================================================== */

static int
file_unlink(void *ctx, const char *path)
{
  char key[NVS_KEY_NAME_MAX_SIZE];

  if (!path_to_key(path, key))
    return -1;
  if (nvs_erase_key(handle, key) != ESP_OK) {
    errno = ENOENT;
    return -1;
  }
  nvs_commit(handle);
  return 0;
}

/* ================================================== */

static int
file_rename(void *ctx, const char *src, const char *dst)
{
  char src_key[NVS_KEY_NAME_MAX_SIZE], dst_key[NVS_KEY_NAME_MAX_SIZE];
  char *data;
  size_t size;
  int ok;

  if (!path_to_key(src, src_key) || !path_to_key(dst, dst_key))
    return -1;
  if (!load(src_key, &data, &size))
    return -1;
  ok = store(dst_key, data, size);
  free(data);
  if (!ok)
    return -1;
  nvs_erase_key(handle, src_key);
  nvs_commit(handle);
  return 0;
}

/* ================================================== */

static const esp_vfs_dir_ops_t dir_ops = {
  .stat_p = file_stat,
  .unlink_p = file_unlink,
  .rename_p = file_rename,
};

static const esp_vfs_fs_ops_t fs_ops = {
  .open_p = file_open,
  .read_p = file_read,
  .write_p = file_write,
  .lseek_p = file_lseek,
  .fstat_p = file_fstat,
  .close_p = file_close,
  .dir = &dir_ops,
};

/* ================================================== */

int
CES_MountFiles(void)
{
  if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return 0;
  return esp_vfs_register_fs(CES_FILE_ROOT, &fs_ops,
                             ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC,
                             NULL) == ESP_OK;
}
