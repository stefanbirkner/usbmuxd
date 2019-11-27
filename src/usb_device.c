/*
 * usb_device.c
 *
 * Copyright (C) 2009 Hector Martin <hector@marcansoft.com>
 * Copyright (C) 2009 Nikias Bassen <nikias@gmx.li>
 * Copyright (C) 2009 Martin Szulecki <opensuse@sukimashita.com>
 * Copyright (C) 2014 Mikkel Kamstrup Erlandsen <mikkel.kamstrup@xamarin.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <libusb.h>

#include "collection.h"
#include "usb.h"
#include "usb_device.h"
#include "log.h"
#include "utils.h"

int usb_device_disconnect(struct usb_device *dev)
{
	if(!dev->dev) {
		return -1;
	}

	// kill the rx xfer and tx xfers and try to make sure the callbacks
	// get called before we free the device
	FOREACH(struct libusb_transfer *xfer, &dev->rx_xfers) {
		usbmuxd_log(LL_DEBUG, "usb_device_disconnect: cancelling RX xfer %p", xfer);
		libusb_cancel_transfer(xfer);
	} ENDFOREACH

	FOREACH(struct libusb_transfer *xfer, &dev->tx_xfers) {
		usbmuxd_log(LL_DEBUG, "usb_device_disconnect: cancelling TX xfer %p", xfer);
		libusb_cancel_transfer(xfer);
	} ENDFOREACH

	// Busy-wait until all xfers are closed
	while(collection_count(&dev->rx_xfers) || collection_count(&dev->tx_xfers)) {
		struct timeval tv;
		int res;

		tv.tv_sec = 0;
		tv.tv_usec = 1000;
		if((res = libusb_handle_events_timeout(NULL, &tv)) < 0) {
			usbmuxd_log(LL_ERROR, "libusb_handle_events_timeout for usb_device_disconnect failed: %s", libusb_error_name(res));
			break;
		}
	}

	collection_free(&dev->tx_xfers);
	collection_free(&dev->rx_xfers);
	libusb_release_interface(dev->dev, dev->interface);
	libusb_close(dev->dev);
	dev->dev = NULL;
	return 0;
}

// Callback from write operation
static void tx_callback(struct libusb_transfer *xfer)
{
	struct usb_device *dev = xfer->user_data;
	usbmuxd_log(LL_SPEW, "TX callback dev %d-%d len %d -> %d status %d", dev->bus, dev->address, xfer->length, xfer->actual_length, xfer->status);
	if(xfer->status != LIBUSB_TRANSFER_COMPLETED) {
		switch(xfer->status) {
			case LIBUSB_TRANSFER_COMPLETED: //shut up compiler
			case LIBUSB_TRANSFER_ERROR:
				// funny, this happens when we disconnect the device while waiting for a transfer, sometimes
				usbmuxd_log(LL_INFO, "Device %d-%d TX aborted due to error or disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_TIMED_OUT:
				usbmuxd_log(LL_ERROR, "TX transfer timed out for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_CANCELLED:
				usbmuxd_log(LL_DEBUG, "Device %d-%d TX transfer cancelled", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_STALL:
				usbmuxd_log(LL_ERROR, "TX transfer stalled for device %d-%d", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_NO_DEVICE:
				// other times, this happens, and also even when we abort the transfer after device removal
				usbmuxd_log(LL_INFO, "Device %d-%d TX aborted due to disconnect", dev->bus, dev->address);
				break;
			case LIBUSB_TRANSFER_OVERFLOW:
				usbmuxd_log(LL_ERROR, "TX transfer overflow for device %d-%d", dev->bus, dev->address);
				break;
			// and nothing happens (this never gets called) if the device is freed after a disconnect! (bad)
			default:
				// this should never be reached.
				break;
		}
		// we can't usb_device_disconnect here due to a deadlock, so instead mark it as dead and reap it after processing events
		// we'll do device_remove there too
		dev->alive = 0;
	}
	if(xfer->buffer)
		free(xfer->buffer);
	collection_remove(&dev->tx_xfers, xfer);
	libusb_free_transfer(xfer);
}

int usb_device_send(struct usb_device *dev, const unsigned char *buf, int length)
{
	int res;
	struct libusb_transfer *xfer = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(xfer, dev->dev, dev->ep_out, (void*)buf, length, tx_callback, dev, 0);
	if((res = libusb_submit_transfer(xfer)) < 0) {
		usbmuxd_log(LL_ERROR, "Failed to submit TX transfer %p len %d to device %d-%d: %s", buf, length, dev->bus, dev->address, libusb_error_name(res));
		libusb_free_transfer(xfer);
		return res;
	}
	collection_add(&dev->tx_xfers, xfer);
	if (length % dev->wMaxPacketSize == 0) {
		usbmuxd_log(LL_DEBUG, "Send ZLP");
		// Send Zero Length Packet
		xfer = libusb_alloc_transfer(0);
		void *buffer = malloc(1);
		libusb_fill_bulk_transfer(xfer, dev->dev, dev->ep_out, buffer, 0, tx_callback, dev, 0);
		if((res = libusb_submit_transfer(xfer)) < 0) {
			usbmuxd_log(LL_ERROR, "Failed to submit TX ZLP transfer to device %d-%d: %s", dev->bus, dev->address, libusb_error_name(res));
			libusb_free_transfer(xfer);
			return res;
		}
		collection_add(&dev->tx_xfers, xfer);
	}
	return 0;
}

// Start a read-callback loop for this device
int usb_device_start_rx_loop(struct usb_device *dev, libusb_transfer_cb_fn callback)
{
	int res;
	void *buf;
	struct libusb_transfer *xfer = libusb_alloc_transfer(0);
	buf = malloc(USB_MRU);
	libusb_fill_bulk_transfer(xfer, dev->dev, dev->ep_in, buf, USB_MRU, callback, dev, 0);
	if((res = libusb_submit_transfer(xfer)) != 0) {
		usbmuxd_log(LL_ERROR, "Failed to submit RX transfer to device %d-%d: %s", dev->bus, dev->address, libusb_error_name(res));
		libusb_free_transfer(xfer);
		return res;
	}

	collection_add(&dev->rx_xfers, xfer);

	return 0;
}

const char *usb_device_get_serial(struct usb_device *dev)
{
	if(!dev->dev) {
		return NULL;
	}
	return dev->serial;
}

uint32_t usb_device_get_location(struct usb_device *dev)
{
	if(!dev->dev) {
		return 0;
	}
	return (dev->bus << 16) | dev->address;
}

uint16_t usb_device_get_pid(struct usb_device *dev)
{
	if(!dev->dev) {
		return 0;
	}
	return dev->devdesc.idProduct;
}

uint64_t usb_device_get_speed(struct usb_device *dev)
{
	if (!dev->dev) {
		return 0;
	}
	return dev->speed;
}
