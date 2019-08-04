import gc
import uos
import flashdev

if flashdev.fdev0:
    try:
        uos.mount(flashdev.fdev0, '/', fstype=uos.VfsLittleFS)
    except OSError:
        print("Performing initial setup")
        uos.VfsLittleFS.mkfs(flashdev.fdev0)
        vfs = uos.VfsFat(flashdev.fdev0)
        uos.mount(vfs, '/flash')
        uos.chdir('/flash')
        with open('/flash/boot.py', 'w') as f:
            f.write("""\
# This file is executed on every boot (including wake-boot from deepsleep)
#import esp
#esp.osdebug(None)
#import webrepl
#webrepl.start()
""")

gc.collect()
