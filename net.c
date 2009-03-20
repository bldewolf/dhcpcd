/* 
 * dhcpcd - DHCP client daemon
 * Copyright 2006-2009 Roy Marples <roy@marples.name>
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

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#define __FAVOR_BSD /* Nasty glibc hack so we can use BSD semantics for UDP */
#include <netinet/udp.h>
#undef __FAVOR_BSD
#ifdef SIOCGIFMEDIA
# include <net/if_media.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "config.h"
#include "common.h"
#include "dhcp.h"
#include "if-options.h"
#include "net.h"
#include "signals.h"

static char hwaddr_buffer[(HWADDR_LEN * 3) + 1];

int
inet_ntocidr(struct in_addr address)
{
	int cidr = 0;
	uint32_t mask = htonl(address.s_addr);

	while (mask) {
		cidr++;
		mask <<= 1;
	}
	return cidr;
}

int
inet_cidrtoaddr(int cidr, struct in_addr *addr)
{
	int ocets;

	if (cidr < 1 || cidr > 32) {
		errno = EINVAL;
		return -1;
	}
	ocets = (cidr + 7) / 8;

	addr->s_addr = 0;
	if (ocets > 0) {
		memset(&addr->s_addr, 255, (size_t)ocets - 1);
	
	memset((unsigned char *)&addr->s_addr + (ocets - 1),
		    (256 - (1 << (32 - cidr) % 8)), 1);
	}

	return 0;
}

uint32_t
get_netmask(uint32_t addr)
{
	uint32_t dst;

	if (addr == 0)
		return 0;

	dst = htonl(addr);
	if (IN_CLASSA(dst))
		return ntohl(IN_CLASSA_NET);
	if (IN_CLASSB(dst))
		return ntohl(IN_CLASSB_NET);
	if (IN_CLASSC(dst))
		return ntohl(IN_CLASSC_NET);

	return 0;
}

char *
hwaddr_ntoa(const unsigned char *hwaddr, size_t hwlen)
{
	char *p = hwaddr_buffer;
	size_t i;

	for (i = 0; i < hwlen && i < HWADDR_LEN; i++) {
		if (i > 0)
			*p ++= ':';
		p += snprintf(p, 3, "%.2x", hwaddr[i]);
	}

	*p ++= '\0';

	return hwaddr_buffer;
}

size_t
hwaddr_aton(unsigned char *buffer, const char *addr)
{
	char c[3];
	const char *p = addr;
	unsigned char *bp = buffer;
	size_t len = 0;

	c[2] = '\0';
	while (*p) {
		c[0] = *p++;
		c[1] = *p++;
		/* Ensure that digits are hex */
		if (isxdigit((unsigned char)c[0]) == 0 ||
		    isxdigit((unsigned char)c[1]) == 0)
		{
			errno = EINVAL;
			return 0;
		}
		/* We should have at least two entries 00:01 */
		if (len == 0 && *p == '\0') {
			errno = EINVAL;
			return 0;
		}
		/* Ensure that next data is EOL or a seperator with data */
		if (!(*p == '\0' || (*p == ':' && *(p + 1) != '\0'))) {
			errno = EINVAL;
			return 0;
		}
		if (*p)
			p++;
		if (bp)
			*bp++ = (unsigned char)strtol(c, NULL, 16);
		len++;
	}
	return len;
}

struct interface *
init_interface(const char *ifname)
{
	int s;
	struct ifreq ifr;
	struct interface *iface = NULL;

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return NULL;

	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == -1)
		goto eexit;

	iface = xzalloc(sizeof(*iface));
	strlcpy(iface->name, ifname, sizeof(iface->name));
	iface->flags = ifr.ifr_flags;
	/* We reserve the 100 range for virtual interfaces, if and when
	 * we can work them out. */
	iface->metric = 200 + if_nametoindex(iface->name);
	if (getifssid(ifname, iface->ssid) != -1) {
		iface->wireless = 1;
		iface->metric += 100;
	}

#ifdef SIOCGIFHWADDR
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFHWADDR, &ifr) == -1)
		goto eexit;

	switch (ifr.ifr_hwaddr.sa_family) {
	case ARPHRD_ETHER:
	case ARPHRD_IEEE802:
		iface->hwlen = ETHER_ADDR_LEN;
		break;
	case ARPHRD_IEEE1394:
		iface->hwlen = EUI64_ADDR_LEN;
	case ARPHRD_INFINIBAND:
		iface->hwlen = INFINIBAND_ADDR_LEN;
		break;
	default:
		iface->hwlen = 0;
	}
	iface->family = ifr.ifr_hwaddr.sa_family;
	if (iface->hwlen != 0)
		memcpy(iface->hwaddr, ifr.ifr_hwaddr.sa_data, iface->hwlen);
#endif

	if (ioctl(s, SIOCGIFMTU, &ifr) == -1)
		goto eexit;
	/* Ensure that the MTU is big enough for DHCP */
	if (ifr.ifr_mtu < MTU_MIN) {
		ifr.ifr_mtu = MTU_MIN;
		strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
		if (ioctl(s, SIOCSIFMTU, &ifr) == -1)
			goto eexit;
	}

	if (up_interface(ifname) != 0)
		goto eexit;
	snprintf(iface->leasefile, sizeof(iface->leasefile),
	    LEASEFILE, ifname);
	/* 0 is a valid fd, so init to -1 */
	iface->raw_fd = -1;
	iface->udp_fd = -1;
	iface->arp_fd = -1;
	close(s);
	return iface;

eexit:
	free(iface);
	close(s);
	return NULL;
}

void
free_interface(struct interface *iface)
{
	if (!iface)
		return;
	if (iface->state) {
		free_options(iface->state->options);
		free(iface->state->old);
		free(iface->state->new);
		free(iface->state->offer);
		free(iface->state);
	}
	free(iface->clientid);
	free(iface);
}

int
do_interface(const char *ifname,
    void (*do_link)(struct interface **, int, char * const *, struct ifreq *),
    struct interface **ifs, int argc, char * const *argv,
    struct in_addr *addr, struct in_addr *net, struct in_addr *dst, int act)
{
	int s;
	struct ifconf ifc;
	int retval = 0, found = 0;
	int len = 10 * sizeof(struct ifreq);
	int lastlen = 0;
	char *p, *e;
	in_addr_t address, netmask;
	struct ifreq *ifr;
	struct sockaddr_in *sin;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return -1;

	/* Not all implementations return the needed buffer size for
	 * SIOGIFCONF so we loop like so for all until it works */
	memset(&ifc, 0, sizeof(ifc));
	for (;;) {
		ifc.ifc_len = len;
		ifc.ifc_buf = xmalloc((size_t)len);
		if (ioctl(s, SIOCGIFCONF, &ifc) == -1) {
			if (errno != EINVAL || lastlen != 0) {
				close(s);
				free(ifc.ifc_buf);	
				return -1;
			}
		} else {
			if (ifc.ifc_len == lastlen)
				break;
			lastlen = ifc.ifc_len;
		}

		free(ifc.ifc_buf);
		ifc.ifc_buf = NULL;
		len *= 2;
	}

	e = (char *)ifc.ifc_buf + ifc.ifc_len;
	for (p = ifc.ifc_buf; p < e;) {
		ifr = (struct ifreq *)(void *)p;

#ifndef __linux__
		if (ifr->ifr_addr.sa_len > sizeof(ifr->ifr_ifru))
			p += offsetof(struct ifreq, ifr_ifru) +
			    ifr->ifr_addr.sa_len;
		else
#endif
			p += sizeof(*ifr);

		if (ifname && strcmp(ifname, ifr->ifr_name) != 0)
			continue;

		found = 1;

		/* Interface discovery for BSD's */
		if (act == 2 && do_link) {
			do_link(ifs, argc, argv, ifr);
			continue;
		}

		if (ifr->ifr_addr.sa_family == AF_INET && addr)	{
			sin = (struct sockaddr_in *)(void *)&ifr->ifr_addr;
			address = sin->sin_addr.s_addr;
			/* Some platforms only partially fill the bits
			 * set by the netmask, so we need to zero it now. */
			sin->sin_addr.s_addr = 0;
			if (ioctl(s, SIOCGIFNETMASK, ifr) == -1)
				continue;
			netmask = sin->sin_addr.s_addr;
			if (act == 1) {
				if (dst) {
					sin = (struct sockaddr_in *)
						(void *)&ifr->ifr_dstaddr;
					if (ioctl(s, SIOCGIFDSTADDR, ifr)
					    == -1)
						dst->s_addr = INADDR_ANY;
					else
						dst->s_addr =
						    sin->sin_addr.s_addr;
				}
				addr->s_addr = address;
				net->s_addr = netmask;
				retval = 1;
				break;
			} else {
				if (address == addr->s_addr &&
				    (!net || netmask == net->s_addr))
				{
					retval = 1;
					break;
				}
			}
		}

	}

	if (!found)
		errno = ENXIO;
	close(s);
	free(ifc.ifc_buf);
	return retval;
}

int
up_interface(const char *ifname)
{
	int s;
	struct ifreq ifr;
	int retval = -1;
#ifdef __linux__
	char *p;
#endif

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
#ifdef __linux__
	/* We can only bring the real interface up */
	if ((p = strchr(ifr.ifr_name, ':')))
		*p = '\0';
#endif
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		if ((ifr.ifr_flags & IFF_UP))
			retval = 0;
		else {
			ifr.ifr_flags |= IFF_UP;
			if (ioctl(s, SIOCSIFFLAGS, &ifr) == 0)
				retval = 0;
		}
	}
	close(s);
	return retval;
}

int
carrier_status(const char *ifname)
{
	int s;
	struct ifreq ifr;
	int retval = -1;
#ifdef SIOCGIFMEDIA
	struct ifmediareq ifmr;
#endif
#ifdef __linux__
	char *p;
#endif

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
#ifdef __linux__
	/* We can only test the real interface up */
	if ((p = strchr(ifr.ifr_name, ':')))
		*p = '\0';
#endif
	if ((retval = ioctl(s, SIOCGIFFLAGS, &ifr)) == 0) {
		if (ifr.ifr_flags & IFF_UP && ifr.ifr_flags & IFF_RUNNING)
			retval = 1;
		else
			retval = 0;
	}

#ifdef SIOCGIFMEDIA
	if (retval == 1) {
		memset(&ifmr, 0, sizeof(ifmr));
		strlcpy(ifmr.ifm_name, ifr.ifr_name, sizeof(ifmr.ifm_name));
		retval = -1;
		if (ioctl(s, SIOCGIFMEDIA, &ifmr) != -1 &&
		    ifmr.ifm_status & IFM_AVALID)
			retval = (ifmr.ifm_status & IFM_ACTIVE) ? 1 : 0;
	}
#endif
	close(s);
	return retval;
}


int
do_mtu(const char *ifname, short int mtu)
{
	struct ifreq ifr;
	int r;
	int s;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		return -1;

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	ifr.ifr_mtu = mtu;
	r = ioctl(s, mtu ? SIOCSIFMTU : SIOCGIFMTU, &ifr);
	close(s);
	if (r == -1)
		return -1;
	return ifr.ifr_mtu;
}

void
free_routes(struct rt *routes)
{
	struct rt *r;

	while (routes) {
		r = routes->next;
		free(routes);
		routes = r;
	}
}

int
open_udp_socket(struct interface *iface)
{
	int s;
	struct sockaddr_in sin;
	int n;
#ifdef SO_BINDTODEVICE
	struct ifreq ifr;
	char *p;
#endif

	if ((s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
		return -1;

	n = 1;
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n)) == -1)
		goto eexit;
#ifdef SO_BINDTODEVICE
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, iface->name, sizeof(ifr.ifr_name));
	/* We can only bind to the real device */
	p = strchr(ifr.ifr_name, ':');
	if (p)
		*p = '\0';
	if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, &ifr,
		sizeof(ifr)) == -1)
		goto eexit;
#endif
	/* As we don't use this socket for receiving, set the
	 * receive buffer to 1 */
	n = 1;
	if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n)) == -1)
		goto eexit;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(DHCP_CLIENT_PORT);
	sin.sin_addr.s_addr = iface->addr.s_addr;
	if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) == -1)
		goto eexit;

	iface->udp_fd = s;
	set_cloexec(s);
	return 0;

eexit:
	close(s);
	return -1;
}

ssize_t
send_packet(const struct interface *iface, struct in_addr to,
    const uint8_t *data, ssize_t len)
{
	struct sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = to.s_addr;
	sin.sin_port = htons(DHCP_SERVER_PORT);
	return sendto(iface->udp_fd, data, len, 0,
	    (struct sockaddr *)&sin, sizeof(sin));
}

struct udp_dhcp_packet
{
	struct ip ip;
	struct udphdr udp;
	struct dhcp_message dhcp;
};
const size_t udp_dhcp_len = sizeof(struct udp_dhcp_packet);

static uint16_t
checksum(const void *data, uint16_t len)
{
	const uint8_t *addr = data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += addr[0] * 256 + addr[1];
		addr += 2;
		len -= 2;
	}

	if (len == 1)
		sum += *addr * 256;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	sum = htons(sum);

	return ~sum;
}

ssize_t
make_udp_packet(uint8_t **packet, const uint8_t *data, size_t length,
    struct in_addr source, struct in_addr dest)
{
	struct udp_dhcp_packet *udpp;
	struct ip *ip;
	struct udphdr *udp;

	udpp = xzalloc(sizeof(*udpp));
	ip = &udpp->ip;
	udp = &udpp->udp;

	/* OK, this is important :)
	 * We copy the data to our packet and then create a small part of the
	 * ip structure and an invalid ip_len (basically udp length).
	 * We then fill the udp structure and put the checksum
	 * of the whole packet into the udp checksum.
	 * Finally we complete the ip structure and ip checksum.
	 * If we don't do the ordering like so then the udp checksum will be
	 * broken, so find another way of doing it! */

	memcpy(&udpp->dhcp, data, length);

	ip->ip_p = IPPROTO_UDP;
	ip->ip_src.s_addr = source.s_addr;
	if (dest.s_addr == 0)
		ip->ip_dst.s_addr = INADDR_BROADCAST;
	else
		ip->ip_dst.s_addr = dest.s_addr;

	udp->uh_sport = htons(DHCP_CLIENT_PORT);
	udp->uh_dport = htons(DHCP_SERVER_PORT);
	udp->uh_ulen = htons(sizeof(*udp) + length);
	ip->ip_len = udp->uh_ulen;
	udp->uh_sum = checksum(udpp, sizeof(*udpp));

	ip->ip_v = IPVERSION;
	ip->ip_hl = 5;
	ip->ip_id = 0;
	ip->ip_tos = IPTOS_LOWDELAY;
	ip->ip_len = htons (sizeof(*ip) + sizeof(*udp) + length);
	ip->ip_id = 0;
	ip->ip_off = htons(IP_DF); /* Don't fragment */
	ip->ip_ttl = IPDEFTTL;

	ip->ip_sum = checksum(ip, sizeof(*ip));

	*packet = (uint8_t *)udpp;
	return sizeof(*ip) + sizeof(*udp) + length;
}

ssize_t
get_udp_data(const uint8_t **data, const uint8_t *udp)
{
	struct udp_dhcp_packet packet;

	memcpy(&packet, udp, sizeof(packet));
	*data = udp + offsetof(struct udp_dhcp_packet, dhcp);
	return ntohs(packet.ip.ip_len) -
	    sizeof(packet.ip) -
	    sizeof(packet.udp);
}

int
valid_udp_packet(const uint8_t *data, size_t data_len, struct in_addr *from)
{
	struct udp_dhcp_packet packet;
	uint16_t bytes, udpsum;

	if (data_len < sizeof(packet.ip)) {
		if (from)
			from->s_addr = INADDR_ANY;
		errno = EINVAL;
		return -1;
	}
	memcpy(&packet, data, MIN(data_len, sizeof(packet)));
	if (from)
		from->s_addr = packet.ip.ip_src.s_addr;
	if (data_len > sizeof(packet)) {
		errno = EINVAL;
		return -1;
	}
	if (checksum(&packet.ip, sizeof(packet.ip)) != 0) {
		errno = EINVAL;
		return -1;
	}

	bytes = ntohs(packet.ip.ip_len);
	if (data_len < bytes) {
		errno = EINVAL;
		return -1;
	}
	udpsum = packet.udp.uh_sum;
	packet.udp.uh_sum = 0;
	packet.ip.ip_hl = 0;
	packet.ip.ip_v = 0;
	packet.ip.ip_tos = 0;
	packet.ip.ip_len = packet.udp.uh_ulen;
	packet.ip.ip_id = 0;
	packet.ip.ip_off = 0;
	packet.ip.ip_ttl = 0;
	packet.ip.ip_sum = 0;
	if (udpsum && checksum(&packet, bytes) != udpsum) {
		errno = EINVAL;
		return -1;
	}

	return 0;
}
