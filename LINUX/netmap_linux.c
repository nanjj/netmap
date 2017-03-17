/*
 * Copyright (C) 2013-2014 Universita` di Pisa. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
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

#include "bsd_glue.h"
#include <linux/file.h>   /* fget(int fd) */

#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include <net/netmap_virt.h>
#include <dev/netmap/netmap_mem2.h>
#include <linux/rtnetlink.h>
#include <linux/nsproxy.h>
#include <net/pkt_sched.h>
#include <net/sch_generic.h>
#ifdef NETMAP_LINUX_HAVE_SCHED_MM
#include <linux/sched/mm.h>
#endif /* NETMAP_LINUX_HAVE_SCHED_MM */

#include "netmap_linux_config.h"

void *
nm_os_malloc(size_t size)
{
	void *rv = kmalloc(size, GFP_ATOMIC | __GFP_ZERO);
	if (IS_ERR(rv))
		return NULL;
	return rv;
}

void *
nm_os_realloc(void *addr, size_t new_size, size_t old_size)
{
	void *rv;
	(void)old_size;
	rv = krealloc(addr, new_size, GFP_ATOMIC | __GFP_ZERO);
	if (IS_ERR(rv))
		return NULL;
	return rv;
}

void
nm_os_free(void *addr){
	kfree(addr);
}

void
nm_os_selinfo_init(NM_SELINFO_T *si)
{
	init_waitqueue_head(si);
}

void
nm_os_selinfo_uninit(NM_SELINFO_T *si)
{
}

void
nm_os_ifnet_lock(void)
{
	rtnl_lock();
}

void
nm_os_ifnet_unlock(void)
{
	rtnl_unlock();
}

void
nm_os_get_module(void)
{
	__module_get(THIS_MODULE);
}

void
nm_os_put_module(void)
{
	module_put(THIS_MODULE);
}

/* Register for a notification on device removal */
static int
linux_netmap_notifier_cb(struct notifier_block *b,
		unsigned long val, void *v)
{
	struct ifnet *ifp = netdev_notifier_info_to_dev(v);

	/* linux calls us while holding rtnl_lock() */
	switch (val) {
	case NETDEV_REGISTER:
		netmap_undo_zombie(ifp);
		break;
	case NETDEV_UNREGISTER:
		netmap_make_zombie(ifp);
		break;
	case NETDEV_GOING_DOWN:
		netmap_disable_all_rings(ifp);
		break;
	case NETDEV_UP:
		netmap_enable_all_rings(ifp);
		break;
	default:
		/* we don't care */
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block linux_netmap_netdev_notifier = {
	.notifier_call = linux_netmap_notifier_cb,
};

static int nm_os_ifnet_registered;

int
nm_os_ifnet_init(void)
{
	int error = NM_REG_NETDEV_NOTIF(&linux_netmap_netdev_notifier);
	if (!error)
		nm_os_ifnet_registered = 1;
	return error;
}

void
nm_os_ifnet_fini(void)
{
	if (nm_os_ifnet_registered) {
		NM_UNREG_NETDEV_NOTIF(&linux_netmap_netdev_notifier);
		nm_os_ifnet_registered = 0;
	}
}

#ifdef NETMAP_LINUX_HAVE_IOMMU
#include <linux/iommu.h>

/* #################### IOMMU ################## */
/*
 * Returns the IOMMU domain id that the device belongs to.
 */
int nm_iommu_group_id(struct device *dev)
{
	struct iommu_group *grp;
	int id;

	if (!dev)
		return 0;

	grp = iommu_group_get(dev);
	if (!grp)
		return 0;

	id = iommu_group_id(grp);
	return id;
}
#else /* ! HAVE_IOMMU */
int nm_iommu_group_id(struct device *dev)
{
	return 0;
}
#endif /* HAVE_IOMMU */

/* #################### VALE OFFLOADINGS SUPPORT ################## */

/* Compute and return a raw checksum over (data, len), using 'cur_sum'
 * as initial value. Both 'cur_sum' and the return value are in host
 * byte order.
 */
rawsum_t
nm_os_csum_raw(uint8_t *data, size_t len, rawsum_t cur_sum)
{
	return csum_partial(data, len, cur_sum);
}

/* Compute an IPv4 header checksum, where 'data' points to the IPv4 header,
 * and 'len' is the IPv4 header length. Return value is in network byte
 * order.
 */
uint16_t
nm_os_csum_ipv4(struct nm_iphdr *iph)
{
	return ip_compute_csum((void*)iph, sizeof(struct nm_iphdr));
}

/* Compute and insert a TCP/UDP checksum over IPv4: 'iph' points to the IPv4
 * header, 'data' points to the TCP/UDP header, 'datalen' is the lenght of
 * TCP/UDP header + payload.
 */
void
nm_os_csum_tcpudp_ipv4(struct nm_iphdr *iph, void *data,
		      size_t datalen, uint16_t *check)
{
	*check = csum_tcpudp_magic(iph->saddr, iph->daddr,
				datalen, iph->protocol,
				csum_partial(data, datalen, 0));
}

/* Compute and insert a TCP/UDP checksum over IPv6: 'ip6h' points to the IPv6
 * header, 'data' points to the TCP/UDP header, 'datalen' is the lenght of
 * TCP/UDP header + payload.
 */
void
nm_os_csum_tcpudp_ipv6(struct nm_ipv6hdr *ip6h, void *data,
		      size_t datalen, uint16_t *check)
{
	*check = csum_ipv6_magic((void *)&ip6h->saddr, (void*)&ip6h->daddr,
				datalen, ip6h->nexthdr,
				csum_partial(data, datalen, 0));
}

uint16_t
nm_os_csum_fold(rawsum_t cur_sum)
{
	return csum_fold(cur_sum);
}

/* on linux we send up one packet at a time */
void *
nm_os_send_up(struct ifnet *ifp, struct mbuf *m, struct mbuf *prev)
{
	(void)ifp;
	(void)prev;
	m->priority = NM_MAGIC_PRIORITY_RX; /* do not reinject to netmap */
	netif_rx(m);
	return NULL;
}

int
nm_os_mbuf_has_offld(struct mbuf *m)
{
	return m->ip_summed == CHECKSUM_PARTIAL || skb_is_gso(m);
}

#ifdef WITH_GENERIC
/* ####################### MITIGATION SUPPORT ###################### */

/*
 * The generic driver calls netmap once per received packet.
 * This is inefficient so we implement a mitigation mechanism,
 * as follows:
 * - the first packet on an idle receiver triggers a notification
 *   and starts a timer;
 * - subsequent incoming packets do not cause a notification
 *   until the timer expires;
 * - when the timer expires and there are pending packets,
 *   a notification is sent up and the timer is restarted.
 */
static NETMAP_LINUX_TIMER_RTYPE
generic_timer_handler(struct hrtimer *t)
{
    struct nm_generic_mit *mit =
	container_of(t, struct nm_generic_mit, mit_timer);
    u_int work_done;

    if (!mit->mit_pending) {
        return HRTIMER_NORESTART;
    }

    /* Some work arrived while the timer was counting down:
     * Reset the pending work flag, restart the timer and send
     * a notification.
     */
    mit->mit_pending = 0;
    /* below is a variation of netmap_generic_irq  XXX revise */
    if (nm_netmap_on(mit->mit_na)) {
        netmap_common_irq(mit->mit_na, mit->mit_ring_idx, &work_done);
        generic_rate(0, 0, 0, 0, 0, 1);
    }
    nm_os_mitigation_restart(mit);

    return HRTIMER_RESTART;
}


void
nm_os_mitigation_init(struct nm_generic_mit *mit, int idx,
                                struct netmap_adapter *na)
{
    hrtimer_init(&mit->mit_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    mit->mit_timer.function = &generic_timer_handler;
    mit->mit_pending = 0;
    mit->mit_ring_idx = idx;
    mit->mit_na = na;
}


void
nm_os_mitigation_start(struct nm_generic_mit *mit)
{
    hrtimer_start(&mit->mit_timer, ktime_set(0, netmap_generic_mit), HRTIMER_MODE_REL);
}

void
nm_os_mitigation_restart(struct nm_generic_mit *mit)
{
    hrtimer_forward_now(&mit->mit_timer, ktime_set(0, netmap_generic_mit));
}

int
nm_os_mitigation_active(struct nm_generic_mit *mit)
{
    return hrtimer_active(&mit->mit_timer);
}

void
nm_os_mitigation_cleanup(struct nm_generic_mit *mit)
{
    hrtimer_cancel(&mit->mit_timer);
}



/* #################### GENERIC ADAPTER SUPPORT ################### */

/*
 * This handler is registered within the attached net_device
 * in the Linux RX subsystem, so that every mbuf passed up by
 * the driver can be stolen to the network stack.
 * Stolen packets are put in a queue where the
 * generic_netmap_rxsync() callback can extract them.
 * Packets that comes from netmap_txsync_to_host() are not
 * stolen.
 */
#ifdef NETMAP_LINUX_HAVE_RX_REGISTER
enum {
	NM_RX_HANDLER_STOLEN,
	NM_RX_HANDLER_PASS,
};

static inline int
linux_generic_rx_handler_common(struct mbuf *m)
{
	int stolen;

	/* If we were called by NM_SEND_UP(), we want to pass the mbuf
	   to network stack. We detect this situation looking at the
	   priority field. */
	if (m->priority == NM_MAGIC_PRIORITY_RX) {
		return NM_RX_HANDLER_PASS;
	}

	/* When we intercept a sk_buff coming from the driver, it happens that
	   skb->data points to the IP header, e.g. the ethernet header has
	   already been pulled. Since we want the netmap rings to contain the
	   full ethernet header, we push it back, so that the RX ring reader
	   can see it. */
	skb_push(m, ETH_HLEN);

	/* Possibly steal the mbuf and notify the pollers for a new RX
	 * packet. */
	stolen = generic_rx_handler(m->dev, m);
	if (stolen) {
		return NM_RX_HANDLER_STOLEN;
	}

	skb_pull(m, ETH_HLEN);

	return NM_RX_HANDLER_PASS;
}

#ifdef NETMAP_LINUX_HAVE_RX_HANDLER_RESULT
static rx_handler_result_t
linux_generic_rx_handler(struct mbuf **pm)
{
	int ret = linux_generic_rx_handler_common(*pm);

	return likely(ret == NM_RX_HANDLER_STOLEN) ? RX_HANDLER_CONSUMED :
						     RX_HANDLER_PASS;
}
#else /* ! HAVE_RX_HANDLER_RESULT */
static struct sk_buff *
linux_generic_rx_handler(struct mbuf *m)
{
	int ret = linux_generic_rx_handler_common(m);

	return likely(ret == NM_RX_HANDLER_STOLEN) ? NULL : m;
}
#endif /* HAVE_RX_HANDLER_RESULT */
#endif /* HAVE_RX_REGISTER */

/* Ask the Linux RX subsystem to intercept (or stop intercepting)
 * the packets incoming from the interface attached to 'na'.
 */
int
nm_os_catch_rx(struct netmap_generic_adapter *gna, int intercept)
{
#ifndef NETMAP_LINUX_HAVE_RX_REGISTER
#warning "Packet reception with emulated (generic) mode not supported for this kernel version"
    return 0;
#else /* HAVE_RX_REGISTER */
    struct netmap_adapter *na = &gna->up.up;
    struct ifnet *ifp = netmap_generic_getifp(gna);

    if (intercept) {
        return -netdev_rx_handler_register(ifp,
                &linux_generic_rx_handler, na);
    } else {
        netdev_rx_handler_unregister(ifp);
        return 0;
    }
#endif /* HAVE_RX_REGISTER */
}

#ifdef NETMAP_LINUX_SELECT_QUEUE
static u16
generic_ndo_select_queue(struct ifnet *ifp, struct mbuf *m
#if NETMAP_LINUX_SELECT_QUEUE >= 3
                                , void *accel_priv
#if NETMAP_LINUX_SELECT_QUEUE >= 4
				, select_queue_fallback_t fallback
#endif /* >= 4 */
#endif /* >= 3 */
		)
{
    return skb_get_queue_mapping(m); // actually 0 on 2.6.23 and before
}
#endif /* SELECT_QUEUE */

/* Replacement for the driver ndo_start_xmit() method.
 * When this function is invoked because of the dev_queue_xmit() call
 * in generic_xmit_frame() (e.g. because of a txsync on the NIC), we have
 * to call the original ndo_start_xmit() method.
 * In all the other cases (e.g. when the TX request comes from the network
 * stack) we intercept the packet and put it into the RX ring associated
 * to the host stack.
 */
static netdev_tx_t
generic_ndo_start_xmit(struct mbuf *m, struct ifnet *ifp)
{
	struct netmap_generic_adapter *gna =
		(struct netmap_generic_adapter *)NA(ifp);

	if (likely(m->priority == NM_MAGIC_PRIORITY_TX)) {
		/* Reset priority, so that generic_netmap_tx_clean()
		 * knows that it can reclaim this mbuf. */
		m->priority = 0;
		return gna->save_start_xmit(m, ifp); /* To the driver. */
	}

	/* To a netmap RX ring. */
	return linux_netmap_start_xmit(m, ifp);
}

struct nm_generic_qdisc {
	unsigned int qidx;
	unsigned int limit;
};

static int
generic_qdisc_init(struct Qdisc *qdisc, struct nlattr *opt)
{
	struct nm_generic_qdisc *priv = NULL;

	/* Kernel < 2.6.39, do not have qdisc->limit, so we will
	 * always use our priv->limit, for simplicity. */

	priv = qdisc_priv(qdisc);
	priv->qidx = 0;
	priv->limit = 1024; /* This is going to be overridden. */

	if (opt) {
		struct nm_generic_qdisc *qdiscopt = nla_data(opt);

		if (nla_len(opt) < sizeof(*qdiscopt)) {
			D("Invalid netlink attribute");
			return EINVAL;
		}

		priv->qidx = qdiscopt->qidx;
		priv->limit = qdiscopt->limit;
		D("Qdisc #%d initialized with max_len = %u", priv->qidx,
				                             priv->limit);
	}

	/* Qdisc bypassing is not an option for now.
	qdisc->flags |= TCQ_F_CAN_BYPASS; */

	return 0;
}

static int
generic_qdisc_enqueue(struct mbuf *m, struct Qdisc *qdisc
#ifdef NETMAP_LINUX_HAVE_QDISC_ENQUEUE_TOFREE
		      , struct mbuf **to_free
#endif
)
{
	struct nm_generic_qdisc *priv = qdisc_priv(qdisc);

	if (unlikely(qdisc_qlen(qdisc) >= priv->limit)) {
		RD(5, "dropping mbuf");

		return qdisc_drop(m, qdisc
#ifdef NETMAP_LINUX_HAVE_QDISC_ENQUEUE_TOFREE
		       , to_free
#endif
			);
		/* or qdisc_reshape_fail() ? */
	}

	ND(5, "Enqueuing mbuf, len %u", qdisc_qlen(qdisc));

	return qdisc_enqueue_tail(m, qdisc);
}

static struct mbuf *
generic_qdisc_dequeue(struct Qdisc *qdisc)
{
	struct mbuf *m = qdisc_dequeue_head(qdisc);

	if (!m) {
		return NULL;
	}

        if (unlikely(m->priority == NM_MAGIC_PRIORITY_TXQE)) {
            /* nm_os_generic_xmit_frame() asked us an event on this mbuf.
             * We have to set the priority to the normal TX token, so that
             * generic_ndo_start_xmit can pass it to the driver. */
            m->priority = NM_MAGIC_PRIORITY_TX;
            ND(5, "Event met, notify %p", m);
            netmap_generic_irq(NA(qdisc_dev(qdisc)),
                               skb_get_queue_mapping(m), NULL);
        }

	ND(5, "Dequeuing mbuf, len %u", qdisc_qlen(qdisc));

	return m;
}

static struct Qdisc_ops
generic_qdisc_ops __read_mostly = {
	.id		= "netmap_generic",
	.priv_size	= sizeof(struct nm_generic_qdisc),
	.init		= generic_qdisc_init,
	.reset		= qdisc_reset_queue,
	.change		= generic_qdisc_init,
	.enqueue	= generic_qdisc_enqueue,
	.dequeue	= generic_qdisc_dequeue,
	.dump		= NULL,
	.owner		= THIS_MODULE,
};

static int
nm_os_catch_qdisc(struct netmap_generic_adapter *gna, int intercept)
{
	struct netmap_adapter *na = &gna->up.up;
	struct ifnet *ifp = netmap_generic_getifp(gna);
	struct nm_generic_qdisc *qdiscopt = NULL;
	struct Qdisc *fqdisc = NULL;
	struct nlattr *nla = NULL;
	struct netdev_queue *txq;
	unsigned int i;

	if (!gna->txqdisc) {
		return 0;
	}

	if (intercept) {
		nla = kmalloc(nla_attr_size(sizeof(*qdiscopt)),
				GFP_KERNEL);
		if (!nla) {
			D("Failed to allocate netlink attribute");
			return ENOMEM;
		}
		nla->nla_type = RTM_NEWQDISC;
		nla->nla_len = nla_attr_size(sizeof(*qdiscopt));
		qdiscopt = (struct nm_generic_qdisc *)nla_data(nla);
		memset(qdiscopt, 0, sizeof(*qdiscopt));
		qdiscopt->limit = na->num_tx_desc;
	}

	if (ifp->flags & IFF_UP) {
		dev_deactivate(ifp);
	}

	/* Replace the current qdiscs with our own. */
	for (i = 0; i < ifp->real_num_tx_queues; i++) {
		struct Qdisc *nqdisc = NULL;
		struct Qdisc *oqdisc;
		int err;

		txq = netdev_get_tx_queue(ifp, i);

		if (intercept) {
			/* This takes a refcount to netmap module, alloc the
			 * qdisc and calls the init() op with NULL netlink
			 * attribute. */
			nqdisc = qdisc_create_dflt(
#ifndef NETMAP_LINUX_QDISC_CREATE_DFLT_3ARGS
					ifp,
#endif  /* NETMAP_LINUX_QDISC_CREATE_DFLT_3ARGS */
					txq, &generic_qdisc_ops,
					TC_H_UNSPEC);
			if (!nqdisc) {
				D("Failed to create qdisc");
				goto qdisc_create;
			}
			fqdisc = fqdisc ?: nqdisc;

			/* Call the change() op passing a valid netlink
			 * attribute. This is used to set the queue idx. */
			qdiscopt->qidx = i;
			err = nqdisc->ops->change(nqdisc, nla);
			if (err) {
				D("Failed to init qdisc");
				goto qdisc_create;
			}
		}

		oqdisc = dev_graft_qdisc(txq, nqdisc);
		/* We can call this also with
		 * odisc == &noop_qdisc, since the noop
		 * qdisc has the TCQ_F_BUILTIN flag set,
		 * and so qdisc_destroy will skip it. */
		qdisc_destroy(oqdisc);
	}

	kfree(nla);

	if (ifp->qdisc) {
		qdisc_destroy(ifp->qdisc);
	}
	if (intercept) {
		atomic_inc(&fqdisc->refcnt);
		ifp->qdisc = fqdisc;
	} else {
		ifp->qdisc = &noop_qdisc;
	}

	if (ifp->flags & IFF_UP) {
		dev_activate(ifp);
	}

	return 0;

qdisc_create:
	if (nla) {
		kfree(nla);
	}

	nm_os_catch_qdisc(gna, 0);

	return -1;
}

/* Must be called under rtnl. */
int
nm_os_catch_tx(struct netmap_generic_adapter *gna, int intercept)
{
	struct netmap_adapter *na = &gna->up.up;
	struct ifnet *ifp = netmap_generic_getifp(gna);
	int err;

	err = nm_os_catch_qdisc(gna, intercept);
	if (err) {
		return err;
	}

	if (intercept) {
		/*
		 * Save the old pointer to the netdev_ops,
		 * create an updated netdev ops replacing the
		 * ndo_select_queue() and ndo_start_xmit() methods
		 * with our custom ones, and make the driver use it.
		 */
		na->if_transmit = (void *)ifp->netdev_ops;
		/* Save a redundant copy of ndo_start_xmit(). */
		gna->save_start_xmit = ifp->netdev_ops->ndo_start_xmit;

		gna->generic_ndo = *ifp->netdev_ops;  /* Copy all */
		gna->generic_ndo.ndo_start_xmit = &generic_ndo_start_xmit;
#ifndef NETMAP_LINUX_SELECT_QUEUE
		D("No packet steering support");
#else
		gna->generic_ndo.ndo_select_queue = &generic_ndo_select_queue;
#endif

		ifp->netdev_ops = &gna->generic_ndo;

	} else {
		/* Restore the original netdev_ops. */
		ifp->netdev_ops = (void *)na->if_transmit;
	}

	return 0;
}

/* Transmit routine used by generic_netmap_txsync(). Returns 0 on success
   and -1 on error (which may be packet drops or other errors). */
int
nm_os_generic_xmit_frame(struct nm_os_gen_arg *a)
{
	struct mbuf *m = a->m;
	struct ifnet *ifp = a->ifp;
	u_int len = a->len;
	netdev_tx_t ret;

	/* We know that the driver needs to prepend ifp->needed_headroom bytes
	 * to each packet to be transmitted. We then reset the mbuf pointers
	 * to the correct initial state:
	 *    ___________________________________________
	 *    ^           ^                             ^
	 *    |           |                             |
	 *   head        data                          end
	 *               tail
	 *
	 * which correspond to an empty buffer with exactly
	 * ifp->needed_headroom bytes between head and data.
	 */
	m->len = 0;
	m->data = m->head + ifp->needed_headroom;
	skb_reset_tail_pointer(m);
	skb_reset_mac_header(m);

        /* Initialize the header pointers assuming this is an IPv4 packet.
         * This is useful to make netmap interact well with TC when
         * netmap_generic_txqdisc == 0.  */
	skb_set_network_header(m, 14);
	skb_set_transport_header(m, 34);
	m->protocol = htons(ETH_P_IP);
	m->pkt_type = PACKET_HOST;

	/* Copy a netmap buffer into the mbuf.
	 * TODO Support the slot flags (NS_MOREFRAG, NS_INDIRECT). */
	skb_copy_to_linear_data(m, a->addr, len); // skb_store_bits(m, 0, addr, len);
	skb_put(m, len);

	/* Hold a reference on this, we are going to recycle mbufs as
	 * much as possible. */
	NM_ATOMIC_INC(&m->users);

	/* On linux m->dev is not reliable, since it can be changed by the
	 * ndo_start_xmit() callback. This happens, for instance, with veth
	 * and bridge drivers. For this reason, the nm_os_generic_xmit_frame()
	 * implementation for linux stores a copy of m->dev into the
	 * destructor_arg field. */
	m->dev = ifp;
	skb_shinfo(m)->destructor_arg = m->dev;

	/* Tell generic_ndo_start_xmit() to pass this mbuf to the driver. */
	skb_set_queue_mapping(m, a->ring_nr);
	m->priority = a->qevent ? NM_MAGIC_PRIORITY_TXQE : NM_MAGIC_PRIORITY_TX;

	ret = dev_queue_xmit(m);

	if (unlikely(ret != NET_XMIT_SUCCESS)) {
		/* Reset priority, so that generic_netmap_tx_clean() can
		 * reclaim this mbuf. */
		m->priority = 0;

		/* Qdisc queue is full (this cannot happen with
		 * the netmap-aware qdisc, see exaplanation in
		 * netmap_generic_txsync), or qdisc is being
		 * deactivated. In the latter case dev_queue_xmit()
		 * does not call the enqueue method and returns
		 * NET_XMIT_DROP.
		 * If there is no carrier, the generic qdisc is
		 * not yet active (is pending in the qdisc_sleeping
		 * field), and so the temporary noop qdisc enqueue
		 * method will drop the packet and return NET_XMIT_CN.
		 */
		RD(3, "Warning: dev_queue_xmit() is dropping [%d]", ret);
		return -1;
	}

	return 0;
}

void
nm_os_generic_set_features(struct netmap_generic_adapter *gna)
{
	gna->rxsg = 1; /* Supported through skb_copy_bits(). */
	gna->txqdisc = netmap_generic_txqdisc;
}
#endif /* WITH_GENERIC */

#ifdef WITH_STACK
u_int nm_os_hw_headroom(struct ifnet *ifp)
{
	return LL_RESERVED_SPACE(ifp) - ifp->hard_header_len;
}

/* Releases stack's reference to data 
 * Releasing the slot is not my job */
void
nm_os_stackmap_mbuf_data_destructor(struct ubuf_info *uarg,
	bool zerocopy_success)
{
	struct stackmap_cb *scb;
	struct nm_os_ubuf_info *u = (struct nm_os_ubuf_info *)uarg;

	scb = container_of(u, struct stackmap_cb, ui);
	if (!(scb->slot->flags & NS_BUSY))
		D("funny, called on non NS_BUSY slot");
	scb->slot->flags &= ~NS_BUSY;
	D("scb %p", scb);
}

void
nm_os_stackmap_restore_data_ready(NM_SOCK_T *sk,
				  struct stackmap_sk_adapter *ska)
{
	sk->sk_data_ready = ska->save_sk_data_ready;
}

void
nm_os_stackmap_data_ready(NM_SOCK_T *sk)
{
	struct sk_buff_head *queue = &sk->sk_receive_queue;
	struct sk_buff *m, *tmp;
	unsigned long cpu_flags;
	u_int count = 0;

	/* XXX we should batch this lock outside the function */
	spin_lock_irqsave(&queue->lock, cpu_flags);
	skb_queue_walk_safe(queue, m, tmp) {
		struct stackmap_cb *scb = STACKMAP_CB(m);

		/* append this buffer to the scratchpad */
		__builtin_prefetch(m->head);
		scb->slot->fd = stackmap_sk(m->sk)->fd;
		scb->slot->len = skb_headlen(m);
		KASSERT(m->data - m->head <= 255, "too high offset");
		scb->slot->offset = (uint8_t)(m->data - m->head);
		stackmap_add_fdtable(scb, m->head);
		sk_eat_skb(sk, m);
		count++;
	}
	if (count > 1)
		D("eaten %u packets", count);
	spin_unlock_irqrestore(&queue->lock, cpu_flags);
	//ska->save_sk_data_ready(sk);
}

NM_SOCK_T *
nm_os_sock_fget(int fd)
{
	int err;
	struct socket *sock = sockfd_lookup(fd, &err);

	if (!sock)
		return NULL;
	return sock->sk;
}

void
nm_os_sock_fput(NM_SOCK_T *sk)
{
	sockfd_put(sk->sk_socket);
}

/* This method + kfree_skb() drops packet rates from 14.5 to 9.5 Mpps
 * at 2.8 Ghz CPU, and netif_receive_skb() to drop packet does so to
 * 6 Mpps.
 * Since we always allocate the same head size of skb, we
 * could batch allocation.
 * Anyways alloc/dealloc overhead of 200 ns is not that bad.
 */
struct mbuf *
nm_os_build_mbuf(struct netmap_adapter *na, char *buf, u_int len)
{
	struct mbuf *m;
	struct page *page;

	m = build_skb(buf, NETMAP_BUF_SIZE(na) - sizeof(struct stackmap_cb));
	if (!m)
		return NULL;
	page = virt_to_page(buf);
	get_page(page); /* survive __kfree_skb */
	ND("skb %p data %p page %p ref %d", m, buf, page, page_ref_count(page));
	m->dev = na->ifp;
	//if (na == stackmap_master(na))
	skb_reserve(m, STACKMAP_DMA_OFFSET); // m->data and tail
	skb_put(m, len - STACKMAP_DMA_OFFSET); // advance m->tail and increment m->len
	return m;
}

/* Based on the TX path from __sys_sendmsg(), sock_sendmsg_nosec() to
 * udp_sendpage()/udp_sendmsg(). Msghdr is placed right after user data.
 *
 * We first form a socket send buffer, so we can call
 * udp_push_pending_frames() which dequeues skbs, builds IP header
 * (ip_finish_skb()) then calls udp_send_skb() that finally builds
 * UDP header.
 */
	/* raw    | headroom | user data | msghdr |    tailroom    | 
	 * slot  buf        off         len
	 * skb   head       data        tail         end      shinfo
	 */
static int
stackmap_udp_sendmsg(struct mbuf *m)
{
	NM_SOCK_T *sk = m->sk;
	struct nm_msghdr *nmsg = NULL;

	struct inet_sock *inet = inet_sk(sk);
	struct udp_sock *up = udp_sk(sk);
	struct flowi4 fl4_stack;
	struct flowi4 *fl4;
	size_t ulen = m->len;
	struct ipcm_cookie ipc;
	struct rtable *rt = NULL;
	int connected = 0;
	__be32 daddr, faddr, saddr;
	__be16 dport;
	u8  tos;
	int err;
	struct ip_options_data opt_copy;
	struct inet_cork *cork = &inet_sk(sk)->cork.base;
	int hlen, missmatch;

	hlen = STACKMAP_DMA_OFFSET + ETH_HDR_LEN +
	    sizeof(struct iphdr) + sizeof(struct udphdr);

	KASSERT(cork->opt == NULL, "cork->opt is non NULL");

	/* There maybe a valid nmsg
	 * XXX Maybe optimize by skipping this in ESTABLISHED state, avoiding 
	 * touching skb->tail.
	 * XXX do better way to invalidate nmsg
	 */
	if (likely(skb_tailroom(m) >= sizeof(*nmsg))) {
		nmsg = (struct nm_msghdr *)skb_tail_pointer(m);
		/* likely() because non-connected case is anyways slow */
		/* XXX do better way to invalidate nmsg */
		if (likely(nmsg->nmsg_namelen < sizeof(struct sockaddr_in *)))
			nmsg = NULL;
	}

	/* we expect the user embedded msg after data */
	if (skb_end_pointer(m) - skb_tail_pointer(m) < sizeof(*nmsg))
		return EINVAL;
	if (sk->sk_state != TCP_ESTABLISHED) /* XXX do better */
		nmsg = (struct nm_msghdr *)(skb_tail_pointer(m));

	ipc.opt = NULL;
	ipc.tx_flags = 0;
	ipc.ttl = 0;
	ipc.tos = -1;

	KASSERT(inet != NULL, "no inet");

	fl4 = &inet->cork.fl.u.ip4;

	/* no pending data case so far, but maybe needed */

	ulen += sizeof(struct udphdr);

	if (nmsg) {
		struct sockaddr_in *sin =
			(struct sockaddr_in *)nmsg->nmsg_name;
		if (sin->sin_family != AF_INET) {
			return -EAFNOSUPPORT;
		}
		daddr = sin->sin_addr.s_addr;
		dport = sin->sin_port;
		if (dport == 0) {
			return -EINVAL;
		}
	} else { /* no connected socket support so far */
		if (sk->sk_state != TCP_ESTABLISHED)
			return -EDESTADDRREQ;
		daddr = inet->inet_daddr;
		dport = inet->inet_dport;
		connected = 1;
	}
#ifdef NETMAP_LINUX_HAVE_SO_TIMESTAMPING /* sockc member */
	ipc.sockc.tsflags = sk->sk_tsflags;
#endif /* NETMAP_LINUX_HAVE_SO_TIMESTAMPING */
	ipc.addr = inet->inet_saddr;
	ipc.oif = sk->sk_bound_dev_if;

	/* no control msg support so far */

	if (!ipc.opt) { /* always true */
		struct ip_options_rcu *inet_opt;

		rcu_read_lock();
		inet_opt = rcu_dereference(inet->inet_opt);
		if (inet_opt) {
			memcpy(&opt_copy, inet_opt,
			       sizeof(*inet_opt) + inet_opt->opt.optlen);
			ipc.opt = &opt_copy.opt;
			hlen += inet_opt->opt.optlen;
		}
		rcu_read_unlock();
	}

	saddr = ipc.addr;
	ipc.addr = faddr = daddr;

#ifdef NETMAP_LINUX_HAVE_SO_TIMESTAMPING
	sock_tx_timestamp(sk, ipc.sockc.tsflags, &ipc.tx_flags);
#endif /* NETMAP_LINUX_HAVE_SO_TIMESTAMPING */

	if (ipc.opt && ipc.opt->opt.srr) {
		if (!daddr)
			return -EINVAL;
		faddr = ipc.opt->opt.faddr;
	}
	tos = get_rttos(&ipc, inet);
	if (sock_flag(sk, SOCK_LOCALROUTE) ||
	    (ipc.opt && ipc.opt->opt.is_strictroute)) {
		tos |= RTO_ONLINK;
	}
	/* no multicast */
	if (!ipc.oif)
		ipc.oif = inet->uc_index;
	/* route lookup */
	if (connected) {
		rt = (struct rtable *)sk_dst_check(sk, 0);
		ND("connected %p", rt);
	}
	if (!rt) {
		struct net *net = sock_net(sk);
		__u8 flow_flags = inet_sk_flowi_flags(sk);

		fl4 = &fl4_stack;

		flowi4_init_output(fl4, ipc.oif, sk->sk_mark, tos,
				   RT_SCOPE_UNIVERSE, sk->sk_protocol,
				   flow_flags,
				   faddr, saddr, dport, inet->inet_sport);

		/* XXX l3mdev_get_saddr() doesn't exist in 4.9 */
		/*
		if (!saddr && ipc.oif) {
			err = l3mdev_get_saddr(net, ipc.oif, fl4);
			if (err < 0)
				goto out;
		}
		*/

		security_sk_classify_flow(sk, flowi4_to_flowi(fl4));
		rt = ip_route_output_flow(net, fl4, sk);
		if (IS_ERR(rt)) {
			D("error on ip_route_output_flow()");
			err = PTR_ERR(rt);
			rt = NULL;
			if (err == -ENETUNREACH)
				IP_INC_STATS(net, IPSTATS_MIB_OUTNOROUTES);
			goto out;
		}

		/* XXX Drop packets not going to a netmap port */
		if (!NM_NA_VALID(rt->dst.dev)) {
			D("output if %s is not netmap mode", rt->dst.dev->name);
			err = -EINVAL;
			goto out;
		}

		err = -EACCES;
		if ((rt->rt_flags & RTCF_BROADCAST) &&
		    !sock_flag(sk, SOCK_BROADCAST))
			goto out;
	}

	saddr = fl4->saddr;
	if (!ipc.addr)
		daddr = ipc.addr = fl4->daddr;
	/* Don't do lockless fastpath which alloc skb
	 * But be careful that the original udp_sendmsg() assumes
	 * non-corkreq case finishes here, never reaching the
	 * following if statement
	 */

	/* why do we need this lock ? anyways, batch later */
	lock_sock(sk);

	fl4 = &inet->cork.fl.u.ip4;
	fl4->daddr = daddr;
	fl4->saddr = saddr;
	fl4->fl4_dport = dport;
	fl4->fl4_sport = inet->inet_sport;
	up->pending = AF_INET;

	up->len += ulen;

	/*
	 * Here is the main difference from udp_sendmsg().
	 * We already have skb with user data, do equivalent to
	 * ip_append_data() without skb allocation
	 */
	if (!skb_queue_empty(&sk->sk_write_queue))
		D("queue is not empty");

	m->ip_summed = CHECKSUM_NONE;

	/* remember: data already point to user data */
	missmatch = hlen - skb_headroom(m);
	if (missmatch) {
		RD(1, "copy data for %d-byte extra headroom", missmatch);
		if (missmatch > 0 && skb_tailroom(m) < missmatch) {
			D("not enough tailroom %d", skb_tailroom(m));
			release_sock(sk);
			return EINVAL;
		}
		memcpy(m->data + missmatch, m->data, m->len);
		skb_reserve(m, missmatch);
		/* no need to shift shinfo and msghdr */
	}

	/*
	 * Before set_network_header,
	 * skb->data must point the beginning of IP header (see hh_len)
	 * and skb->tail must point the end of data
	 */
	skb_push(m, sizeof(struct udphdr) + sizeof(struct iphdr));
	skb_set_network_header(m, 0);
	m->transport_header =
		(m->network_header + sizeof(struct iphdr));
	/* find where to start putting bytes */

	cork->dst = &rt->dst;
	rt = NULL; /* emulate ip_append_data() steals reference */
	/* enqueuing to socket needed for subsequent ip_finish_skb()
	 * called in udp_push_pending_frames()
	 */
	__skb_queue_tail(&sk->sk_write_queue, m);

	/* XXX make sure that queued packet might be acked
	 * before being stackmap_enqueued */
	udp_push_pending_frames(sk);

	/* postpone push_pending_frames for TCP compatibility */
	release_sock(sk);
out:
	ip_rt_put(rt);
	return 0;
}

void
nm_os_stackmap_mbuf_recv(struct mbuf *m)
{
	skb_put(m, STACKMAP_CB(m)->kring->na->virt_hdr_len);
	m->protocol = eth_type_trans(m, m->dev);
	if (ntohs(m->protocol) == 0x0806)
		D("ARP");
	netif_receive_skb(m);
}

int
nm_os_stackmap_sendpage(struct netmap_adapter *na, struct netmap_slot *slot)
{
	struct stackmap_sk_adapter *ska;
	struct stackmap_cb *scb;
	struct page *page;
	u_int poff, len;
	NM_SOCK_T *sk;
	void *nmb = NMB(na, slot);
	int err;

	ska = stackmap_ska_from_fd(na, slot->fd);
	if (!ska) {
		D("no ska for fd %d (na %s)", slot->fd, na->name);
		return 0;
	}
	sk = ska->sk;

	page = virt_to_page(nmb);
	get_page(page); // survive __kfree_skb()
	poff = nmb - page_to_virt(page) + na->virt_hdr_len + slot->offset;
	len = slot->len - na->virt_hdr_len - slot->offset;
	scb = STACKMAP_CB_NMB(nmb, NETMAP_BUF_SIZE(na));
	stackmap_cb_set_state(scb, SCB_M_SENDPAGE);
	ND("slot %d sk %p fd %d nmb %p scb %p (flag 0x%08x) pageoff %u",
		(int)(slot - scb->kring->ring->slot), sk,
		ska->fd, nmb, scb, scb->flags, poff);

	/* let the stack to manage the buffer */
	slot->flags |= NS_BUSY;
	err = sk->sk_prot->sendpage(sk, page, poff, len, 0);
	if (err < 0) {
		/* Treat as if this buffer is consumed and hope mbuf
		 * has been freed.
		 * mbuf hasn't reached ndo_start_xmit() that sets ubuf 
		 * destructor. So we clear NS_BUSY here. Duplicate clear
		 * isn't a problem.
		 */
		D("error %d in sendpage() slot %d",
				err, slot - scb->kring->ring->slot);
		bzero(scb, sizeof(*scb));
		if (slot->flags & NS_BUSY)
			D("Weird, NS_BUSY on sendpage() error. Clear anyways");
		slot->flags &= ~NS_BUSY;
	}

	/* Didn't reach ndo_start_xmit() */
	if (stackmap_cb_get_state(scb) == SCB_M_SENDPAGE) {
		stackmap_cb_set_state(scb, SCB_M_QUEUED);
		/* NS_BUSY is also transferred */
		if (stackmap_extra_enqueue(na, slot)) {
			ND("no extra space for nmb %p slot %p scb %p", nmb, scb->slot, scb);
			return -EBUSY;
		}
		D("enqueued nmb %p to now this slot is at %p scb %p", nmb, scb->slot, scb);
	}
	return 0;
}

#endif /* WITH_STACK */

/* Use ethtool to find the current NIC rings lengths, so that the netmap
   rings can have the same lengths. */
int
nm_os_generic_find_num_desc(struct ifnet *ifp, unsigned int *tx, unsigned int *rx)
{
	int error = EOPNOTSUPP;
#ifdef NETMAP_LINUX_HAVE_GET_RINGPARAM
	struct ethtool_ringparam rp;

	if (ifp->ethtool_ops && ifp->ethtool_ops->get_ringparam) {
		ifp->ethtool_ops->get_ringparam(ifp, &rp);
		*tx = rp.tx_pending ? rp.tx_pending : rp.tx_max_pending;
		*rx = rp.rx_pending ? rp.rx_pending : rp.rx_max_pending;
		if (*rx < 3) {
			D("Invalid RX ring size %u, using default", *rx);
			*rx = netmap_generic_ringsize;
		}
		if (*tx < 3) {
			D("Invalid TX ring size %u, using default", *tx);
			*tx = netmap_generic_ringsize;
		}
		error = 0;
	}
#endif /* HAVE_GET_RINGPARAM */
	return error;
}

/* Fills in the output arguments with the number of hardware TX/RX queues. */
void
nm_os_generic_find_num_queues(struct ifnet *ifp, u_int *txq, u_int *rxq)
{
#ifdef NETMAP_LINUX_HAVE_SET_CHANNELS
	struct ethtool_channels ch;
	memset(&ch, 0, sizeof(ch));
	if (ifp->ethtool_ops && ifp->ethtool_ops->get_channels) {
		ifp->ethtool_ops->get_channels(ifp, &ch);
		*txq = ch.tx_count ? ch.tx_count : ch.combined_count;
		*rxq = ch.rx_count ? ch.rx_count : ch.combined_count;
	} else
#endif /* HAVE_SET_CHANNELS */
	{
		*txq = *rxq = ifp->real_num_tx_queues;
#if defined(NETMAP_LINUX_HAVE_REAL_NUM_RX_QUEUES)
		*rxq = ifp->real_num_rx_queues;
#endif /* HAVE_REAL_NUM_RX_QUEUES */
	}
}

int
netmap_linux_config(struct netmap_adapter *na,
		u_int *txr, u_int *txd, u_int *rxr, u_int *rxd)
{
	struct ifnet *ifp = na->ifp;
	int error = 0;

	rtnl_lock();

	if (ifp == NULL) {
		D("zombie adapter");
		error = ENXIO;
		goto out;
	}
	error = nm_os_generic_find_num_desc(ifp, txd, rxd);
	if (error)
		goto out;
	nm_os_generic_find_num_queues(ifp, txr, rxr);

out:
	rtnl_unlock();

	return error;
}


/* ######################## FILE OPERATIONS ####################### */

struct net_device *
ifunit_ref(const char *name)
{
#ifndef NETMAP_LINUX_HAVE_INIT_NET
	return dev_get_by_name(name);
#else
	void *ns = &init_net;
#ifdef CONFIG_NET_NS
	ns = current->nsproxy->net_ns;
#endif
	return dev_get_by_name(ns, name);
#endif
}

void if_ref(struct net_device *ifp)
{
	dev_hold(ifp);
}

void if_rele(struct net_device *ifp)
{
	dev_put(ifp);
}

struct nm_linux_selrecord_t {
	struct file *file;
	struct poll_table_struct *pwait;
};

/*
 * Remap linux arguments into the FreeBSD call.
 * - pwait is the poll table, passed as 'dev';
 *   If pwait == NULL someone else already woke up before. We can report
 *   events but they are filtered upstream.
 *   If pwait != NULL, then pwait->key contains the list of events.
 * - events is computed from pwait as above.
 * - file is passed as 'td';
 */
static u_int
linux_netmap_poll(struct file * file, struct poll_table_struct *pwait)
{
#ifdef NETMAP_LINUX_PWAIT_KEY
	int events = pwait ? pwait->NETMAP_LINUX_PWAIT_KEY : \
		     POLLIN | POLLOUT | POLLERR;
#else
	int events = POLLIN | POLLOUT; /* XXX maybe... */
#endif /* PWAIT_KEY */
	struct nm_linux_selrecord_t sr = {
		.file = file,
		.pwait = pwait
	};
	struct netmap_priv_d *priv = file->private_data;
	return netmap_poll(priv, events, &sr);
}

static int
#ifdef NETMAP_LINUX_HAVE_FAULT_VMA_ARG
linux_netmap_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
#else
linux_netmap_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#endif /* NETMAP_LINUX_HAVE_FAULT_VMA_ARG */
	struct netmap_priv_d *priv = vma->vm_private_data;
	struct netmap_adapter *na = priv->np_na;
	struct page *page;
	unsigned long off = (vma->vm_pgoff + vmf->pgoff) << PAGE_SHIFT;
	unsigned long pa, pfn;

	pa = netmap_mem_ofstophys(na->nm_mem, off);
	ND("fault off %lx -> phys addr %lx", off, pa);
	if (pa == 0)
		return VM_FAULT_SIGBUS;
	pfn = pa >> PAGE_SHIFT;
	if (!pfn_valid(pfn))
		return VM_FAULT_SIGBUS;
	page = pfn_to_page(pfn);
	get_page(page);
	vmf->page = page;
	return 0;
}

static struct vm_operations_struct linux_netmap_mmap_ops = {
	.fault = linux_netmap_fault,
};

static int
linux_netmap_mmap(struct file *f, struct vm_area_struct *vma)
{
	int error = 0;
	unsigned long off;
	u_int memsize, memflags;
	struct netmap_priv_d *priv = f->private_data;
	struct netmap_adapter *na = priv->np_na;
	/*
	 * vma->vm_start: start of mapping user address space
	 * vma->vm_end: end of the mapping user address space
	 * vma->vm_pfoff: offset of first page in the device
	 */

	if (priv->np_nifp == NULL) {
		return -EINVAL;
	}
	mb();

	/* check that [off, off + vsize) is within our memory */
	error = netmap_mem_get_info(na->nm_mem, &memsize, &memflags, NULL);
	ND("get_info returned %d", error);
	if (error)
		return -error;
	off = vma->vm_pgoff << PAGE_SHIFT;
	ND("off %lx size %lx memsize %x", off,
			(vma->vm_end - vma->vm_start), memsize);
	if (off + (vma->vm_end - vma->vm_start) > memsize)
		return -EINVAL;
	if (memflags & NETMAP_MEM_EXT)
		return -ENODEV;
	if (memflags & NETMAP_MEM_IO) {
		vm_ooffset_t pa;

		/* the underlying memory is contiguous */
		pa = netmap_mem_ofstophys(na->nm_mem, 0);
		if (pa == 0)
			return -EINVAL;
		return remap_pfn_range(vma, vma->vm_start,
				pa >> PAGE_SHIFT,
				vma->vm_end - vma->vm_start,
				vma->vm_page_prot);
	} else {
		/* non contiguous memory, we serve
		 * page faults as they come
		 */
		vma->vm_private_data = priv;
		vma->vm_ops = &linux_netmap_mmap_ops;
	}
	return 0;
}


/*
 * This one is probably already protected by the netif lock XXX
 */
netdev_tx_t
linux_netmap_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	netmap_transmit(dev, skb);
	return (NETDEV_TX_OK);
}

/* while in netmap mode, we cannot tolerate any change in the
 * number of rx/tx rings and descriptors
 */
int
linux_netmap_set_ringparam(struct net_device *dev,
	struct ethtool_ringparam *e)
{
	return -EBUSY;
}

#ifdef NETMAP_LINUX_HAVE_SET_CHANNELS
int
linux_netmap_set_channels(struct net_device *dev,
	struct ethtool_channels *e)
{
	return -EBUSY;
}
#endif


#ifndef NETMAP_LINUX_HAVE_UNLOCKED_IOCTL
#define LIN_IOCTL_NAME	.ioctl
static int
linux_netmap_ioctl(struct inode *inode, struct file *file, u_int cmd, u_long data /* arg */)
#else
#define LIN_IOCTL_NAME	.unlocked_ioctl
static long
linux_netmap_ioctl(struct file *file, u_int cmd, u_long data /* arg */)
#endif
{
	struct netmap_priv_d *priv = file->private_data;
	int ret = 0;
	union {
		struct nm_ifreq ifr;
		struct nmreq nmr;
	} arg;
	size_t argsize = 0;

	switch (cmd) {
	case NIOCTXSYNC:
	case NIOCRXSYNC:
		break;
	case NIOCCONFIG:
		argsize = sizeof(arg.ifr);
		break;
	default:
		argsize = sizeof(arg.nmr);
		break;
	}
	if (argsize) {
		if (!data)
			return -EINVAL;
		bzero(&arg, argsize);
		if (copy_from_user(&arg, (void *)data, argsize) != 0)
			return -EFAULT;
	}
	ret = netmap_ioctl(priv, cmd, (caddr_t)&arg, NULL);
	if (data && copy_to_user((void*)data, &arg, argsize) != 0)
		return -EFAULT;
	return -ret;
}

#ifdef CONFIG_COMPAT
#include <asm/compat.h>

static long
linux_netmap_compat_ioctl(struct file *file, unsigned int cmd,
                          unsigned long arg)
{
    return linux_netmap_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static int
linux_netmap_release(struct inode *inode, struct file *file)
{
	(void)inode;	/* UNUSED */
	if (file->private_data)
		netmap_dtor(file->private_data);
	return (0);
}


static int
linux_netmap_open(struct inode *inode, struct file *file)
{
	struct netmap_priv_d *priv;
	int error;
	(void)inode;	/* UNUSED */

	NMG_LOCK();
	priv = netmap_priv_new();
	if (priv == NULL) {
		error = -ENOMEM;
		goto out;
	}
	file->private_data = priv;
out:
	NMG_UNLOCK();

	return (0);
}


static struct file_operations netmap_fops = {
    .owner = THIS_MODULE,
    .open = linux_netmap_open,
    .mmap = linux_netmap_mmap,
    LIN_IOCTL_NAME = linux_netmap_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = linux_netmap_compat_ioctl,
#endif
    .poll = linux_netmap_poll,
    .release = linux_netmap_release,
};


#ifdef WITH_VALE
#ifdef CONFIG_NET_NS
#include <net/netns/generic.h>

int netmap_bns_id;

struct netmap_bns {
	struct net *net;
	struct nm_bridge *bridges;
	u_int num_bridges;
};

#ifdef NETMAP_LINUX_HAVE_PERNET_OPS_ID
static int
nm_bns_create(struct net *net, struct netmap_bns **ns)
{
	*ns = net_generic(net, netmap_bns_id);
	return 0;
}
#define nm_bns_destroy(_1, _2)
#else
static int
nm_bns_create(struct net *net, struct netmap_bns **ns)
{
	int error = 0;

	*ns = kmalloc(sizeof(*ns), GFP_KERNEL);
	if (!*ns)
		return -ENOMEM;

	error = net_assign_generic(net, netmap_bns_id, *ns);
	if (error) {
		kfree(*ns);
		*ns = NULL;
	}
	return error;
}

void
nm_bns_destroy(struct net *net, struct netmap_bns *ns)
{
	kfree(ns);
	net_assign_generic(net, netmap_bns_id, NULL);
}
#endif

struct net*
netmap_bns_get(void)
{
	return get_net(current->nsproxy->net_ns);
}

void
netmap_bns_put(struct net *net_ns)
{
	put_net(net_ns);
}

void
netmap_bns_getbridges(struct nm_bridge **b, u_int *n)
{
	struct net *net_ns = current->nsproxy->net_ns;
	struct netmap_bns *ns = net_generic(net_ns, netmap_bns_id);

	*b = ns->bridges;
	*n = ns->num_bridges;
}

static int __net_init
netmap_pernet_init(struct net *net)
{
	struct netmap_bns *ns;
	int error = 0;

	error = nm_bns_create(net, &ns);
	if (error)
		return error;

	ns->net = net;
	ns->num_bridges = NM_BRIDGES;
	ns->bridges = netmap_init_bridges2(ns->num_bridges);
	if (ns->bridges == NULL) {
		nm_bns_destroy(net, ns);
		return -ENOMEM;
	}

	return 0;
}

static void __net_init
netmap_pernet_exit(struct net *net)
{
	struct netmap_bns *ns = net_generic(net, netmap_bns_id);

	netmap_uninit_bridges2(ns->bridges, ns->num_bridges);
	ns->bridges = NULL;

	nm_bns_destroy(net, ns);
}

static struct pernet_operations netmap_pernet_ops = {
	.init = netmap_pernet_init,
	.exit = netmap_pernet_exit,
#ifdef NETMAP_LINUX_HAVE_PERNET_OPS_ID
	.id = &netmap_bns_id,
	.size = sizeof(struct netmap_bns),
#endif
};

static int netmap_bns_registered = 0;
int
netmap_bns_register(void)
{
	int rv;
#ifdef NETMAP_LINUX_HAVE_PERNET_OPS_ID
	rv = register_pernet_subsys(&netmap_pernet_ops);
#else
	rv = register_pernet_gen_subsys(&netmap_bns_id,
			&netmap_pernet_ops);
#endif
	netmap_bns_registered = !rv;
	return -rv;
}

void
netmap_bns_unregister(void)
{
	if (!netmap_bns_registered)
		return;
#ifdef NETMAP_LINUX_HAVE_PERNET_OPS_ID
	unregister_pernet_subsys(&netmap_pernet_ops);
#else
	unregister_pernet_gen_subsys(netmap_bns_id,
			&netmap_pernet_ops);
#endif
}
#endif /* CONFIG_NET_NS */
#endif /* WITH_VALE */

/* ##################### kthread wrapper ##################### */
#include <linux/eventfd.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>
#include <linux/poll.h>
#include <linux/kthread.h>
#include <linux/cpumask.h> /* nr_cpu_ids */

u_int
nm_os_ncpus(void)
{
	return nr_cpu_ids;
}

struct nm_kctx {
	struct mm_struct *mm;       /* to access guest memory */
	struct task_struct *worker; /* the kernel thread */
	atomic_t scheduled;         /* pending wake_up request */
	int attach_user;            /* kthread attached to user_process */
	int affinity;

	/* files to exchange notifications */
	struct file *ioevent_file;          /* notification from guest */
	struct file *irq_file;              /* notification to guest (interrupt) */
	struct eventfd_ctx *irq_ctx;

	/* poll ioeventfd to receive notification from the guest */
	poll_table poll_table;
	wait_queue_head_t *waitq_head;
	wait_queue_t waitq;

	/* worker function and parameter */
	nm_kctx_worker_fn_t worker_fn;
	void *worker_private;

	/* notify function, only needed when use_kthread == 0 */
	nm_kctx_notify_fn_t notify_fn;

	/* integer to manage multiple worker contexts */
	long type;

	/* does this kernel context use a kthread ? */
	int use_kthread;
};

void inline
nm_os_kctx_worker_wakeup(struct nm_kctx *nmk)
{
	if (!nmk->worker) {
		/* Propagate notification to the user. */
		nmk->notify_fn(nmk->worker_private);
		return;
	}

	/*
	 * There may be a race between FE and BE,
	 * which call both this function, and worker kthread,
	 * that reads ptk->scheduled.
	 *
	 * For us it is not important the counter value,
	 * but simply that it has changed since the last
	 * time the kthread saw it.
	 */
	atomic_inc(&nmk->scheduled);
	wake_up_process(nmk->worker);
}


static void
nm_kctx_poll_fn(struct file *file, wait_queue_head_t *wq_head, poll_table *pt)
{
	struct nm_kctx *nmk;

	nmk = container_of(pt, struct nm_kctx, poll_table);
	nmk->waitq_head = wq_head;
	add_wait_queue(wq_head, &nmk->waitq);
}

static int
nm_kctx_poll_wakeup(wait_queue_t *wq, unsigned mode, int sync, void *key)
{
	struct nm_kctx *nmk;

	/* We received a kick on the ioevent_file. If there is a worker,
	 * wake it up, otherwise do the work here. */

	nmk = container_of(wq, struct nm_kctx, waitq);
	if (nmk->worker) {
		nm_os_kctx_worker_wakeup(nmk);
	} else {
		nmk->worker_fn(nmk->worker_private, 0);
	}

	return 0;
}

static void inline
nm_kctx_worker_fn(struct nm_kctx *nmk)
{
	__set_current_state(TASK_RUNNING);
	nmk->worker_fn(nmk->worker_private, 1); /* work */
	if (need_resched())
		schedule();
}

static int
nm_kctx_worker(void *data)
{
	struct nm_kctx *nmk = data;
	int old_scheduled = atomic_read(&nmk->scheduled);
	int new_scheduled = old_scheduled;
	mm_segment_t oldfs = get_fs();

	if (nmk->mm) {
		set_fs(USER_DS);
		use_mm(nmk->mm);
	}

	while (!kthread_should_stop()) {
		if (!nmk->ioevent_file) {
			/*
			 * if ioevent_file is not defined, we don't have
			 * notification mechanism and we continually
			 * execute worker_fn()
			 */
			nm_kctx_worker_fn(nmk);

		} else {
			/*
			 * Set INTERRUPTIBLE state before to check if there
			 * is work. If wake_up() is called, although we have
			 * not seen the new counter value, the kthread state
			 * is set to RUNNING and after schedule() it is not
			 * moved off run queue.
			 */
			set_current_state(TASK_INTERRUPTIBLE);

			new_scheduled = atomic_read(&nmk->scheduled);

			/* check if there is a pending notification */
			if (likely(new_scheduled != old_scheduled)) {
				old_scheduled = new_scheduled;
				nm_kctx_worker_fn(nmk);
			} else {
				schedule();
			}
		}
	}

	__set_current_state(TASK_RUNNING);

	if (nmk->mm) {
		unuse_mm(nmk->mm);
	}

	set_fs(oldfs);
	return 0;
}

void inline
nm_os_kctx_send_irq(struct nm_kctx *nmk)
{
	if (nmk->irq_ctx) {
		eventfd_signal(nmk->irq_ctx, 1);
	}
}

static void
nm_kctx_close_files(struct nm_kctx *nmk)
{
	if (nmk->ioevent_file) {
		fput(nmk->ioevent_file);
		nmk->ioevent_file = NULL;
	}

	if (nmk->irq_file) {
		fput(nmk->irq_file);
		nmk->irq_file = NULL;
		eventfd_ctx_put(nmk->irq_ctx);
		nmk->irq_ctx = NULL;
	}
}

static int
nm_kctx_open_files(struct nm_kctx *nmk, void *opaque)
{
	struct file *file;
	struct ptnetmap_cfgentry_qemu *ring_cfg = opaque;

	nmk->ioevent_file = NULL;
	nmk->irq_file = NULL;

	if (!opaque) {
		return 0;
	}

	if (ring_cfg->ioeventfd) {
		file = eventfd_fget(ring_cfg->ioeventfd);
		if (IS_ERR(file))
			goto err;
		nmk->ioevent_file = file;
	}

	if (ring_cfg->irqfd) {
		file = eventfd_fget(ring_cfg->irqfd);
		if (IS_ERR(file))
			goto err;
		nmk->irq_file = file;
		nmk->irq_ctx = eventfd_ctx_fileget(file);
	}

	return 0;

err:
	nm_kctx_close_files(nmk);
	return -PTR_ERR(file);
}

static void
nm_kctx_init_poll(struct nm_kctx *nmk)
{
	init_waitqueue_func_entry(&nmk->waitq, nm_kctx_poll_wakeup);
	init_poll_funcptr(&nmk->poll_table, nm_kctx_poll_fn);
}

static int
nm_kctx_start_poll(struct nm_kctx *nmk)
{
	unsigned long mask;
	int ret = 0;

	if (nmk->waitq_head)
		return 0;

	mask = nmk->ioevent_file->f_op->poll(nmk->ioevent_file,
					     &nmk->poll_table);
	if (mask)
		nm_kctx_poll_wakeup(&nmk->waitq, 0, 0, (void *)mask);
	if (mask & POLLERR) {
		if (nmk->waitq_head)
			remove_wait_queue(nmk->waitq_head, &nmk->waitq);
		ret = EINVAL;
	}

	return ret;
}

static void
nm_kctx_stop_poll(struct nm_kctx *nmk)
{
	if (nmk->waitq_head) {
		remove_wait_queue(nmk->waitq_head, &nmk->waitq);
		nmk->waitq_head = NULL;
	}
}

void
nm_os_kctx_worker_setaff(struct nm_kctx *nmk, int affinity)
{
	nmk->affinity = affinity;
}

struct nm_kctx *
nm_os_kctx_create(struct nm_kctx_cfg *cfg, unsigned int cfgtype,
		     void *opaque)
{
	struct nm_kctx *nmk = NULL;
	int error;

	if (cfgtype != PTNETMAP_CFGTYPE_QEMU) {
		D("Unsupported cfgtype %u", cfgtype);
		return NULL;
	}

	if (!cfg->use_kthread && cfg->notify_fn == NULL) {
		D("Error: botify function missing with use_htead == 0");
		return NULL;
	}

	nmk = kzalloc(sizeof *nmk, GFP_KERNEL);
	if (!nmk)
		return NULL;

	nmk->worker_fn = cfg->worker_fn;
	nmk->worker_private = cfg->worker_private;
	nmk->notify_fn = cfg->notify_fn;
	nmk->type = cfg->type;
	nmk->use_kthread = cfg->use_kthread;
	atomic_set(&nmk->scheduled, 0);
	nmk->attach_user = cfg->attach_user;

	/* open event fds */
	error = nm_kctx_open_files(nmk, opaque);
	if (error)
		goto err;

	nm_kctx_init_poll(nmk);

	return nmk;
err:
	kfree(nmk);
	return NULL;
}

int
nm_os_kctx_worker_start(struct nm_kctx *nmk)
{
	int error = 0;

	if (nmk->worker) {
		return EBUSY;
	}

	/* Get caller's memory mapping if needed. */
	if (nmk->attach_user) {
		nmk->mm = get_task_mm(current);
	}

	/* Run the context in a kernel thread, if needed. */
	if (nmk->use_kthread) {
		char name[16];

		snprintf(name, sizeof(name), "nmkth:%d:%ld", current->pid,
								nmk->type);
		nmk->worker = kthread_create(nm_kctx_worker, nmk, name);
		if (IS_ERR(nmk->worker)) {
			error = -PTR_ERR(nmk->worker);
			goto err;
		}

		kthread_bind(nmk->worker, nmk->affinity);
		wake_up_process(nmk->worker);
	}

	if (nmk->ioevent_file) {
		error = nm_kctx_start_poll(nmk);
		if (error) {
			goto err;
		}
	}

	return 0;

err:
	if (nmk->worker) {
		kthread_stop(nmk->worker);
		nmk->worker = NULL;
	}
	if (nmk->mm) {
		mmput(nmk->mm);
		nmk->mm = NULL;
	}
	return error;
}

void
nm_os_kctx_worker_stop(struct nm_kctx *nmk)
{
	nm_kctx_stop_poll(nmk);

	if (nmk->worker) {
		kthread_stop(nmk->worker);
		nmk->worker = NULL;
	}

	if (nmk->mm) {
		mmput(nmk->mm);
		nmk->mm = NULL;
	}
}

void
nm_os_kctx_destroy(struct nm_kctx *nmk)
{
	if (!nmk)
		return;

	if (nmk->worker) {
		nm_os_kctx_worker_stop(nmk);
	}

	nm_kctx_close_files(nmk);

	kfree(nmk);
}

/* ##################### PTNETMAP SUPPORT ##################### */
#ifdef WITH_PTNETMAP_GUEST
/*
 * ptnetmap memory device (memdev) for linux guest
 * Used to expose host memory to the guest through PCI-BAR
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>

int ptnet_probe(struct pci_dev *pdev, const struct pci_device_id *id);
void ptnet_remove(struct pci_dev *pdev);

/*
 * PCI Device ID Table
 * list of (VendorID,DeviceID) supported by this driver
 */
static struct pci_device_id ptnetmap_guest_device_table[] = {
	{ PCI_DEVICE(PTNETMAP_PCI_VENDOR_ID, PTNETMAP_PCI_DEVICE_ID), },
	{ PCI_DEVICE(PTNETMAP_PCI_VENDOR_ID, PTNETMAP_PCI_NETIF_ID), },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, ptnetmap_guest_device_table);

/*
 * ptnetmap memdev private data structure
 */
struct ptnetmap_memdev
{
	struct pci_dev *pdev;
	void __iomem *pci_io;
	void __iomem *pci_mem;
	struct netmap_mem_d *nm_mem;
	int bars;
};

/*
 * map host netmap memory through PCI-BAR in the guest OS
 *
 * return physical (nm_paddr) and virtual (nm_addr) addresses
 * of the netmap memory mapped in the guest.
 */
int
nm_os_pt_memdev_iomap(struct ptnetmap_memdev *ptn_dev, vm_paddr_t *nm_paddr,
                      void **nm_addr, uint64_t *mem_size)
{
	struct pci_dev *pdev = ptn_dev->pdev;
	phys_addr_t mem_paddr;
	int err = 0;

	*mem_size = ioread32(ptn_dev->pci_io + PTNET_MDEV_IO_MEMSIZE_HI);
	*mem_size = ioread32(ptn_dev->pci_io + PTNET_MDEV_IO_MEMSIZE_LO) |
		(*mem_size << 32);

	D("=== BAR %d start %llx len %llx mem_size %lx ===",
			PTNETMAP_MEM_PCI_BAR,
			pci_resource_start(pdev, PTNETMAP_MEM_PCI_BAR),
			pci_resource_len(pdev, PTNETMAP_MEM_PCI_BAR),
			(unsigned long)(*mem_size));

	/* map memory allocator */
	mem_paddr = pci_resource_start(pdev, PTNETMAP_MEM_PCI_BAR);
	ptn_dev->pci_mem = *nm_addr = ioremap_cache(mem_paddr, *mem_size);
	if (ptn_dev->pci_mem == NULL) {
		err = -ENOMEM;
	}
	*nm_paddr = mem_paddr;

	return err;
}

uint32_t
nm_os_pt_memdev_ioread(struct ptnetmap_memdev *ptn_dev, unsigned int reg)
{
	return ioread32(ptn_dev->pci_io + reg);
}

/*
 * unmap PCI-BAR
 */
void
nm_os_pt_memdev_iounmap(struct ptnetmap_memdev *ptn_dev)
{
	if (ptn_dev->pci_mem) {
		iounmap(ptn_dev->pci_mem);
		ptn_dev->pci_mem = NULL;
	}
}

/*
 * Device Initialization Routine
 *
 * Returns 0 on success, negative on failure
 */
static int
ptnetmap_guest_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct ptnetmap_memdev *ptn_dev;
	int bars, err;
	uint16_t mem_id;

	if (id->device == PTNETMAP_PCI_NETIF_ID) {
		/* Probe the ptnet device. */
		return ptnet_probe(pdev, id);
	}

	/* Probe the memdev device. */
	ptn_dev = kzalloc(sizeof(*ptn_dev), GFP_KERNEL);
	if (ptn_dev == NULL)
		return -ENOMEM;

	ptn_dev->pdev = pdev;
	bars = pci_select_bars(pdev, IORESOURCE_MEM | IORESOURCE_IO);
	/* enable the device */
	err = pci_enable_device(pdev);
	if (err)
		goto err;

	err = pci_request_selected_regions(pdev, bars, PTNETMAP_MEMDEV_NAME);
	if (err)
		goto err_pci_reg;

	ptn_dev->pci_io = pci_iomap(pdev, PTNETMAP_IO_PCI_BAR, 0);
	if (ptn_dev->pci_io == NULL) {
		err = -ENOMEM;
		goto err_iomap;
	}
	pci_set_drvdata(pdev, ptn_dev);
	pci_set_master(pdev); /* XXX probably not needed */

	ptn_dev->bars = bars;
	mem_id = ioread32(ptn_dev->pci_io + PTNET_MDEV_IO_MEMID);

	/* create guest allocator */
	ptn_dev->nm_mem = netmap_mem_pt_guest_attach(ptn_dev, mem_id);
	if (ptn_dev->nm_mem == NULL) {
		err = -ENOMEM;
		goto err_nmd_attach;
	}
	netmap_mem_get(ptn_dev->nm_mem);

	return 0;

err_nmd_attach:
	pci_set_drvdata(pdev, NULL);
	iounmap(ptn_dev->pci_io);
err_iomap:
	pci_release_selected_regions(pdev, bars);
err_pci_reg:
	pci_disable_device(pdev);
err:
	kfree(ptn_dev);
	return err;
}

/*
 * Device Removal Routine
 */
static void
ptnetmap_guest_remove(struct pci_dev *pdev)
{
	struct ptnetmap_memdev *ptn_dev = pci_get_drvdata(pdev);

	if (pdev->device == PTNETMAP_PCI_NETIF_ID) {
		/* Remove the ptnet device. */
		return ptnet_remove(pdev);
	}

	/* Remove the memdev device. */

	if (ptn_dev->nm_mem) {
		netmap_mem_put(ptn_dev->nm_mem);
		ptn_dev->nm_mem = NULL;
	}
	nm_os_pt_memdev_iounmap(ptn_dev);
	pci_set_drvdata(pdev, NULL);
	iounmap(ptn_dev->pci_io);
	pci_release_selected_regions(pdev, ptn_dev->bars);
	pci_disable_device(pdev);
	kfree(ptn_dev);
}

/*
 * pci driver information
 */
static struct pci_driver ptnetmap_guest_drivers = {
	.name       = "ptnetmap-guest-drivers",
	.id_table   = ptnetmap_guest_device_table,
	.probe      = ptnetmap_guest_probe,
	.remove     = ptnetmap_guest_remove,
};

/*
 * Driver Registration Routine
 *
 * Returns 0 on success, negative on failure
 */
static int
ptnetmap_guest_init(void)
{
	int ret;

	/* register pci driver */
	ret = pci_register_driver(&ptnetmap_guest_drivers);
	if (ret < 0) {
		D("Failed to register drivers");
		return ret;
	}

	return 0;
}

/*
 * Driver Exit Cleanup Routine
 */
void
ptnetmap_guest_fini(void)
{
	/* unregister pci driver */
	pci_unregister_driver(&ptnetmap_guest_drivers);
}

#else /* !WITH_PTNETMAP_GUEST */
#define ptnetmap_guest_init()		0
#define ptnetmap_guest_fini()
#endif /* WITH_PTNETMAP_GUEST */

#ifdef WITH_SINK

/*
 * An emulated netmap-enabled device acting as a packet sink, useful for
 * performance tests of netmap applications or other netmap subsystems
 * (i.e. VALE, ptnetmap).
 *
 * The sink_delay_ns parameter is used to tune the speed of the packet sink
 * device. The absolute value of the parameter is interpreted as the number
 * of nanoseconds that are required to send a packet into the sink.
 * For positive values, the sink device emulates a NIC transmitting packets
 * asynchronously with respect to the txsync() caller, similarly to what
 * happens with real NICs.
 * For negative values, the sink device emulates a packet consumer,
 * transmitting packets synchronously with respect to the txsync() caller.
 */
static int sink_delay_ns = 100;
module_param(sink_delay_ns, int, 0644);
static struct net_device *nm_sink_netdev = NULL; /* global sink netdev */
s64 nm_sink_next_link_idle; /* for link emulation */

#define NM_SINK_SLOTS	1024
#define NM_SINK_DELAY_NS \
	((unsigned int)(sink_delay_ns > 0 ? sink_delay_ns : -sink_delay_ns))

static int
nm_sink_register(struct netmap_adapter *na, int onoff)
{
	if (onoff)
		nm_set_native_flags(na);
	else
		nm_clear_native_flags(na);

	nm_sink_next_link_idle = ktime_get_ns();

	return 0;
}

static inline void
nm_sink_emu(unsigned int n)
{
	u64 wait_until = nm_sink_next_link_idle;
	u64 now = ktime_get_ns();

	if (sink_delay_ns < 0 || nm_sink_next_link_idle < now) {
		/* If we are emulating packet consumer mode or the link went
		 * idle some time ago, we need to update the link emulation
		 * variable, because we don't want the caller to accumulate
		 * credit. */
		nm_sink_next_link_idle = now;
	}
	/* Schedule new transmissions. */
	nm_sink_next_link_idle += n * NM_SINK_DELAY_NS;
	if (sink_delay_ns < 0) {
		/* In packet consumer mode we emulate synchronous
		 * transmission, so we have to wait right now for the link
		 * to become idle. */
		wait_until = nm_sink_next_link_idle;
	}
	while (ktime_get_ns() < wait_until) ;
}

static int
nm_sink_txsync(struct netmap_kring *kring, int flags)
{
	unsigned int const lim = kring->nkr_num_slots - 1;
	unsigned int const head = kring->rhead;
	unsigned int n; /* num of packets to be transmitted */

	n = kring->nkr_num_slots + head - kring->nr_hwcur;
	if (n >= kring->nkr_num_slots) {
		n -= kring->nkr_num_slots;
	}
	kring->nr_hwcur = head;
	kring->nr_hwtail = nm_prev(kring->nr_hwcur, lim);

	nm_sink_emu(n);

	return 0;
}

static int
nm_sink_rxsync(struct netmap_kring *kring, int flags)
{
	u_int const head = kring->rhead;

	/* First part: nothing received for now. */
	/* Second part: skip past packets that userspace has released */
	kring->nr_hwcur = head;

	return 0;
}

static int nm_sink_open(struct net_device *netdev) { return 0; }
static int nm_sink_close(struct net_device *netdev) { return 0; }

static netdev_tx_t
nm_sink_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	kfree_skb(skb);
	nm_sink_emu(1);
	return NETDEV_TX_OK;
}

static const struct net_device_ops nm_sink_netdev_ops = {
	.ndo_open = nm_sink_open,
	.ndo_stop = nm_sink_close,
	.ndo_start_xmit = nm_sink_start_xmit,
};

int
netmap_sink_init(void)
{
	struct netmap_adapter na;
	struct net_device *netdev;
	int err;

	netdev = alloc_etherdev(0);
	if (!netdev) {
		return ENOMEM;
	}
	netdev->netdev_ops = &nm_sink_netdev_ops ;
	strncpy(netdev->name, "nmsink", sizeof(netdev->name) - 1);
	netdev->features = NETIF_F_HIGHDMA;
	strcpy(netdev->name, "nmsink%d");
	err = register_netdev(netdev);
	if (err) {
		free_netdev(netdev);
	}

	bzero(&na, sizeof(na));
	na.ifp = netdev;
	na.num_tx_desc = NM_SINK_SLOTS;
	na.num_rx_desc = NM_SINK_SLOTS;
	na.nm_register = nm_sink_register;
	na.nm_txsync = nm_sink_txsync;
	na.nm_rxsync = nm_sink_rxsync;
	na.num_tx_rings = na.num_rx_rings = 1;
	netmap_attach(&na);

	netif_carrier_on(netdev);
	nm_sink_netdev = netdev;

	return 0;
}

void
netmap_sink_fini(void)
{
	struct net_device *netdev = nm_sink_netdev;

	nm_sink_netdev = NULL;
	unregister_netdev(netdev);
	netmap_detach(netdev);
	free_netdev(netdev);
}
#endif  /* WITH_SINK */


/* ########################## MODULE INIT ######################### */

struct miscdevice netmap_cdevsw = { /* same name as FreeBSD */
	MISC_DYNAMIC_MINOR,
	"netmap",
	&netmap_fops,
};


static int linux_netmap_init(void)
{
	int err;
	/* Errors have negative values on linux. */
	err = -netmap_init();
	if (err) {
		return err;
	}

	err = ptnetmap_guest_init();
	if (err) {
		return err;
	}
#ifdef WITH_SINK
	err = netmap_sink_init();
	if (err) {
		D("Warning: could not init netmap sink interface");
	}
#endif /* WITH_SINK */
	return 0;
}


static void linux_netmap_fini(void)
{
#ifdef WITH_SINK
	netmap_sink_fini();
#endif /* WITH_SINK */
        ptnetmap_guest_fini();
        netmap_fini();
}

#ifndef NETMAP_LINUX_HAVE_LIVE_ADDR_CHANGE
#define IFF_LIVE_ADDR_CHANGE 0
#endif

#ifndef NETMAP_LINUX_HAVE_TX_SKB_SHARING
#define IFF_TX_SKB_SHARING 0
#endif

static struct device_driver linux_dummy_drv = {.owner = THIS_MODULE};

static int linux_nm_vi_open(struct net_device *netdev)
{
	netif_start_queue(netdev);
	return 0;
}

static int linux_nm_vi_stop(struct net_device *netdev)
{
	netif_stop_queue(netdev);
	return 0;
}
static int linux_nm_vi_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	if (skb != NULL)
		kfree_skb(skb);
	return 0;
}

#ifdef NETMAP_LINUX_HAVE_GET_STATS64
static 
#ifdef NETMAP_LINUX_HAVE_NONVOID_GET_STATS64
struct rtnl_link_stats64 *
#else /* !VOID */
void
#endif /* NETMAP_LINUX_HAVE_NONVOID_GET_STATS64 */
linux_nm_vi_get_stats(struct net_device *netdev, struct rtnl_link_stats64 *stats)
{
#ifdef NETMAP_LINUX_HAVE_NONVOID_GET_STATS64
	return stats;
#endif /* !NETMAP_LINUX_HAVE_VOID_GET_STATS64 */
}
#endif /* NETMAP_LINUX_HAVE_GET_STATS64 */

static int linux_nm_vi_change_mtu(struct net_device *netdev, int new_mtu)
{
	return 0;
}
#ifdef NETMAP_LINUX_HAVE_NETDEV_DTOR
static void linux_nm_vi_destructor(struct net_device *netdev)
{
//	netmap_detach(netdev);
	free_netdev(netdev);
}
#endif
static const struct net_device_ops nm_vi_ops = {
	.ndo_open = linux_nm_vi_open,
	.ndo_stop = linux_nm_vi_stop,
	.ndo_start_xmit = linux_nm_vi_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_change_mtu = linux_nm_vi_change_mtu,
#ifdef NETMAP_LINUX_HAVE_GET_STATS64
	.ndo_get_stats64 = linux_nm_vi_get_stats,
#endif
};
/* dev->name is not initialized yet */
static void
linux_nm_vi_setup(struct ifnet *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &nm_vi_ops;
	dev->priv_flags &= ~IFF_TX_SKB_SHARING;
	dev->priv_flags |= IFF_LIVE_ADDR_CHANGE;
#ifdef NETMAP_LINUX_HAVE_NETDEV_DTOR
	dev->destructor = linux_nm_vi_destructor;
#else 
	dev->needs_free_netdev = 1;
#endif
	dev->tx_queue_len = 0;
	/* XXX */
	dev->features = NETIF_F_LLTX | NETIF_F_SG | NETIF_F_FRAGLIST |
		NETIF_F_HIGHDMA | NETIF_F_HW_CSUM | NETIF_F_TSO;
#ifdef NETMAP_LINUX_HAVE_HW_FEATURES
	dev->hw_features = dev->features & ~NETIF_F_LLTX;
#endif
#ifdef NETMAP_LINUX_HAVE_ADDR_RANDOM
	eth_hw_addr_random(dev);
#endif
}

int
nm_os_vi_persist(const char *name, struct ifnet **ret)
{
	struct ifnet *ifp;
	int error;

	if (!try_module_get(linux_dummy_drv.owner))
		return EFAULT;
#ifdef NETMAP_LINUX_ALLOC_NETDEV_4ARGS
	ifp = alloc_netdev(0, name, NET_NAME_UNKNOWN, linux_nm_vi_setup);
#else
	ifp = alloc_netdev(0, name, linux_nm_vi_setup);
#endif
	if (!ifp) {
		error = ENOMEM;
		goto err_put;
	}
	dev_net_set(ifp, &init_net);
	ifp->features |= NETIF_F_NETNS_LOCAL; /* just for safety */
	ifp->dev.driver = &linux_dummy_drv;
	error = register_netdev(ifp);
	if (error < 0) {
		D("error %d", error);
		error = -error;
		goto err_free;
	}
	netif_start_queue(ifp);
	*ret = ifp;
	return 0;

err_free:
	free_netdev(ifp);
err_put:
	module_put(linux_dummy_drv.owner);
	return error;
}

void
nm_os_vi_detach(struct ifnet *ifp)
{
	netif_stop_queue(ifp);
	unregister_netdev(ifp);
	module_put(linux_dummy_drv.owner);
}

void
nm_os_selwakeup(NM_SELINFO_T *si)
{
	/* We use wake_up_interruptible() since select() and poll()
	 * sleep in an interruptbile way. */
	wake_up_interruptible(si);
}

void
nm_os_selrecord(NM_SELRECORD_T *sr, NM_SELINFO_T *si)
{
	poll_wait(sr->file, si, sr->pwait);
}

module_init(linux_netmap_init);
module_exit(linux_netmap_fini);

/* export certain symbols to other modules */
EXPORT_SYMBOL(netmap_attach);		/* driver attach routines */
EXPORT_SYMBOL(netmap_attach_ext);
EXPORT_SYMBOL(netmap_adapter_get);
EXPORT_SYMBOL(netmap_adapter_put);
#ifdef WITH_PTNETMAP_GUEST
EXPORT_SYMBOL(netmap_pt_guest_attach);	/* ptnetmap driver attach routine */
EXPORT_SYMBOL(netmap_pt_guest_rxsync);	/* ptnetmap generic rxsync */
EXPORT_SYMBOL(netmap_pt_guest_txsync);	/* ptnetmap generic txsync */
EXPORT_SYMBOL(netmap_mem_pt_guest_ifp_del); /* unlink passthrough interface */
#endif /* WITH_PTNETMAP_GUEST */
EXPORT_SYMBOL(netmap_detach);		/* driver detach routines */
EXPORT_SYMBOL(netmap_ring_reinit);	/* ring init on error */
EXPORT_SYMBOL(netmap_reset);		/* ring init routines */
EXPORT_SYMBOL(netmap_rx_irq);	        /* default irq handler */
EXPORT_SYMBOL(netmap_no_pendintr);	/* XXX mitigation - should go away */
#ifdef WITH_VALE
EXPORT_SYMBOL(netmap_bdg_ctl);		/* bridge configuration routine */
EXPORT_SYMBOL(netmap_bdg_learning);	/* the default lookup function */
EXPORT_SYMBOL(netmap_bdg_name);		/* the bridge the vp is attached to */
#endif /* WITH_VALE */
EXPORT_SYMBOL(netmap_disable_all_rings);
EXPORT_SYMBOL(netmap_enable_all_rings);
EXPORT_SYMBOL(netmap_krings_create);
EXPORT_SYMBOL(netmap_krings_delete);	/* used by veth module */
EXPORT_SYMBOL(netmap_mem_rings_create);	/* used by veth module */
EXPORT_SYMBOL(netmap_mem_rings_delete);	/* used by veth module */
#ifdef WITH_PIPES
EXPORT_SYMBOL(netmap_pipe_txsync);	/* used by veth module */
EXPORT_SYMBOL(netmap_pipe_rxsync);	/* used by veth module */
#endif /* WITH_PIPES */
EXPORT_SYMBOL(netmap_verbose);

MODULE_AUTHOR("http://info.iet.unipi.it/~luigi/netmap/");
MODULE_DESCRIPTION("The netmap packet I/O framework");
MODULE_LICENSE("Dual BSD/GPL"); /* the code here is all BSD. */
