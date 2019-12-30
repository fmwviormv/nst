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
#define select(r,w,t)	select(OpenMax, r, w, NULL, t)

void		 send_reset(int, enum Msg);
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

	send_reset(udp_s, Msg_Reset);
	msg_reset(peer);
	close(STDERR_FILENO); /* we need that filedescriptor :D */

	for (;;) {
		struct timeval	 timeout;

		if (resend_c >= ResetAfter) {
			resend_c = 0;
			send_reset(udp_s, Msg_Reset);
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
send_reset(const int s, enum Msg msg)
{
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

	if (FD_ISSET(tcp_s, &rfds)) {
		struct sockaddr	 addr;
		socklen_t	 addrlen = (socklen_t)sizeof(addr);
		int		 s;

		s = accept(tcp_s, &addr, &addrlen);
		for (i = 0; i < PeersMax; ++i) {
			if (peer[i].free && s != -1) {
				if (peer[i].s != -1)
					close(peer[i].s);

				peer[i].free = 0;
				peer[i].dontsend = 0;
				peer[i].s = s;
				peer[i].recv.open = 0;
				peer[i].recv.close = 0;
				peer[i].recv.off = 0;
				peer[i].recv.size = 0;
				peer[i].send.open = 1;
				peer[i].send.size = 0;
				break;
			}
		}
	}

	for (i = 0; i < PeersMax; ++i) {
		const int	 s = peer[i].s;

		if (s == -1)
			continue;

		if (FD_ISSET(s, &rfds)) {
			uint8_t		*buf = peer[i].send.buf;
			size_t		 off = peer[i].send.size;
			ssize_t		 nr;

			off = peer[i].send.size;
			nr = read(s, buf + off, PeerMaxSend - off);
			if (nr == -1) {
				close(s);
				peer[i].s = -1;
				peer[i].send.close = 1;
			} else if (nr == 0) {
				close(s);
				peer[i].s = -1;
				peer[i].send.close = 1;
			} else
				peer[i].send.size += (size_t)nr;
		}

		if (FD_ISSET(s, &wfds)) {
			uint8_t		*buf = peer[i].recv.buf;
			size_t		 off = peer[i].recv.off;
			size_t		 size = peer[i].recv.size;
			ssize_t		 nw;

			if (size == 0) {
				close(s);
				peer[i].free = 1;
				peer[i].s = -1;
			} else if ((nw = write(s, buf + off, size)) == -1) {
				close(s);
				peer[i].s = -1;
				peer[i].send.close = 1;
			} else {
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
	}
}

void
proc_message(void)
{
	if (!msg_process(peer))
		return;

	while (msg_process(peer)) ;
}
