Once our driver is done running probe(), we run into a problem. The driver provided from WSL2 (usb-storage) takes over our driver. This can be seen from our dmesg:

`[ 4853.001115] usb-storage 2-1:1.0: USB Mass Storage device detected`  
`[ 4853.001868] usb-storage 2-1:1.0: device ignored`  
`[ 4853.030856] usbcore: registered new interface driver usb-storage`

This explains why the device shows up in `/dev` even when our driver does not wire the device into the kernel's SCSI subsystem yet. The default driver takes over after our driver is done running, so we want to prevent this usb-storage from taking over.

We blacklist usb-storage by adding this code under `/etc/modprobe.d/blacklist-rtl9210-usbstorage.conf`:

`options usb-storage quirks=0bda:9210:i`

where the `i` quirk flag tells `usb-storage` to ignore that specific `vendor:product ID`. Rebooting WSL2 from now on will make `usb-storage` ignore our device, so that only our driver can interact with it.
