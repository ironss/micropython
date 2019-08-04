/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Damien P. George
 * Copyright (c) 2016 Paul Sokolovsky
 * Copyright (c) 2019 Brush Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/mpconfig.h"
#if MICROPY_VFS_LITTLEFS

#if !MICROPY_VFS
#error "with MICROPY_VFS_LITTLEFS enabled, must also enable MICROPY_VFS"
#endif

#include <string.h>
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/objarray.h"
#include "py/binary.h"

#include "extmod/vfs_littlefs.h"
#include "lib/littlefs/lfs.h"

#include "py/lexer.h"
#include "py/obj.h"
#include "extmod/vfs.h"

#include "py/stream.h"


typedef struct _fs_user_mount_t {
    mp_obj_base_t base;
    uint16_t flags;
    mp_obj_t read[5];
    mp_obj_t write[5];
    mp_obj_t erase[3];
    // new protocol uses just ioctl, old uses sync (optional) and count
    union {
        mp_obj_t ioctl[4];
        struct {
            mp_obj_t sync[2];
            mp_obj_t count[2];
        } old;
    } u;
    lfs_size_t block_size;
    lfs_size_t block_count;
    lfs_size_t start_block;
    lfs_t lfs;
    struct lfs_config lfs_config;
} fs_user_mount_t;
#define mp_obj_littlefs_vfs_t fs_user_mount_t

#define FSUSER_NATIVE       (0x0001) // readblocks[2]/writeblocks[2] contain native func
#define FSUSER_FREE_OBJ     (0x0002) // fs_user_mount_t obj should be freed on umount
#define FSUSER_HAVE_IOCTL   (0x0004) // new protocol with ioctl
#define FSUSER_NO_FILESYSTEM (0x0008) // the block device has no filesystem on it


// IO operations
// TODO: These functions should go into vfs_littlefs_io_fdev.c

int lfs_io_fdev_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    fs_user_mount_t *vfs = c->context;
    mp_obj_array_t ar = {{&mp_type_bytearray}, BYTEARRAY_TYPECODE, 0, size, buffer};
    vfs->read[2] = MP_OBJ_NEW_SMALL_INT(block);
    vfs->read[3] = MP_OBJ_NEW_SMALL_INT(off);
    vfs->read[4] = MP_OBJ_FROM_PTR(&ar);
    mp_call_method_n_kw(3, 0, vfs->read);
    return LFS_ERR_OK;
}

int lfs_io_fdev_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    fs_user_mount_t *vfs = c->context;
    mp_obj_array_t ar = {{&mp_type_bytearray}, BYTEARRAY_TYPECODE, 0, size, (void *)buffer};
    vfs->write[2] = MP_OBJ_NEW_SMALL_INT(block);
    vfs->write[3] = MP_OBJ_NEW_SMALL_INT(off);
    vfs->write[4] = MP_OBJ_FROM_PTR(&ar);
    mp_call_method_n_kw(3, 0, vfs->write);
    return LFS_ERR_OK;
}

 int lfs_io_fdev_erase(const struct lfs_config *c, lfs_block_t block)
 {
     fs_user_mount_t *vfs = c->context;
     vfs->erase[2] = MP_OBJ_NEW_SMALL_INT(block);
     mp_call_method_n_kw(1, 0, vfs->erase);
     return LFS_ERR_OK;
 }

int lfs_io_fdev_sync(const struct lfs_config *c)
{
    return LFS_ERR_OK;
}


// Mapping from LFS error to POSIX errno
static int lfserr_to_errno(int lfserr) {
    return -lfserr;
}


STATIC mp_import_stat_t littlefs_vfs_import_stat(void *vfs_in, const char *path) {
    fs_user_mount_t *vfs = vfs_in;

    int err;
    struct lfs_info info;
    
    err = lfs_stat(&vfs->lfs, path, &info);
    if (err != LFS_ERR_OK) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    if (info.type == LFS_TYPE_REG) {
        return MP_IMPORT_STAT_FILE;
    } else {
        return MP_IMPORT_STAT_DIR;
    }

    return MP_IMPORT_STAT_NO_EXIST;
}


STATIC mp_obj_t littlefs_vfs_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    // create new object
    fs_user_mount_t *vfs = m_new_obj(fs_user_mount_t);
    vfs->base.type = type;
    vfs->flags = FSUSER_FREE_OBJ;

    // load block protocol methods
    mp_load_method(args[0], MP_QSTR_read, vfs->read);
    mp_load_method_maybe(args[0], MP_QSTR_write, vfs->write);
    mp_load_method_maybe(args[0], MP_QSTR_erase, vfs->erase);
    mp_load_method_maybe(args[0], MP_QSTR_ioctl, vfs->u.ioctl);
    if (vfs->u.ioctl[0] != MP_OBJ_NULL) {
        // device supports new block protocol, so indicate it
        vfs->flags |= FSUSER_HAVE_IOCTL;
    } else {
        // no ioctl method, so assume the device uses the old block protocol
        mp_load_method_maybe(args[0], MP_QSTR_sync, vfs->u.old.sync);
        mp_load_method(args[0], MP_QSTR_count, vfs->u.old.count);
    }

    vfs->start_block = mp_obj_int_get_checked(mp_load_attr(args[0], MP_QSTR_start_block));
    vfs->block_count = mp_obj_int_get_checked(mp_load_attr(args[0], MP_QSTR_block_count));
    vfs->block_size = mp_obj_int_get_checked(mp_load_attr(args[0], MP_QSTR_block_size));

    vfs->lfs_config.context = vfs;
    vfs->lfs_config.read = lfs_io_fdev_read;
    vfs->lfs_config.prog = lfs_io_fdev_prog;
    vfs->lfs_config.erase = lfs_io_fdev_erase;
    vfs->lfs_config.sync = lfs_io_fdev_sync;
    vfs->lfs_config.read_size = mp_obj_int_get_checked(mp_load_attr(args[0], MP_QSTR_read_size));
    vfs->lfs_config.prog_size = mp_obj_int_get_checked(mp_load_attr(args[0], MP_QSTR_write_size));
    vfs->lfs_config.block_size = vfs->block_size;
    vfs->lfs_config.block_count = vfs->block_count;
    vfs->lfs_config.block_cycles = mp_obj_int_get_checked(mp_load_attr(args[0], MP_QSTR_block_cycles));
    vfs->lfs_config.cache_size = mp_obj_int_get_checked(mp_load_attr(args[0], MP_QSTR_cache_size));
    vfs->lfs_config.lookahead_size = mp_obj_int_get_checked(mp_load_attr(args[0], MP_QSTR_lookahead_size));
    vfs->lfs_config.read_buffer = 0;
    vfs->lfs_config.prog_buffer = 0;
    vfs->lfs_config.lookahead_buffer = 0;
    vfs->lfs_config.name_max = 0;  // Default to LFS_NAME_MAX
    vfs->lfs_config.file_max = 0;  // Default to LFS_FILE_MAX
    vfs->lfs_config.attr_max = 0;  // Default to LFS_ATTR_MAX

    // mount the block device so the VFS methods can be used
    int err = lfs_mount(&vfs->lfs, &vfs->lfs_config);
    if (err != LFS_ERR_OK) {
        // don't error out if no filesystem, to let mkfs() or mount() create one if wanted
        vfs->flags |= FSUSER_NO_FILESYSTEM;
    }

    return MP_OBJ_FROM_PTR(vfs);
}

#if _FS_REENTRANT
STATIC mp_obj_t littlefs_vfs_del(mp_obj_t self_in) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(self_in);
    
    int err;
    err = lfs_unmount(&self->lfs);
    (void)err;

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(littlefs_vfs_del_obj, littlefs_vfs_del);
#endif

STATIC mp_obj_t littlefs_vfs_mkfs(mp_obj_t bdev_in) {
    // create new object
    fs_user_mount_t *vfs = MP_OBJ_TO_PTR(littlefs_vfs_make_new(&mp_littlefs_vfs_type, 1, 0, &bdev_in));

    int err;
    err = lfs_format(&vfs->lfs, &vfs->lfs_config);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(littlefs_vfs_mkfs_fun_obj, littlefs_vfs_mkfs);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(littlefs_vfs_mkfs_obj, MP_ROM_PTR(&littlefs_vfs_mkfs_fun_obj));

typedef struct _mp_littlefs_vfs_ilistdir_it_t {
    mp_obj_base_t base;
    mp_fun_1_t iternext;
    bool is_str;
    lfs_t * lfs;
    lfs_dir_t dir;
} mp_littlefs_vfs_ilistdir_it_t;

STATIC mp_obj_t mp_littlefs_vfs_ilistdir_it_iternext(mp_obj_t self_in) {
    mp_littlefs_vfs_ilistdir_it_t *self = MP_OBJ_TO_PTR(self_in);

    for (;;) {
        struct lfs_info info;
        int err = lfs_dir_read(self->lfs, &self->dir, &info);
        char * fname = info.name;
        if (err == LFS_ERR_OK) {  // End of directory
            break;
        }
        if (err < 0) {  // Error
            lfs_dir_close(self->lfs, &self->dir);
            mp_raise_OSError(lfserr_to_errno(err));
        }

        // Ignore '.' and '..'
        if (fname[0] == '.' && ((fname[1] == '.' && fname[2] == 0) || fname[1] == 0))
            continue;

        // Make 4-tuple with info about this entry
        mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(4, NULL));
        if (self->is_str) {
            t->items[0] = mp_obj_new_str(fname, strlen(fname));
        } else {
            t->items[0] = mp_obj_new_bytes((const byte*)fname, strlen(fname));
        }
        if (info.type == LFS_TYPE_DIR) {
            // dir
            t->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFDIR);
        } else {
            // file
            t->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFREG);
        }
        t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // no inode number
        t->items[3] = mp_obj_new_int_from_uint(info.size);

        return MP_OBJ_FROM_PTR(t);
}
    // ignore error because we may be closing a second time
    lfs_dir_close(self->lfs, &self->dir);

    return MP_OBJ_STOP_ITERATION;
}

STATIC mp_obj_t littlefs_vfs_ilistdir_func(size_t n_args, const mp_obj_t *args) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(args[0]);
    
    bool is_str_type = true;
    const char *path;
    if (n_args == 2) {
        if (mp_obj_get_type(args[1]) == &mp_type_bytes) {
            is_str_type = false;
        }
        path = mp_obj_str_get_str(args[1]);
    } else {
        path = "";
    }

    // Create a new iterator object to list the dir
    mp_littlefs_vfs_ilistdir_it_t *iter = m_new_obj(mp_littlefs_vfs_ilistdir_it_t);
    iter->base.type = &mp_type_polymorph_iter;
    iter->iternext = mp_littlefs_vfs_ilistdir_it_iternext;
    iter->is_str = is_str_type;
    iter->lfs = &self->lfs;
    
    int err = lfs_dir_open(&self->lfs, &iter->dir, path);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }

    return MP_OBJ_FROM_PTR(iter);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(littlefs_vfs_ilistdir_obj, 1, 2, littlefs_vfs_ilistdir_func);

STATIC mp_obj_t littlefs_vfs_rmdir(mp_obj_t vfs_in, mp_obj_t path_in) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    const char *path = mp_obj_str_get_str(path_in);

    int err;
    struct lfs_info info;
    
    err = lfs_stat(&self->lfs, path, &info);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }
    if (info.type != LFS_TYPE_DIR) {
        mp_raise_OSError(MP_ENOTDIR);
    }

    err = lfs_remove(&self->lfs, path);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(littlefs_vfs_rmdir_obj, littlefs_vfs_rmdir);

STATIC mp_obj_t littlefs_vfs_remove(mp_obj_t vfs_in, mp_obj_t path_in) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    const char *path = mp_obj_str_get_str(path_in);

    int err;
    struct lfs_info info;
    
    err = lfs_stat(&self->lfs, path, &info);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }
    if (info.type != LFS_TYPE_REG) {
        mp_raise_OSError(MP_EISDIR);
    }

    err = lfs_remove(&self->lfs, path);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(littlefs_vfs_remove_obj, littlefs_vfs_remove);

STATIC mp_obj_t littlefs_vfs_rename(mp_obj_t vfs_in, mp_obj_t path_in, mp_obj_t path_out) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    const char *old_path = mp_obj_str_get_str(path_in);
    const char *new_path = mp_obj_str_get_str(path_out);
    
    int err;
    err = lfs_rename(&self->lfs, old_path, new_path);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(littlefs_vfs_rename_obj, littlefs_vfs_rename);

STATIC mp_obj_t littlefs_vfs_mkdir(mp_obj_t vfs_in, mp_obj_t path_o) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    const char *path = mp_obj_str_get_str(path_o);
    
    int err;
    err = lfs_mkdir(&self->lfs, path);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(littlefs_vfs_mkdir_obj, littlefs_vfs_mkdir);

STATIC mp_obj_t littlefs_vfs_chdir(mp_obj_t vfs_in, mp_obj_t path_in) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    const char *path;
    path = mp_obj_str_get_str(path_in);

    (void)self;
    (void)path;
#if 0
    FRESULT res = f_chdir(&self->fatfs, path);

    if (res != FR_OK) {
        mp_raise_OSError(fresult_to_errno_table[res]);
    }
#endif

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(littlefs_vfs_chdir_obj, littlefs_vfs_chdir);

STATIC mp_obj_t littlefs_vfs_getcwd(mp_obj_t vfs_in) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    char buf[MICROPY_ALLOC_PATH_MAX + 1];
    
    (void)self;
    buf[0] = 0;
#if 0
    FRESULT res = f_getcwd(&self->fatfs, buf, sizeof(buf));
    if (res != FR_OK) {
        mp_raise_OSError(fresult_to_errno_table[res]);
    }
#endif

    return mp_obj_new_str(buf, strlen(buf));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(littlefs_vfs_getcwd_obj, littlefs_vfs_getcwd);

STATIC mp_obj_t littlefs_vfs_stat(mp_obj_t vfs_in, mp_obj_t path_in) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    const char *path = mp_obj_str_get_str(path_in);

    int err;
    struct lfs_info info;

    err = lfs_stat(&self->lfs, path, &info);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }
    mp_int_t mode = 0;
    if (info.type == LFS_TYPE_DIR) {
        mode |= MP_S_IFDIR;
    } else {
        mode |= MP_S_IFREG;
    }

    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    t->items[0] = MP_OBJ_NEW_SMALL_INT(mode); // st_mode
    t->items[1] = MP_OBJ_NEW_SMALL_INT(0); // st_ino
    t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // st_dev
    t->items[3] = MP_OBJ_NEW_SMALL_INT(0); // st_nlink
    t->items[4] = MP_OBJ_NEW_SMALL_INT(0); // st_uid
    t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // st_gid
    t->items[6] = mp_obj_new_int_from_uint(info.size); // st_size
    t->items[7] = MP_OBJ_NEW_SMALL_INT(0); // st_atime
    t->items[8] = MP_OBJ_NEW_SMALL_INT(0); // st_mtime
    t->items[9] = MP_OBJ_NEW_SMALL_INT(0 ); // st_ctime

    return MP_OBJ_FROM_PTR(t);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(littlefs_vfs_stat_obj, littlefs_vfs_stat);

// Get the status of a VFS.
STATIC mp_obj_t littlefs_vfs_statvfs(mp_obj_t vfs_in, mp_obj_t path_in) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    (void)path_in;

    lfs_ssize_t total_blocks = self->block_count;
    lfs_ssize_t allocated_blocks = lfs_fs_size(&self->lfs);
    if (allocated_blocks < 0) {
        int err = allocated_blocks;
        mp_raise_OSError(lfserr_to_errno(err));
    }
    lfs_ssize_t free_blocks = total_blocks - allocated_blocks;
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    t->items[0] = MP_OBJ_NEW_SMALL_INT(self->block_size); // f_bsize - filesystem block size
    t->items[1] = t->items[0]; // f_frsize - filesystem fragment size
    t->items[2] = MP_OBJ_NEW_SMALL_INT(self->block_count); // f_blocks - size of filesystem in blocks
    t->items[3] = MP_OBJ_NEW_SMALL_INT(free_blocks); // f_bfree - free blocks
    t->items[4] = t->items[3]; // f_bavail - free blocks for unprivileged users
    t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // f_files - number of inodes
    t->items[6] = MP_OBJ_NEW_SMALL_INT(0); // f_ffree - number of free inodes
    t->items[7] = MP_OBJ_NEW_SMALL_INT(0); // f_favail - number of free inodes for unprivileged users
    t->items[8] = MP_OBJ_NEW_SMALL_INT(0); // f_flags - mount flags
    t->items[9] = MP_OBJ_NEW_SMALL_INT(LFS_NAME_MAX); // f_namemax - maximum filename length

    return MP_OBJ_FROM_PTR(t);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(littlefs_vfs_statvfs_obj, littlefs_vfs_statvfs);

STATIC mp_obj_t littlefs_vfs_mount(mp_obj_t self_in, mp_obj_t readonly, mp_obj_t mkfs) {
    fs_user_mount_t *self = MP_OBJ_TO_PTR(self_in);

    // Read-only device indicated by writeblocks[0] == MP_OBJ_NULL.
    // User can specify read-only device by:
    //  1. readonly=True keyword argument
    //  2. nonexistent writeblocks method (then writeblocks[0] == MP_OBJ_NULL already)
    if (mp_obj_is_true(readonly)) {
        self->write[0] = MP_OBJ_NULL;
    }

    // If there is no filesystem, and we need to make one, then do so...
    if ((self->flags & FSUSER_NO_FILESYSTEM) && mp_obj_is_true(mkfs)) {
        int err;
        err = lfs_format(&self->lfs, &self->lfs_config);
        if (err != LFS_ERR_OK) {
            mp_raise_OSError(lfserr_to_errno(err));
        }
    }

    // Mount the filesystem
    int err = lfs_mount(&self->lfs, &self->lfs_config);
    if (err != LFS_ERR_OK) {
        mp_raise_OSError(lfserr_to_errno(err));
    }

    self->flags &= ~FSUSER_NO_FILESYSTEM;
    return mp_const_none;


}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(littlefs_vfs_mount_obj, littlefs_vfs_mount);

STATIC mp_obj_t littlefs_vfs_umount(mp_obj_t self_in) {
    (void)self_in;
    // keep the filesystem mounted internally so the VFS methods can still be used
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(littlefs_vfs_umount_obj, littlefs_vfs_umount);


MP_DECLARE_CONST_FUN_OBJ_3(littlefs_vfs_open_obj);

STATIC const mp_rom_map_elem_t littlefs_vfs_locals_dict_table[] = {
    #if _FS_REENTRANT
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&littlefs_vfs_del_obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_mkfs), MP_ROM_PTR(&littlefs_vfs_mkfs_obj) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&littlefs_vfs_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&littlefs_vfs_ilistdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&littlefs_vfs_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&littlefs_vfs_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&littlefs_vfs_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&littlefs_vfs_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&littlefs_vfs_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&littlefs_vfs_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&littlefs_vfs_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_statvfs), MP_ROM_PTR(&littlefs_vfs_statvfs_obj) },
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&littlefs_vfs_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&littlefs_vfs_umount_obj) },
};
STATIC MP_DEFINE_CONST_DICT(littlefs_vfs_locals_dict, littlefs_vfs_locals_dict_table);

STATIC const mp_vfs_proto_t littlefs_vfs_proto = {
    .import_stat = littlefs_vfs_import_stat,
};

const mp_obj_type_t mp_littlefs_vfs_type = {
    { &mp_type_type },
    .name = MP_QSTR_VfsLittleFS,
    .make_new = littlefs_vfs_make_new,
    .protocol = &littlefs_vfs_proto,
    .locals_dict = (mp_obj_dict_t*)&littlefs_vfs_locals_dict,
};


// File operations

extern const mp_obj_type_t mp_type_vfs_littlefs_fileio;
extern const mp_obj_type_t mp_type_vfs_littlefs_textio;


typedef struct _pyb_file_obj_t {
    mp_obj_base_t base;
    bool is_open;
    lfs_t * lfsp;
    lfs_file_t lfile;
} pyb_file_obj_t;


STATIC void file_obj_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    mp_printf(print, "<io.%s %p>", mp_obj_get_type_str(self_in), MP_OBJ_TO_PTR(self_in));
}

STATIC mp_uint_t file_obj_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    pyb_file_obj_t *self = MP_OBJ_TO_PTR(self_in);

    lfs_ssize_t size_or_err = lfs_file_read(self->lfsp, &self->lfile, buf, size);
    if (size_or_err < 0) {
        *errcode = lfserr_to_errno(size_or_err);
        return MP_STREAM_ERROR;
    }

    return size_or_err;
}

STATIC mp_uint_t file_obj_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    pyb_file_obj_t *self = MP_OBJ_TO_PTR(self_in);

    lfs_ssize_t size_or_err = lfs_file_write(self->lfsp, &self->lfile, buf, size);
    if (size_or_err < 0) {
        *errcode = lfserr_to_errno(size_or_err);
        return MP_STREAM_ERROR;
    }

    return size_or_err;
}

STATIC mp_obj_t file_obj___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return mp_stream_close(args[0]);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(file_obj___exit___obj, 4, 4, file_obj___exit__);


#define lfs_from_mp_whence(w) (w)

STATIC mp_uint_t file_obj_ioctl(mp_obj_t o_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    pyb_file_obj_t *self = MP_OBJ_TO_PTR(o_in);

    if (request == MP_STREAM_SEEK) {
        struct mp_stream_seek_t *s = (struct mp_stream_seek_t*)(uintptr_t)arg;

        int lfs_whence = lfs_from_mp_whence(s->whence);
        lfs_soff_t offset_or_err = lfs_file_seek(self->lfsp, &self->lfile, s->offset, lfs_whence);
        if (offset_or_err < 0) {
            *errcode = lfserr_to_errno(offset_or_err);
            return MP_STREAM_ERROR;
        }
        s->offset = offset_or_err;
        return 0;

    } else if (request == MP_STREAM_FLUSH) {
        int err = lfs_file_sync(self->lfsp, &self->lfile);
        if (err < 0) {
            *errcode = lfserr_to_errno(err);
            return MP_STREAM_ERROR;
        }
        return 0;

    } else if (request == MP_STREAM_CLOSE) {
        if (self->is_open) {
            int err = lfs_file_close(self->lfsp, &self->lfile);
            if (err < 0) {
                *errcode = lfserr_to_errno(err);
                return MP_STREAM_ERROR;
            }
            self->is_open = false;
        }
        return 0;

    } else {
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
}

// Note: encoding is ignored for now; it's also not a valid kwarg for CPython's FileIO,
// but by adding it here we can use one single mp_arg_t array for open() and FileIO's constructor
STATIC const mp_arg_t file_open_args[] = {
    { MP_QSTR_file, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_rom_obj = MP_ROM_PTR(&mp_const_none_obj)} },
    { MP_QSTR_mode, MP_ARG_OBJ, {.u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_r)} },
    { MP_QSTR_encoding, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_PTR(&mp_const_none_obj)} },
};
#define FILE_OPEN_NUM_ARGS MP_ARRAY_SIZE(file_open_args)

STATIC mp_obj_t file_open(fs_user_mount_t *vfs, const mp_obj_type_t *type, mp_arg_val_t *args) {
    int mode = 0;
    const char *mode_s = mp_obj_str_get_str(args[1].u_obj);
    // TODO make sure only one of r, w, x, a, and b, t are specified
    while (*mode_s) {
        switch (*mode_s++) {
            case 'r':
                mode |= LFS_O_RDONLY;
                break;
            case 'w':
                mode |= LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
                break;
            case 'x':
                mode |= LFS_O_WRONLY | LFS_O_EXCL;
                break;
            case 'a':
                mode |= LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND;
                break;
            case '+':
                mode |= LFS_O_RDONLY | LFS_O_WRONLY;
                break;
            #if MICROPY_PY_IO_FILEIO
            case 'b':
                type = &mp_type_vfs_littlefs_fileio;
                break;
            #endif
            case 't':
                type = &mp_type_vfs_littlefs_textio;
                break;
        }
    }

    pyb_file_obj_t *o = m_new_obj_with_finaliser(pyb_file_obj_t);
    o->base.type = type;
    o->lfsp = &vfs->lfs;
    o->is_open = false;

    const char *fname = mp_obj_str_get_str(args[0].u_obj);
    assert(vfs != NULL);

    int err = lfs_file_open(o->lfsp, &o->lfile, fname, mode);
    if (err != LFS_ERR_OK) {
        m_del_obj(pyb_file_obj_t, o);
        mp_raise_OSError(lfserr_to_errno(err));
    }
    o->is_open = true;

    return MP_OBJ_FROM_PTR(o);
}

STATIC mp_obj_t file_obj_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_val_t arg_vals[FILE_OPEN_NUM_ARGS];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, FILE_OPEN_NUM_ARGS, file_open_args, arg_vals);
    return file_open(NULL, type, arg_vals);
}

// TODO gc hook to close the file if not already closed

STATIC const mp_rom_map_elem_t rawfile_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&file_obj___exit___obj) },
};

STATIC MP_DEFINE_CONST_DICT(rawfile_locals_dict, rawfile_locals_dict_table);

#if MICROPY_PY_IO_FILEIO
STATIC const mp_stream_p_t fileio_stream_p = {
    .read = file_obj_read,
    .write = file_obj_write,
    .ioctl = file_obj_ioctl,
};

const mp_obj_type_t mp_type_vfs_littlefs_fileio = {
    { &mp_type_type },
    .name = MP_QSTR_FileIO,
    .print = file_obj_print,
    .make_new = file_obj_make_new,
    .getiter = mp_identity_getiter,
    .iternext = mp_stream_unbuffered_iter,
    .protocol = &fileio_stream_p,
    .locals_dict = (mp_obj_dict_t*)&rawfile_locals_dict,
};
#endif

STATIC const mp_stream_p_t textio_stream_p = {
    .read = file_obj_read,
    .write = file_obj_write,
    .ioctl = file_obj_ioctl,
    .is_text = true,
};

const mp_obj_type_t mp_type_vfs_littlefs_textio = {
    { &mp_type_type },
    .name = MP_QSTR_TextIOWrapper,
    .print = file_obj_print,
    .make_new = file_obj_make_new,
    .getiter = mp_identity_getiter,
    .iternext = mp_stream_unbuffered_iter,
    .protocol = &textio_stream_p,
    .locals_dict = (mp_obj_dict_t*)&rawfile_locals_dict,
};

// Factory function for I/O stream classes
STATIC mp_obj_t littlefs_builtin_open_self(mp_obj_t self_in, mp_obj_t path, mp_obj_t mode) {
    // TODO: analyze buffering args and instantiate appropriate type
    fs_user_mount_t *self = MP_OBJ_TO_PTR(self_in);
    mp_arg_val_t arg_vals[FILE_OPEN_NUM_ARGS];
    arg_vals[0].u_obj = path;
    arg_vals[1].u_obj = mode;
    arg_vals[2].u_obj = mp_const_none;
    return file_open(self, &mp_type_vfs_littlefs_textio, arg_vals);
}
MP_DEFINE_CONST_FUN_OBJ_3(littlefs_vfs_open_obj, littlefs_builtin_open_self);

#endif // MICROPY_VFS_LITTLEFS
