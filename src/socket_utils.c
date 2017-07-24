/*
 * Copyright (c) 2017 Sound <sound@sagaforce.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "socket_utils.h"
#include "os/io.h"
#include "os/socket.h"

int socket_read_fully(int fd, void *buffer, int size)
{
	int n = 0;
	int total = 0;
	
	while(size > 0 && (n = os_read(fd, buffer, size)) > 0)
	{
		buffer = (char *)buffer + n;
		size -= n;
		total += n;
	}
	
	if(n < 0) {
		return -1;
	}
	
	return total;
}

int socket_write_fully(int fd, const void *buffer, int size)
{
	int n = 0;
	int total = 0;
	
	while(size > 0 && (n = os_write(fd, buffer, size)) > 0)
	{
		buffer = (char *)buffer + n;
		size -= n;
		total += n;
	}
	
	if(n < 0) {
		return -1;
	}
	
	return total;
}
