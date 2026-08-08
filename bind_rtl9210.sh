#!/bin/bash
make clean
sudo rmmod rtl9210 2>/dev/null
make
sudo insmod src/rtl9210.ko
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/usb-storage/unbind
echo "2-1:1.0" | sudo tee /sys/bus/usb/drivers/rtl9210/bind
dmesg
