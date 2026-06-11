The USB (Universal Serial Bus) requires a protocol to communicate with the kernel. We have two options: BOT (Bulk-Only Transport) and UASP (USB Attached SCSI).

BOT (Bulk-Only Transport)
-------------------------
Host sends a command block wrapper (CBW), which is a BOT-specific envelope. The device then sends/receives the data, and sends a command status wrapper (CSW) to report the status. BOT only needs 2 endpoints, IN and OUT, since it sends and receives commands, data, and statuses sequentially. The problem with this protocol is that it is very slow, because the host must wait until the device reports the status before sending the next command. This is due to BOT being developed a very long time ago (1990s), so it has limited performance. It is however, more versatile and can be used by almost any device.

UAS (USB Attached SCSI)
-----------------------
UAS sends commands in parallel, unlike BOT. Up to 32 commands can be sent simultaneously. How does it do this? It's due to UAS using 4 endpoints (command, data IN, data OUT, status) instead of 2. While one command is being sent to the host, the host can be sending data for another command. This improves performance significantly if there are many different operations queued (eg. read after write after read). Only compatible with USB 3.0 and up.

In our device, the default protocol selected is BOT, and this can be seen at bInterfaceProtocol under the Interface Descriptor. We change the protocol from BOT to UAS immediately in our prove function to improve performance and reduce latency.
