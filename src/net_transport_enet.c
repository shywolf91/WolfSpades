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

#include <enet/enet.h>
#include <stdint.h>
#include <string.h>

#include "net_transport.h"

static ENetHost* host = NULL;
static ENetPeer* peer = NULL;
static int enet_ready = 0;
static ENetPacket* pending_packet = NULL;

int net_transport_open(void) {
	if(!enet_ready) {
		if(enet_initialize() != 0)
			return 0;
		enet_ready = 1;
	}

	if(host) {
		enet_host_destroy(host);
		host = NULL;
		peer = NULL;
	}

	host = enet_host_create(NULL, 1, 1, 0, 0);
	if(!host)
		return 0;
	enet_host_compress_with_range_coder(host);
	return 1;
}

void net_transport_close(void) {
	if(pending_packet) {
		enet_packet_destroy(pending_packet);
		pending_packet = NULL;
	}
	if(host) {
		enet_host_destroy(host);
		host = NULL;
	}
	peer = NULL;
	if(enet_ready) {
		enet_deinitialize();
		enet_ready = 0;
	}
}

int net_transport_connect(const char* host_name, int port, unsigned int connect_data) {
	ENetAddress address;

	if(!host && !net_transport_open())
		return 0;

	enet_address_set_host(&address, host_name);
	address.port = (enet_uint16)port;
	peer = enet_host_connect(host, &address, 1, connect_data);
	return peer != NULL;
}

void net_transport_disconnect(void) {
	if(!host || !peer)
		return;

	enet_peer_disconnect(peer, 0);

	ENetEvent event;
	while(enet_host_service(host, &event, 3000) > 0) {
		switch(event.type) {
			case ENET_EVENT_TYPE_RECEIVE: enet_packet_destroy(event.packet); break;
			case ENET_EVENT_TYPE_DISCONNECT:
				enet_host_destroy(host);
				host = NULL;
				peer = NULL;
				return;
			default: break;
		}
	}

	enet_peer_reset(peer);
	enet_host_destroy(host);
	host = NULL;
	peer = NULL;
}

int net_transport_poll(net_transport_event* out, int timeout_ms) {
	ENetEvent event;

	if(!out || !host)
		return 0;

	out->type = NET_TRANSPORT_EVENT_NONE;
	out->data = NULL;
	out->length = 0;
	out->event_data = 0;

	if(enet_host_service(host, &event, (enet_uint32)timeout_ms) <= 0)
		return 0;

	switch(event.type) {
		case ENET_EVENT_TYPE_CONNECT:
			out->type = NET_TRANSPORT_EVENT_CONNECT;
			return 1;
		case ENET_EVENT_TYPE_RECEIVE:
			if(pending_packet)
				enet_packet_destroy(pending_packet);
			pending_packet = event.packet;
			out->type = NET_TRANSPORT_EVENT_RECEIVE;
			out->data = event.packet->data;
			out->length = event.packet->dataLength;
			return 1;
		case ENET_EVENT_TYPE_DISCONNECT:
			out->type = NET_TRANSPORT_EVENT_DISCONNECT;
			out->event_data = event.data;
			if(event.peer)
				event.peer->data = NULL;
			peer = NULL;
			return 1;
		default: return 0;
	}
}

void net_transport_recv_done(net_transport_event* ev) {
	(void)ev;
	if(pending_packet) {
		enet_packet_destroy(pending_packet);
		pending_packet = NULL;
	}
	if(ev) {
		ev->data = NULL;
		ev->length = 0;
	}
}

int net_transport_send(const void* data, size_t length) {
	if(!peer || !data || length == 0)
		return 0;
	ENetPacket* packet = enet_packet_create(data, length, ENET_PACKET_FLAG_RELIABLE);
	if(!packet)
		return 0;
	return enet_peer_send(peer, 0, packet) == 0;
}

unsigned int net_transport_rtt(void) {
	return peer ? peer->roundTripTime : 0;
}
