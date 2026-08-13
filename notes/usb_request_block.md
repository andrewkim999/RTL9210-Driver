URB (USB Request Block) is the layer below the tranfer protocol. If BOT and UAS are the ways to organize a delivery truck system, URB is the container on the truck itself. URB contains all the necessary information to execute USB transactions, whether that is command, data IN and OUT, or status.

For every endpoint being used, we need an URB. For BOT we allocate 2 URBs to send and receive data sequentially. For UAS we allocate 3 URBs to send and receive data in parallel.

Inquiry Operation
=================
BOT
---
* bulk-out URB -> EP 2 OUT (send inquiry command)
* bulk-in URB  <- EP 1 IN  (receive data)
* bulk-in URB  <- EP 1 IN  (receive status)

UAS
---
* command URB -> EP 4 OUT (send inquiry command)
* data IN URB <- EP 1 IN  (receive data)
* status URB  <- EP 3 IN  (receive status)

Read Operation
==============
BOT
---
* bulk-out URB -> EP 2 OUT (send read command)
* bulk-in URB  <- EP 1 IN  (receive data)
* bulk-in URB  <- EP 1 IN  (receive status)

UAS
---
* command URB -> EP 4 OUT (send read command)
* data IN URB <- EP 1 IN  (receive data)
* status URB  <- EP 3 IN  (receive status)

Write Operation
===============
BOT
---
* bulk-out URB -> EP 2 OUT (send write command)
* bulk-out URB -> EP 2 OUT (send data)
* bulk-in URB  <- EP 1 IN  (receive status)

UAS
---
* command URB  -> EP 4 OUT (send write command)
* data OUT URB -> EP 2 OUT (send the data)
* status URB   <- EP 3 IN  (receive completion status)

We allocate the URBs using usb_alloc_urb() in probe(). These URBs are only freed upon reaching disconnect(). We then initialize the URBs using usb_fill_bulk_urb() in inquiry()/read()/write() which provides the usb device pointer, pipe, transfer buffer, desired transfer length, completion handler, and context. The URB is submitted through usb_submit_urb().
