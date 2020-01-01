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

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <md5.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "msg.h"

#define IN_HISTORY(seq)		(&ihist[(seq) & (MessageHistory - 1)])
#define OUT_HISTORY(seq)	(&ohist[(seq) & (MessageHistory - 1)])
#define DIFF16(x, y)		((0x10000 + (x) - (y)) & 0xffff)

typedef int	 seq_t;

struct msg {
	char		 delivered;
	seq_t		 lasttry;
	seq_t		 seq;
	struct {
		char		 opened;
		char		 closed;
		char		 blocked;
		size_t		 size;
	} peer[PeersMax];
	uint8_t		 data[MessageDataMaxSize];
};

size_t		 msg_sendlimit(const struct peer *);
void		 msg_sendmsg(int, struct msg *, enum Msg,
		    const struct addrinfo *);

const uint8_t	 psk[] = { 1, 2, 3, 4, 5, 6, 7, 8 };

seq_t		 iseq, oseq, useq;
struct msg	 ihist[MessageHistory];
struct msg	 ohist[MessageHistory];
struct timespec	 last_sendtime;

enum Msg
msg_recv(const int s)
{
	const uint32_t	 curtime = (uint32_t)time(NULL);
	unsigned char	 buf[DatagramMaxSize + MD5_DIGEST_LENGTH];
	size_t		 i, size, datasize;
	ssize_t		 nr;
	MD5_CTX		 md5_ctx;
	uint8_t		 digest[MD5_DIGEST_LENGTH];
	uint32_t	 msgtime, timediff;
	seq_t		 msgseq, rnext;
	uint8_t		*rmask;
	struct msg	*msg;
	int		 p;

	if ((nr = recv(s, buf, sizeof(buf), 0)) == -1)
		return Msg_Bad;

	size = (size_t)nr;
	if (size < 16 || size > DatagramMaxSize)
		return Msg_Bad;

	MD5Init(&md5_ctx);
	MD5Update(&md5_ctx, buf + 2, 6);

	for (i = 16; i < size; i += MD5_DIGEST_LENGTH) {
		int		 j;

		MD5Update(&md5_ctx, psk, sizeof(psk));
		MD5Final(digest, &md5_ctx);

		for (j = 0; j < MD5_DIGEST_LENGTH; ++j)
			buf[i + j] ^= digest[j];
	}

	MD5Init(&md5_ctx);
	MD5Update(&md5_ctx, buf + 2, 6);
	MD5Update(&md5_ctx, psk, sizeof(psk));
	MD5Update(&md5_ctx, buf + 16, size - 16);
	MD5Final(digest, &md5_ctx);

	for (i = 0; i < 8; ++i)
		if (buf[i + 8] != digest[i])
			return Msg_Bad;

	i = 16;
	msgtime = buf[i++];
	msgtime = (msgtime << 8) + buf[i++];
	msgtime = (msgtime << 8) + buf[i++];
	msgtime = (msgtime << 8) + buf[i++];
	timediff = msgtime - curtime;

	if ((timediff & 0x80000000) != 0)
		timediff = ~timediff + 1;

	if ((timediff & 0xffffffff) > TimeDiffMax)
		return Msg_Bad;
	else if (i == size - 1)
		switch (buf[i]) {
		case Msg_Reset:
		case Msg_Reset_OK:
			return (enum Msg)buf[i];
		default:
			return Msg_Bad;
		}

	msgseq = buf[i++];
	msgseq = (msgseq << 8) + buf[i++];
	if (DIFF16(msgseq, iseq) >= MessageHistory)
		return Msg_Bad;

	if ((msg = IN_HISTORY(msgseq))->seq == msgseq)
		return Msg_Bad;

	msg->seq = msgseq;
	rnext = ((seq_t)buf[i] << 8) + buf[i + 1];
	rmask = buf + i + 2;
	i += ReportSize;
	datasize = 0;

	for (p = 0; p < PeersMax; ++p) {
		size_t		 x = buf[i++];

		msg->peer[p].opened = (x & 128) != 0;
		x = ((x & 127) << 8) + buf[i++];
		datasize += msg->peer[p].size = x / 3;
		x = (x % 3);
		msg->peer[p].closed = x > 1;
		msg->peer[p].blocked = x > 0;
	}

	if (size < i + datasize) {
		msg->seq = -1;
		return Msg_Bad;
	}

	memcpy(msg->data, buf + i, datasize);

	for (p = 0; p < MessageHistory; ++p) {
		struct msg	*const m = &ohist[p];
		const seq_t	 diff = DIFF16(rnext, m->seq);

		if (m->seq >= 0 && 0 < diff && diff <= MessageHistory)
			m->delivered = 1;
	}

	for (p = 0; p < ReportCount; ++p) {
		const uint8_t	 bit = 1 << (7 - (p & 7));
		const seq_t	 seq = (rnext + p + 1) & 0xffff;
		struct msg	*const m = OUT_HISTORY(seq);

		if ((rmask[p >> 3] & bit) && m->seq == seq)
			m->delivered = 1;
	}

	return Msg_OK;
}

int
msg_process(struct peer *const peers)
{
	struct msg	*const msg = IN_HISTORY(iseq);
	const uint8_t	*data = msg->data;
	int		 i;

	if (msg->seq != iseq)
		return 0;

	for (i = 0; i < PeersMax; data += msg->peer[i++].size) {
		size_t		 off, size;

		if (msg->peer[i].opened) {
			peers[i].recv.open = 1;
			peers[i].recv.close = 0;
			peers[i].recv.off = 0;
			peers[i].recv.size = 0;
		}

		off = peers[i].recv.off + peers[i].recv.size;
		size = sizeof(peers[i].recv.buf) - off;

		if (msg->peer[i].size < size)
			size = msg->peer[i].size;

		memcpy(peers[i].recv.buf + off, data, size);
		peers[i].recv.size += size;

		if (msg->peer[i].closed)
			peers[i].recv.close = 1;

		peers[i].dontsend = msg->peer[i].blocked;
	}

	iseq = (iseq + 1) & 0xffff;
	return 1;
}

void
msg_send(const int s, struct peer *const peers,
    const struct addrinfo *const to)
{
	struct msg	*const msg = OUT_HISTORY(oseq);
	const size_t	 sendlimit = msg_sendlimit(peers);
	size_t		 remaining = MessageDataMaxSize;
	uint8_t		*data = msg->data;
	int		 p;

	msg->delivered = 0;
	msg->seq = oseq;
	oseq = (oseq + 1) & 0xffff;

	for (p = 0; p < PeersMax; ++p) {
		size_t		 size = peers[p].send.size;
		uint8_t		*src = peers[p].send.buf;

		if (peers[p].dontsend) size = 0;
		if (sendlimit < size) size = sendlimit;
		if (remaining < size) size = remaining;

		msg->peer[p].opened = peers[p].send.open;
		msg->peer[p].closed = 0;
		msg->peer[p].blocked = peers[p].recv.size >= AlertSize;
		msg->peer[p].size = size;
		memcpy(data, src, size);
		memmove(src, src + size, peers[p].send.size -= size);
		remaining -= size;

		peers[p].send.open = 0;
		if (peers[p].send.size == 0) {
			msg->peer[p].closed = peers[p].send.close;
			peers[p].send.close = 0;
		}
	}

	msg_sendmsg(s, msg, 0, to);
}

void
msg_sendreset(const int s, const enum Msg reset_type,
    const struct addrinfo *const to)
{
	msg_sendmsg(s, NULL, reset_type, to);
}

int
msg_resendold(const int s, const struct addrinfo *const to)
{
	int		 i;
	const struct msg *const oldest = OUT_HISTORY(oseq);
	struct msg	*best = NULL;

	for (i = 0; i < MessageHistory; ++i) {
		struct msg	*const msg = &ohist[i];

		if (msg->seq >= 0 && !msg->delivered && (best == NULL
		    || DIFF16(best->lasttry, msg->lasttry) < 0x8000))
			best = msg;
	}

	if (best == NULL)
		return 0;
	else if (DIFF16(useq, best->lasttry) >= MessageHistory / 2)
		msg_sendmsg(s, best, 0, to);
	else if (oldest->seq == oseq && !oldest->delivered)
		msg_sendmsg(s, best, 0, to);
	else
		return 0;

	return 1;
}

void
msg_reset(struct peer *const peers)
{
	int		 i;

	iseq = oseq = useq = 0;
	for (i = 0; i < MessageHistory; ++i) {
		ihist[i].seq = -1;
		ohist[i].seq = -1;
	}

	for (i = 0; i < PeersMax; ++i) {
		if (peers[i].s != -1)
			close(peers[i].s);

		peers[i].free = 1;
		peers[i].dontsend = 1;
		peers[i].s = -1;
		peers[i].recv.open = 0;
		peers[i].recv.close = 0;
		peers[i].recv.off = 0;
		peers[i].recv.size = 0;
		peers[i].send.open = 0;
		peers[i].send.close = 0;
		peers[i].send.size = 0;
	}
}

struct timeval *
msg_gettimeout(struct timeval *const timeout)
{
	enum {
		Second = 1000 * 1000 * 1000,
		Exact = Second / SendFrequency,
		Enough = Exact * 3 / 4
	};
	struct timespec	 curtime, td;

	clock_gettime(CLOCK_MONOTONIC, &curtime);
	td.tv_sec = curtime.tv_sec - last_sendtime.tv_sec;
	td.tv_nsec = curtime.tv_nsec - last_sendtime.tv_nsec;

	if (td.tv_sec != 0 && td.tv_nsec < 0) {
		--td.tv_sec;
		td.tv_nsec += Second;
	} else if (td.tv_nsec < 0)
		td.tv_nsec = 0;

	if (td.tv_sec == 0 && td.tv_nsec < Enough) {
		timeout->tv_sec = 0;
		timeout->tv_usec = (Exact - td.tv_nsec) / 1000;
		return timeout;
	} else
		return NULL;
}

size_t
msg_sendlimit(const struct peer *const peers)
{
	size_t		 low = 0, high = PeerMaxSend;

	while (low < high) {
		const size_t	 mid = (low + high) >> 1;
		size_t		 size = 0;
		int		 i;

		for (i = 0; i < PeersMax; ++i) {
			const size_t	 t = peers[i].send.size;
			if (!peers[i].dontsend)
				size += t < mid ? t : mid;
		}

		if (size == MessageDataMaxSize)
			return mid;
		else if (size < MessageDataMaxSize)
			low = mid + 1;
		else
			high = mid;
	}

	return low;
}

void
msg_sendmsg(const int s, struct msg *const msg,
    const enum Msg reset_type, const struct addrinfo *const to)
{
	enum {
		Second = 1000 * 1000 * 1000,
		Exact = Second / SendFrequency,
		Near = Exact / 2,
		Far = Exact * 3 / 2
	};
	const uint32_t	 msgtime = (uint32_t)time(NULL);
	unsigned char	 buf[DatagramMaxSize + MD5_DIGEST_LENGTH];
	size_t		 i, size, datasize;
	MD5_CTX		 md5_ctx;
	uint8_t		 digest[MD5_DIGEST_LENGTH];
	struct timespec	 curtime, td;
	seq_t		 rnext;
	int		 p;

	clock_gettime(CLOCK_MONOTONIC, &curtime);
	td.tv_sec = curtime.tv_sec - last_sendtime.tv_sec;
	td.tv_nsec = curtime.tv_nsec - last_sendtime.tv_nsec;

	if (td.tv_sec != 0 && td.tv_nsec < 0) {
		--td.tv_sec;
		td.tv_nsec += Second;
	} else if (td.tv_nsec < 0)
		td.tv_nsec = 0;

	if (td.tv_sec == 0 && td.tv_nsec < Near)
		return;
	else if (td.tv_sec > 0 || td.tv_nsec > Far)
		memcpy(&last_sendtime, &curtime, sizeof(curtime));
	else if ((last_sendtime.tv_nsec += Exact) >= Second) {
		++last_sendtime.tv_sec;
		last_sendtime.tv_nsec -= Second;
	}

	arc4random_buf(buf, 8);
	size = 16;
	buf[size++] = (uint8_t)(msgtime >> 24);
	buf[size++] = (uint8_t)(msgtime >> 16);
	buf[size++] = (uint8_t)(msgtime >> 8);
	buf[size++] = (uint8_t)msgtime;

	if (msg == NULL)
		buf[size++] = (uint8_t)reset_type;
	else if (msg->seq < 0)
		return;
	else {
		msg->lasttry = useq;
		useq = (useq + 1) & 0xffff;
		buf[size++] = (uint8_t)(msg->seq >> 8);
		buf[size++] = (uint8_t)msg->seq;

		for (rnext = iseq; ; rnext = (rnext + 1) & 0xffff) {
			const struct msg *const m = IN_HISTORY(rnext);

			if (m->seq != rnext || !m->delivered)
				break;
		}

		buf[size++] = (uint8_t)(rnext >> 8);
		buf[size++] = (uint8_t)rnext;
		memset(buf + size, 0, ReportSize - 2);

		for (p = 0; p < ReportCount; ++p) {
			const uint8_t	 bit = 1 << (7 - (p & 7));
			const seq_t	 seq = rnext + p + 1;
			const struct msg *const m = IN_HISTORY(seq);

			if (m->seq == seq && m->delivered)
				buf[size + (p >> 3)] |= bit;
		}

		size += ReportSize - 2;
		datasize = 0;

		for (p = 0; p < PeersMax; ++p) {
			size_t		 x;

			datasize += x = msg->peer[p].size;
			x *= 3;
			x += msg->peer[p].closed ? 2 :
			    msg->peer[p].blocked ? 1 : 0;
			buf[size++] = (uint8_t)(x >> 8);
			buf[size++] = (uint8_t)x;

			if (msg->peer[p].opened)
				buf[size - 2] |= 128;
		}

		memcpy(buf + size, msg->data, datasize);
		size += datasize;

		if (size < DatagramMaxSize) {
			size_t		 r = DatagramMaxSize - size;

			r = r < 15 ? r : 15;
			r = (ssize_t)arc4random_uniform((uint32_t)r);
			memset(buf + size, 0, r);
			size += r;
		}
	}

	MD5Init(&md5_ctx);
	MD5Update(&md5_ctx, buf + 2, 6);
	MD5Update(&md5_ctx, psk, sizeof(psk));
	MD5Update(&md5_ctx, buf + 16, size - 16);
	MD5Final(digest, &md5_ctx);

	for (i = 0; i < 8; ++i)
		buf[i + 8] = digest[i];

	MD5Init(&md5_ctx);
	MD5Update(&md5_ctx, buf + 2, 6);

	for (i = 16; i < size; i += MD5_DIGEST_LENGTH) {
		int		 j;

		MD5Update(&md5_ctx, psk, sizeof(psk));
		MD5Final(digest, &md5_ctx);

		for (j = 0; j < MD5_DIGEST_LENGTH; ++j)
			buf[i + j] ^= digest[j];
	}

	sendto(s, buf, size, 0, to->ai_addr, to->ai_addrlen);
}
