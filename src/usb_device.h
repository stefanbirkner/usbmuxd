/*
 * usb_device.h
 *
 * Copyright (C) 2009 Hector Martin <hector@marcansoft.com>
 * Copyright (C) 2009 Nikias Bassen <nikias@gmx.li>
 * Copyright (C) 2009 Martin Szulecki <opensuse@sukimashita.com>
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

#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <libusb.h>
#include "collection.h"

// libusb fragments packets larger than this (usbfs limitation)
// on input, this creates race conditions and other issues
#define USB_MRU 16384

struct usb_device {
	libusb_device_handle *dev;
	uint8_t bus, address;
	char serial[256];
	int alive;
	uint8_t interface, ep_in, ep_out;
	struct collection rx_xfers;
	struct collection tx_xfers;
	int wMaxPacketSize;
	uint64_t speed;
	struct libusb_device_descriptor devdesc;
};

int usb_device_disconnect(struct usb_device *dev);
const char *usb_device_get_serial(struct usb_device *dev);
uint32_t usb_device_get_location(struct usb_device *dev);
uint16_t usb_device_get_pid(struct usb_device *dev);
uint64_t usb_device_get_speed(struct usb_device *dev);
// Start a read-callback loop for this device
int usb_device_start_rx_loop(struct usb_device *dev, libusb_transfer_cb_fn callback);
int usb_device_send(struct usb_device *dev, const unsigned char *buf, int length);

#endif
