/*
 * Copyright (c) 2019 Ali Farzanrad <ali_farzanrad@riseup.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#define _POSIX_C_SOURCE	200809L

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <err.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "msg.h"

void		 getaddr(const char *, const char *, const char *);
void		 print(const char *, const void *, size_t);

const char	*family;
const char	*socktype;
struct protoent	*protocol;
struct addrinfo	*ai;

int
main(const int argc, const char *const *const argv)
{
	if (argc != 5)
		errx(1, "bad args");
	else if (FD_SETSIZE < OpenMax)
		errx(1, "fd_set is too small");
	else if (AlertSize <= 0)
		errx(1, "recv buf is too small");

	getaddr(argv[2], argv[3], argv[4]);
	print(argv[1], ai->ai_addr, (size_t)ai->ai_addrlen);
}

void
getaddr(const char *const type, const char *const host,
    const char *const port)
{
	struct addrinfo	 hints;
	int		 error;

	memset(&hints, 0, sizeof hints);

	if (strcmp(type, "tcp") == 0) {
		family = "AF_INET";
		socktype = "SOCK_STREAM";
		protocol = getprotobyname("tcp");
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
	} else if (strcmp(type, "tcp6") == 0) {
		family = "AF_INET6";
		socktype = "SOCK_STREAM";
		protocol = getprotobyname("tcp");
		hints.ai_family = AF_INET6;
		hints.ai_socktype = SOCK_STREAM;
	} else if (strcmp(type, "udp") == 0) {
		family = "AF_INET";
		socktype = "SOCK_DGRAM";
		protocol = getprotobyname("udp");
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
	} else if (strcmp(type, "udp6") == 0) {
		family = "AF_INET6";
		socktype = "SOCK_DGRAM";
		protocol = getprotobyname("udp");
		hints.ai_family = AF_INET6;
		hints.ai_socktype = SOCK_DGRAM;
	} else
		errx(1, "bad type");

	error = getaddrinfo(host, port, &hints, &ai);

	if (error || ai == NULL)
		errx(1, "getaddrinfo:%s", gai_strerror(error));
	else if (ai->ai_family != hints.ai_family)
		errx(1, "bad family");
	else if (ai->ai_socktype != hints.ai_socktype)
		errx(1, "bad socktype");
	else if (ai->ai_protocol != protocol->p_proto)
		errx(1, "bad protocol");
}

void
print(const char *const name, const void *const p, const size_t size)
{
	printf("\nstatic const unsigned char _%s[] = {", name);

	for (size_t i = 0; i < size; ++i) {
		if (i != 0)
			putchar(',');
		printf(" %d", (int)i[(const unsigned char *)p]);
	}

	printf(" };\n"
	    "const struct addrinfo %s = {\n"
	    "\t.ai_family = %s,\n"
	    "\t.ai_socktype = %s,\n"
	    "\t.ai_protocol = %d, /* %s */\n"
	    "\t.ai_addrlen = (socklen_t)sizeof(_%s),\n"
	    "\t.ai_addr = (struct sockaddr *)_%s\n"
	    "};\n", name, family, socktype,
	    protocol->p_proto, protocol->p_name, name, name);
}
