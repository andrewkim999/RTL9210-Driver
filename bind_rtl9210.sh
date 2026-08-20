#!/bin/bash
sudo umount /dev/sde1 2>/dev/null
sudo rmmod rtl9210
echo "exit code: $?"
lsmod | grep rtl9210

make clean && make
sudo insmod src/rtl9210.ko

echo "3-4:1.0" | sudo tee /sys/bus/usb/drivers/usb-storage/unbind
echo "3-4:1.0" | sudo tee /sys/bus/usb/drivers/rtl9210/bind
sudo dmesg -w
