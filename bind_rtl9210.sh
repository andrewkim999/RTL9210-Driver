#!/bin/bash
sudo rmmod driver 2>/dev/null
sudo insmod driver.ko
dmesg
