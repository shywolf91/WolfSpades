/*
	Copyright (c) 2017-2020 ByteBit

	This file is part of BetterSpades.

	BetterSpades is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	BetterSpades is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with BetterSpades.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef NET_TRANSPORT_H
#define NET_TRANSPORT_H

#include <stddef.h>

typedef enum {
	NET_TRANSPORT_EVENT_NONE = 0,
	NET_TRANSPORT_EVENT_CONNECT,
	NET_TRANSPORT_EVENT_RECEIVE,
	NET_TRANSPORT_EVENT_DISCONNECT,
} net_transport_event_type;

typedef struct {
	net_transport_event_type type;
	unsigned char* data;
	size_t length;
	unsigned int event_data;
} net_transport_event;

/* Lifecycle */
int net_transport_open(void);
void net_transport_close(void);

/* Connection (no-op on null backend) */
int net_transport_connect(const char* host, int port, unsigned int connect_data);
void net_transport_disconnect(void);

/*
 * Poll for one event. timeout_ms == 0 is non-blocking (frame path).
 * Returns 1 if an event was written to *out, 0 if none, <0 on error.
 * For RECEIVE events, call net_transport_recv_done() when finished.
 */
int net_transport_poll(net_transport_event* out, int timeout_ms);
void net_transport_recv_done(net_transport_event* ev);

int net_transport_send(const void* data, size_t length);
unsigned int net_transport_rtt(void);

#endif
