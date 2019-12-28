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
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "msg.h"

#define socket(a)	socket(a.ai_family,a.ai_socktype,a.ai_protocol)
#define bind(s,a)	bind(s, a.ai_addr, a.ai_addrlen)

void		 send_reset(int);
void		 recv_message(int, int, struct timeval *);
void		 proc_message(void);

struct peer	 peer[PeersMax];

int
main(void)
{
	struct rlimit	 nofile = { OpenMax, OpenMax };
	int		 udp_s, tcp_s;
	int		 i, resend_c = ResetAfter;

	if (setrlimit(RLIMIT_NOFILE, &nofile) == -1)
		err(1, "setrlimit");
	for (i = 0; i < OpenMax; ++i)
		if (i != STDERR_FILENO)
			close(i);
#ifdef USE_UNVEIL
	if (unveil("/", "") == -1)
		err(1, "unveil");
	if (unveil(NULL, NULL) == -1)
		err(1, "unveil");
#endif
#ifdef USE_PLEDGE
	if (pledge("stdio inet", NULL) == -1)
		err(1, "pledge");
#endif

	if ((udp_s = socket(client_ai)) == -1)
		err(1, "socket");
	if (bind(udp_s, client_ai) == -1)
		err(1, "bind");

	if ((tcp_s = socket(listen_ai)) == -1)
		err(1, "socket");
	if (bind(tcp_s, listen_ai) == -1)
		err(1, "bind");
	if (listen(tcp_s, 2) == -1)
		err(1, "listen");

	close(STDERR_FILENO); /* we need that filedescriptor :D */
	msg_reset(peer);

	for (;;) {
		struct timeval	 timeout;

		if (resend_c >= ResetAfter) {
			send_reset(udp_s);
			msg_reset(peer);
		} else if (msg_gettimeout(&timeout)) {
			recv_message(udp_s, tcp_s, &timeout);
			proc_message();
		} else if (msg_resendold(udp_s, &server_ai)) {
			++resend_c;
		} else {
			resend_c = 0;
			msg_send(udp_s, peer, &server_ai);
		}
	}

	return 0;
}

void
send_reset(const int s)
{
	enum Msg	 msg = Msg_Reset;
	fd_set		 fds;
	struct timeval	 timeout;

	for (;;) {
		if (msg_gettimeout(&timeout) == NULL) {
			msg_sendreset(s, msg, &server_ai);
			if (msg == Msg_Reset_OK)
				return;
		} else {
			FD_ZERO(&fds);
			FD_SET(s, &fds);
			select(OpenMax, &fds, NULL, NULL, &timeout);

			switch (msg_recv(s)) {
			case Msg_Reset:
				msg = Msg_Reset_OK;
				break;
			case Msg_Reset_OK:
				return;
			default:
				break;
			}
		}
	}
}

void
recv_message(const int udp_s, const int tcp_s,
    struct timeval *const timeout)
{
	fd_set		 rfds, wfds;
	int		 i;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_SET(udp_s, &rfds);

	for (i = 0; i < PeersMax; ++i) {
		if (peer[i].free)
			FD_SET(tcp_s, &rfds);
		if (peer[i].s < 0)
			continue;

		if (peer[i].send.size < PeerMaxSend)
			FD_SET(peer[i].s, &rfds);
		if (peer[i].recv.size > 0 || peer[i].recv.close)
			FD_SET(peer[i].s, &wfds);
	}

	if (select(OpenMax, &rfds, &wfds, NULL, timeout) < 1)
		return;

	if (FD_ISSET(udp_s, &rfds))
		msg_recv(udp_s);

	if (FD_ISSET(tcp_s, &rfds)) {
		struct sockaddr	 addr;
		socklen_t	 addrlen = (socklen_t)sizeof(addr);
		int		 s;

		s = accept4(tcp_s, &addr, &addrlen, SOCK_NONBLOCK);
		for (i = 0; i < PeersMax; ++i) {
			if (peer[i].s < 0 && peer[i].free && s >= 0) {
				peer[i].free = 0;
				peer[i].dontsend = 0;
				peer[i].s = s;
				peer[i].recv.open = 0;
				peer[i].recv.close = 0;
				peer[i].recv.off = 0;
				peer[i].recv.size = 0;
				peer[i].send.open = 1;
				peer[i].send.close = 0;
				peer[i].send.size = 0;
				break;
			}
		}
	}

	for (i = 0; i < PeersMax; ++i) {
		const int	 s = peer[i].s;
		uint8_t		*buf;
		ssize_t		 off, size;

		if (s < 0)
			continue;

		if (FD_ISSET(s, &rfds)) {
			buf = peer[i].send.buf;
			off = peer[i].send.size;
			size = read(s, buf + off, PeerMaxSend - off);
			if (size > 0)
				peer[i].send.size += size;
			else
				switch (size == 0 ? ENOTCONN : errno) {
				case EBADF:
				case EIO:
				case ENOTCONN:
					close(s);
					peer[i].s = -1;
					peer[i].send.close = 1;
					break;
				default:
					break;
				}
		}

		if (FD_ISSET(s, &wfds)) {
			buf = peer[i].recv.buf;
			off = peer[i].recv.off;
			size = write(s, buf + off, peer[i].recv.size);
			if (size >= 0) {
				off += size;
				size = peer[i].recv.size -= size;
				if (off >= MoveSize) {
					memmove(buf, buf + off, size);
					off = 0;
				}
				peer[i].recv.off = off;
				if (size == 0 && peer[i].recv.close) {
					close(s);
					peer[i].s = -1;
					peer[i].free = 1;
				}
			} else
				switch (errno) {
				case EBADF:
				case EIO:
				case ENETDOWN:
				case EPIPE:
					close(s);
					peer[i].s = -1;
					peer[i].send.close = 1;
					break;
				default:
					break;
				}
		}
	}
}

void
proc_message(void)
{
	if (!msg_process(peer))
		return;

	while (msg_process(peer)) ;
}
