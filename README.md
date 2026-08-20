# Introduction
This is a Linux driver for the RTL9210 SSD reader developed by me for learning purposes. It uses the BOT protocol, connects to the SCSI middle layer as a lower layer driver, and supports all SCSI commands (that the hardware can support) through asynchronous/non-blocking execution. I used WSL2 on Ubuntu at first to develop and test the code, then verified that the driver works on Arch Linux as well. 

Below is the information of the hardware and software I used for testing. The full length copy of the endpoint descriptors are located at `/docs/descriptors.txt`. The most recent version of the driver can be found at `/src/rtl9210.c`.

RTL9210 SSD Reader
------------------
* Vendor:   0x0bda Realtek Semiconductor Corp.
* Product:  0x9210 RTL9210 M.2 NVME Adapter

SSD
---
* WD_Black SN7100 1TB NVMe SSD
* Filesystems used to test: ext4, extfat  

Linux Distributions Tested on
-----------------------------
* Ubuntu 24.04.1 LTS (Kernel Version: 6.8)  
* Arch Linux 2026.08.01 (Kernel Version: 7.1.5)  

Driver Interface
----------------
* Interface Class:    Mass Storage
* Interface Subclass: SCSI
* Interface Protocol: Bulk-Only

# Initial Skeleton
Initially, I added a `struct usb_driver` and assigned the `.name`, `.probe`, `.disconnect`, and `.id_table`. The `.name` is simply the name of the driver. `probe()` and `disconnect()` are both functions that are called when the driver is inserted/removed as a kernel module. So I created these functions, then assigned those to `.probe` and `.disconnect`. `.id_table` should be an array of vendor/product ID pairs my driver claims to support, so I also added a `struct usb_device_id` and assigned it. I then registered the driver using `module_usb_driver()` with `struct usb_driver` as the input.

`probe()` takes a `struct usb_interface` and a `struct usb_device_id` as inputs. `struct usb_interface` is what the USB device driver talks to, and interfaces can be used to configure hardware settings like the USB communication protocol (eg. BOT vs UAS). It is also used to get `struct usb_device` through `interface_to_usbdeb()`, where that `struct usb_device` is used for communications with endpoints later. `struct usb_device_id` tells us which entry in our `.id_table` matched, which is necessary when there are multiple vendor:product ID pairs but isn't needed in our case since we only have 1 pair.

# BOT vs UAS
The USB (Universal Serial Bus) requires a protocol to communicate with the kernel. We have two options: BOT (Bulk-Only Transport) and UAS (USB Attached SCSI).  

BOT (Bulk-Only Transport):  
Host sends a command block wrapper (CBW), which is a BOT-specific envelope. The device then sends/receives the data and sends a command status wrapper (CSW) to report the status. BOT only needs 2 endpoints, IN and OUT, since it sends and receives commands, data, and statuses sequentially. The problem with this protocol is that it is very slow, because the host must wait until the device reports the status before sending the next command. This is due to BOT being developed a very long time ago (1990s), so it has limited performance. However, BOT is also more versatile and can be used by almost any device.

UAS (USB Attached SCSI):  
UAS sends commands in parallel, unlike BOT. Up to 32 commands can be sent simultaneously. UAS uses 4 endpoints (command, data IN, data OUT, status) instead of 2 to send commands in parallel. While one command is being sent to the host, the host can be sending data for another command. This improves performance significantly if there are many different operations queued (eg. read after write after read). This feature is called streams, which is only compatible with USB 3.0 and up.

I was using vhci_hcd to test our driver, which is a virtual USB controller provided by WSL2. vhci_hcd does not support streams required by UAS, so in this case I chose to use BOT due to hardware limitations.
```
[   16.415425] usb 2-1: USB controller vhci_hcd.0 does not support streams, which are required by the UAS driver.
[   16.415428] usb 2-1: Please try an other USB controller if you wish to use UAS.
```

To use the BOT protocol, I needed to allocate URBs (USB Request block). I simply added 2 `struct urb`s in my per-device state, which are then allocated using `usb_alloc_urb()`. Those URBs are then submitted using `usb_submit_urb()` in the order of
```
command -> data -> status
```

# USB Request Block
URB (USB Request Block) is the layer below the transfer protocol. We can fill out URBs to contain all the necessary information to execute USB transactions, whether that is command, data IN, data OUT, or status.

For every endpoint being used, we need an URB. For BOT we allocate 2 URBs to send and receive data sequentially. For UAS we allocate 3 URBs to send and receive data in parallel.

INQUIRY:
```
BOT:
bulk-out URB -> EP 2 OUT (send INQUIRY command)
bulk-in URB  <- EP 1 IN  (receive data)
bulk-in URB  <- EP 1 IN  (receive status)

UAS:
command URB -> EP 4 OUT (send INQIURY command)
data IN URB <- EP 1 IN  (receive data)
status URB  <- EP 3 IN  (receive status)
```

READ:
```
BOT:
bulk-out URB -> EP 2 OUT (send READ command)
bulk-in URB  <- EP 1 IN  (receive data)
bulk-in URB  <- EP 1 IN  (receive status)

UAS:
command URB -> EP 4 OUT (send READ command)
data IN URB <- EP 1 IN  (receive data)
status URB  <- EP 3 IN  (receive status)
```

WRITE:
```
BOT:
bulk-out URB -> EP 2 OUT (send WRITE command)
bulk-out URB -> EP 2 OUT (send data)
bulk-in URB  <- EP 1 IN  (receive status)

UAS:
command URB  -> EP 4 OUT (send WRITE command)
data OUT URB -> EP 2 OUT (send data)
status URB   <- EP 3 IN  (receive status)
```

I allocated the URBs using `usb_alloc_urb()` in `probe()`. These URBs are only freed upon reaching `disconnect()`. I then initialized the URBs using `usb_fill_bulk_urb()` in `inquiry()`/`read()`/`write()` which provides the USB device pointer, pipe, transfer buffer, desired transfer length, completion handler, and context. The URB is submitted through `usb_submit_urb()`.

# Inquiry
The very first transfer to be sent will be the INQUIRY command. INQUIRY is an SCSI command that makes the device identify itself. INQIURY fills out a data buffer with the vendor, product, and revision. INQIURY is the simplest possible read operation that we implement before implementing actual read/write commands. For now I will be implementing SCSI commands in individual functions, such as `inquiry()`, `read()`, `write()`, etc.

Since we are using BOT to transfer data, we use the command block wrapper `struct bulk_cb_wrap` to request data and command status wrapper `struct bulk_cs_wrap` to receive status. These are structs provided under `storage.h` in the kernel. With BOT, we send requests and receive data sequentially. The sequence goes like:
```
send CBW -> wait -> receive data -> wait -> receive CSW -> wait -> done
```

There are delays that must happen between each steps due to the time taken to transfer. I provided these delays through the use of `struct completion` and `wait_for_completion()`. If we don't provide delays, the driver will not wait for the transfer and move to the next step immediately.

Once we are done implementing, we get the vendor, product, and revision from the data buffer:
```
[   27.044044] rtl9210: INQUIRY CBW sent
[   27.482501] rtl9210: INQUIRY response received
[   27.482516] rtl9210: vendor:   Realtek
[   27.482522] rtl9210: product:  RTL9210 NVME
[   27.482525] rtl9210: revision: 1.00
[   27.485955] rtl9210: INQUIRY CSW received
```

# Read Capacity
Before writing READ, we start with writing READ_CAPACITY. We need to know two things before issuing a correct read:
1. Block size - we need to know how many bytes a block is (usually 512 or 4096 bytes for NVMe)
2. Max LBA (logical block address) - this is the last addressable logical block, which is needed so we know what the valid address range is and avoid requesting an out-of-bounds block.

READ_CAPACITY_10 is a standard way to get both and is structurally almost identical to our INQUIRY function. This time, instead of a 96 byte response, we get an 8 byte response. The first 4 bytes being the last LBA (in big endian), and the last 4 bytes being the block length in bytes (in big endian).

We can calculate the total capacity of the SSD in bytes by simply multiplying the number of logical blocks and the block size:  
```
capacity = (max LBA + 1) * (block size)
```

In my case I used a 1 TB SSD, so we can double check if the capacity matches. After sending our READ_CAPACITY_10, our response reads:  
```
[   27.490778] rtl9210: READ CAPACITY(10) CBW sent
[   27.494803] rtl9210: max LBA=1953525167, block size=512 bytes
[   27.494819] rtl9210: capacity=1000204886016 bytes
[   27.498020] rtl9210: READ CAPACITY(10) CSW received
```

Which equates to:  
```
1000204886016/(1000^3) = 1.0002 TB in decimal  
1000204886016/(1024^3) = 931.5 GB in binary
```

# Read/Write
`read()` and `write()` are very similar to `read_capacity()`, the main difference being that we must fill out the block address under CDB bytes 2-5 and number of blocks under bytes 7-8. Once I was finished writing the functions, I tested it with `write_test()`. This function is called with the block address and number of blocks to write. It stores the original blocks at that address, fills my magic number `5a` ('Z' in ASCII) in those blocks, then restores the data.

I made `read()` print the blocks read for testing purposes. When `write_test()` is called on block_address=1953519615 and num_blocks=1 we get:
```
[   27.500885] rtl9210: READ(10) CBW sent
[   27.503592] rtl9210: READ(10) data received
[   27.506262] rtl9210: READ(10) CSW received
[   27.506273] rtl9210: 00000000: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506281] rtl9210: 00000010: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506285] rtl9210: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506288] rtl9210: 00000030: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506291] rtl9210: 00000040: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506294] rtl9210: 00000050: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506296] rtl9210: 00000060: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506299] rtl9210: 00000070: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506301] rtl9210: 00000080: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506304] rtl9210: 00000090: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506307] rtl9210: 000000a0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506310] rtl9210: 000000b0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506312] rtl9210: 000000c0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506315] rtl9210: 000000d0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506317] rtl9210: 000000e0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506320] rtl9210: 000000f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506322] rtl9210: 00000100: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506325] rtl9210: 00000110: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506328] rtl9210: 00000120: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506331] rtl9210: 00000130: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506333] rtl9210: 00000140: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506336] rtl9210: 00000150: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506338] rtl9210: 00000160: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506341] rtl9210: 00000170: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506343] rtl9210: 00000180: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506346] rtl9210: 00000190: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506349] rtl9210: 000001a0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506352] rtl9210: 000001b0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506354] rtl9210: 000001c0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506356] rtl9210: 000001d0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506358] rtl9210: 000001e0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.506360] rtl9210: 000001f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.508192] rtl9210: WRITE(10) CBW sent
[   27.510508] rtl9210: WRITE(10) data received
[   27.512972] rtl9210: WRITE(10) CSW received
[   27.514412] rtl9210: READ(10) CBW sent
[   27.517428] rtl9210: READ(10) data received
[   27.519319] rtl9210: READ(10) CSW received
[   27.519324] rtl9210: 00000000: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519329] rtl9210: 00000010: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519332] rtl9210: 00000020: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519334] rtl9210: 00000030: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519336] rtl9210: 00000040: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519337] rtl9210: 00000050: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519339] rtl9210: 00000060: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519341] rtl9210: 00000070: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519343] rtl9210: 00000080: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519344] rtl9210: 00000090: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519346] rtl9210: 000000a0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519348] rtl9210: 000000b0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519349] rtl9210: 000000c0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519351] rtl9210: 000000d0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519353] rtl9210: 000000e0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519354] rtl9210: 000000f0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519356] rtl9210: 00000100: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519358] rtl9210: 00000110: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519359] rtl9210: 00000120: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519361] rtl9210: 00000130: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519363] rtl9210: 00000140: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519365] rtl9210: 00000150: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519366] rtl9210: 00000160: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519368] rtl9210: 00000170: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519370] rtl9210: 00000180: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519371] rtl9210: 00000190: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519373] rtl9210: 000001a0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519375] rtl9210: 000001b0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519376] rtl9210: 000001c0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519378] rtl9210: 000001d0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519380] rtl9210: 000001e0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.519381] rtl9210: 000001f0: 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a 5a  ZZZZZZZZZZZZZZZZ
[   27.520486] rtl9210: WRITE(10) CBW sent
[   27.521621] rtl9210: WRITE(10) data received
[   27.522818] rtl9210: WRITE(10) CSW received
[   27.523651] rtl9210: READ(10) CBW sent
[   27.524461] rtl9210: READ(10) data received
[   27.525172] rtl9210: READ(10) CSW received
[   27.525177] rtl9210: 00000000: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525181] rtl9210: 00000010: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525183] rtl9210: 00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525185] rtl9210: 00000030: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525186] rtl9210: 00000040: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525187] rtl9210: 00000050: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525189] rtl9210: 00000060: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525190] rtl9210: 00000070: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525191] rtl9210: 00000080: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525193] rtl9210: 00000090: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525194] rtl9210: 000000a0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525195] rtl9210: 000000b0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525196] rtl9210: 000000c0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525198] rtl9210: 000000d0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525199] rtl9210: 000000e0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525200] rtl9210: 000000f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525201] rtl9210: 00000100: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525203] rtl9210: 00000110: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525204] rtl9210: 00000120: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525206] rtl9210: 00000130: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525207] rtl9210: 00000140: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525208] rtl9210: 00000150: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525210] rtl9210: 00000160: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525211] rtl9210: 00000170: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525212] rtl9210: 00000180: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525213] rtl9210: 00000190: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525215] rtl9210: 000001a0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525216] rtl9210: 000001b0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525217] rtl9210: 000001c0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525219] rtl9210: 000001d0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525220] rtl9210: 000001e0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525221] rtl9210: 000001f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.525224] rtl9210: write test passed
```
which shows that block 1953519615 was stored, then filled with `5a`, then restored correctly.

We can also read the file contents of the very first block (block 0), which is the boot sector:
```
[   27.526212] rtl9210: READ(10) CBW sent
[   27.526946] rtl9210: READ(10) data received
[   27.528011] rtl9210: READ(10) CSW received
[   27.528016] rtl9210: 00000000: fa b8 00 10 8e d0 bc 00 b0 b8 00 00 8e d8 8e c0  ................
[   27.528021] rtl9210: 00000010: fb be 00 7c bf 00 06 b9 00 02 f3 a4 ea 21 06 00  ...|.........!..
[   27.528023] rtl9210: 00000020: 00 be be 07 38 04 75 0b 83 c6 10 81 fe fe 07 75  ....8.u........u
[   27.528025] rtl9210: 00000030: f3 eb 16 b4 02 b0 01 bb 00 7c b2 80 8a 74 01 8b  .........|...t..
[   27.528026] rtl9210: 00000040: 4c 02 cd 13 ea 00 7c 00 00 eb fe 00 00 00 00 00  L.....|.........
[   27.528028] rtl9210: 00000050: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528029] rtl9210: 00000060: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528031] rtl9210: 00000070: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528032] rtl9210: 00000080: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528034] rtl9210: 00000090: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528035] rtl9210: 000000a0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528036] rtl9210: 000000b0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528038] rtl9210: 000000c0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528039] rtl9210: 000000d0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528040] rtl9210: 000000e0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528042] rtl9210: 000000f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528043] rtl9210: 00000100: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528045] rtl9210: 00000110: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528046] rtl9210: 00000120: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528048] rtl9210: 00000130: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528049] rtl9210: 00000140: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528050] rtl9210: 00000150: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528052] rtl9210: 00000160: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528053] rtl9210: 00000170: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528054] rtl9210: 00000180: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528056] rtl9210: 00000190: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528057] rtl9210: 000001a0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528059] rtl9210: 000001b0: 00 00 00 00 00 00 00 00 51 8a ee 13 00 00 00 05  ........Q.......
[   27.528060] rtl9210: 000001c0: 05 01 07 fe ff ff 00 40 00 00 00 18 70 74 00 00  .......@....pt..
[   27.528062] rtl9210: 000001d0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528063] rtl9210: 000001e0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
[   27.528065] rtl9210: 000001f0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 55 aa  ..............U.
```
which is marked by the boot signature `55 aa` at the last 2 bytes.

# Opcodes
The SCSI commands that are commonly sent by the device include `TEST_UNIT_READY`, `INQUIRY`, `MODE_SENSE`, `READ_CAPACITY`, `READ_10`, `WRITE_10`, `SYNCHRONIZE_CACHE`, `REPORT_LUNS`, and `ATA12`. These are the only commands that my `queuecommand` recognizes right now, although I can research and add more supported commands if necessary.  

The commands that are not supported by the hardware (that I found out so far) include `MODE_SENSE_10` and `MAINTENANCE_IN`. When URBs are sent for these commands, instead of returning properly it stalls the endpoint with a `-EPIPE` status. So I had to make `queuecommand` flag `DID_ERROR`, signal `scsi_done()`, and return right away when given these commands. 

# SCSI
The next step is to register our driver to the SCSI subsystem. The SCSI subsystem consists of three layers: upper layer driver, lower layer driver, and the SCSI midlayer. The SCSI midlayer is responsible for receiving requests from the higher layer (eg. filesystem) and sending appropriate SCSI commands to the lower layer driver. Up to this point we have proven that our driver can execute basic SCSI commands through `probe()`, but now we want a way to receive commands from the SCSI midlayer as a lower layer driver.

The first step is to implement an SCSI host in our driver so it can identify our device. But what is a SCSI host and device?

A SCSI host is responsible for transporting SCSI commands from the midlayer to the device. It is capable of speaking SCSI to multiple targets and LUNs (although the host in our driver only speaks to 1 target). We use `struct Scsi_Host` as an instance of a host in our driver.

A SCSI device is the actual hardware discovered through the host. Each device is identified by SCSI addressing scheme [Host:Bus:Target:Lun], which can be seen from our `dmesg` and `lsscsi` as [1:0:0:0].

I configured our host settings through the `struct scsi_host_template`. I then initialized a new `struct Scsi_Host` through `scsi_host_alloc`, which prepares the instance but doesn't register it to the SCSI subsystem until `scsi_add_host()` is called. After calling `scsi_add_host()`, our driver calls `scsi_scan_host()` to discover all devices associated with the host. The SCSI midlayer will then call the function provided under the `.queuecommand` field in `struct scsi_host_template` to send SCSI commands. At `disconnect()`, `scsi_remove_host()` (unregisters host from SCSI subsystem) and `scsi_host_put()` (frees host allocation) are added.

I also noticed we can use the `.cmd_size` field to store per-command data. Although our driver currently executes 1 command at a time (`.can_queue = 1`), I moved the critical data to a separate `struct cmd_priv` in case I want to implement concurrent SCSI command executions in the future. Each command now has their own private bulk wraps and data buffers, which prevents race conditions with SCSI commands executing concurrently.

# Async
Once we registered our driver to the SCSI subsystem, the SCSI commands are now issued through `queuecommand()` only. When we test our driver if the commands are issued, we get the following error:

```
[  281.068862] ------------[ cut here ]------------
[  281.068865] Voluntary context switch within RCU read-side critical section!
...
[  281.068929] Call Trace:
[  281.068931]  <TASK>
[  281.068934]  ? vhci_rx_loop+0x2806/0x2df0 [vhci_hcd]
[  281.068940]  __schedule+0xae/0xaa0
[  281.068947]  ? usb_hcd_submit_urb+0x9d/0xc30 [usbcore]
[  281.068963]  schedule+0x63/0xe0
[  281.068966]  schedule_timeout+0x14a/0x160
[  281.068969]  wait_for_completion+0x8c/0x170
[  281.068972]  rtl9210_queuecommand+0x1f6/0x640 [rtl9210]
[  281.068979]  scsi_queue_rq+0x3aa/0xc60
[  281.068984]  blk_mq_dispatch_rq_list+0x1b9/0x7e0
```

`blk_mq_dispatch_rq_list` is the function from the SCSI midlayer, and is also the caller several frames up from `queuecommand`. From the error message we can tell that that this function calls `queuecommand` within a RCU read-side critical section. This means that no context switching can happen within that section, because context switching might possibly update RCU-protected data (which was the data the old thread was running on).  

The function `wait_for_completion()` puts the current thread to sleep until the condition is satisfied (calling `complete()`). We used this previously after every `usb_submit_urb()` because we wanted to make the driver wait until moving on to submitting the next urb (aka being 'synchronous'), since we must submit (CBW->data->CSW) URBs in order for the BOT protocol to run successfully.

The problem is that putting the current thread to sleep leads to an automatic context switch. This wasn't an issue when we didn't register the driver to the SCSI subsystem, but now that we registered our device and issue commands through `queuecommand()` we can no longer execute commands in a blocking context (since `queuecommand()` is called within a critical section). We must now execute all commands in a non-blocking, or "atomic context".

So, I changed the code so that all commands execute the same general-purpose functions `submit_async()` and `async_complete()`, which are non-blocking. `queuecommand()` calls `submit_async()`, then `submit_async()` fills out the `struct bulk_cs_wrap` and submits the URB. The `urb_complete()` is now replaced with the new `async_complete()`. All functions calling `usb_submit_urb()` return immediately, which is not an issue anymore because we can never reach the next `usb_submit_urb()` until the current URB submission is completed. When the URB submission is complete, the SCSI midlayer invokes a callback to `async_complete()`, which then submits the next URB or calls `scsi_done()`. So now we don't have to worry about multiple URB submissions, nor do we need to use `wait_for_completion()`. It does not put any threads to sleep, so the "RCU read-side critical section" error does not appear anymore when tested.

I have also added a `struct work_struct` under the per-device state and a new `clear_halt_work()`. This function is called in the case where an URB failed (returned bad status) and clears the URBs through `usb_clear_halt()`. `usb_clear_halt()` is actually not safe to use in an atomic context, so we use `schedule_work()` to schedule the `clear_halt_work()` function call and let the next available `kworker` thread execute it. `kworker` threads are kernel threads that can run in atomic contexts. 

# Commands
Identifying the Bus-Port
------------------------
After plugging in the device, run `lsusb` to find the device. We are looking for something that looks like `Bus 003 Device 009: ID 0bda:9210 Realtek Semiconductor Corp. RTL9210 M.2 NVME Adapter`, where the device ID is `0bda:9210`. Then, run this block of code to find the bus-port prefix:
```
for d in /sys/bus/usb/devices/*/idVendor; do
  dir=$(dirname "$d")
  echo "$dir: $(cat "$d" 2>/dev/null):$(cat "$dir/idProduct" 2>/dev/null)"
done | grep -i 0bda:9210
```
In my case, I get `3-4`. The interface we want to bind/unbind has the suffix `1.0`, so the bus-port becomes `3-4:1.0`. Then replace my `3-4:1.0` bus-ports with yours in the last 2 echo commands in `bind_rtl9210.sh`:
```
echo "3-4:1.0" | sudo tee /sys/bus/usb/drivers/usb-storage/unbind
echo "3-4:1.0" | sudo tee /sys/bus/usb/drivers/rtl9210/bind
```
Now we can run the bash script `bind_rtl9210.sh` to insert the driver module. You will see the status of the driver through the `rtl9210:` messages. If you don't want to clutter your terminal with the giant diagnostic messages, feel free to modify or remove the `sudo dmesg -w` line in the bash script.

Inserting the Driver Module
---------------------------
To insert the driver module, run the following commands (or simply run `./bind_rtl9210.sh`):
```
sudo umount /dev/sde1 2>/dev/null  
sudo rmmod rtl9210  
echo "exit code: $?"  
lsmod | grep rtl9210
make clean && make
sudo insmod src/rtl9210.ko  
```

Also execute these commands to manually bind the driver and make sure the provided driver didn't take over:
```
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/usb-storage/unbind  
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/rtl9210/bind  
sudo dmesg -w   # optional
```
Make sure to replace `2-1:1.0` with your system's bus-port.  
Execute these commands also when the driver code is modified/updated.

Disk and Partition
------------------
To view the list of SCSI devices, run `lsscsi`.  
I got something along the lines of  
```
WSL2
[0:0:0:0]    disk    Msft     Virtual Disk     1.0   /dev/sda  
[0:0:0:1]    disk    Msft     Virtual Disk     1.0   /dev/sdb  
[0:0:0:2]    disk    Msft     Virtual Disk     1.0   /dev/sdc  
[0:0:0:3]    disk    Msft     Virtual Disk     1.0   /dev/sdd  
[1:0:0:0]    disk    Realtek  RTL9210 NVME     1.00  /dev/sde

Arch Linux
[0:0:0:0]    disk    Realtek  RTL9210 NVME     1.00  /dev/sda 
[N:0:6:1]    disk    SAMSUNG MZVL21T0HCLR-00B00__1              /dev/nvme0n1
```  
The disk `/dev/sde` (or `/dev/sda` in Arch) is the actual device plugged in.

Then, to view the partition level detain, run `lsblk /dev/sdX`, replacing `sdX` with your disk number (eg. `lsblk /dev/sde`).  
I got something like  
```
WSL2
NAME   MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS  
sde      8:64   0 931.5G  0 disk  
└─sde1   8:65   0 931.5G  0 part

Arch Linux
NAME   MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS
sda      8:0    0 931.5G  0 disk 
└─sda1   8:1    0 931.5G  0 part /run/media/handyk/ex_ssd
```  
Where `sde1` and `sda1` represents the partition under the disk.

Allocating the Filesystem
-------------------------
We must allocate our storage device with a filesystem compatible with Linux. In my case, I went with ext4.  
Run the following line to allocate the ext4 filesystem on your partition:
```
sudo mkfs.ext4 /dev/sdX1  # sdX1 replaced with your partition
```

I also installed `exfat` on my SSD through the windows disk manager, which works fine with our driver when tested.

Mount Test
----------
Run the following commands to test mounting your partition:
```
sudo mkdir -p /mnt/test         # create mount point  
sudo mount /dev/sde1 /mnt/test  # mount partition  
ls -la /mnt/test                # list all files
```  

Basic Read/Write Test
---------------------
The following commands should print "hello world" 3 times in total:
```
echo "hello world" | sudo tee /mnt/test/hello.txt   # first hello world print  
cat /mnt/test/hello.txt                             # second hello world print  
sync  
sudo umount /mnt/test  
sudo mount /dev/sdX1 /mnt/test                      # sdX1 replaced with your partition  
cat /mnt/test/hello.txt                             # third hello world print
```
