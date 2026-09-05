#ifndef VFSI_H
#define VFSI_H

#include "object-file.h"

struct stat;

/*
 * Try to enumerate the loose objects in "objects_dir" through the vfsi C
 * API. Returns 0 when vfsi is not configured/available (the caller should
 * use the normal filesystem path), 1 when vfsi completed the iteration, and
 * -1 when vfsi was requested but failed.
 */
int vfsi_for_each_loose_file(const char *objects_dir,
			     const struct git_hash_algo *algop,
			     each_loose_object_fn obj_cb,
			     each_loose_cruft_fn cruft_cb,
			     each_loose_subdir_fn subdir_cb,
			     void *data);

/*
 * Serve a per-object lstat()/stat() from attributes already collected by a
 * vfsi loose-object scan. Returns 1 and fills "st" when the path is cached;
 * returns 0 when the caller should use the normal filesystem path.
 */
int vfsi_fill_stat(const char *path, struct stat *st);

/*
 * Return a malloc'd copy of a loose object's bytes that were prefetched by a
 * vfsi scan. Returns 1 when the path is cached, 0 otherwise.
 */
int vfsi_read_loose_object(const char *path, void **buf, unsigned long *size);

#endif
