#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/uas.h>

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
	struct usb_host_endpoint *status;	// EP3 IN  0x83
	struct usb_host_endpoint *cmd;		// EP4 OUT 0x04
	
	struct urb *data_urb;
	struct urb *status_urb;
	struct urb *cmd_urb;
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
		else if (usb_endpoint_is_bulk_in(ep) && ep->bEndpointAddress == 0x83)
			dev->status = &alt->endpoint[i];
		else if (usb_endpoint_is_bulk_out(ep) && ep->bEndpointAddress == 0x04)
			dev->cmd = &alt->endpoint[i];
	}

	if (!dev->data_in || !dev->data_out || !dev->status || !dev->cmd) {
		printk(KERN_ERR "RTL9210: missing endpoints\n");
		return -ENODEV;
	}

	return 0;
}

static void rtl9210_inquiry_data_complete(struct urb *urb) {
	__u8 *buf = urb->transfer_buffer;

	if (urb->status) {
		printk(KERN_ERR "rtl9210: data URB failed: %d\n", urb->status);
		kfree(buf);
		return;
	}

	printk(KERN_INFO "rtl9210: INQUIRY response received\n");
	printk(KERN_INFO "rtl9210: vendor:   %.8s\n",  &buf[8]);
	printk(KERN_INFO "rtl9210: product:  %.16s\n", &buf[16]);
	printk(KERN_INFO "rtl9210: revision: %.4s\n",  &buf[32]);

	kfree(buf);
}

static void rtl9210_inquiry_status_complete(struct urb *urb) {
	struct sense_iu *iu = urb->transfer_buffer;


	if (urb->status) {
		printk(KERN_ERR "rtl9210: status URB failed: %d\n", urb->status);
		kfree(buf);
		return;
	}

	printk(KERN_INFO "rtl9210: status iu_id=0x%02x status=0x%02x\n", 
			iu->iu_id, iu->status);

	if (iu->status != 0)
		printk(KERN_ERR "rtl9210: SCSI error status: 0x%02x\n", iu->status);

	kfree(iu);
}

static void rtl9210_inquiry_cmd_complete(struct urb *urb) {
	struct command_iu *iu = urb->transfer_buffer;

	if (urb->status)
		printk(KERN_ERR "rtl9210: cmd URB failed: %d\n", urb->status);
	else
		printk(KERN_INFO "rtl9210: INQUIRY command sent\n");

	kfree(iu);
}

static int rtl9210_send_inquiry(struct rtl9210_dev *dev) {
	struct command_iu *cmd_buf;
	__u8 *data_buf;
	struct sense_iu *status_buf;

	cmd_buf = kzalloc(sizeof(struct command_iu), GFP_KERNEL);
	if (!cmd_buf)
		return -ENOMEM;

	data_buf = kzalloc(INQUIRY_REPLY_LEN, GFP_KERNEL);
	if (!data_buf) {
		kfree(cmd_buf);
		return -ENOMEM;
	}

	status_buf = kzalloc(sizeof(struct sense_iu), GFP_KERNEL);
	if (!status_buf) {
		kfree(cmd_buf);
		kfree(data_buf);
		return -ENOMEM;
	}
	
	cmd_buf->iu_id     = IU_ID_COMMAND;
	cmd_buf->tag       = cpu_to_be16(1);
	cmd_buf->prio_attr = UAS_SIMPLE_TAG;	// executes in order, no special priority
	cmd_buf->len       = 0;
	int_to_scsilun(0, &cmd_iu->lun);	// LUN 0

	cmd_buf->cdb[0] = 0x12;	// INQUIRY opcode
	cmd_buf->cdb[1] = 0x00;	// EVPD = 0
	cmd_buf->cdb[2] = 0x00;	// page code = 0
	cmd_buf->cdb[3] = 0x00;	// reserved
	cmd_buf->cdb[4] = INQUIRY_REPLY_LENGTH;	// allocation length
	cmd_buf->cdb[5] = 0x00;	// control


	/* fill command URB -> EP4 OUT 0x04 */
	usb_fill_bulk_urb(dev->cmd_urb, dev->udev,
			usb_sndbulkpipe(dev->udev, 0x04),
			cmd_iu, sizeof(*cmd_iu),
			rtl9210_cmd_complete, dev);

	return 0;
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
	
	if (protocol != USB_PR_UAS) {
		printk(KERN_INFO "rtl9210: switching to UAS mode\n");

		/* interface: the interface being updated, alternate: the setting being chosen */
		ret = usb_set_interface(udev, 0, 1); 	// interface = 0, alternate = 1
		if (ret) {
			printk(KERN_ERR "rtl9210: failed to switch to UAS: %d\n", ret);
			return ret;
		}

		protocol = intf->cur_altsetting->desc.bInterfaceProtocol;
		printk(KERN_INFO "rtl9210: protocol now %s\n",
				protocol == USB_PR_UAS ? "UAS" : "Bulk-Only");
	}

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
	printk(KERN_INFO "rtl9210: found all 4 endpoints\n");

	/* allocate URBs */
	dev->data_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->data_urb) {
		kfree(dev);
		return -ENOMEM;
	}

	dev->status_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->status_urb) {
		usb_free_urb(dev->data_urb);
		kfree(dev);
		return -ENOMEM;
	}
	
	dev->cmd_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->cmd_urb) {
		usb_free_urb(dev->data_urb);
		usb_free_urb(dev->status_urb);
		kfree(dev);
		return -ENOMEM;
	}
	
	printk(KERN_INFO "rtl9210: URBs allocated\n");

	rtl9210_send_inquiry(dev);

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

	return 0;
}

static void rtl9210_disconnect(struct usb_interface *intf) {
	struct rtl9210_dev *dev = usb_get_intfdata(intf);
	
	usb_set_intfdata(intf, NULL);

	usb_free_urb(dev->data_urb);
	usb_free_urb(dev->status_urb);
	usb_free_urb(dev->cmd_urb);
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
