Before writing a general READ function, we start with writing READ CAPACITY. We need to know two things before issuing a correct read:
1. Block size - we need to know how many bytes a block is (usually 512 or 4096 bytes for NVMe)
2. Max LBA (logical block address) - this is the last addressable logical block, which is needed so we know what the valid address range is and avoid requesting an out-of-bounds block.

READ CAPACITY(10) is a standard way to get both, and is structurally almost identical to our INQUIRY function. This time, instead of a 96 byte response, we get an 8 byte response. The first 4 bytes being the last LBA (in big endian), and the last 4 bytes being the block length in bytes (in big endian).

We can calculate the total capacity of the SSD in bytes by simply multiplying the number of logical blocks and the block size:
capacity = (max LBA + 1) * (block size)

In my case I used a 1 TB SSD, so we can double check if the capacity matches.

After sending our READ CAPACITY(10), our response reads:
max LBA=1953525167, block size=512 bytes
capacity=1000204886016 bytes

1000204886016/(1000^3) = 1.0002 TB in decimal
1000204886016/(1024^3) = 931.5 GB in binary
