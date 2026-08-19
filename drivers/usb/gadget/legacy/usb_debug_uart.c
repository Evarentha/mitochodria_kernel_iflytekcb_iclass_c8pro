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
#include <linux/workqueue.h>
#include <linux/console.h>

#include "u_serial.h"

#ifdef CONFIG_USB_DEBUG_UART
#include "../../dwc3/gadget.h"
#endif

#ifdef CONFIG_USB_DEBUG_UART
/* Forward declaration for func_to_acm */
struct f_acm {
	struct gserial			port;
	u8				ctrl_id, data_id;
	u8				port_num;
	/* ... (other fields not needed here) */
};

static inline struct f_acm *func_to_acm(struct usb_function *f)
{
	return container_of(f, struct f_acm, port.func);
}
#endif

USB_GADGET_COMPOSITE_OPTIONS();

#define USB_DEBUG_UART_VENDOR_ID	0x1d6b	/* Linux Foundation */
#define USB_DEBUG_UART_PRODUCT_ID	0x0104	/* Multifunction composite */
#define USB_DEBUG_UART_RETRY_MS		5000

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

/* Workqueue retry state for gadget registration */
static struct delayed_work usb_debug_uart_probe_work;
static atomic_t usb_debug_uart_bound = ATOMIC_INIT(0);
static atomic_t usb_debug_uart_registered = ATOMIC_INIT(0);
static atomic_t usb_debug_uart_shutdown = ATOMIC_INIT(0);
static atomic_t usb_debug_uart_retries = ATOMIC_INIT(0);

static int usb_debug_uart_bind_config(struct usb_configuration *c)
{
	int status;

	f_acm = usb_get_function(fi_acm);
	if (IS_ERR(f_acm))
		return PTR_ERR(f_acm);

	status = usb_add_function(c, f_acm);
	if (status < 0) {
		usb_put_function(f_acm);
		f_acm = NULL;	/* 防止 fail 路径 double put */
	}

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

	/* Defensive check: ensure previous unbind cleaned up */
	if (WARN_ON(!IS_ERR_OR_NULL(fi_acm) || !IS_ERR_OR_NULL(f_acm))) {
		pr_err("USB Debug UART: bind called with stale pointers\n");
		return -EBUSY;
	}

	/*
	 * Do NOT allocate a gserial line here: the ACM function instance
	 * (usb_get_function_instance("acm")) allocates its own line in
	 * acm_alloc_instance() and owns it for its lifetime. A second
	 * allocation here would push the ACM instance onto port 1 while
	 * the console index stays at its default, breaking
	 * gs_console_connect() and stalling SET_CONFIGURATION.
	 */
	fi_acm = usb_get_function_instance("acm");
	if (IS_ERR(fi_acm)) {
		status = PTR_ERR(fi_acm);
		pr_err("USB Debug UART: usb_get_function_instance failed: %d\n", status);
		goto fail_get_instance;
	}

	status = usb_string_ids_tab(cdev, strings_dev);
	if (status < 0) {
		pr_err("USB Debug UART: usb_string_ids_tab failed: %d\n", status);
		goto fail_string_ids;
	}

	device_desc.iManufacturer = strings_dev[USB_GADGET_MANUFACTURER_IDX].id;
	device_desc.iProduct = strings_dev[USB_GADGET_PRODUCT_IDX].id;

	if (gadget_is_otg(cdev->gadget) && !otg_desc[0]) {
		struct usb_descriptor_header *usb_desc;

		usb_desc = usb_otg_descriptor_alloc(cdev->gadget);
		if (!usb_desc) {
			status = -ENOMEM;
			pr_err("USB Debug UART: OTG descriptor alloc failed\n");
			goto fail_string_ids;
		}
		usb_otg_descriptor_init(cdev->gadget, usb_desc);
		otg_desc[0] = usb_desc;
		otg_desc[1] = NULL;
	}

	status = usb_add_config(cdev, &debug_uart_config_driver,
				usb_debug_uart_bind_config);
	if (status < 0) {
		pr_err("USB Debug UART: usb_add_config failed: %d\n", status);
		goto fail_otg_desc;
	}

	usb_composite_overwrite_options(cdev, &coverwrite);

	dev_info(&cdev->gadget->dev, "USB Debug UART gadget ready\n");
	atomic_set(&usb_debug_uart_bound, 1);

	/*
	 * Arm the panic TX path here: bind runs in process context
	 * (no cdev->lock held), so GFP_KERNEL allocation is legal.
	 * Doing this in set_alt (which runs under cdev->lock spinlock)
	 * caused SET_CONFIGURATION to stall and the host to report
	 * "can't set config #1, error -32 (EPIPE)".
	 */
	if (f_acm && !IS_ERR_OR_NULL(f_acm)) {
		struct f_acm *acm = func_to_acm(f_acm);
		if (acm && acm->port.in) {
			if (dwc3_gadget_debug_uart_arm(acm->port.in))
				pr_err("USB Debug UART: panic TX arm failed\n");
			else
				pr_emerg("USB_UART_ARM_OK\n");

#ifdef CONFIG_U_SERIAL_CONSOLE
			/*
			 * Explicitly set the console index to our allocated port.
			 * gserial_cons is a global shared by all gserial users (f_serial,
			 * f_obex, etc). If another gadget allocates a line after us, its
			 * gserial_alloc_line() would overwrite the index. By setting it here
			 * at the end of our bind, we ensure gs_console_connect() sees the
			 * correct port number and doesn't reject with -ENXIO.
			 */
			gserial_cons.index = acm->port_num;
			pr_debug("USB Debug UART: console index set to %u\n", acm->port_num);
#endif
		}
	}
	return 0;

fail_otg_desc:
	if (!IS_ERR_OR_NULL(f_acm)) {
		usb_put_function(f_acm);
		f_acm = NULL;
	}
	kfree(otg_desc[0]);
	otg_desc[0] = NULL;
fail_string_ids:
	usb_put_function_instance(fi_acm);
	fi_acm = NULL;
fail_get_instance:
	return status;
}

static int usb_debug_uart_unbind(struct usb_composite_dev *cdev)
{
	pr_info("USB Debug UART: unbind called\n");

	/* Reset bound flag to allow workqueue retry if needed */
	atomic_set(&usb_debug_uart_bound, 0);

#ifdef CONFIG_USB_DEBUG_UART
	/*
	 * Explicitly disarm to free DMA resources (16KB buffer + TRB).
	 * The composite_unbind path calls function->unbind (acm_unbind),
	 * not function->disable (acm_disable), so disarm is never called.
	 */
	if (f_acm && !IS_ERR_OR_NULL(f_acm)) {
		struct f_acm *acm = func_to_acm(f_acm);
		if (acm && acm->port.in) {
			pr_debug("USB Debug UART: explicit disarm before unbind\n");
			dwc3_gadget_debug_uart_disarm(acm->port.in);
		}
	}
#endif

	/* Clean up function resources and clear pointers */
	if (!IS_ERR_OR_NULL(f_acm)) {
		usb_put_function(f_acm);
		f_acm = NULL;
	}
	if (!IS_ERR_OR_NULL(fi_acm)) {
		usb_put_function_instance(fi_acm);
		fi_acm = NULL;
	}

	kfree(otg_desc[0]);
	otg_desc[0] = NULL;

	pr_debug("USB Debug UART: unbind complete, resources freed\n");
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

/*
 * The DWC3 UDC may not be registered yet when this initcall runs (in
 * dual-role mode the gadget core is only initialized after VBUS is
 * detected).
 *
 * Kernel provides a pending mechanism: when UDC doesn't exist,
 * usb_gadget_probe_driver() adds the driver to a pending list and returns
 * success (0). When UDC registers later, it automatically binds pending
 * drivers via check_pending_gadget_drivers().
 *
 * However, if the bind callback (usb_debug_uart_bind) fails for any reason,
 * the pending entry is removed and never retried. To handle this case, we
 * periodically check if binding succeeded (via the 'bound' flag). If not
 * bound after USB_DEBUG_UART_RESET_RETRIES attempts, we unregister and
 * re-register to retry.
 */
#define USB_DEBUG_UART_RESET_RETRIES	6	/* 30s (6 * 5s) before reset */

static void usb_debug_uart_probe(struct work_struct *work)
{
	int registered, bound;
	int ret;

	/* Check shutdown first to prevent rescheduling during module unload */
	if (atomic_read(&usb_debug_uart_shutdown))
		return;

	bound = atomic_read(&usb_debug_uart_bound);
	registered = atomic_read(&usb_debug_uart_registered);

	pr_emerg("USB_UART: reg=%d bnd=%d ret=%u\n",
		 registered, bound, atomic_read(&usb_debug_uart_retries));

	/* Successfully bound, stop retrying */
	if (bound)
		return;

	if (registered) {
		/*
		 * Already registered (in pending list or bound attempt in
		 * progress) but not yet bound. If this persists beyond the
		 * retry threshold, the bind likely failed and the pending
		 * entry was removed. Unregister and re-register to retry.
		 */
		if (atomic_inc_return(&usb_debug_uart_retries) >= USB_DEBUG_UART_RESET_RETRIES) {
			/*
			 * Double-check bound status before unregistering to
			 * avoid race at the 30s boundary where bind might
			 * have just completed.
			 */
			if (!atomic_read(&usb_debug_uart_bound)) {
				pr_info("USB Debug UART: bind timeout after %us, resetting\n",
					(USB_DEBUG_UART_RESET_RETRIES *
					 USB_DEBUG_UART_RETRY_MS) / 1000);
				usb_composite_unregister(&usb_debug_uart_driver);
				atomic_set(&usb_debug_uart_registered, 0);
				atomic_set(&usb_debug_uart_retries, 0);
			}
		}
	} else {
		/* First registration or after reset */
		ret = usb_composite_probe(&usb_debug_uart_driver);
		if (ret == 0) {
			pr_emerg("USB_UART_PROBE_OK\n");
			atomic_set(&usb_debug_uart_registered, 1);
			atomic_set(&usb_debug_uart_retries, 0);
		} else {
			pr_emerg("USB_UART_PROBE_FAIL=%d\n", ret);
		}
	}

	/* Only reschedule if not shutting down */
	if (!atomic_read(&usb_debug_uart_shutdown))
		schedule_delayed_work(&usb_debug_uart_probe_work,
				      msecs_to_jiffies(USB_DEBUG_UART_RETRY_MS));
}

static int __init usb_debug_uart_init(void)
{
	pr_emerg("USB_DEBUG_UART_INIT\n");
	INIT_DELAYED_WORK(&usb_debug_uart_probe_work, usb_debug_uart_probe);
	schedule_delayed_work(&usb_debug_uart_probe_work, 0);
	pr_emerg("USB_DEBUG_UART_WQ_OK\n");

	return 0;
}
subsys_initcall(usb_debug_uart_init);

static void __exit usb_debug_uart_exit(void)
{
	/* Set shutdown flag first to prevent workqueue rescheduling */
	atomic_set(&usb_debug_uart_shutdown, 1);

	/* Cancel any pending work and wait for current execution to finish */
	cancel_delayed_work_sync(&usb_debug_uart_probe_work);

	if (atomic_read(&usb_debug_uart_registered))
		usb_composite_unregister(&usb_debug_uart_driver);
}
module_exit(usb_debug_uart_exit);

MODULE_DESCRIPTION("USB Debug UART Gadget");
MODULE_AUTHOR("IFLYTEK");
MODULE_LICENSE("GPL v2");
