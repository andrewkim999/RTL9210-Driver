We compared BOT and UAS, but what do these protocols actually transfer data from the driver to the kernel? URB (USB Request Block) is the layer below the tranfer protocol. If BOT and UAS are the ways to organize a delivery truck system, URB is the container on the truck itself. URB contains all the necessary information to execute USB transactions, whether that is command, data IN and OUT, or status.

For every endpoint being used, we need an URB. In our driver, we allocate 3 URBs, because a read or write operation uses a maximum of 3 endpoints at a time. The details are shown below.

Read Operation
--------------
command URB -> EP 4 OUT (send read command)
data IN URB <- EP 1 IN  (receive the data)
status URB  <- EP 3 IN  (receive completion status)

Write Operation
---------------
command URB  -> EP 4 OUT (send write command)
data OUT URB -> EP 2 OUT (send the data)
status URB   <- EP 3 IN  (receive completion status)

We allocate the URBs using the usb_alloc_urb function from the linux kernel. According to "[PATCH v3 1/2] USB: core: add a memory pool to urb caching host-controller private data", this function has been changed so that it creates a mempool attached to an URB, instead of allocating and freeing kernel memory every URB use. This way we guarantee memory efficiency during our URB usage.
