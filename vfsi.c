#include "git-compat-util.h"
#include "abspath.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "odb/source-files.h"
#include "odb/source-loose.h"
#include "strbuf.h"
#include "strmap.h"
#include "strvec.h"
#include "thread-utils.h"
#include "git-vfsi.h"
#include <vfsi.h>

#include <dlfcn.h>

struct vfsi_bindings {
	void *handle;
	uint32_t (*abi_version)(void);
	int (*dummy_open_mount)(const char *, const char *, struct vfsi_fs **);
	int (*nfs_open_mount_export)(const char *, const char *, const char *,
				     struct vfsi_fs **);
	int (*listdir)(struct vfsi_fs *, const char *, vfsi_listdir_cb, void *);
	int (*listdirv)(struct vfsi_fs *, const char *const *, size_t,
			size_t, bool, vfsi_listdirv_cb, void *);
	int (*read_paths)(struct vfsi_fs *, const char *const *, size_t,
			  vfsi_read_paths_cb, void *);
	void (*free)(struct vfsi_fs *);
};

enum vfsi_loader_state {
	VFSI_LOADER_UNINITIALIZED,
	VFSI_LOADER_AVAILABLE,
	VFSI_LOADER_UNAVAILABLE,
};

static struct {
	struct vfsi_bindings bindings;
	enum vfsi_loader_state loader_state;
	pthread_mutex_t mutex;
} vfsi_runtime = { .mutex = PTHREAD_MUTEX_INITIALIZER };

enum {
	VFSI_NF4DIR = 2,
	VFSI_NF4REG = 1,
	VFSI_NF4LNK = 5
};

struct vfsi_object {
	char *display_dir;
	char *display_path;
	char *name;
	char *real_path;
	struct vfsi_attrs attrs;
	unsigned char *data;
	size_t data_len;
};

struct vfsi_source_context {
	struct vfsi_fs *fs;
	char *mountpoint;
	struct vfsi_object *objects;
	size_t objects_nr;
	size_t objects_cap;
	struct strmap objects_by_path;
	pthread_mutex_t mutex;
	int traversal_active;
};

static int vfsi_prefetch_enabled(void)
{
	const char *value = getenv("VFSI_PREFETCH_OBJECTS");

	return value && *value && strcmp(value, "0");
}

static void vfsi_clear_objects(struct vfsi_source_context *ctx)
{
	size_t i;

	strmap_clear(&ctx->objects_by_path, 0);
	for (i = 0; i < ctx->objects_nr; i++) {
		free(ctx->objects[i].display_dir);
		free(ctx->objects[i].display_path);
		free(ctx->objects[i].name);
		free(ctx->objects[i].real_path);
		free(ctx->objects[i].data);
	}
	ctx->objects_nr = 0;
}

static struct odb_source_loose *vfsi_loose_source(struct odb_source *source)
{
	if (!source)
		return NULL;
	if (source->type == ODB_SOURCE_FILES)
		return odb_source_files_downcast(source)->loose;
	if (source->type == ODB_SOURCE_LOOSE)
		return odb_source_loose_downcast(source);
	return NULL;
}

static struct vfsi_source_context *vfsi_source_context(struct odb_source *source,
							int create)
{
	struct odb_source_loose *loose = vfsi_loose_source(source);
	struct vfsi_source_context *ctx;

	if (!loose)
		return NULL;
	pthread_mutex_lock(&vfsi_runtime.mutex);
	ctx = loose->vfsi;
	if (!ctx && create) {
		CALLOC_ARRAY(ctx, 1);
		strmap_init(&ctx->objects_by_path);
		if (init_recursive_mutex(&ctx->mutex))
			die_errno(_("vfsi: cannot initialize source mutex"));
		loose->vfsi = ctx;
	}
	pthread_mutex_unlock(&vfsi_runtime.mutex);
	return ctx;
}

void vfsi_source_invalidate(struct odb_source *source)
{
	struct vfsi_source_context *ctx = vfsi_source_context(source, 0);

	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->mutex);
	/* Callback replay owns a stable snapshot and clears it before returning. */
	if (!ctx->traversal_active)
		vfsi_clear_objects(ctx);
	pthread_mutex_unlock(&ctx->mutex);
}

void vfsi_source_close(struct odb_source *source)
{
	struct vfsi_source_context *ctx = vfsi_source_context(source, 0);

	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->mutex);
	vfsi_clear_objects(ctx);
	if (ctx->fs && vfsi_runtime.bindings.free)
		vfsi_runtime.bindings.free(ctx->fs);
	ctx->fs = NULL;
	free(ctx->mountpoint);
	ctx->mountpoint = NULL;
	pthread_mutex_unlock(&ctx->mutex);
}

void vfsi_source_release(struct odb_source *source)
{
	struct odb_source_loose *loose = vfsi_loose_source(source);
	struct vfsi_source_context *ctx;

	if (!loose || !loose->vfsi)
		return;
	ctx = loose->vfsi;
	vfsi_source_close(source);
	free(ctx->objects);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
	loose->vfsi = NULL;
}

static int load_symbol(void *handle, const char *name, void **out)
{
	void *sym = dlsym(handle, name);

	if (!sym) {
		warning(_("vfsi: cannot load %s: %s"), name, dlerror());
		return -1;
	}
	*out = sym;
	return 0;
}

static int load_vfsi_bindings(const char *library)
{
	struct vfsi_bindings candidate = { 0 };
	uint32_t version;
	int result;

	pthread_mutex_lock(&vfsi_runtime.mutex);
	if (vfsi_runtime.loader_state == VFSI_LOADER_AVAILABLE) {
		result = 1;
		goto out;
	}
	if (vfsi_runtime.loader_state == VFSI_LOADER_UNAVAILABLE) {
		result = 0;
		goto out;
	}

	candidate.handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
	if (!candidate.handle) {
		warning(_("vfsi: cannot load %s: %s"), library, dlerror());
		goto unavailable;
	}
#define LOAD_VFSI(name, field) do { \
	void *sym; \
	if (load_symbol(candidate.handle, name, &sym) < 0) \
		goto unavailable; \
	memcpy(&(field), &sym, sizeof(sym)); \
} while (0)
	LOAD_VFSI("vfsi_abi_version", candidate.abi_version);
	version = candidate.abi_version();
	if (version != VFSI_ABI_VERSION) {
		warning(_("vfsi: ABI version %"PRIu32" is incompatible with required version %u"),
			version, VFSI_ABI_VERSION);
		goto unavailable;
	}
	LOAD_VFSI("vfsi_dummy_open_mount", candidate.dummy_open_mount);
	LOAD_VFSI("vfsi_nfs_open_mount_export", candidate.nfs_open_mount_export);
	LOAD_VFSI("vfsi_listdir", candidate.listdir);
	LOAD_VFSI("vfsi_listdirv", candidate.listdirv);
	LOAD_VFSI("vfsi_read_paths", candidate.read_paths);
	LOAD_VFSI("vfsi_free", candidate.free);
#undef LOAD_VFSI

	vfsi_runtime.bindings = candidate;
	vfsi_runtime.loader_state = VFSI_LOADER_AVAILABLE;
	result = 1;
	goto out;

unavailable:
	if (candidate.handle)
		dlclose(candidate.handle);
	vfsi_runtime.loader_state = VFSI_LOADER_UNAVAILABLE;
	result = 0;
out:
	pthread_mutex_unlock(&vfsi_runtime.mutex);
	return result;
}

struct vfsi_mount {
	char *mountpoint;
	char *host;
	char *export_root;
};

static void vfsi_mount_clear(struct vfsi_mount *mount)
{
	free(mount->mountpoint);
	free(mount->host);
	free(mount->export_root);
	memset(mount, 0, sizeof(*mount));
}

#ifdef __linux__
static void unescape_mount_field(char *field)
{
	char *src = field;
	char *dst = field;

	while (*src) {
		if (src[0] == '\\' && src[1] >= '0' && src[1] <= '7' &&
		    src[2] >= '0' && src[2] <= '7' &&
		    src[3] >= '0' && src[3] <= '7') {
			*dst++ = ((src[1] - '0') << 6) |
				 ((src[2] - '0') << 3) | (src[3] - '0');
			src += 4;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';
}
#endif

static int vfsi_mount_for(const char *path, struct vfsi_mount *result)
{
#ifdef __linux__
	FILE *fp;
	char line[8192];
	size_t best_len = 0;

	fp = fopen("/proc/self/mounts", "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		char source[4096], mountpoint[4096], fstype[64];
		char *export_sep;
		size_t len;

		if (sscanf(line, "%4095s %4095s %63s", source, mountpoint,
			   fstype) != 3)
			continue;
		if (strcmp(fstype, "nfs") && strcmp(fstype, "nfs4"))
			continue;
		unescape_mount_field(source);
		unescape_mount_field(mountpoint);
		len = strlen(mountpoint);
		if (len <= best_len || !starts_with(path, mountpoint) ||
		    (len != 1 && path[len] != '/' && path[len] != '\0'))
			continue;
		export_sep = strstr(source, ":/");
		if (!export_sep)
			continue;
		vfsi_mount_clear(result);
		result->mountpoint = xstrdup(mountpoint);
		result->host = xmemdupz(source, export_sep - source);
		result->export_root = xstrdup(export_sep + 1);
		best_len = len;
	}
	fclose(fp);
	return result->mountpoint != NULL;
#else
	(void)path;
	(void)result;
	return 0;
#endif
}

static int open_vfsi(struct vfsi_source_context *ctx, const char *objects_path)
{
	struct vfsi_bindings *b = &vfsi_runtime.bindings;
	const char *impl = getenv("VFSI_IMPL");
	const char *library = getenv("VFSI_LIBRARY");
	struct vfsi_mount mount = { 0 };
	const char *host_override;
	const char *export_override;
	const char *mount_override;
	const char *dummy_root;
	const char *dummy_mount;
	int rc;

	if (!impl || !strcmp(impl, "off") || !library)
		return 0;
	if (strcmp(impl, "nfs") && strcmp(impl, "dummy"))
		return 0;

	if (!load_vfsi_bindings(library))
		return 0;

	if (!strcmp(impl, "dummy")) {
		dummy_root = getenv("VFSI_ROOT");
		dummy_mount = getenv("VFSI_MOUNT");
		if (!dummy_root || !dummy_mount)
			return 0;
		if (ctx->fs) {
			b->free(ctx->fs);
			ctx->fs = NULL;
		}
		free(ctx->mountpoint);
		ctx->mountpoint = xstrdup(dummy_mount);
		rc = b->dummy_open_mount(dummy_root, dummy_mount, &ctx->fs);
	} else {
		host_override = getenv("VFSI_HOST");
		export_override = getenv("VFSI_EXPORT");
		mount_override = getenv("VFSI_MOUNT");
		if (!vfsi_mount_for(objects_path, &mount)) {
			if (!mount_override || !export_override)
				return 0;
			mount.mountpoint = xstrdup(mount_override);
			mount.export_root = xstrdup(export_override);
			mount.host = xstrdup(host_override ? host_override : "127.0.0.1");
		}
		if (host_override) {
			free(mount.host);
			mount.host = xstrdup(host_override);
		}
		if (export_override) {
			free(mount.export_root);
			mount.export_root = xstrdup(export_override);
		}
		if (mount_override) {
			free(mount.mountpoint);
			mount.mountpoint = xstrdup(mount_override);
		}
		if (ctx->fs) {
			b->free(ctx->fs);
			ctx->fs = NULL;
		}
		free(ctx->mountpoint);
		ctx->mountpoint = xstrdup(mount.mountpoint);
		rc = b->nfs_open_mount_export(mount.host, mount.export_root,
					      mount.mountpoint, &ctx->fs);
		vfsi_mount_clear(&mount);
	}
	if (rc) {
		warning(_("vfsi: open failed: %d"), rc);
		return -1;
	}
	return 1;
}

struct vfsi_walk {
	struct vfsi_source_context *ctx;
	const struct git_hash_algo *algop;
	each_loose_object_fn *obj_cb;
	each_loose_cruft_fn *cruft_cb;
	each_loose_subdir_fn *subdir_cb;
	void *data;
	struct strvec subdirs;
	struct strvec display_subdirs;
	const char *display_root;
	const char *real_root;
};

static int is_hexpair(const char *s)
{
	return isxdigit((unsigned char)s[0]) &&
	       isxdigit((unsigned char)s[1]) && s[2] == '\0';
}

static int cmp_subdir_ptr(const void *a, const void *b)
{
	const char *const *pa = a;
	const char *const *pb = b;

	return strcmp(*pa, *pb);
}

static bool collect_subdir_cb(const char *name, const struct vfsi_attrs *attrs,
			      void *userdata)
{
	struct vfsi_walk *walk = userdata;

	if (attrs->ftype != VFSI_NF4DIR || !is_hexpair(name))
		return true;
	strvec_pushf(&walk->subdirs, "%s/%s", walk->real_root, name);
	strvec_pushf(&walk->display_subdirs, "%s/%s",
		     walk->display_root, name);
	return true;
}

static int process_loose_entry(struct vfsi_walk *walk,
			       const char *dir, const char *name)
{
	struct object_id oid;
	const struct git_hash_algo *algop = walk->algop;
	size_t namelen = strlen(name);
	char *path;
	int rc = 0;

	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return 0;
	path = xstrfmt("%s/%s", dir, name);
	if (namelen == algop->hexsz - 2) {
		unsigned subdir_nr = (unsigned)hexval_table[(unsigned char)dir[strlen(dir) - 2]];

		subdir_nr = (subdir_nr << 4) |
			    hexval_table[(unsigned char)dir[strlen(dir) - 1]];
		oid.hash[0] = subdir_nr;
		if (!hex_to_bytes(oid.hash + 1, name, algop->rawsz - 1)) {
			oid_set_algo(&oid, algop);
			memset(oid.hash + algop->rawsz, 0,
			       GIT_MAX_RAWSZ - algop->rawsz);
			if (walk->obj_cb)
				rc = walk->obj_cb(&oid, path, walk->data);
		} else if (walk->cruft_cb) {
			rc = walk->cruft_cb(name, path, walk->data);
		}
	} else if (walk->cruft_cb) {
		rc = walk->cruft_cb(name, path, walk->data);
	}
	free(path);
	return rc;
}

static const char *display_dir_for_real(struct vfsi_walk *walk,
					const char *dir)
{
	size_t i;

	for (i = 0; i < walk->subdirs.nr; i++)
		if (!strcmp(walk->subdirs.v[i], dir))
			return walk->display_subdirs.v[i];
	return NULL;
}

static struct vfsi_object *vfsi_object_add(struct vfsi_source_context *ctx,
					   const char *display_dir,
					   const char *display_path,
					   const char *real_dir,
					   const char *name,
					   const struct vfsi_attrs *attrs)
{
	struct vfsi_object *obj;
	size_t index = ctx->objects_nr;
	void *index_value;

	ALLOC_GROW(ctx->objects, ctx->objects_nr + 1, ctx->objects_cap);
	obj = &ctx->objects[ctx->objects_nr++];
	memset(obj, 0, sizeof(*obj));
	obj->display_dir = xstrdup(display_dir);
	obj->display_path = xstrdup(display_path);
	obj->name = xstrdup(name);
	obj->real_path = xstrfmt("%s/%s", real_dir, name);
	obj->attrs = *attrs;
	index_value = (void *)(uintptr_t)(index + 1);
	strmap_put(&ctx->objects_by_path, obj->display_path, index_value);
	strmap_put(&ctx->objects_by_path, obj->real_path, index_value);
	return obj;
}

static bool loose_entry_cb(const char *dir, const char *name,
			   const struct vfsi_attrs *attrs,
			   void *userdata)
{
	struct vfsi_walk *walk = userdata;
	const char *display_dir = display_dir_for_real(walk, dir);
	char *display_path;

	if (!display_dir)
		return true;
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return true;
	display_path = xstrfmt("%s/%s", display_dir, name);
	vfsi_object_add(walk->ctx, display_dir, display_path, dir, name, attrs);
	free(display_path);
	return true;
}

static struct vfsi_object *vfsi_find_object(struct vfsi_source_context *ctx,
					    const char *path)
{
	void *index_value = strmap_get(&ctx->objects_by_path, path);
	size_t index;

	if (!index_value)
		return NULL;
	index = (uintptr_t)index_value - 1;
	if (index >= ctx->objects_nr)
		BUG("vfsi path index is out of bounds");
	return &ctx->objects[index];
}

static bool vfsi_store_read_paths_cb(const char *path,
				     const unsigned char *data,
				     size_t len, void *userdata)
{
	struct vfsi_source_context *ctx = userdata;
	struct vfsi_object *obj = vfsi_find_object(ctx, path);
	if (!obj)
		return true;
	free(obj->data);
	obj->data = xmalloc(len ? len : 1);
	memcpy(obj->data, data, len);
	obj->data_len = len;
	return true;
}

static int vfsi_prefetch_object_data(struct vfsi_source_context *ctx)
{
	const char **paths;
	size_t i;
	size_t next = 0;
	size_t chunk;
	int rc = 0;

	if (!ctx->objects_nr)
		return 0;
	paths = xcalloc(ctx->objects_nr, sizeof(*paths));
	for (i = 0; i < ctx->objects_nr; i++)
		paths[next++] = ctx->objects[i].real_path;
	for (chunk = 0; chunk < next; chunk += 4096) {
		size_t count = next - chunk < 4096 ? next - chunk : 4096;

		rc = vfsi_runtime.bindings.read_paths(ctx->fs,
						  (const char *const *)&paths[chunk],
						  count, vfsi_store_read_paths_cb,
						  ctx);
		if (rc)
			break;
	}
	free(paths);
	return rc;
}

static void vfsi_attrs_to_stat(const struct vfsi_attrs *attrs, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_ino = attrs->fileid;
	st->st_mode = attrs->mode & 07777;
	if (attrs->ftype == VFSI_NF4DIR)
		st->st_mode |= S_IFDIR;
	else if (attrs->ftype == VFSI_NF4LNK)
		st->st_mode |= S_IFLNK;
	else
		st->st_mode |= S_IFREG;
	st->st_nlink = attrs->nlink;
	st->st_uid = attrs->uid;
	st->st_gid = attrs->gid;
	st->st_size = attrs->size;
	st->st_blocks = attrs->blocks;
	st->st_atime = attrs->atime_sec;
	st->st_mtime = attrs->mtime_sec;
	st->st_ctime = attrs->ctime_sec;
}

int vfsi_fill_stat(struct odb_source *source, const char *path, struct stat *st)
{
	struct vfsi_source_context *ctx = vfsi_source_context(source, 0);
	struct vfsi_object *obj;

	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->mutex);
	obj = vfsi_find_object(ctx, path);
	if (!obj) {
		pthread_mutex_unlock(&ctx->mutex);
		return 0;
	}
	vfsi_attrs_to_stat(&obj->attrs, st);
	pthread_mutex_unlock(&ctx->mutex);
	return 1;
}

int vfsi_read_loose_object(struct odb_source *source, const char *path,
			   void **buf, unsigned long *size)
{
	struct vfsi_source_context *ctx = vfsi_source_context(source, 0);
	struct vfsi_object *obj;

	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->mutex);
	obj = vfsi_find_object(ctx, path);
	if (!obj || !obj->data) {
		pthread_mutex_unlock(&ctx->mutex);
		return 0;
	}
	*buf = xmalloc(obj->data_len ? obj->data_len : 1);
	memcpy(*buf, obj->data, obj->data_len);
	*size = obj->data_len;
	pthread_mutex_unlock(&ctx->mutex);
	return 1;
}

static int vfsi_for_each_loose_file_locked(struct vfsi_source_context *ctx,
					   const char *objects_dir,
					   const struct git_hash_algo *algop,
					   each_loose_object_fn obj_cb,
					   each_loose_cruft_fn cruft_cb,
					   each_loose_subdir_fn subdir_cb,
					   void *data, int *result)
{
	struct vfsi_walk walk = {
		.ctx = ctx,
		.algop = algop,
		.obj_cb = obj_cb,
		.cruft_cb = cruft_cb,
		.subdir_cb = subdir_cb,
		.data = data,
		.display_root = objects_dir,
	};
	char *real_objects;
	size_t i;
	const char **dirs = NULL;
	int rc;

	*result = 0;

	real_objects = real_pathdup(objects_dir, 0);
	if (!real_objects)
		return 0;
	vfsi_clear_objects(ctx);
	walk.real_root = real_objects;
	strvec_init(&walk.subdirs);
	strvec_init(&walk.display_subdirs);
	rc = open_vfsi(ctx, real_objects);
	if (rc <= 0) {
		strvec_clear(&walk.subdirs);
		strvec_clear(&walk.display_subdirs);
		free(real_objects);
		return rc;
	}

	rc = vfsi_runtime.bindings.listdir(ctx->fs, real_objects,
				       collect_subdir_cb, &walk);
	if (rc) {
		error(_("vfsi: unable to list %s"), real_objects);
		goto fail;
	}
	if (!walk.subdirs.nr)
		goto done;

	dirs = xcalloc(walk.subdirs.nr, sizeof(*dirs));
	for (i = 0; i < walk.subdirs.nr; i++)
		dirs[i] = walk.subdirs.v[i];
	/* Match the normal implementation's numeric subdirectory order
	 * (objects/00 … objects/ff), which fsck relies on when it learns
	 * object types while scanning. */
	qsort(dirs, walk.subdirs.nr, sizeof(*dirs), cmp_subdir_ptr);
	rc = vfsi_runtime.bindings.listdirv(ctx->fs, dirs, walk.subdirs.nr,
					0, 0, loose_entry_cb, &walk);
	if (rc) {
		error(_("vfsi: listdirv failed"));
		goto fail;
	}
	if (vfsi_prefetch_enabled() && vfsi_prefetch_object_data(ctx) < 0) {
		error(_("vfsi: unable to prefetch loose objects"));
		goto fail;
	}
	/* Replay each directory in numeric order. Within a directory, retain the
	 * backend's READDIR order, and invoke subdir_cb immediately after its
	 * entries, matching for_each_file_in_obj_subdir(). */
	for (i = 0; i < walk.subdirs.nr; i++) {
		const char *dir = display_dir_for_real(&walk, dirs[i]);
		const char *slash = strrchr(dir, '/');
		size_t j;
		unsigned nr;

		for (j = 0; j < ctx->objects_nr; j++) {
			struct vfsi_object *obj = &ctx->objects[j];

			if (strcmp(obj->display_dir, dir))
				continue;
			*result = process_loose_entry(&walk, obj->display_dir,
						      obj->name);
			if (*result)
				goto handled;
		}
		if (!slash || !subdir_cb)
			continue;
		nr = (unsigned)hexval_table[(unsigned char)slash[1]];
		nr = (nr << 4) | hexval_table[(unsigned char)slash[2]];
		*result = subdir_cb(nr, dir, data);
		if (*result)
			goto handled;
	}

handled:
done:
	rc = 1;
	goto out;
fail:
	rc = -1;
out:
	strvec_clear(&walk.subdirs);
	strvec_clear(&walk.display_subdirs);
	free(real_objects);
	free(dirs);
	return rc;
}

int vfsi_for_each_loose_file(struct odb_source *source,
			     const char *objects_dir,
			     const struct git_hash_algo *algop,
			     each_loose_object_fn obj_cb,
			     each_loose_cruft_fn cruft_cb,
			     each_loose_subdir_fn subdir_cb,
			     void *data, int *result)
{
	struct vfsi_source_context *ctx;
	int rc;

	if (!result)
		BUG("vfsi_for_each_loose_file requires a result pointer");
	ctx = vfsi_source_context(source, 1);
	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->traversal_active) {
		pthread_mutex_unlock(&ctx->mutex);
		return 0;
	}
	ctx->traversal_active = 1;
	rc = vfsi_for_each_loose_file_locked(ctx, objects_dir, algop,
					    obj_cb, cruft_cb,
					    subdir_cb, data, result);
	ctx->traversal_active = 0;
	/* Cached entries exist only to serve singular calls made by callbacks. */
	vfsi_clear_objects(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}
