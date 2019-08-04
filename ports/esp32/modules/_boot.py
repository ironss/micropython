import gc
import uos
import flashbdev

if flashbdev.bdev0:
    try:
        uos.mount(flashbdev.bdev0, '/')
    except OSError:
        import inisetup
        vfs = inisetup.setup()

if flashbdev.bdev1:
    try:
        uos.mount(flashbdev.bdev1, '/lfs', fstype=uos.VfsLittleFS)
    except OSError:
        pass

gc.collect()
