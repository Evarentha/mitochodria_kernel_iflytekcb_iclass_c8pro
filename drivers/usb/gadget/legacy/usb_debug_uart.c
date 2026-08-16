// SPDX-License-Identifier: GPL-2.0
/*
 * usb_debug_uart.c - Kernel USB debug UART gadget with panic transmit
 *
 * Built-in CDC ACM composite gadget that mirrors kernel printk to an
 * external Linux host over USB. The console is automatically registered
 * without requiring console=ttyGS0 in bootargs.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/tty.h>

#include "u_serial.h"

USB_GADGET_COMPOSITE_OPTIONS();

#define USB_DEBUG_UART_VENDOR_ID	0x1d6b	/* Linux Foundation */
#define USB_DEBUG_UART_PRODUCT_ID	0x0104	/* Multifunction composite */

static struct usb_device_descriptor device_desc = {
	.bLength =		USB_DT_DEVICE_SIZE,
	.bDescriptorType =	USB_DT_DEVICE,
	.bcdUSB =		cpu_to_le16(0x0200),
	.bDeviceClass =		USB_CLASS_COMM,
	.bDeviceSubClass =	0,
	.bDeviceProtocol =	0,
	.idVendor =		cpu_to_le16(USB_DEBUG_UART_VENDOR_ID),
	.idProduct =		cpu_to_le16(USB_DEBUG_UART_PRODUCT_ID),
	.bNumConfigurations =	1,
};

static const struct usb_descriptor_header *otg_desc[2];

static struct usb_string strings_dev[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = "IFLYTEK",
	[USB_GADGET_PRODUCT_IDX].s = "Kernel Debug UART",
	[USB_GADGET_SERIAL_IDX].s = "",
	{  }
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

static struct usb_function_instance *fi_acm;
static struct usb_function *f_acm;
static unsigned char tty_line;

static int usb_debug_uart_bind_config(struct usb_configuration *c)
{
	int status;

	f_acm = usb_get_function(fi_acm);
	if (IS_ERR(f_acm))
		return PTR_ERR(f_acm);

	status = usb_add_function(c, f_acm);
	if (status < 0)
		usb_put_function(f_acm);

	return status;
}

static struct usb_configuration debug_uart_config_driver = {
	.label			= "CDC ACM Debug Console",
	.bConfigurationValue	= 1,
	.bmAttributes		= USB_CONFIG_ATT_SELFPOWER,
};

static int usb_debug_uart_bind(struct usb_composite_dev *cdev)
{
	int status;

	/* Allocate a tty line for console */
	status = gserial_alloc_line(&tty_line);
	if (status)
		return status;

	fi_acm = usb_get_function_instance("acm");
	if (IS_ERR(fi_acm)) {
		status = PTR_ERR(fi_acm);
		goto fail_get_instance;
	}

	status = usb_string_ids_tab(cdev, strings_dev);
	if (status < 0)
		goto fail_string_ids;

	device_desc.iManufacturer = strings_dev[USB_GADGET_MANUFACTURER_IDX].id;
	device_desc.iProduct = strings_dev[USB_GADGET_PRODUCT_IDX].id;

	if (gadget_is_otg(cdev->gadget) && !otg_desc[0]) {
		struct usb_descriptor_header *usb_desc;

		usb_desc = usb_otg_descriptor_alloc(cdev->gadget);
		if (!usb_desc) {
			status = -ENOMEM;
			goto fail_string_ids;
		}
		usb_otg_descriptor_init(cdev->gadget, usb_desc);
		otg_desc[0] = usb_desc;
		otg_desc[1] = NULL;
	}

	status = usb_add_config(cdev, &debug_uart_config_driver,
				usb_debug_uart_bind_config);
	if (status < 0)
		goto fail_otg_desc;

	usb_composite_overwrite_options(cdev, &coverwrite);

	dev_info(&cdev->gadget->dev, "USB Debug UART gadget ready\n");
	return 0;

fail_otg_desc:
	kfree(otg_desc[0]);
	otg_desc[0] = NULL;
fail_string_ids:
	usb_put_function_instance(fi_acm);
fail_get_instance:
	gserial_free_line(tty_line);
	return status;
}

static int usb_debug_uart_unbind(struct usb_composite_dev *cdev)
{
	if (!IS_ERR_OR_NULL(f_acm))
		usb_put_function(f_acm);
	if (!IS_ERR_OR_NULL(fi_acm))
		usb_put_function_instance(fi_acm);

	gserial_free_line(tty_line);

	kfree(otg_desc[0]);
	otg_desc[0] = NULL;

	return 0;
}

static struct usb_composite_driver usb_debug_uart_driver = {
	.name		= "usb_debug_uart",
	.dev		= &device_desc,
	.strings	= dev_strings,
	.max_speed	= USB_SPEED_SUPER,
	.bind		= usb_debug_uart_bind,
	.unbind		= usb_debug_uart_unbind,
};

static int __init usb_debug_uart_init(void)
{
	return usb_composite_probe(&usb_debug_uart_driver);
}
subsys_initcall(usb_debug_uart_init);

static void __exit usb_debug_uart_exit(void)
{
	usb_composite_unregister(&usb_debug_uart_driver);
}
module_exit(usb_debug_uart_exit);

MODULE_DESCRIPTION("USB Debug UART Gadget");
MODULE_AUTHOR("IFLYTEK");
MODULE_LICENSE("GPL v2");
