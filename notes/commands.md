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
