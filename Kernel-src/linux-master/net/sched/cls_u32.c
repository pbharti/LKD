/*
 * net/sched/cls_u32.c	Ugly (or Universal) 32bit key Packet Classifier.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *
 *	The filters are packed to hash tables of key nodes
 *	with a set of 32bit key/mask pairs at every node.
 *	Nodes reference next level hash tables etc.
 *
 *	This scheme is the best universal classifier I managed to
 *	invent; it is not super-fast, but it is not slow (provided you
 *	program it correctly), and general enough.  And its relative
 *	speed grows as the number of rules becomes larger.
 *
 *	It seems that it represents the best middle point between
 *	speed and manageability both by human and by machine.
 *
 *	It is especially useful for link sharing combined with QoS;
 *	pure RSVP doesn't need such a general approach and can use
 *	much simpler (and faster) schemes, sort of cls_rsvp.c.
 *
 *	JHS: We should remove the CONFIG_NET_CLS_IND from here
 *	eventually when the meta match extension is made available
 *
 *	nfmark match added by Catalin(ux aka Dino) BOIE <catab at umbrella.ro>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/percpu.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/bitmap.h>
#include <linux/netdevice.h>
#include <linux/hash.h>
#include <net/netlink.h>
#include <net/act_api.h>
#include <net/pkt_cls.h>
#include <linux/netdevice.h>
#include <linux/idr.h>

struct tc_u_knode {
	struct tc_u_knode __rcu	*next;
	u32			handle;
	struct tc_u_hnode __rcu	*ht_up;
	struct tcf_exts		exts;
#ifdef CONFIG_NET_CLS_IND
	int			ifindex;
#endif
	u8			fshift;
	struct tcf_result	res;
	struct tc_u_hnode __rcu	*ht_down;
#ifdef CONFIG_CLS_U32_PERF
	struct tc_u32_pcnt __percpu *pf;
#endif
	u32			flags;
#ifdef CONFIG_CLS_U32_MARK
	u32			val;
	u32			mask;
	u32 __percpu		*pcpu_success;
#endif
	struct tcf_proto	*tp;
	union {
		struct work_struct	work;
		struct rcu_head		rcu;
	};
	/* The 'sel' field MUST be the last field in structure to allow for
	 * tc_u32_keys allocated at end of structure.
	 */
	struct tc_u32_sel	sel;
};

struct tc_u_hnode {
	struct tc_u_hnode __rcu	*next;
	u32			handle;
	u32			prio;
	struct tc_u_common	*tp_c;
	int			refcnt;
	unsigned int		divisor;
	struct idr		handle_idr;
	struct rcu_head		rcu;
	/* The 'ht' field MUST be the last field in structure to allow for
	 * more entries allocated at end of structure.
	 */
	struct tc_u_knode __rcu	*ht[1];
};

struct tc_u_common {
	struct tc_u_hnode __rcu	*hlist;
	struct tcf_block	*block;
	int			refcnt;
	struct idr		handle_idr;
	struct hlist_node	hnode;
	struct rcu_head		rcu;
};

static inline unsigned int u32_hash_fold(__be32 key,
					 const struct tc_u32_sel *sel,
					 u8 fshift)
{
	unsigned int h = ntohl(key & sel->hmask) >> fshift;

	return h;
}

static int u32_classify(struct sk_buff *skb, const struct tcf_proto *tp,
			struct tcf_result *res)
{
	struct {
		struct tc_u_knode *knode;
		unsigned int	  off;
	} stack[TC_U32_MAXDEPTH];

	struct tc_u_hnode *ht = rcu_dereference_bh(tp->root);
	unsigned int off = skb_network_offset(skb);
	struct tc_u_knode *n;
	int sdepth = 0;
	int off2 = 0;
	int sel = 0;
#ifdef CONFIG_CLS_U32_PERF
	int j;
#endif
	int i, r;

next_ht:
	n = rcu_dereference_bh(ht->ht[sel]);

next_knode:
	if (n) {
		struct tc_u32_key *key = n->sel.keys;

#ifdef CONFIG_CLS_U32_PERF
		__this_cpu_inc(n->pf->rcnt);
		j = 0;
#endif

		if (tc_skip_sw(n->flags)) {
			n = rcu_dereference_bh(n->next);
			goto next_knode;
		}

#ifdef CONFIG_CLS_U32_MARK
		if ((skb->mark & n->mask) != n->val) {
			n = rcu_dereference_bh(n->next);
			goto next_knode;
		} else {
			__this_cpu_inc(*n->pcpu_success);
		}
#endif

		for (i = n->sel.nkeys; i > 0; i--, key++) {
			int toff = off + key->off + (off2 & key->offmask);
			__be32 *data, hdata;

			if (skb_headroom(skb) + toff > INT_MAX)
				goto out;

			data = skb_header_pointer(skb, toff, 4, &hdata);
			if (!data)
				goto out;
			if ((*data ^ key->val) & key->mask) {
				n = rcu_dereference_bh(n->next);
				goto next_knode;
			}
#ifdef CONFIG_CLS_U32_PERF
			__this_cpu_inc(n->pf->kcnts[j]);
			j++;
#endif
		}

		ht = rcu_dereference_bh(n->ht_down);
		if (!ht) {
check_terminal:
			if (n->sel.flags & TC_U32_TERMINAL) {

				*res = n->res;
#ifdef CONFIG_NET_CLS_IND
				if (!tcf_match_indev(skb, n->ifindex)) {
					n = rcu_dereference_bh(n->next);
					goto next_knode;
				}
#endif
#ifdef CONFIG_CLS_U32_PERF
				__this_cpu_inc(n->pf->rhit);
#endif
				r = tcf_exts_exec(skb, &n->exts, res);
				if (r < 0) {
					n = rcu_dereference_bh(n->next);
					goto next_knode;
				}

				return r;
			}
			n = rcu_dereference_bh(n->next);
			goto next_knode;
		}

		/* PUSH */
		if (sdepth >= TC_U32_MAXDEPTH)
			goto deadloop;
		stack[sdepth].knode = n;
		stack[sdepth].off = off;
		sdepth++;

		ht = rcu_dereference_bh(n->ht_down);
		sel = 0;
		if (ht->divisor) {
			__be32 *data, hdata;

			data = skb_header_pointer(skb, off + n->sel.hoff, 4,
						  &hdata);
			if (!data)
				goto out;
			sel = ht->divisor & u32_hash_fold(*data, &n->sel,
							  n->fshift);
		}
		if (!(n->sel.flags & (TC_U32_VAROFFSET | TC_U32_OFFSET | TC_U32_EAT)))
			goto next_ht;

		if (n->sel.flags & (TC_U32_OFFSET | TC_U32_VAROFFSET)) {
			off2 = n->sel.off + 3;
			if (n->sel.flags & TC_U32_VAROFFSET) {
				__be16 *data, hdata;

				data = skb_header_pointer(skb,
							  off + n->sel.offoff,
							  2, &hdata);
				if (!data)
					goto out;
				off2 += ntohs(n->sel.offmask & *data) >>
					n->sel.offshift;
			}
			off2 &= ~3;
		}
		if (n->sel.flags & TC_U32_EAT) {
			off += off2;
			off2 = 0;
		}

		if (off < skb->len)
			goto next_ht;
	}

	/* POP */
	if (sdepth--) {
		n = stack[sdepth].knode;
		ht = rcu_dereference_bh(n->ht_up);
		off = stack[sdepth].off;
		goto check_terminal;
	}
out:
	return -1;

deadloop:
	net_warn_ratelimited("cls_u32: dead loop\n");
	return -1;
}

static struct tc_u_hnode *u32_lookup_ht(struct tc_u_common *tp_c, u32 handle)
{
	struct tc_u_hnode *ht;

	for (ht = rtnl_dereference(tp_c->hlist);
	     ht;
	     ht = rtnl_dereference(ht->next))
		if (ht->handle == handle)
			break;

	return ht;
}

static struct tc_u_knode *u32_lookup_key(struct tc_u_hnode *ht, u32 handle)
{
	unsigned int sel;
	struct tc_u_knode *n = NULL;

	sel = TC_U32_HASH(handle);
	if (sel > ht->divisor)
		goto out;

	for (n = rtnl_dereference(ht->ht[sel]);
	     n;
	     n = rtnl_dereference(n->next))
		if (n->handle == handle)
			break;
out:
	return n;
}


static void *u32_get(struct tcf_proto *tp, u32 handle)
{
	struct tc_u_hnode *ht;
	struct tc_u_common *tp_c = tp->data;

	if (TC_U32_HTID(handle) == TC_U32_ROOT)
		ht = rtnl_dereference(tp->root);
	else
		ht = u32_lookup_ht(tp_c, TC_U32_HTID(handle));

	if (!ht)
		return NULL;

	if (TC_U32_KEY(handle) == 0)
		return ht;

	return u32_lookup_key(ht, handle);
}

static u32 gen_new_htid(struct tc_u_common *tp_c, struct tc_u_hnode *ptr)
{
	unsigned long idr_index;
	int err;

	/* This is only used inside rtnl lock it is safe to increment
	 * without read _copy_ update semantics
	 */
	err = idr_alloc_ext(&tp_c->handle_idr, ptr, &idr_index,
			    1, 0x7FF, GFP_KERNEL);
	if (err)
		return 0;
	return (u32)(idr_index | 0x800) << 20;
}

static struct hlist_head *tc_u_common_hash;

#define U32_HASH_SHIFT 10
#define U32_HASH_SIZE (1 << U32_HASH_SHIFT)

static unsigned int tc_u_hash(const struct tcf_proto *tp)
{
	return hash_ptr(tp->chain->block, U32_HASH_SHIFT);
}

static struct tc_u_common *tc_u_common_find(const struct tcf_proto *tp)
{
	struct tc_u_common *tc;
	unsigned int h;

	h = tc_u_hash(tp);
	hlist_for_each_entry(tc, &tc_u_common_hash[h], hnode) {
		if (tc->block == tp->chain->block)
			return tc;
	}
	return NULL;
}

static int u32_init(struct tcf_proto *tp)
{
	struct tc_u_hnode *root_ht;
	struct tc_u_common *tp_c;
	unsigned int h;

	tp_c = tc_u_common_find(tp);

	root_ht = kzalloc(sizeof(*root_ht), GFP_KERNEL);
	if (root_ht == NULL)
		return -ENOBUFS;

	root_ht->refcnt++;
	root_ht->handle = tp_c ? gen_new_htid(tp_c, root_ht) : 0x80000000;
	root_ht->prio = tp->prio;
	idr_init(&root_ht->handle_idr);

	if (tp_c == NULL) {
		tp_c = kzalloc(sizeof(*tp_c), GFP_KERNEL);
		if (tp_c == NULL) {
			kfree(root_ht);
			return -ENOBUFS;
		}
		tp_c->block = tp->chain->block;
		INIT_HLIST_NODE(&tp_c->hnode);
		idr_init(&tp_c->handle_idr);

		h = tc_u_hash(tp);
		hlist_add_head(&tp_c->hnode, &tc_u_common_hash[h]);
	}

	tp_c->refcnt++;
	RCU_INIT_POINTER(root_ht->next, tp_c->hlist);
	rcu_assign_pointer(tp_c->hlist, root_ht);
	root_ht->tp_c = tp_c;

	rcu_assign_pointer(tp->root, root_ht);
	tp->data = tp_c;
	return 0;
}

static int u32_destroy_key(struct tcf_proto *tp, struct tc_u_knode *n,
			   bool free_pf)
{
	tcf_exts_destroy(&n->exts);
	tcf_exts_put_net(&n->exts);
	if (n->ht_down)
		n->ht_down->refcnt--;
#ifdef CONFIG_CLS_U32_PERF
	if (free_pf)
		free_percpu(n->pf);
#endif
#ifdef CONFIG_CLS_U32_MARK
	if (free_pf)
		free_percpu(n->pcpu_success);
#endif
	kfree(n);
	return 0;
}

/* u32_delete_key_rcu should be called when free'ing a copied
 * version of a tc_u_knode obtained from u32_init_knode(). When
 * copies are obtained from u32_init_knode() the statistics are
 * shared between the old and new copies to allow readers to
 * continue to update the statistics during the copy. To support
 * this the u32_delete_key_rcu variant does not free the percpu
 * statistics.
 */
static void u32_delete_key_work(struct work_struct *work)
{
	struct tc_u_knode *key = container_of(work, struct tc_u_knode, work);

	rtnl_lock();
	u32_destroy_key(key->tp, key, false);
	rtnl_unlock();
}

static void u32_delete_key_rcu(struct rcu_head *rcu)
{
	struct tc_u_knode *key = container_of(rcu, struct tc_u_knode, rcu);

	INIT_WORK(&key->work, u32_delete_key_work);
	tcf_queue_work(&key->work);
}

/* u32_delete_key_freepf_rcu is the rcu callback variant
 * that free's the entire structure including the statistics
 * percpu variables. Only use this if the key is not a copy
 * returned by u32_init_knode(). See u32_delete_key_rcu()
 * for the variant that should be used with keys return from
 * u32_init_knode()
 */
static void u32_delete_key_freepf_work(struct work_struct *work)
{
	struct tc_u_knode *key = container_of(work, struct tc_u_knode, work);

	rtnl_lock();
	u32_destroy_key(key->tp, key, true);
	rtnl_unlock();
}

static void u32_delete_key_freepf_rcu(struct rcu_head *rcu)
{
	struct tc_u_knode *key = container_of(rcu, struct tc_u_knode, rcu);

	INIT_WORK(&key->work, u32_delete_key_freepf_work);
	tcf_queue_work(&key->work);
}

static int u32_delete_key(struct tcf_proto *tp, struct tc_u_knode *key)
{
	struct tc_u_knode __rcu **kp;
	struct tc_u_knode *pkp;
	struct tc_u_hnode *ht = rtnl_dereference(key->ht_up);

	if (ht) {
		kp = &ht->ht[TC_U32_HASH(key->handle)];
		for (pkp = rtnl_dereference(*kp); pkp;
		     kp = &pkp->next, pkp = rtnl_dereference(*kp)) {
			if (pkp == key) {
				RCU_INIT_POINTER(*kp, key->next);

				tcf_unbind_filter(tp, &key->res);
				tcf_exts_get_net(&key->exts);
				call_rcu(&key->rcu, u32_delete_key_freepf_rcu);
				return 0;
			}
		}
	}
	WARN_ON(1);
	return 0;
}

static void u32_clear_hw_hnode(struct tcf_proto *tp, struct tc_u_hnode *h)
{
	struct tcf_block *block = tp->chain->block;
	struct tc_cls_u32_offload cls_u32 = {};

	tc_cls_common_offload_init(&cls_u32.common, tp);
	cls_u32.command = TC_CLSU32_DELETE_HNODE;
	cls_u32.hnode.divisor = h->divisor;
	cls_u32.hnode.handle = h->handle;
	cls_u32.hnode.prio = h->prio;

	tc_setup_cb_call(block, NULL, TC_SETUP_CLSU32, &cls_u32, false);
}

static int u32_replace_hw_hnode(struct tcf_proto *tp, struct tc_u_hnode *h,
				u32 flags)
{
	struct tcf_block *block = tp->chain->block;
	struct tc_cls_u32_offload cls_u32 = {};
	bool skip_sw = tc_skip_sw(flags);
	bool offloaded = false;
	int err;

	tc_cls_common_offload_init(&cls_u32.common, tp);
	cls_u32.command = TC_CLSU32_NEW_HNODE;
	cls_u32.hnode.divisor = h->divisor;
	cls_u32.hnode.handle = h->handle;
	cls_u32.hnode.prio = h->prio;

	err = tc_setup_cb_call(block, NULL, TC_SETUP_CLSU32, &cls_u32, skip_sw);
	if (err < 0) {
		u32_clear_hw_hnode(tp, h);
		return err;
	} else if (err > 0) {
		offloaded = true;
	}

	if (skip_sw && !offloaded)
		return -EINVAL;

	return 0;
}

static void u32_remove_hw_knode(struct tcf_proto *tp, u32 handle)
{
	struct tcf_block *block = tp->chain->block;
	struct tc_cls_u32_offload cls_u32 = {};

	tc_cls_common_offload_init(&cls_u32.common, tp);
	cls_u32.command = TC_CLSU32_DELETE_KNODE;
	cls_u32.knode.handle = handle;

	tc_setup_cb_call(block, NULL, TC_SETUP_CLSU32, &cls_u32, false);
}

static int u32_replace_hw_knode(struct tcf_proto *tp, struct tc_u_knode *n,
				u32 flags)
{
	struct tcf_block *block = tp->chain->block;
	struct tc_cls_u32_offload cls_u32 = {};
	bool skip_sw = tc_skip_sw(flags);
	int err;

	tc_cls_common_offload_init(&cls_u32.common, tp);
	cls_u32.command = TC_CLSU32_REPLACE_KNODE;
	cls_u32.knode.handle = n->handle;
	cls_u32.knode.fshift = n->fshift;
#ifdef CONFIG_CLS_U32_MARK
	cls_u32.knode.val = n->val;
	cls_u32.knode.mask = n->mask;
#else
	cls_u32.knode.val = 0;
	cls_u32.knode.mask = 0;
#endif
	cls_u32.knode.sel = &n->sel;
	cls_u32.knode.exts = &n->exts;
	if (n->ht_down)
		cls_u32.knode.link_handle = n->ht_down->handle;

	err = tc_setup_cb_call(block, NULL, TC_SETUP_CLSU32, &cls_u32, skip_sw);
	if (err < 0) {
		u32_remove_hw_knode(tp, n->handle);
		return err;
	} else if (err > 0) {
		n->flags |= TCA_CLS_FLAGS_IN_HW;
	}

	if (skip_sw && !(n->flags & TCA_CLS_FLAGS_IN_HW))
		return -EINVAL;

	return 0;
}

static void u32_clear_hnode(struct tcf_proto *tp, struct tc_u_hnode *ht)
{
	struct tc_u_knode *n;
	unsigned int h;

	for (h = 0; h <= ht->divisor; h++) {
		while ((n = rtnl_dereference(ht->ht[h])) != NULL) {
			RCU_INIT_POINTER(ht->ht[h],
					 rtnl_dereference(n->next));
			tcf_unbind_filter(tp, &n->res);
			u32_remove_hw_knode(tp, n->handle);
			idr_remove_ext(&ht->handle_idr, n->handle);
			if (tcf_exts_get_net(&n->exts))
				call_rcu(&n->rcu, u32_delete_key_freepf_rcu);
			else
				u32_destroy_key(n->tp, n, true);
		}
	}
}

static int u32_destroy_hnode(struct tcf_proto *tp, struct tc_u_hnode *ht)
{
	struct tc_u_common *tp_c = tp->data;
	struct tc_u_hnode __rcu **hn;
	struct tc_u_hnode *phn;

	WARN_ON(ht->refcnt);

	u32_clear_hnode(tp, ht);

	hn = &tp_c->hlist;
	for (phn = rtnl_dereference(*hn);
	     phn;
	     hn = &phn->next, phn = rtnl_dereference(*hn)) {
		if (phn == ht) {
			u32_clear_hw_hnode(tp, ht);
			idr_destroy(&ht->handle_idr);
			idr_remove_ext(&tp_c->handle_idr, ht->handle);
			RCU_INIT_POINTER(*hn, ht->next);
			kfree_rcu(ht, rcu);
			return 0;
		}
	}

	return -ENOENT;
}

static bool ht_empty(struct tc_u_hnode *ht)
{
	unsigned int h;

	for (h = 0; h <= ht->divisor; h++)
		if (rcu_access_pointer(ht->ht[h]))
			return false;

	return true;
}

static void u32_destroy(struct tcf_proto *tp)
{
	struct tc_u_common *tp_c = tp->data;
	struct tc_u_hnode *root_ht = rtnl_dereference(tp->root);

	WARN_ON(root_ht == NULL);

	if (root_ht && --root_ht->refcnt == 0)
		u32_destroy_hnode(tp, root_ht);

	if (--tp_c->refcnt == 0) {
		struct tc_u_hnode *ht;

		hlist_del(&tp_c->hnode);

		for (ht = rtnl_dereference(tp_c->hlist);
		     ht;
		     ht = rtnl_dereference(ht->next)) {
			ht->refcnt--;
			u32_clear_hnode(tp, ht);
		}

		while ((ht = rtnl_dereference(tp_c->hlist)) != NULL) {
			RCU_INIT_POINTER(tp_c->hlist, ht->next);
			kfree_rcu(ht, rcu);
		}

		idr_destroy(&tp_c->handle_idr);
		kfree(tp_c);
	}

	tp->data = NULL;
}

static int u32_delete(struct tcf_proto *tp, void *arg, bool *last)
{
	struct tc_u_hnode *ht = arg;
	struct tc_u_hnode *root_ht = rtnl_dereference(tp->root);
	struct tc_u_common *tp_c = tp->data;
	int ret = 0;

	if (ht == NULL)
		goto out;

	if (TC_U32_KEY(ht->handle)) {
		u32_remove_hw_knode(tp, ht->handle);
		ret = u32_delete_key(tp, (struct tc_u_knode *)ht);
		goto out;
	}

	if (root_ht == ht)
		return -EINVAL;

	if (ht->refcnt == 1) {
		ht->refcnt--;
		u32_destroy_hnode(tp, ht);
	} else {
		return -EBUSY;
	}

out:
	*last = true;
	if (root_ht) {
		if (root_ht->refcnt > 1) {
			*last = false;
			goto ret;
		}
		if (root_ht->refcnt == 1) {
			if (!ht_empty(root_ht)) {
				*last = false;
				goto ret;
			}
		}
	}

	if (tp_c->refcnt > 1) {
		*last = false;
		goto ret;
	}

	if (tp_c->refcnt == 1) {
		struct tc_u_hnode *ht;

		for (ht = rtnl_dereference(tp_c->hlist);
		     ht;
		     ht = rtnl_dereference(ht->next))
			if (!ht_empty(ht)) {
				*last = false;
				break;
			}
	}

ret:
	return ret;
}

static u32 gen_new_kid(struct tc_u_hnode *ht, u32 htid)
{
	unsigned long idr_index;
	u32 start = htid | 0x800;
	u32 max = htid | 0xFFF;
	u32 min = htid;

	if (idr_alloc_ext(&ht->handle_idr, NULL, &idr_index,
			  start, max + 1, GFP_KERNEL)) {
		if (idr_alloc_ext(&ht->handle_idr, NULL, &idr_index,
				  min + 1, max + 1, GFP_KERNEL))
			return max;
	}

	return (u32)idr_index;
}

static const struct nla_policy u32_policy[TCA_U32_MAX + 1] = {
	[TCA_U32_CLASSID]	= { .type = NLA_U32 },
	[TCA_U32_HASH]		= { .type = NLA_U32 },
	[TCA_U32_LINK]		= { .type = NLA_U32 },
	[TCA_U32_DIVISOR]	= { .type = NLA_U32 },
	[TCA_U32_SEL]		= { .len = sizeof(struct tc_u32_sel) },
	[TCA_U32_INDEV]		= { .type = NLA_STRING, .len = IFNAMSIZ },
	[TCA_U32_MARK]		= { .len = sizeof(struct tc_u32_mark) },
	[TCA_U32_FLAGS]		= { .type = NLA_U32 },
};

static int u32_set_parms(struct net *net, struct tcf_proto *tp,
			 unsigned long base, struct tc_u_hnode *ht,
			 struct tc_u_knode *n, struct nlattr **tb,
			 struct nlattr *est, bool ovr)
{
	int err;

	err = tcf_exts_validate(net, tp, tb, est, &n->exts, ovr);
	if (err < 0)
		return err;

	if (tb[TCA_U32_LINK]) {
		u32 handle = nla_get_u32(tb[TCA_U32_LINK]);
		struct tc_u_hnode *ht_down = NULL, *ht_old;

		if (TC_U32_KEY(handle))
			return -EINVAL;

		if (handle) {
			ht_down = u32_lookup_ht(ht->tp_c, handle);

			if (ht_down == NULL)
				return -EINVAL;
			ht_down->refcnt++;
		}

		ht_old = rtnl_dereference(n->ht_down);
		rcu_assign_pointer(n->ht_down, ht_down);

		if (ht_old)
			ht_old->refcnt--;
	}
	if (tb[TCA_U32_CLASSID]) {
		n->res.classid = nla_get_u32(tb[TCA_U32_CLASSID]);
		tcf_bind_filter(tp, &n->res, base);
	}

#ifdef CONFIG_NET_CLS_IND
	if (tb[TCA_U32_INDEV]) {
		int ret;
		ret = tcf_change_indev(net, tb[TCA_U32_INDEV]);
		if (ret < 0)
			return -EINVAL;
		n->ifindex = ret;
	}
#endif
	return 0;
}

static void u32_replace_knode(struct tcf_proto *tp, struct tc_u_common *tp_c,
			      struct tc_u_knode *n)
{
	struct tc_u_knode __rcu **ins;
	struct tc_u_knode *pins;
	struct tc_u_hnode *ht;

	if (TC_U32_HTID(n->handle) == TC_U32_ROOT)
		ht = rtnl_dereference(tp->root);
	else
		ht = u32_lookup_ht(tp_c, TC_U32_HTID(n->handle));

	ins = &ht->ht[TC_U32_HASH(n->handle)];

	/* The node must always exist for it to be replaced if this is not the
	 * case then something went very wrong elsewhere.
	 */
	for (pins = rtnl_dereference(*ins); ;
	     ins = &pins->next, pins = rtnl_dereference(*ins))
		if (pins->handle == n->handle)
			break;

	idr_replace_ext(&ht->handle_idr, n, n->handle);
	RCU_INIT_POINTER(n->next, pins->next);
	rcu_assign_pointer(*ins, n);
}

static struct tc_u_knode *u32_init_knode(struct tcf_proto *tp,
					 struct tc_u_knode *n)
{
	struct tc_u_knode *new;
	struct tc_u32_sel *s = &n->sel;

	new = kzalloc(sizeof(*n) + s->nkeys*sizeof(struct tc_u32_key),
		      GFP_KERNEL);

	if (!new)
		return NULL;

	RCU_INIT_POINTER(new->next, n->next);
	new->handle = n->handle;
	RCU_INIT_POINTER(new->ht_up, n->ht_up);

#ifdef CONFIG_NET_CLS_IND
	new->ifindex = n->ifindex;
#endif
	new->fshift = n->fshift;
	new->res = n->res;
	new->flags = n->flags;
	RCU_INIT_POINTER(new->ht_down, n->ht_down);

	/* bump reference count as long as we hold pointer to structure */
	if (new->ht_down)
		new->ht_down->refcnt++;

#ifdef CONFIG_CLS_U32_PERF
	/* Statistics may be incremented by readers during update
	 * so we must keep them in tact. When the node is later destroyed
	 * a special destroy call must be made to not free the pf memory.
	 */
	new->pf = n->pf;
#endif

#ifdef CONFIG_CLS_U32_MARK
	new->val = n->val;
	new->mask = n->mask;
	/* Similarly success statistics must be moved as pointers */
	new->pcpu_success = n->pcpu_success;
#endif
	new->tp = tp;
	memcpy(&new->sel, s, sizeof(*s) + s->nkeys*sizeof(struct tc_u32_key));

	if (tcf_exts_init(&new->exts, TCA_U32_ACT, TCA_U32_POLICE)) {
		kfree(new);
		return NULL;
	}

	return new;
}

static int u32_change(struct net *net, struct sk_buff *in_skb,
		      struct tcf_proto *tp, unsigned long base, u32 handle,
		      struct nlattr **tca, void **arg, bool ovr)
{
	struct tc_u_common *tp_c = tp->data;
	struct tc_u_hnode *ht;
	struct tc_u_knode *n;
	struct tc_u32_sel *s;
	struct nlattr *opt = tca[TCA_OPTIONS];
	struct nlattr *tb[TCA_U32_MAX + 1];
	u32 htid, flags = 0;
	int err;
#ifdef CONFIG_CLS_U32_PERF
	size_t size;
#endif

	if (opt == NULL)
		return handle ? -EINVAL : 0;

	err = nla_parse_nested(tb, TCA_U32_MAX, opt, u32_policy, NULL);
	if (err < 0)
		return err;

	if (tb[TCA_U32_FLAGS]) {
		flags = nla_get_u32(tb[TCA_U32_FLAGS]);
		if (!tc_flags_valid(flags))
			return -EINVAL;
	}

	n = *arg;
	if (n) {
		struct tc_u_knode *new;

		if (TC_U32_KEY(n->handle) == 0)
			return -EINVAL;

		if (n->flags != flags)
			return -EINVAL;

		new = u32_init_knode(tp, n);
		if (!new)
			return -ENOMEM;

		err = u32_set_parms(net, tp, base,
				    rtnl_dereference(n->ht_up), new, tb,
				    tca[TCA_RATE], ovr);

		if (err) {
			u32_destroy_key(tp, new, false);
			return err;
		}

		err = u32_replace_hw_knode(tp, new, flags);
		if (err) {
			u32_destroy_key(tp, new, false);
			return err;
		}

		if (!tc_in_hw(new->flags))
			new->flags |= TCA_CLS_FLAGS_NOT_IN_HW;

		u32_replace_knode(tp, tp_c, new);
		tcf_unbind_filter(tp, &n->res);
		tcf_exts_get_net(&n->exts);
		call_rcu(&n->rcu, u32_delete_key_rcu);
		return 0;
	}

	if (tb[TCA_U32_DIVISOR]) {
		unsigned int divisor = nla_get_u32(tb[TCA_U32_DIVISOR]);

		if (--divisor > 0x100)
			return -EINVAL;
		if (TC_U32_KEY(handle))
			return -EINVAL;
		ht = kzalloc(sizeof(*ht) + divisor*sizeof(void *), GFP_KERNEL);
		if (ht == NULL)
			return -ENOBUFS;
		if (handle == 0) {
			handle = gen_new_htid(tp->data, ht);
			if (handle == 0) {
				kfree(ht);
				return -ENOMEM;
			}
		} else {
			err = idr_alloc_ext(&tp_c->handle_idr, ht, NULL,
					    handle, handle + 1, GFP_KERNEL);
			if (err) {
				kfree(ht);
				return err;
			}
		}
		ht->tp_c = tp_c;
		ht->refcnt = 1;
		ht->divisor = divisor;
		ht->handle = handle;
		ht->prio = tp->prio;
		idr_init(&ht->handle_idr);

		err = u32_replace_hw_hnode(tp, ht, flags);
		if (err) {
			idr_remove_ext(&tp_c->handle_idr, handle);
			kfree(ht);
			return err;
		}

		RCU_INIT_POINTER(ht->next, tp_c->hlist);
		rcu_assign_pointer(tp_c->hlist, ht);
		*arg = ht;

		return 0;
	}

	if (tb[TCA_U32_HASH]) {
		htid = nla_get_u32(tb[TCA_U32_HASH]);
		if (TC_U32_HTID(htid) == TC_U32_ROOT) {
			ht = rtnl_dereference(tp->root);
			htid = ht->handle;
		} else {
			ht = u32_lookup_ht(tp->data, TC_U32_HTID(htid));
			if (ht == NULL)
				return -EINVAL;
		}
	} else {
		ht = rtnl_dereference(tp->root);
		htid = ht->handle;
	}

	if (ht->divisor < TC_U32_HASH(htid))
		return -EINVAL;

	if (handle) {
		if (TC_U32_HTID(handle) && TC_U32_HTID(handle^htid))
			return -EINVAL;
		handle = htid | TC_U32_NODE(handle);
		err = idr_alloc_ext(&ht->handle_idr, NULL, NULL,
				    handle, handle + 1,
				    GFP_KERNEL);
		if (err)
			return err;
	} else
		handle = gen_new_kid(ht, htid);

	if (tb[TCA_U32_SEL] == NULL) {
		err = -EINVAL;
		goto erridr;
	}

	s = nla_data(tb[TCA_U32_SEL]);

	n = kzalloc(sizeof(*n) + s->nkeys*sizeof(struct tc_u32_key), GFP_KERNEL);
	if (n == NULL) {
		err = -ENOBUFS;
		goto erridr;
	}

#ifdef CONFIG_CLS_U32_PERF
	size = sizeof(struct tc_u32_pcnt) + s->nkeys * sizeof(u64);
	n->pf = __alloc_percpu(size, __alignof__(struct tc_u32_pcnt));
	if (!n->pf) {
		err = -ENOBUFS;
		goto errfree;
	}
#endif

	memcpy(&n->sel, s, sizeof(*s) + s->nkeys*sizeof(struct tc_u32_key));
	RCU_INIT_POINTER(n->ht_up, ht);
	n->handle = handle;
	n->fshift = s->hmask ? ffs(ntohl(s->hmask)) - 1 : 0;
	n->flags = flags;
	n->tp = tp;

	err = tcf_exts_init(&n->exts, TCA_U32_ACT, TCA_U32_POLICE);
	if (err < 0)
		goto errout;

#ifdef CONFIG_CLS_U32_MARK
	n->pcpu_success = alloc_percpu(u32);
	if (!n->pcpu_success) {
		err = -ENOMEM;
		goto errout;
	}

	if (tb[TCA_U32_MARK]) {
		struct tc_u32_mark *mark;

		mark = nla_data(tb[TCA_U32_MARK]);
		n->val = mark->val;
		n->mask = mark->mask;
	}
#endif

	err = u32_set_parms(net, tp, base, ht, n, tb, tca[TCA_RATE], ovr);
	if (err == 0) {
		struct tc_u_knode __rcu **ins;
		struct tc_u_knode *pins;

		err = u32_replace_hw_knode(tp, n, flags);
		if (err)
			goto errhw;

		if (!tc_in_hw(n->flags))
			n->flags |= TCA_CLS_FLAGS_NOT_IN_HW;

		ins = &ht->ht[TC_U32_HASH(handle)];
		for (pins = rtnl_dereference(*ins); pins;
		     ins = &pins->next, pins = rtnl_dereference(*ins))
			if (TC_U32_NODE(handle) < TC_U32_NODE(pins->handle))
				break;

		RCU_INIT_POINTER(n->next, pins);
		rcu_assign_pointer(*ins, n);
		*arg = n;
		return 0;
	}

errhw:
#ifdef CONFIG_CLS_U32_MARK
	free_percpu(n->pcpu_success);
#endif

errout:
	tcf_exts_destroy(&n->exts);
#ifdef CONFIG_CLS_U32_PERF
errfree:
	free_percpu(n->pf);
#endif
	kfree(n);
erridr:
	idr_remove_ext(&ht->handle_idr, handle);
	return err;
}

static void u32_walk(struct tcf_proto *tp, struct tcf_walker *arg)
{
	struct tc_u_common *tp_c = tp->data;
	struct tc_u_hnode *ht;
	struct tc_u_knode *n;
	unsigned int h;

	if (arg->stop)
		return;

	for (ht = rtnl_dereference(tp_c->hlist);
	     ht;
	     ht = rtnl_dereference(ht->next)) {
		if (ht->prio != tp->prio)
			continue;
		if (arg->count >= arg->skip) {
			if (arg->fn(tp, ht, arg) < 0) {
				arg->stop = 1;
				return;
			}
		}
		arg->count++;
		for (h = 0; h <= ht->divisor; h++) {
			for (n = rtnl_dereference(ht->ht[h]);
			     n;
			     n = rtnl_dereference(n->next)) {
				if (arg->count < arg->skip) {
					arg->count++;
					continue;
				}
				if (arg->fn(tp, n, arg) < 0) {
					arg->stop = 1;
					return;
				}
				arg->count++;
			}
		}
	}
}

static void u32_bind_class(void *fh, u32 classid, unsigned long cl)
{
	struct tc_u_knode *n = fh;

	if (n && n->res.classid == classid)
		n->res.class = cl;
}

static int u32_dump(struct net *net, struct tcf_proto *tp, void *fh,
		    struct sk_buff *skb, struct tcmsg *t)
{
	struct tc_u_knode *n = fh;
	struct tc_u_hnode *ht_up, *ht_down;
	struct nlattr *nest;

	if (n == NULL)
		return skb->len;

	t->tcm_handle = n->handle;

	nest = nla_nest_start(skb, TCA_OPTIONS);
	if (nest == NULL)
		goto nla_put_failure;

	if (TC_U32_KEY(n->handle) == 0) {
		struct tc_u_hnode *ht = fh;
		u32 divisor = ht->divisor + 1;

		if (nla_put_u32(skb, TCA_U32_DIVISOR, divisor))
			goto nla_put_failure;
	} else {
#ifdef CONFIG_CLS_U32_PERF
		struct tc_u32_pcnt *gpf;
		int cpu;
#endif

		if (nla_put(skb, TCA_U32_SEL,
			    sizeof(n->sel) + n->sel.nkeys*sizeof(struct tc_u32_key),
			    &n->sel))
			goto nla_put_failure;

		ht_up = rtnl_dereference(n->ht_up);
		if (ht_up) {
			u32 htid = n->handle & 0xFFFFF000;
			if (nla_put_u32(skb, TCA_U32_HASH, htid))
				goto nla_put_failure;
		}
		if (n->res.classid &&
		    nla_put_u32(skb, TCA_U32_CLASSID, n->res.classid))
			goto nla_put_failure;

		ht_down = rtnl_dereference(n->ht_down);
		if (ht_down &&
		    nla_put_u32(skb, TCA_U32_LINK, ht_down->handle))
			goto nla_put_failure;

		if (n->flags && nla_put_u32(skb, TCA_U32_FLAGS, n->flags))
			goto nla_put_failure;

#ifdef CONFIG_CLS_U32_MARK
		if ((n->val || n->mask)) {
			struct tc_u32_mark mark = {.val = n->val,
						   .mask = n->mask,
						   .success = 0};
			int cpum;

			for_each_possible_cpu(cpum) {
				__u32 cnt = *per_cpu_ptr(n->pcpu_success, cpum);

				mark.success += cnt;
			}

			if (nla_put(skb, TCA_U32_MARK, sizeof(mark), &mark))
				goto nla_put_failure;
		}
#endif

		if (tcf_exts_dump(skb, &n->exts) < 0)
			goto nla_put_failure;

#ifdef CONFIG_NET_CLS_IND
		if (n->ifindex) {
			struct net_device *dev;
			dev = __dev_get_by_index(net, n->ifindex);
			if (dev && nla_put_string(skb, TCA_U32_INDEV, dev->name))
				goto nla_put_failure;
		}
#endif
#ifdef CONFIG_CLS_U32_PERF
		gpf = kzalloc(sizeof(struct tc_u32_pcnt) +
			      n->sel.nkeys * sizeof(u64),
			      GFP_KERNEL);
		if (!gpf)
			goto nla_put_failure;

		for_each_possible_cpu(cpu) {
			int i;
			struct tc_u32_pcnt *pf = per_cpu_ptr(n->pf, cpu);

			gpf->rcnt += pf->rcnt;
			gpf->rhit += pf->rhit;
			for (i = 0; i < n->sel.nkeys; i++)
				gpf->kcnts[i] += pf->kcnts[i];
		}

		if (nla_put_64bit(skb, TCA_U32_PCNT,
				  sizeof(struct tc_u32_pcnt) +
				  n->sel.nkeys * sizeof(u64),
				  gpf, TCA_U32_PAD)) {
			kfree(gpf);
			goto nla_put_failure;
		}
		kfree(gpf);
#endif
	}

	nla_nest_end(skb, nest);

	if (TC_U32_KEY(n->handle))
		if (tcf_exts_dump_stats(skb, &n->exts) < 0)
			goto nla_put_failure;
	return skb->len;

nla_put_failure:
	nla_nest_cancel(skb, nest);
	return -1;
}

static struct tcf_proto_ops cls_u32_ops __read_mostly = {
	.kind		=	"u32",
	.classify	=	u32_classify,
	.init		=	u32_init,
	.destroy	=	u32_destroy,
	.get		=	u32_get,
	.change		=	u32_change,
	.delete		=	u32_delete,
	.walk		=	u32_walk,
	.dump		=	u32_dump,
	.bind_class	=	u32_bind_class,
	.owner		=	THIS_MODULE,
};

static int __init init_u32(void)
{
	int i, ret;

	pr_info("u32 classifier\n");
#ifdef CONFIG_CLS_U32_PERF
	pr_info("    Performance counters on\n");
#endif
#ifdef CONFIG_NET_CLS_IND
	pr_info("    input device check on\n");
#endif
#ifdef CONFIG_NET_CLS_ACT
	pr_info("    Actions configured\n");
#endif
	tc_u_common_hash = kvmalloc_array(U32_HASH_SIZE,
					  sizeof(struct hlist_head),
					  GFP_KERNEL);
	if (!tc_u_common_hash)
		return -ENOMEM;

	for (i = 0; i < U32_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&tc_u_common_hash[i]);

	ret = register_tcf_proto_ops(&cls_u32_ops);
	if (ret)
		kvfree(tc_u_common_hash);
	return ret;
}

static void __exit exit_u32(void)
{
	unregister_tcf_proto_ops(&cls_u32_ops);
	kvfree(tc_u_common_hash);
}

module_init(init_u32)
module_exit(exit_u32)
MODULE_LICENSE("GPL");
