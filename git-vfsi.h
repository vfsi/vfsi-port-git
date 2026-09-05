#ifndef GIT_VFSI_H
#define GIT_VFSI_H

#include "object-file.h"

struct stat;
struct odb_source;

#ifdef USE_VFSI

/*
 * Try to enumerate the loose objects in "objects_dir" through the vfsi C
 * API. Returns 0 when vfsi is not configured/available, 1 when vfsi handled
 * the traversal, and -1 on a vfsi transport/API failure. When it returns 1,
 * "result" contains the exact result from the object/cruft/subdirectory
 * callback (zero when the traversal completed).
 */
int vfsi_for_each_loose_file(struct odb_source *source,
			     const char *objects_dir,
			     const struct git_hash_algo *algop,
			     each_loose_object_fn obj_cb,
			     each_loose_cruft_fn cruft_cb,
			     each_loose_subdir_fn subdir_cb,
			     void *data, int *result);

int vfsi_fill_stat(struct odb_source *source, const char *path, struct stat *st);
int vfsi_read_loose_object(struct odb_source *source, const char *path,
			   void **buf, unsigned long *size);
void vfsi_source_close(struct odb_source *source);
void vfsi_source_invalidate(struct odb_source *source);
void vfsi_source_release(struct odb_source *source);

#else

static inline int vfsi_for_each_loose_file(struct odb_source *source,
					   const char *objects_dir,
					   const struct git_hash_algo *algop,
					   each_loose_object_fn obj_cb,
					   each_loose_cruft_fn cruft_cb,
					   each_loose_subdir_fn subdir_cb,
					   void *data, int *result)
{
	(void)source;
	(void)objects_dir;
	(void)algop;
	(void)obj_cb;
	(void)cruft_cb;
	(void)subdir_cb;
	(void)data;
	(void)result;
	return 0;
}

static inline int vfsi_fill_stat(struct odb_source *source, const char *path,
				 struct stat *st)
{
	(void)source;
	(void)path;
	(void)st;
	return 0;
}

static inline int vfsi_read_loose_object(struct odb_source *source,
					 const char *path, void **buf,
					 unsigned long *size)
{
	(void)source;
	(void)path;
	(void)buf;
	(void)size;
	return 0;
}

static inline void vfsi_source_close(struct odb_source *source)
{
	(void)source;
}

static inline void vfsi_source_invalidate(struct odb_source *source)
{
	(void)source;
}

static inline void vfsi_source_release(struct odb_source *source)
{
	(void)source;
}

#endif

#endif
