#include <vfsi.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static void *library(void) {
    static void *handle;
    if (!handle) { handle = dlopen(getenv("VFSI_TEST_REAL_LIBRARY"), RTLD_NOW | RTLD_LOCAL); if (!handle) abort(); }
    return handle;
}
#define RESOLVE(name) __typeof__(&name) fn; do { void *sym = dlsym(library(), #name); if (!sym) abort(); memcpy(&fn, &sym, sizeof(fn)); } while (0)
uint32_t vfsi_abi_version(void) { return getenv("VFSI_TEST_BAD_ABI") ? 999 : VFSI_ABI_VERSION; }
void vfsi_free(vfsi_fs *fs) { RESOLVE(vfsi_free); fn(fs); }
int vfsi_dummy_open_mount(const char *a, const char *b, vfsi_fs **out) {
    if (getenv("VFSI_TEST_OPEN_FAIL")) { *out = NULL; return EIO; }
    RESOLVE(vfsi_dummy_open_mount); return fn(a,b,out);
}
int vfsi_nfs_open_mount_export(const char *a,const char *b,const char *c,vfsi_fs **out) {
    if (getenv("VFSI_TEST_OPEN_FAIL")) { *out = NULL; return EIO; }
    RESOLVE(vfsi_nfs_open_mount_export); return fn(a,b,c,out);
}
int vfsi_listdir(vfsi_fs *fs,const char *p,vfsi_listdir_cb cb,void *data) { RESOLVE(vfsi_listdir); return fn(fs,p,cb,data); }
#ifndef VFSI_TEST_NO_IDENTITY
struct vfsi_result vfsi_get_object_idv(vfsi_fs *fs,const vfsi_object_id_op *ops,size_t count,vfsi_result *results) {
    RESOLVE(vfsi_get_object_idv);
    struct vfsi_result result = fn(fs,ops,count,results);
    /* Preserve the existing invalid-metadata fallback regression after the
     * coherent identity query refreshes listing attributes. */
    for (size_t i=0;i<count;i++) {
	if (ops[i].attrs && getenv("VFSI_TEST_BAD_TIME")) ops[i].attrs->mtime_nsec=1000000000;
	if (getenv("VFSI_TEST_BAD_ID")) ops[i].out->version=999;
    }
    return result;
}
#endif
#ifndef VFSI_TEST_NO_PREFETCH
int vfsi_read_paths(vfsi_fs *fs,const char *const *paths,size_t count,vfsi_read_paths_cb cb,void *data) { RESOLVE(vfsi_read_paths); return fn(fs,paths,count,cb,data); }
struct read_forwarding { vfsi_read_paths_cb cb; void *data; };
static bool forward_read(const char *path, const unsigned char *bytes, size_t len, void *opaque) {
    struct read_forwarding *state = opaque;
    const char *trace = getenv("VFSI_TEST_CALL_LOG");
    if (trace) { FILE *file = fopen(trace,"a"); if (!file) abort();
	fprintf(file,"read-bytes %zu\n",len); fclose(file); }
    return state->cb(path,bytes,len,state->data);
}
int vfsi_read_paths_with_limit(vfsi_fs *fs,const char *const *paths,size_t count,size_t limit,vfsi_read_paths_cb cb,void *data) {
    RESOLVE(vfsi_read_paths_with_limit);
    struct read_forwarding state = {cb,data};
    return fn(fs,paths,count,limit,forward_read,&state);
}
#endif
struct forwarding {
    vfsi_indexed_listdir_cb callback;
    void *data;
};
static bool forward_entry(size_t index, const char *dir, const char *name, const vfsi_attrs *attrs, void *opaque) {
    struct forwarding *state = opaque;
    vfsi_attrs altered = *attrs;
    if (getenv("VFSI_TEST_BAD_TIME")) altered.mtime_nsec = 1000000000;
    return state->callback(getenv("VFSI_TEST_BAD_INDEX") ? SIZE_MAX : index,
			   dir, name, &altered, state->data);
}
int vfsi_listdirs(vfsi_fs *fs,const char *const *paths,size_t count,const vfsi_listing_options *options,vfsi_indexed_listdir_cb cb,void *data,vfsi_listing_result *results) {
    RESOLVE(vfsi_listdirs);
    struct forwarding state = { cb, data };
    int rc = fn(fs,paths,count,options,forward_entry,&state,results);
    const char *freshen = getenv("VFSI_TEST_FRESHEN");
    if (!rc && freshen && utimensat(AT_FDCWD, freshen, NULL, 0)) abort();
    if (getenv("VFSI_TEST_PARTIAL")) {
	for (size_t i=0;i<count;i++) results[i].completion=0;
	return EFBIG;
    }
    return rc;
}


#if defined(VFSI_TEST_CURSOR_API) && defined(VFSI_DIRECTORY_CURSOR_IDS_VERSION)
#ifndef VFSI_TEST_NO_CURSOR_FREE
void vfsi_dir_cursor_free(vfsi_dir_cursor *cursor) {
    RESOLVE(vfsi_dir_cursor_free); fn(cursor);
}
#endif
struct cursor_forwarding { vfsi_indexed_listdir_cursor_ids_cb cb; void *data; };
static bool forward_cursor(size_t index, const char *dir, const char *name,
			   const vfsi_attrs *attrs, const vfsi_object_id *id,
			   vfsi_dir_cursor *child, void *opaque) {
    struct cursor_forwarding *state = opaque;
    return state->cb(getenv("VFSI_TEST_BAD_INDEX") ? SIZE_MAX : index,
		     dir,name,attrs,id,child,state->data);
}
int vfsi_listdirs_from_cursors_with_ids(vfsi_fs *fs,const char *const *paths,
	vfsi_dir_cursor **cursors,size_t count,const vfsi_listing_options *options,
	vfsi_indexed_listdir_cursor_ids_cb cb,void *data,vfsi_listing_result *results) {
    RESOLVE(vfsi_listdirs_from_cursors_with_ids);
    static unsigned group;
    const char *name = strrchr(paths[0],'/');
    int fanout = name && strlen(name+1)==2;
    const char *trace = getenv("VFSI_TEST_CALL_LOG");
    if (fanout) {
	const char *failure = getenv("VFSI_TEST_GROUP_FAIL");
	group++;
	if (trace) { FILE *file=fopen(trace,"a"); if(!file) abort();
	    fprintf(file,"group %u %zu\n",group,count); fclose(file); }
	/* Fail preflight without consuming cursors: caller must release them. */
	if (failure && group==(unsigned)atoi(failure)) return EFBIG;
    }
    struct cursor_forwarding state={cb,data};
    return fn(fs,paths,cursors,count,options,forward_cursor,&state,results);
}
#endif
