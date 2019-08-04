import esp

class FlashDev:
    def __init__(self, start_addr, size_bytes, block_size, read_size, write_size):
        self.block_size = block_size
        self.start_block = start_addr // block_size
        self.block_count = size_bytes // block_size
        self.read_size = read_size
        self.write_size = write_size
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
    fdev0 = None
else:
    # for now we use a fixed size for the filesystem
    fdev0_start_addr = esp.flash_user_start()
    fdev0_size_bytes = 2048 * 1024
    fdev0_block_size = 4096
    fdev0 = FlashDev(fdev0_start_addr, fdev0_size_bytes, fdev0_block_size, 64, 64)
