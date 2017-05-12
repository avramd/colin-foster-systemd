/***
  This file is part of systemd.

  Copyright (C) 2017 Intel Corporation. All rights reserved.

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sd-radv.h"

#include "macro.h"
#include "alloc-util.h"
#include "fd-util.h"
#include "icmp6-util.h"
#include "in-addr-util.h"
#include "radv-internal.h"
#include "socket-util.h"
#include "string-util.h"
#include "util.h"

_public_ int sd_radv_new(sd_radv **ret) {
        _cleanup_(sd_radv_unrefp) sd_radv *ra = NULL;

        assert_return(ret, -EINVAL);

        ra = new0(sd_radv, 1);
        if (!ra)
                return -ENOMEM;

        ra->n_ref = 1;

        LIST_HEAD_INIT(ra->prefixes);

        *ret = ra;
        ra = NULL;

        return 0;
}

_public_ int sd_radv_attach_event(sd_radv *ra, sd_event *event, int64_t priority) {
        int r;

        assert_return(ra, -EINVAL);
        assert_return(!ra->event, -EBUSY);

        if (event)
                ra->event = sd_event_ref(event);
        else {
                r = sd_event_default(&ra->event);
                if (r < 0)
                        return 0;
        }

        ra->event_priority = priority;

        return 0;
}

_public_ int sd_radv_detach_event(sd_radv *ra) {

        assert_return(ra, -EINVAL);

        ra->event = sd_event_unref(ra->event);
        return 0;
}

_public_ sd_event *sd_radv_get_event(sd_radv *ra) {
        assert_return(ra, NULL);

        return ra->event;
}

_public_ sd_radv *sd_radv_ref(sd_radv *ra) {
        if (!ra)
                return NULL;

        assert(ra->n_ref > 0);
        ra->n_ref++;

        return ra;
}

_public_ sd_radv *sd_radv_unref(sd_radv *ra) {
        if (!ra)
                return NULL;

        assert(ra->n_ref > 0);
        ra->n_ref--;

        if (ra->n_ref > 0)
                return NULL;

        while (ra->prefixes) {
                sd_radv_prefix *p = ra->prefixes;

                LIST_REMOVE(prefix, ra->prefixes, p);
                sd_radv_prefix_unref(p);
        }

        sd_radv_detach_event(ra);
        return mfree(ra);
}

_public_ int sd_radv_stop(sd_radv *ra) {
        assert_return(ra, -EINVAL);

        log_radv("Stopping IPv6 Router Advertisement daemon");

        ra->state = SD_RADV_STATE_IDLE;

        return 0;
}

_public_ int sd_radv_start(sd_radv *ra) {
        assert_return(ra, -EINVAL);
        assert_return(ra->event, -EINVAL);
        assert_return(ra->ifindex > 0, -EINVAL);

        if (ra->state != SD_RADV_STATE_IDLE)
                return 0;

        ra->state = SD_RADV_STATE_ADVERTISING;

        log_radv("Started IPv6 Router Advertisement daemon");

        return 0;
}

_public_ int sd_radv_set_ifindex(sd_radv *ra, int ifindex) {
        assert_return(ra, -EINVAL);
        assert_return(ifindex >= -1, -EINVAL);

        if (ra->state != SD_RADV_STATE_IDLE)
                return -EBUSY;

        ra->ifindex = ifindex;

        return 0;
}

_public_ int sd_radv_set_mac(sd_radv *ra, const struct ether_addr *mac_addr) {
        assert_return(ra, -EINVAL);

        if (ra->state != SD_RADV_STATE_IDLE)
                return -EBUSY;

        if (mac_addr)
                ra->mac_addr = *mac_addr;
        else
                zero(ra->mac_addr);

        return 0;
}

_public_ int sd_radv_set_mtu(sd_radv *ra, uint32_t mtu) {
        assert_return(ra, -EINVAL);
        assert_return(mtu >= 1280, -EINVAL);

        if (ra->state != SD_RADV_STATE_IDLE)
                return -EBUSY;

        ra->mtu = mtu;

        return 0;
}

_public_ int sd_radv_set_hop_limit(sd_radv *ra, uint8_t hop_limit) {
        assert_return(ra, -EINVAL);

        if (ra->state != SD_RADV_STATE_IDLE)
                return -EBUSY;

        ra->hop_limit = hop_limit;

        return 0;
}

_public_ int sd_radv_set_router_lifetime(sd_radv *ra, uint32_t router_lifetime) {
        assert_return(ra, -EINVAL);

        if (ra->state != SD_RADV_STATE_IDLE)
                return -EBUSY;

        /* RFC 4191, Section 2.2, "...If the Router Lifetime is zero, the
           preference value MUST be set to (00) by the sender..." */
        if (router_lifetime == 0 &&
            (ra->flags & (0x3 << 3)) != (SD_NDISC_PREFERENCE_MEDIUM << 3))
                return -ETIME;

        ra->lifetime = router_lifetime;

        return 0;
}

_public_ int sd_radv_set_managed_information(sd_radv *ra, int managed) {
        assert_return(ra, -EINVAL);

        if (ra->state != SD_RADV_STATE_IDLE)
                return -EBUSY;

        SET_FLAG(ra->flags, ND_RA_FLAG_MANAGED, managed);

        return 0;
}

_public_ int sd_radv_set_other_information(sd_radv *ra, int other) {
        assert_return(ra, -EINVAL);

        if (ra->state != SD_RADV_STATE_IDLE)
                return -EBUSY;

        SET_FLAG(ra->flags, ND_RA_FLAG_OTHER, other);

        return 0;
}

_public_ int sd_radv_set_preference(sd_radv *ra, unsigned preference) {
        int r = 0;

        assert_return(ra, -EINVAL);
        assert_return(IN_SET(preference,
                             SD_NDISC_PREFERENCE_LOW,
                             SD_NDISC_PREFERENCE_MEDIUM,
                             SD_NDISC_PREFERENCE_HIGH), -EINVAL);

        ra->flags = (ra->flags & ~(0x3 << 3)) | (preference << 3);

        return r;
}

_public_ int sd_radv_add_prefix(sd_radv *ra, sd_radv_prefix *p) {
        sd_radv_prefix *cur;
        _cleanup_free_ char *addr_p = NULL;

        assert_return(ra, -EINVAL);

        if (!p)
                return -EINVAL;

        LIST_FOREACH(prefix, cur, ra->prefixes) {
                int r;

                r = in_addr_prefix_intersect(AF_INET6,
                                             (union in_addr_union*) &cur->opt.in6_addr,
                                             cur->opt.prefixlen,
                                             (union in_addr_union*) &p->opt.in6_addr,
                                             p->opt.prefixlen);
                if (r > 0) {
                        _cleanup_free_ char *addr_cur = NULL;

                        (void) in_addr_to_string(AF_INET6,
                                                 (union in_addr_union*) &cur->opt.in6_addr,
                                                 &addr_cur);
                        (void) in_addr_to_string(AF_INET6,
                                                 (union in_addr_union*) &p->opt.in6_addr,
                                                 &addr_p);

                        log_radv("IPv6 prefix %s/%u already configured, ignoring %s/%u",
                                 addr_cur, cur->opt.prefixlen,
                                 addr_p, p->opt.prefixlen);

                        return -EEXIST;
                }
        }

        p = sd_radv_prefix_ref(p);

        LIST_APPEND(prefix, ra->prefixes, p);

        ra->n_prefixes++;

        (void) in_addr_to_string(AF_INET6, (union in_addr_union*) &p->opt.in6_addr, &addr_p);
        log_radv("Added prefix %s/%d", addr_p, p->opt.prefixlen);

        return 0;
}

_public_ int sd_radv_prefix_new(sd_radv_prefix **ret) {
        _cleanup_(sd_radv_prefix_unrefp) sd_radv_prefix *p = NULL;

        assert_return(ret, -EINVAL);

        p = new0(sd_radv_prefix, 1);
        if (!p)
                return -ENOMEM;

        p->n_ref = 1;

        p->opt.type = ND_OPT_PREFIX_INFORMATION;
        p->opt.length = (sizeof(p->opt) - 1) /8 + 1;

        p->opt.prefixlen = 64;

        /* RFC 4861, Section 6.2.1 */
        SET_FLAG(p->opt.flags, ND_OPT_PI_FLAG_ONLINK, true);
        SET_FLAG(p->opt.flags, ND_OPT_PI_FLAG_AUTO, true);
        p->opt.preferred_lifetime = htobe32(604800);
        p->opt.valid_lifetime = htobe32(2592000);

        LIST_INIT(prefix, p);

        *ret = p;
        p = NULL;

        return 0;
}

_public_ sd_radv_prefix *sd_radv_prefix_ref(sd_radv_prefix *p) {
        if (!p)
                return NULL;

        assert(p->n_ref > 0);
        p->n_ref++;

        return p;
}

_public_ sd_radv_prefix *sd_radv_prefix_unref(sd_radv_prefix *p) {
        if (!p)
                return NULL;

        assert(p->n_ref > 0);
        p->n_ref--;

        if (p->n_ref > 0)
                return NULL;

        return mfree(p);
}

_public_ int sd_radv_prefix_set_prefix(sd_radv_prefix *p, struct in6_addr *in6_addr,
                                       unsigned char prefixlen) {
        assert_return(p, -EINVAL);
        assert_return(in6_addr, -EINVAL);

        if (prefixlen < 3 || prefixlen > 128)
                return -EINVAL;

        if (prefixlen > 64)
                /* unusual but allowed, log it */
                log_radv("Unusual prefix length %d greater than 64", prefixlen);

        p->opt.in6_addr = *in6_addr;
        p->opt.prefixlen = prefixlen;

        return 0;
}

_public_ int sd_radv_prefix_set_onlink(sd_radv_prefix *p, int onlink) {
        assert_return(p, -EINVAL);

        SET_FLAG(p->opt.flags, ND_OPT_PI_FLAG_ONLINK, onlink);

        return 0;
}

_public_ int sd_radv_prefix_set_address_autoconfiguration(sd_radv_prefix *p,
                                                          int address_autoconfiguration) {
        assert_return(p, -EINVAL);

        SET_FLAG(p->opt.flags, ND_OPT_PI_FLAG_AUTO, address_autoconfiguration);

        return 0;
}

_public_ int sd_radv_prefix_set_valid_lifetime(sd_radv_prefix *p,
                                               uint32_t valid_lifetime) {
        assert_return(p, -EINVAL);

        p->opt.valid_lifetime = htobe32(valid_lifetime);

        return 0;
}

_public_ int sd_radv_prefix_set_preferred_lifetime(sd_radv_prefix *p,
                                                   uint32_t preferred_lifetime) {
        assert_return(p, -EINVAL);

        p->opt.preferred_lifetime = htobe32(preferred_lifetime);

        return 0;
}
