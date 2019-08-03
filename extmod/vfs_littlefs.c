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


int lfs_io_bdev_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    fs_user_mount_t *vfs = c->context;
    mp_obj_array_t ar = {{&mp_type_bytearray}, BYTEARRAY_TYPECODE, 0, size, buffer};
    vfs->read[2] = MP_OBJ_NEW_SMALL_INT(block);
    vfs->read[3] = MP_OBJ_NEW_SMALL_INT(off);
    vfs->read[4] = MP_OBJ_FROM_PTR(&ar);
    mp_call_method_n_kw(3, 0, vfs->read);
    return LFS_ERR_OK;
}

int lfs_io_bdev_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    fs_user_mount_t *vfs = c->context;
    mp_obj_array_t ar = {{&mp_type_bytearray}, BYTEARRAY_TYPECODE, 0, size, (void *)buffer};
    vfs->write[2] = MP_OBJ_NEW_SMALL_INT(block);
    vfs->write[3] = MP_OBJ_NEW_SMALL_INT(off);
    vfs->write[4] = MP_OBJ_FROM_PTR(&ar);
    mp_call_method_n_kw(3, 0, vfs->write);
    return LFS_ERR_OK;
}

 int lfs_io_bdev_erase(const struct lfs_config *c, lfs_block_t block)
 {
     fs_user_mount_t *vfs = c->context;
     vfs->erase[2] = MP_OBJ_NEW_SMALL_INT(block);
     mp_call_method_n_kw(1, 0, vfs->erase);
     return LFS_ERR_OK;
 }

int lfs_io_bdev_sync(const struct lfs_config *c)
{
    return LFS_ERR_OK;
}



static int lfserr_to_errno(int lfserr) {
    return lfserr;
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
    vfs->lfs_config.read = lfs_io_bdev_read;
    vfs->lfs_config.prog = lfs_io_bdev_prog;
    vfs->lfs_config.erase = lfs_io_bdev_erase;
    vfs->lfs_config.sync = lfs_io_bdev_sync;
    vfs->lfs_config.read_size = vfs->block_size;
    vfs->lfs_config.prog_size = vfs->block_size;
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

    (void)self;
    (void)path;
#if 0
    FILINFO fno;
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) {
        // stat root directory
        fno.fsize = 0;
        fno.fdate = 0x2821; // Jan 1, 2000
        fno.ftime = 0;
        fno.fattrib = AM_DIR;
    } else {
        FRESULT res = f_stat(&self->fatfs, path, &fno);
        if (res != FR_OK) {
            mp_raise_OSError(fresult_to_errno_table[res]);
        }
    }

    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    mp_int_t mode = 0;
    if (fno.fattrib & AM_DIR) {
        mode |= MP_S_IFDIR;
    } else {
        mode |= MP_S_IFREG;
    }
    mp_int_t seconds = timeutils_seconds_since_2000(
        1980 + ((fno.fdate >> 9) & 0x7f),
        (fno.fdate >> 5) & 0x0f,
        fno.fdate & 0x1f,
        (fno.ftime >> 11) & 0x1f,
        (fno.ftime >> 5) & 0x3f,
        2 * (fno.ftime & 0x1f)
    );
    t->items[0] = MP_OBJ_NEW_SMALL_INT(mode); // st_mode
    t->items[1] = MP_OBJ_NEW_SMALL_INT(0); // st_ino
    t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // st_dev
    t->items[3] = MP_OBJ_NEW_SMALL_INT(0); // st_nlink
    t->items[4] = MP_OBJ_NEW_SMALL_INT(0); // st_uid
    t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // st_gid
    t->items[6] = mp_obj_new_int_from_uint(fno.fsize); // st_size
    t->items[7] = MP_OBJ_NEW_SMALL_INT(seconds); // st_atime
    t->items[8] = MP_OBJ_NEW_SMALL_INT(seconds); // st_mtime
    t->items[9] = MP_OBJ_NEW_SMALL_INT(seconds); // st_ctime

    return MP_OBJ_FROM_PTR(t);
#endif

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(littlefs_vfs_stat_obj, littlefs_vfs_stat);

// Get the status of a VFS.
STATIC mp_obj_t littlefs_vfs_statvfs(mp_obj_t vfs_in, mp_obj_t path_in) {
    mp_obj_littlefs_vfs_t *self = MP_OBJ_TO_PTR(vfs_in);
    (void)path_in;

    (void)self;
#if 0
    DWORD nclst;
    FATFS *fatfs = &self->fatfs;
    FRESULT res = f_getfree(fatfs, &nclst);
    if (FR_OK != res) {
        mp_raise_OSError(fresult_to_errno_table[res]);
    }

    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));

    t->items[0] = MP_OBJ_NEW_SMALL_INT(fatfs->csize * SECSIZE(fatfs)); // f_bsize
    t->items[1] = t->items[0]; // f_frsize
    t->items[2] = MP_OBJ_NEW_SMALL_INT((fatfs->n_fatent - 2)); // f_blocks
    t->items[3] = MP_OBJ_NEW_SMALL_INT(nclst); // f_bfree
    t->items[4] = t->items[3]; // f_bavail
    t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // f_files
    t->items[6] = MP_OBJ_NEW_SMALL_INT(0); // f_ffree
    t->items[7] = MP_OBJ_NEW_SMALL_INT(0); // f_favail
    t->items[8] = MP_OBJ_NEW_SMALL_INT(0); // f_flags
    t->items[9] = MP_OBJ_NEW_SMALL_INT(FF_MAX_LFN); // f_namemax

    return MP_OBJ_FROM_PTR(t);
#endif

    return mp_const_none;
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


STATIC const mp_rom_map_elem_t littlefs_vfs_locals_dict_table[] = {
    #if _FS_REENTRANT
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&littlefs_vfs_del_obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_mkfs), MP_ROM_PTR(&littlefs_vfs_mkfs_obj) },
//    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&littlefs_vfs_open_obj) },
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

#endif // MICROPY_VFS_LITTLEFS

