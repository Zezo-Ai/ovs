/*
 * Copyright (c) 2014, 2015, 2016, 2017 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "ovs-router.h"

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "classifier.h"
#include "command-line.h"
#include "compiler.h"
#include "dpif.h"
#include "fatal-signal.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/json.h"
#include "netdev.h"
#include "packets.h"
#include "seq.h"
#include "ovs-thread.h"
#include "route-table.h"
#include "tnl-ports.h"
#include "unixctl.h"
#include "util.h"
#include "unaligned.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(ovs_router);

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

static struct ovs_mutex mutex = OVS_MUTEX_INITIALIZER;
static struct classifier cls;

/* By default, use the system routing table.  For system-independent testing,
 * the unit tests disable using the system routing table. */
static bool use_system_routing_table = true;

struct ovs_router_entry {
    struct cls_rule cr;
    char output_netdev[IFNAMSIZ];
    struct in6_addr gw;
    struct in6_addr nw_addr;
    struct in6_addr src_addr;
    uint8_t plen;
    uint8_t priority;
    bool local;
    uint32_t mark;
};

static struct ovs_router_entry *
ovs_router_entry_cast(const struct cls_rule *cr)
{
    return cr ? CONTAINER_OF(cr, struct ovs_router_entry, cr) : NULL;
}

/* Disables obtaining routes from the system routing table, for testing
 * purposes. */
void
ovs_router_disable_system_routing_table(void)
{
    use_system_routing_table = false;
}

static bool
ovs_router_lookup_fallback(const struct in6_addr *ip6_dst,
                           char output_netdev[], struct in6_addr *src6,
                           struct in6_addr *gw6)
{
    ovs_be32 src;

    if (!use_system_routing_table
        || !route_table_fallback_lookup(ip6_dst, output_netdev, gw6)) {
        return false;
    }
    if (netdev_get_in4_by_name(output_netdev, (struct in_addr *)&src)) {
        return false;
    }
    if (src6) {
        in6_addr_set_mapped_ipv4(src6, src);
    }
    return true;
}

bool
ovs_router_lookup(uint32_t mark, const struct in6_addr *ip6_dst,
                  char output_netdev[],
                  struct in6_addr *src, struct in6_addr *gw)
{
    const struct cls_rule *cr;
    struct flow flow = {.ipv6_dst = *ip6_dst, .pkt_mark = mark};

    if (src && ipv6_addr_is_set(src)) {
        const struct cls_rule *cr_src;
        struct flow flow_src = {.ipv6_dst = *src, .pkt_mark = mark};

        cr_src = classifier_lookup(&cls, OVS_VERSION_MAX, &flow_src, NULL,
                                   NULL);
        if (cr_src) {
            struct ovs_router_entry *p_src = ovs_router_entry_cast(cr_src);
            if (!p_src->local) {
                return false;
            }
        } else {
            return false;
        }
    }

    cr = classifier_lookup(&cls, OVS_VERSION_MAX, &flow, NULL, NULL);
    if (cr) {
        struct ovs_router_entry *p = ovs_router_entry_cast(cr);

        ovs_strlcpy(output_netdev, p->output_netdev, IFNAMSIZ);
        *gw = p->gw;
        if (src && !ipv6_addr_is_set(src)) {
            *src = p->src_addr;
        }
        return true;
    }
    return ovs_router_lookup_fallback(ip6_dst, output_netdev, src, gw);
}

static void
rt_entry_free(struct ovs_router_entry *p)
{
    cls_rule_destroy(&p->cr);
    free(p);
}

static void rt_init_match(struct match *match, uint32_t mark,
                          const struct in6_addr *ip6_dst,
                          uint8_t plen)
{
    struct in6_addr dst;
    struct in6_addr mask;

    mask = ipv6_create_mask(plen);

    dst = ipv6_addr_bitand(ip6_dst, &mask);
    memset(match, 0, sizeof *match);
    match->flow.ipv6_dst = dst;
    match->wc.masks.ipv6_dst = mask;
    match->wc.masks.pkt_mark = UINT32_MAX;
    match->flow.pkt_mark = mark;
}

static int
verify_prefsrc(const struct in6_addr *ip6_dst,
               const char netdev_name[],
               struct in6_addr *prefsrc)
{
    struct in6_addr *mask, *addr6;
    struct netdev *dev;
    int err, n_in6, i;

    err = netdev_open(netdev_name, NULL, &dev);
    if (err) {
        return err;
    }

    err = netdev_get_addr_list(dev, &addr6, &mask, &n_in6);
    if (err) {
        goto out;
    }

    for (i = 0; i < n_in6; i++) {
        struct in6_addr a1, a2;
        a1 = ipv6_addr_bitand(ip6_dst, &mask[i]);
        a2 = ipv6_addr_bitand(prefsrc, &mask[i]);

        /* Check that the interface has "prefsrc" and
         * it is same broadcast domain with "ip6_dst". */
        if (IN6_ARE_ADDR_EQUAL(prefsrc, &addr6[i]) &&
            IN6_ARE_ADDR_EQUAL(&a1, &a2)) {
            goto out;
        }
    }
    err = ENOENT;

out:
    free(addr6);
    free(mask);
    netdev_close(dev);
    return err;
}

int
ovs_router_get_netdev_source_address(const struct in6_addr *ip6_dst,
                                     const char netdev_name[],
                                     struct in6_addr *psrc)
{
    struct in6_addr *mask, *addr6;
    int err, n_in6, i, max_plen = -1;
    struct netdev *dev;
    bool is_ipv4;

    err = netdev_open(netdev_name, NULL, &dev);
    if (err) {
        return err;
    }

    err = netdev_get_addr_list(dev, &addr6, &mask, &n_in6);
    if (err) {
        goto out;
    }

    is_ipv4 = IN6_IS_ADDR_V4MAPPED(ip6_dst);

    for (i = 0; i < n_in6; i++) {
        struct in6_addr a1, a2;
        int mask_bits;

        if (is_ipv4 && !IN6_IS_ADDR_V4MAPPED(&addr6[i])) {
            continue;
        }

        a1 = ipv6_addr_bitand(ip6_dst, &mask[i]);
        a2 = ipv6_addr_bitand(&addr6[i], &mask[i]);
        mask_bits = bitmap_count1(ALIGNED_CAST(const unsigned long *, &mask[i]), 128);

        if (!memcmp(&a1, &a2, sizeof (a1)) && mask_bits > max_plen) {
            *psrc = addr6[i];
            max_plen = mask_bits;
        }
    }
    if (max_plen == -1) {
        err = ENOENT;
    }
out:
    free(addr6);
    free(mask);
    netdev_close(dev);
    return err;
}

static int
ovs_router_insert__(uint32_t mark, uint8_t priority, bool local,
                    const struct in6_addr *ip6_dst,
                    uint8_t plen, const char output_netdev[],
                    const struct in6_addr *gw,
                    const struct in6_addr *ip6_src)
{
    int (*get_src_addr)(const struct in6_addr *ip6_dst,
                        const char output_netdev[],
                        struct in6_addr *prefsrc);
    const struct cls_rule *cr;
    struct ovs_router_entry *p;
    struct match match;
    int err;

    rt_init_match(&match, mark, ip6_dst, plen);

    p = xzalloc(sizeof *p);
    ovs_strlcpy(p->output_netdev, output_netdev, sizeof p->output_netdev);
    if (ipv6_addr_is_set(gw)) {
        p->gw = *gw;
    }
    p->mark = mark;
    p->nw_addr = match.flow.ipv6_dst;
    p->plen = plen;
    p->local = local;
    p->priority = priority;

    if (ipv6_addr_is_set(ip6_src)) {
        p->src_addr = *ip6_src;
        get_src_addr = verify_prefsrc;
    } else {
        get_src_addr = ovs_router_get_netdev_source_address;
    }

    err = get_src_addr(ip6_dst, output_netdev, &p->src_addr);
    if (err && ipv6_addr_is_set(gw)) {
        err = get_src_addr(gw, output_netdev, &p->src_addr);
    }
    if (err) {
        struct ds ds = DS_EMPTY_INITIALIZER;

        ipv6_format_mapped(ip6_dst, &ds);
        VLOG_DBG_RL(&rl, "src addr not available for route %s", ds_cstr(&ds));
        free(p);
        ds_destroy(&ds);
        return err;
    }
    /* Longest prefix matches first. */
    cls_rule_init(&p->cr, &match, priority);

    ovs_mutex_lock(&mutex);
    cr = classifier_replace(&cls, &p->cr, OVS_VERSION_MIN, NULL, 0);
    ovs_mutex_unlock(&mutex);

    if (cr) {
        /* An old rule with the same match was displaced. */
        ovsrcu_postpone(rt_entry_free, ovs_router_entry_cast(cr));
    }
    tnl_port_map_insert_ipdev(output_netdev);
    seq_change(tnl_conf_seq);
    return 0;
}

void
ovs_router_insert(uint32_t mark, const struct in6_addr *ip_dst, uint8_t plen,
                  bool local, const char output_netdev[],
                  const struct in6_addr *gw, const struct in6_addr *prefsrc)
{
    if (use_system_routing_table) {
        uint8_t priority = local ? plen + 64 : plen;
        ovs_router_insert__(mark, priority, local, ip_dst, plen,
                            output_netdev, gw, prefsrc);
    }
}

/* The same as 'ovs_router_insert', but it adds the route even if updates
 * from the system routing table are disabled.  Used for unit tests. */
void
ovs_router_force_insert(uint32_t mark, const struct in6_addr *ip_dst,
                        uint8_t plen, bool local, const char output_netdev[],
                        const struct in6_addr *gw,
                        const struct in6_addr *prefsrc)
{
    uint8_t priority = local ? plen + 64 : plen;

    ovs_router_insert__(mark, priority, local, ip_dst, plen,
                        output_netdev, gw, prefsrc);
}

static void
rt_entry_delete__(const struct cls_rule *cr)
{
    struct ovs_router_entry *p = ovs_router_entry_cast(cr);

    tnl_port_map_delete_ipdev(p->output_netdev);
    classifier_remove_assert(&cls, cr);
    ovsrcu_postpone(rt_entry_free, ovs_router_entry_cast(cr));
}

static bool
rt_entry_delete(uint32_t mark, uint8_t priority,
                const struct in6_addr *ip6_dst, uint8_t plen)
{
    const struct cls_rule *cr;
    struct cls_rule rule;
    struct match match;
    bool res = false;

    rt_init_match(&match, mark, ip6_dst, plen);

    cls_rule_init(&rule, &match, priority);

    /* Find the exact rule. */
    cr = classifier_find_rule_exactly(&cls, &rule, OVS_VERSION_MAX);
    if (cr) {
        ovs_mutex_lock(&mutex);
        rt_entry_delete__(cr);
        ovs_mutex_unlock(&mutex);

        res = true;
    }

    cls_rule_destroy(&rule);
    return res;
}

static bool
scan_ipv6_route(const char *s, struct in6_addr *addr, unsigned int *plen)
{
    char *error = ipv6_parse_cidr(s, addr, plen);
    if (error) {
        free(error);
        return false;
    }
    return true;
}

static bool
scan_ipv4_route(const char *s, ovs_be32 *addr, unsigned int *plen)
{
    char *error = ip_parse_cidr(s, addr, plen);
    if (error) {
        free(error);
        return false;
    }
    return true;
}

static void
ovs_router_add(struct unixctl_conn *conn, int argc,
              const char *argv[], void *aux OVS_UNUSED)
{
    struct in6_addr src6 = in6addr_any;
    struct in6_addr gw6 = in6addr_any;
    char src6_s[IPV6_SCAN_LEN + 1];
    struct in6_addr ip6;
    uint32_t mark = 0;
    unsigned int plen;
    ovs_be32 src = 0;
    ovs_be32 gw = 0;
    bool is_ipv6;
    ovs_be32 ip;
    int err;
    int i;

    if (scan_ipv4_route(argv[1], &ip, &plen)) {
        in6_addr_set_mapped_ipv4(&ip6, ip);
        plen += 96;
        is_ipv6 = false;
    } else if (scan_ipv6_route(argv[1], &ip6, &plen)) {
        is_ipv6 = true;
    } else {
        unixctl_command_reply_error(conn,
                                    "Invalid 'ip/plen' parameter");
        return;
    }

    /* Parse optional parameters. */
    for (i = 3; i < argc; i++) {
        if (ovs_scan(argv[i], "pkt_mark=%"SCNi32, &mark)) {
            continue;
        }

        if (is_ipv6) {
            if (ovs_scan(argv[i], "src="IPV6_SCAN_FMT, src6_s) &&
                ipv6_parse(src6_s, &src6)) {
                continue;
            }
            if (ipv6_parse(argv[i], &gw6)) {
                continue;
            }
        } else {
            if (ovs_scan(argv[i], "src="IP_SCAN_FMT, IP_SCAN_ARGS(&src))) {
                continue;
            }
            if (ip_parse(argv[i], &gw)) {
                continue;
            }
        }

        unixctl_command_reply_error(conn,
                                    "Invalid pkt_mark, IP gateway or src_ip");
        return;
    }

    if (gw) {
        in6_addr_set_mapped_ipv4(&gw6, gw);
    }
    if (src) {
        in6_addr_set_mapped_ipv4(&src6, src);
    }

    err = ovs_router_insert__(mark, plen + 32, false, &ip6, plen, argv[2],
                              &gw6, &src6);
    if (err) {
        unixctl_command_reply_error(conn, "Error while inserting route.");
    } else {
        unixctl_command_reply(conn, "OK");
    }
}

static void
ovs_router_del(struct unixctl_conn *conn, int argc OVS_UNUSED,
              const char *argv[], void *aux OVS_UNUSED)
{
    struct in6_addr ip6;
    uint32_t mark = 0;
    unsigned int plen;
    ovs_be32 ip;

    if (scan_ipv4_route(argv[1], &ip, &plen)) {
        in6_addr_set_mapped_ipv4(&ip6, ip);
        plen += 96;
    } else if (!scan_ipv6_route(argv[1], &ip6, &plen)) {
        unixctl_command_reply_error(conn, "Invalid parameters");
        return;
    }
    if (argc > 2) {
        if (!ovs_scan(argv[2], "pkt_mark=%"SCNi32, &mark)) {
            unixctl_command_reply_error(conn, "Invalid pkt_mark");
            return;
        }
    }

    if (rt_entry_delete(mark, plen + 32, &ip6, plen)) {
        unixctl_command_reply(conn, "OK");
        seq_change(tnl_conf_seq);
    } else {
        unixctl_command_reply_error(conn, "Not found");
    }
}

static void
ovs_router_show_json(struct json **routes)
{
    int n_rules = classifier_count(&cls);
    struct json **json_entries = NULL;
    struct ovs_router_entry *rt;
    struct ds ds;
    int i = 0;

    if (!n_rules) {
        goto out;
    }

    json_entries = xmalloc(n_rules * sizeof *json_entries);
    ds_init(&ds);

    CLS_FOR_EACH (rt, cr, &cls) {
        bool user = rt->priority != rt->plen && !rt->local;
        uint8_t plen = rt->plen;
        struct json *json, *nh;

        if (i >= n_rules) {
            break;
        }

        json = json_object_create();
        nh = json_object_create();

        if (IN6_IS_ADDR_V4MAPPED(&rt->nw_addr)) {
            plen -= 96;
        }

        json_object_put(json, "user", json_boolean_create(user));
        json_object_put(json, "local", json_boolean_create(rt->local));
        json_object_put(json, "priority", json_integer_create(rt->priority));
        json_object_put(json, "prefix", json_integer_create(plen));
        json_object_put_string(nh, "dev", rt->output_netdev);

        ipv6_format_mapped(&rt->nw_addr, &ds);
        json_object_put_string(json, "dst", ds_cstr_ro(&ds));
        ds_clear(&ds);

        ipv6_format_mapped(&rt->src_addr, &ds);
        json_object_put_string(json, "prefsrc", ds_cstr_ro(&ds));
        ds_clear(&ds);

        if (rt->mark) {
            json_object_put(json, "mark", json_integer_create(rt->mark));
        }

        if (ipv6_addr_is_set(&rt->gw)) {
            ipv6_format_mapped(&rt->gw, &ds);
            json_object_put_string(nh, "gateway", ds_cstr_ro(&ds));
            ds_clear(&ds);
        }

        json_object_put(json, "nexthops", json_array_create_1(nh));
        json_entries[i++] = json;
    }

    ds_destroy(&ds);

out:
    *routes = json_array_create(json_entries, i);
}

static void
ovs_router_show_text(struct ds *ds)
{
    struct ovs_router_entry *rt;

    ds_put_format(ds, "Route Table:\n");
    CLS_FOR_EACH (rt, cr, &cls) {
        uint8_t plen;
        if (rt->priority == rt->plen || rt->local) {
            ds_put_format(ds, "Cached: ");
        } else {
            ds_put_format(ds, "User: ");
        }
        ipv6_format_mapped(&rt->nw_addr, ds);
        plen = rt->plen;
        if (IN6_IS_ADDR_V4MAPPED(&rt->nw_addr)) {
            plen -= 96;
        }
        ds_put_format(ds, "/%"PRIu8, plen);
        if (rt->mark) {
            ds_put_format(ds, " MARK %"PRIu32, rt->mark);
        }

        ds_put_format(ds, " dev %s", rt->output_netdev);
        if (ipv6_addr_is_set(&rt->gw)) {
            ds_put_format(ds, " GW ");
            ipv6_format_mapped(&rt->gw, ds);
        }
        ds_put_format(ds, " SRC ");
        ipv6_format_mapped(&rt->src_addr, ds);
        if (rt->local) {
            ds_put_format(ds, " local");
        }
        ds_put_format(ds, "\n");
    }
}

static void
ovs_router_show(struct unixctl_conn *conn, int argc OVS_UNUSED,
               const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    if (unixctl_command_get_output_format(conn) == UNIXCTL_OUTPUT_FMT_JSON) {
        struct json *routes;

        ovs_router_show_json(&routes);
        unixctl_command_reply_json(conn, routes);
    } else {
        struct ds ds = DS_EMPTY_INITIALIZER;

        ovs_router_show_text(&ds);
        unixctl_command_reply(conn, ds_cstr(&ds));
        ds_destroy(&ds);
    }
}

static void
ovs_router_lookup_cmd(struct unixctl_conn *conn, int argc,
                      const char *argv[], void *aux OVS_UNUSED)
{
    struct in6_addr gw, src = in6addr_any;
    char iface[IFNAMSIZ];
    struct in6_addr ip6;
    unsigned int plen;
    uint32_t mark = 0;
    ovs_be32 ip;

    if (scan_ipv4_route(argv[1], &ip, &plen) && plen == 32) {
        in6_addr_set_mapped_ipv4(&ip6, ip);
    } else if (!(scan_ipv6_route(argv[1], &ip6, &plen) && plen == 128)) {
        unixctl_command_reply_error(conn, "Invalid parameters");
        return;
    }
    if (argc > 2) {
        if (!ovs_scan(argv[2], "pkt_mark=%"SCNi32, &mark)) {
            unixctl_command_reply_error(conn, "Invalid pkt_mark");
            return;
        }
    }
    if (ovs_router_lookup(mark, &ip6, iface, &src, &gw)) {
        struct ds ds = DS_EMPTY_INITIALIZER;

        ds_put_format(&ds, "src ");
        ipv6_format_mapped(&src, &ds);
        ds_put_format(&ds, "\ngateway ");
        ipv6_format_mapped(&gw, &ds);
        ds_put_format(&ds, "\ndev %s\n", iface);
        unixctl_command_reply(conn, ds_cstr(&ds));
        ds_destroy(&ds);
    } else {
        unixctl_command_reply_error(conn, "Not found");
    }
}

void
ovs_router_flush(void)
{
    struct ovs_router_entry *rt;

    ovs_mutex_lock(&mutex);
    classifier_defer(&cls);
    CLS_FOR_EACH(rt, cr, &cls) {
        if (rt->priority == rt->plen || rt->local) {
            rt_entry_delete__(&rt->cr);
        }
    }
    classifier_publish(&cls);
    ovs_mutex_unlock(&mutex);
    seq_change(tnl_conf_seq);
}

static void
ovs_router_flush_handler(void *aux OVS_UNUSED)
{
    ovs_router_flush();
}

void
ovs_router_init(void)
{
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;

    if (ovsthread_once_start(&once)) {
        fatal_signal_add_hook(ovs_router_flush_handler, NULL, NULL, true);
        classifier_init(&cls, NULL);
        unixctl_command_register("ovs/route/add",
                                 "ip/plen dev [gw] "
                                 "[pkt_mark=mark] [src=src_ip]",
                                 2, 5, ovs_router_add, NULL);
        unixctl_command_register("ovs/route/show", "", 0, 0,
                                 ovs_router_show, NULL);
        unixctl_command_register("ovs/route/del", "ip/plen "
                                 "[pkt_mark=mark]", 1, 2, ovs_router_del,
                                 NULL);
        unixctl_command_register("ovs/route/lookup", "ip_addr "
                                 "[pkt_mark=mark]", 1, 2,
                                 ovs_router_lookup_cmd, NULL);
        ovsthread_once_done(&once);
    }
}
