The next step is to connect the device through the SCSI layer.

Host vs Device
--------------
A SCSI host is responsible for transporting SCSI commands to the device. It is capable of speaking SCSI to multiple targets and LUNs (although the host in our driver only speaks to 1 target). Our Scsi_Host struct is the single instance of a host that acts as the bridge between the CPU and the device.

A SCSI device is the actual hardware discovered through the host. Each device is identified by SCSI addressing scheme [Host:Bus:Target:Lun], which can be seen from our dmesg and lsscsi as [1:0:0:0].  
