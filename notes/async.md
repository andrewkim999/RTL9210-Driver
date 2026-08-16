Once we registered our driver to the SCSI subsystem, the SCSI commands are now issued through the `queuecommand` function only. When we test our driver if the commands are issued, we get the following error:
```
[  281.068862] ------------[ cut here ]------------
[  281.068865] Voluntary context switch within RCU read-side critical section!
...
[  281.068929] Call Trace:
[  281.068931]  <TASK>
[  281.068934]  ? vhci_rx_loop+0x2806/0x2df0 [vhci_hcd]
[  281.068940]  __schedule+0xae/0xaa0
[  281.068947]  ? usb_hcd_submit_urb+0x9d/0xc30 [usbcore]
[  281.068963]  schedule+0x63/0xe0
[  281.068966]  schedule_timeout+0x14a/0x160
[  281.068969]  wait_for_completion+0x8c/0x170
[  281.068972]  rtl9210_queuecommand+0x1f6/0x640 [rtl9210]
[  281.068979]  scsi_queue_rq+0x3aa/0xc60
[  281.068984]  blk_mq_dispatch_rq_list+0x1b9/0x7e0
```

`blk_mq_dispatch_rq_list` is the function from the SCSI midlayer, and is also the caller several frames up from `queuecommand`. From the error message we can tell that that this function calls `queuecommand` within a RCU read-side critical section. This means that no context switching can happen within that section, because context switching might possibly update RCU-protected data (which was the data the old thread was running on).  

The function `wait_for_completion` puts the current thread to sleep until the condition is satisfied (calling the `complete` function). We used this previously after every `usb_submit_urb` because we wanted to make the driver wait until moving on to submitting the next urb (aka being 'synchronous'), since we must submit (CBW->data->CSW) URBs in order for the BOT protocol to run successfully.

The problem is that putting the current thread to sleep leads to an automatic context switch. This wasn't an issue when we didn't register the driver to the SCSI subsystem, but now that we registered our device and issue commands through `queuecommand` we can no longer execute commands in a blocking context (since `queuecommand` is called within a critical section). We must now execute all commands in a non-blocking, or "atomic context".

So, I changed the code so that all commands execute the same general-purpose functions `submit_async` and `async_complete`, which are non-blocking. The `queuecommand` function calls `submit_async`, then `submit_async` fills out the `struct bulk_cs_wrap` and submits the URB. The `urb_complete` is now replaced with the new `async_complete`. All functions calling `usb_submit_urb` return immediately, which is not an issue anymore because we can never reach the next `usb_submit_urb` until the current URB submission is completed. When the URB submission is complete, the SCSI midlayer invokes a callback to `async_complete`, which then submits the next URB or signal `scsi_done`. So now we don't have to worry about multiple URB submissions, nor do we need to use `wait_for_completion`. It does not put any threads to sleep, so the "RCU read-side critical section" error does not appear anymore when tested.

I have also added a `struct work_struct` under the per-device state and a function `clear_halt_work`. This function is called in the case where an URB failed (returned bad status) and clears the URBs through `usb_clear_halt`. `usb_clear_halt` is actually not safe to use in an atomic context, so we use `schedule_work` to schedule the `clear_halt_work` function call and let the next available `kworker` thread execute it. `kworker` threads are kernel threads that can run in atomic contexts. 
