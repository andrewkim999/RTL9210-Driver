#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/uas.h>
#include <linux/usb/storage.h>

#include <scsi/scsi_proto.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>

#define RTL_VENDOR_ID	0x0bda
#define RTL_PRODUCT_ID	0x9210
#define USB_PR_BULK		0x50
#define USB_PR_UAS		0x62

#define INQUIRY_REPLY_LEN       96
#define READ_CAPACITY_REPLY_LEN 8

static u32 max_lba, blk_size;

/* per device state */
struct rtl9210_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct Scsi_Host *shost;

	struct usb_host_endpoint *data_in;	// EP1 IN  0x81
	struct usb_host_endpoint *data_out;	// EP2 OUT 0x02
	
	struct urb *bulk_in_urb;
	struct urb *bulk_out_urb;
};

enum rtl9210_phase { PHASE_CBW, PHASE_DATA, PHASE_CSW };

/* per command state (allocated by midlayer via .cmd_size) */
struct rtl9210_cmd_priv {
	struct rtl9210_dev *dev;
	enum rtl9210_phase phase;
	
	struct bulk_cb_wrap *cbw;
	struct bulk_cs_wrap *csw;
	void *data_buf;

	u32 len;
	u8 direction;
};

/* endpoint parsing function */
static int rtl9210_find_endpoints(struct rtl9210_dev *dev) 
{
	struct usb_host_interface *alt = dev->intf->cur_altsetting;
	struct usb_endpoint_descriptor *ep;

	for (int i = 0; i < alt->desc.bNumEndpoints; i++) {
		ep = &alt->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(ep) && ep->bEndpointAddress == 0x81)
			dev->data_in = &alt->endpoint[i];
		else if (usb_endpoint_is_bulk_out(ep) && ep->bEndpointAddress == 0x02)
			dev->data_out = &alt->endpoint[i];
	}

	if (!dev->data_in || !dev->data_out) {
		printk(KERN_ERR "rtl9210: missing endpoints\n");
		return -ENODEV;
	}

	return 0;
}

static void rtl9210_finish_command(struct scsi_cmnd *cmd, int host_status)
{
	struct rtl9210_cmd_priv *priv = scsi_cmd_priv(cmd);

	cmd->result = host_status << 16;
	
	kfree(priv->cbw);
	kfree(priv->csw);
	kfree(priv->data_buf);

	scsi_done(cmd);
}

static void rtl9210_async_complete(struct urb *urb)
{
	struct scsi_cmnd *cmd = urb->context;
	struct rtl9210_cmd_priv *priv = scsi_cmd_priv(cmd);
	struct rtl9210_dev *dev = priv->dev;
	unsigned int pipe;
	void *buf;
	int len;

	if (urb->status) {
		printk(KERN_ERR "rtl9210: URB failed in phase %d: %d\n", priv->phase, urb->status);
		rtl9210_finish_command(cmd, DID_ERROR);
		return;
	}

	switch (priv->phase) {
	case PHASE_CBW:
		if (priv->len) {
			priv->phase = PHASE_DATA;
			buf = priv->data_buf;
			len = priv->len;
			pipe = (priv->direction == US_BULK_FLAG_IN)
				? usb_rcvbulkpipe(dev->udev, 0x81)
				: usb_sndbulkpipe(dev->udev, 0x02);
		} else {
			priv->phase = PHASE_CSW;
			buf = priv->csw;
			len = US_BULK_CS_WRAP_LEN;
			pipe = usb_rcvbulkpipe(dev->udev, 0x81);
		}
		break;
	case PHASE_DATA:
		if (priv->direction == US_BULK_FLAG_IN)
			sg_copy_from_buffer(scsi_sglist(cmd), scsi_sg_count(cmd), priv->data_buf, priv->len);
		
		priv->phase = PHASE_CSW;
		buf = priv->csw;
		len = US_BULK_CS_WRAP_LEN;
		pipe = usb_rcvbulkpipe(dev->udev, 0x81);
		break;
	case PHASE_CSW:
		if (priv->cbw->Tag != priv->csw->Tag || priv->csw->Status != 0) {
			printk(KERN_ERR "rtl9210: CSW error/tag mismatch\n");
			rtl9210_finish_command(cmd, DID_ERROR);
		} else {
			if (priv->cbw->CDB[0] == READ_CAPACITY && priv->data_buf) {
				max_lba  = be32_to_cpu(*(__be32 *)&((u8 *)priv->data_buf)[0]);
				blk_size = be32_to_cpu(*(__be32 *)&((u8 *)priv->data_buf)[4]);
				printk(KERN_INFO "rtl9210: max LBA=%u block size=%u\n", max_lba, blk_size);
			}
			rtl9210_finish_command(cmd, DID_OK);
		}	
		return;
	}

	usb_fill_bulk_urb(urb, dev->udev, pipe, buf, len, rtl9210_async_complete, cmd);
	
	if (usb_submit_urb(urb, GFP_ATOMIC)) {
		printk(KERN_ERR "rtl9210: resubmit failed\n");
		rtl9210_finish_command(cmd, DID_ERROR);
	}
}

static int rtl9210_submit_async(struct rtl9210_dev *dev, struct scsi_cmnd *cmd)
{
	struct rtl9210_cmd_priv *priv = scsi_cmd_priv(cmd);
	u32 len = scsi_bufflen(cmd);

	memset(priv, 0, sizeof(*priv));
	priv->dev 		= dev;
	priv->phase 	= PHASE_CBW;
	
	priv->cbw 		= kzalloc(sizeof(*priv->cbw), GFP_ATOMIC);
	priv->csw 		= kzalloc(sizeof(*priv->csw), GFP_ATOMIC);
	priv->data_buf 	= len ? kzalloc(len, GFP_ATOMIC) : NULL;
	
	priv->len 		= len;
	priv->direction = (cmd->sc_data_direction == DMA_TO_DEVICE) 
		? US_BULK_FLAG_OUT : US_BULK_FLAG_IN;

	if (!priv->cbw || !priv->csw || (!priv->data_buf && len))
		goto fail;

	if (priv->direction == US_BULK_FLAG_OUT && len)
		sg_copy_to_buffer(scsi_sglist(cmd), scsi_sg_count(cmd), priv->data_buf, len);

	priv->cbw->Signature 		  = cpu_to_le32(US_BULK_CB_SIGN);
	priv->cbw->Tag 				  = scsi_cmd_to_rq(cmd)->tag;
	priv->cbw->DataTransferLength = cpu_to_le32(len);
	priv->cbw->Flags 			  = priv->direction;
	priv->cbw->Lun 				  = 0;
	priv->cbw->Length 			  = cmd->cmd_len;
	memcpy(priv->cbw->CDB, cmd->cmnd, cmd->cmd_len);

	usb_fill_bulk_urb(dev->bulk_out_urb, dev->udev,
						usb_sndbulkpipe(dev->udev, 0x02),
						priv->cbw, US_BULK_CB_WRAP_LEN,
						rtl9210_async_complete, cmd);

	if (usb_submit_urb(dev->bulk_out_urb, GFP_ATOMIC))
		goto fail;

	return 0;

fail:
	kfree(priv->cbw);
	kfree(priv->csw);
	kfree(priv->data_buf);

	return -ENOMEM;
}

/*
 * The queuecommand function is used to queue up a scsi
 * command block to the LLDD.  When the driver finished
 * processing the command the done callback is invoked.
 *
 * If queuecommand returns 0, then the driver has accepted the
 * command.  It must also push it to the HBA if the scsi_cmnd
 * flag SCMD_LAST is set, or if the driver does not implement
 * commit_rqs.  The done() function must be called on the command
 * when the driver has finished with it. (you may call done on the
 * command before queuecommand returns, but in this case you	
 * *must* return 0 from queuecommand).
 */
/* source: linux kernel scsi_host.h */

static int rtl9210_queuecommand(struct Scsi_Host *shost, struct scsi_cmnd *cmd)
{
	struct rtl9210_dev *dev = shost_priv(shost);
	u8 opcode = cmd->cmnd[0];

	if (cmd->device->id != 0 || cmd->device->lun != 0) {
		cmd->result = DID_NO_CONNECT << 16;
		scsi_done(cmd);
		return 0;
	}

	switch(opcode) {
	case TEST_UNIT_READY:	// 0x00
		break;
	case INQUIRY:			// 0x12
		break;
	case READ_CAPACITY:		// 0x25
		break;
	case READ_10:			// 0x28
		u32 block_address = be32_to_cpu(*(__be32 *)&cmd->cmnd[2]);
		u16 num_blocks 	  = be16_to_cpu(*(__be16 *)&cmd->cmnd[7]);

		if ((u64)block_address + num_blocks > max_lba + 1) {
			printk(KERN_ERR "rtl9210: READ(10) out of range\n");
			cmd->result = DID_ERROR << 16;
			scsi_done(cmd);
			return 0;
		}
		break;
	case WRITE_10:			// 0x2a
		break;
	default:
		printk(KERN_WARNING "rtl9210: unhandled opcode 0x%02x\n", opcode);
		cmd->result = DID_OK << 16;
		scsi_done(cmd);
		return 0;
	}

	if (rtl9210_submit_async(dev, cmd)) {
		cmd->result = DID_ERROR << 16;
		scsi_done(cmd);
	}

	return 0;
}

static struct scsi_host_template rtl9210_host_template = {
	.cmd_size	  = sizeof(struct rtl9210_cmd_priv),
	.module 	  = THIS_MODULE,
	.name 		  = "rtl9210",
	.queuecommand = rtl9210_queuecommand,
	.can_queue 	  = 1,	// only 1 command in flight at a time for now
	.this_id 	  = -1,	// no fixed host adapter ID
	.sg_tablesize = SG_ALL,
	.max_sectors  = 240,
};

/*	Array of vendor/product ID pairs my driver claims to support.
	USB_DEVICE() is a macro that expands to fill in the struct fields.
	{} at the end marks the end of the array. 
*/
static const struct usb_device_id rtl9210_table[] = {
	{USB_DEVICE(RTL_VENDOR_ID, RTL_PRODUCT_ID)},
	{}
};

/*	1. Embeds the ID table into my .ko file as metadata (check through modinfo rtl9210.ko)
	2. Allows the kernel's udev system to automatically load my module when a matching
	device is plugged in, without needing to manually run insmod
*/
MODULE_DEVICE_TABLE(usb, rtl9210_table);

/* struct usb_interface - what usb device drivers talk to */
static int rtl9210_probe(struct usb_interface *intf, const struct usb_device_id *id) 
{
	struct usb_device *udev;
	struct Scsi_Host *shost;
	struct rtl9210_dev *dev;
	u8 protocol;
	int ret;

	udev = interface_to_usbdev(intf);
	protocol = intf->cur_altsetting->desc.bInterfaceProtocol;
	
	printk(KERN_INFO "rtl9210: device connected\n");
	printk(KERN_INFO "rtl9210: USB %x.%02x\n",
		   udev->descriptor.bcdUSB >> 8,
		   udev->descriptor.bcdUSB & 0xff);
	printk(KERN_INFO "rtl9210: protocol %s\n",
		   protocol == USB_PR_UAS ? "UAS" : "Bulk-Only");
	
	shost = scsi_host_alloc(&rtl9210_host_template, sizeof(struct rtl9210_dev));
	if (!shost)
		return -ENOMEM;

	shost->max_id  = 1;
	shost->max_lun = 1;
	
	dev = shost_priv(shost);

	dev->shost = shost;
	dev->udev  = udev;
	dev->intf  = intf;
	
	ret = rtl9210_find_endpoints(dev);
	if (ret)
		goto err_free;

	printk(KERN_INFO "rtl9210: found all 2 endpoints\n");

	/* allocate URBs */
	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb) {
		ret = -ENOMEM;
		goto err_free;
	}

	dev->bulk_out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_out_urb) {
		ret = -ENOMEM;
		goto err_free;
	}
	
	printk(KERN_INFO "rtl9210: URBs allocated\n");

	/**
 	 * usb_set_intfdata() - associate driver-specific data with an interface
 	 * @intf: USB interface
 	 * @data: driver data
 	 *
 	 * Drivers can use this function in their probe() callbacks to associate
 	 * driver-specific data with an interface.
 	 *
 	 * Note that there is generally no need to clear the driver-data pointer even
 	 * if some drivers do so for historical or implementation-specific reasons.
 	 */
	/* source: linux kernel usb.h */
	usb_set_intfdata(intf, dev);

	ret = scsi_add_host(shost, &intf->dev);
	if (ret) {
		printk(KERN_ERR "rtl9210: scsi_add_host failed: %d\n", ret);
		goto err_free;
	}

	scsi_scan_host(shost);

	return 0;

err_free:
	usb_free_urb(dev->bulk_in_urb);
	usb_free_urb(dev->bulk_out_urb);
	scsi_host_put(shost);

	return ret;
}

static void rtl9210_disconnect(struct usb_interface *intf) 
{
	struct rtl9210_dev *dev = usb_get_intfdata(intf);
	
	usb_set_intfdata(intf, NULL);
	scsi_remove_host(dev->shost);	// unregisters from SCSI layer

	usb_free_urb(dev->bulk_in_urb);
	usb_free_urb(dev->bulk_out_urb);

	scsi_host_put(dev->shost);		// frees host allocation
	
	printk(KERN_INFO "rtl9210: device disconnected\n");
}

static struct usb_driver rtl9210_driver = {
	.name		= "rtl9210",
	.probe 		= rtl9210_probe,
	.disconnect = rtl9210_disconnect,
	.id_table	= rtl9210_table,
};

/**
 * module_usb_driver() - Helper macro for registering a USB driver
 * @__usb_driver: usb_driver struct
 *
 * Helper macro for USB drivers which do not do anything special in module
 * init/exit. This eliminates a lot of boilerplate. Each module may only
 * use this macro once, and calling it replaces module_init() and module_exit()
 */
/* source: linux kernel usb.h */
module_usb_driver(rtl9210_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("andrewkim999");
MODULE_DESCRIPTION("Realtek RTL9210 USB NVMe Adapter Driver");
