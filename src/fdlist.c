/*
 * fdlist.c
 *
 * Copyright (C) 2009 Hector Martin <hector@marcansoft.com>
 * Copyright (C) 2009 Nikias Bassen <nikias@gmx.li>
 * Copyright (c) 2013 Federico Mena Quintero
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define _GNU_SOURCE 

#include <poll.h>
#include <signal.h>
#include <stdlib.h>

#include "fdlist.h"

static void fdlist_add(struct fdlist *list, enum fdowner owner, int fd, short events)
{
	if(list->count == list->capacity) {
		list->capacity *= 2;
		list->owners = realloc(list->owners, sizeof(*list->owners) * list->capacity);
		list->fds = realloc(list->fds, sizeof(*list->fds) * list->capacity);
	}
	list->owners[list->count] = owner;
	list->fds[list->count].fd = fd;
	list->fds[list->count].events = events;
	list->fds[list->count].revents = 0;
	list->count++;
}

void fdlist_init(struct fdlist *list, int socket_fd)
{
	list->count = 0;
	list->capacity = 4;
	list->owners = malloc(sizeof(*list->owners) * list->capacity);
	list->fds = malloc(sizeof(*list->fds) * list->capacity);
	sigemptyset(list->empty_sigset); // unmask all signals
	fdlist_add(list, FD_LISTEN, socket_fd, POLLIN);
}

void fdlist_add_client_fd(struct fdlist *list, int fd, short events)
{
	fdlist_add(list, FD_CLIENT, fd, events);
}

void fdlist_add_usb_fd(struct fdlist *list, int fd, short events)
{
	fdlist_add(list, FD_USB, fd, events);
}

int fdlist_get_socket_fd(struct fdlist *list)
{
	return list->fds[0].fd;
}

int fdlist_is_socket_ready(struct fdlist *list)
{
	return list->fds[0].revents;
}

void fdlist_free(struct fdlist *list)
{
	list->count = 0;
	list->capacity = 0;
	free(list->owners);
	list->owners = NULL;
	free(list->fds);
	list->fds = NULL;
}

int fdlist_ppoll(struct fdlist *list, struct timespec *timeout_ts)
{
	list->fds[0].revents = 0; //reset socket fd
	return ppoll(list->fds, list->count, timeout_ts, list->empty_sigset);
}

void fdlist_remove_client_and_usb_fds(struct fdlist *list)
{
	list->count = 1;
}
