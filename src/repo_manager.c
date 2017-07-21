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
#include "repo_manager.h"
#include <stdio.h>
#include "os/sandbox.h"
#include "os/io.h"

int git_lfs_repo_manager_service(int socket)
{
	// allow file-io
	if(os_sandbox(SANDBOX_FILEIO) < 0) {
		fprintf(stderr, "Sandbox fail.\n");
		return -1;
	}
	
	for(;;) {
		char dummy;
		if(os_read(socket, &dummy, 1) < 0) {
			break;
		}
	}
	
	return 0;
}
