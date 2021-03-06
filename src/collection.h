/*
 * collection.h
 *
 * Copyright (C) 2009 Hector Martin <hector@marcansoft.com>
 * Copyright (C) 2009 Nikias Bassen <nikias@gmx.li>
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

#ifndef COLLECTION_H
#define COLLECTION_H

struct collection {
	void **list;
	int capacity;
};

void collection_init(struct collection *col);
void collection_add(struct collection *col, void *element);
void collection_remove(struct collection *col, void *element);
int collection_count(struct collection *col);
void collection_free(struct collection *col);
void collection_copy(struct collection *dest, struct collection *src);

#endif