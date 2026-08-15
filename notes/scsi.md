The next step is to register our driver to the SCSI subsystem. The SCSI subsystem consists of three layers: upper layer driver, lower layer driver, and the SCSI midlayer. The SCSI midlayer is responsible for receiving requests from the higher layer (eg. filesystem) and sending appropriate SCSI commands to the lower layer driver. Up to this point we have proven that our driver can execute basic SCSI commands through `probe`, but now we want a way to receive commands from the SCSI midlayer as a lower layer driver. 

Host vs Device
--------------
A SCSI host is responsible for transporting SCSI commands to the device. It is capable of speaking SCSI to multiple targets and LUNs (although the host in our driver only speaks to 1 target). Our `struct Scsi_Host` is the single instance of a host that acts as the bridge between the kernel and the device.

A SCSI device is the actual hardware discovered through the host. Each device is identified by SCSI addressing scheme [Host:Bus:Target:Lun], which can be seen from our `dmesg` and `lsscsi` as [1:0:0:0].

Code
----
We configure our host settings through the `host_template`. We then initialize a new `struct Scsi_Host` through `scsi_host_alloc`, which prepares the instance but doesn't register it to the SCSI subsystem until `scsi_add_host` is called. After calling `scsi_add_host`, we call `scsi_scan_host` to discover all devices associated with the host. The SCSI midlayer will then call the function provided under the `.queuecommand` field in `host_template` to send SCSI commands. At `disconnect`, `scsi_remove_host` (unregisters host from SCSI subsystem) and `scsi_host_put` (frees host allocation) are added.
