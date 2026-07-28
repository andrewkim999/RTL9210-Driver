#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/uas.h>
#include <linux/usb/storage.h>

#define RTL_VENDOR_ID	0x0bda
#define RTL_PRODUCT_ID	0x9210
#define USB_PR_BULK		0x50
#define USB_PR_UAS		0x62
#define INQUIRY_REPLY_LEN 96

/* stores per device state */
struct rtl9210_dev {
	struct usb_device *udev;
	struct usb_interface *intf;

	/* UAS endpoints 
	   bit 7 set = IN (device -> host)
	   bit 7 clear = OUT (host -> device)
	*/
	struct usb_host_endpoint *data_in;	// EP1 IN  0x81
	struct usb_host_endpoint *data_out;	// EP2 OUT 0x02
	
	struct urb *bulk_in_urb;
	struct urb *bulk_out_urb;

	struct completion urb_done;
	int urb_status;
};

/* endpoint parsing function */
static int rtl9210_find_endpoints(struct rtl9210_dev *dev) {
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

static void rtl9210_urb_complete(struct urb *urb) {
	struct rtl9210_dev *dev = urb->context;
	dev->urb_status = urb->status;
	complete(&dev->urb_done);
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

static int rtl9210_send_inquiry(struct rtl9210_dev *dev) {
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

	printk(KERN_INFO "sizeof(*csw)=%d\n", sizeof(*csw));

	cbw->Signature 			= cpu_to_le32(US_BULK_CB_SIGN);	// 'USBC'
    cbw->Tag 				= 1;
	cbw->DataTransferLength = cpu_to_le32(INQUIRY_REPLY_LEN);
	cbw->Flags 				= 0x80;	// data IN (device -> host)
	cbw->Lun 				= 0;
	cbw->Length 			= 6;	// INQUIRY CDB is 6 bytes
	cbw->CDB[0] 			= 0x12;	// INQUIRY opcode
	cbw->CDB[4] 			= INQUIRY_REPLY_LEN;

//	rtl9210_bot_reset_recovery(dev);

	/* phase 1: send CBW */
	init_completion(&dev->urb_done);
	usb_fill_bulk_urb(dev->bulk_out_urb, dev->udev,
			usb_sndbulkpipe(dev->udev, 0x02),
			cbw, US_BULK_CB_WRAP_LEN,
			rtl9210_urb_complete, dev);

	ret = usb_submit_urb(dev->bulk_out_urb, GFP_KERNEL);
	if (ret) {
		printk(KERN_ERR "rtl9210: failed to submit CBW: %d\n", ret);
		goto done;
	}
	wait_for_completion(&dev->urb_done);

	if (dev->urb_status != US_BULK_STAT_OK) {
		printk(KERN_ERR "rtl9210: CBW failed: %d\n", dev->urb_status);
		ret = dev->urb_status;
		goto done;
	}
	printk(KERN_INFO "rtl9210: CBW sent\n");

	/* phase 2: receive data */
	//usb_clear_halt(dev->udev, usb_rcvbulkpipe(dev->udev, 0x81));

	init_completion(&dev->urb_done);
	usb_fill_bulk_urb(dev->bulk_in_urb, dev->udev,
			usb_rcvbulkpipe(dev->udev, 0x81),
			data_buf, INQUIRY_REPLY_LEN,
			rtl9210_urb_complete, dev);
	
	ret = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (ret) {
		printk(KERN_ERR "rtl9210: failed to submit data URB: %d\n", ret);
		goto done;
	}
	wait_for_completion(&dev->urb_done);
	printk(KERN_INFO "rtl9210: data phase actual=%d, status=%d\n", 
			dev->bulk_out_urb->actual_length, dev->urb_status);

	if (dev->urb_status) {
		printk(KERN_ERR "rtl9210: data failed: %d\n", dev->urb_status);
		ret = dev->urb_status;
		goto done;
	}
	printk(KERN_INFO "rtl9210: INQUIRY response received\n");
	printk(KERN_INFO "rtl9210: vendor:   %.8s\n",  &data_buf[8]);
	printk(KERN_INFO "rtl9210: product:  %.16s\n", &data_buf[16]);
	printk(KERN_INFO "rtl9210: revision: %.4s\n",  &data_buf[32]);

	/* phase 3: receive CSW */
	init_completion(&dev->urb_done);
	usb_fill_bulk_urb(dev->bulk_in_urb, dev->udev,
			usb_rcvbulkpipe(dev->udev, 0x81),
			csw, US_BULK_CS_WRAP_LEN,
			rtl9210_urb_complete, dev);

	ret = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (ret) {
		printk(KERN_ERR "rtl9210: failed to submit CSW URB: %d\n", ret);
		goto done;
	}
	wait_for_completion(&dev->urb_done);

	if (dev->urb_status != US_BULK_STAT_OK) {
		printk(KERN_ERR "rtl9210: CSW failed: %d\n", dev->urb_status);
		ret = dev->urb_status;
		goto done;
	}

	printk(KERN_INFO "rtl9210: CSW Signature=0x%02x\n", csw->Signature);
	printk(KERN_INFO "rtl9210: CSW Tag      =0x%02x\n", csw->Tag);
	printk(KERN_INFO "rtl9210: CSW Residue  =0x%02x\n", csw->Residue);
	printk(KERN_INFO "rtl9210: CSW status   =0x%02x\n", csw->Status);

	ret = 0;

done:
//	rtl9210_bot_reset_recovery(dev);
	
	kfree(cbw);
	kfree(data_buf);
	kfree(csw);
	return ret;
}

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
static int rtl9210_probe(struct usb_interface *intf, const struct usb_device_id *id) {
	struct usb_device *udev;
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
	
	dev = kzalloc(sizeof(struct rtl9210_dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev = udev;
	dev->intf = intf;
	
	ret = rtl9210_find_endpoints(dev);
	if (ret) {
		kfree(dev);
		return ret;
	}
	printk(KERN_INFO "rtl9210: found all 2 endpoints\n");

	/* allocate URBs */
	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb) {
		kfree(dev);
		return -ENOMEM;
	}

	dev->bulk_out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_out_urb) {
		usb_free_urb(dev->bulk_in_urb);
		kfree(dev);
		return -ENOMEM;
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

	ret = rtl9210_send_inquiry(dev);
	if (ret)
		printk(KERN_ERR "rtl9210: inquiry failed: %d\n", ret);

	return 0;
}

static void rtl9210_disconnect(struct usb_interface *intf) {
	struct rtl9210_dev *dev = usb_get_intfdata(intf);
	
	usb_set_intfdata(intf, NULL);

	usb_free_urb(dev->bulk_in_urb);
	usb_free_urb(dev->bulk_out_urb);
	kfree(dev);
	
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
