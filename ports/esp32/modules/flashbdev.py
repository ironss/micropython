import esp

class FlashBdev:
    def __init__(self, start_addr, size_bytes, block_size):
        self.block_size = block_size
        self.start_block = start_addr // block_size
        self.blocks = size_bytes // block_size

    def readblocks(self, n, buf):
        #print("readblocks(%s, %x(%d))" % (n, id(buf), len(buf)))
        esp.flash_read((n + self.start_block) * self.block_size, buf)

    def writeblocks(self, n, buf):
        #print("writeblocks(%s, %x(%d))" % (n, id(buf), len(buf)))
        #assert len(buf) <= self.SEC_SIZE, len(buf)
        esp.flash_erase(n + self.start_block)
        esp.flash_write((n + self.start_block) * self.block_size, buf)

    def ioctl(self, op, arg):
        #print("ioctl(%d, %r)" % (op, arg))
        if op == 4:  # BP_IOCTL_SEC_COUNT
            return self.blocks
        if op == 5:  # BP_IOCTL_SEC_SIZE
            return self.block_size

class FlashDev_LFS:
    def __init__(self, start_addr, size_bytes, block_size):
        self.block_size = block_size
        self.start_block = start_addr // block_size
        self.block_count = size_bytes // block_size
        self.write_size = 64
        self.erase_size = 64
        self.block_cycles = 100
        self.cache_size = 1024
        self.lookahead_size = 32

    def read(self, block, offset, buf):
        #print("readflash: (%d %d %x(%d))" % (block, offset, id(buf), len(buf)))
        esp.flash_read(((self.start_block + block) * self.block_size) + offset, buf)
        #print(buf)

    def write(self, block, offset, buf):
        #print("writeflash: (%d %d %x(%d))" % (block, offset, id(buf), len(buf)))
        assert(block < self.block_count)
        esp.flash_write(((self.start_block + block) * self.block_size) + offset, buf)

    def erase(self, block):
        #print("eraseblock %s" % (block))
        assert(block < self.block_count)
        esp.flash_erase(self.start_block + block)
    
    def ioctl(self, op, arg):
        #print("ioctl(%d, %r)" % (op, arg))
        if op == 4:  # BP_IOCTL_SEC_COUNT
            return self.blocks
        if op == 5:  # BP_IOCTL_SEC_SIZE
            return self.block_size

size = esp.flash_size()
print("Flash size: {}".format(size))
if size < 1024*1024:
    # flash too small for a filesystem
    bdev0 = None
    bdev1 = None
else:
    # for now we use a fixed size for the filesystem
    bdev0_start_addr = esp.flash_user_start()
    bdev0_size_bytes = 2048 * 1024
    bdev0_block_size = 4096
    bdev0 = FlashBdev(bdev0_start_addr, bdev0_size_bytes, bdev0_block_size)
    
    bdev1_start_addr = bdev0_start_addr + bdev0_size_bytes
    bdev1_size_bytes = 2048 * 1024
    bdev1_block_size = 4096
    bdev1 = FlashDev_LFS(bdev1_start_addr, bdev1_size_bytes, bdev1_block_size)
