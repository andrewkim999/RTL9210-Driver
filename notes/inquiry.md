The very first transfer to be sent will be the inquiry command. The inquiry command is an SCSI command that makes the device identify itself. Our inquiry data provides the vendor, product, and revision. Inquiry is the simplest possible read operation that we implement before implementing actual read/write commands.

Since we are using BOT to transfer data, we use the command block wrapper (bulk_cb_wrap) to request data and command status wrapper (bulk_cs_wrap) to receive status. These are structs provided under storage.h in the Linux kernel. With BOT, we send requests and receive data sequentially. The sequence goes like:

**send CBW -> wait -> receive data -> wait -> receive CSW -> wait -> done**

Notice that there are delays that must happen between each steps due to the time taken to transfer. We must provide these delays through the use of the completion struct. Otherwise the driver will not wait for the transfer and move to the next step immediately.

There are 2 threads running during this part of the inquiry code. Thread 1 runs the driver code, and thread 2 runs the USB interrupt handler. When thread 1 calls usb_submit_urb(), the USB host controller (thread 2) handles the transfer in the background using interrupts. We must stop the driver code from filling and submitting a new URB before the transfer completes for the previous URB. Thus we make thread 1 sleep until the transfer is finished. The wait_for_completion function allows thread 1 to sleep until thread 2 finishes the transfer and calls the urb_complete function. 

Correction: thread 2 is not a regular thread, it's an interrupt context 
