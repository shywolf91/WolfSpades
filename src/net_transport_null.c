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

#include "net_transport.h"

int net_transport_open(void) {
	return 1;
}

void net_transport_close(void) { }

int net_transport_connect(const char* host, int port, unsigned int connect_data) {
	(void)host;
	(void)port;
	(void)connect_data;
	return 0;
}

void net_transport_disconnect(void) { }

int net_transport_poll(net_transport_event* out, int timeout_ms) {
	(void)timeout_ms;
	if(out) {
		out->type = NET_TRANSPORT_EVENT_NONE;
		out->data = NULL;
		out->length = 0;
		out->event_data = 0;
	}
	return 0;
}

void net_transport_recv_done(net_transport_event* ev) {
	(void)ev;
}

int net_transport_send(const void* data, size_t length) {
	(void)data;
	(void)length;
	return 0;
}

unsigned int net_transport_rtt(void) {
	return 0;
}
