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

/* stores per device state */
struct rtl9210_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct Scsi_Host *shost;

	struct usb_host_endpoint *data_in;	// EP1 IN  0x81
	struct usb_host_endpoint *data_out;	// EP2 OUT 0x02
	
	struct urb *bulk_in_urb;
	struct urb *bulk_out_urb;

	struct completion urb_done;
	int urb_status;
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

static void rtl9210_urb_complete(struct urb *urb) 
{
	struct rtl9210_dev *dev = urb->context;
	dev->urb_status = urb->status;
	complete(&dev->urb_done);
}

static int rtl9210_bulk_transfer(struct rtl9210_dev *dev, struct urb *urb, 
								 unsigned int pipe, void *buf, int len) 
{	
	int ret;

	init_completion(&dev->urb_done);
	usb_fill_bulk_urb(urb, dev->udev, pipe, buf, len, rtl9210_urb_complete, dev);

	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret) {
		printk(KERN_ERR "rtl9210: failed to submit URB: %d\n", ret);	
		return ret;
	}

	wait_for_completion(&dev->urb_done);

	return dev->urb_status;
}
/*
static int rtl9210_bot_reset_recovery(struct rtl9210_dev *dev) {
	int ret;

	ret = usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0),
			0xff, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			0, dev->intf->cur_altsetting->desc.bInterfaceNumber,
			NULL, 0, 5000);
	if (ret < 0)
		printk(KERN_WARNING "rtl9210: BOT reset failed: %d\n", ret);

	usb_clear_halt(dev->udev, usb_sndbulkpipe(dev->udev, 0x02));
	usb_clear_halt(dev->udev, usb_rcvbulkpipe(dev->udev, 0x81));

	return ret;
}*/

static int rtl9210_send_inquiry(struct rtl9210_dev *dev) 
{
	struct bulk_cb_wrap *cbw;
	struct bulk_cs_wrap *csw;
	__u8 *data_buf;
	int ret;

	cbw = kzalloc(sizeof(struct bulk_cb_wrap), GFP_KERNEL);
	if (!cbw)
		return -ENOMEM;

	data_buf = kzalloc(INQUIRY_REPLY_LEN, GFP_KERNEL);
	if (!data_buf) {
		kfree(cbw);
		return -ENOMEM;
	}

	csw = kzalloc(sizeof(struct bulk_cs_wrap), GFP_KERNEL);
	if (!csw) {
		kfree(cbw);
		kfree(data_buf);
		return -ENOMEM;
	}

	cbw->Signature 			= cpu_to_le32(US_BULK_CB_SIGN);	// 'USBC'
    cbw->Tag 				= 1;
	cbw->DataTransferLength = cpu_to_le32(INQUIRY_REPLY_LEN);
	cbw->Flags 				= 0x80;		// data IN (device -> host)
	cbw->Lun 				= 0;
	cbw->Length 			= 6;		// INQUIRY CDB is 6 bytes
	cbw->CDB[0] 			= INQUIRY;	// INQUIRY opcode
	cbw->CDB[4] 			= INQUIRY_REPLY_LEN;

//	rtl9210_bot_reset_recovery(dev);

	/* phase 1: send CBW */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_out_urb, 
			usb_sndbulkpipe(dev->udev, 0x02), cbw, US_BULK_CB_WRAP_LEN);
	if (ret) {
		printk(KERN_ERR "rtl9210: INQUIRY CBW failed: %d\n", ret);
		goto done;
	}

	printk(KERN_INFO "rtl9210: INQUIRY CBW sent\n");

	/* phase 2: receive data */
	//usb_clear_halt(dev->udev, usb_rcvbulkpipe(dev->udev, 0x81));
	ret = rtl9210_bulk_transfer(dev, dev->bulk_in_urb, 
			usb_rcvbulkpipe(dev->udev, 0x81), data_buf, INQUIRY_REPLY_LEN);
	if (ret) {
		printk(KERN_ERR "rtl9210: INQUIRY data failed: %d\n", ret);
		goto done;
	}

	printk(KERN_INFO "rtl9210: INQUIRY response received\n");
	printk(KERN_INFO "rtl9210: vendor:   %.8s\n",  &data_buf[8]);
	printk(KERN_INFO "rtl9210: product:  %.16s\n", &data_buf[16]);
	printk(KERN_INFO "rtl9210: revision: %.4s\n",  &data_buf[32]);
	
	/* phase 3: receive CSW */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_in_urb, 
			usb_rcvbulkpipe(dev->udev, 0x81), csw, US_BULK_CS_WRAP_LEN);
	if (ret) {
		printk(KERN_ERR "rtl9210: INQUIRY CSW failed: %d\n", ret);
		goto done;
	}

	if (cbw->Tag != csw->Tag) {
		printk(KERN_ERR "rtl9210: CSW tag mismatch: expected %u, got %u\n", 
				cbw->Tag, csw->Tag);
		ret = -EIO;
		goto done;
	}

	printk(KERN_INFO "rtl9210: INQUIRY CSW received\n");

	ret = 0;
done:
//	rtl9210_bot_reset_recovery(dev);	
	kfree(cbw);
	kfree(data_buf);
	kfree(csw);
	return ret;
}

static int rtl9210_read_capacity(struct rtl9210_dev *dev, u32 *max_lba, u32 *block_size) 
{
	struct bulk_cb_wrap *cbw;
	struct bulk_cs_wrap *csw;
	__u8 *data_buf;
	int ret;

	cbw = kzalloc(sizeof(struct bulk_cb_wrap), GFP_KERNEL);
	if (!cbw)
		return -ENOMEM;

	data_buf = kzalloc(READ_CAPACITY_REPLY_LEN, GFP_KERNEL);
	if (!data_buf) {
		kfree(cbw);
		return -ENOMEM;
	}

	csw = kzalloc(sizeof(struct bulk_cs_wrap), GFP_KERNEL);
	if (!csw) {
		kfree(cbw);
		kfree(data_buf);
		return -ENOMEM;
	}

	cbw->Signature 			= cpu_to_le32(US_BULK_CB_SIGN);	// 'USBC'
    cbw->Tag 				= 2;
	cbw->DataTransferLength = cpu_to_le32(READ_CAPACITY_REPLY_LEN);
	cbw->Flags 				= 0x80;				// data IN (device -> host)
	cbw->Lun 				= 0;
	cbw->Length 			= 10;				// READ CAPACITY(10) CDB is 10 bytes
	cbw->CDB[0] 			= READ_CAPACITY;	// READ CAPACITY(10) opcode

	/* phase 1: send CBW */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_out_urb, 
			usb_sndbulkpipe(dev->udev, 0x02), cbw, US_BULK_CB_WRAP_LEN);	
	if (ret) {
		printk(KERN_ERR "rtl9210: READ CAPACITY(10) CBW failed: %d\n", ret);
		goto done;
	}

	printk(KERN_INFO "rtl9210: READ CAPACITY(10) CBW sent\n");

	/* phase 2: receive data */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_in_urb, 
			usb_rcvbulkpipe(dev->udev, 0x81), data_buf, READ_CAPACITY_REPLY_LEN);
	if (ret) {
		printk(KERN_ERR "rtl9210: READ CAPACITY(10) data failed: %d\n", ret);
		goto done;
	}

	*max_lba    = be32_to_cpu(*(__be32 *)&data_buf[0]);
	*block_size = be32_to_cpu(*(__be32 *)&data_buf[4]);
	
	printk(KERN_INFO "rtl9210: max LBA=%u, block size=%u bytes\n",
			*max_lba, *block_size);
	printk(KERN_INFO "rtl9210: capacity=%llu bytes\n",
			((u64)(*max_lba) + 1) * (*block_size));

	/* phase 3: receive CSW */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_in_urb, 
			usb_rcvbulkpipe(dev->udev, 0x81), csw, US_BULK_CS_WRAP_LEN);
	if (ret) {
		printk(KERN_ERR "rtl9210: READ CAPACITY(10) CSW failed: %d\n", ret);
		goto done;
	}

	if (cbw->Tag != csw->Tag) {
		printk(KERN_ERR "rtl9210: CSW tag mismatch: expected %u, got %u\n", 
				cbw->Tag, csw->Tag);
		ret = -EIO;
		goto done;
	}

	printk(KERN_INFO "rtl9210: READ CAPACITY(10) CSW received\n");

	ret = 0;

done:
	kfree(cbw);
	kfree(data_buf);
	kfree(csw);
	return ret;
}

static int rtl9210_read(struct rtl9210_dev *dev, u32 max_lba, u32 block_size,
		u32 block_address, u32 num_blocks, void *data) 
{
	struct bulk_cb_wrap *cbw;
	struct bulk_cs_wrap *csw;
	__u8 *data_buf;
	u32 transfer_length;
	int ret;

	transfer_length = num_blocks * block_size;

	if ((u64)block_address + num_blocks > max_lba + 1) {
		printk(KERN_ERR "rtl9210: READ(10) request out of range\n");
		return -EINVAL;
	}

	cbw = kzalloc(sizeof(struct bulk_cb_wrap), GFP_KERNEL);
	if (!cbw)
		return -ENOMEM;

	data_buf = kzalloc(transfer_length, GFP_KERNEL);
	if (!data_buf) {
		kfree(cbw);
		return -ENOMEM;
	}

	csw = kzalloc(sizeof(struct bulk_cs_wrap), GFP_KERNEL);
	if (!csw) {
		kfree(cbw);
		kfree(data_buf);
		return -ENOMEM;
	}

	cbw->Signature 			= cpu_to_le32(US_BULK_CB_SIGN);	// 'USBC'
    cbw->Tag 				= 3;
	cbw->DataTransferLength = cpu_to_le32(transfer_length);
	cbw->Flags 				= 0x80;		// data IN (device -> host)
	cbw->Lun 				= 0;
	cbw->Length 			= 10;		// READ(10) CDB is 10 bytes
	cbw->CDB[0] 			= READ_10;	// READ(10) opcode
	
	cbw->CDB[2]				= (block_address >> 24) & 0xff;
	cbw->CDB[3]				= (block_address >> 16) & 0xff;
	cbw->CDB[4]				= (block_address >> 8)  & 0xff;
	cbw->CDB[5]				= block_address & 0xff;

	cbw->CDB[7]				= (num_blocks >> 8) & 0xff;
	cbw->CDB[8]				= num_blocks & 0xff;

	/* phase 1: send CBW */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_out_urb, 
			usb_sndbulkpipe(dev->udev, 0x02), cbw, US_BULK_CB_WRAP_LEN);	
	if (ret) {
		printk(KERN_ERR "rtl9210: READ(10) CBW failed: %d\n", ret);
		goto done;
	}

	printk(KERN_INFO "rtl9210: READ(10) CBW sent\n");

	/* phase 2: receive data */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_in_urb, 
			usb_rcvbulkpipe(dev->udev, 0x81), data_buf, transfer_length);
	if (ret) {
		printk(KERN_ERR "rtl9210: READ(10) data failed: %d\n", ret);
		goto done;
	}

	memcpy(data, data_buf, transfer_length);

	printk(KERN_INFO "rtl9210: READ(10) data received\n");

	/* phase 3: receive CSW */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_in_urb, 
			usb_rcvbulkpipe(dev->udev, 0x81), csw, US_BULK_CS_WRAP_LEN);
	if (ret) {
		printk(KERN_ERR "rtl9210: READ(10) CSW failed: %d\n", ret);
		goto done;
	}

	if (cbw->Tag != csw->Tag) {
		printk(KERN_ERR "rtl9210: CSW tag mismatch: expected %u, got %u\n", 
				cbw->Tag, csw->Tag);
		ret = -EIO;
		goto done;
	}

	printk(KERN_INFO "rtl9210: READ(10) CSW received\n");

	print_hex_dump(KERN_INFO, "rtl9210: ", DUMP_PREFIX_OFFSET,
			16, 1, data, num_blocks * block_size, true);

	ret = 0;

done:
	kfree(cbw);
	kfree(data_buf);
	kfree(csw);
	return ret;
}

static int rtl9210_write(struct rtl9210_dev *dev, u32 max_lba, u32 block_size,
		u32 block_address, u32 num_blocks, void *data)
{
	struct bulk_cb_wrap *cbw;
	struct bulk_cs_wrap *csw;
	__u8 *data_buf;
	u32 transfer_length;
	int ret;

	transfer_length = num_blocks * block_size;

	if (block_address < 16384 || (u64)block_address + num_blocks > max_lba + 1) {
		printk(KERN_ERR "rtl9210: WRITE(10) request out of range\n");
		return -EINVAL;
	}

	cbw = kzalloc(sizeof(struct bulk_cb_wrap), GFP_KERNEL);
	if (!cbw)
		return -ENOMEM;

	data_buf = kzalloc(transfer_length, GFP_KERNEL);
	if (!data_buf) {
		kfree(cbw);
		return -ENOMEM;
	}

	csw = kzalloc(sizeof(struct bulk_cs_wrap), GFP_KERNEL);
	if (!csw) {
		kfree(cbw);
		kfree(data_buf);
		return -ENOMEM;
	}

	cbw->Signature 			= cpu_to_le32(US_BULK_CB_SIGN);	// 'USBC'
    cbw->Tag 				= 4;
	cbw->DataTransferLength = cpu_to_le32(transfer_length);
	cbw->Flags 				= 0x00;		// data OUT (host -> device)
	cbw->Lun 				= 0;
	cbw->Length 			= 10;		// WRITE(10) CDB is 10 bytes
	cbw->CDB[0] 			= WRITE_10;	// WRITE(10) opcode
	
	cbw->CDB[2]				= (block_address >> 24) & 0xff;
	cbw->CDB[3]				= (block_address >> 16) & 0xff;
	cbw->CDB[4]				= (block_address >> 8)  & 0xff;
	cbw->CDB[5]				= block_address & 0xff;

	cbw->CDB[7]				= (num_blocks >> 8) & 0xff;
	cbw->CDB[8]				= num_blocks & 0xff;

	/* phase 1: send CBW */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_out_urb, 
			usb_sndbulkpipe(dev->udev, 0x02), cbw, US_BULK_CB_WRAP_LEN);	
	if (ret) {
		printk(KERN_ERR "rtl9210: WRITE(10) CBW failed: %d\n", ret);
		goto done;
	}

	printk(KERN_INFO "rtl9210: WRITE(10) CBW sent\n");

	/* phase 2: send data */
	memcpy(data_buf, data, transfer_length);

	ret = rtl9210_bulk_transfer(dev, dev->bulk_out_urb, 
			usb_sndbulkpipe(dev->udev, 0x02), data_buf, transfer_length);
	if (ret) {
		printk(KERN_ERR "rtl9210: WRITE(10) data failed: %d\n", ret);
		goto done;
	}

	printk(KERN_INFO "rtl9210: WRITE(10) data received\n");

	/* phase 3: receive CSW */
	ret = rtl9210_bulk_transfer(dev, dev->bulk_in_urb, 
			usb_rcvbulkpipe(dev->udev, 0x81), csw, US_BULK_CS_WRAP_LEN);
	if (ret) {
		printk(KERN_ERR "rtl9210: WRITE(10) CSW failed: %d\n", ret);
		goto done;
	}

	if (cbw->Tag != csw->Tag) {
		printk(KERN_ERR "rtl9210: CSW tag mismatch: expected %u, got %u\n", 
				cbw->Tag, csw->Tag);
		ret = -EIO;
		goto done;
	}

	printk(KERN_INFO "rtl9210: WRITE(10) CSW received\n");
	
	ret = 0;

done:
	kfree(cbw);
	kfree(data_buf);
	kfree(csw);
	return ret;
}

static int rtl9210_write_test(struct rtl9210_dev *dev, 
		u32 max_lba, u32 block_size, u32 block_address, u32 num_blocks)
{
	void *original, *data, *expected;
	size_t len;
	int ret;

	len = num_blocks * block_size;

	original = kzalloc(len, GFP_KERNEL);	
	data     = kzalloc(len, GFP_KERNEL);
	expected = kzalloc(len, GFP_KERNEL);

	if (!original || !data || !expected) {	
		ret = -ENOMEM;
		goto done;	
	}

	/* read and store original data */
	ret = rtl9210_read(dev, max_lba, block_size, block_address, num_blocks, original);
	if (ret) goto done;

	/* test write function: magic number 5a */
	memset(data, 'Z', len);
	ret = rtl9210_write(dev, max_lba, block_size, block_address, num_blocks, data);
	if (ret) goto done;

	/* verify data written */
	ret = rtl9210_read(dev, max_lba, block_size, block_address, num_blocks, data);
	if (ret) goto done;
	
	memset(expected, 'Z', len);
	if (memcmp(data, expected, len)) {
		printk(KERN_INFO "rtl9210: failed to write correct data\n");
		ret = -EIO;
		goto done;
	}

	/* restore original data */
	ret = rtl9210_write(dev, max_lba, block_size, block_address, num_blocks, original);
	if (ret) goto done;
	
	/* verify original data */
	ret = rtl9210_read(dev, max_lba, block_size, block_address, num_blocks, data);
	if (ret) goto done;
	
	if (memcmp(data, original, len)) {
		printk(KERN_ERR "rtl9210: failed to restore original data\n");	
		ret = -EIO;
		goto done;
	}

	printk(KERN_INFO "rtl9210: write test passed\n");
	ret = 0;

done:
	kfree(original);
	kfree(data);
	kfree(expected);
	return ret;
}

static int rtl9210_queuecommand(struct Scsi_Host *shost, struct scsi_cmnd *cmd)
{
	cmd->result = DID_ERROR << 16;
	scsi_done(cmd);
	return 0;
}

static struct scsi_host_template rtl9210_host_template = {
	.module 	  = THIS_MODULE,
	.name 		  = "rtl9210",
	.queuecommand = rtl9210_queuecommand,
	.this_id 	  = -1,	// no fixed host adapter ID
	.can_queue 	  = 1,	// only 1 command in flight at a time for now
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

	/* informational only, not fatal to probe */
	/*
	ret = rtl9210_send_inquiry(dev);
	if (ret)
		printk(KERN_ERR "rtl9210: INQUIRY failed: %d\n", ret);

	u32 max_lba, block_size;
	ret = rtl9210_read_capacity(dev, &max_lba, &block_size);
	if (ret)
		printk(KERN_ERR "rtl9210: READ CAPACITY(10) failed: %d\n", ret);
	else
		ret = rtl9210_write_test(dev, max_lba, block_size, 1953519615, 1);
	
	if (ret)
		printk(KERN_ERR "rtl9210: write test failed: %d\n", ret);
	*/

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
