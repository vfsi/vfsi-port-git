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
int vfsi_dummy_open_mount(const char *a, const char *b, vfsi_fs **out) { RESOLVE(vfsi_dummy_open_mount); return fn(a,b,out); }
int vfsi_nfs_open_mount_export(const char *a,const char *b,const char *c,vfsi_fs **out) { RESOLVE(vfsi_nfs_open_mount_export); return fn(a,b,c,out); }
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
int vfsi_read_paths_with_limit(vfsi_fs *fs,const char *const *paths,size_t count,size_t limit,vfsi_read_paths_cb cb,void *data) { RESOLVE(vfsi_read_paths_with_limit); return fn(fs,paths,count,limit,cb,data); }
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
