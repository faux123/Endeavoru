/*
 * Copyright (C) 2010 Google, Inc.
 * Author: Nick Pelly <npelly@google.com>
 * Based on Motorola's mdm6600_modem driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

/*
 * TODO check if we need to implement throttling
 * TODO handle suspend/resume/LP0/LP1
 */

#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>
#include <linux/workqueue.h>

static bool debug = false;
static bool debug_data = false;

#define BP_MODEM_STATUS 0x20a1
#define BP_RSP_AVAIL 0x01a1
#define BP_SPEED_CHANGE 0x2aa1

#define BP_STATUS_CAR 0x01
#define BP_STATUS_DSR 0x02
#define BP_STATUS_BREAK 0x04
#define BP_STATUS_RNG 0x08

#define POOL_SZ 16

#define MODEM_INTERFACE_NUM 4

static const struct usb_device_id mdm6600_id_table[] = {
	{ USB_DEVICE(0x22b8, 0x2a70) },
	{ },
};
MODULE_DEVICE_TABLE(usb, mdm6600_id_table);

struct mdm6600_urb_write_pool {
	spinlock_t busy_lock;  /* protects busy flags */
	bool busy[POOL_SZ];
	struct urb *urb[POOL_SZ];
	struct usb_anchor in_flight;
	int buffer_sz;  /* allocated urb buffer size */
};

struct mdm6600_urb_read_pool {
	struct urb *urb[POOL_SZ];
	struct usb_anchor in_flight;  /* urb's owned by USB core */
	struct work_struct work;  /* bottom half */
	struct usb_anchor pending;  /* urb's waiting for driver bottom half */
	int buffer_sz;  /* allocated urb buffer size */
};

struct mdm6600_port {
	struct usb_serial *serial;
	struct usb_serial_port *port;

	struct mdm6600_urb_write_pool write;
	struct mdm6600_urb_read_pool read;

	u16 tiocm_status;
};

static void mdm6600_read_bulk_work(struct work_struct *work);
static void mdm6600_read_bulk_cb(struct urb *urb);
static void mdm6600_write_bulk_cb(struct urb *urb);

/* called after probe for each of 5 usb_serial interfaces */
static int mdm6600_attach(struct usb_serial *serial)
{
	int i;
	struct mdm6600_port *modem;
	struct usb_host_interface *host_iface =
		serial->interface->cur_altsetting;
	struct usb_endpoint_descriptor *epwrite = NULL;
	struct usb_endpoint_descriptor *epread = NULL;

	modem = kzalloc(sizeof(*modem), GFP_KERNEL);
	if (!modem)
		return -ENOMEM;
	usb_set_serial_data(serial, modem);

	modem->serial = serial;
	modem->port = modem->serial->port[0]; /* always 1 port per usb_serial */
	modem->tiocm_status = 0;

	/* find endpoints */
	for (i = 0; i < host_iface->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep =
			&host_iface->endpoint[i].desc;
		if (usb_endpoint_is_bulk_out(ep))
			epwrite = ep;
		if (usb_endpoint_is_bulk_in(ep))
			epread = ep;
	}
	if (!epwrite) {
		dev_err(&serial->dev->dev, "%s No bulk out endpoint\n",
			__func__);
		return -EIO;
	}
	if (!epread) {
		dev_err(&serial->dev->dev, "%s No bulk in endpoint\n",
			__func__);
		return -EIO;
	}

	/* setup write pool */
	spin_lock_init(&modem->write.busy_lock);
	init_usb_anchor(&modem->write.in_flight);
	/* The * 20 calculation is from Motorola's original driver, I do not
	 * know the reasoning */
	modem->write.buffer_sz = le16_to_cpu(epwrite->wMaxPacketSize) * 20;
	for (i = 0; i < POOL_SZ; i++) {
		struct urb *u = usb_alloc_urb(0, GFP_KERNEL);
		if (!u)
			return -ENOMEM;
		u->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		u->transfer_buffer = usb_alloc_coherent(serial->dev,
			modem->write.buffer_sz, GFP_KERNEL, &u->transfer_dma);
		if (!u->transfer_buffer)
			return -ENOMEM;
		usb_fill_bulk_urb(u, serial->dev,
			usb_sndbulkpipe(serial->dev, epwrite->bEndpointAddress),
			u->transfer_buffer, modem->write.buffer_sz,
			mdm6600_write_bulk_cb, modem);
		modem->write.urb[i] = u;
		modem->write.busy[i] = false;
	}

	/* read pool */
	INIT_WORK(&modem->read.work, mdm6600_read_bulk_work);
	init_usb_anchor(&modem->read.in_flight);
	init_usb_anchor(&modem->read.pending);
	/* The * 2 calculation is from Motorola's original driver, I do not
	 * know the reasoning */
	modem->read.buffer_sz = le16_to_cpu(epwrite->wMaxPacketSize) * 2;
	for (i = 0; i < POOL_SZ; i++) {
		struct urb *u = usb_alloc_urb(0, GFP_KERNEL);
		if (!u)
			return -ENOMEM;
		u->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		u->transfer_buffer = usb_alloc_coherent(serial->dev,
			modem->read.buffer_sz, GFP_KERNEL, &u->transfer_dma);
		if (!u->transfer_buffer)
			return -ENOMEM;
		usb_fill_bulk_urb(u, serial->dev,
			usb_rcvbulkpipe(serial->dev, epread->bEndpointAddress),
			u->transfer_buffer, modem->read.buffer_sz,
			mdm6600_read_bulk_cb, modem);
		modem->read.urb[i] = u;
	}

	return 0;
}

static void mdm6600_disconnect(struct usb_serial *serial)
{
	struct mdm6600_port *modem = usb_get_serial_data(serial);

	dbg("%s: port %d", __func__, modem->port->number);

	/* cancel pending writes */
	usb_kill_anchored_urbs(&modem->write.in_flight);

	/* stop reading from mdm6600 */
	usb_kill_anchored_urbs(&modem->read.in_flight);
	usb_kill_urb(modem->port->interrupt_in_urb);

	/* cancel read bottom half */
	cancel_work_sync(&modem->read.work);

	/* drop pending reads */
	usb_scuttle_anchored_urbs(&modem->read.pending);

	modem->tiocm_status = 0;
}

static void mdm6600_release_urb(struct urb *u, int sz)
{
	usb_free_coherent(u->dev, sz, u->transfer_buffer, u->transfer_dma);
	u->transfer_buffer = NULL;
	usb_free_urb(u);
}

static void mdm6600_release(struct usb_serial *serial)
{
	struct mdm6600_port *modem = usb_get_serial_data(serial);
	int i;

	for (i = 0; i < POOL_SZ; i++) {
		mdm6600_release_urb(modem->write.urb[i],
			modem->write.buffer_sz);
		modem->write.urb[i] = NULL;
		mdm6600_release_urb(modem->read.urb[i], modem->read.buffer_sz);
		modem->read.urb[i] = NULL;
	}
}

/* called when tty is opened */
static int mdm6600_open(struct tty_struct *tty, struct usb_serial_port *port)
{
	int i;
	int rc = 0;
	struct mdm6600_port *modem = usb_get_serial_data(port->serial);

	dbg("%s: port %d", __func__, port->number);

	BUG_ON(modem->port != port);

	modem->tiocm_status = 0;

	if (port->number == MODEM_INTERFACE_NUM) {
		BUG_ON(!port->interrupt_in_urb);
		rc = usb_submit_urb(port->interrupt_in_urb, GFP_KERNEL);
		if (rc) {
			dev_err(&port->dev,
			    "%s: failed to submit interrupt urb, error %d\n",
			    __func__, rc);
			return rc;
		}
	}
	for (i = 0; i < POOL_SZ; i++) {
		rc = usb_submit_urb(modem->read.urb[i], GFP_KERNEL);
		if (rc) {
			dev_err(&port->dev,
			    "%s: failed to submit bulk read urb, error %d\n",
			    __func__, rc);
			return rc;
		}
	}

	return rc;
}

static void mdm6600_close(struct usb_serial_port *port)
{
	struct mdm6600_port *modem = usb_get_serial_data(port->serial);

	dbg("%s: port %d", __func__, port->number);

	/* cancel pending writes */
	usb_kill_anchored_urbs(&modem->write.in_flight);

	/* stop reading from mdm6600 */
	usb_kill_anchored_urbs(&modem->read.in_flight);
	usb_kill_urb(port->interrupt_in_urb);

	/* cancel read bottom half */
	cancel_work_sync(&modem->read.work);

	/* drop pending reads */
	usb_scuttle_anchored_urbs(&modem->read.pending);

	modem->tiocm_status = 0;
}

static struct urb *mdm6600_get_unused_write_urb(
	struct mdm6600_urb_write_pool *p)
{
	int i;
	unsigned long flags;
	struct urb *u = NULL;

	spin_lock_irqsave(&p->busy_lock, flags);

	for (i = 0; i < POOL_SZ; i++)
		if (!p->busy[i])
			break;
	if (i >= POOL_SZ)
		goto out;

	u = p->urb[i];
	p->busy[i] = true;

out:
	spin_unlock_irqrestore(&p->busy_lock, flags);
	return u;
}

static int mdm6600_mark_write_urb_unused(struct mdm6600_urb_write_pool *p,
	struct urb *u)
{
	int i;
	unsigned long flags;
	int rc = -EINVAL;

	spin_lock_irqsave(&p->busy_lock, flags);

	for (i = 0; i < POOL_SZ; i++)
		if (p->urb[i] == u)
			break;
	if (i >= POOL_SZ)
		goto out;

	p->busy[i] = false;
	rc = 0;

out:
	spin_unlock_irqrestore(&p->busy_lock, flags);
	return rc;
}

static void mdm6600_write_bulk_cb(struct urb *u)
{
	int status;
	struct mdm6600_port *modem = u->context;

	dbg("%s: urb %p status %d", __func__, u, u->status);

	status = u->status;
	if (status)
		dev_warn(&modem->serial->dev->dev, "%s non-zero status %d\n",
			__func__, u->status);

	if (mdm6600_mark_write_urb_unused(&modem->write, u))
		dev_warn(&modem->serial->dev->dev, "%s unknown urb %p\n",
			__func__, u);

	if (!status)
		usb_serial_port_softint(modem->port);
}

static int mdm6600_write(struct tty_struct *tty, struct usb_serial_port *port,
			const unsigned char *buf, int count)
{
	int rc;
	struct urb *u;
	struct usb_serial *serial = port->serial;
	struct mdm6600_port *modem = usb_get_serial_data(port->serial);

	dbg("%s: port %d count %d pool %p", __func__, port->number, count,
		&modem->write);

	if (!count || !serial->num_bulk_out)
		return 0;

	u = mdm6600_get_unused_write_urb(&modem->write);
	if (!u) {
		dev_info(&port->dev, "%s: all buffers busy!\n", __func__);
		return 0;
	}

	count = min(count, modem->write.buffer_sz);
	memcpy(u->transfer_buffer, buf, count);
	u->transfer_buffer_length = count;
	usb_serial_debug_data(debug_data, &port->dev, __func__,
		u->transfer_buffer_length, u->transfer_buffer);

	rc = usb_submit_urb(u, GFP_ATOMIC);
	if (rc < 0) {
		dev_err(&port->dev, "%s: submit bulk urb failed %d\n",
			__func__, rc);
		return rc;
	}

	return count;
}

static int mdm6600_tiocmget(struct tty_struct *tty, struct file *file)
{
	struct usb_serial_port *port = tty->driver_data;
	struct mdm6600_port *modem = usb_get_serial_data(port->serial);

	dbg("%s: port %d modem_status %x\n", __func__, port->number,
		modem->tiocm_status);

	return modem->tiocm_status;
}

static int mdm6600_dtr_control(struct usb_serial_port *port, int ctrl)
{
	struct usb_device *dev = port->serial->dev;
	struct usb_interface *iface = port->serial->interface;
	u8 request = 0x22;
	u8 request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT;
	int timeout = HZ * 5;
	int rc;

	rc = usb_autopm_get_interface(iface);
	if (rc < 0) {
		dev_err(&dev->dev, "%s %s autopm failed %d",
			dev_driver_string(&iface->dev), dev_name(&iface->dev),
			rc);
		return rc;
	}

	rc = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), request,
		request_type, ctrl, port->number, NULL, 0, timeout);
	usb_autopm_put_interface(iface);

	return rc;
}

static int mdm6600_tiocmset(struct tty_struct *tty, struct file *file,
			unsigned int set, unsigned int clear)
{
	struct usb_serial_port *port = tty->driver_data;

	dbg("%s: port %d set %x clear %x\n", __func__, port->number, set,
		clear);

	if (port->number != MODEM_INTERFACE_NUM)
		return 0;
	if (clear & TIOCM_DTR)
		return mdm6600_dtr_control(port, 0);
	if (set & TIOCM_DTR)
		return mdm6600_dtr_control(port, 1);
	return 0;
}

static void mdm6600_apply_bp_status(u8 bp_status, u16 *tiocm_status)
{
	if (bp_status & BP_STATUS_CAR)
		*tiocm_status |= TIOCM_CAR;
	else
		*tiocm_status &= ~TIOCM_CAR;
	if (bp_status & BP_STATUS_DSR)
		*tiocm_status |= TIOCM_DSR;
	else
		*tiocm_status &= ~TIOCM_DSR;
	if (bp_status & BP_STATUS_RNG)
		*tiocm_status |= TIOCM_RNG;
	else
		*tiocm_status &= ~TIOCM_RNG;
}

static void mdm6600_read_int_callback(struct urb *u)
{
	int rc;
	u16 request;
	u8 *data = u->transfer_buffer;
	struct usb_serial_port *port = u->context;
	struct mdm6600_port *modem = usb_get_serial_data(port->serial);

	dbg("%s: urb %p", __func__, u);

	switch (u->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		dbg("%s: urb terminated, status %d", __func__, u->status);
		goto exit;
	default:
		dbg("%s: urb status non-zero %d", __func__, u->status);
		goto exit;
	}

	usb_serial_debug_data(debug_data, &port->dev, __func__,
		u->actual_length, data);

	if (u->actual_length < 2) {
		dbg("%s: interrupt transfer too small %d",
			__func__, u->actual_length);
		goto exit;
	}
	request = *((u16 *)data);

	switch (request) {
	case BP_MODEM_STATUS:
		if (u->actual_length < 9) {
			dev_err(&port->dev,
				"%s: modem status urb too small %d\n",
				__func__, u->actual_length);
			break;
		}
		if (port->number != MODEM_INTERFACE_NUM)
			break;
		mdm6600_apply_bp_status(data[8], &modem->tiocm_status);
		dbg("%s: modem_status now %x", __func__, modem->tiocm_status);
		break;
	case BP_RSP_AVAIL:
		dbg("%s: BP_RSP_AVAIL", __func__);
		break;
	case BP_SPEED_CHANGE:
		dbg("%s: BP_SPEED_CHANGE", __func__);
		break;
	default:
		dbg("%s: undefined BP request type %d", __func__, request);
		break;
	}

exit:
	rc = usb_submit_urb(u, GFP_ATOMIC);
	if (rc)
		dev_err(&u->dev->dev,
			"%s: Error %d re-submitting interrupt urb\n",
			__func__, rc);
}

static size_t mdm6600_pass_to_tty(struct tty_struct *tty, void *buf, size_t sz)
{
	unsigned char *b = buf;
	size_t c;
	size_t s = sz;

	tty_buffer_request_room(tty, sz);
	while (s > 0) {
		c = tty_insert_flip_string(tty, b, s);
		if (c != s)
			dbg("%s passed only %u of %u bytes\n",
				__func__, c, s);
		if (c == 0)
			break;
		tty_flip_buffer_push(tty);
		s -= c;
		b += c;
	}

	return sz - s;
}

static void mdm6600_read_bulk_work(struct work_struct *work)
{
	int rc;
	size_t c;
	struct urb *u;
	struct tty_struct *tty;
	struct mdm6600_port *modem = container_of(work, struct mdm6600_port,
		read.work);

	dbg("%s", __func__);

	while (true) {
		u = usb_get_from_anchor(&modem->read.pending);
		if (!u)
			break;
		dbg("%s: processing urb %p len %u", __func__, u,
			u->actual_length);
		usb_serial_debug_data(debug_data, &modem->port->dev, __func__,
			u->actual_length, u->transfer_buffer);
		tty = tty_port_tty_get(&modem->port->port);
		if (!tty) {
			dev_warn(&modem->port->dev, "%s: could not find tty\n",
				__func__);
			goto next;
		}
		c = mdm6600_pass_to_tty(tty, u->transfer_buffer,
			u->actual_length);
		if (c != u->actual_length)
			dev_warn(&modem->port->dev,
				"%s: dropped %u of %u bytes\n",
				__func__, u->actual_length - c,
				u->actual_length);
		tty_kref_put(tty);

next:
		rc = usb_submit_urb(u, GFP_KERNEL);
		if (rc)
			dev_err(&u->dev->dev,
				"%s: Error %d re-submitting read urb\n",
				__func__, rc);
	}
}

static void mdm6600_read_bulk_cb(struct urb *u)
{
	struct mdm6600_port *modem = u->context;

	dbg("%s: urb %p", __func__, u);

	if (u->status) {
		int rc;
		dev_warn(&modem->serial->dev->dev, "%s non-zero status %d\n",
			__func__, u->status);
		/* straight back into use */
		rc = usb_submit_urb(u, GFP_ATOMIC);
		if (rc)
			dev_err(&u->dev->dev,
				"%s: Error %d re-submitting read urb\n",
				__func__, rc);
		return;
	}

	usb_anchor_urb(u, &modem->read.pending);
	schedule_work(&modem->read.work);
}

static struct usb_driver mdm6600_usb_driver = {
	.name =		"mdm6600",
	.probe =	usb_serial_probe,
	.disconnect =	usb_serial_disconnect,
	.id_table =	mdm6600_id_table,
	.no_dynamic_id = 	1,
};

static struct usb_serial_driver mdm6600_usb_serial_driver = {
	.driver = {
		.owner =	THIS_MODULE,
		.name =		"mdm6600",
	},
	.num_ports =		1,
	.description =		"MDM 6600 modem usb-serial driver",
	.id_table =		mdm6600_id_table,
	.usb_driver =		&mdm6600_usb_driver,
	.attach =		mdm6600_attach,
	.disconnect =		mdm6600_disconnect,
	.release =		mdm6600_release,
	.open =			mdm6600_open,
	.close = 		mdm6600_close,
	.write = 		mdm6600_write,
	.tiocmset =		mdm6600_tiocmset,
	.tiocmget =		mdm6600_tiocmget,
	.read_int_callback =	mdm6600_read_int_callback,
};

static int __init mdm6600_init(void)
{
	int retval;

	retval = usb_serial_register(&mdm6600_usb_serial_driver);
	if (retval)
		return retval;
	retval = usb_register(&mdm6600_usb_driver);
	if (retval)
		usb_serial_deregister(&mdm6600_usb_serial_driver);
	return retval;
}

static void __exit mdm6600_exit(void)
{
	usb_deregister(&mdm6600_usb_driver);
	usb_serial_deregister(&mdm6600_usb_serial_driver);
}

module_init(mdm6600_init);
module_exit(mdm6600_exit);
MODULE_LICENSE("GPL");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug enabled or not");
module_param(debug_data, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug_data, "Debug enabled or not");