# Commands

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
dmesg   # optional
```

Also execute these commands to manually bind the driver and make sure the provided driver didn't take over:

```
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/usb-storage/unbind  
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/rtl9210/bind  
dmesg   # optional
```

Note: Execute these commands also when the driver code is modified/updated.

Disk and Partition
-----------------
To view the list of SCSI devices, run `lsscsi`.  
I got something along the lines of  
```
[0:0:0:0]    disk    Msft     Virtual Disk     1.0   /dev/sda  
[0:0:0:1]    disk    Msft     Virtual Disk     1.0   /dev/sdb  
[0:0:0:2]    disk    Msft     Virtual Disk     1.0   /dev/sdc  
[0:0:0:3]    disk    Msft     Virtual Disk     1.0   /dev/sdd  
[1:0:0:0]    disk    Realtek  RTL9210 NVME     1.00  /dev/sde
```  

The 5th disk `/dev/sde` is the actual device plugged in.

Then, to view the partition level detain, run 'lsblk /dev/sde'.  
I got something like  
```
NAME   MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS  
sde      8:64   0 931.5G  0 disk  
└─sde1   8:65   0 931.5G  0 part
```  

Where `sde1` represents the partition under the disk.

Allocating the Filesystem (ext4)
--------------------------------
We must allocate our storage device with a filesystem compatible with Linux. In my case, I went with ext4.  
Run the following line to allocate the ext4 filesystem on your partition:

```
sudo mkfs.ext4 /dev/sde1
```  

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
sudo mount /dev/sde1 /mnt/test  
cat /mnt/test/hello.txt                             # third hello world print
```

# Bot vs UAS

The USB (Universal Serial Bus) requires a protocol to communicate with the kernel. We have two options: BOT (Bulk-Only Transport) and UASP (USB Attached SCSI).

BOT (Bulk-Only Transport)  
Host sends a command block wrapper (CBW), which is a BOT-specific envelope. The device then sends/receives the data, and sends a command status wrapper (CSW) to report the status. BOT only needs 2 endpoints, IN and OUT, since it sends and receives commands, data, and statuses sequentially. The problem with this protocol is that it is very slow, because the host must wait until the device reports the status before sending the next command. This is due to BOT being developed a very long time ago (1990s), so it has limited performance. It is however, more versatile and can be used by almost any device.

UAS (USB Attached SCSI)  
UAS sends commands in parallel, unlike BOT. Up to 32 commands can be sent simultaneously. How does it do this? It's due to UAS using 4 endpoints (command, data IN, data OUT, status) instead of 2. While one command is being sent to the host, the host can be sending data for another command. This improves performance significantly if there are many different operations queued (eg. read after write after read). This feature is called streams, which is only compatible with USB 3.0 and up.

One thing to note is that we are using vhci_hcd to test our driver, which is a virtual USB controller provided by our WSL2. vhci_hcd does not support streams required by UAS, so in this case we choose to use BOT due to hardware limitations.

# USB Request Block
URB (USB Request Block) is the layer below the tranfer protocol. If BOT and UAS are the ways to organize a delivery truck system, URB is the container on the truck itself. URB contains all the necessary information to execute USB transactions, whether that is command, data IN and OUT, or status.

For every endpoint being used, we need an URB. For BOT we allocate 2 URBs to send and receive data sequentially. For UAS we allocate 3 URBs to send and receive data in parallel.

Inquiry:
BOT:
```
bulk-out URB -> EP 2 OUT (send inquiry command)
bulk-in URB  <- EP 1 IN  (receive data)
bulk-in URB  <- EP 1 IN  (receive status)
```

UAS:
```
command URB -> EP 4 OUT (send inquiry command)
data IN URB <- EP 1 IN  (receive data)
status URB  <- EP 3 IN  (receive status)
```

Read:
BOT:
```
bulk-out URB -> EP 2 OUT (send read command)
bulk-in URB  <- EP 1 IN  (receive data)
bulk-in URB  <- EP 1 IN  (receive status)
```

UAS
```
command URB -> EP 4 OUT (send read command)
data IN URB <- EP 1 IN  (receive data)
status URB  <- EP 3 IN  (receive status)
```

Write:
BOT
```
bulk-out URB -> EP 2 OUT (send write command)
bulk-out URB -> EP 2 OUT (send data)
bulk-in URB  <- EP 1 IN  (receive status)
```

UAS
```
command URB  -> EP 4 OUT (send write command)
data OUT URB -> EP 2 OUT (send data)
status URB   <- EP 3 IN  (receive status)
```

We allocate the URBs using `usb_alloc_urb()` in `probe()`. These URBs are only freed upon reaching `disconnect()`. We then initialize the URBs using `usb_fill_bulk_urb()` in `inquiry()`/`read()`/`write()` which provides the USB device pointer, pipe, transfer buffer, desired transfer length, completion handler, and context. The URB is submitted through `usb_submit_urb()`.

# Inquiry
The very first transfer to be sent will be the inquiry command. The inquiry command is an SCSI command that makes the device identify itself. Our inquiry data provides the vendor, product, and revision. Inquiry is the simplest possible read operation that we implement before implementing actual read/write commands.

Since we are using BOT to transfer data, we use the command block wrapper `struct bulk_cb_wrap` to request data and command status wrapper `struct bulk_cs_wrap` to receive status. These are structs provided under `storage.h` in the kernel. With BOT, we send requests and receive data sequentially. The sequence goes like:

```
send CBW -> wait -> receive data -> wait -> receive CSW -> wait -> done
```

Notice that there are delays that must happen between each steps due to the time taken to transfer. We must provide these delays through the use of the completion struct. Otherwise the driver will not wait for the transfer and move to the next step immediately.

There are 2 threads running during this part of the inquiry code. Thread 1 runs the driver code, and thread 2 runs the USB interrupt handler. When thread 1 calls `usb_submit_urb()`, the USB host controller (thread 2) handles the transfer in the background using interrupts. We must stop the driver code from filling and submitting a new URB before the transfer completes for the previous URB. Thus we make thread 1 sleep until the transfer is finished. `wait_for_completion()` allows thread 1 to sleep until thread 2 finishes the transfer and calls `urb_complete()`. 

Correction: thread 2 is not a regular thread, it's an interrupt context

# Read Capacity
Before writing READ, we start with writing READ_CAPACITY. We need to know two things before issuing a correct read:
1. Block size - we need to know how many bytes a block is (usually 512 or 4096 bytes for NVMe)
2. Max LBA (logical block address) - this is the last addressable logical block, which is needed so we know what the valid address range is and avoid requesting an out-of-bounds block.

READ_CAPACITY_10 is a standard way to get both and is structurally almost identical to our INQUIRY function. This time, instead of a 96 byte response, we get an 8 byte response. The first 4 bytes being the last LBA (in big endian), and the last 4 bytes being the block length in bytes (in big endian).

We can calculate the total capacity of the SSD in bytes by simply multiplying the number of logical blocks and the block size:  
```
capacity = (max LBA + 1) * (block size)
```

In my case I used a 1 TB SSD, so we can double check if the capacity matches.

After sending our READ CAPACITY(10), our response reads:  
```
max LBA=1953525167, block size=512 bytes  
capacity=1000204886016 bytes
```

Which equates to:  
```
1000204886016/(1000^3) = 1.0002 TB in decimal  
1000204886016/(1024^3) = 931.5 GB in binary
```

# Remove Default Driver
Once our driver is done running probe(), we run into a problem. The driver provided from WSL2 (usb-storage) takes over our driver. This can be seen from our dmesg:

```
[ 4853.001115] usb-storage 2-1:1.0: USB Mass Storage device detected  
[ 4853.001868] usb-storage 2-1:1.0: device ignored  
[ 4853.030856] usbcore: registered new interface driver usb-storage
```

This explains why the device shows up in `/dev` even when our driver does not wire the device into the kernel's SCSI subsystem yet. The default driver takes over after our driver is done running, so we want to prevent this usb-storage from taking over.

We blacklist usb-storage by adding this code under `/etc/modprobe.d/blacklist-rtl9210-usbstorage.conf`:

```
options usb-storage quirks=0bda:9210:i
```

where the `i` quirk flag tells `usb-storage` to ignore that specific `vendor:product ID`. Rebooting WSL2 from now on will make `usb-storage` ignore our device, so that only our driver can interact with it.

# SCSI
The next step is to register our driver to the SCSI subsystem. The SCSI subsystem consists of three layers: upper layer driver, lower layer driver, and the SCSI midlayer. The SCSI midlayer is responsible for receiving requests from the higher layer (eg. filesystem) and sending appropriate SCSI commands to the lower layer driver. Up to this point we have proven that our driver can execute basic SCSI commands through `probe()`, but now we want a way to receive commands from the SCSI midlayer as a lower layer driver.

The first step is to implement an SCSI host in our driver so it can identify our device. But what is a SCSI host and device?

A SCSI host is responsible for transporting SCSI commands from the midlayer to the device. It is capable of speaking SCSI to multiple targets and LUNs (although the host in our driver only speaks to 1 target). We use `struct Scsi_Host` as an instance of a host in our driver.

A SCSI device is the actual hardware discovered through the host. Each device is identified by SCSI addressing scheme [Host:Bus:Target:Lun], which can be seen from our `dmesg` and `lsscsi` as [1:0:0:0].

We configure our host settings through the `struct scsi_host_template`. We then initialize a new `struct Scsi_Host` through `scsi_host_alloc`, which prepares the instance but doesn't register it to the SCSI subsystem until `scsi_add_host()` is called. After calling `scsi_add_host()`, we call `scsi_scan_host()` to discover all devices associated with the host. The SCSI midlayer will then call the function provided under the `.queuecommand` field in `struct scsi_host_template` to send SCSI commands. At `disconnect()`, `scsi_remove_host()` (unregisters host from SCSI subsystem) and `scsi_host_put()` (frees host allocation) are added.

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
