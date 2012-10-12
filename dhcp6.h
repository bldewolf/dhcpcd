/* 
 * dhcpcd - DHCP client daemon
 * Copyright (c) 2006-2012 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef DHCP6_H
#define DHCP6_H

#include "dhcpcd.h"

/* UDP port numbers for DHCP */
#define DHCP6_CLIENT_PORT	546
#define DHCP6_SERVER_PORT	547

/* DHCP message type */
#define DHCP6_SOLICIT		1
#define DHCP6_ADVERTISE		2
#define DHCP6_REQUEST		3
#define DHCP6_CONFIRM		4
#define DHCP6_RENEW		5
#define DHCP6_REBIND		6
#define DHCP6_REPLY		7
#define DHCP6_RELEASE		8
#define DHCP6_DECLINE		9
#define DHCP6_RECONFIGURE	10
#define DHCP6_INFORMATION_REQ	11
#define DHCP6_RELAY_FLOW	12
#define DHCP6_RELAY_REPL	13

#define D6_OPTION_CLIENTID		1
#define D6_OPTION_SERVERID		2
#define D6_OPTION_ORO			6
#define D6_OPTION_IA_ADDR		5
#define D6_OPTION_PREFERENCE		7
#define D6_OPTION_ELAPSED		8
#define D6_OPTION_RAPID_COMMIT		9
#define D6_OPTION_UNICAST		12
#define D6_OPTION_STATUS_CODE		13
#define D6_OPTION_VENDOR		16
#define D6_OPTION_SIP_SERVERS_NAME	21
#define D6_OPTION_SIP_SERVERS_ADDRESS	22
#define D6_OPTION_DNS_SERVERS		23
#define D6_OPTION_DOMAIN_LIST		24
#define D6_OPTION_NIS_SERVERS		27
#define D6_OPTION_NISP_SERVERS		28
#define D6_OPTION_NIS_DOMAIN_NAME	29
#define D6_OPTION_NISP_DOMAIN_NAME	30
#define D6_OPTION_SNTP_SERVERS		31
#define D6_OPTION_INFO_REFRESH_TIME	32
#define D6_OPTION_BCMS_SERVER_D		33
#define D6_OPTION_BCMS_SERVER_A		34

#include "dhcp.h"
extern const struct dhcp_opt const dhcp6_opts[];

struct dhcp6_message {
	uint8_t type;
	uint8_t xid[3];
	/* followed by options */
} _packed;

struct dhcp6_option {
	uint16_t code;
	uint16_t len;
	/* followed by data */
} _packed;

enum DH6S {
	DH6S_INIT,
	DH6S_DISCOVER,
	DH6S_REQUEST,
	DH6S_BOUND,
	DH6S_RENEW,
	DH6S_REBIND,
	DH6S_REBOOT,
	DH6S_INFORM,
	DH6S_RENEW_REQUESTED,
	DH6S_PROBE
};

struct dhcp6_state {
	enum DH6S state;
	time_t start_uptime;
	int interval;
	struct dhcp6_message *send;
	size_t send_len;
	struct dhcp6_message *new;
	size_t new_len;
	struct dhcp6_message *old;
	size_t old_len;
};

#define D6_STATE(ifp) ((struct dhcp6_state *)(ifp)->if_data[IF_DATA_DHCP6])
#define D6_STATE_RUNNING(ifp) (D6_STATE((ifp)) && D6_STATE((ifp))->new)
#define D6_FIRST_OPTION(m)						       \
    ((struct dhcp6_option *)						       \
        ((uint8_t *)(m) + sizeof(struct dhcp6_message)))
#define D6_NEXT_OPTION(o)						       \
    ((struct dhcp6_option *)						       \
        (((uint8_t *)o) + sizeof(struct dhcp6_option) + ntohs((o)->len)))
#define D6_OPTION_DATA(o)						       \
    ((uint8_t *)(o) + sizeof(struct dhcp6_option))
#define D6_CFIRST_OPTION(m)						       \
    ((const struct dhcp6_option *)					       \
        ((const uint8_t *)(m) + sizeof(struct dhcp6_message)))
#define D6_CNEXT_OPTION(o)						       \
    ((const struct dhcp6_option *)					       \
        (((const uint8_t *)o) + sizeof(struct dhcp6_option) + ntohs((o)->len)))
#define D6_COPTION_DATA(o)						       \
    ((const uint8_t *)(o) + sizeof(struct dhcp6_option))

void dhcp6_printoptions(void);
int dhcp6_start(struct interface *, int);
ssize_t dhcp6_env(char **, const char *, const struct interface *,
    const struct dhcp6_message *, ssize_t);
void dhcp6_free(struct interface *);
void dhcp6_drop(struct interface *);

#endif