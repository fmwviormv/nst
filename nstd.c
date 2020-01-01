/*
 * Copyright (c) 2019, 2020 Ali Farzanrad <ali_farzanrad@riseup.net>
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
#define connect(s,a)	connect(s, a.ai_addr, a.ai_addrlen)
#define select(r,w,t)	select(OpenMax, r, w, NULL, t)

void		 send_reset(int, enum Msg);
void		 recv_message(int, struct timeval *);
void		 proc_message(void);

struct peer	 peer[PeersMax];

int
main(void)
{
	struct rlimit	 nofile = { OpenMax, OpenMax };
	int		 udp_s;
	int		 i, resend_c = ResetAfter;

	if (setrlimit(RLIMIT_NOFILE, &nofile) == -1)
		err(1, "setrlimit");
	for (i = 0; i < OpenMax; ++i)
		if (i != STDERR_FILENO)
			close(i);
	for (i = 0; i < PeersMax; ++i)
		peer[i].s = -1;
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

	if ((udp_s = socket(server_ai)) == -1)
		err(1, "socket");
	if (bind(udp_s, server_ai) == -1)
		err(1, "bind");

	send_reset(udp_s, Msg_Reset);
	msg_reset(peer);

	for (;;) {
		struct timeval	 timeout;

		if (resend_c >= ResetAfter) {
			resend_c = 0;
			send_reset(udp_s, Msg_Reset);
			msg_reset(peer);
		} else if (msg_gettimeout(&timeout)) {
			recv_message(udp_s, &timeout);
			proc_message();
		} else if (msg_resendold(udp_s, &client_ai)) {
			++resend_c;
		} else {
			resend_c = 0;
			msg_send(udp_s, peer, &client_ai);
		}
	}

	return 0;
}

void
send_reset(const int s, enum Msg msg)
{
	fd_set		 fds;
	struct timeval	 timeout;

	for (;;) {
		if (msg_gettimeout(&timeout) == NULL) {
			msg_sendreset(s, msg, &client_ai);
			if (msg == Msg_Reset_OK)
				return;
		} else {
			FD_ZERO(&fds);
			FD_SET(s, &fds);
			if (select(&fds, NULL, &timeout) < 1)
				continue;

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
recv_message(const int udp_s, struct timeval *const timeout)
{
	fd_set		 rfds, wfds;
	int		 i;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_SET(udp_s, &rfds);

	for (i = 0; i < PeersMax; ++i) {
		if (peer[i].s == -1)
			continue;

		if (peer[i].send.size < PeerMaxSend)
			FD_SET(peer[i].s, &rfds);
		if (peer[i].recv.size > 0 || peer[i].recv.close)
			FD_SET(peer[i].s, &wfds);
	}

	if (select(&rfds, &wfds, timeout) < 1)
		return;

	if (FD_ISSET(udp_s, &rfds)) {
		if (msg_recv(udp_s) == Msg_Reset) {
			send_reset(udp_s, Msg_Reset_OK);
			msg_reset(peer);
			return;
		}
	}

	for (i = 0; i < PeersMax; ++i) {
		int		 s = peer[i].s;
		errno = 0;

		if (s != -1 && FD_ISSET(s, &rfds)) {
			uint8_t		*buf = peer[i].send.buf;
			size_t		 off = peer[i].send.size;
			ssize_t		 nr;

			nr = recv(s, buf + off, PeerMaxSend - off, 0);
			if (nr == -1)
				s = -1;
			else
				peer[i].send.size += (size_t)nr;
		}

		if (s != -1 && FD_ISSET(s, &wfds)) {
			uint8_t		*buf = peer[i].recv.buf;
			size_t		 off = peer[i].recv.off;
			size_t		 size = peer[i].recv.size;
			ssize_t		 nw;

			if (size == 0 ||
			    (nw = send(s, buf + off, size, 0)) == -1)
				s = -1;
			else {
				off += (size_t)nw;
				size -= (size_t)nw;

				if (off >= MoveSize) {
					memmove(buf, buf + off, size);
					off = 0;
				}

				peer[i].recv.off = off;
				peer[i].recv.size = size;
			}
		}

		if (s == -1 && peer[i].s != -1) {
			warn("peer %d closed", i);
			close(peer[i].s);
			peer[i].s = -1;
			peer[i].send.close = 1;

			if (peer[i].recv.close) {
				peer[i].free = 1;
				peer[i].recv.close = 0;
			}
		}
	}
}

void
proc_message(void)
{
	int		 i;

	if (!msg_process(peer))
		return;

	while (msg_process(peer)) ;

	for (i = 0; i < PeersMax; ++i) {
		int		 s = peer[i].s;

		if (s == -1 && peer[i].recv.open) {
			if (peer[i].recv.close)
				warnx("open/close %d", i);

			if ((s = socket(connect_ai)) == -1)
				warn("socket");
			else if (connect(s, connect_ai) == -1) {
				warn("connect");
				close(s);
				s = -1;
			}

			peer[i].free = 0;
			peer[i].s = s;
			peer[i].recv.open = 0;
			peer[i].send.size = 0;

			if (s == -1)
				peer[i].send.close = 1;
			else
				warnx("peer %d connected", i);
		}

		if (s == -1 && peer[i].recv.close) {
			peer[i].free = 1;
			peer[i].recv.close = 0;
		}
	}
}
