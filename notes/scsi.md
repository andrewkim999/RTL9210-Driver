The next step is to register our driver to the SCSI subsystem. The SCSI subsystem consists of three layers: upper layer driver, lower layer driver, and the SCSI midlayer. The SCSI midlayer is responsible for receiving requests from the higher layer (eg. filesystem) and sending appropriate SCSI commands to the lower layer driver. Up to this point we have proven that our driver can execute basic SCSI commands through the probe() function, but now we want a way to receive commands from the SCSI midlayer as a lower layer driver. 

Host vs Device
--------------
A SCSI host is responsible for transporting SCSI commands to the device. It is capable of speaking SCSI to multiple targets and LUNs (although the host in our driver only speaks to 1 target). Our Scsi_Host struct is the single instance of a host that acts as the bridge between the CPU and the device.

A SCSI device is the actual hardware discovered through the host. Each device is identified by SCSI addressing scheme [Host:Bus:Target:Lun], which can be seen from our dmesg and lsscsi as [1:0:0:0].  
