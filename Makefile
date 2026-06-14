KDIR := /home/andrewkim999/test/wsl2-kernel

obj-m += src/rtl9210.o

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
