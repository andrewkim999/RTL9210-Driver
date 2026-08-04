Once our driver is done running probe(), we run into a problem. The driver provided from WSL2 (usb-storage) takes over our driver. This can be seen from our dmesg:

[ 4853.001115] usb-storage 2-1:1.0: USB Mass Storage device detected
[ 4853.001868] usb-storage 2-1:1.0: device ignored
[ 4853.030856] usbcore: registered new interface driver usb-storage

This explains why the device shows up in /dev even when our driver does not wire the device into the kernel's SCSI subsystem yet. The default driver takes over after our driver is done running, so we want to prevent this usb-storage from taking over.

In my case, the device shows up as the following when running "lsblk -S":

NAME
    HCTL       TYPE VENDOR   MODEL                    REV SERIAL                           TRAN
sda 0:0:0:0    disk Msft     Virtual Disk             1.0 60022480b85a9136c0b1d97945cba312
sdb 0:0:0:1    disk Msft     Virtual Disk             1.0 600224800d8cfbd1bd14b15dd978686e
sdc 0:0:0:2    disk Msft     Virtual Disk             1.0 600224801dd2b10f145d49033909bf96
sdd 0:0:0:3    disk Msft     Virtual Disk             1.0 600224807c949bc3a9092249700bae85
sde 1:0:0:0    disk Realtek  WD_BLACK SN7100 1TB 7615M0WD 25404P800456                     usb

"sde" here is not the device connected through my driver, it is from the usb-storage driver loaded.

We blacklist usb-storage by adding the following command under /etc/modprobe.d/blacklist-rtl9210-usbstorage.conf:

options usb-storage quirks=0bda:9210:i

where the i quirk flag tells usb-storage to ignore that specific vendor:product ID. Rebooting WSL2 from now on will make usb-storage ignore our device, so that only our driver can interact with it.

Every time we rebuild our driver, we have to unload and reload the updated driver manually. Run the following command when building:

sudo rmmod rtl9210; make && sudo insmod rtl9210.ko
