/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
#ident "Copyright (c) 2007-2010 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."


/*

Managing the tree shape:  How insertion, deletion, and querying work

When we insert a message into the BRT, here's what happens.

Insert_a_message_at_root (msg)
   root = find the root
   insert_the_message_into_the_buffers_of(msg, root)
   If the root is way too full then process the root ourself.  "Way too full" means something like twice as much messages as it's supposed to have.
   else If the root needs to be split, then split it 
   else if the root's buffers are too full then (it must be a nonleaf)
      create a work item to process the root.	The workitem specifies a height and a key (the height is the height of the root, and the key can be any key)
   endif
   If the brt file is fragmented, and the file isn't being shrunk, then set file->being_shrunk and schedule a work item to shrink the file.

To process a nonleaf node (height, key)
   Note: Height is always > 0.
   Note: This process occurs asynchrnously, but we get the YDB lock at the beginning.
   Descend the tree following KEY until	 a node of HEIGHT is found.
      While the node is too full then 
	 pick the heaviest child
	 bring that child into memory (use nonblocking get_and_pin, which means that if we get a try-again, we go back up and restart the process_a_node job.
	 move all messages for that child from the node to the child.
	 If the child needs to be split or merged, then split or merge the child.
	 If the resulting child's (or children's) buffers are too full then create a work item for each such child to process the child.  (This can only happen
	       for nonleaf children, since otherwise there are no buffers to be too full).

We also have a background thread that traverses the tree (relatively slowly) to flatten the tree.
Background_flattener:
   It's state is a height and a key and a child number
   Repeat:
      sleep (say 1s)
      grab the ydb lock
      descend the tree to find the height and key
      while the node is not empty:
	 bring the child into memory (possibly causing a TRY_AGAIN)
	 move all messages from the node into the child
	 if the child needs to be split or merged then split or merge the child
	 set the state to operate on the next relevant node in the depth-first order
	   That is: if there are more children, increment the child number, and return.
		    if there are no more children, then return with an error code that says "next".  At the first point at the descent is not to the ultimate
			     child, then set the state to visit that node and that child.  
		    if we get back up to the root then the state goes to "root" and "child 0" so the whole background flattener can run again the next BRT.   
		      Probably only open BRTs get flattened.
		      It may be important for the flattener not to run if there've been no message insertions since the last time it ran.
      The background flattener should also garbage collect MVCC versions.  The flattener should remember the MVCC versions it has encountered
	so that if any of those are no longer live, it can run again.
			 

To shrink a file: Let X be the size of the reachable data.  
    We define an acceptable bloat constant of C.  For example we set C=2 if we are willing to allow the file to be as much as 2X in size.
    The goal is to find the smallest amount of stuff we can move to get the file down to size CX.
    That seems like a difficult problem, so we use the following heuristics:
       If we can relocate the last block to an lower location, then do so immediately.	(The file gets smaller right away, so even though the new location
	 may even not be in the first CX bytes, we are making the file smaller.)
       Otherwise all of the earlier blocks are smaller than the last block (of size L).	 So find the smallest region that has L free bytes in it.
	 (This can be computed in one pass)
	 Move the first allocated block in that region to some location not in the interior of the region.
	       (Outside of the region is OK, and reallocating the block at the edge of the region is OK).
	    This has the effect of creating a smaller region with at least L free bytes in it.
	 Go back to the top (because by now some other block may have been allocated or freed).
    Claim: if there are no other allocations going on concurrently, then this algorithm will shrink the file reasonably efficiently.  By this I mean that
       each block of shrinkage does the smallest amount of work possible.  That doesn't mean that the work overall is minimized.
    Note: If there are other allocations and deallocations going on concurrently, we might never get enough space to move the last block.  But it takes a lot
      of allocations and deallocations to make that happen, and it's probably reasonable for the file not to shrink in this case.

To split or merge a child of a node:
Split_or_merge (node, childnum) {
  If the child needs to be split (it's a leaf with too much stuff or a nonleaf with too much fanout)
    fetch the node and the child into main memory.
    split the child, producing two nodes A and B, and also a pivot.   Don't worry if the resulting child is still too big or too small.	 Fix it on the next pass.
    fixup node to point at the two new children.  Don't worry about the node getting too much fanout.
    return;
  If the child needs to be merged (it's a leaf with too little stuff (less than 1/4 full) or a nonleaf with too little fanout (less than 1/4)
    fetch node, the child  and a sibling of the child into main memory.
    move all messages from the node to the two children (so that the FIFOs are empty)
    If the two siblings together fit into one node then
      merge the two siblings. 
      fixup the node to point at one child
    Otherwise
      load balance the content of the two nodes
    Don't worry about the resulting children having too many messages or otherwise being too big or too small.	Fix it on the next pass.
  }
}


Lookup:
 As of #3312, we don't do any tree shaping on lookup.
 We don't promote eagerly or use aggressive promotion or passive-aggressive promotion.	We just push messages down according to the traditional BRT algorithm
  on insertions.
 For lookups, we maintain the invariant that the in-memory leaf nodes have a soft copy which reflects all the messages above it in the tree.
 So when a leaf node is brought into memory, we apply all messages above it.
 When a message is inserted into the tree, we apply it to all the leaf nodes to which it is applicable.
 When flushing to a leaf, we flush to the hard copy not to the soft copy.
*/

#include "includes.h"
#include "checkpoint.h"
// Access to nested transaction logic
#include "ule.h"
#include "xids.h"
#include "roll.h"
#include "toku_atomic.h"
#include "sub_block.h"


static const uint32_t this_version = BRT_LAYOUT_VERSION;


void
toku_brt_header_suppress_rollbacks(struct brt_header *h, TOKUTXN txn) {
    TXNID txnid = toku_txn_get_txnid(txn);
    assert(h->txnid_that_created_or_locked_when_empty == TXNID_NONE ||
	   h->txnid_that_created_or_locked_when_empty == txnid);
    h->txnid_that_created_or_locked_when_empty = txnid;
    TXNID rootid = toku_txn_get_root_txnid(txn);
    assert(h->root_that_created_or_locked_when_empty == TXNID_NONE ||
	   h->root_that_created_or_locked_when_empty == rootid);
    h->root_that_created_or_locked_when_empty  = rootid;
}

static void brt_cursor_invalidate(BRT_CURSOR brtcursor);

// We invalidate all the OMTCURSORS any time we push into the root of the BRT for that OMT.
// We keep a counter on each brt header, but if the brt header is evicted from the cachetable
// then we lose that counter.  So we also keep a global counter.
// An alternative would be to keep only the global counter.  But that would invalidate all OMTCURSORS
// even from unrelated BRTs.  This way we only invalidate an OMTCURSOR if
static u_int64_t global_root_put_counter = 0;

enum reactivity { RE_STABLE, RE_FUSIBLE, RE_FISSIBLE };

void
toku_assert_entire_node_in_memory(BRTNODE node) {
    for (int i = 0; i < node->n_children; i++) {
        assert(BP_STATE(node,i) == PT_AVAIL);
    }
}

static u_int32_t
get_leaf_num_entries(BRTNODE node) {
    u_int32_t result = 0;
    int i;
    toku_assert_entire_node_in_memory(node);
    for ( i = 0; i < node->n_children; i++) {
        result += toku_omt_size(BLB_BUFFER(node, i));
    }
    return result;
}

static enum reactivity
get_leaf_reactivity (BRTNODE node) {
    enum reactivity re = RE_STABLE;
    assert(node->height==0);
    if (node->dirty) {
	unsigned int size = toku_serialize_brtnode_size(node);
	if (size > node->nodesize && get_leaf_num_entries(node) > 1) {
	    re = RE_FISSIBLE;
	}
	else if ((size*4) < node->nodesize && !BLB_SEQINSERT(node, node->n_children-1)) {
	    re = RE_FUSIBLE;
	}
    }
    return re;
}

static enum reactivity
get_nonleaf_reactivity (BRTNODE node) {
    assert(node->height>0);
    int n_children = node->n_children;
    if (n_children > TREE_FANOUT) return RE_FISSIBLE;
    if (n_children*4 < TREE_FANOUT) return RE_FUSIBLE;
    return RE_STABLE;
}

static enum reactivity
get_node_reactivity (BRTNODE node) {
    toku_assert_entire_node_in_memory(node);
    if (node->height==0)
	return get_leaf_reactivity(node);
    else
	return get_nonleaf_reactivity(node);
}

static BOOL
nonleaf_node_is_gorged (BRTNODE node) {
    BOOL buffers_are_empty = TRUE;
    toku_assert_entire_node_in_memory(node);
    assert(node->height > 0);
    for (int child = 0; child < node->n_children; ++child) {
        if (BNC_NBYTESINBUF(node, child) > 0) {
            buffers_are_empty = FALSE;
            break;
        }
    }
    return (BOOL)((toku_serialize_brtnode_size(node) > node->nodesize)
		  &&
		  (!buffers_are_empty));
}

static void brtnode_put_cmd (BRT t, BRTNODE node, BRT_MSG cmd);


static void
flush_this_child (BRT t, BRTNODE node, int childnum, enum reactivity *child_re, BOOL is_first_flush, BOOL flush_recursively,
		  ANCESTORS ancestors, struct pivot_bounds const * const);

static void brt_verify_flags(BRT brt, BRTNODE node) {
    assert(brt->flags == node->flags);
}

int toku_brt_debug_mode = 0;

//#define SLOW
#ifdef SLOW
#define VERIFY_NODE(t,n) (toku_verify_or_set_counts(n), toku_verify_estimates(t,n))
#else
#define VERIFY_NODE(t,n) ((void)0)
#endif

//#define BRT_TRACE
#ifdef BRT_TRACE
#define WHEN_BRTTRACE(x) x
#else
#define WHEN_BRTTRACE(x) ((void)0)
#endif

static u_int32_t compute_child_fullhash (CACHEFILE cf, BRTNODE node, int childnum) {
    assert(node->height>0 && childnum<node->n_children);
    switch (BP_HAVE_FULLHASH(node, childnum)) {
    case TRUE:
	{
	    assert(BP_FULLHASH(node, childnum)==toku_cachetable_hash(cf, BP_BLOCKNUM(node, childnum)));
	    return BP_FULLHASH(node, childnum);
	}
    case FALSE:
	{
	    u_int32_t child_fullhash = toku_cachetable_hash(cf, BP_BLOCKNUM(node, childnum));
	    BP_HAVE_FULLHASH(node, childnum) = TRUE;
	    BP_FULLHASH(node, childnum) = child_fullhash;
	    return child_fullhash;
	}
    }
    abort(); return 0;
}

static void maybe_apply_ancestors_messages_to_node (BRT t, BRTNODE node, ANCESTORS ancestors, struct pivot_bounds const * const bounds);

static long brtnode_memory_size (BRTNODE node);

int toku_pin_brtnode (BRT brt, BLOCKNUM blocknum, u_int32_t fullhash,
		      UNLOCKERS unlockers,
		      ANCESTORS ancestors, struct pivot_bounds const * const bounds,
		      struct brtnode_fetch_extra *bfe,
		      BRTNODE *node_p) {
    void *node_v;
    int r = toku_cachetable_get_and_pin_nonblocking(
            brt->cf, 
            blocknum, 
            fullhash, 
            &node_v, 
            NULL, 
            toku_brtnode_flush_callback, 
            toku_brtnode_fetch_callback, 
            toku_brtnode_pe_callback,
            toku_brtnode_pf_req_callback,
            toku_brtnode_pf_callback,
            bfe, //read_extraargs 
            brt->h, //write_extraargs
            unlockers);
    if (r==0) {
	BRTNODE node = node_v;
	maybe_apply_ancestors_messages_to_node(brt, node, ancestors, bounds);
	*node_p = node;
	// printf("%*sPin %ld\n", 8-node->height, "", blocknum.b);
    } else {
	assert(r==TOKUDB_TRY_AGAIN); // Any other error and we should bomb out ASAP.
	// printf("%*sPin %ld try again\n", 8, "", blocknum.b);
    }
    return r;
}

void toku_pin_brtnode_holding_lock (BRT brt, BLOCKNUM blocknum, u_int32_t fullhash,
				   ANCESTORS ancestors, struct pivot_bounds const * const bounds,
                                   struct brtnode_fetch_extra *bfe,
				   BRTNODE *node_p) {
    void *node_v;
    int r = toku_cachetable_get_and_pin(
        brt->cf, 
        blocknum, 
        fullhash, 
        &node_v, 
        NULL, 
        toku_brtnode_flush_callback, 
        toku_brtnode_fetch_callback, 
        toku_brtnode_pe_callback, 
        toku_brtnode_pf_req_callback,
        toku_brtnode_pf_callback,
        bfe,
        brt->h
        );
    assert(r==0);
    BRTNODE node = node_v;
    maybe_apply_ancestors_messages_to_node(brt, node, ancestors, bounds);
    *node_p = node;
}

static inline void
toku_verify_estimates (BRT t, BRTNODE node);

void toku_unpin_brtnode (BRT brt, BRTNODE node) 
// Effect: Unpin a brt node.
{
    // printf("%*sUnpin %ld\n", 8-node->height, "", node->thisnodename.b);
    VERIFY_NODE(brt,node);
    int r = toku_cachetable_unpin(brt->cf, node->thisnodename, node->fullhash, (enum cachetable_dirty) node->dirty, brtnode_memory_size(node));
    assert(r==0);
}

struct fill_leafnode_estimates_state {
    SUBTREE_EST e;
};

static int
fill_leafnode_estimates (OMTVALUE val, u_int32_t UU(idx), void *vs)
{
    LEAFENTRY le = val;
    struct fill_leafnode_estimates_state *s = vs;
    s->e->dsize += le_keylen(le) + le_latest_vallen(le);
    s->e->ndata++;
    s->e->nkeys++;
    return 0; // must return 0 to work with an omt_iterator
}

static struct subtree_estimates
calc_leaf_stats (OMT buffer) {
    struct subtree_estimates e = zero_estimates;
    struct fill_leafnode_estimates_state f = {&e};
    toku_omt_iterate(buffer, fill_leafnode_estimates, &f);
    return e;
}

void
toku_brt_leaf_reset_calc_leaf_stats(BRTNODE node) {
    invariant(node->height==0);
    int i = 0;
    for (i = 0; i < node->n_children; i++) {
        // basement node may be evicted, so only update stats if the basement node
        // is fully in memory
        // TODO: (Zardosht) for row cache, figure out a better way to do this
        if (BP_STATE(node,i) == PT_AVAIL) {
            node->bp[i].subtree_estimates = calc_leaf_stats(BLB_BUFFER(node, i));
        }
    }
}

// TODO: (Zardosht) look into this and possibly fix and use
static void __attribute__((__unused__))
brt_leaf_check_leaf_stats (BRTNODE node)
{
    assert(node);
    assert(FALSE);
    // static int count=0; count++;
    // if (node->height>0) return;
    // struct subtree_estimates e = calc_leaf_stats(node);
    // assert(e.ndata == node->u.l.leaf_stats.ndata);
    // assert(e.nkeys == node->u.l.leaf_stats.nkeys);
    // assert(e.dsize == node->u.l.leaf_stats.dsize);
    // assert(node->u.l.leaf_stats.exact);
}

// This should be done incrementally in most cases.
static void
fixup_child_estimates (BRTNODE node, int childnum_of_node, BRTNODE child, BOOL dirty_it)
// Effect:  Sum the child leafentry estimates and store them in NODE.
// Parameters:
//   node		 The node to modify
//   childnum_of_node	 Which child changed   (PERFORMANCE: Later we could compute this incrementally)
//   child		 The child that changed.
//   dirty_it		 If true, then mark the node dirty.  (Don't want to do this when updating in in-memory leaf.  Only force dirty when messages are being pushed down.
{
    struct subtree_estimates estimates = zero_estimates;
    estimates.exact = TRUE;
    int i;
    for (i=0; i<child->n_children; i++) {
	SUBTREE_EST child_se = &BP_SUBTREE_EST(child,i);
	estimates.nkeys += child_se->nkeys;
	estimates.ndata += child_se->ndata;
	estimates.dsize += child_se->dsize;
	if (!child_se->exact) estimates.exact = FALSE;
	if (child->height>0) {
	    if (BP_STATE(child,i) != PT_AVAIL || 
                toku_fifo_n_entries(BNC_BUFFER(child,i))!=0) 
            {
                estimates.exact=FALSE;
	    }
	}
    } 
    // We only call this function if we have reason to believe that the child changed.
    BP_SUBTREE_EST(node,childnum_of_node) = estimates;
    if (dirty_it) {
        node->dirty=1;
    }
}


static inline void
toku_verify_estimates (BRT t, BRTNODE node) {
    int childnum;
    for (childnum=0; childnum<node->n_children; childnum++) {
	// we'll just do this estimate
	u_int64_t child_estimate = 0;
        // can only check the state of available partitions
        if (BP_STATE(node, childnum) == PT_AVAIL) {
            if (node->height > 0) {
                BLOCKNUM childblocknum = BP_BLOCKNUM(node, childnum);
                u_int32_t fullhash = compute_child_fullhash(t->cf, node, childnum);
                void *childnode_v;
                struct brtnode_fetch_extra bfe;
                fill_bfe_for_full_read(&bfe, t->h);
                int r = toku_cachetable_get_and_pin(
                    t->cf, 
                    childblocknum, 
                    fullhash, 
                    &childnode_v, 
                    NULL, 
                    toku_brtnode_flush_callback, 
                    toku_brtnode_fetch_callback, 
                    toku_brtnode_pe_callback, 
                    toku_brtnode_pf_req_callback,
                    toku_brtnode_pf_callback,
                    &bfe, 
                    t->h
                    );
                assert_zero(r);
                BRTNODE childnode = childnode_v;
                for (int i=0; i<childnode->n_children; i++) {
            	    child_estimate += BP_SUBTREE_EST(childnode, i).ndata;
                }
                toku_unpin_brtnode(t, childnode);
            }
            else {
                child_estimate = toku_omt_size(BLB_BUFFER(node, childnum));
            }
            assert(BP_SUBTREE_EST(node,childnum).ndata==child_estimate);
        }
    }
}

static LEAFENTRY
fetch_from_buf (OMT omt, u_int32_t idx) {
    OMTVALUE v = 0;
    int r = toku_omt_fetch(omt, idx, &v, NULL);
    assert_zero(r);
    return (LEAFENTRY)v;
}

static long
brtnode_memory_size (BRTNODE node)
// Effect: Estimate how much main memory a node requires.
{
    long retval = 0;
    int n_children = node->n_children;
    retval += sizeof(*node);
    retval += (n_children)*(sizeof(node->bp[0]));
    retval += node->totalchildkeylens;

    // now calculate the sizes of the partitions
    for (int i = 0; i < n_children; i++) {
        if (BP_STATE(node,i) == PT_INVALID || BP_STATE(node,i) == PT_ON_DISK) {
            continue;
        }
        else if (BP_STATE(node,i) == PT_COMPRESSED) {
            struct sub_block* sb = node->bp[i].ptr;
            retval += sizeof(*sb);
            retval += sb->compressed_size;
        }
        else if (BP_STATE(node,i) == PT_AVAIL) {
            if (node->height > 0) {
                NONLEAF_CHILDINFO childinfo = node->bp[i].ptr;
                retval += sizeof(*childinfo);
                retval += toku_fifo_memory_size(BNC_BUFFER(node, i));
            }
            else {
                BASEMENTNODE bn = node->bp[i].ptr;
                retval += sizeof(*bn);
                retval += BLB_NBYTESINBUF(node,i);
                OMT curr_omt = BLB_BUFFER(node, i);
                retval += (toku_omt_memory_size(curr_omt));
            }
        }
        else {
            assert(FALSE);
        }
    }
    return retval;
}

// assign unique dictionary id
static uint64_t dict_id_serial = 1;
static DICTIONARY_ID
next_dict_id(void) {
    uint32_t i = toku_sync_fetch_and_increment_uint64(&dict_id_serial);
    assert(i);	// guarantee unique dictionary id by asserting 64-bit counter never wraps
    DICTIONARY_ID d = {.dictid = i};
    return d;
}

static void 
destroy_basement_node (BASEMENTNODE bn)
{
    // The buffer may have been freed already, in some cases.
    if (bn->buffer) {
	toku_omt_destroy(&bn->buffer);
	bn->buffer = NULL;
    }
}


u_int8_t 
toku_brtnode_partition_state (struct brtnode_fetch_extra* bfe, int childnum)
{
    if (bfe->type == brtnode_fetch_all || 
        (bfe->type == brtnode_fetch_subset && bfe->child_to_read == childnum))
    {
        return PT_AVAIL;
    }
    else {
        return PT_COMPRESSED;
    }
}



//fd is protected (must be holding fdlock)
void toku_brtnode_flush_callback (CACHEFILE cachefile, int fd, BLOCKNUM nodename, void *brtnode_v, void *extraargs, long size __attribute__((unused)), BOOL write_me, BOOL keep_me, BOOL for_checkpoint) {
    struct brt_header *h = extraargs;
    BRTNODE brtnode = brtnode_v;
    assert(brtnode->thisnodename.b==nodename.b);
    //printf("%s:%d %p->mdict[0]=%p\n", __FILE__, __LINE__, brtnode, brtnode->mdicts[0]);
    if (write_me) {
	if (!h->panic) { // if the brt panicked, stop writing, otherwise try to write it.
	    toku_assert_entire_node_in_memory(brtnode);
	    int n_workitems, n_threads; 
	    toku_cachefile_get_workqueue_load(cachefile, &n_workitems, &n_threads);
	    int r = toku_serialize_brtnode_to(fd, brtnode->thisnodename, brtnode, h, n_workitems, n_threads, for_checkpoint);
	    if (r) {
		if (h->panic==0) {
		    char *e = strerror(r);
		    int	  l = 200 + strlen(e);
		    char s[l];
		    h->panic=r;
		    snprintf(s, l-1, "While writing data to disk, error %d (%s)", r, e);
		    h->panic_string = toku_strdup(s);
		}
	    }
	}
    }
    //printf("%s:%d %p->mdict[0]=%p\n", __FILE__, __LINE__, brtnode, brtnode->mdicts[0]);
    if (!keep_me) {
	toku_brtnode_free(&brtnode);
    }
    //printf("%s:%d n_items_malloced=%lld\n", __FILE__, __LINE__, n_items_malloced);
}


//fd is protected (must be holding fdlock)
int toku_brtnode_fetch_callback (CACHEFILE UU(cachefile), int fd, BLOCKNUM nodename, u_int32_t fullhash, 
				 void **brtnode_pv, long *sizep, int *dirtyp, void *extraargs) {
    assert(extraargs);
    assert(*brtnode_pv == NULL);
    struct brtnode_fetch_extra *bfe = (struct brtnode_fetch_extra *)extraargs;
    BRTNODE *result=(BRTNODE*)brtnode_pv;
    int r = toku_deserialize_brtnode_from(fd, nodename, fullhash, result, bfe);
    if (r == 0) {
	*sizep = brtnode_memory_size(*result);
	*dirtyp = (*result)->dirty;
    }
    return r;
}

// callback for partially evicting a node
int toku_brtnode_pe_callback (void *brtnode_pv, long bytes_to_free, long* bytes_freed, void* UU(extraargs)) {
    BRTNODE node = (BRTNODE)brtnode_pv;
    long orig_size = brtnode_memory_size(node);
    assert(bytes_to_free > 0);

    // 
    // nothing on internal nodes for now
    //
    if (node->dirty || node->height > 0) {
        *bytes_freed = 0;
    }
    //
    // partial eviction strategy for basement nodes:
    //  if the bn is compressed, evict it
    //  else: check if it requires eviction, if it does, evict it, if not, sweep the clock count
    //
    //
    else {
        for (int i = 0; i < node->n_children; i++) {
            // Get rid of compressed stuff no matter what.
            if (BP_STATE(node,i) == PT_COMPRESSED) {
                struct sub_block* sb = node->bp[i].ptr;
                toku_free(sb->compressed_ptr);
                toku_free(node->bp[i].ptr);
                node->bp[i].ptr = NULL;
                BP_STATE(node,i) = PT_ON_DISK;
            }
            else if (BP_STATE(node,i) == PT_AVAIL) {
                if (BP_SHOULD_EVICT(node,i)) {
                    // free the basement node
                    BASEMENTNODE bn = node->bp[i].ptr;
                    OMT curr_omt = BLB_BUFFER(node, i);
                    toku_omt_free_items(curr_omt);
                    destroy_basement_node(bn);

                    toku_free(node->bp[i].ptr);
                    node->bp[i].ptr = NULL;
                    BP_STATE(node,i) = PT_ON_DISK;
                }
                else {
                    BP_SWEEP_CLOCK(node,i);
                }
            }
            else if (BP_STATE(node,i) == PT_ON_DISK) {
                continue;
            }
            else {
                assert(FALSE);
            }
        }
    }
    *bytes_freed = orig_size - brtnode_memory_size(node);
    return 0;
}


// callback that states if partially reading a node is necessary
// could have just used toku_brtnode_fetch_callback, but wanted to separate the two cases to separate functions
BOOL toku_brtnode_pf_req_callback(void* brtnode_pv, void* read_extraargs) {
    // placeholder for now
    BOOL retval = FALSE;
    BRTNODE node = brtnode_pv;
    struct brtnode_fetch_extra *bfe = read_extraargs;
    if (bfe->type == brtnode_fetch_none) {
        retval = FALSE;
    }
    else if (bfe->type == brtnode_fetch_all) {
        retval = FALSE;
        for (int i = 0; i < node->n_children; i++) {
            BP_TOUCH_CLOCK(node,i);
        }
        for (int i = 0; i < node->n_children; i++) {
            // if we find a partition that is not available,
            // then a partial fetch is required because
            // the entire node must be made available
            if (BP_STATE(node,i) != PT_AVAIL) {
                retval = TRUE;
                break;
            }
        }
    }
    else if (bfe->type == brtnode_fetch_subset) {
        // we do not take into account prefetching yet
        // as of now, if we need a subset, the only thing
        // we can possibly require is a single basement node
        // we find out what basement node the query cares about
        // and check if it is available
        assert(bfe->brt);
        assert(bfe->search);
        bfe->child_to_read = toku_brt_search_which_child(
            bfe->brt,
            node,
            bfe->search
            );
        BP_TOUCH_CLOCK(node,bfe->child_to_read);
        retval = (BP_STATE(node,bfe->child_to_read) != PT_AVAIL);
    }
    else {
        // we have a bug. The type should be known
        assert(FALSE);
    }
    return retval;
}

// callback for partially reading a node
// could have just used toku_brtnode_fetch_callback, but wanted to separate the two cases to separate functions
int toku_brtnode_pf_callback(void* brtnode_pv, void* read_extraargs, int fd, long* sizep) {
    BRTNODE node = brtnode_pv;
    struct brtnode_fetch_extra *bfe = read_extraargs;
    // there must be a reason this is being called. If we get a garbage type or the type is brtnode_fetch_none,
    // then something went wrong
    assert((bfe->type == brtnode_fetch_subset) || (bfe->type == brtnode_fetch_all));
    // TODO: possibly cilkify expensive operations in this loop
    // TODO: review this with others to see if it can be made faster
    for (int i = 0; i < node->n_children; i++) {
        if (BP_STATE(node,i) == PT_AVAIL) {
            continue;
        }
        if (toku_brtnode_partition_state(bfe, i) == PT_AVAIL) {
            if (BP_STATE(node,i) == PT_COMPRESSED) {
                //
                // decompress the subblock
                //
                toku_deserialize_bp_from_compressed(node, i);
            }
            else if (BP_STATE(node,i) == PT_ON_DISK) {
                toku_deserialize_bp_from_disk(node, i, fd, bfe);
            }
            else {
                assert(FALSE);
            }
        }
    }
    *sizep = brtnode_memory_size(node);
    return 0;
}



static int
leafval_heaviside_le (u_int32_t klen, void *kval,
		      struct cmd_leafval_heaviside_extra *be)
    __attribute__((__warn_unused_result__));

static int
leafval_heaviside_le (u_int32_t klen, void *kval,
		      struct cmd_leafval_heaviside_extra *be) {
    BRT t = be->t;
    DBT dbt;
    DBT const * const key = be->key;
    return t->compare_fun(t->db,
			  toku_fill_dbt(&dbt, kval, klen),
			  key);
}

//TODO: #1125 optimize
int
toku_cmd_leafval_heaviside (OMTVALUE lev, void *extra) {
    LEAFENTRY le=lev;
    struct cmd_leafval_heaviside_extra *be = extra;
    u_int32_t keylen;
    void*     key = le_key_and_len(le, &keylen);
    return leafval_heaviside_le(keylen, key,
				be);
}

static int
brt_compare_pivot(BRT brt, const DBT *key, bytevec ck)
    __attribute__((__warn_unused_result__));

static int
brt_compare_pivot(BRT brt, const DBT *key, bytevec ck)
{
    int cmp;
    DBT mydbt;
    struct kv_pair *kv = (struct kv_pair *) ck;
    cmp = brt->compare_fun(brt->db, key, toku_fill_dbt(&mydbt, kv_pair_key(kv), kv_pair_keylen(kv)));
    return cmp;
}

// destroys the internals of the brtnode, but it does not free the values
// that are stored
// this is common functionality for toku_brtnode_free and rebalance_brtnode_leaf
// MUST NOT do anything besides free the structures that have been allocated
void toku_destroy_brtnode_internals(BRTNODE node)
{
    for (int i=0; i<node->n_children-1; i++) {
	toku_free(node->childkeys[i]);
    }
    toku_free(node->childkeys);
    node->childkeys = NULL;

    for (int i=0; i < node->n_children; i++) {
        if (BP_STATE(node,i) == PT_AVAIL) {
            if (node->height > 0) {
                if (BNC_BUFFER(node,i)) {
                    toku_fifo_free(&BNC_BUFFER(node,i));
                }
            }
            else {
                BASEMENTNODE bn = node->bp[i].ptr;
                destroy_basement_node(bn);
            }
        }
        else if (BP_STATE(node,i) == PT_COMPRESSED) {
            struct sub_block* sb = node->bp[i].ptr;
            toku_free(sb->compressed_ptr);
        }
        else {
            assert(node->bp[i].ptr == NULL);
        }
        // otherwise, there is nothing
        toku_free(node->bp[i].ptr);
    }
    toku_free(node->bp);
    node->bp = NULL;

}


/* Frees a node, including all the stuff in the hash table. */
void toku_brtnode_free (BRTNODE *nodep) {

    //TODO: #1378 Take omt lock (via brtnode) around call to toku_omt_destroy().

    BRTNODE node=*nodep;
    if (node->height == 0) {
	for (int i = 0; i < node->n_children; i++) {
            if (BP_STATE(node,i) == PT_AVAIL) {
                OMT curr_omt = BLB_BUFFER(node, i);
                toku_omt_free_items(curr_omt);
            }
	}
    }
    toku_destroy_brtnode_internals(node);
    toku_free(node);
    *nodep=0;
}

static void
brtheader_destroy(struct brt_header *h) {
    if (!h->panic) assert(!h->checkpoint_header);

    //header and checkpoint_header have same Blocktable pointer
    //cannot destroy since it is still in use by CURRENT
    if (h->type == BRTHEADER_CHECKPOINT_INPROGRESS) h->blocktable = NULL; 
    else {
	assert(h->type == BRTHEADER_CURRENT);
	toku_blocktable_destroy(&h->blocktable);
	if (h->descriptor.dbt.data) toku_free(h->descriptor.dbt.data);
    }
}

static int
brtheader_alloc(struct brt_header **hh) {
    int r = 0;
    if ((CALLOC(*hh))==0) {
	assert(errno==ENOMEM);
	r = ENOMEM;
    }
    return r;
}

// Make a copy of the header for the purpose of a checkpoint
static void
brtheader_copy_for_checkpoint(struct brt_header *h, LSN checkpoint_lsn) {
    assert(h->type == BRTHEADER_CURRENT);
    assert(h->checkpoint_header == NULL);
    assert(h->panic==0);

    struct brt_header* XMALLOC(ch);
    *ch = *h; //Do a shallow copy
    ch->type = BRTHEADER_CHECKPOINT_INPROGRESS; //Different type
    //printf("checkpoint_lsn=%" PRIu64 "\n", checkpoint_lsn.lsn);
    ch->checkpoint_lsn = checkpoint_lsn;
    ch->panic_string = NULL;
    
    //ch->blocktable is SHARED between the two headers
    h->checkpoint_header = ch;
}

static void
brtheader_free(struct brt_header *h)
{
    brtheader_destroy(h);
    toku_free(h);
}
	
void
toku_brtheader_free (struct brt_header *h) {
    brtheader_free(h);
}

void
toku_initialize_empty_brtnode (BRTNODE n, BLOCKNUM nodename, int height, int num_children, int layout_version, unsigned int nodesize, unsigned int flags)
// Effect: Fill in N as an empty brtnode.
{
    assert(layout_version != 0);
    assert(height >= 0);

    n->max_msn_applied_to_node_on_disk = MIN_MSN;    // correct value for root node, harmless for others
    n->max_msn_applied_to_node_in_memory = MIN_MSN;  // correct value for root node, harmless for others
    n->nodesize = nodesize;
    n->flags = flags;
    n->thisnodename = nodename;
    n->layout_version	       = layout_version;
    n->layout_version_original = layout_version;
    n->layout_version_read_from_disk = layout_version;
    n->height = height;
    n->dirty = 1;
    n->totalchildkeylens = 0;
    n->childkeys = 0;
    n->bp = 0;
    n->n_children = num_children; 
    n->bp_offset = 0;

    if (num_children > 0) {
        XMALLOC_N(num_children-1, n->childkeys);
        XMALLOC_N(num_children, n->bp);
	for (int i = 0; i < num_children; i++) {
            BP_FULLHASH(n,i)=0;
            BP_HAVE_FULLHASH(n,i)=FALSE;
            BP_BLOCKNUM(n,i).b=0;
            BP_STATE(n,i) = PT_INVALID;
            BP_OFFSET(n,i) = 0;
            BP_SUBTREE_EST(n,i) = zero_estimates;
            BP_INIT_TOUCHED_CLOCK(n, i);
            n->bp[i].ptr = NULL;
            if (height > 0) {
                n->bp[i].ptr = toku_malloc(sizeof(struct brtnode_nonleaf_childinfo));
                memset(n->bp[i].ptr, 0, sizeof(struct brtnode_nonleaf_childinfo));
                int r = toku_fifo_create(&BNC_BUFFER(n,i));
                assert_zero(r);
                BNC_NBYTESINBUF(n,i) = 0;
            }
            else {
                n->bp[i].ptr = toku_xmalloc(sizeof(struct brtnode_leaf_basement_node));
                BASEMENTNODE bn = n->bp[i].ptr;
                memset(bn, 0, sizeof(struct brtnode_leaf_basement_node));
                toku_setup_empty_bn(bn);
            }
	}
    }
}

static void
brt_init_new_root(BRT brt, BRTNODE nodea, BRTNODE nodeb, DBT splitk, CACHEKEY *rootp, BRTNODE *newrootp)
// Effect:  Create a new root node whose two children are NODEA and NODEB, and the pivotkey is SPLITK.
//  Store the new root's identity in *ROOTP, and the node in *NEWROOTP.
//  Unpin nodea and nodeb.
//  Leave the new root pinned.
{
    BRTNODE XMALLOC(newroot);
    int new_height = nodea->height+1;
    BLOCKNUM newroot_diskoff;
    toku_allocate_blocknum(brt->h->blocktable, &newroot_diskoff, brt->h);
    assert(newroot);
    *rootp=newroot_diskoff;
    assert(new_height > 0);
    toku_initialize_empty_brtnode (newroot, newroot_diskoff, new_height, 2, brt->h->layout_version, brt->h->nodesize, brt->flags);
    //printf("new_root %lld %d %lld %lld\n", newroot_diskoff, newroot->height, nodea->thisnodename, nodeb->thisnodename);
    //printf("%s:%d Splitkey=%p %s\n", __FILE__, __LINE__, splitkey, splitkey);
    newroot->childkeys[0] = splitk.data;
    newroot->totalchildkeylens=splitk.size;
    BP_BLOCKNUM(newroot,0)=nodea->thisnodename;
    BP_BLOCKNUM(newroot,1)=nodeb->thisnodename;
    BP_HAVE_FULLHASH(newroot, 0) = FALSE;
    BP_HAVE_FULLHASH(newroot, 1) = FALSE;
    fixup_child_estimates(newroot, 0, nodea, TRUE);
    fixup_child_estimates(newroot, 1, nodeb, TRUE);
    {
	MSN msna = nodea->max_msn_applied_to_node_in_memory;
	MSN msnb = nodeb->max_msn_applied_to_node_in_memory;
	invariant(msna.msn == msnb.msn);
	newroot->max_msn_applied_to_node_in_memory = msna;
    }
    BP_STATE(newroot,0) = PT_AVAIL;
    BP_STATE(newroot,1) = PT_AVAIL;
    newroot->dirty = 1;
    toku_unpin_brtnode(brt, nodea);
    toku_unpin_brtnode(brt, nodeb);
    //printf("%s:%d put %lld\n", __FILE__, __LINE__, newroot_diskoff);
    u_int32_t fullhash = toku_cachetable_hash(brt->cf, newroot_diskoff);
    newroot->fullhash = fullhash;
    toku_cachetable_put(brt->cf, newroot_diskoff, fullhash, newroot, brtnode_memory_size(newroot),
			toku_brtnode_flush_callback, toku_brtnode_pe_callback, brt->h);
    *newrootp = newroot;
}

void 
toku_create_new_brtnode (BRT t, BRTNODE *result, int height, int n_children) {
    assert(t->h->nodesize > 0);
    if (height == 0) 
        assert(n_children > 0);

    BLOCKNUM name;
    toku_allocate_blocknum(t->h->blocktable, &name, t->h);

    BRTNODE XMALLOC(n);
    toku_initialize_empty_brtnode(n, name, height, n_children, t->h->layout_version, t->h->nodesize, t->flags);
    assert(n->nodesize > 0);

    u_int32_t fullhash = toku_cachetable_hash(t->cf, n->thisnodename);
    n->fullhash = fullhash;
    int r = toku_cachetable_put(t->cf, n->thisnodename, fullhash,
                            n, brtnode_memory_size(n),
                            toku_brtnode_flush_callback, toku_brtnode_pe_callback, t->h);
    assert_zero(r);

    *result = n;
}

static void
init_childinfo(BRTNODE node, int childnum, BRTNODE child) {
    BP_BLOCKNUM(node,childnum) = child->thisnodename;
    BP_HAVE_FULLHASH(node,childnum) = FALSE;
    BP_STATE(node,childnum) = PT_AVAIL;
    BP_OFFSET(node,childnum) = 0;
    BP_SUBTREE_EST(node,childnum) = zero_estimates;
    node->bp[childnum].ptr = toku_malloc(sizeof(struct brtnode_nonleaf_childinfo));
    assert(node->bp[childnum].ptr);
    BNC_NBYTESINBUF(node,childnum) = 0;
    int r = toku_fifo_create(&BNC_BUFFER(node,childnum));
    resource_assert_zero(r);
}

static void
init_childkey(BRTNODE node, int childnum, struct kv_pair *pivotkey, size_t pivotkeysize) {
    node->childkeys[childnum] = pivotkey;
    node->totalchildkeylens += pivotkeysize;
}

static struct kv_pair const *prepivotkey (BRTNODE node, int childnum, struct kv_pair const * const lower_bound_exclusive) {
    if (childnum==0)
	return lower_bound_exclusive;
    else {
	return node->childkeys[childnum-1];
    }
}

static struct kv_pair const *postpivotkey (BRTNODE node, int childnum, struct kv_pair const * const upper_bound_inclusive) {
    if (childnum+1 == node->n_children)
	return upper_bound_inclusive;
    else {
	return node->childkeys[childnum];
    }
}
static struct pivot_bounds next_pivot_keys (BRTNODE node, int childnum, struct pivot_bounds const * const old_pb) {
    struct pivot_bounds pb = {.lower_bound_exclusive = prepivotkey(node, childnum, old_pb->lower_bound_exclusive),
			      .upper_bound_inclusive = postpivotkey(node, childnum, old_pb->upper_bound_inclusive)};
    return pb;
}

// append a child node to a parent node
void
toku_brt_nonleaf_append_child(BRTNODE node, BRTNODE child, struct kv_pair *pivotkey, size_t pivotkeysize) {
    int childnum = node->n_children;
    node->n_children++;
    XREALLOC_N(node->n_children, node->bp);
    init_childinfo(node, childnum, child);
    XREALLOC_N(node->n_children-1, node->childkeys);
    if (pivotkey) {
	invariant(childnum > 0);
	init_childkey(node, childnum-1, pivotkey, pivotkeysize);
    }
    node->dirty = 1;
}

static u_int64_t
brtleaf_disk_size(BRTNODE node)
// Effect: get the disk size of a leafentry
{
    assert(node->height == 0);
    toku_assert_entire_node_in_memory(node);
    u_int64_t retval = 0;
    int i;
    for (i = 0; i < node->n_children; i++) {
	OMT curr_buffer = BLB_BUFFER(node, i);
	u_int32_t n_leafentries = toku_omt_size(curr_buffer);
	u_int32_t j;
	for (j=0; j < n_leafentries; j++) {
	    OMTVALUE v;
	    LEAFENTRY curr_le = NULL;
	    int r = toku_omt_fetch(curr_buffer, j, &v, NULL);
	    curr_le = v;
	    assert_zero(r);
	    retval += leafentry_disksize(curr_le);
	}
    }
    return retval;
}

static void
brtleaf_get_split_loc(
    BRTNODE node, 
    u_int64_t sumlesizes, 
    int* bn_index, 
    int* le_index 
    )
// Effect: Find the location within a leaf node where we want to perform a split
{
    assert(node->height == 0);
    u_int32_t size_so_far = 0;
    int i;
    for (i = 0; i < node->n_children; i++) {
	OMT curr_buffer = BLB_BUFFER(node, i);
	u_int32_t n_leafentries = toku_omt_size(curr_buffer);
	u_int32_t j;
	for (j=0; j < n_leafentries; j++) {
	    LEAFENTRY curr_le = NULL;
	    OMTVALUE v;
	    int r = toku_omt_fetch(curr_buffer, j, &v, NULL);
	    curr_le = v;
	    assert_zero(r);
	    size_so_far += leafentry_disksize(curr_le);
	    if (size_so_far >= sumlesizes/2) {
		*bn_index = i;
		*le_index = j;
		goto exit;
	    }
	}
    }
exit:
    return;
}

// TODO: (Zardosht) possibly get rid of this function and use toku_omt_split_at in
// brtleaf_split
static void
move_leafentries(
    OMT* dest_omt,
    OMT src_omt,
    u_int32_t lbi, //lower bound inclusive
    u_int32_t ube, //upper bound exclusive
    SUBTREE_EST se_diff,
    u_int32_t* num_bytes_moved
    )
//Effect: move leafentries in the range [lbi, upe) from src_omt to newly created dest_omt
{
    OMTVALUE *MALLOC_N(ube-lbi, new_le);
    u_int32_t i = 0;
    *num_bytes_moved = 0;
    for (i = lbi; i < ube; i++) {
	LEAFENTRY curr_le = NULL;
	curr_le = fetch_from_buf(src_omt, i);

	se_diff->nkeys++;
	se_diff->ndata++;
	se_diff->dsize += le_keylen(curr_le) + le_latest_vallen(curr_le);

	*num_bytes_moved += OMT_ITEM_OVERHEAD + leafentry_disksize(curr_le);
	new_le[i-lbi] = curr_le;
    }

    int r = toku_omt_create_steal_sorted_array(
	dest_omt,
	&new_le,
	ube-lbi,
	ube-lbi
	);
    assert_zero(r);
    // now remove the elements from src_omt
    for (i=ube-1; i >= lbi; i--) {
	toku_omt_delete_at(src_omt,i);
    }
}

static void
brtleaf_split (BRT t, BRTNODE node, BRTNODE *nodea, BRTNODE *nodeb, DBT *splitk, BOOL create_new_node)
// Effect: Split a leaf node.
{
    BRTNODE B;
    //printf("%s:%d splitting leaf %" PRIu64 " which is size %u (targetsize = %u)\n", __FILE__, __LINE__, node->thisnodename.b, toku_serialize_brtnode_size(node), node->nodesize);

    assert(node->height==0);
    assert(node->nodesize>0);
    toku_assert_entire_node_in_memory(node);
    MSN max_msn_applied_to_node = node->max_msn_applied_to_node_in_memory;

    //printf("%s:%d A is at %lld\n", __FILE__, __LINE__, A->thisnodename);
    //printf("%s:%d B is at %lld nodesize=%d\n", __FILE__, __LINE__, B->thisnodename, B->nodesize);


    // variables that say where we will do the split. We do it in the basement node indexed at 
    // at split_node, and at the index split_at_in_node within that basement node.
    int split_node = 0;
    int split_at_in_node = 0;
    {
	{
	    // TODO: (Zardosht) see if we can/should make this faster, we iterate over the rows twice
	    u_int64_t sumlesizes=0;
	    sumlesizes = brtleaf_disk_size(node);
	    // TODO: (Zardosht) #3537, figure out serial insertion optimization again later
	    // split in half
	    brtleaf_get_split_loc(
		node,
		sumlesizes,
		&split_node,
		&split_at_in_node
		);
	}
	// Now we know where we are going to break it
	// the two nodes will have a total of n_children+1 basement nodes
	// and n_children-1 pivots
	// the left node, node, will have split_node+1 basement nodes
	// the right node, B, will have n_children-split_node basement nodes
	// the pivots of node will be the first split_node pivots that originally exist
	// the pivots of B will be the last (n_children - 1 - split_node) pivots that originally exist

	//set up the basement nodes in the new node
	int num_children_in_node = split_node + 1;
	int num_children_in_b = node->n_children - split_node;
	if (create_new_node) {
	    toku_create_new_brtnode(
		t,
		&B,
		0,
		num_children_in_b
		);
	    assert(B->nodesize>0);
	}
	else {
	    B = *nodeb;
	    REALLOC_N(num_children_in_b-1, B->childkeys);
	    REALLOC_N(num_children_in_b,   B->bp);
            for (int i = 0; i < num_children_in_b; i++) {
                BP_STATE(B,i) = PT_AVAIL;
                BP_OFFSET(B,i) = 0;
                BP_BLOCKNUM(B,i).b = 0;
                BP_FULLHASH(B,i) = 0;
                BP_HAVE_FULLHASH(B,i) = FALSE;
                BP_SUBTREE_EST(B,i)= zero_estimates;
                B->bp[i].ptr = toku_xmalloc(sizeof(struct brtnode_leaf_basement_node));
                BASEMENTNODE bn = B->bp[i].ptr;
                toku_setup_empty_bn(bn);
            }
	}
	//
	// first move all the data
	//

	// handle the move of a subset of data in split_node from node to B

        BP_STATE(B,0) = PT_AVAIL;
	struct subtree_estimates se_diff = zero_estimates;
	u_int32_t diff_size = 0;
	destroy_basement_node ((BASEMENTNODE)B->bp[0].ptr); // Destroy B's empty OMT, so I can rebuild it from an array
	move_leafentries(
	    &BLB_BUFFER(B, 0),
	    BLB_BUFFER(node, split_node),
	    split_at_in_node+1,
	    toku_omt_size(BLB_BUFFER(node, split_node)),
	    &se_diff,
	    &diff_size
	    );
	BLB_NBYTESINBUF(node, split_node) -= diff_size;
	BLB_NBYTESINBUF(B, 0) += diff_size;
	subtract_estimates(&BP_SUBTREE_EST(node,split_node), &se_diff);
	add_estimates(&BP_SUBTREE_EST(B,0), &se_diff);

	// move the rest of the basement nodes
	int curr_dest_bn_index = 1;
	for (int i = num_children_in_node; i < node->n_children; i++, curr_dest_bn_index++) {
	    destroy_basement_node((BASEMENTNODE)B->bp[curr_dest_bn_index].ptr);
            toku_free(B->bp[curr_dest_bn_index].ptr);
	    B->bp[curr_dest_bn_index] = node->bp[i];
	}
	node->n_children = num_children_in_node;
	B->n_children = num_children_in_b;

	//
	// now handle the pivots
	//

	// make pivots in B
	for (int i=0; i < num_children_in_b-1; i++) {
	    B->childkeys[i] = node->childkeys[i+split_node];
	    B->totalchildkeylens += toku_brt_pivot_key_len(node->childkeys[i+split_node]);
	    node->totalchildkeylens -= toku_brt_pivot_key_len(node->childkeys[i+split_node]);
	    node->childkeys[i+split_node] = NULL;
	}
	REALLOC_N(num_children_in_node,	  node->bp);
	REALLOC_N(num_children_in_node-1, node->childkeys);

	toku_brt_leaf_reset_calc_leaf_stats(node);
	toku_brt_leaf_reset_calc_leaf_stats(B);
    }

    if (splitk) {
	memset(splitk, 0, sizeof *splitk);
	OMTVALUE lev = 0;
	int r=toku_omt_fetch(BLB_BUFFER(node, split_node), toku_omt_size(BLB_BUFFER(node, split_node))-1, &lev, NULL);
	assert_zero(r); // that fetch should have worked.
	LEAFENTRY le=lev;
	splitk->size = le_keylen(le);
	splitk->data = kv_pair_malloc(le_key(le), le_keylen(le), 0, 0);
	splitk->flags=0;
    }

    node->max_msn_applied_to_node_in_memory = max_msn_applied_to_node;
    B	->max_msn_applied_to_node_in_memory = max_msn_applied_to_node;

    node->dirty = 1;
    B->dirty = 1;

    *nodea = node;
    *nodeb = B;

    //printf("%s:%d new sizes Node %" PRIu64 " size=%u omtsize=%d dirty=%d; Node %" PRIu64 " size=%u omtsize=%d dirty=%d\n", __FILE__, __LINE__,
    //		 node->thisnodename.b, toku_serialize_brtnode_size(node), node->height==0 ? (int)(toku_omt_size(node->u.l.buffer)) : -1, node->dirty,
    //		 B   ->thisnodename.b, toku_serialize_brtnode_size(B   ), B   ->height==0 ? (int)(toku_omt_size(B   ->u.l.buffer)) : -1, B->dirty);
    //toku_dump_brtnode(t, node->thisnodename, 0, NULL, 0, NULL, 0);
    //toku_dump_brtnode(t, B   ->thisnodename, 0, NULL, 0, NULL, 0);
}

static void
brt_nonleaf_split (BRT t, BRTNODE node, BRTNODE *nodea, BRTNODE *nodeb, DBT *splitk)
// Effect: node must be a node-leaf node.  It is split into two nodes, and the fanout is split between them.
//    Sets splitk->data pointer to a malloc'd value
//    Sets nodea, and nodeb to the two new nodes.
//    The caller must replace the old node with the two new nodes.
//    This function will definitely reduce the number of children for the node,
//    but it does not guarantee that the resulting nodes are smaller than nodesize.
{
    VERIFY_NODE(t,node);
    toku_assert_entire_node_in_memory(node);
    int old_n_children = node->n_children;
    int n_children_in_a = old_n_children/2;
    int n_children_in_b = old_n_children-n_children_in_a;
    MSN max_msn_applied_to_node = node->max_msn_applied_to_node_in_memory;
    BRTNODE B;
    assert(node->height>0);
    assert(node->n_children>=2); // Otherwise, how do we split?	 We need at least two children to split. */
    toku_create_new_brtnode(t, &B, node->height, n_children_in_b);
    {
	/* The first n_children_in_a go into node a.
	 * That means that the first n_children_in_a-1 keys go into node a.
	 * The splitter key is key number n_children_in_a */
	int i;

	for (i=n_children_in_a; i<old_n_children; i++) {

	    int targchild = i-n_children_in_a;
            // TODO: Figure out better way to handle this
            // the problem is that toku_create_new_brtnode for B creates
            // all the data structures, whereas we really don't want it to fill
            // in anything for the bp's.
            // Now we have to go free what it just created so we can
            // slide the bp over            
            if (BNC_BUFFER(B,targchild)) {
                toku_fifo_free(&BNC_BUFFER(B,targchild));
            }
            toku_free(B->bp[targchild].ptr);
            // now move the bp over
            B->bp[targchild] = node->bp[i];
            memset(&node->bp[i], 0, sizeof(node->bp[0]));
            
	    // Delete a child, removing the preceeding pivot key.  The child number must be > 0
	    {
		assert(i>0);
		if (i>n_children_in_a) {
		    B->childkeys[targchild-1] = node->childkeys[i-1];
		    B->totalchildkeylens += toku_brt_pivot_key_len(node->childkeys[i-1]);
		    node->totalchildkeylens -= toku_brt_pivot_key_len(node->childkeys[i-1]);
		    node->childkeys[i-1] = 0;
		}
	    }
	}

	node->n_children=n_children_in_a;

	splitk->data = (void*)(node->childkeys[n_children_in_a-1]);
	splitk->size = toku_brt_pivot_key_len(node->childkeys[n_children_in_a-1]);
	node->totalchildkeylens -= toku_brt_pivot_key_len(node->childkeys[n_children_in_a-1]);

	REALLOC_N(n_children_in_a,   node->bp);
	REALLOC_N(n_children_in_a-1, node->childkeys);

    }

    node->max_msn_applied_to_node_in_memory = max_msn_applied_to_node;
    B	->max_msn_applied_to_node_in_memory = max_msn_applied_to_node;

    node->dirty = 1;
    B	->dirty = 1;
    toku_assert_entire_node_in_memory(node);
    toku_assert_entire_node_in_memory(B);
    VERIFY_NODE(t,node);
    VERIFY_NODE(t,B);
    *nodea = node;
    *nodeb = B;
}

/* NODE is a node with a child.
 * childnum was split into two nodes childa, and childb.  childa is the same as the original child.  childb is a new child.
 * We must slide things around, & move things from the old table to the new tables.
 * Requires: the CHILDNUMth buffer of node is empty.
 * We don't push anything down to children.  We split the node, and things land wherever they land.
 * We must delete the old buffer (but the old child is already deleted.)
 * On return, the new children are unpinned.
 */
static void
handle_split_of_child (BRT t, BRTNODE node, int childnum,
		       BRTNODE childa, BRTNODE childb,
		       DBT *splitk /* the data in the childsplitk is alloc'd and is consumed by this call. */
		       )
{
    assert(node->height>0);
    assert(0 <= childnum && childnum < node->n_children);
    toku_assert_entire_node_in_memory(node);
    toku_assert_entire_node_in_memory(childa);
    toku_assert_entire_node_in_memory(childb);
    int	old_count = BNC_NBYTESINBUF(node, childnum);
    assert(old_count==0);
    int cnum;
    int r;
    WHEN_NOT_GCOV(
    if (toku_brt_debug_mode) {
	int i;
	printf("%s:%d Child %d splitting on %s\n", __FILE__, __LINE__, childnum, (char*)splitk->data);
	printf("%s:%d oldsplitkeys:", __FILE__, __LINE__);
	for(i=0; i<node->n_children-1; i++) printf(" %s", (char*)node->childkeys[i]);
	printf("\n");
    }
		  )

    node->dirty = 1;

    XREALLOC_N(node->n_children+1, node->bp);
    XREALLOC_N(node->n_children, node->childkeys);
    // Slide the children over.
    // suppose n_children is 10 and childnum is 5, meaning node->childnum[5] just got split
    // this moves node->bp[6] through node->bp[9] over to
    // node->bp[7] through node->bp[10]
    for (cnum=node->n_children; cnum>childnum+1; cnum--) {
        node->bp[cnum] = node->bp[cnum-1];
    }
    memset(&node->bp[childnum+1],0,sizeof(node->bp[0]));
    node->n_children++;

    assert(BP_BLOCKNUM(node, childnum).b==childa->thisnodename.b); // use the same child

    BP_BLOCKNUM(node, childnum+1) = childb->thisnodename;
    BP_HAVE_FULLHASH(node, childnum+1) = TRUE;
    BP_FULLHASH(node, childnum+1) = childb->fullhash;
    BP_SUBTREE_EST(node,childnum+1) = zero_estimates;
    BP_STATE(node,childnum+1) = PT_AVAIL;
    BP_OFFSET(node,childnum+1) = 0;
    fixup_child_estimates(node, childnum,   childa, TRUE);
    fixup_child_estimates(node, childnum+1, childb, TRUE);

    node->bp[childnum+1].ptr = toku_malloc(sizeof(struct brtnode_nonleaf_childinfo));
    assert(node->bp[childnum+1].ptr);
    r=toku_fifo_create(&BNC_BUFFER(node,childnum+1)); assert_zero(r);
    BNC_NBYTESINBUF(node, childnum+1) = 0;

    // Slide the keys over
    {
	struct kv_pair *pivot = splitk->data;

	for (cnum=node->n_children-2; cnum>childnum; cnum--) {
	    node->childkeys[cnum] = node->childkeys[cnum-1];
	}
	//if (logger) assert((t->flags&TOKU_DB_DUPSORT)==0); // the setpivot is wrong for TOKU_DB_DUPSORT, so recovery will be broken.
	node->childkeys[childnum]= pivot;
	node->totalchildkeylens += toku_brt_pivot_key_len(pivot);
    }

    WHEN_NOT_GCOV(
    if (toku_brt_debug_mode) {
	int i;
	printf("%s:%d splitkeys:", __FILE__, __LINE__);
	for(i=0; i<node->n_children-2; i++) printf(" %s", (char*)node->childkeys[i]);
	printf("\n");
    }
		  )

    /* Keep pushing to the children, but not if the children would require a pushdown */
    toku_assert_entire_node_in_memory(node);
    toku_assert_entire_node_in_memory(childa);
    toku_assert_entire_node_in_memory(childb);

    VERIFY_NODE(t, node);
    VERIFY_NODE(t, childa);
    VERIFY_NODE(t, childb);

    toku_unpin_brtnode(t, childa); 
    toku_unpin_brtnode(t, childb);
}

static void
brt_split_child (BRT t, BRTNODE node, int childnum, BOOL *did_react)
{
    if (0) {
	printf("%s:%d Node %" PRId64 "->u.n.n_children=%d estimates=", __FILE__, __LINE__, node->thisnodename.b, node->n_children);
	//int i;
	//for (i=0; i<node->u.n.n_children; i++) printf(" %" PRIu64, BNC_SUBTREE_LEAFENTRY_ESTIMATE(node, i));
	printf("\n");
    }
    assert(node->height>0);
    BRTNODE child;
    assert(BNC_NBYTESINBUF(node, childnum)==0); // require that the buffer for this child is empty
    {
	void *childnode_v;
	// For now, don't use toku_pin_brtnode since we aren't yet prepared to deal with the TRY_AGAIN, and we don't have to apply all the messages above to do this split operation.
        struct brtnode_fetch_extra bfe;
        fill_bfe_for_full_read(&bfe, t->h);
	int r = toku_cachetable_get_and_pin(t->cf,
					    BP_BLOCKNUM(node, childnum),
					    compute_child_fullhash(t->cf, node, childnum),
					    &childnode_v,
					    NULL,
					    toku_brtnode_flush_callback, 
					    toku_brtnode_fetch_callback, 
					    toku_brtnode_pe_callback,
                                            toku_brtnode_pf_req_callback,
                                            toku_brtnode_pf_callback,
					    &bfe,
					    t->h);
	assert(r==0);
	child = childnode_v;
	assert(child->thisnodename.b!=0);
	VERIFY_NODE(t,child);
    }

    BRTNODE nodea, nodeb;
    DBT splitk;
    // printf("%s:%d node %" PRIu64 "->u.n.n_children=%d height=%d\n", __FILE__, __LINE__, node->thisnodename.b, node->u.n.n_children, node->height);
    assert(t->h->nodesize>=node->nodesize); /* otherwise we might be in trouble because the nodesize shrank. */
    if (child->height==0) {
	brtleaf_split(t, child, &nodea, &nodeb, &splitk, TRUE);
    } else {
	brt_nonleaf_split(t, child, &nodea, &nodeb, &splitk);
    }
    // printf("%s:%d child did split\n", __FILE__, __LINE__);
    *did_react = TRUE;
    {
	handle_split_of_child (t, node, childnum, nodea, nodeb, &splitk);
	if (0) {
	    printf("%s:%d Node %" PRId64 "->n_children=%d estimates=", __FILE__, __LINE__, node->thisnodename.b, node->n_children);
	    //int i;
	    //for (i=0; i<node->u.n.n_children; i++) printf(" %" PRIu64, BNC_SUBTREE_LEAFENTRY_ESTIMATE(node, i));
	    printf("\n");
	}
    }
}

static void
bump_nkeys (SUBTREE_EST a, int direction) {
    int keybump=direction;
    a->nkeys += keybump;
    assert(a->exact);
}

static void
brt_leaf_delete_leafentry (
    BASEMENTNODE bn,
    SUBTREE_EST se,
    u_int32_t idx, 
    LEAFENTRY le
    )
// Effect: Delete leafentry
//   idx is the location where it is
//   le is the leafentry to be deleted
{
    // Figure out if one of the other keys is the same key
    bump_nkeys(se, -1);

    {
	int r = toku_omt_delete_at(bn->buffer, idx);
	assert(r==0);
    }

    bn->n_bytes_in_buffer -= OMT_ITEM_OVERHEAD + leafentry_disksize(le);

    {
	u_int32_t oldlen = le_latest_vallen(le) + le_keylen(le);
	assert(se->dsize >= oldlen);
	se->dsize -= oldlen;
    }
    assert(se->dsize < (1U<<31)); // make sure we didn't underflow
    se->ndata --;
}

void
brt_leaf_apply_cmd_once (
    BASEMENTNODE bn, 
    SUBTREE_EST se,
    const BRT_MSG cmd,
    u_int32_t idx, 
    LEAFENTRY le, 
    TOKULOGGER logger
    )
// Effect: Apply cmd to leafentry (msn is ignored)
//   idx is the location where it goes
//   le is old leafentry
{
    // brt_leaf_check_leaf_stats(node);

    size_t newlen=0, newdisksize=0;
    LEAFENTRY new_le=0;
    {
	OMT snapshot_txnids   = logger ? logger->snapshot_txnids   : NULL;
	OMT live_list_reverse = logger ? logger->live_list_reverse : NULL;
	int r = apply_msg_to_leafentry(cmd, le, &newlen, &newdisksize, &new_le, snapshot_txnids, live_list_reverse);
	assert(r==0);
    }
    if (new_le) assert(newdisksize == leafentry_disksize(new_le));

    if (le && new_le) {
	// If we are replacing a leafentry, then the counts on the estimates remain unchanged, but the size might change
	{
	    u_int32_t oldlen = le_keylen(le) + le_latest_vallen(le);
	    assert(se->dsize >= oldlen);
	    assert(se->dsize < (1U<<31)); // make sure we didn't underflow
	    se->dsize -= oldlen;
	    se->dsize += le_keylen(new_le) + le_latest_vallen(new_le); // add it in two pieces to avoid ugly overflow
	    assert(se->dsize < (1U<<31)); // make sure we didn't underflow
	}

	bn->n_bytes_in_buffer -= OMT_ITEM_OVERHEAD + leafentry_disksize(le);
	
	//printf("%s:%d Added %u-%u got %lu\n", __FILE__, __LINE__, le_keylen(new_le), le_latest_vallen(le), node->u.l.leaf_stats.dsize);
	// the ndata and nkeys remains unchanged

	bn->n_bytes_in_buffer += OMT_ITEM_OVERHEAD + newdisksize;

	{ int r = toku_omt_set_at(bn->buffer, new_le, idx); assert(r==0); }
	toku_free(le);

    } else {
	if (le) {
	    brt_leaf_delete_leafentry (bn, se, idx, le);
	    toku_free(le);
	}
	if (new_le) {

	    int r = toku_omt_insert_at(bn->buffer, new_le, idx);
	    assert(r==0);

	    bn->n_bytes_in_buffer += OMT_ITEM_OVERHEAD + newdisksize;

	    se->dsize += le_latest_vallen(new_le) + le_keylen(new_le);
	    assert(se->dsize < (1U<<31)); // make sure we didn't underflow
	    se->ndata++;
	    // Look at the key to the left and the one to the right.  If both are different then increment nkeys.
	    bump_nkeys(se, +1);
	}
    }
    // brt_leaf_check_leaf_stats(node);

}

static const uint32_t setval_tag = 0xee0ccb99; // this was gotten by doing "cat /dev/random|head -c4|od -x" to get a random number.  We want to make sure that the user actually passes us the setval_extra_s that we passed in.
struct setval_extra_s {
    u_int32_t  tag;
    BOOL did_set_val;
    int	 setval_r;    // any error code that setval_fun wants to return goes here.
    // need arguments for brt_leaf_apply_cmd_once
    BASEMENTNODE bn;
    SUBTREE_EST se;
    MSN msn;	      // captured from original message, not currently used
    XIDS xids;
    const DBT *key;
    u_int32_t idx;
    LEAFENTRY le;
    TOKULOGGER logger;	  
    int made_change;
};

/*
 * If new_val == NULL, we send a delete message instead of an insert.
 * This happens here instead of in do_delete() for consistency.
 * setval_fun() is called from handlerton, passing in svextra_v
 * from setval_extra_s input arg to brt->update_fun().
 */
static void setval_fun (const DBT *new_val, void *svextra_v) {
    struct setval_extra_s *svextra = svextra_v;
    assert(svextra->tag==setval_tag);
    assert(!svextra->did_set_val);
    svextra->did_set_val = TRUE;

    {
	// can't leave scope until brt_leaf_apply_cmd_once if
	// this is a delete
	DBT val;
	BRT_MSG_S msg = { BRT_NONE, svextra->msn, svextra->xids,
			  .u.id={svextra->key, NULL} };
	if (new_val) {
	    msg.type = BRT_INSERT;
	    msg.u.id.val = new_val;
	} else {
	    msg.type = BRT_DELETE_ANY;
	    toku_init_dbt(&val);
	    msg.u.id.val = &val;
	}
	brt_leaf_apply_cmd_once(svextra->bn, svextra->se, &msg,
				svextra->idx, svextra->le,
				svextra->logger);
	svextra->setval_r = 0;
    }
    svextra->made_change = TRUE;
}

static UPDATE_STATUS_S update_status;

void 
toku_update_get_status(UPDATE_STATUS s) {
    *s = update_status;
}

// We are already past the msn filter (in brt_leaf_put_cmd(), which calls do_update()),
// so capturing the msn in the setval_extra_s is not strictly required.	 The alternative
// would be to put a dummy msn in the messages created by setval_fun(), but preserving
// the original msn seems cleaner and it preserves accountability at a lower layer.
static int do_update(BRT t, BASEMENTNODE bn, SUBTREE_EST se, BRT_MSG cmd, int idx,
		     LEAFENTRY le, TOKULOGGER logger, int* made_change) {
    LEAFENTRY le_for_update;
    DBT key;
    const DBT *keyp;
    const DBT *update_function_extra;
    DBT vdbt;
    const DBT *vdbtp;

    // the location of data depends whether this is a regular or
    // broadcast update
    if (cmd->type == BRT_UPDATE) {
	// key is passed in with command (should be same as from le)
	// update function extra is passed in with command
	update_status.updates++;
	keyp = cmd->u.id.key;
	update_function_extra = cmd->u.id.val;
    } else if (cmd->type == BRT_UPDATE_BROADCAST_ALL) {
	// key is not passed in with broadcast, it comes from le
	// update function extra is passed in with command
	assert(le);  // for broadcast updates, we just hit all leafentries
		     // so this cannot be null
	assert(cmd->u.id.key->size == 0);
	update_status.updates_broadcast++;
	keyp = toku_fill_dbt(&key, le_key(le), le_keylen(le));
	update_function_extra = cmd->u.id.val;
    } else {
	assert(FALSE);
    }

    if (le && !le_latest_is_del(le)) {
	// if the latest val exists, use it, and we'll use the leafentry later
	u_int32_t vallen;
	void *valp = le_latest_val_and_len(le, &vallen);
	vdbtp = toku_fill_dbt(&vdbt, valp, vallen);
	le_for_update = le;
    } else {
	// otherwise, the val and leafentry are both going to be null
	vdbtp = NULL;
	le_for_update = NULL;
    }

    struct setval_extra_s setval_extra = {setval_tag, FALSE, 0, bn, se, cmd->msn, cmd->xids,
					  keyp, idx, le_for_update, logger, 0};
    // call handlerton's brt->update_fun(), which passes setval_extra to setval_fun()
    int r = t->update_fun(t->db,
			  keyp,
			  vdbtp,
			  update_function_extra,
			  setval_fun, &setval_extra);

    *made_change = setval_extra.made_change;
    
    // TODO(leif): ensure that really bad return codes actually cause a
    // crash higher up the stack somewhere
    if (r == 0) { r = setval_extra.setval_r; }
    return r;
}

// should be static, but used by test program(s)
static void
brt_leaf_put_cmd (
    BRT t, 
    BASEMENTNODE bn, 
    SUBTREE_EST se, 
    BRT_MSG cmd, 
    int* made_change
    )
// Effect: Put a cmd into a leaf.
// The leaf could end up "too big" or "too small".  The caller must fix that up.
{

    TOKULOGGER logger = toku_cachefile_logger(t->cf);

    LEAFENTRY storeddata;
    OMTVALUE storeddatav=NULL;

    u_int32_t omt_size;
    int r;
    struct cmd_leafval_heaviside_extra be = {t, cmd->u.id.key};
    *made_change = 0;

    unsigned int doing_seqinsert = bn->seqinsert;
    bn->seqinsert = 0;

    switch (cmd->type) {
    case BRT_INSERT_NO_OVERWRITE:
    case BRT_INSERT: {
	u_int32_t idx;
	*made_change = 1;
	if (doing_seqinsert) {
	    idx = toku_omt_size(bn->buffer);
	    r = toku_omt_fetch(bn->buffer, idx-1, &storeddatav, NULL);
	    if (r != 0) goto fz;
	    storeddata = storeddatav;
	    int cmp = toku_cmd_leafval_heaviside(storeddata, &be);
	    if (cmp >= 0) goto fz;
	    r = DB_NOTFOUND;
	} else {
	fz:
	    r = toku_omt_find_zero(bn->buffer, toku_cmd_leafval_heaviside, &be,
				   &storeddatav, &idx, NULL);
	}
	if (r==DB_NOTFOUND) {
	    storeddata = 0;
	} else {
	    assert(r==0);
	    storeddata=storeddatav;
	}
	
	brt_leaf_apply_cmd_once(bn, se, cmd, idx, storeddata, logger);

	// if the insertion point is within a window of the right edge of
	// the leaf then it is sequential
	// window = min(32, number of leaf entries/16)
	{
	u_int32_t s = toku_omt_size(bn->buffer);
	u_int32_t w = s / 16;
	if (w == 0) w = 1;
	if (w > 32) w = 32;

	// within the window?
	if (s - idx <= w)
	    bn->seqinsert = doing_seqinsert + 1;
	}
	break;
    }
    case BRT_DELETE_ANY:
    case BRT_ABORT_ANY:
    case BRT_COMMIT_ANY: {
	u_int32_t idx;
	// Apply to all the matches

	r = toku_omt_find_zero(bn->buffer, toku_cmd_leafval_heaviside, &be,
			       &storeddatav, &idx, NULL);
	if (r == DB_NOTFOUND) break;
	assert(r==0);
	storeddata=storeddatav;

	while (1) {
	    u_int32_t num_leafentries_before = toku_omt_size(bn->buffer);

	    brt_leaf_apply_cmd_once(bn, se, cmd, idx, storeddata, logger);
	    *made_change = 1;

	    { 
		// Now we must find the next leafentry. 
		u_int32_t num_leafentries_after = toku_omt_size(bn->buffer); 
		//idx is the index of the leafentry we just modified.
		//If the leafentry was deleted, we will have one less leafentry in 
		//the omt than we started with and the next leafentry will be at the 
		//same index as the deleted one. Otherwise, the next leafentry will 
		//be at the next index (+1). 
		assert(num_leafentries_before	== num_leafentries_after || 
		       num_leafentries_before-1 == num_leafentries_after); 
		if (num_leafentries_after==num_leafentries_before) idx++; //Not deleted, advance index.

		assert(idx <= num_leafentries_after);
		if (idx == num_leafentries_after) break; //Reached the end of the leaf
		r = toku_omt_fetch(bn->buffer, idx, &storeddatav, NULL); 
		assert_zero(r);
	    } 
	    storeddata=storeddatav;
	    {	// Continue only if the next record that we found has the same key.
		DBT adbt;
		u_int32_t keylen;
		void *keyp = le_key_and_len(storeddata, &keylen);
		if (t->compare_fun(t->db,
				   toku_fill_dbt(&adbt, keyp, keylen),
				   cmd->u.id.key) != 0)
		    break;
	    }
	}

	break;
    }
    case BRT_OPTIMIZE_FOR_UPGRADE:
	*made_change = 1;
	bn->optimized_for_upgrade = *((uint32_t*)(cmd->u.id.val->data)); // record version of software that sent the optimize_for_upgrade message
	// fall through so that optimize_for_upgrade performs rest of the optimize logic
    case BRT_COMMIT_BROADCAST_ALL:
    case BRT_OPTIMIZE:
	// Apply to all leafentries
	omt_size = toku_omt_size(bn->buffer);
	for (u_int32_t idx = 0; idx < omt_size; ) {
	    r = toku_omt_fetch(bn->buffer, idx, &storeddatav, NULL);
	    assert_zero(r);
	    storeddata=storeddatav;
	    int deleted = 0;
	    if (!le_is_clean(storeddata)) { //If already clean, nothing to do.
		brt_leaf_apply_cmd_once(bn, se, cmd, idx, storeddata, logger);
		u_int32_t new_omt_size = toku_omt_size(bn->buffer);
		if (new_omt_size != omt_size) {
		    assert(new_omt_size+1 == omt_size);
		    //Item was deleted.
		    deleted = 1;
		}
		*made_change = 1;
	    }
	    if (deleted)
		omt_size--;
	    else
		idx++;
	}
	assert(toku_omt_size(bn->buffer) == omt_size);

	break;
    case BRT_COMMIT_BROADCAST_TXN:
    case BRT_ABORT_BROADCAST_TXN:
	// Apply to all leafentries if txn is represented
	omt_size = toku_omt_size(bn->buffer);
	for (u_int32_t idx = 0; idx < omt_size; ) {
	    r = toku_omt_fetch(bn->buffer, idx, &storeddatav, NULL); 
	    assert_zero(r);
	    storeddata=storeddatav;
	    int deleted = 0;
	    if (le_has_xids(storeddata, cmd->xids)) {
		brt_leaf_apply_cmd_once(bn, se, cmd, idx, storeddata, logger);
		u_int32_t new_omt_size = toku_omt_size(bn->buffer);
		if (new_omt_size != omt_size) {
		    assert(new_omt_size+1 == omt_size);
		    //Item was deleted.
		    deleted = 1;
		}
		*made_change = 1;
	    }
	    if (deleted)
		omt_size--;
	    else
		idx++;
	}
	assert(toku_omt_size(bn->buffer) == omt_size);

	break;
    case BRT_UPDATE: {
	u_int32_t idx;
	r = toku_omt_find_zero(bn->buffer, toku_cmd_leafval_heaviside, &be,
			       &storeddatav, &idx, NULL);
	if (r==DB_NOTFOUND) {
	    r = do_update(t, bn, se, cmd, idx, NULL, logger, made_change);
	} else if (r==0) {
	    storeddata=storeddatav;
	    r = do_update(t, bn, se, cmd, idx, storeddata, logger, made_change);
	} // otherwise, a worse error, just return it
	break;
    }
    case BRT_UPDATE_BROADCAST_ALL: {
	// apply to all leafentries.
	u_int32_t idx = 0;
	u_int32_t num_leafentries_before;
	while (idx < (num_leafentries_before = toku_omt_size(bn->buffer))) {
	    r = toku_omt_fetch(bn->buffer, idx, &storeddatav, NULL);
	    assert(r==0);
	    storeddata=storeddatav;
	    r = do_update(t, bn, se, cmd, idx, storeddata, logger, made_change);
	    // TODO(leif): This early return means get_leaf_reactivity()
	    // and VERIFY_NODE() never get called.  Is this a problem?
	    assert(r==0);

	    if (num_leafentries_before == toku_omt_size(bn->buffer)) {
		// we didn't delete something, so increment the index.
		idx++;
	    }
	}
	break;
    }
    case BRT_NONE: break; // don't do anything
    }

    return;
}

// append a cmd to a nonleaf node's child buffer
// should be static, but used by test programs
void
toku_brt_append_to_child_buffer(BRTNODE node, int childnum, int type, MSN msn, XIDS xids, const DBT *key, const DBT *val) {
    assert(BP_STATE(node,childnum) == PT_AVAIL);
    int diff = key->size + val->size + KEY_VALUE_OVERHEAD + BRT_CMD_OVERHEAD + xids_get_serialize_size(xids);
    int r = toku_fifo_enq(BNC_BUFFER(node,childnum), key->data, key->size, val->data, val->size, type, msn, xids);
    assert_zero(r);
    BNC_NBYTESINBUF(node, childnum) += diff;
    node->dirty = 1;
}

static void brt_nonleaf_cmd_once_to_child (BRTNODE node, unsigned int childnum, BRT_MSG cmd)
// Previously we had passive aggressive promotion, but that causes a lot of I/O a the checkpoint.  So now we are just putting it in the buffer here.
// Also we don't worry about the node getting overfull here.  It's the caller's problem.
{
    toku_brt_append_to_child_buffer(node, childnum, cmd->type, cmd->msn, cmd->xids, cmd->u.id.key, cmd->u.id.val);
}

/* find the leftmost child that may contain the key */
unsigned int toku_brtnode_which_child (BRTNODE node , const DBT *k, BRT t) {
#define DO_PIVOT_SEARCH_LR 0
#if DO_PIVOT_SEARCH_LR
    int i;
    for (i=0; i<node->n_children-1; i++) {
	int cmp = brt_compare_pivot(t, k, d, node->childkeys[i]);
	if (cmp > 0) continue;
	if (cmp < 0) return i;
	return i;
    }
    return node->n_children-1;
#else
#endif
#define DO_PIVOT_SEARCH_RL 0
#if DO_PIVOT_SEARCH_RL
    // give preference for appending to the dictionary.	 no change for
    // random keys
    int i;
    for (i = node->n_children-2; i >= 0; i--) {
	int cmp = brt_compare_pivot(t, k, d, node->childkeys[i]);
	if (cmp > 0) return i+1;
    }
    return 0;
#endif
#define DO_PIVOT_BIN_SEARCH 1
#if DO_PIVOT_BIN_SEARCH
    // a funny case of no pivots
    if (node->n_children <= 1) return 0;

    // check the last key to optimize seq insertions
    int n = node->n_children-1;
    int cmp = brt_compare_pivot(t, k, node->childkeys[n-1]);
    if (cmp > 0) return n;

    // binary search the pivots
    int lo = 0;
    int hi = n-1; // skip the last one, we checked it above
    int mi;
    while (lo < hi) {
	mi = (lo + hi) / 2;
	cmp = brt_compare_pivot(t, k, node->childkeys[mi]);
	if (cmp > 0) {
	    lo = mi+1;
	    continue;
	} 
	if (cmp < 0) {
	    hi = mi;
	    continue;
	}
	return mi;
    }
    return lo;
#endif
}

static void brt_nonleaf_cmd_once (BRT t, BRTNODE node, BRT_MSG cmd)
// Effect: Insert a message into a nonleaf.  We may put it into a child, possibly causing the child to become reactive.
//  We don't do the splitting and merging.  That's up to the caller after doing all the puts it wants to do.
//  The re_array[i] gets set to reactivity of any modified child.
{

    /* find the right subtree */
    //TODO: accesses key, val directly
    unsigned int childnum = toku_brtnode_which_child(node, cmd->u.id.key, t);

    brt_nonleaf_cmd_once_to_child (node, childnum, cmd);
}

static void
brt_nonleaf_cmd_all (BRTNODE node, BRT_MSG cmd)
// Effect: Put the cmd into a nonleaf node.  We put it into all children, possibly causing the children to become reactive.
//  We don't do the splitting and merging.  That's up to the caller after doing all the puts it wants to do.
//  The re_array[i] gets set to the reactivity of any modified child i.	 (And there may be several such children.)
{
    int i;
    for (i = 0; i < node->n_children; i++) {
	brt_nonleaf_cmd_once_to_child(node, i, cmd);
    }
}

static BOOL
brt_msg_applies_once(BRT_MSG cmd)
{
    BOOL ret_val;
    
    //TODO: Accessing type directly
    switch (cmd->type) {
    case BRT_INSERT_NO_OVERWRITE: 
    case BRT_INSERT:
    case BRT_DELETE_ANY:
    case BRT_ABORT_ANY:
    case BRT_COMMIT_ANY:
    case BRT_UPDATE:
	ret_val = TRUE;
	break;
    case BRT_COMMIT_BROADCAST_ALL:
    case BRT_COMMIT_BROADCAST_TXN:
    case BRT_ABORT_BROADCAST_TXN:
    case BRT_OPTIMIZE:
    case BRT_OPTIMIZE_FOR_UPGRADE:
    case BRT_UPDATE_BROADCAST_ALL:
    case BRT_NONE:
	ret_val = FALSE;
	break;
    default:
	assert(FALSE);
    }
    return ret_val;
}

static BOOL
brt_msg_applies_all(BRT_MSG cmd)
{
    BOOL ret_val;
    
    //TODO: Accessing type directly
    switch (cmd->type) {
    case BRT_NONE:
    case BRT_INSERT_NO_OVERWRITE: 
    case BRT_INSERT:
    case BRT_DELETE_ANY:
    case BRT_ABORT_ANY:
    case BRT_COMMIT_ANY:
    case BRT_UPDATE:
	ret_val = FALSE;
	break;
    case BRT_COMMIT_BROADCAST_ALL:
    case BRT_COMMIT_BROADCAST_TXN:
    case BRT_ABORT_BROADCAST_TXN:
    case BRT_OPTIMIZE:
    case BRT_OPTIMIZE_FOR_UPGRADE:
    case BRT_UPDATE_BROADCAST_ALL:
	ret_val = TRUE;
	break;
    default:
	assert(FALSE);
    }
    return ret_val;
}

static BOOL
brt_msg_does_nothing(BRT_MSG cmd)
{
    return (cmd->type == BRT_NONE);
}

static void
brt_nonleaf_put_cmd (BRT t, BRTNODE node, BRT_MSG cmd)
// Effect: Put the cmd into a nonleaf node.  We may put it into a child, possibly causing the child to become reactive.
//  We don't do the splitting and merging.  That's up to the caller after doing all the puts it wants to do.
//  The re_array[i] gets set to the reactivity of any modified child i.	 (And there may be several such children.)
//
{
    MSN cmd_msn = cmd->msn;
    invariant(cmd_msn.msn > node->max_msn_applied_to_node_in_memory.msn);
    node->max_msn_applied_to_node_in_memory = cmd_msn;

    //TODO: Accessing type directly
    switch (cmd->type) {
    case BRT_INSERT_NO_OVERWRITE: 
    case BRT_INSERT:
    case BRT_DELETE_ANY:
    case BRT_ABORT_ANY:
    case BRT_COMMIT_ANY:
    case BRT_UPDATE:
	brt_nonleaf_cmd_once(t, node, cmd);
	return;
    case BRT_COMMIT_BROADCAST_ALL:
    case BRT_COMMIT_BROADCAST_TXN:
    case BRT_ABORT_BROADCAST_TXN:
    case BRT_OPTIMIZE:
    case BRT_OPTIMIZE_FOR_UPGRADE:
    case BRT_UPDATE_BROADCAST_ALL:
	brt_nonleaf_cmd_all (node, cmd);  // send message to all children
	return;
    case BRT_NONE:
	return;
    }
    abort(); // cannot happen
}

static void
merge_leaf_nodes (BRTNODE a, BRTNODE b) {
    toku_assert_entire_node_in_memory(a);
    toku_assert_entire_node_in_memory(b);
    assert(a->height == 0);
    assert(b->height == 0);
    assert(a->n_children > 0);
    assert(b->n_children > 0);

    // this BOOL states if the last basement node in a has any items or not
    // If it does, then it stays in the merge. If it does not, the last basement node
    // of a gets eliminated because we do not have a pivot to store for it (because it has no elements)
    BOOL a_has_tail = toku_omt_size(BLB_BUFFER(a, a->n_children-1));
    
    // move each basement node from b to a
    // move the pivots, adding one of what used to be max(a)
    // move the estimates
    int num_children = a->n_children + b->n_children;
    if (!a_has_tail) {
	destroy_basement_node((BASEMENTNODE)a->bp[a->n_children-1].ptr);
        toku_free(a->bp[a->n_children-1].ptr);
	num_children--;
    }

    //realloc pivots and basement nodes in a
    REALLOC_N(num_children, a->bp);
    REALLOC_N(num_children-1, a->childkeys);

    // fill in pivot for what used to be max of node 'a', if it is needed
    if (a_has_tail) {
	LEAFENTRY le = fetch_from_buf(
	    BLB_BUFFER(a, a->n_children-1), 
	    toku_omt_size(BLB_BUFFER(a, a->n_children-1))-1
	    );
	a->childkeys[a->n_children-1] = kv_pair_malloc(le_key(le), le_keylen(le), 0, 0);
	a->totalchildkeylens += le_keylen(le);
    }

    u_int32_t offset = a_has_tail ? a->n_children : a->n_children - 1;
    for (int i = 0; i < b->n_children; i++) {
	a->bp[i+offset] = b->bp[i];
        memset(&b->bp[i],0,sizeof(b->bp[0]));
	if (i < (b->n_children-1)) {
	    a->childkeys[i+offset] = b->childkeys[i];
	    b->childkeys[i] = NULL;
	}
    }
    a->totalchildkeylens += b->totalchildkeylens;
    a->n_children = num_children;

    // now that all the data has been moved from b to a, we can destroy the data in b
    // b can remain untouched, as it will be destroyed later
    b->totalchildkeylens = 0;
    b->n_children = 0;
    a->dirty = 1;
    b->dirty = 1;

}

static int
balance_leaf_nodes (BRTNODE a, BRTNODE b, struct kv_pair **splitk)
// Effect:
//  If b is bigger then move stuff from b to a until b is the smaller.
//  If a is bigger then move stuff from a to b until a is the smaller.
{
    DBT splitk_dbt;
    // first merge all the data into a
    merge_leaf_nodes(a,b);
    // now split them	 
    brtleaf_split(NULL, a, &a, &b, &splitk_dbt, FALSE);
    *splitk = splitk_dbt.data;

    return 0;
}


static void
maybe_merge_pinned_leaf_nodes (BRTNODE parent, int childnum_of_parent,
			       BRTNODE a, BRTNODE b, struct kv_pair *parent_splitk, 
			       BOOL *did_merge, BOOL *did_rebalance, struct kv_pair **splitk)
// Effect: Either merge a and b into one one node (merge them into a) and set *did_merge = TRUE.    
//	   (We do this if the resulting node is not fissible)
//	   or distribute the leafentries evenly between a and b, and set *did_rebalance = TRUE.	  
//	   (If a and be are already evenly distributed, we may do nothing.)
{
    unsigned int sizea = toku_serialize_brtnode_size(a);
    unsigned int sizeb = toku_serialize_brtnode_size(b);
    if ((sizea + sizeb)*4 > (a->nodesize*3)) {
	// the combined size is more than 3/4 of a node, so don't merge them.
	*did_merge = FALSE;
	if (sizea*4 > a->nodesize && sizeb*4 > a->nodesize) {
	    // no need to do anything if both are more than 1/4 of a node.
	    *did_rebalance = FALSE;
	    *splitk = parent_splitk;
	    return;
	}
	// one is less than 1/4 of a node, and together they are more than 3/4 of a node.
	toku_free(parent_splitk); // We don't need the parent_splitk any more.	If we need a splitk (if we don't merge) we'll malloc a new one.
	*did_rebalance = TRUE;
	int r = balance_leaf_nodes(a, b, splitk);
	assert(r==0);
    } else {
	// we are merging them.
	*did_merge = TRUE;
	*did_rebalance = FALSE;
	*splitk = 0;
	toku_free(parent_splitk); // if we are merging, the splitk gets freed.
	merge_leaf_nodes(a, b);
    }
    fixup_child_estimates(parent, childnum_of_parent,	a, TRUE);
    fixup_child_estimates(parent, childnum_of_parent+1, b, TRUE);
}

static void
maybe_merge_pinned_nonleaf_nodes (BRTNODE parent, int childnum_of_parent, struct kv_pair *parent_splitk,
				  BRTNODE a, BRTNODE b,
				  BOOL *did_merge, BOOL *did_rebalance, struct kv_pair **splitk)
{
    toku_assert_entire_node_in_memory(a);
    toku_assert_entire_node_in_memory(b);
    assert(parent_splitk);
    int old_n_children = a->n_children;
    int new_n_children = old_n_children + b->n_children;
    XREALLOC_N(new_n_children, a->bp);
    memcpy(a->bp + old_n_children,
	   b->bp,
	   b->n_children*sizeof(b->bp[0]));
    memset(b->bp,0,b->n_children*sizeof(b->bp[0]));
    
    XREALLOC_N(new_n_children-1, a->childkeys);
    a->childkeys[old_n_children-1] = parent_splitk;
    memcpy(a->childkeys + old_n_children,
	   b->childkeys,
	   (b->n_children-1)*sizeof(b->childkeys[0]));
    a->totalchildkeylens += b->totalchildkeylens + toku_brt_pivot_key_len(parent_splitk);
    a->n_children = new_n_children;

    b->totalchildkeylens = 0;
    b->n_children = 0;

    a->dirty = 1;
    b->dirty = 1;

    fixup_child_estimates(parent, childnum_of_parent, a, TRUE);
    *did_merge = TRUE;
    *did_rebalance = FALSE;
    *splitk    = NULL;

}

static void
maybe_merge_pinned_nodes (BRTNODE parent, int childnum_of_parent, struct kv_pair *parent_splitk,
			  BRTNODE a, BRTNODE b, 
			  BOOL *did_merge, BOOL *did_rebalance, struct kv_pair **splitk)
// Effect: either merge a and b into one node (merge them into a) and set *did_merge = TRUE.  
//	   (We do this if the resulting node is not fissible)
//	   or distribute a and b evenly and set *did_merge = FALSE and *did_rebalance = TRUE  
//	   (If a and be are already evenly distributed, we may do nothing.)
//  If we distribute:
//    For leaf nodes, we distribute the leafentries evenly.
//    For nonleaf nodes, we distribute the children evenly.  That may leave one or both of the nodes overfull, but that's OK.
//  If we distribute, we set *splitk to a malloced pivot key.
// Parameters:
//  t			The BRT.
//  parent		The parent of the two nodes to be split.
//  childnum_of_parent	Which child of the parent is a?	 (b is the next child.)
//  parent_splitk	The pivot key between a and b.	 This is either free()'d or returned in *splitk.
//  a			The first node to merge.
//  b			The second node to merge.
//  logger		The logger.
//  did_merge		(OUT):	Did the two nodes actually get merged?
//  splitk		(OUT):	If the two nodes did not get merged, the new pivot key between the two nodes.
{
    MSN msn_max;
    assert(a->height == b->height);
    toku_assert_entire_node_in_memory(parent);
    toku_assert_entire_node_in_memory(a);
    toku_assert_entire_node_in_memory(b);
    parent->dirty = 1; // just to make sure 
    {
	MSN msna = a->max_msn_applied_to_node_in_memory;
	MSN msnb = b->max_msn_applied_to_node_in_memory;
	msn_max = (msna.msn > msnb.msn) ? msna : msnb;
	if (a->height > 0) {
	    invariant(msn_max.msn <= parent->max_msn_applied_to_node_in_memory.msn);  // parent msn must be >= children's msn
	}
    }
    if (a->height == 0) {
	maybe_merge_pinned_leaf_nodes(parent, childnum_of_parent, a, b, parent_splitk, did_merge, did_rebalance, splitk);
    } else {
	maybe_merge_pinned_nonleaf_nodes(parent, childnum_of_parent, parent_splitk, a, b, did_merge, did_rebalance, splitk);
    }
    if (*did_merge || *did_rebalance) {	 
	// accurate for leaf nodes because all msgs above have been applied,
	// accurate for non-leaf nodes because buffer immediately above each node has been flushed
	a->max_msn_applied_to_node_in_memory = msn_max;
	b->max_msn_applied_to_node_in_memory = msn_max;
    }
}

static void
brt_merge_child (BRT t, BRTNODE node, int childnum_to_merge, BOOL *did_react,
		 ANCESTORS ancestors, struct pivot_bounds const * const bounds)
{
    if (node->n_children < 2) return; // if no siblings, we are merged as best we can.
    toku_assert_entire_node_in_memory(node);

    int childnuma,childnumb;
    if (childnum_to_merge > 0) {
	childnuma = childnum_to_merge-1;
	childnumb = childnum_to_merge;
    } else {
	childnuma = childnum_to_merge;
	childnumb = childnum_to_merge+1;
    }
    assert(0 <= childnuma);
    assert(childnuma+1 == childnumb);
    assert(childnumb < node->n_children);

    assert(node->height>0);

    const struct pivot_bounds next_bounds_a = next_pivot_keys(node, childnuma, bounds);
    const struct pivot_bounds next_bounds_b = next_pivot_keys(node, childnumb, bounds);

    if (toku_fifo_n_entries(BNC_BUFFER(node,childnuma))>0) {
	enum reactivity ignore;
	flush_this_child(t, node, childnuma, &ignore, FALSE, FALSE, ancestors, &next_bounds_a);
    }
    if (toku_fifo_n_entries(BNC_BUFFER(node,childnumb))>0) {
	enum reactivity ignore;
	flush_this_child(t, node, childnumb, &ignore, FALSE, FALSE, ancestors, &next_bounds_b);
    }

    // We suspect that at least one of the children is fusible, but they might not be.

    BRTNODE childa, childb;
    {
	void *childnode_v;
	u_int32_t childfullhash = compute_child_fullhash(t->cf, node, childnuma);
        struct brtnode_fetch_extra bfe;
        fill_bfe_for_full_read(&bfe, t->h);
	int r = toku_cachetable_get_and_pin(
            t->cf, 
            BP_BLOCKNUM(node, childnuma), 
            childfullhash, 
            &childnode_v, 
            NULL,
            toku_brtnode_flush_callback, 
            toku_brtnode_fetch_callback, 
            toku_brtnode_pe_callback, 
            toku_brtnode_pf_req_callback,
            toku_brtnode_pf_callback,
            &bfe, 
            t->h
            );
	assert(r==0);
	childa = childnode_v;
    }
    {
	void *childnode_v;
	u_int32_t childfullhash = compute_child_fullhash(t->cf, node, childnumb);
        struct brtnode_fetch_extra bfe;
        fill_bfe_for_full_read(&bfe, t->h);
	int r = toku_cachetable_get_and_pin(
            t->cf, 
            BP_BLOCKNUM(node, childnumb), 
            childfullhash, &childnode_v, 
            NULL,
            toku_brtnode_flush_callback, 
            toku_brtnode_fetch_callback, 
            toku_brtnode_pe_callback, 
            toku_brtnode_pf_req_callback,
            toku_brtnode_pf_callback,
            &bfe, 
            t->h
            );
	assert(r==0);
	childb = childnode_v;
    }

    // now we have both children pinned in main memory.

    BOOL did_merge, did_rebalance;
    {
	struct kv_pair *splitk_kvpair = 0;
	struct kv_pair *old_split_key = node->childkeys[childnuma];
	unsigned int deleted_size = toku_brt_pivot_key_len(old_split_key);
	maybe_merge_pinned_nodes(node, childnuma, node->childkeys[childnuma], childa, childb, &did_merge, &did_rebalance, &splitk_kvpair);
	if (childa->height>0) { int i; for (i=0; i+1<childa->n_children; i++) assert(childa->childkeys[i]); }
	//toku_verify_estimates(t,childa);
	// the tree did react if a merge (did_merge) or rebalance (new spkit key) occurred
	*did_react = (BOOL)(did_merge || did_rebalance);
	if (did_merge) assert(!splitk_kvpair); else assert(splitk_kvpair);

	node->totalchildkeylens -= deleted_size; // The key was free()'d inside the maybe_merge_pinned_nodes.

	if (did_merge) {
	    toku_fifo_free(&BNC_BUFFER(node, childnumb));
            toku_free(node->bp[childnumb].ptr);
	    node->n_children--;
	    memmove(&node->bp[childnumb],
		    &node->bp[childnumb+1],
		    (node->n_children-childnumb)*sizeof(node->bp[0]));
	    REALLOC_N(node->n_children, node->bp);
	    memmove(&node->childkeys[childnuma],
		    &node->childkeys[childnuma+1],
		    (node->n_children-childnumb)*sizeof(node->childkeys[0]));
	    REALLOC_N(node->n_children-1, node->childkeys);
	    fixup_child_estimates(node, childnuma, childa, TRUE);
	    assert(BP_BLOCKNUM(node, childnuma).b == childa->thisnodename.b);
	    childa->dirty = 1; // just to make sure
	    childb->dirty = 1; // just to make sure
	} else {
	    assert(splitk_kvpair);
	    // If we didn't merge the nodes, then we need the correct pivot.
	    node->childkeys[childnuma] = splitk_kvpair;
	    node->totalchildkeylens += toku_brt_pivot_key_len(node->childkeys[childnuma]);
	    node->dirty = 1;
	}
    }
    assert(node->dirty);
    // Unpin both, and return the first nonzero error code that is found
    toku_unpin_brtnode(t, childa);
    if (did_merge) {
	BLOCKNUM bn = childb->thisnodename;
	int rrb = toku_cachetable_unpin_and_remove(t->cf, bn);
	assert(rrb==0);
	toku_free_blocknum(t->h->blocktable, &bn, t->h);
    } else {
	toku_unpin_brtnode(t, childb);
    }
}

static void
brt_handle_maybe_reactive_child(BRT t, BRTNODE node, int childnum, enum reactivity re, BOOL *did_react,
				ANCESTORS ancestors, struct pivot_bounds const * const bounds) {
    switch (re) {
    case RE_STABLE:
	*did_react = FALSE;
	return;
    case RE_FISSIBLE:
	brt_split_child(t, node, childnum, did_react);
	return;
    case RE_FUSIBLE:
	brt_merge_child(t, node, childnum, did_react, ancestors, bounds);
	return;
    }
    abort(); // cannot happen
}

static void
brt_handle_maybe_reactive_root (BRT brt, CACHEKEY *rootp, BRTNODE *nodep) {
    BRTNODE node = *nodep;
    toku_assert_entire_node_in_memory(node);
    enum reactivity re = get_node_reactivity(node);
    switch (re) {
    case RE_STABLE:
	return;
    case RE_FISSIBLE:
	// The root node should split, so make a new root.
	{
	    BRTNODE nodea,nodeb;
	    DBT splitk;
	    assert(brt->h->nodesize>=node->nodesize); /* otherwise we might be in trouble because the nodesize shrank. */
	    if (node->height==0) {
		brtleaf_split(brt, node, &nodea, &nodeb, &splitk, TRUE);
	    } else {
		brt_nonleaf_split(brt, node, &nodea, &nodeb, &splitk);
	    }
	    brt_init_new_root(brt, nodea, nodeb, splitk, rootp, nodep);
	    return;
	}
    case RE_FUSIBLE:
	return; // Cannot merge anything at the root, so return happy.
    }
    abort(); // cannot happen
}

static void find_heaviest_child (BRTNODE node, int *childnum) {
    int max_child = 0;
    int max_weight = BNC_NBYTESINBUF(node, 0);
    int i;

    if (0) printf("%s:%d weights: %d", __FILE__, __LINE__, max_weight);
    assert(node->n_children>0);
    for (i=1; i<node->n_children; i++) {
	int this_weight = BNC_NBYTESINBUF(node,i);
	if (0) printf(" %d", this_weight);
	if (max_weight < this_weight) {
	    max_child = i;
	    max_weight = this_weight;
	}
    }
    *childnum = max_child;
    if (0) printf("\n");
}

static void
flush_some_child (BRT t, BRTNODE node, BOOL is_first_flush, BOOL flush_recursively,
		  ANCESTORS ancestors, struct pivot_bounds const * const bounds)
// Effect: Pick a child (the heaviest child) and flush it.
//  If flush_recursively is true, then we must flush the grandchild after the flush (if the grandchild is overfull).
//  Furthermore, if is_first_flush is true, then we can flush the grandchild several times (but only one of those grandchildren will receive the is_first_flush==TRUE)
//  After the flush, this function we may split or merge the node.
//   IS_FIRST_FLUSH=TRUE, FLUSH_RECURSIVELY=TRUE    then flush several grandchildren, but only one grandchild gets IS_FIRST_FLUSH=TRUE
//   IS_FIRST_FLUSH=TRUE, FLUSH_RECURSIVELY=TRUE    then flush one grandchild (and up to one of it's children, and one of its children and so forth).
//   FLUSH_RECURSIVELY=FALSE			    don't flush any grandchildren
{
    assert(node->height>0);
    toku_assert_entire_node_in_memory(node);
    int childnum;
    find_heaviest_child(node, &childnum);
    assert(toku_fifo_n_entries(BNC_BUFFER(node, childnum))>0);
    enum reactivity child_re = RE_STABLE;
    flush_this_child (t, node, childnum, &child_re, is_first_flush, flush_recursively,
		      ancestors, bounds);
    BOOL did_react;
    brt_handle_maybe_reactive_child(t, node, childnum, child_re, &did_react,
				    ancestors, bounds); 
}

static void assert_leaf_up_to_date(BRTNODE node) {
    assert(node->height == 0);
    toku_assert_entire_node_in_memory(node);
    for (int i=0; i < node->n_children; i++) {
	assert(BLB_SOFTCOPYISUPTODATE(node, i));
    }
}

static void
flush_this_child (BRT t, BRTNODE node, int childnum, enum reactivity *child_re, BOOL is_first_flush, BOOL flush_recursively,
		  ANCESTORS ancestors, struct pivot_bounds const * const bounds)
// Effect: Push everything in the CHILDNUMth buffer of node down into the child.
//  The child may split or merge as a result of the activity.
//  The IS_FIRST_FLUSH variable is a way to prevent the flushing from walking the entire tree.	If IS_FIRST_FLUSH==TRUE then we are allowed to flush more than one child, otherwise
//   we are allowed to flush only one child.
// For this version, flush_this_child cannot release the lock during I/O, but it does need the ancestor information so that it can apply messages when a page comes in.
{
    toku_assert_entire_node_in_memory(node);
    struct ancestors next_ancestors = {node, childnum, ancestors};
    const struct pivot_bounds next_bounds = next_pivot_keys(node, childnum, bounds);
    assert(node->height>0);
    BLOCKNUM targetchild = BP_BLOCKNUM(node, childnum);
    toku_verify_blocknum_allocated(t->h->blocktable, targetchild);
    u_int32_t childfullhash = compute_child_fullhash(t->cf, node, childnum);
    BRTNODE child;
    struct brtnode_fetch_extra bfe;
    fill_bfe_for_full_read(&bfe, t->h);
    toku_pin_brtnode_holding_lock(t, targetchild, childfullhash, &next_ancestors, &next_bounds, &bfe, &child); // get that child node in, and apply the ancestor messages if it's a leaf.

    toku_assert_entire_node_in_memory(node);
    assert(child->thisnodename.b!=0);
    VERIFY_NODE(t, child);

    FIFO fifo = BNC_BUFFER(node,childnum);
    if (child->height==0) {
	// The child is a leaf node. 
	assert_leaf_up_to_date(child); //  The child has all the messages applied to it.
	// We've arranged that the path from the root to this child is empty, except for the childnum fifo in node.
	// We must empty the fifo, and arrange for the child to be written to disk, and then mark it as clean and up-to-date.
	bytevec key, val;
	ITEMLEN keylen, vallen;
	u_int32_t type;
	MSN msn;
	XIDS xids;
	while(0==toku_fifo_peek(fifo, &key, &keylen, &val, &vallen, &type, &msn, &xids)) {
	    int n_bytes_removed = (keylen + vallen + KEY_VALUE_OVERHEAD + BRT_CMD_OVERHEAD + xids_get_serialize_size(xids));

	    int r = toku_fifo_deq(fifo);
	    assert(r==0);

	    BNC_NBYTESINBUF(node, childnum) -= n_bytes_removed;
	}

	node->dirty=TRUE;
	child->dirty=TRUE;
	fixup_child_estimates(node, childnum, child, TRUE);
	*child_re = get_node_reactivity(child);
	toku_unpin_brtnode(t, child);
    } else {
	bytevec key,val;
	ITEMLEN keylen, vallen;
	//printf("%s:%d Try random_pick, weight=%d \n", __FILE__, __LINE__, BNC_NBYTESINBUF(node, childnum));
	assert(toku_fifo_n_entries(fifo)>0);
	u_int32_t type;
	MSN msn;
	XIDS xids;
	while(0==toku_fifo_peek(fifo, &key, &keylen, &val, &vallen, &type, &msn, &xids)) {
	    DBT hk,hv;

	    //TODO: Factor out (into a function) conversion of fifo_entry to message
	    BRT_MSG_S brtcmd = { (enum brt_msg_type)type, msn, xids, .u.id= {toku_fill_dbt(&hk, key, keylen),
										  toku_fill_dbt(&hv, val, vallen)} };

	    int n_bytes_removed = (hk.size + hv.size + KEY_VALUE_OVERHEAD + BRT_CMD_OVERHEAD + xids_get_serialize_size(xids));

	    //printf("%s:%d random_picked\n", __FILE__, __LINE__);
	    brtnode_put_cmd (t, child, &brtcmd);

	    //printf("%s:%d %d=push_a_brt_cmd_down=();	child_did_split=%d (weight=%d)\n", __FILE__, __LINE__, r, child_did_split, BNC_NBYTESINBUF(node, childnum));

	    {
		int r = toku_fifo_deq(fifo);
		//printf("%s:%d deleted status=%d\n", __FILE__, __LINE__, r);
		assert(r==0);
	    }

	    BNC_NBYTESINBUF(node, childnum) -= n_bytes_removed;
	    node->dirty = 1;

	}
	if (0) printf("%s:%d done random picking\n", __FILE__, __LINE__);

	// Having pushed all that stuff to a child, do we need to flush the child?  We may have to flush it many times if there were lots of messages that just got pushed down.
	// If we were to only flush one child, we could possibly end up with a very big node after a while.
	// This repeated flushing can cause some inserts to take a long time (possibly walking all over the tree).
	// When we get the background flushing working, it may be OK if that happens, but for now, we just flush a little.
	if (flush_recursively) {
	    int n_flushed = 0;
	    while (nonleaf_node_is_gorged(child) && (is_first_flush || n_flushed==0)) {
		// don't do more than one child unless this is the first flush.
		flush_some_child(t, child, is_first_flush && n_flushed==0, flush_recursively,
				 &next_ancestors, &next_bounds);
		n_flushed++;
	    }
	}
	fixup_child_estimates(node, childnum, child, TRUE);
	// Now it's possible that the child needs to be merged or split.
	*child_re = get_node_reactivity(child);
	toku_unpin_brtnode(t, child);
    }
}

static void
brtnode_put_cmd (BRT t, BRTNODE node, BRT_MSG cmd)
// Effect: Push CMD into the subtree rooted at NODE.
//   If NODE is a leaf, then
//	put CMD into leaf, applying it to the leafentries
//   If NODE is a nonleaf, then push the cmd into the FIFO(s) of the relevent child(ren).
//   The node may become overfull.  That's not our problem.
{
    toku_assert_entire_node_in_memory(node);
    if (node->height==0) {
	// we need to make sure that after doing all the put_cmd operations 
	// that the tree above is completely flushed out, 
	// otherwise may have an inconsistency (part of the data is there, and part isn't)

	assert_leaf_up_to_date(node);

	// Do nothing
    } else {
	brt_nonleaf_put_cmd(t, node, cmd);
    }
}

static const struct pivot_bounds infinite_bounds = {.lower_bound_exclusive=NULL,
						    .upper_bound_inclusive=NULL};

static void
brtnode_nonleaf_put_cmd_at_root (BRT t, BRTNODE node, BRT_MSG cmd) 
// Effect: Push CMD into the subtree rooted at nonleaf NODE, and indicate whether as a result NODE should split or should merge.
//   Push the cmd in the relevant child's (or children's) FIFOs. 
//   The node may get too full or something.  It's the caller job to fix that up.
// Requires: node is not a leaf.
{
    assert(node->height>0);
    toku_assert_entire_node_in_memory(node);
    brt_nonleaf_put_cmd(t, node, cmd);
}

// Effect: applies the cmd to the leaf if the appropriate basement node is in memory.
//           If the appropriate basement node is not in memory, then nothing gets applied
//           If the appropriate basement node must be in memory, it is the caller's responsibility to ensure
//             that it is
void toku_apply_cmd_to_leaf(BRT t, BRTNODE node, BRT_MSG cmd, int *made_change) {
    VERIFY_NODE(t, node);
    // ignore messages that have already been applied to this leaf
    if (cmd->msn.msn <= node->max_msn_applied_to_node_in_memory.msn) {
	// TODO3514  add accountability counter here
	return;
    }
    else {
	node->max_msn_applied_to_node_in_memory = cmd->msn;
    }
    
    if (brt_msg_applies_once(cmd)) {
	unsigned int childnum = toku_brtnode_which_child(node, cmd->u.id.key, t);
        if (BP_STATE(node,childnum) == PT_AVAIL) {
            brt_leaf_put_cmd(
            t, 
            (BASEMENTNODE)node->bp[childnum].ptr, 
            &BP_SUBTREE_EST(node, childnum),
            cmd, 
            made_change
            );
        }
    }
    else if (brt_msg_applies_all(cmd)) {
	int bn_made_change = 0;
	for (int childnum=0; childnum<node->n_children; childnum++) {
            if (BP_STATE(node,childnum) == PT_AVAIL) {
                brt_leaf_put_cmd(
                    t, 
                    (BASEMENTNODE)node->bp[childnum].ptr, 
                    &BP_SUBTREE_EST(node,childnum),
                    cmd, 
                    &bn_made_change
                    );
                if (bn_made_change) *made_change = 1;
            }
	}
    }
    else if (!brt_msg_does_nothing(cmd)) {
	assert(FALSE);
    }
    VERIFY_NODE(t, node);
}


static void push_something_at_root (BRT brt, BRTNODE *nodep, BRT_MSG cmd)
// Effect:  Put CMD into brt by descending into the tree as deeply as we can
//   without performing I/O (but we must fetch the root),
//   bypassing only empty FIFOs
//   If the cmd is a broadcast message, we copy the message as needed as we descend the tree so that each relevant subtree receives the message.
//   At the end of the descent, we are either at a leaf, or we hit a nonempty FIFO.
//     If it's a leaf, and the leaf is gorged or hungry, then we split the leaf or merge it with the neighbor.
//	Note: for split operations, no disk I/O is required.  For merges, I/O may be required, so for a broadcast delete, quite a bit
//	 of I/O could be induced in the worst case.
//     If it's a nonleaf, and the node is gorged or hungry, then we flush everything in the heaviest fifo to the child.
//	 During flushing, we allow the child to become gorged.
//	   (And for broadcast messages, we simply place the messages into all the relevant fifos of the child, rather than trying to descend.)
//	 After flushing to a child, if the child is gorged (underful), then
//	     if the child is leaf, we split (merge) it
//	     if the child is a nonleaf, we flush the heaviest child recursively.
//	 Note: After flushing, a node could still be gorged (or possibly hungry.)  We let it remain so.
//	 Note: During the initial descent, we may gorged many nonleaf nodes.  We wish to flush only one nonleaf node at each level.
{
    BRTNODE node = *nodep;
    toku_assert_entire_node_in_memory(node);
    if (node->height==0) {
	// Must special case height 0, since brtnode_put_cmd() doesn't modify leaves.
	// Part of the problem is: if the node is in memory, then it was updated as part of the in-memory operation.
	// If the root node is not in memory, then we must apply it.
	int made_dirty = 0;
	// not up to date, which means the get_and_pin actually fetched it into memory.
	toku_apply_cmd_to_leaf(brt, node, cmd, &made_dirty);
	if (made_dirty) node->dirty = 1;
   } else {
	brtnode_nonleaf_put_cmd_at_root(brt, node, cmd);
	//if (should_split) printf("%s:%d Pushed something simple, should_split=1\n", __FILE__, __LINE__); 
    }
    //printf("%s:%d should_split=%d node_size=%" PRIu64 "\n", __FILE__, __LINE__, should_split, brtnode_memory_size(node));

}

static void compute_and_fill_remembered_hash (BRT brt) {
    struct remembered_hash *rh = &brt->h->root_hash;
    assert(brt->cf); // if cf is null, we'll be hosed.
    rh->valid = TRUE;
    rh->fnum=toku_cachefile_filenum(brt->cf);
    rh->root=brt->h->root;
    rh->fullhash = toku_cachetable_hash(brt->cf, rh->root);
}

static u_int32_t get_roothash (BRT brt) {
    struct remembered_hash *rh = &brt->h->root_hash;
    BLOCKNUM root = brt->h->root;
    // compare cf first, since cf is NULL for invalid entries.
    assert(rh);
    //printf("v=%d\n", rh->valid);
    if (rh->valid) {
	//printf("f=%d\n", rh->fnum.fileid); 
	//printf("cf=%d\n", toku_cachefile_filenum(brt->cf).fileid);
	if (rh->fnum.fileid == toku_cachefile_filenum(brt->cf).fileid)
	    if (rh->root.b == root.b)
		return rh->fullhash;
    }
    compute_and_fill_remembered_hash(brt);
    return rh->fullhash;
}

static void apply_cmd_to_in_memory_non_root_leaves (
    BRT t, 
    CACHEKEY nodenum, 
    u_int32_t fullhash, 
    BRT_MSG cmd, 
    BOOL is_root, 
    BRTNODE parent, 
    int parents_childnum
    ) 
{
    void *node_v;
    int r = toku_cachetable_get_and_pin_if_in_memory(t->cf, nodenum, fullhash, &node_v);
    if (r) { goto exit; }

    BRTNODE node = node_v;
    // internal node
    if (node->height>0) {
	if (brt_msg_applies_once(cmd)) {
	    unsigned int childnum = toku_brtnode_which_child(node, cmd->u.id.key, t);
	    u_int32_t child_fullhash = compute_child_fullhash(t->cf, node, childnum);
	    apply_cmd_to_in_memory_non_root_leaves(t, BP_BLOCKNUM(node, childnum), child_fullhash, cmd, FALSE, node, childnum);
	}
	else if (brt_msg_applies_all(cmd)) {
	    for (int childnum=0; childnum<node->n_children; childnum++) {
		assert(BP_HAVE_FULLHASH(node, childnum));
		apply_cmd_to_in_memory_non_root_leaves(t, BP_BLOCKNUM(node, childnum), BP_FULLHASH(node, childnum), cmd, FALSE, node, childnum);
	    }
	}
	else if (brt_msg_does_nothing(cmd)) {
	}
	else {
	    assert(FALSE);
	}
    }
    // leaf node
    else {
	// only apply message if this is NOT a root node, because push_something_at_root
	// has already applied it
	if (!is_root) {
	    int made_change;
	    toku_apply_cmd_to_leaf(t, node, cmd, &made_change);
	}
    }
    
    if (parent) {
	fixup_child_estimates(parent, parents_childnum, node, FALSE);
    }
    
    toku_unpin_brtnode(t, node);
exit:
    return;
}

CACHEKEY* toku_calculate_root_offset_pointer (BRT brt, u_int32_t *roothash) {
    *roothash = get_roothash(brt);
    return &brt->h->root;
}


int 
toku_brt_root_put_cmd (BRT brt, BRT_MSG_S * cmd)
// Effect:
//  - assign msn to cmd	 
//  - push the cmd into the brt
//  - cmd will set new msn in tree
{
    BRTNODE node;
    CACHEKEY *rootp;
    //assert(0==toku_cachetable_assert_all_unpinned(brt->cachetable));
    assert(brt->h);

    brt->h->root_put_counter = global_root_put_counter++;
    u_int32_t fullhash;
    rootp = toku_calculate_root_offset_pointer(brt, &fullhash);

    // get the root node
    struct brtnode_fetch_extra bfe;
    fill_bfe_for_full_read(&bfe, brt->h);
    toku_pin_brtnode_holding_lock(brt, *rootp, fullhash, NULL, &infinite_bounds, &bfe, &node);
    toku_assert_entire_node_in_memory(node);
    cmd->msn.msn = node->max_msn_applied_to_node_in_memory.msn + 1;
    // Note, the lower level function that filters messages based on msn, 
    // (brt_leaf_put_cmd() or brt_nonleaf_put_cmd()) will capture the msn and
    // store it in the relevant node, including the root node.	This is how the 
    // new msn is set in the root.

    VERIFY_NODE(brt, node);
    assert(node->fullhash==fullhash);
    brt_verify_flags(brt, node);

    push_something_at_root(brt, &node, cmd);
    // verify that msn of latest message was captured in root node (push_something_at_root() did not release ydb lock)
    invariant(cmd->msn.msn == node->max_msn_applied_to_node_in_memory.msn);

    apply_cmd_to_in_memory_non_root_leaves(brt, *rootp, fullhash, cmd, TRUE, NULL, -1);
    if (node->height > 0 && nonleaf_node_is_gorged(node)) {
	// No need for a loop here.  We only inserted one message, so flushing a single child suffices.
	flush_some_child(brt, node, TRUE, TRUE,
		(ANCESTORS)NULL, &infinite_bounds);
    }
    brt_handle_maybe_reactive_root(brt, rootp, &node);

    toku_unpin_brtnode(brt, node);  // unpin root
    return 0;
}

// Effect: Insert the key-val pair into brt.
int toku_brt_insert (BRT brt, DBT *key, DBT *val, TOKUTXN txn) {
    return toku_brt_maybe_insert(brt, key, val, txn, FALSE, ZERO_LSN, TRUE, BRT_INSERT);
}

int
toku_brt_load_recovery(TOKUTXN txn, char const * old_iname, char const * new_iname, int do_fsync, int do_log, LSN *load_lsn) {
    int r = 0;
    assert(txn);
    toku_txn_force_fsync_on_commit(txn);  //If the txn commits, the commit MUST be in the log
					  //before the (old) file is actually unlinked
    TOKULOGGER logger = toku_txn_logger(txn);

    BYTESTRING old_iname_bs = {.len=strlen(old_iname), .data=(char*)old_iname};
    BYTESTRING new_iname_bs = {.len=strlen(new_iname), .data=(char*)new_iname};
    r = toku_logger_save_rollback_load(txn, &old_iname_bs, &new_iname_bs);
    if (r==0 && do_log && logger) {
	TXNID xid = toku_txn_get_txnid(txn);
	r = toku_log_load(logger, load_lsn, do_fsync, xid, old_iname_bs, new_iname_bs);
    }
    return r;
}

// 2954
// this function handles the tasks needed to be recoverable
//  - write to rollback log
//  - write to recovery log
int
toku_brt_hot_index_recovery(TOKUTXN txn, FILENUMS filenums, int do_fsync, int do_log, LSN *hot_index_lsn)
{
    int r = 0;
    assert(txn);
    TOKULOGGER logger = toku_txn_logger(txn);

    // write to the rollback log
    r = toku_logger_save_rollback_hot_index(txn, &filenums);
    if ( r==0 && do_log && logger) {
	TXNID xid = toku_txn_get_txnid(txn);
	// write to the recovery log
	r = toku_log_hot_index(logger, hot_index_lsn, do_fsync, xid, filenums);
    }
    return r;
}

static int brt_optimize (BRT brt, BOOL upgrade);

// Effect: Optimize the brt.
int
toku_brt_optimize (BRT brt) {
    int r = brt_optimize(brt, FALSE);
    return r;
}

int
toku_brt_optimize_for_upgrade (BRT brt) {
    int r = brt_optimize(brt, TRUE);
    return r;
}

static int
brt_optimize (BRT brt, BOOL upgrade) {
    int r = 0;

    TXNID oldest = TXNID_NONE_LIVING;
    if (!upgrade) {
	TOKULOGGER logger = toku_cachefile_logger(brt->cf);
	oldest = toku_logger_get_oldest_living_xid(logger, NULL);
    }

    XIDS root_xids = xids_get_root_xids();
    XIDS message_xids;
    if (oldest == TXNID_NONE_LIVING) {
	message_xids = root_xids;
    }
    else {
	r = xids_create_child(root_xids, &message_xids, oldest);
	invariant(r==0);
    }

    DBT key;
    DBT val;
    toku_init_dbt(&key);
    toku_init_dbt(&val);
    if (upgrade) {
	// maybe there's a better place than the val dbt to put the version, but it seems harmless and is convenient
	toku_fill_dbt(&val, &this_version, sizeof(this_version));  
	BRT_MSG_S brtcmd = { BRT_OPTIMIZE_FOR_UPGRADE, ZERO_MSN, message_xids, .u.id={&key,&val}};
	r = toku_brt_root_put_cmd(brt, &brtcmd);
    }
    else {
	BRT_MSG_S brtcmd = { BRT_OPTIMIZE, ZERO_MSN, message_xids, .u.id={&key,&val}};
	r = toku_brt_root_put_cmd(brt, &brtcmd);
    }
    xids_destroy(&message_xids);
    return r;
}


int
toku_brt_load(BRT brt, TOKUTXN txn, char const * new_iname, int do_fsync, LSN *load_lsn) {
    int r = 0;
    char const * old_iname = toku_cachefile_fname_in_env(brt->cf);
    int do_log = 1;
    r = toku_brt_load_recovery(txn, old_iname, new_iname, do_fsync, do_log, load_lsn);
    return r;
}

// 2954
// brt actions for logging hot index filenums
int 
toku_brt_hot_index(BRT brt __attribute__ ((unused)), TOKUTXN txn, FILENUMS filenums, int do_fsync, LSN *lsn) {
    int r = 0;
    int do_log = 1;
    r = toku_brt_hot_index_recovery(txn, filenums, do_fsync, do_log, lsn);
    return r;
}

int 
toku_brt_log_put (TOKUTXN txn, BRT brt, const DBT *key, const DBT *val) {
    int r = 0;
    TOKULOGGER logger = toku_txn_logger(txn);
    if (logger && brt->h->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
	BYTESTRING keybs = {.len=key->size, .data=key->data};
	BYTESTRING valbs = {.len=val->size, .data=val->data};
	TXNID xid = toku_txn_get_txnid(txn);
	// if (type == BRT_INSERT)
	    r = toku_log_enq_insert(logger, (LSN*)0, 0, toku_cachefile_filenum(brt->cf), xid, keybs, valbs);
	// else
	    // r = toku_log_enq_insert_no_overwrite(logger, (LSN*)0, 0, toku_cachefile_filenum(brt->cf), xid, keybs, valbs);
    }
    return r;
}

int
toku_brt_log_put_multiple (TOKUTXN txn, BRT src_brt, BRT *brts, int num_brts, const DBT *key, const DBT *val) {
    int r = 0;
    assert(txn);
    assert(num_brts > 0);
    TOKULOGGER logger = toku_txn_logger(txn);
    if (logger) {
	FILENUM	 fnums[num_brts];
	int i;
	int num_unsuppressed_brts = 0;
	for (i = 0; i < num_brts; i++) {
	    if (brts[i]->h->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
		//Logging not suppressed for this brt.
		fnums[num_unsuppressed_brts++] = toku_cachefile_filenum(brts[i]->cf);
	    }
	}
	if (num_unsuppressed_brts) {
	    FILENUMS filenums = {.num = num_unsuppressed_brts, .filenums = fnums};
	    BYTESTRING keybs = {.len=key->size, .data=key->data};
	    BYTESTRING valbs = {.len=val->size, .data=val->data};
	    TXNID xid = toku_txn_get_txnid(txn);
	    FILENUM src_filenum = src_brt ? toku_cachefile_filenum(src_brt->cf) : FILENUM_NONE;
	    r = toku_log_enq_insert_multiple(logger, (LSN*)0, 0, src_filenum, filenums, xid, keybs, valbs);
	}
    }
    return r;
}

int 
toku_brt_maybe_insert (BRT brt, DBT *key, DBT *val, TOKUTXN txn, BOOL oplsn_valid, LSN oplsn, BOOL do_logging, enum brt_msg_type type) {
    assert(type==BRT_INSERT || type==BRT_INSERT_NO_OVERWRITE);
    int r = 0;
    XIDS message_xids = xids_get_root_xids(); //By default use committed messages
    TXNID xid = toku_txn_get_txnid(txn);
    if (txn) {
	if (brt->h->txnid_that_created_or_locked_when_empty != xid) {
	    BYTESTRING keybs  = {key->size, key->data};
	    r = toku_logger_save_rollback_cmdinsert(txn, toku_cachefile_filenum(brt->cf), &keybs);
	    if (r!=0) return r;
	    r = toku_txn_note_brt(txn, brt);
	    if (r!=0) return r;
	    //We have transactions, and this is not 2440.  We must send the full root-to-leaf-path
	    message_xids = toku_txn_get_xids(txn);
	}
	else if (txn->ancestor_txnid64 != brt->h->root_xid_that_created) {
	    //We have transactions, and this is 2440, however the txn doing 2440 did not create the dictionary.	 We must send the full root-to-leaf-path
	    message_xids = toku_txn_get_xids(txn);
	}
    }
    TOKULOGGER logger = toku_txn_logger(txn);
    if (do_logging && logger &&
	brt->h->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
	BYTESTRING keybs = {.len=key->size, .data=key->data};
	BYTESTRING valbs = {.len=val->size, .data=val->data};
	if (type == BRT_INSERT) {
	    r = toku_log_enq_insert(logger, (LSN*)0, 0, toku_cachefile_filenum(brt->cf), xid, keybs, valbs);
	}
	else {
	    r = toku_log_enq_insert_no_overwrite(logger, (LSN*)0, 0, toku_cachefile_filenum(brt->cf), xid, keybs, valbs);
	}
	if (r!=0) return r;
    }

    LSN treelsn;
    if (oplsn_valid && oplsn.lsn <= (treelsn = toku_brt_checkpoint_lsn(brt)).lsn) {
	r = 0;
    } else {
	r = toku_brt_send_insert(brt, key, val, message_xids, type);
    }
    return r;
}

static int
brt_send_update_msg(BRT brt, BRT_MSG_S *msg, TOKUTXN txn) {
    msg->xids = (txn
		 ? toku_txn_get_xids(txn)
		 : xids_get_root_xids());
    int r = toku_brt_root_put_cmd(brt, msg);
    return r;
}

int
toku_brt_maybe_update(BRT brt, const DBT *key, const DBT *update_function_extra,
		      TOKUTXN txn, BOOL oplsn_valid, LSN oplsn,
		      BOOL do_logging) {
    int r = 0;

    TXNID xid = toku_txn_get_txnid(txn);
    if (txn) {
	BYTESTRING keybs = { key->size, key->data };
	r = toku_logger_save_rollback_cmdupdate(
	    txn, toku_cachefile_filenum(brt->cf), &keybs);
	if (r != 0) { goto cleanup; }
	r = toku_txn_note_brt(txn, brt);
	if (r != 0) { goto cleanup; }
    }

    TOKULOGGER logger = toku_txn_logger(txn);
    if (do_logging && logger &&
	brt->h->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
	BYTESTRING keybs = {.len=key->size, .data=key->data};
	BYTESTRING extrabs = {.len=update_function_extra->size,
			      .data=update_function_extra->data};
	r = toku_log_enq_update(logger, NULL, 0,
				toku_cachefile_filenum(brt->cf),
				xid, keybs, extrabs);
	if (r != 0) { goto cleanup; }
    }

    LSN treelsn;
    if (oplsn_valid &&
	oplsn.lsn <= (treelsn = toku_brt_checkpoint_lsn(brt)).lsn) {
	r = 0;
    } else {
	BRT_MSG_S msg = { BRT_UPDATE, ZERO_MSN, NULL,
			  .u.id = { key, update_function_extra }};
	r = brt_send_update_msg(brt, &msg, txn);
    }

cleanup:
    return r;
}

int
toku_brt_maybe_update_broadcast(BRT brt, const DBT *update_function_extra,
				TOKUTXN txn, BOOL oplsn_valid, LSN oplsn,
				BOOL do_logging, BOOL is_resetting_op) {
    int r = 0;

    TXNID xid = toku_txn_get_txnid(txn);
    u_int8_t  resetting = is_resetting_op ? 1 : 0;
    if (txn) {
	r = toku_logger_save_rollback_cmdupdatebroadcast(txn, toku_cachefile_filenum(brt->cf), resetting);
	if (r != 0) { goto cleanup; }
	r = toku_txn_note_brt(txn, brt);
	if (r != 0) { goto cleanup; }
    }

    TOKULOGGER logger = toku_txn_logger(txn);
    if (do_logging && logger &&
	brt->h->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
	BYTESTRING extrabs = {.len=update_function_extra->size,
			      .data=update_function_extra->data};
	r = toku_log_enq_updatebroadcast(logger, NULL, 0,
					 toku_cachefile_filenum(brt->cf),
					 xid, extrabs, resetting);
	if (r != 0) { goto cleanup; }
    }

    LSN treelsn;
    if (oplsn_valid &&
	oplsn.lsn <= (treelsn = toku_brt_checkpoint_lsn(brt)).lsn) {
	r = 0;
    } else {
	DBT nullkey;
	const DBT *nullkeyp = toku_init_dbt(&nullkey);
	BRT_MSG_S msg = { BRT_UPDATE_BROADCAST_ALL, ZERO_MSN, NULL,
			  .u.id = { nullkeyp, update_function_extra }};
	r = brt_send_update_msg(brt, &msg, txn);
    }

cleanup:
    return r;
}

int
toku_brt_send_insert(BRT brt, DBT *key, DBT *val, XIDS xids, enum brt_msg_type type) {
    BRT_MSG_S brtcmd = { type, ZERO_MSN, xids, .u.id = { key, val }};
    int r = toku_brt_root_put_cmd(brt, &brtcmd);
    return r;
}

int
toku_brt_send_commit_any(BRT brt, DBT *key, XIDS xids) {
    DBT val; 
    BRT_MSG_S brtcmd = { BRT_COMMIT_ANY, ZERO_MSN, xids, .u.id = { key, toku_init_dbt(&val) }};
    int r = toku_brt_root_put_cmd(brt, &brtcmd);
    return r;
}

int toku_brt_delete(BRT brt, DBT *key, TOKUTXN txn) {
    return toku_brt_maybe_delete(brt, key, txn, FALSE, ZERO_LSN, TRUE);
}

int
toku_brt_log_del(TOKUTXN txn, BRT brt, const DBT *key) {
    int r = 0;
    TOKULOGGER logger = toku_txn_logger(txn);
    if (logger && brt->h->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
	BYTESTRING keybs = {.len=key->size, .data=key->data};
	TXNID xid = toku_txn_get_txnid(txn);
	r = toku_log_enq_delete_any(logger, (LSN*)0, 0, toku_cachefile_filenum(brt->cf), xid, keybs);
    }
    return r;
}

int
toku_brt_log_del_multiple (TOKUTXN txn, BRT src_brt, BRT *brts, int num_brts, const DBT *key, const DBT *val) {
    int r = 0;
    assert(txn);
    assert(num_brts > 0);
    TOKULOGGER logger = toku_txn_logger(txn);
    if (logger) {
	FILENUM	 fnums[num_brts];
	int i;
	int num_unsuppressed_brts = 0;
	for (i = 0; i < num_brts; i++) {
	    if (brts[i]->h->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
		//Logging not suppressed for this brt.
		fnums[num_unsuppressed_brts++] = toku_cachefile_filenum(brts[i]->cf);
	    }
	}
	if (num_unsuppressed_brts) {
	    FILENUMS filenums = {.num = num_unsuppressed_brts, .filenums = fnums};
	    BYTESTRING keybs = {.len=key->size, .data=key->data};
	    BYTESTRING valbs = {.len=val->size, .data=val->data};
	    TXNID xid = toku_txn_get_txnid(txn);
	    FILENUM src_filenum = src_brt ? toku_cachefile_filenum(src_brt->cf) : FILENUM_NONE;
	    r = toku_log_enq_delete_multiple(logger, (LSN*)0, 0, src_filenum, filenums, xid, keybs, valbs);
	}
    }
    return r;
}

int 
toku_brt_maybe_delete(BRT brt, DBT *key, TOKUTXN txn, BOOL oplsn_valid, LSN oplsn, BOOL do_logging) {
    int r;
    XIDS message_xids = xids_get_root_xids(); //By default use committed messages
    TXNID xid = toku_txn_get_txnid(txn);
    if (txn) {
	if (brt->h->txnid_that_created_or_locked_when_empty != xid) {
	    BYTESTRING keybs  = {key->size, key->data};
	    r = toku_logger_save_rollback_cmddelete(txn, toku_cachefile_filenum(brt->cf), &keybs);
	    if (r!=0) return r;
	    r = toku_txn_note_brt(txn, brt);
	    if (r!=0) return r;
	    //We have transactions, and this is not 2440.  We must send the full root-to-leaf-path
	    message_xids = toku_txn_get_xids(txn);
	}
	else if (txn->ancestor_txnid64 != brt->h->root_xid_that_created) {
	    //We have transactions, and this is 2440, however the txn doing 2440 did not create the dictionary.	 We must send the full root-to-leaf-path
	    message_xids = toku_txn_get_xids(txn);
	}
    }
    TOKULOGGER logger = toku_txn_logger(txn);
    if (do_logging && logger &&
	brt->h->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
	BYTESTRING keybs = {.len=key->size, .data=key->data};
	r = toku_log_enq_delete_any(logger, (LSN*)0, 0, toku_cachefile_filenum(brt->cf), xid, keybs);
	if (r!=0) return r;
    }
    
    LSN treelsn;
    if (oplsn_valid && oplsn.lsn <= (treelsn = toku_brt_checkpoint_lsn(brt)).lsn) {
	r = 0;
    } else {
	r = toku_brt_send_delete(brt, key, message_xids);
    }
    return r;
}

int
toku_brt_send_delete(BRT brt, DBT *key, XIDS xids) {
    DBT val; toku_init_dbt(&val);
    BRT_MSG_S brtcmd = { BRT_DELETE_ANY, ZERO_MSN, xids, .u.id = { key, &val }};
    int result = toku_brt_root_put_cmd(brt, &brtcmd);
    return result;
}

/* ******************** open,close and create  ********************** */

// Test only function (not used in running system). This one has no env
int toku_open_brt (const char *fname, int is_create, BRT *newbrt, int nodesize, CACHETABLE cachetable, TOKUTXN txn,
		   int (*compare_fun)(DB*,const DBT*,const DBT*), DB *db) {
    BRT brt;
    int r;
    const int only_create = 0;

    r = toku_brt_create(&brt);
    if (r != 0)
	return r;
    r = toku_brt_set_nodesize(brt, nodesize); assert_zero(r);
    r = toku_brt_set_bt_compare(brt, compare_fun); assert_zero(r);

    r = toku_brt_open(brt, fname, is_create, only_create, cachetable, txn, db);
    if (r != 0) {
	return r;
    }

    *newbrt = brt;
    return r;
}

static int setup_initial_brt_root_node (BRT t, BLOCKNUM blocknum) {
    BRTNODE XMALLOC(node);
    toku_initialize_empty_brtnode(node, blocknum, 0, 1, t->h->layout_version, t->h->nodesize, t->flags);
    BP_STATE(node,0) = PT_AVAIL;

    u_int32_t fullhash = toku_cachetable_hash(t->cf, blocknum);
    node->fullhash = fullhash;
    int r = toku_cachetable_put(t->cf, blocknum, fullhash,
                                node, brtnode_memory_size(node),
                                toku_brtnode_flush_callback, toku_brtnode_pe_callback, t->h);
    if (r != 0)
	toku_free(node);
    else
        toku_unpin_brtnode(t, node);
    return r;
}

// open a file for use by the brt
// Requires:  File does not exist.
static int brt_create_file(BRT brt, const char *fname, int *fdp) {
    brt = brt;
    mode_t mode = S_IRWXU|S_IRWXG|S_IRWXO;
    int r;
    int fd;
    fd = open(fname, O_RDWR | O_BINARY, mode);
    assert(fd==-1);
    if (errno != ENOENT) {
	r = errno;
	return r;
    }
    fd = open(fname, O_RDWR | O_CREAT | O_BINARY, mode);
    if (fd==-1) {
	r = errno;
	return r;
    }

    r = toku_fsync_directory(fname);
    resource_assert_zero(r);

    *fdp = fd;
    return 0;
}

// open a file for use by the brt.  if the file does not exist, error
static int brt_open_file(const char *fname, int *fdp) {
    mode_t mode = S_IRWXU|S_IRWXG|S_IRWXO;
    int r;
    int fd;
    fd = open(fname, O_RDWR | O_BINARY, mode);
    if (fd==-1) {
	r = errno;
	assert(r!=0);
	return r;
    }
    *fdp = fd;
    return 0;
}

static int
brtheader_log_fassociate_during_checkpoint (CACHEFILE cf, void *header_v) {
    struct brt_header *h = header_v;
    char* fname_in_env = toku_cachefile_fname_in_env(cf);
    BYTESTRING bs = { strlen(fname_in_env), // don't include the NUL
		      fname_in_env };
    TOKULOGGER logger = toku_cachefile_logger(cf);
    FILENUM filenum = toku_cachefile_filenum (cf);
    int r = toku_log_fassociate(logger, NULL, 0, filenum, h->flags, bs);
    return r;
}

static int
brtheader_log_suppress_rollback_during_checkpoint (CACHEFILE cf, void *header_v) {
    int r = 0;
    struct brt_header *h = header_v;
    TXNID xid = h->txnid_that_created_or_locked_when_empty;
    if (xid != TXNID_NONE) {
	//Only log if useful.
	TOKULOGGER logger = toku_cachefile_logger(cf);
	FILENUM filenum = toku_cachefile_filenum (cf);
	r = toku_log_suppress_rollback(logger, NULL, 0, filenum, xid);
    }
    return r;
}


static int brtheader_note_pin_by_checkpoint (CACHEFILE cachefile, void *header_v);
static int brtheader_note_unpin_by_checkpoint (CACHEFILE cachefile, void *header_v);

static int 
brt_init_header_partial (BRT t, TOKUTXN txn) {
    int r;
    t->h->flags = t->flags;
    if (t->h->cf!=NULL) assert(t->h->cf == t->cf);
    t->h->cf = t->cf;
    t->h->nodesize=t->nodesize;
    t->h->num_blocks_to_upgrade = 0;
    t->h->root_xid_that_created = txn ? txn->ancestor_txnid64 : TXNID_NONE;

    compute_and_fill_remembered_hash(t);

    t->h->root_put_counter = global_root_put_counter++; 
	    
    BLOCKNUM root = t->h->root;
    if ((r=setup_initial_brt_root_node(t, root))!=0) { return r; }
    //printf("%s:%d putting %p (%d)\n", __FILE__, __LINE__, t->h, 0);
    toku_cachefile_set_userdata(t->cf,
				t->h,
				brtheader_log_fassociate_during_checkpoint,
				brtheader_log_suppress_rollback_during_checkpoint,
				toku_brtheader_close,
				toku_brtheader_checkpoint,
				toku_brtheader_begin_checkpoint,
				toku_brtheader_end_checkpoint,
				brtheader_note_pin_by_checkpoint,
				brtheader_note_unpin_by_checkpoint);

    return r;
}

static int
brt_init_header (BRT t, TOKUTXN txn) {
    t->h->type = BRTHEADER_CURRENT;
    t->h->checkpoint_header = NULL;
    toku_blocktable_create_new(&t->h->blocktable);
    BLOCKNUM root;
    //Assign blocknum for root block, also dirty the header
    toku_allocate_blocknum(t->h->blocktable, &root, t->h);
    t->h->root = root;

    toku_list_init(&t->h->live_brts);
    toku_list_init(&t->h->zombie_brts);
    toku_list_init(&t->h->checkpoint_before_commit_link);
    int r = brt_init_header_partial(t, txn);
    if (r==0) toku_block_verify_no_free_blocknums(t->h->blocktable);
    return r;
}


// allocate and initialize a brt header.
// t->cf is not set to anything.
static int 
brt_alloc_init_header(BRT t, TOKUTXN txn) {
    int r;
    uint64_t now = (uint64_t) time(NULL);

    r = brtheader_alloc(&t->h);
    if (r != 0) {
	if (0) { died2: toku_free(t->h); }
	t->h=0;
	return r;
    }

    t->h->layout_version = BRT_LAYOUT_VERSION;
    t->h->layout_version_original = BRT_LAYOUT_VERSION;
    t->h->layout_version_read_from_disk = BRT_LAYOUT_VERSION;	     // fake, prevent unnecessary upgrade logic

    t->h->build_id = BUILD_ID;
    t->h->build_id_original = BUILD_ID;
    
    t->h->time_of_creation = now;
    t->h->time_of_last_modification = 0;

    memset(&t->h->descriptor, 0, sizeof(t->h->descriptor));

    r = brt_init_header(t, txn);
    if (r != 0) goto died2;
    return r;
}

int toku_read_brt_header_and_store_in_cachefile (CACHEFILE cf, LSN max_acceptable_lsn, struct brt_header **header, BOOL* was_open)
// If the cachefile already has the header, then just get it.
// If the cachefile has not been initialized, then don't modify anything.
// max_acceptable_lsn is the latest acceptable checkpointed version of the file.
{
    {
	struct brt_header *h;
	if ((h=toku_cachefile_get_userdata(cf))!=0) {
	    *header = h;
	    *was_open = TRUE;
	    return 0;
	}
    }
    *was_open = FALSE;
    struct brt_header *h;
    int r;
    {
	int fd = toku_cachefile_get_and_pin_fd (cf);
	r = toku_deserialize_brtheader_from(fd, max_acceptable_lsn, &h);
	toku_cachefile_unpin_fd(cf);
    }
    if (r!=0) return r;
    h->cf = cf;
    h->root_put_counter = global_root_put_counter++;
    toku_cachefile_set_userdata(cf,
				(void*)h,
				brtheader_log_fassociate_during_checkpoint,
				brtheader_log_suppress_rollback_during_checkpoint,
				toku_brtheader_close,
				toku_brtheader_checkpoint,
				toku_brtheader_begin_checkpoint,
				toku_brtheader_end_checkpoint,
				brtheader_note_pin_by_checkpoint,
				brtheader_note_unpin_by_checkpoint);
    *header = h;
    return 0;
}

static void
brtheader_note_brt_close(BRT t) {
    struct brt_header *h = t->h;
    if (h) { //Might not yet have been opened.
	toku_brtheader_lock(h);
	toku_list_remove(&t->live_brt_link);
	toku_list_remove(&t->zombie_brt_link);
	toku_brtheader_unlock(h);
    }
}

static int
brtheader_note_brt_open(BRT live) {
    struct brt_header *h = live->h;
    int retval = 0;
    toku_brtheader_lock(h);
    while (!toku_list_empty(&h->zombie_brts)) {
	//Remove dead brt from list
	BRT zombie = toku_list_struct(toku_list_pop(&h->zombie_brts), struct brt, zombie_brt_link);
	toku_brtheader_unlock(h); //Cannot be holding lock when swapping brts.
	retval = toku_txn_note_swap_brt(live, zombie); //Steal responsibility, close
	toku_brtheader_lock(h);
	if (retval) break;
    }
    if (retval==0) {
	toku_list_push(&h->live_brts, &live->live_brt_link);
	h->dictionary_opened = TRUE;
    }

    toku_brtheader_unlock(h);
    return retval;
}

static int
verify_builtin_comparisons_consistent(BRT t, u_int32_t flags) {
    if ((flags & TOKU_DB_KEYCMP_BUILTIN) && (t->compare_fun != toku_builtin_compare_fun))
	return EINVAL;
    return 0;
}

int
toku_update_descriptor(struct brt_header * h, DESCRIPTOR d, int fd) {
    int r = 0;
    DISKOFF offset;
    //4 for checksum
    toku_realloc_descriptor_on_disk(h->blocktable, toku_serialize_descriptor_size(d)+4, &offset, h);
    r = toku_serialize_descriptor_contents_to_fd(fd, d, offset);
    if (r) {
	goto cleanup;
    }
    if (h->descriptor.dbt.data) {
	toku_free(h->descriptor.dbt.data);
    }
    h->descriptor.dbt.size = d->dbt.size;
    h->descriptor.dbt.data = toku_memdup(d->dbt.data, d->dbt.size);

    r = 0;
cleanup:
    return r;
}

int
toku_brt_change_descriptor(
    BRT t, 
    const DBT* old_descriptor, 
    const DBT* new_descriptor, 
    BOOL do_log, 
    TOKUTXN txn
    ) 
{
    int r = 0;
    int fd;
    DESCRIPTOR_S new_d;
    BYTESTRING old_desc_bs = { old_descriptor->size, old_descriptor->data };
    BYTESTRING new_desc_bs = { new_descriptor->size, new_descriptor->data };
    if (!txn) {
	r = EINVAL;
	goto cleanup;
    }
    // put information into rollback file
    r = toku_logger_save_rollback_change_fdescriptor(
	txn, 
	toku_cachefile_filenum(t->cf), 
	&old_desc_bs
	);
    if (r != 0) { goto cleanup; }
    r = toku_txn_note_brt(txn, t);
    if (r != 0) { goto cleanup; }

    if (do_log) {
	TOKULOGGER logger = toku_txn_logger(txn);
	TXNID xid = toku_txn_get_txnid(txn);
	r = toku_log_change_fdescriptor(
	    logger, NULL, 0, 
	    toku_cachefile_filenum(t->cf),
	    xid,
	    old_desc_bs,
	    new_desc_bs
	    );
	if (r != 0) { goto cleanup; }
    }

    // write new_descriptor to header
    new_d.dbt = *new_descriptor;
    fd = toku_cachefile_get_and_pin_fd (t->cf);
    r = toku_update_descriptor(t->h, &new_d, fd);
    if (r == 0)	 // very infrequent operation, worth precise threadsafe count
	(void) toku_sync_fetch_and_increment_uint64(&update_status.descriptor_set);
    toku_cachefile_unpin_fd(t->cf);
    if (r!=0) goto cleanup;

cleanup:
    return r;
}

// This is the actual open, used for various purposes, such as normal use, recovery, and redirect.  
// fname_in_env is the iname, relative to the env_dir  (data_dir is already in iname as prefix).
// The checkpointed version (checkpoint_lsn) of the dictionary must be no later than max_acceptable_lsn .
static int
brt_open(BRT t, const char *fname_in_env, int is_create, int only_create, CACHETABLE cachetable, TOKUTXN txn, DB *db, FILENUM use_filenum, DICTIONARY_ID use_dictionary_id, LSN max_acceptable_lsn) {
    int r;
    BOOL txn_created = FALSE;

    if (t->did_set_flags) {
	r = verify_builtin_comparisons_consistent(t, t->flags);
	if (r!=0) return r;
    }

    //printf("%s:%d %d alloced\n", __FILE__, __LINE__, get_n_items_malloced()); toku_print_malloced_items();
    WHEN_BRTTRACE(fprintf(stderr, "BRTTRACE: %s:%d toku_brt_open(%s, \"%s\", %d, %p, %d, %p)\n",
			  __FILE__, __LINE__, fname_in_env, dbname, is_create, newbrt, nodesize, cachetable));
    char *fname_in_cwd = toku_cachetable_get_fname_in_cwd(cachetable, fname_in_env);
    if (0) { died0:  if (fname_in_cwd) toku_free(fname_in_cwd); assert(r); return r; }

    assert(is_create || !only_create);
    t->db = db;
    BOOL did_create = FALSE;
    FILENUM reserved_filenum = use_filenum;
    {
	int fd = -1;
	r = brt_open_file(fname_in_cwd, &fd);
	int use_reserved_filenum = reserved_filenum.fileid != FILENUM_NONE.fileid;
	if (r==ENOENT && is_create) {
	    toku_cachetable_reserve_filenum(cachetable, &reserved_filenum, use_reserved_filenum, reserved_filenum);
	    if (0) {
		died1:
		if (did_create)
		    toku_cachetable_unreserve_filenum(cachetable, reserved_filenum);
		goto died0;
	    }
	    if (use_reserved_filenum) assert(reserved_filenum.fileid == use_filenum.fileid);
	    did_create = TRUE;
	    mode_t mode = S_IRWXU|S_IRWXG|S_IRWXO;
	    if (txn) {
		BYTESTRING bs = { .len=strlen(fname_in_env), .data = (char*)fname_in_env };
		r = toku_logger_save_rollback_fcreate(txn, reserved_filenum, &bs); // bs is a copy of the fname relative to the environment
		if (r != 0) goto died1;
	    }
	    txn_created = (BOOL)(txn!=NULL);
	    r = toku_logger_log_fcreate(txn, fname_in_env, reserved_filenum, mode, t->flags, t->nodesize);
	    if (r!=0) goto died1;
	    r = brt_create_file(t, fname_in_cwd, &fd);
	}
	toku_free(fname_in_cwd);
	fname_in_cwd = NULL;
	if (r != 0) goto died1;
	// TODO: #2090
	r=toku_cachetable_openfd_with_filenum(&t->cf, cachetable, fd, 
					      fname_in_env,
					      use_reserved_filenum||did_create, reserved_filenum, did_create);
	if (r != 0) goto died1;
    }
    if (r!=0) {
	died_after_open: 
	toku_cachefile_close(&t->cf, 0, FALSE, ZERO_LSN);
	goto died1;
    }
    assert(t->nodesize>0);
    //printf("%s:%d %d alloced\n", __FILE__, __LINE__, get_n_items_malloced()); toku_print_malloced_items();
    if (0) {
    died_after_read_and_pin:
	goto died_after_open;
    }
    BOOL was_already_open;
    if (is_create) {
	r = toku_read_brt_header_and_store_in_cachefile(t->cf, max_acceptable_lsn, &t->h, &was_already_open);
	if (r==TOKUDB_DICTIONARY_NO_HEADER) {
	    r = brt_alloc_init_header(t, txn);
	    if (r != 0) goto died_after_read_and_pin;
	}
	else if (r!=0) {
	    goto died_after_read_and_pin;
	}
	else if (only_create) {
	    assert_zero(r);
	    r = EEXIST;
	    goto died_after_read_and_pin;
	}
	else goto found_it;
    } else {
	if ((r = toku_read_brt_header_and_store_in_cachefile(t->cf, max_acceptable_lsn, &t->h, &was_already_open))!=0) goto died_after_open;
	found_it:
	t->nodesize = t->h->nodesize;		      /* inherit the pagesize from the file */
	if (!t->did_set_flags) {
	    r = verify_builtin_comparisons_consistent(t, t->flags);
	    if (r!=0) goto died_after_read_and_pin;
	    t->flags = t->h->flags;
	    t->did_set_flags = TRUE;
	} else {
	    if (t->flags != t->h->flags) {		  /* if flags have been set then flags must match */
		r = EINVAL; goto died_after_read_and_pin;
	    }
	}
    }

    if (!was_already_open) {
	if (!did_create) { //Only log the fopen that OPENs the file.  If it was already open, don't log.
	    r = toku_logger_log_fopen(txn, fname_in_env, toku_cachefile_filenum(t->cf), t->flags);
	    if (r!=0) goto died_after_read_and_pin;
	}
    }
    int use_reserved_dict_id = use_dictionary_id.dictid != DICTIONARY_ID_NONE.dictid;
    if (!was_already_open) {
	DICTIONARY_ID dict_id;
	if (use_reserved_dict_id)
	    dict_id = use_dictionary_id;
	else
	    dict_id = next_dict_id();
	t->h->dict_id = dict_id;
    }
    else {
	// dict_id is already in header
	if (use_reserved_dict_id)
	    assert(t->h->dict_id.dictid == use_dictionary_id.dictid);
    }
    assert(t->h);
    assert(t->h->dict_id.dictid != DICTIONARY_ID_NONE.dictid);
    assert(t->h->dict_id.dictid < dict_id_serial);

    r = toku_maybe_upgrade_brt(t);	  // possibly do some work to complete the version upgrade of brt
    if (r!=0) goto died_after_read_and_pin;

    // brtheader_note_brt_open must be after all functions that can fail.
    r = brtheader_note_brt_open(t);
    if (r!=0) goto died_after_read_and_pin;
    if (t->db) t->db->descriptor = &t->h->descriptor;
    if (txn_created) {
	assert(txn);
	toku_brt_header_suppress_rollbacks(t->h, txn);
	r = toku_txn_note_brt(txn, t);
	assert_zero(r);
    }

    //Opening a brt may restore to previous checkpoint.	 Truncate if necessary.
    {
	int fd = toku_cachefile_get_and_pin_fd (t->h->cf);
	toku_maybe_truncate_cachefile_on_open(t->h->blocktable, fd, t->h);
	toku_cachefile_unpin_fd(t->h->cf);
    }
    WHEN_BRTTRACE(fprintf(stderr, "BRTTRACE -> %p\n", t));
    return 0;
}

// Open a brt for the purpose of recovery, which requires that the brt be open to a pre-determined FILENUM
// and may require a specific checkpointed version of the file.	 
// (dict_id is assigned by the brt_open() function.)
int
toku_brt_open_recovery(BRT t, const char *fname_in_env, int is_create, int only_create, CACHETABLE cachetable, TOKUTXN txn, 
		       DB *db, FILENUM use_filenum, LSN max_acceptable_lsn) {
    int r;
    assert(use_filenum.fileid != FILENUM_NONE.fileid);
    r = brt_open(t, fname_in_env, is_create, only_create, cachetable,
		 txn, db, use_filenum, DICTIONARY_ID_NONE, max_acceptable_lsn);
    return r;
}

// Open a brt in normal use.  The FILENUM and dict_id are assigned by the brt_open() function.
int
toku_brt_open(BRT t, const char *fname_in_env, int is_create, int only_create, CACHETABLE cachetable, TOKUTXN txn, DB *db) {
    int r;
    r = brt_open(t, fname_in_env, is_create, only_create, cachetable, txn, db, FILENUM_NONE, DICTIONARY_ID_NONE, MAX_LSN);
    return r;
}

// Open a brt for use by redirect.  The new brt must have the same dict_id as the old_brt passed in.  (FILENUM is assigned by the brt_open() function.)
static int
brt_open_for_redirect(BRT *new_brtp, const char *fname_in_env, TOKUTXN txn, BRT old_brt) {
    int r;
    BRT t;
    struct brt_header *old_h = old_brt->h;
    assert(old_h->dict_id.dictid != DICTIONARY_ID_NONE.dictid);
    r = toku_brt_create(&t);
    assert_zero(r);
    r = toku_brt_set_bt_compare(t, old_brt->compare_fun);
    assert_zero(r);
    r = toku_brt_set_update(t, old_brt->update_fun);
    assert_zero(r);
    r = toku_brt_set_nodesize(t, old_brt->nodesize);
    assert_zero(r);
    CACHETABLE ct = toku_cachefile_get_cachetable(old_brt->cf);
    r = brt_open(t, fname_in_env, 0, 0, ct, txn, old_brt->db, FILENUM_NONE, old_h->dict_id, MAX_LSN);
    assert_zero(r);
    assert(t->h->dict_id.dictid == old_h->dict_id.dictid);
    assert(t->db == old_brt->db);

    *new_brtp = t;
    return r;
}



//Callback to ydb layer to set db->i->brt = brt
//Used for redirect.
static void (*callback_db_set_brt)(DB *db, BRT brt)  = NULL;

static void
brt_redirect_cursors (BRT brt_to, BRT brt_from) {
    assert(brt_to->db == brt_from->db);
    while (!toku_list_empty(&brt_from->cursors)) {
	struct toku_list * c_list = toku_list_head(&brt_from->cursors);
	BRT_CURSOR c = toku_list_struct(c_list, struct brt_cursor, cursors_link);
	brt_cursor_invalidate(c);

	toku_list_remove(&c->cursors_link);

	toku_list_push(&brt_to->cursors, &c->cursors_link);

	c->brt = brt_to;
    }
}

static void
brt_redirect_db (BRT brt_to, BRT brt_from) {
    assert(brt_to->db == brt_from->db);
    callback_db_set_brt(brt_from->db, brt_to);
}

static int
fake_db_brt_close_delayed(DB *db, u_int32_t UU(flags)) {
    BRT brt_to_close = db->api_internal;
    char *error_string = NULL;
    int r = toku_close_brt(brt_to_close, &error_string);
    assert_zero(r);
    assert(error_string == NULL);
    toku_free(db);
    return 0;
}

static int
toku_brt_header_close_redirected_brts(struct brt_header * h) {
//Requires:
//  toku_brt_db_delay_closed has NOT been called on any brts referring to h.
//For each brt referring to h, close it.
    struct toku_list *list;
    int num_brts = 0;
    for (list = h->live_brts.next; list != &h->live_brts; list = list->next) {
	num_brts++;
    }
    assert(num_brts>0);
    BRT brts[num_brts];
    DB *dbs[num_brts];
    int which = 0;
    for (list = h->live_brts.next; list != &h->live_brts; list = list->next) {
	XCALLOC(dbs[which]);
	brts[which] = toku_list_struct(list, struct brt, live_brt_link);
	assert(!brts[which]->was_closed);
	dbs[which]->api_internal = brts[which];
	brts[which]->db = dbs[which];
	which++;
    }
    assert(which == num_brts);
    for (which = 0; which < num_brts; which++) {
	int r;
	r = toku_brt_db_delay_closed(brts[which], dbs[which], fake_db_brt_close_delayed, 0);
	assert_zero(r);
    }
    return 0;
}



// This function performs most of the work to redirect a dictionary to different file.
// It is called for redirect and to abort a redirect.  (This function is almost its own inverse.)
static int
dictionary_redirect_internal(const char *dst_fname_in_env, struct brt_header *src_h, TOKUTXN txn, struct brt_header **dst_hp) {
    int r;

    assert(toku_list_empty(&src_h->zombie_brts));
    assert(!toku_list_empty(&src_h->live_brts));

    FILENUM src_filenum = toku_cachefile_filenum(src_h->cf);
    FILENUM dst_filenum = FILENUM_NONE;

    struct brt_header *dst_h = NULL;
    struct toku_list *list;
    for (list = src_h->live_brts.next; list != &src_h->live_brts; list = list->next) {
	BRT src_brt;
	src_brt = toku_list_struct(list, struct brt, live_brt_link);
	assert(!src_brt->was_closed);

	BRT dst_brt;
	r = brt_open_for_redirect(&dst_brt, dst_fname_in_env, txn, src_brt);
	assert_zero(r);
	if (dst_filenum.fileid==FILENUM_NONE.fileid) {	// if first time through loop
	    dst_filenum = toku_cachefile_filenum(dst_brt->cf);
	    assert(dst_filenum.fileid!=FILENUM_NONE.fileid);
	    assert(dst_filenum.fileid!=src_filenum.fileid); //Cannot be same file.
	}
	else { // All dst_brts must have same filenum
	    assert(dst_filenum.fileid == toku_cachefile_filenum(dst_brt->cf).fileid);
	}
	if (!dst_h) dst_h = dst_brt->h;
	else assert(dst_h == dst_brt->h);

	//Do not need to swap descriptors pointers.
	//Done by brt_open_for_redirect
	assert(dst_brt->db->descriptor == &dst_brt->h->descriptor);

	//Set db->i->brt to new brt
	brt_redirect_db(dst_brt, src_brt);
	
	//Move cursors.
	brt_redirect_cursors (dst_brt, src_brt);
    }
    assert(dst_h);

    r = toku_brt_header_close_redirected_brts(src_h);
    assert_zero(r);
    *dst_hp = dst_h;

    return r;

}


//This is the 'abort redirect' function.  The redirect of old_h to new_h was done
//and now must be undone, so here we redirect new_h back to old_h.
int
toku_dictionary_redirect_abort(struct brt_header *old_h, struct brt_header *new_h, TOKUTXN txn) {
    char *old_fname_in_env = toku_cachefile_fname_in_env(old_h->cf);

    int r;
    {
	FILENUM old_filenum = toku_cachefile_filenum(old_h->cf);
	FILENUM new_filenum = toku_cachefile_filenum(new_h->cf);
	assert(old_filenum.fileid!=new_filenum.fileid); //Cannot be same file.

	//No living brts in old header.
	assert(toku_list_empty(&old_h->live_brts));
	//Must have a zombie in old header.
	assert(!toku_list_empty(&old_h->zombie_brts));
    }

    // If application did not close all DBs using the new file, then there should 
    // be no zombies and we need to redirect the DBs back to the original file.
    if (!toku_list_empty(&new_h->live_brts)) {
	assert(toku_list_empty(&new_h->zombie_brts));
	struct brt_header *dst_h;
	// redirect back from new_h to old_h
	r = dictionary_redirect_internal(old_fname_in_env, new_h, txn, &dst_h);
	assert_zero(r);
	assert(dst_h == old_h);
    }
    else {
	//No live brts.	 Zombies on both sides will die on their own eventually.
	//No need to redirect back.
	assert(!toku_list_empty(&new_h->zombie_brts));
	r = 0;
    }
    return r;
}


/****
 * on redirect or abort:
 *  if redirect txn_note_doing_work(txn)
 *  if redirect connect src brt to txn (txn modified this brt)
 *  for each src brt
 *    open brt to dst file (create new brt struct)
 *    if redirect connect dst brt to txn 
 *    redirect db to new brt
 *    redirect cursors to new brt
 *  close all src brts
 *  if redirect make rollback log entry
 * 
 * on commit:
 *   nothing to do
 *
 *****/

int 
toku_dictionary_redirect (const char *dst_fname_in_env, BRT old_brt, TOKUTXN txn) {
// Input args:
//   new file name for dictionary (relative to env)
//   old_brt is a live brt of open handle ({DB, BRT} pair) that currently refers to old dictionary file.
//   (old_brt may be one of many handles to the dictionary.)
//   txn that created the loader
// Requires: 
//   ydb_lock is held.
//   The brt is open.  (which implies there can be no zombies.)
//   The new file must be a valid dictionary.
//   The block size and flags in the new file must match the existing BRT.
//   The new file must already have its descriptor in it (and it must match the existing descriptor).
// Effect:   
//   Open new BRTs (and related header and cachefile) to the new dictionary file with a new FILENUM.
//   Redirect all DBs that point to brts that point to the old file to point to brts that point to the new file.
//   Copy the dictionary id (dict_id) from the header of the original file to the header of the new file.
//   Create a rollback log entry.
//   The original BRT, header, cachefile and file remain unchanged.  They will be cleaned up on commmit.
//   If the txn aborts, then this operation will be undone
    int r;

    struct brt_header * old_h = old_brt->h;

    // dst file should not be open.  (implies that dst and src are different because src must be open.)
    {
	CACHETABLE ct = toku_cachefile_get_cachetable(old_h->cf);
	CACHEFILE cf;
	r = toku_cachefile_of_iname_in_env(ct, dst_fname_in_env, &cf);
	if (r==0) {
	    r = EINVAL;
	    goto cleanup;
	}
	assert(r==ENOENT);
	r = 0;	      
    }

    if (txn) {
	r = toku_txn_note_brt(txn, old_brt);  // mark old brt as touched by this txn
	assert_zero(r);
    }

    struct brt_header *new_h;
    r = dictionary_redirect_internal(dst_fname_in_env, old_h, txn, &new_h);
    assert_zero(r);

    // make rollback log entry
    if (txn) {
	assert(toku_list_empty(&new_h->zombie_brts));
	assert(!toku_list_empty(&new_h->live_brts));
	struct toku_list *list;
	for (list = new_h->live_brts.next; list != &new_h->live_brts; list = list->next) {
	    BRT new_brt;
	    new_brt = toku_list_struct(list, struct brt, live_brt_link);
	    r = toku_txn_note_brt(txn, new_brt);	  // mark new brt as touched by this txn
	    assert_zero(r);
	}
	FILENUM old_filenum = toku_cachefile_filenum(old_h->cf);
	FILENUM new_filenum = toku_cachefile_filenum(new_h->cf);
	r = toku_logger_save_rollback_dictionary_redirect(txn, old_filenum, new_filenum);
	assert_zero(r);

	TXNID xid = toku_txn_get_txnid(txn);
	toku_brt_header_suppress_rollbacks(new_h, txn);
	r = toku_log_suppress_rollback(txn->logger, NULL, 0, new_filenum, xid);
	assert_zero(r);
    }
    
cleanup:
    return r;
}


DICTIONARY_ID
toku_brt_get_dictionary_id(BRT brt) {
    struct brt_header *h = brt->h;
    DICTIONARY_ID dict_id = h->dict_id;
    return dict_id;
}

int toku_brt_set_flags(BRT brt, unsigned int flags) {
    assert(flags==(flags&TOKU_DB_KEYCMP_BUILTIN)); // make sure there are no extraneous flags
    brt->did_set_flags = TRUE;
    brt->flags = flags;
    return 0;
}

int toku_brt_get_flags(BRT brt, unsigned int *flags) {
    *flags = brt->flags;
    assert(brt->flags==(brt->flags&TOKU_DB_KEYCMP_BUILTIN)); // make sure there are no extraneous flags
    return 0;
}


int toku_brt_set_nodesize(BRT brt, unsigned int nodesize) {
    brt->nodesize = nodesize;
    return 0;
}

int toku_brt_get_nodesize(BRT brt, unsigned int *nodesize) {
    *nodesize = brt->nodesize;
    return 0;
}

int toku_brt_set_bt_compare(BRT brt, int (*bt_compare)(DB *, const DBT*, const DBT*)) {
    brt->compare_fun = bt_compare;
    return 0;
}

int toku_brt_set_update(BRT brt, int (*update_fun)(DB *,
						   const DBT *key, const DBT *old_val, const DBT *extra,
						   void (*set_val)(const DBT *new_val, void *set_extra), void *set_extra)) {
    brt->update_fun = update_fun;
    return 0;
}

brt_compare_func toku_brt_get_bt_compare (BRT brt) {
    return brt->compare_fun;
}


int toku_brt_create_cachetable(CACHETABLE *ct, long cachesize, LSN initial_lsn, TOKULOGGER logger) {
    if (cachesize == 0)
	cachesize = 128*1024*1024;
    return toku_create_cachetable(ct, cachesize, initial_lsn, logger);
}

// Create checkpoint-in-progress versions of header and translation (btt) (and fifo for now...).
//Has access to fd (it is protected).
int
toku_brtheader_begin_checkpoint (CACHEFILE UU(cachefile), int UU(fd), LSN checkpoint_lsn, void *header_v) {
    struct brt_header *h = header_v;
    int r = h->panic;
    if (r==0) {
	// hold lock around copying and clearing of dirty bit
	toku_brtheader_lock (h);
	assert(h->type == BRTHEADER_CURRENT);
	assert(h->checkpoint_header == NULL);
	brtheader_copy_for_checkpoint(h, checkpoint_lsn);
	h->dirty = 0;	     // this is only place this bit is cleared	(in currentheader)
	toku_block_translation_note_start_checkpoint_unlocked(h->blocktable);
	toku_brtheader_unlock (h);
    }
    return r;
}

int
toku_brt_zombie_needed(BRT zombie) {
    return toku_omt_size(zombie->txns) != 0 || zombie->pinned_by_checkpoint;
}

//Must be protected by ydb lock.
//Is only called by checkpoint begin, which holds it
static int
brtheader_note_pin_by_checkpoint (CACHEFILE UU(cachefile), void *header_v)
{
    //Set arbitrary brt (for given header) as pinned by checkpoint.
    //Only one can be pinned (only one checkpoint at a time), but not worth verifying.
    struct brt_header *h = header_v;
    BRT brt_to_pin;
    toku_brtheader_lock(h);
    if (!toku_list_empty(&h->live_brts)) {
	brt_to_pin = toku_list_struct(toku_list_head(&h->live_brts), struct brt, live_brt_link);
    }
    else {
	//Header exists, so at least one brt must.  No live means at least one zombie.
	assert(!toku_list_empty(&h->zombie_brts));
	brt_to_pin = toku_list_struct(toku_list_head(&h->zombie_brts), struct brt, zombie_brt_link);
    }
    toku_brtheader_unlock(h);
    assert(!brt_to_pin->pinned_by_checkpoint);
    brt_to_pin->pinned_by_checkpoint = 1;

    return 0;
}

//Must be protected by ydb lock.
//Called by end_checkpoint, which grabs ydb lock around note_unpin
static int
brtheader_note_unpin_by_checkpoint (CACHEFILE UU(cachefile), void *header_v)
{
    //Must find which brt for this header is pinned, and unpin it.
    //Once found, we might have to close it if it was user closed and no txns touch it.
    //
    //HOW do you loop through a 'list'????
    struct brt_header *h = header_v;
    BRT brt_to_unpin = NULL;

    toku_brtheader_lock(h);
    if (!toku_list_empty(&h->live_brts)) {
	struct toku_list *list;
	for (list = h->live_brts.next; list != &h->live_brts; list = list->next) {
	    BRT candidate;
	    candidate = toku_list_struct(list, struct brt, live_brt_link);
	    if (candidate->pinned_by_checkpoint) {
		brt_to_unpin = candidate;
		break;
	    }
	}
    }
    if (!brt_to_unpin) {
	//Header exists, something is pinned, so exactly one zombie must be pinned
	assert(!toku_list_empty(&h->zombie_brts));
	struct toku_list *list;
	for (list = h->zombie_brts.next; list != &h->zombie_brts; list = list->next) {
	    BRT candidate;
	    candidate = toku_list_struct(list, struct brt, zombie_brt_link);
	    if (candidate->pinned_by_checkpoint) {
		brt_to_unpin = candidate;
		break;
	    }
	}
    }
    toku_brtheader_unlock(h);
    assert(brt_to_unpin);
    assert(brt_to_unpin->pinned_by_checkpoint);
    brt_to_unpin->pinned_by_checkpoint = 0; //Unpin
    int r = 0;
    //Close if necessary
    if (brt_to_unpin->was_closed && !toku_brt_zombie_needed(brt_to_unpin)) {
	//Close immediately.
	assert(brt_to_unpin->close_db);
	r = brt_to_unpin->close_db(brt_to_unpin->db, brt_to_unpin->close_flags);
    }
    return r;

}

// Write checkpoint-in-progress versions of header and translation to disk (really to OS internal buffer).
// Must have access to fd (protected)
int
toku_brtheader_checkpoint (CACHEFILE cf, int fd, void *header_v)
{
    struct brt_header *h = header_v;
    struct brt_header *ch = h->checkpoint_header;
    int r = 0;
    if (h->panic!=0) goto handle_error;
    //printf("%s:%d allocated_limit=%lu writing queue to %lu\n", __FILE__, __LINE__,
    //	     block_allocator_allocated_limit(h->block_allocator), h->unused_blocks.b*h->nodesize);
    assert(ch);
    if (ch->panic!=0) goto handle_error;
    assert(ch->type == BRTHEADER_CHECKPOINT_INPROGRESS);
    if (ch->dirty) {	    // this is only place this bit is tested (in checkpoint_header)
	TOKULOGGER logger = toku_cachefile_logger(cf);
	if (logger) {
	    r = toku_logger_fsync_if_lsn_not_fsynced(logger, ch->checkpoint_lsn);
	    if (r!=0) goto handle_error;
	}
	{
	    ch->checkpoint_count++;
	    // write translation and header to disk (or at least to OS internal buffer)
	    r = toku_serialize_brt_header_to(fd, ch);
	    if (r!=0) goto handle_error;
	}
	ch->dirty = 0;		      // this is only place this bit is cleared (in checkpoint_header)
    }
    else toku_block_translation_note_skipped_checkpoint(ch->blocktable);
    if (0) {
handle_error:
	if (h->panic) r = h->panic;
	else if (ch->panic) {
	    r = ch->panic;
	    //Steal panic string.  Cannot afford to malloc.
	    h->panic	     = ch->panic;
	    h->panic_string  = ch->panic_string;
	}
	else toku_block_translation_note_failed_checkpoint(ch->blocktable);
    }
    return r;

}

// Really write everything to disk (fsync dictionary), then free unused disk space 
// (i.e. tell BlockAllocator to liberate blocks used by previous checkpoint).
// Must have access to fd (protected)
int
toku_brtheader_end_checkpoint (CACHEFILE cachefile, int fd, void *header_v) {
    struct brt_header *h = header_v;
    int r = h->panic;
    if (r==0) {
	assert(h->type == BRTHEADER_CURRENT);
	struct brt_header *ch = h->checkpoint_header;
	BOOL checkpoint_success_so_far = (BOOL)(ch->checkpoint_count==h->checkpoint_count+1 && ch->dirty==0);
	if (checkpoint_success_so_far) {
	    r = toku_cachefile_fsync(cachefile);
	    if (r!=0) 
		toku_block_translation_note_failed_checkpoint(h->blocktable);
	    else {
		h->checkpoint_count++;	      // checkpoint succeeded, next checkpoint will save to alternate header location
		h->checkpoint_lsn = ch->checkpoint_lsn;	 //Header updated.
	    }
	}
	toku_block_translation_note_end_checkpoint(h->blocktable, fd, h);
    }
    if (h->checkpoint_header) {	 // could be NULL only if panic was true at begin_checkpoint
	brtheader_free(h->checkpoint_header);
	h->checkpoint_header = NULL;
    }
    return r;
}

//Has access to fd (it is protected).
int
toku_brtheader_close (CACHEFILE cachefile, int fd, void *header_v, char **malloced_error_string, BOOL oplsn_valid, LSN oplsn) {
    struct brt_header *h = header_v;
    assert(h->type == BRTHEADER_CURRENT);
    toku_brtheader_lock(h);
    assert(toku_list_empty(&h->live_brts));
    assert(toku_list_empty(&h->zombie_brts));
    toku_brtheader_unlock(h);
    int r = 0;
    if (h->panic) {
	r = h->panic;
    } else if (h->dictionary_opened) { //Otherwise header has never fully been created.
	assert(h->cf == cachefile);
	TOKULOGGER logger = toku_cachefile_logger(cachefile);
	LSN lsn = ZERO_LSN;
	//Get LSN
	if (oplsn_valid) {
	    //Use recovery-specified lsn
	    lsn = oplsn;
	    //Recovery cannot reduce lsn of a header.
	    if (lsn.lsn < h->checkpoint_lsn.lsn)
		lsn = h->checkpoint_lsn;
	}
	else {
	    //Get LSN from logger
	    lsn = ZERO_LSN; // if there is no logger, we use zero for the lsn
	    if (logger) {
		char* fname_in_env = toku_cachefile_fname_in_env(cachefile);
		assert(fname_in_env);
		BYTESTRING bs = {.len=strlen(fname_in_env), .data=fname_in_env};
		r = toku_log_fclose(logger, &lsn, h->dirty, bs, toku_cachefile_filenum(cachefile)); // flush the log on close (if new header is being written), otherwise it might not make it out.
		if (r!=0) return r;
	    }
	}
	if (h->dirty) {	       // this is the only place this bit is tested (in currentheader)
	    if (logger) { //Rollback cachefile MUST NOT BE CLOSED DIRTY
			  //It can be checkpointed only via 'checkpoint'
		assert(logger->rollback_cachefile != cachefile);
	    }
	    int r2;
	    //assert(lsn.lsn!=0);
	    r2 = toku_brtheader_begin_checkpoint(cachefile, fd, lsn, header_v);
	    if (r==0) r = r2;
	    r2 = toku_brtheader_checkpoint(cachefile, fd, h);
	    if (r==0) r = r2;
	    r2 = toku_brtheader_end_checkpoint(cachefile, fd, header_v);
	    if (r==0) r = r2;
	    if (!h->panic) assert(!h->dirty);	     // dirty bit should be cleared by begin_checkpoint and never set again (because we're closing the dictionary)
	}
    }
    if (malloced_error_string) *malloced_error_string = h->panic_string;
    if (r == 0) {
	r = h->panic;
    }
    toku_brtheader_free(h);
    return r;
}

int
toku_brt_db_delay_closed (BRT zombie, DB* db, int (*close_db)(DB*, u_int32_t), u_int32_t close_flags) {
//Requires: close_db needs to call toku_close_brt to delete the final reference.
    int r;
    struct brt_header *h = zombie->h;
    if (zombie->was_closed) r = EINVAL;
    else if (zombie->db && zombie->db!=db) r = EINVAL;
    else {
	assert(zombie->close_db==NULL);
	zombie->close_db    = close_db;
	zombie->close_flags = close_flags;
	zombie->was_closed  = 1;
	if (!zombie->db) zombie->db = db;
	if (!toku_brt_zombie_needed(zombie)) {
	    //Close immediately.
	    r = zombie->close_db(zombie->db, zombie->close_flags);
	}
	else {
	    //Try to pass responsibility off.
	    toku_brtheader_lock(zombie->h);
	    toku_list_remove(&zombie->live_brt_link); //Remove from live.
	    BRT replacement = NULL;
	    if (!toku_list_empty(&h->live_brts)) {
		replacement = toku_list_struct(toku_list_head(&h->live_brts), struct brt, live_brt_link);
	    }
	    else if (!toku_list_empty(&h->zombie_brts)) {
		replacement = toku_list_struct(toku_list_head(&h->zombie_brts), struct brt, zombie_brt_link);
	    }
	    toku_list_push(&h->zombie_brts, &zombie->zombie_brt_link); //Add to dead list.
	    toku_brtheader_unlock(zombie->h);
	    if (replacement == NULL) r = 0;  //Just delay close
	    else {
		//Pass responsibility off and close zombie.
		//Skip adding to dead list
		r = toku_txn_note_swap_brt(replacement, zombie);
	    }
	}
    }
    return r;
}

// Close brt.  If opsln_valid, use given oplsn as lsn in brt header instead of logging 
// the close and using the lsn provided by logging the close.  (Subject to constraint 
// that if a newer lsn is already in the dictionary, don't overwrite the dictionary.)
int toku_close_brt_lsn (BRT brt, char **error_string, BOOL oplsn_valid, LSN oplsn) {
    assert(!toku_brt_zombie_needed(brt));
    assert(!brt->pinned_by_checkpoint);
    int r;
    while (!toku_list_empty(&brt->cursors)) {
	BRT_CURSOR c = toku_list_struct(toku_list_pop(&brt->cursors), struct brt_cursor, cursors_link);
	r=toku_brt_cursor_close(c);
	if (r!=0) return r;
    }

    // Must do this work before closing the cf
    r=toku_txn_note_close_brt(brt);
    assert_zero(r);
    toku_omt_destroy(&brt->txns);
    brtheader_note_brt_close(brt);

    if (brt->cf) {
	if (!brt->h->panic)
	    assert(0==toku_cachefile_count_pinned(brt->cf, 1)); // For the brt, the pinned count should be zero (but if panic, don't worry)
	//printf("%s:%d closing cachetable\n", __FILE__, __LINE__);
	// printf("%s:%d brt=%p ,brt->h=%p\n", __FILE__, __LINE__, brt, brt->h);
	if (error_string) assert(*error_string == 0);
	r = toku_cachefile_close(&brt->cf, error_string, oplsn_valid, oplsn);
	if (r==0 && error_string) assert(*error_string == 0);
    }
    toku_free(brt);
    return r;
}

int toku_close_brt (BRT brt, char **error_string) {
    return toku_close_brt_lsn(brt, error_string, FALSE, ZERO_LSN);
}

int toku_brt_create(BRT *brt_ptr) {
    BRT MALLOC(brt);
    if (brt == 0)
	return ENOMEM;
    memset(brt, 0, sizeof *brt);
    toku_list_init(&brt->live_brt_link);
    toku_list_init(&brt->zombie_brt_link);
    toku_list_init(&brt->cursors);
    brt->flags = 0;
    brt->did_set_flags = FALSE;
    brt->nodesize = BRT_DEFAULT_NODE_SIZE;
    brt->compare_fun = toku_builtin_compare_fun;
    brt->update_fun = NULL;
    int r = toku_omt_create(&brt->txns);
    if (r!=0) { toku_free(brt); return r; }
    *brt_ptr = brt;
    return 0;
}


int toku_brt_flush (BRT brt) {
    return toku_cachefile_flush(brt->cf);
}

/* ************* CURSORS ********************* */

static inline void
brt_cursor_cleanup_dbts(BRT_CURSOR c) {
    if (!c->current_in_omt) {
	if (c->key.data) toku_free(c->key.data);
	if (c->val.data) toku_free(c->val.data);
	memset(&c->key, 0, sizeof(c->key));
	memset(&c->val, 0, sizeof(c->val));
    }
}

//
// This function is used by the leafentry iterators.
// returns TOKUDB_ACCEPT if live transaction context is allowed to read a value
// that is written by transaction with LSN of id
// live transaction context may read value if either id is the root ancestor of context, or if
// id was committed before context's snapshot was taken.
// For id to be committed before context's snapshot was taken, the following must be true:
//  - id < context->snapshot_txnid64 AND id is not in context's live root transaction list
// For the above to NOT be true:
//  - id > context->snapshot_txnid64 OR id is in context's live root transaction list
//
static
int does_txn_read_entry(TXNID id, TOKUTXN context) {
    int rval;
    TXNID oldest_live_in_snapshot = toku_get_oldest_in_live_root_txn_list(context);
    if (id < oldest_live_in_snapshot || id == context->ancestor_txnid64) {
	rval = TOKUDB_ACCEPT;
    }
    else if (id > context->snapshot_txnid64 || toku_is_txn_in_live_root_txn_list(context, id)) {
	rval = 0;
    }
    else {
	rval = TOKUDB_ACCEPT;
    }
    return rval;
}

static inline int brt_cursor_extract_key_and_val(
		   LEAFENTRY le,
		   BRT_CURSOR cursor,
		   u_int32_t *keylen,
		   void	    **key,
		   u_int32_t *vallen,
		   void	    **val) {
    int r = 0;
    if (toku_brt_cursor_is_leaf_mode(cursor)) {
	*key = le_key_and_len(le, keylen);
	*val = le;
	*vallen = leafentry_memsize(le);
    } else if (cursor->is_snapshot_read) {
	le_iterate_val(
	    le, 
	    does_txn_read_entry, 
	    val, 
	    vallen, 
	    cursor->ttxn
	    );
	*key = le_key_and_len(le, keylen);
    } else {
	*key = le_key_and_len(le, keylen);
	*val = le_latest_val_and_len(le, vallen);
    }
    return r;
}

static inline void load_dbts_from_omt(BRT_CURSOR c, DBT *key, DBT *val) {
    OMTVALUE le = 0;
    int r = toku_omt_cursor_current(c->omtcursor, &le);
    assert_zero(r);
    r = brt_cursor_extract_key_and_val(le,
				       c,
				       &key->size,
				       &key->data,
				       &val->size,
				       &val->data);
    assert_zero(r);
}

// When an omt cursor is invalidated, this is the brt-level function
// that is called.  This function is only called by the omt logic.
// This callback is called when either (a) the brt logic invalidates one
// cursor (see brt_cursor_invalidate()) or (b) when the omt logic invalidates
// all the cursors for an omt.
static void
brt_cursor_invalidate_callback(OMTCURSOR UU(omt_c), void *extra) {
    BRT_CURSOR cursor = extra;

    //TODO: #1378 assert that this thread owns omt lock in brtcursor

    if (cursor->current_in_omt) {
	DBT key,val;
	load_dbts_from_omt(cursor, toku_init_dbt(&key), toku_init_dbt(&val));
	cursor->key.data = toku_memdup(key.data, key.size);
	cursor->val.data = toku_memdup(val.data, val.size);
	cursor->key.size = key.size;
	cursor->val.size = val.size;
	//TODO: Find some way to deal with ENOMEM here.
	//Until then, just assert that the memdups worked.
	assert(cursor->key.data && cursor->val.data);
	cursor->current_in_omt = FALSE;
    }
}

// Called at start of every slow query, and only from slow queries.
// When all cursors are invalidated (from writer thread, or insert/delete),
// this function is not used.
static void
brt_cursor_invalidate(BRT_CURSOR brtcursor) {
    toku_omt_cursor_invalidate(brtcursor->omtcursor); // will call brt_cursor_invalidate_callback()
}

int toku_brt_cursor (
    BRT brt, 
    BRT_CURSOR *cursorptr, 
    TOKUTXN ttxn, 
    BOOL is_snapshot_read
    ) 
{
    if (is_snapshot_read) {
	invariant(ttxn != NULL);
	int accepted = does_txn_read_entry(brt->h->root_xid_that_created, ttxn);
	if (accepted!=TOKUDB_ACCEPT) {
	    invariant(accepted==0);
	    return TOKUDB_MVCC_DICTIONARY_TOO_NEW;
	}
    }
    BRT_CURSOR cursor = toku_malloc(sizeof *cursor);
    // if this cursor is to do read_committed fetches, then the txn objects must be valid.
    if (cursor == 0)
	return ENOMEM;
    memset(cursor, 0, sizeof(*cursor));
    cursor->brt = brt;
    cursor->current_in_omt = FALSE;
    cursor->prefetching = FALSE;
    cursor->oldest_living_xid = ttxn ? toku_logger_get_oldest_living_xid(ttxn->logger, NULL) : TXNID_NONE;
    cursor->is_snapshot_read = is_snapshot_read;
    cursor->is_leaf_mode = FALSE;
    cursor->ttxn = ttxn;
    toku_list_push(&brt->cursors, &cursor->cursors_link);
    int r = toku_omt_cursor_create(&cursor->omtcursor);
    assert_zero(r);
    toku_omt_cursor_set_invalidate_callback(cursor->omtcursor,
					    brt_cursor_invalidate_callback, cursor);
    cursor->root_put_counter=0;
    *cursorptr = cursor;
    return 0;
}

void
toku_brt_cursor_set_leaf_mode(BRT_CURSOR brtcursor) {
    brtcursor->is_leaf_mode = TRUE;
}

int
toku_brt_cursor_is_leaf_mode(BRT_CURSOR brtcursor) {
    return brtcursor->is_leaf_mode;
}

// Called during cursor destruction
// It is the same as brt_cursor_invalidate, except that
// we make sure the callback function is never called.
static void
brt_cursor_invalidate_no_callback(BRT_CURSOR brtcursor) {
    toku_omt_cursor_set_invalidate_callback(brtcursor->omtcursor, NULL, NULL);
    toku_omt_cursor_invalidate(brtcursor->omtcursor); // will NOT call brt_cursor_invalidate_callback()
}

//TODO: #1378 When we split the ydb lock, touching cursor->cursors_link
//is not thread safe.
int toku_brt_cursor_close(BRT_CURSOR cursor) {
    brt_cursor_invalidate_no_callback(cursor);
    brt_cursor_cleanup_dbts(cursor);
    toku_list_remove(&cursor->cursors_link);
    toku_omt_cursor_destroy(&cursor->omtcursor);
    toku_free_n(cursor, sizeof *cursor);
    return 0;
}

static inline void brt_cursor_set_prefetching(BRT_CURSOR cursor) {
    cursor->prefetching = TRUE;
}

static inline BOOL brt_cursor_prefetching(BRT_CURSOR cursor) {
    return cursor->prefetching;
}

//Return TRUE if cursor is uninitialized.  FALSE otherwise.
static BOOL
brt_cursor_not_set(BRT_CURSOR cursor) {
    assert((cursor->key.data==NULL) == (cursor->val.data==NULL));
    return (BOOL)(!cursor->current_in_omt && cursor->key.data == NULL);
}

static int
pair_leafval_heaviside_le (u_int32_t klen, void *kval,
			   brt_search_t *search) {
    DBT x;
    int cmp = search->compare(search,
			      search->k ? toku_fill_dbt(&x, kval, klen) : 0);
    // The search->compare function returns only 0 or 1
    switch (search->direction) {
    case BRT_SEARCH_LEFT:   return cmp==0 ? -1 : +1;
    case BRT_SEARCH_RIGHT:  return cmp==0 ? +1 : -1; // Because the comparison runs backwards for right searches.
    }
    abort(); return 0;
}


static int
heaviside_from_search_t (OMTVALUE lev, void *extra) {
    LEAFENTRY le=lev;
    brt_search_t *search = extra;
    u_int32_t keylen;
    void* key = le_key_and_len(le, &keylen);

    return pair_leafval_heaviside_le (keylen, key,
				      search);
}


// This is the only function that associates a brt cursor (and its contained
// omt cursor) with a brt node (and its associated omt).  This is different
// from older code because the old code associated the omt cursor with the
// omt when the search found a match.  In this new design, the omt cursor
// will not be associated with the omt until after the application-level
// callback accepts the search result.
// The lock is necessary because we don't want two threads modifying
// the omt's list of cursors simultaneously.
// Note, this is only place in brt code that calls toku_omt_cursor_set_index().
// Requires: cursor->omtcursor is valid
static inline void
brt_cursor_update(BRT_CURSOR brtcursor) {
    //Free old version if it is using local memory.
    OMTCURSOR omtcursor = brtcursor->omtcursor;
    if (!brtcursor->current_in_omt) {
	brt_cursor_cleanup_dbts(brtcursor);
	brtcursor->current_in_omt = TRUE;
	toku_omt_cursor_associate(brtcursor->leaf_info.to_be.omt, omtcursor);
	//no longer touching linked list, and
	//only one thread can touch cursor at a time, protected by ydb lock
    }
    toku_omt_cursor_set_index(omtcursor, brtcursor->leaf_info.to_be.index);
}

//
// Returns true if the value that is to be read is empty.
//
static inline int 
is_le_val_del(LEAFENTRY le, BRT_CURSOR brtcursor) {
    int rval;
    if (brtcursor->is_snapshot_read) {
	BOOL is_del;
	le_iterate_is_del(
	    le, 
	    does_txn_read_entry, 
	    &is_del, 
	    brtcursor->ttxn
	    );
	rval = is_del;
    }
    else {
	rval = le_latest_is_del(le);
    }
    return rval;
}

static BOOL
key_is_in_leaf_range (BRT t, const DBT *key, DBT const * const lower_bound_exclusive, DBT const * const upper_bound_inclusive) {
    return
	((lower_bound_exclusive == NULL) || (t->compare_fun(t->db, lower_bound_exclusive, key) < 0))
	&&
	((upper_bound_inclusive == NULL) || (t->compare_fun(t->db, key,	 upper_bound_inclusive) <= 0));
}

static const DBT zero_dbt = {0,0,0,0};

static void search_save_bound (brt_search_t *search, DBT *pivot) {
    if (search->have_pivot_bound) {
	toku_free(search->pivot_bound.data);
    }
    search->pivot_bound = zero_dbt;
    search->pivot_bound.data = toku_malloc(pivot->size);
    search->pivot_bound.size = pivot->size;
    memcpy(search->pivot_bound.data, pivot->data, pivot->size);
    search->have_pivot_bound = TRUE;
}

static BOOL search_pivot_is_bounded (brt_search_t *search, BRT brt, DBT *pivot)
// Effect:  Return TRUE iff the pivot has already been searched (for fixing #3522.)
//  If searching from left to right, if we have already searched all the values less than pivot, we don't want to search again.
//  If searching from right to left, if we have already searched all the vlaues greater than pivot, we don't want to search again.
{
    if (!search->have_pivot_bound) return TRUE; // isn't bounded.
    int comp = brt->compare_fun(brt->db, pivot, &search->pivot_bound);
    if (search->direction == BRT_SEARCH_LEFT) {
	// searching from left to right.  If the comparison function says the pivot is <= something we already compared, don't do it again.
	return comp>0;
    } else {
	return comp<0;
    }
}

static BOOL msg_type_has_key (enum brt_msg_type m) {
    switch (m) {
    case BRT_NONE:
    case BRT_COMMIT_BROADCAST_ALL:
    case BRT_COMMIT_BROADCAST_TXN:
    case BRT_ABORT_BROADCAST_TXN:
    case BRT_OPTIMIZE:
    case BRT_OPTIMIZE_FOR_UPGRADE:
    case BRT_UPDATE_BROADCAST_ALL:
	return FALSE;
    case BRT_INSERT:
    case BRT_DELETE_ANY:
    case BRT_ABORT_ANY:
    case BRT_COMMIT_ANY:
    case BRT_INSERT_NO_OVERWRITE:
    case BRT_UPDATE:
	return TRUE;
    }
    assert(0);
}

static int
apply_buffer_messages_to_node (
    BRT t, 
    BASEMENTNODE bn, 
    SUBTREE_EST se, 
    BRTNODE ancestor, 
    int childnum, 
    int height,
    MSN min_applied_msn,
    struct pivot_bounds const * const bounds
    )
// Effect: For all the messages in ANCESTOR that are between lower_bound_exclusive (exclusive) and upper_bound_inclusive (inclusive), apply the message node.
//  In ANCESTOR, the relevant messages are all in the buffer for child number CHILDNUM.
//  Treat the bounds as minus or plus infinity respectively if they are NULL.
{
    assert(ancestor->height==height);
    assert(ancestor->height>0);
    assert(0 <= childnum && childnum < ancestor->n_children);
    int r = 0;
    DBT lbe, ubi;
    DBT *lbe_ptr, *ubi_ptr;
    if (bounds->lower_bound_exclusive==NULL) {
	lbe_ptr = NULL;
    } else {
	lbe = kv_pair_key_to_dbt(bounds->lower_bound_exclusive);
	lbe_ptr = &lbe;;
    }
    if (bounds->upper_bound_inclusive==NULL) {
	ubi_ptr = NULL;
    } else {
	ubi = kv_pair_key_to_dbt(bounds->upper_bound_inclusive);
	ubi_ptr = &ubi;
    }
    int made_change;
    assert(BP_STATE(ancestor,childnum) == PT_AVAIL);
    FIFO_ITERATE(BNC_BUFFER(ancestor, childnum), key, keylen, val, vallen, type, msn, xids,
		 ({
		     DBT hk;
		     toku_fill_dbt(&hk, key, keylen);
		     if (msn.msn > min_applied_msn.msn && (!msg_type_has_key(type) || key_is_in_leaf_range(t, &hk, lbe_ptr, ubi_ptr))) {
			 DBT hv;
			 BRT_MSG_S brtcmd = { (enum brt_msg_type)type, msn, xids, .u.id = {&hk,
											   toku_fill_dbt(&hv, val, vallen)} };
			 brt_leaf_put_cmd(t, bn, se, &brtcmd, &made_change);
		     }
		 }));
    return r;
}

static void
maybe_apply_ancestors_messages_to_node (BRT t, BRTNODE node, ANCESTORS ancestors, struct pivot_bounds const * const bounds)
// Effect: Bring a leaf node up-to-date according to all the messages in the ancestors.	  If the leaf node is already up-to-date then do nothing.
//  If NODE is not a leaf node, then don't meodify it.
//  The dirtyness of the node is not changed.
{
    VERIFY_NODE(t, node);
    BOOL update_stats = FALSE;
    if (node->height > 0) { goto exit; }
    // know we are a leaf node
    // need to apply messages to each basement node
    // TODO: (Zardosht) cilkify this, watch out for setting of max_msn_applied_to_node
    for (int i = 0; i < node->n_children; i++) {
	if (BP_STATE(node,i) != PT_AVAIL || BLB_SOFTCOPYISUPTODATE(node, i)) {
	    continue;
	}
	update_stats = TRUE;
	int height = 0;
	BASEMENTNODE curr_bn = (BASEMENTNODE)node->bp[i].ptr;
	SUBTREE_EST curr_se = &BP_SUBTREE_EST(node,i);
	ANCESTORS curr_ancestors = ancestors;
	struct pivot_bounds curr_bounds = next_pivot_keys(node, i, bounds);
	while (curr_ancestors) {
	    height++;
	    apply_buffer_messages_to_node(
		t, 
		curr_bn, 
		curr_se, 
		curr_ancestors->node, 
		curr_ancestors->childnum, 
		height,
		node->max_msn_applied_to_node_on_disk,
		&curr_bounds
		); 
	    if (curr_ancestors->node->max_msn_applied_to_node_in_memory.msn > node->max_msn_applied_to_node_in_memory.msn) {
		node->max_msn_applied_to_node_in_memory = curr_ancestors->node->max_msn_applied_to_node_in_memory;
	    }
	    curr_ancestors= curr_ancestors->next;
	}
	BLB_SOFTCOPYISUPTODATE(node, i) = TRUE;
	
    }
    // Must update the leaf estimates.	Might as well use the estimates from the soft copy (even if they make it out to disk), since they are
    // the best estimates we have.
    if (update_stats) {
	toku_brt_leaf_reset_calc_leaf_stats(node);
	{
	    ANCESTORS curr_ancestors = ancestors;
	    BRTNODE prev_node = node;
	    while (curr_ancestors) {
		BRTNODE next_node = curr_ancestors->node;
		fixup_child_estimates(next_node, curr_ancestors->childnum, prev_node, FALSE);
		prev_node = next_node;
		curr_ancestors = curr_ancestors->next;
	    }
	}
    }
exit:
    VERIFY_NODE(t, node);
}

// This is a bottom layer of the search functions.
static int
brt_search_basement_node(
    BASEMENTNODE bn, 
    brt_search_t *search, 
    BRT_GET_CALLBACK_FUNCTION getf, 
    void *getf_v, 
    BOOL *doprefetch, 
    BRT_CURSOR brtcursor
    )
{
    assert(bn->soft_copy_is_up_to_date);

    // Now we have to convert from brt_search_t to the heaviside function with a direction.  What a pain...

    int direction;
    switch (search->direction) {
    case BRT_SEARCH_LEFT:   direction = +1; goto ok;
    case BRT_SEARCH_RIGHT:  direction = -1; goto ok;
    }
    return EINVAL;  // This return and the goto are a hack to get both compile-time and run-time checking on enum
 ok: ;
    OMTVALUE datav;
    u_int32_t idx = 0;
    int r = toku_omt_find(bn->buffer,
			  heaviside_from_search_t,
			  search,
			  direction,
			  &datav, &idx, NULL);
    if (r!=0) return r;

    LEAFENTRY le = datav;
    if (toku_brt_cursor_is_leaf_mode(brtcursor))
	goto got_a_good_value;	// leaf mode cursors see all leaf entries
    if (is_le_val_del(le,brtcursor)) {
	// Provisionally deleted stuff is gone.
	// So we need to scan in the direction to see if we can find something
	while (1) {
	    switch (search->direction) {
	    case BRT_SEARCH_LEFT:
		idx++;
		if (idx>=toku_omt_size(bn->buffer)) return DB_NOTFOUND;
		break;
	    case BRT_SEARCH_RIGHT:
		if (idx==0) return DB_NOTFOUND;
		idx--;
		break;
	    default:
		assert(FALSE);
	    }
	    r = toku_omt_fetch(bn->buffer, idx, &datav, NULL);
	    assert_zero(r); // we just validated the index
	    le = datav;
	    if (!is_le_val_del(le,brtcursor)) goto got_a_good_value;
	}
    }
got_a_good_value:
    {
	u_int32_t keylen;
	void	 *key;
	u_int32_t vallen;
	void	 *val;

	r = brt_cursor_extract_key_and_val(le,
					   brtcursor,
					   &keylen,
					   &key,
					   &vallen,
					   &val);

	assert(brtcursor->current_in_omt == FALSE);
	if (r==0) {
	    r = getf(keylen, key, vallen, val, getf_v);
	}
	if (r==0) {
	    // Leave the omtcursor alone above (pass NULL to omt_find/fetch)
	    // This prevents the omt from calling associate(), which would
	    // require a lock to keep the list of cursors safe when the omt
	    // is used by the brt.  (We don't want to impose the locking requirement
	    // on the omt for non-brt uses.)
	    //
	    // Instead, all associating of omtcursors with omts (for leaf nodes)
	    // is done in brt_cursor_update.
	    brtcursor->leaf_info.to_be.omt   = bn->buffer;
	    brtcursor->leaf_info.to_be.index = idx;
	    brt_cursor_update(brtcursor);
	    //The search was successful.  Prefetching can continue.
	    *doprefetch = TRUE;
	}
    }
    return r;
}

static int
brt_search_node (
    BRT brt, 
    BRTNODE node, 
    brt_search_t *search, 
    int child_to_search,
    BRT_GET_CALLBACK_FUNCTION getf, 
    void *getf_v, 
    BOOL *doprefetch, 
    BRT_CURSOR brtcursor, 
    UNLOCKERS unlockers, 
    ANCESTORS, 
    struct pivot_bounds const * const bounds
    );

// the number of nodes to prefetch
#define TOKU_DO_PREFETCH 0
#if TOKU_DO_PREFETCH

static void
brt_node_maybe_prefetch(BRT brt, BRTNODE node, int childnum, BRT_CURSOR brtcursor, BOOL *doprefetch) {

    // if we want to prefetch in the tree 
    // then prefetch the next children if there are any
    if (*doprefetch && brt_cursor_prefetching(brtcursor)) {
	int i;
	for (i=0; i<TOKU_DO_PREFETCH; i++) {
	    int nextchildnum = childnum+i+1;
	    if (nextchildnum >= node->n_children) 
		break;
	    BLOCKNUM nextchildblocknum = BP_BLOCKNUM(node, nextchildnum);
	    u_int32_t nextfullhash =  compute_child_fullhash(brt->cf, node, nextchildnum);
	    toku_cachefile_prefetch(
                brt->cf, 
                nextchildblocknum, 
                nextfullhash, 
		toku_brtnode_flush_callback, 
		toku_brtnode_fetch_callback, 
		toku_brtnode_pe_callback,
		toku_brtnode_pf_req_callback,
		toku_brtnode_pf_callback,
		brt->h, 
		brt->h
		);
	    *doprefetch = FALSE;
	}
    }
}

#endif

struct unlock_brtnode_extra {
    BRT brt;
    BRTNODE node;
};
// When this is called, the cachetable lock is held
static void
unlock_brtnode_fun (void *v) {
    struct unlock_brtnode_extra *x = v;
    BRT brt = x->brt;
    BRTNODE node = x->node;
    // CT lock is held
    int r = toku_cachetable_unpin_ct_prelocked(brt->cf, node->thisnodename, node->fullhash, (enum cachetable_dirty) node->dirty, brtnode_memory_size(node));
    assert(r==0);
}

/* search in a node's child */
static int
brt_search_child(BRT brt, BRTNODE node, int childnum, brt_search_t *search, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v, BOOL *doprefetch, BRT_CURSOR brtcursor, UNLOCKERS unlockers,
		 ANCESTORS ancestors, struct pivot_bounds const * const bounds)
// Effect: Search in a node's child.  Searches are read-only now (at least as far as the hardcopy is concerned).
{
    struct ancestors		next_ancestors = {node, childnum, ancestors};

    BLOCKNUM childblocknum = BP_BLOCKNUM(node,childnum);
    u_int32_t fullhash =  compute_child_fullhash(brt->cf, node, childnum);
    BRTNODE childnode;

    struct brtnode_fetch_extra bfe;
    fill_bfe_for_subset_read(
        &bfe, 
        brt->h,
        brt,
        search
        );
    {
	int rr = toku_pin_brtnode(brt, childblocknum, fullhash,
				  unlockers,
				  &next_ancestors, bounds,
				  &bfe,
				  &childnode);
	if (rr==TOKUDB_TRY_AGAIN) return rr;
	assert(rr==0);
    }

    struct unlock_brtnode_extra unlock_extra   = {brt,childnode};
    struct unlockers		next_unlockers = {TRUE, unlock_brtnode_fun, (void*)&unlock_extra, unlockers};

    int r = brt_search_node(brt, childnode, search, bfe.child_to_read, getf, getf_v, doprefetch, brtcursor, &next_unlockers, &next_ancestors, bounds);
    if (r!=TOKUDB_TRY_AGAIN) {
	// Even if r is reactive, we want to handle the maybe reactive child.

#if TOKU_DO_PREFETCH
	// maybe prefetch the next child
	if (r == 0)
	    brt_node_maybe_prefetch(brt, node, childnum, brtcursor, doprefetch);
#endif

	assert(next_unlockers.locked);
	toku_unpin_brtnode(brt, childnode); // unpin the childnode before handling the reactive child (because that may make the childnode disappear.)
    } else {
	// try again.

        // there are two cases where we get TOKUDB_TRY_AGAIN
        //  case 1 is when some later call to toku_pin_brtnode returned
        //  that value and unpinned all the nodes anyway. case 2
        //  is when brt_search_node had to stop its search because
        //  some piece of a node that it needed was not in memory. In this case,
        //  the node was not unpinned, so we unpin it here
	if (next_unlockers.locked) {
            toku_unpin_brtnode(brt, childnode);
	}
    }

    return r;
}

int
toku_brt_search_which_child(
    BRT brt, 
    BRTNODE node, 
    brt_search_t *search
    )
{
    int c;    
    DBT pivotkey;
    toku_init_dbt(&pivotkey);

    /* binary search is overkill for a small array */
    int child[node->n_children];
    
    /* scan left to right or right to left depending on the search direction */
    for (c = 0; c < node->n_children; c++) {
        child[c] = (search->direction == BRT_SEARCH_LEFT) ? c : node->n_children - 1 - c;
    }
    for (c = 0; c < node->n_children-1; c++) {
        int p = (search->direction == BRT_SEARCH_LEFT) ? child[c] : child[c] - 1;
        struct kv_pair *pivot = node->childkeys[p];
        toku_fill_dbt(&pivotkey, kv_pair_key(pivot), kv_pair_keylen(pivot));
        if (search_pivot_is_bounded(search, brt, &pivotkey) && search->compare(search, &pivotkey)) {
            return child[c];
        }
    }
    /* check the first (left) or last (right) node if nothing has been found */
    return child[c];
}

static void
maybe_search_save_bound(
    BRTNODE node,
    int child_searched,
    brt_search_t *search
    ) 
{
    DBT pivotkey;
    toku_init_dbt(&pivotkey);

    int p = (search->direction == BRT_SEARCH_LEFT) ? child_searched : child_searched - 1;
    if (p >=0 && p < node->n_children-1) {
        struct kv_pair *pivot = node->childkeys[p];
        toku_fill_dbt(&pivotkey, kv_pair_key(pivot), kv_pair_keylen(pivot));
        search_save_bound(search, &pivotkey);
    }
}

static int
brt_search_node(
    BRT brt, 
    BRTNODE node, 
    brt_search_t *search,
    int child_to_search,
    BRT_GET_CALLBACK_FUNCTION getf, 
    void *getf_v, 
    BOOL *doprefetch, 
    BRT_CURSOR brtcursor, 
    UNLOCKERS unlockers,
    ANCESTORS ancestors, 
    struct pivot_bounds const * const bounds
    )
{   int r = 0;
    // assert that we got a valid child_to_search
    assert(child_to_search >= 0 || child_to_search < node->n_children);
    //
    // At this point, we must have the necessary partition available to continue the search
    //
    assert(BP_STATE(node,child_to_search) == PT_AVAIL);
    while (child_to_search >= 0 && child_to_search < node->n_children) {
        //
        // Normally, the child we want to use is available, as we checked
        // before entering this while loop. However, if we pass through 
        // the loop once, getting DB_NOTFOUND for this first value
        // of child_to_search, we enter the while loop again with a 
        // child_to_search that may not be in memory. If it is not,
        // we need to return TOKUDB_TRY_AGAIN so the query can
        // read teh appropriate partition into memory
        //
        if (BP_STATE(node,child_to_search) != PT_AVAIL) {
            return TOKUDB_TRY_AGAIN;
        }
        const struct pivot_bounds next_bounds = next_pivot_keys(node, child_to_search, bounds);
        if (node->height > 0) {
            r = brt_search_child(
                brt, 
                node, 
                child_to_search, 
                search, 
                getf, 
                getf_v, 
                doprefetch, 
                brtcursor, 
                unlockers, 
                ancestors, 
                &next_bounds
                );
        }
        else {
            r = brt_search_basement_node(
                (BASEMENTNODE)node->bp[child_to_search].ptr,
                search,
                getf,
                getf_v,
                doprefetch,
                brtcursor
                );
        }
        if (r == 0) return r; //Success
        
        if (r != DB_NOTFOUND) {
            return r; //Error (or message to quit early, such as TOKUDB_FOUND_BUT_REJECTED or TOKUDB_TRY_AGAIN)
        } 
        // we have a new pivotkey
        else {
            // If we got a DB_NOTFOUND then we have to search the next record.        Possibly everything present is not visible.
            // This way of doing DB_NOTFOUND is a kludge, and ought to be simplified.  Something like this is needed for DB_NEXT, but
            //        for point queries, it's overkill.  If we got a DB_NOTFOUND on a point query then we should just stop looking.
            // When releasing locks on I/O we must not search the same subtree again, or we won't be guaranteed to make forward progress.
            // If we got a DB_NOTFOUND, then the pivot is too small if searching from left to right (too large if searching from right to left).
            // So save the pivot key in the search object.
            // printf("%*ssave_bound %s\n", 9-node->height, "", (char*)pivotkey.data);
            maybe_search_save_bound(
                node,
                child_to_search,
                search
                );
        }
        // not really necessary, just put this here so that reading the
        // code becomes simpler. The point is at this point in the code,
        // we know that we got DB_NOTFOUND and we have to continue
        assert(r == DB_NOTFOUND);
        // TODO: (Zardosht), if the necessary partition is not available, we need to return and get the partition
        if (search->direction == BRT_SEARCH_LEFT) {
            child_to_search++;
        }
        else {
            child_to_search--;
        }
    }    
    return r;
}

static int
toku_brt_search (BRT brt, brt_search_t *search, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v, BRT_CURSOR brtcursor, u_int64_t *root_put_counter)
// Effect: Perform a search.  Associate cursor with a leaf if possible.
// All searches are performed through this function.
{
    int r;

 try_again:

    assert(brt->h);

    *root_put_counter = brt->h->root_put_counter;

    u_int32_t fullhash;
    CACHEKEY *rootp = toku_calculate_root_offset_pointer(brt, &fullhash);

    BRTNODE node;

    struct brtnode_fetch_extra bfe;
    fill_bfe_for_subset_read(
        &bfe, 
        brt->h,
        brt,
        search
        );
    toku_pin_brtnode_holding_lock(brt, *rootp, fullhash, NULL, &infinite_bounds, &bfe, &node);

    struct unlock_brtnode_extra unlock_extra   = {brt,node};
    struct unlockers		unlockers      = {TRUE, unlock_brtnode_fun, (void*)&unlock_extra, (UNLOCKERS)NULL};

    {
	BOOL doprefetch = FALSE;
	//static int counter = 0;	 counter++;
	r = brt_search_node(brt, node, search, bfe.child_to_read, getf, getf_v, &doprefetch, brtcursor, &unlockers, (ANCESTORS)NULL, &infinite_bounds);
	if (r==TOKUDB_TRY_AGAIN) {
            // there are two cases where we get TOKUDB_TRY_AGAIN
            //  case 1 is when some later call to toku_pin_brtnode returned
            //  that value and unpinned all the nodes anyway. case 2
            //  is when brt_search_node had to stop its search because
            //  some piece of a node that it needed was not in memory. In this case,
            //  the node was not unpinned, so we unpin it here
            if (unlockers.locked) {
                toku_unpin_brtnode(brt, node);
            }
	    goto try_again;
	} else {
	    assert(unlockers.locked);
	}
    }

    assert(unlockers.locked);
    toku_unpin_brtnode(brt, node);


    //Heaviside function (+direction) queries define only a lower or upper
    //bound.  Some queries require both an upper and lower bound.
    //They do this by wrapping the BRT_GET_CALLBACK_FUNCTION with another
    //test that checks for the other bound.  If the other bound fails,
    //it returns TOKUDB_FOUND_BUT_REJECTED which means not found, but
    //stop searching immediately, as opposed to DB_NOTFOUND
    //which can mean not found, but keep looking in another leaf.
    if (r==TOKUDB_FOUND_BUT_REJECTED) r = DB_NOTFOUND;
    else if (r==DB_NOTFOUND) {
	//We truly did not find an answer to the query.
	//Therefore, the BRT_GET_CALLBACK_FUNCTION has NOT been called.
	//The contract specifies that the callback function must be called
	//for 'r= (0|DB_NOTFOUND|TOKUDB_FOUND_BUT_REJECTED)'
	//TODO: #1378 This is not the ultimate location of this call to the
	//callback.  It is surely wrong for node-level locking, and probably
	//wrong for the STRADDLE callback for heaviside function(two sets of key/vals)
	int r2 = getf(0,NULL, 0,NULL, getf_v);
	if (r2!=0) r = r2;
    }
    return r;
}

struct brt_cursor_search_struct {
    BRT_GET_CALLBACK_FUNCTION getf;
    void *getf_v;
    BRT_CURSOR cursor;
    brt_search_t *search;
};

/* search for the first kv pair that matches the search object */
static int
brt_cursor_search(BRT_CURSOR cursor, brt_search_t *search, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    brt_cursor_invalidate(cursor);
    int r = toku_brt_search(cursor->brt, search, getf, getf_v, cursor, &cursor->root_put_counter);
    return r;
}

static inline int compare_k_x(BRT brt, const DBT *k, const DBT *x) {
    return brt->compare_fun(brt->db, k, x);
}

static int
brt_cursor_compare_one(brt_search_t *search __attribute__((__unused__)), DBT *x __attribute__((__unused__)))
{
    return 1;
}

static int brt_cursor_compare_set(brt_search_t *search, DBT *x) {
    BRT brt = search->context;
    return compare_k_x(brt, search->k, x) <= 0; /* return min xy: kv <= xy */
}

static int
brt_cursor_current_getf(ITEMLEN keylen,		 bytevec key,
			ITEMLEN vallen,		 bytevec val,
			void *v) {
    struct brt_cursor_search_struct *bcss = v;
    int r;
    if (key==NULL) {
	r = bcss->getf(0, NULL, 0, NULL, bcss->getf_v);
    } else {
	BRT_CURSOR cursor = bcss->cursor;
	DBT newkey = {.size=keylen, .data=(void*)key}; // initializes other fields to zero
	//Safe to access cursor->key/val because current_in_omt is FALSE
	if (compare_k_x(cursor->brt, &cursor->key, &newkey) != 0) {
	    r = bcss->getf(0, NULL, 0, NULL, bcss->getf_v); // This was once DB_KEYEMPTY
	    if (r==0) r = TOKUDB_FOUND_BUT_REJECTED;
	}
	else
	    r = bcss->getf(keylen, key, vallen, val, bcss->getf_v);
    }
    return r;
}

int
toku_brt_cursor_current(BRT_CURSOR cursor, int op, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    if (brt_cursor_not_set(cursor))
	return EINVAL;
    if (op == DB_CURRENT) {
	brt_cursor_invalidate(cursor);
	struct brt_cursor_search_struct bcss = {getf, getf_v, cursor, 0};
	brt_search_t search; brt_search_init(&search, brt_cursor_compare_set, BRT_SEARCH_LEFT, &cursor->key, cursor->brt);
	int r = toku_brt_search(cursor->brt, &search, brt_cursor_current_getf, &bcss, cursor, &cursor->root_put_counter);
	brt_search_finish(&search);
	return r;
    }
    brt_cursor_invalidate(cursor);
    return getf(cursor->key.size, cursor->key.data, cursor->val.size, cursor->val.data, getf_v); // brt_cursor_copyout(cursor, outkey, outval);
}

static int
brt_flatten_getf(ITEMLEN UU(keylen),	  bytevec UU(key),
		 ITEMLEN UU(vallen),	  bytevec UU(val),
		 void *UU(v)) {
    return DB_NOTFOUND;
}

int
toku_brt_flatten(BRT brt, TOKUTXN ttxn)
{
    BRT_CURSOR tmp_cursor;
    int r = toku_brt_cursor(brt, &tmp_cursor, ttxn, FALSE);
    if (r!=0) return r;
    brt_search_t search; brt_search_init(&search, brt_cursor_compare_one, BRT_SEARCH_LEFT, 0, tmp_cursor->brt);
    r = brt_cursor_search(tmp_cursor, &search, brt_flatten_getf, NULL);
    brt_search_finish(&search);
    if (r==DB_NOTFOUND) r = 0;
    {
	//Cleanup temporary cursor
	int r2 = toku_brt_cursor_close(tmp_cursor);
	if (r==0) r = r2;
    }
    return r;
}

int
toku_brt_cursor_first(BRT_CURSOR cursor, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    brt_search_t search; brt_search_init(&search, brt_cursor_compare_one, BRT_SEARCH_LEFT, 0, cursor->brt);
    int r = brt_cursor_search(cursor, &search, getf, getf_v);
    brt_search_finish(&search);
    return r;
}

int
toku_brt_cursor_last(BRT_CURSOR cursor, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    brt_search_t search; brt_search_init(&search, brt_cursor_compare_one, BRT_SEARCH_RIGHT, 0, cursor->brt);
    int r = brt_cursor_search(cursor, &search, getf, getf_v);
    brt_search_finish(&search);;
    return r;
}

static int brt_cursor_compare_next(brt_search_t *search, DBT *x) {
    BRT brt = search->context;
    return compare_k_x(brt, search->k, x) < 0; /* return min xy: kv < xy */
}

static int
brt_cursor_shortcut (BRT_CURSOR cursor, int direction, u_int32_t limit, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v) {
    int r;
    OMTCURSOR omtcursor	    = cursor->omtcursor;
    OMT	      omt	    = toku_omt_cursor_get_omt(omtcursor);
    u_int64_t h_put_counter = cursor->brt->h->root_put_counter;
    u_int64_t c_put_counter = cursor->root_put_counter;
    BOOL found = FALSE;

    //Verify that no messages have been inserted
    //since the last time the cursor's pointer was set.
    //Also verify the omt cursor is still valid.
    //(Necessary to recheck after the maybe_get_and_pin)
    if (c_put_counter==h_put_counter && toku_omt_cursor_is_valid(cursor->omtcursor)) {
	u_int32_t index = 0;
	r = toku_omt_cursor_current_index(omtcursor, &index);
	assert_zero(r);

	//Starting with the prev, find the first real (non-provdel) leafentry.
	while (index != limit) {
	    OMTVALUE le = NULL;
	    index += direction;
	    r = toku_omt_fetch(omt, index, &le, NULL);
	    assert_zero(r);

	    if (toku_brt_cursor_is_leaf_mode(cursor) || !is_le_val_del(le, cursor)) {
		u_int32_t keylen;
		void	 *key;
		u_int32_t vallen;
		void	 *val;

		r = brt_cursor_extract_key_and_val(le,
						   cursor,
						   &keylen,
						   &key,
						   &vallen,
						   &val);

		if (r==0) {
		    r = getf(keylen, key, vallen, val, getf_v);
		}
		if (r==0) {
		    //Update cursor.
		    cursor->leaf_info.to_be.index = index;
		    brt_cursor_update(cursor);
		    found = TRUE;
		}
		break;
	    }
	}
	if (r==0 && !found) r = DB_NOTFOUND;
    }
    else r = EINVAL;

    return r;
}

static int
brt_cursor_next_shortcut (BRT_CURSOR cursor, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
// Effect: If possible, increment the cursor and return the key-value pair
//  (i.e., the next one from what the cursor pointed to before.)
// That is, do DB_NEXT on DUP databases, and do DB_NEXT_NODUP on NODUP databases.
{
    int r;
    if (toku_omt_cursor_is_valid(cursor->omtcursor)) {
	u_int32_t limit = toku_omt_size(toku_omt_cursor_get_omt(cursor->omtcursor)) - 1;
	r = brt_cursor_shortcut(cursor, 1, limit, getf, getf_v);
    }
    else r = EINVAL;
    return r;
}

int
toku_brt_cursor_next(BRT_CURSOR cursor, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    int r;
    if (brt_cursor_next_shortcut(cursor, getf, getf_v)==0) {
	r = 0;
    } else {
	brt_search_t search; brt_search_init(&search, brt_cursor_compare_next, BRT_SEARCH_LEFT, &cursor->key, cursor->brt);
	r = brt_cursor_search(cursor, &search, getf, getf_v);
	brt_search_finish(&search);
    }
    if (r == 0) brt_cursor_set_prefetching(cursor);
    return r;
}

static int
brt_cursor_search_eq_k_x_getf(ITEMLEN keylen,	       bytevec key,
			      ITEMLEN vallen,	       bytevec val,
			      void *v) {
    struct brt_cursor_search_struct *bcss = v;
    int r;
    if (key==NULL) {
	r = bcss->getf(0, NULL, 0, NULL, bcss->getf_v);
    } else {
	BRT_CURSOR cursor = bcss->cursor;
	DBT newkey = {.size=keylen, .data=(void*)key}; // initializes other fields to zero
	if (compare_k_x(cursor->brt, bcss->search->k, &newkey) == 0) {
	    r = bcss->getf(keylen, key, vallen, val, bcss->getf_v);
	} else {
	    r = bcss->getf(0, NULL, 0, NULL, bcss->getf_v);
	    if (r==0) r = TOKUDB_FOUND_BUT_REJECTED;
	}
    }
    return r;
}

/* search for the kv pair that matches the search object and is equal to k */
static int
brt_cursor_search_eq_k_x(BRT_CURSOR cursor, brt_search_t *search, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    brt_cursor_invalidate(cursor);
    struct brt_cursor_search_struct bcss = {getf, getf_v, cursor, search};
    int r = toku_brt_search(cursor->brt, search, brt_cursor_search_eq_k_x_getf, &bcss, cursor, &cursor->root_put_counter);
    return r;
}

static int
brt_cursor_prev_shortcut (BRT_CURSOR cursor, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
// Effect: If possible, decrement the cursor and return the key-value pair
//  (i.e., the previous one from what the cursor pointed to before.)
// That is, do DB_PREV on DUP databases, and do DB_PREV_NODUP on NODUP databases.
{
    int r;
    if (toku_omt_cursor_is_valid(cursor->omtcursor)) {
	r = brt_cursor_shortcut(cursor, -1, 0, getf, getf_v);
    }
    else r = EINVAL;
    return r;
}



static int brt_cursor_compare_prev(brt_search_t *search, DBT *x) {
    BRT brt = search->context;
    return compare_k_x(brt, search->k, x) > 0; /* return max xy: kv > xy */
}

int
toku_brt_cursor_prev(BRT_CURSOR cursor, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    if (brt_cursor_prev_shortcut(cursor, getf, getf_v)==0)
	return 0;
    brt_search_t search; brt_search_init(&search, brt_cursor_compare_prev, BRT_SEARCH_RIGHT, &cursor->key, cursor->brt);
    int r = brt_cursor_search(cursor, &search, getf, getf_v);
    brt_search_finish(&search);
    return r;
}

static int brt_cursor_compare_set_range(brt_search_t *search, DBT *x) {
    BRT brt = search->context;
    return compare_k_x(brt, search->k,	x) <= 0; /* return kv <= xy */
}

int
toku_brt_cursor_set(BRT_CURSOR cursor, DBT *key, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    brt_search_t search; brt_search_init(&search, brt_cursor_compare_set_range, BRT_SEARCH_LEFT, key, cursor->brt);
    int r = brt_cursor_search_eq_k_x(cursor, &search, getf, getf_v);
    brt_search_finish(&search);
    return r;
}

int
toku_brt_cursor_set_range(BRT_CURSOR cursor, DBT *key, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    brt_search_t search; brt_search_init(&search, brt_cursor_compare_set_range, BRT_SEARCH_LEFT, key, cursor->brt);
    int r = brt_cursor_search(cursor, &search, getf, getf_v);
    brt_search_finish(&search);
    return r;
}

static int brt_cursor_compare_set_range_reverse(brt_search_t *search, DBT *x) {
    BRT brt = search->context;
    return compare_k_x(brt, search->k, x) >= 0; /* return kv >= xy */
}

int
toku_brt_cursor_set_range_reverse(BRT_CURSOR cursor, DBT *key, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    brt_search_t search; brt_search_init(&search, brt_cursor_compare_set_range_reverse, BRT_SEARCH_RIGHT, key, cursor->brt);
    int r = brt_cursor_search(cursor, &search, getf, getf_v);
    brt_search_finish(&search);
    return r;
}


//TODO: When tests have been rewritten, get rid of this function.
//Only used by tests.
int
toku_brt_cursor_get (BRT_CURSOR cursor, DBT *key, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v, int get_flags)
{
    int op = get_flags & DB_OPFLAGS_MASK;
    if (get_flags & ~DB_OPFLAGS_MASK)
	return EINVAL;

    switch (op) {
    case DB_CURRENT:
    case DB_CURRENT_BINDING:
	return toku_brt_cursor_current(cursor, op, getf, getf_v);
    case DB_FIRST:
	return toku_brt_cursor_first(cursor, getf, getf_v);
    case DB_LAST:
	return toku_brt_cursor_last(cursor, getf, getf_v);
    case DB_NEXT:
    case DB_NEXT_NODUP:
	if (brt_cursor_not_set(cursor))
	    return toku_brt_cursor_first(cursor, getf, getf_v);
	else
	    return toku_brt_cursor_next(cursor, getf, getf_v);
    case DB_PREV:
    case DB_PREV_NODUP:
	if (brt_cursor_not_set(cursor))
	    return toku_brt_cursor_last(cursor, getf, getf_v);
	else
	    return toku_brt_cursor_prev(cursor, getf, getf_v);
    case DB_SET:
	return toku_brt_cursor_set(cursor, key, getf, getf_v);
    case DB_SET_RANGE:
	return toku_brt_cursor_set_range(cursor, key, getf, getf_v);
    default: ;// Fall through
    }
    return EINVAL;
}

void
toku_brt_cursor_peek(BRT_CURSOR cursor, const DBT **pkey, const DBT **pval)
// Effect: Retrieves a pointer to the DBTs for the current key and value.
// Requires:  The caller may not modify the DBTs or the memory at which they points.
// Requires:  The caller must be in the context of a
// BRT_GET_(STRADDLE_)CALLBACK_FUNCTION
{
    if (cursor->current_in_omt) load_dbts_from_omt(cursor, &cursor->key, &cursor->val);
    *pkey = &cursor->key;
    *pval = &cursor->val;
}

//We pass in toku_dbt_fake to the search functions, since it will not pass the
//key(or val) to the heaviside function if key(or val) is NULL.
//It is not used for anything else,
//the actual 'extra' information for the heaviside function is inside the
//wrapper.
static const DBT __toku_dbt_fake;
static const DBT* const toku_dbt_fake = &__toku_dbt_fake;

BOOL toku_brt_cursor_uninitialized(BRT_CURSOR c) {
    return brt_cursor_not_set(c);
}

int toku_brt_get_cursor_count (BRT brt) {
    int n = 0;
    struct toku_list *list;
    for (list = brt->cursors.next; list != &brt->cursors; list = list->next)
	n += 1;
    return n;
}

// TODO: Get rid of this
int toku_brt_dbt_set(DBT* key, DBT* key_source) {
    int r = toku_dbt_set(key_source->size, key_source->data, key, NULL);
    return r;
}

/* ********************************* lookup **************************************/

int
toku_brt_lookup (BRT brt, DBT *k, BRT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    int r, rr;
    BRT_CURSOR cursor;

    rr = toku_brt_cursor(brt, &cursor, NULL, FALSE);
    if (rr != 0) return rr;

    int op = DB_SET;
    r = toku_brt_cursor_get(cursor, k, getf, getf_v, op);

    rr = toku_brt_cursor_close(cursor); assert_zero(rr);

    return r;
}

/* ********************************* delete **************************************/
static int
getf_nothing (ITEMLEN UU(keylen), bytevec UU(key), ITEMLEN UU(vallen), bytevec UU(val), void *UU(pair_v)) {
    return 0;
}

int
toku_brt_cursor_delete(BRT_CURSOR cursor, int flags, TOKUTXN txn) {
    int r;

    int unchecked_flags = flags;
    BOOL error_if_missing = (BOOL) !(flags&DB_DELETE_ANY);
    unchecked_flags &= ~DB_DELETE_ANY;
    if (unchecked_flags!=0) r = EINVAL;
    else if (brt_cursor_not_set(cursor)) r = EINVAL;
    else {
	r = 0;
	if (error_if_missing) {
	    r = toku_brt_cursor_current(cursor, DB_CURRENT, getf_nothing, NULL);
	}
	if (r == 0) {
	    //We need to have access to the (key,val) that the cursor points to.
	    //By invalidating the cursor we guarantee we have a local copy.
	    //
	    //If we try to use the omtcursor, there exists a race condition
	    //(node could be evicted), but maybe_get_and_pin() prevents delete.
	    brt_cursor_invalidate(cursor);
	    r = toku_brt_delete(cursor->brt, &cursor->key, txn);
	}
    }
    return r;
}

/* ********************* keyrange ************************ */


static void toku_brt_keyrange_internal (BRT brt, CACHEKEY nodename, 
					u_int32_t fullhash, DBT *key, u_int64_t *less,	u_int64_t *equal,  u_int64_t *greater) {
    BRTNODE node;
    {
	void *node_v;
	//assert(fullhash == toku_cachetable_hash(brt->cf, nodename));
        struct brtnode_fetch_extra bfe;
        // TODO: (Zardosht) change this
        fill_bfe_for_full_read(&bfe, brt->h);
	int rr = toku_cachetable_get_and_pin(
	    brt->cf, 
	    nodename, 
	    fullhash,
            &node_v, 
            NULL, 
            toku_brtnode_flush_callback, 
            toku_brtnode_fetch_callback, 
            toku_brtnode_pe_callback, 
            toku_brtnode_pf_req_callback,
            toku_brtnode_pf_callback,
            &bfe, 
            brt->h
            );
	assert_zero(rr);
	node = node_v;
	assert(node->fullhash==fullhash);
    }
    int n_keys = node->n_children-1;
    int compares[n_keys];
    int i;
    for (i=0; i<n_keys; i++) {
	struct kv_pair *pivot = node->childkeys[i];
	DBT dbt;
	compares[i] = brt->compare_fun(brt->db, toku_fill_dbt(&dbt, kv_pair_key(pivot), kv_pair_keylen(pivot)), key);
    }
    for (i=0; i<node->n_children; i++) {
	int prevcomp = (i==0) ? -1 : compares[i-1];
	int nextcomp = (i+1 >= n_keys) ? 1 : compares[i];
	u_int64_t subest = BP_SUBTREE_EST(node,i).ndata;
	if (nextcomp < 0) {
	    // We're definitely looking too far to the left
	    *less += subest;
	} else if (prevcomp > 0) {
	    // We're definitely looking too far to the right
	    *greater += subest;
	} else if (prevcomp == 0 && nextcomp == 0) {
	    // We're looking at a subtree that contains all zeros
	    *equal   += subest;
	} else {
	    // nextcomp>=0 and prevcomp<=0, so something in the subtree could match
	    // but they are not both zero, so it's not the whole subtree, so we need to recurse
	    if (node->height > 0) {
		toku_brt_keyrange_internal(brt, BP_BLOCKNUM(node, i), compute_child_fullhash(brt->cf, node, i), key, less, equal, greater);
	    }
	    else {
		struct cmd_leafval_heaviside_extra be = {brt, key};
		u_int32_t idx;
		int r = toku_omt_find_zero(BLB_BUFFER(node, i), toku_cmd_leafval_heaviside, &be, 0, &idx, NULL);
		*less += idx;
		*greater += toku_omt_size(BLB_BUFFER(node, i))-idx;
		if (r==0) {
		    (*greater)--;
		    (*equal)++;
		}
	    }
	}
    }
    toku_unpin_brtnode(brt, node);
}

int toku_brt_keyrange (BRT brt, DBT *key, u_int64_t *less,  u_int64_t *equal,  u_int64_t *greater) {
    assert(brt->h);
    u_int32_t fullhash;
    CACHEKEY *rootp = toku_calculate_root_offset_pointer(brt, &fullhash);

    *less = *equal = *greater = 0;
    toku_brt_keyrange_internal (brt, *rootp, fullhash, key, less, equal, greater);
    return 0;
}

int toku_brt_stat64 (BRT brt, TOKUTXN UU(txn), struct brtstat64_s *s) {
    {
	int64_t file_size;
	int fd = toku_cachefile_get_and_pin_fd(brt->cf);
	int r = toku_os_get_file_size(fd, &file_size);
	toku_cachefile_unpin_fd(brt->cf);
	assert_zero(r);
	s->fsize = file_size + toku_cachefile_size_in_memory(brt->cf);
    }

    assert(brt->h);
    u_int32_t fullhash;
    CACHEKEY *rootp = toku_calculate_root_offset_pointer(brt, &fullhash);
    CACHEKEY root = *rootp;
    void *node_v;
    struct brtnode_fetch_extra bfe;
    fill_bfe_for_min_read(&bfe, brt->h);
    int r = toku_cachetable_get_and_pin(
        brt->cf, 
        root, 
        fullhash,
        &node_v, 
        NULL,
        toku_brtnode_flush_callback, 
        toku_brtnode_fetch_callback, 
        toku_brtnode_pe_callback, 
        toku_brtnode_pf_req_callback,
        toku_brtnode_pf_callback,
        &bfe, 
        brt->h
        );
    if (r!=0) return r;
    BRTNODE node = node_v;

    s->nkeys = s->ndata = s->dsize = 0;
    int i;
    for (i=0; i<node->n_children; i++) {
        SUBTREE_EST se = &BP_SUBTREE_EST(node,i);
	s->nkeys += se->nkeys;
	s->ndata += se->ndata;
	s->dsize += se->dsize;
    }
    
    r = toku_cachetable_unpin(brt->cf, root, fullhash, CACHETABLE_CLEAN, 0);
    if (r!=0) return r;
    return 0;
}

/* ********************* debugging dump ************************ */
static int
toku_dump_brtnode (FILE *file, BRT brt, BLOCKNUM blocknum, int depth, struct kv_pair *lorange, struct kv_pair *hirange) {
    int result=0;
    BRTNODE node;
    void *node_v;
    u_int32_t fullhash = toku_cachetable_hash(brt->cf, blocknum);
    struct brtnode_fetch_extra bfe;
    fill_bfe_for_full_read(&bfe, brt->h);
    int r = toku_cachetable_get_and_pin(
        brt->cf, 
        blocknum, 
        fullhash,
        &node_v, 
        NULL,
	toku_brtnode_flush_callback, 
	toku_brtnode_fetch_callback, 
	toku_brtnode_pe_callback, 
        toku_brtnode_pf_req_callback,
        toku_brtnode_pf_callback,
	&bfe, 
	brt->h
	);
    assert_zero(r);
    node=node_v;
    assert(node->fullhash==fullhash);
    result=toku_verify_brtnode(brt, ZERO_MSN, ZERO_MSN, blocknum, -1, lorange, hirange, NULL, NULL, 0, 1, 0);
    fprintf(file, "%*sNode=%p\n", depth, "", node);
    
    fprintf(file, "%*sNode %"PRId64" nodesize=%u height=%d n_children=%d  keyrange=%s %s\n",
	depth, "", blocknum.b, node->nodesize, node->height, node->n_children, (char*)(lorange ? kv_pair_key(lorange) : 0), (char*)(hirange ? kv_pair_key(hirange) : 0));
    {
	int i;
	for (i=0; i+1< node->n_children; i++) {
	    fprintf(file, "%*spivotkey %d =", depth+1, "", i);
	    toku_print_BYTESTRING(file, toku_brt_pivot_key_len(node->childkeys[i]), node->childkeys[i]->key);
	    fprintf(file, "\n");
	}
	for (i=0; i< node->n_children; i++) {
	    {
		SUBTREE_EST e = &BP_SUBTREE_EST(node,i);
		fprintf(file, " est={n=%" PRIu64 " k=%" PRIu64 " s=%" PRIu64 " e=%d}",
			e->ndata, e->nkeys, e->dsize, (int)e->exact);
	    }
	    fprintf(file, "\n");
	    if (node->height > 0) {
		fprintf(file, "%*schild %d buffered (%d entries):", depth+1, "", i, toku_fifo_n_entries(BNC_BUFFER(node,i)));
		FIFO_ITERATE(BNC_BUFFER(node,i), key, keylen, data, datalen, type, msn, xids,
				  {
				      data=data; datalen=datalen; keylen=keylen;
				      fprintf(file, "%*s xid=%"PRIu64" %u (type=%d) msn=%"PRIu64"\n", depth+2, "", xids_get_innermost_xid(xids), (unsigned)toku_dtoh32(*(int*)key), type, msn.msn);
				      //assert(strlen((char*)key)+1==keylen);
				      //assert(strlen((char*)data)+1==datalen);
				  });
	    }
	    else {
		int size = toku_omt_size(BLB_BUFFER(node, i));
		if (0)
		for (int j=0; j<size; j++) {
		    OMTVALUE v = 0;
		    r = toku_omt_fetch(BLB_BUFFER(node, i), j, &v, 0);
		    assert_zero(r);
		    fprintf(file, " [%d]=", j);
		    print_leafentry(file, v);
		    fprintf(file, "\n");
		}
		//	       printf(" (%d)%u ", len, *(int*)le_key(data)));
		fprintf(file, "\n");
	    }
	}
	if (node->height > 0) {
	    for (i=0; i<node->n_children; i++) {
		fprintf(file, "%*schild %d\n", depth, "", i);
		if (i>0) {
		    char *key = node->childkeys[i-1]->key;
		    fprintf(file, "%*spivot %d len=%u %u\n", depth+1, "", i-1, node->childkeys[i-1]->keylen, (unsigned)toku_dtoh32(*(int*)key));
		}
		toku_dump_brtnode(file, brt, BP_BLOCKNUM(node, i), depth+4,
				  (i==0) ? lorange : node->childkeys[i-1],
				  (i==node->n_children-1) ? hirange : node->childkeys[i]);
	    }
	}
    }
    r = toku_cachetable_unpin(brt->cf, blocknum, fullhash, CACHETABLE_CLEAN, 0);
    assert_zero(r);
    return result;
}

int toku_dump_brt (FILE *f, BRT brt) {
    CACHEKEY *rootp = NULL;
    assert(brt->h);
    u_int32_t fullhash = 0;
    toku_dump_translation_table(f, brt->h->blocktable);
    rootp = toku_calculate_root_offset_pointer(brt, &fullhash);
    return toku_dump_brtnode(f, brt, *rootp, 0, 0, 0);
}

int toku_brt_truncate (BRT brt) {
    int r;

    // flush the cached tree blocks and remove all related pairs from the cachetable
    r = toku_brt_flush(brt);

    // TODO log the truncate?

    int fd = toku_cachefile_get_and_pin_fd(brt->cf);
    toku_brtheader_lock(brt->h);
    if (r==0) {
	//Free all data blocknums and associated disk space (if not held on to by checkpoint)
	toku_block_translation_truncate_unlocked(brt->h->blocktable, fd, brt->h);
	//Assign blocknum for root block, also dirty the header
	toku_allocate_blocknum_unlocked(brt->h->blocktable, &brt->h->root, brt->h);
	// reinit the header
	r = brt_init_header_partial(brt, NULL);
    }

    toku_brtheader_unlock(brt->h);
    toku_cachefile_unpin_fd(brt->cf);

    return r;
}

static int
toku_brt_lock_init(void) {
    int r = 0;
    if (r==0)
	r = toku_pwrite_lock_init();
    return r;
}

static int
toku_brt_lock_destroy(void) {
    int r = 0;
    if (r==0) 
	r = toku_pwrite_lock_destroy();
    return r;
}

int toku_brt_init(void (*ydb_lock_callback)(void),
		  void (*ydb_unlock_callback)(void),
		  void (*db_set_brt)(DB*,BRT)) {
    int r = 0;
    //Portability must be initialized first
    if (r==0) 
	r = toku_portability_init();
    if (r==0) 
	r = toku_brt_lock_init();
    if (r==0) 
	r = toku_checkpoint_init(ydb_lock_callback, ydb_unlock_callback);
    if (r == 0)
	r = toku_brt_serialize_init();
    if (r==0)
	callback_db_set_brt = db_set_brt;
    return r;
}

int toku_brt_destroy(void) {
    int r = 0;
    if (r == 0)
	r = toku_brt_serialize_destroy();
    if (r==0) 
	r = toku_brt_lock_destroy();
    if (r==0)
	r = toku_checkpoint_destroy();
    //Portability must be cleaned up last
    if (r==0) 
	r = toku_portability_destroy();
    return r;
}


// Require that dictionary specified by brt is fully written to disk before
// transaction txn is committed.
void
toku_brt_require_local_checkpoint (BRT brt, TOKUTXN txn) {
    toku_brtheader_lock(brt->h);
    toku_list_push(&txn->checkpoint_before_commit,
		   &brt->h->checkpoint_before_commit_link);
    toku_brtheader_unlock(brt->h);
}


//Suppress both rollback and recovery logs.
void
toku_brt_suppress_recovery_logs (BRT brt, TOKUTXN txn) {
    assert(brt->h->txnid_that_created_or_locked_when_empty == toku_txn_get_txnid(txn));
    assert(brt->h->txnid_that_suppressed_recovery_logs	   == TXNID_NONE);
    brt->h->txnid_that_suppressed_recovery_logs		   = toku_txn_get_txnid(txn);
    toku_list_push(&txn->checkpoint_before_commit, &brt->h->checkpoint_before_commit_link);
}

BOOL
toku_brt_is_recovery_logging_suppressed (BRT brt) {
    return brt->h->txnid_that_suppressed_recovery_logs != TXNID_NONE;
}

LSN toku_brt_checkpoint_lsn(BRT brt) {
    return brt->h->checkpoint_lsn;
}

int toku_brt_header_set_panic(struct brt_header *h, int panic, char *panic_string) {
    if (h->panic == 0) {
	h->panic = panic;
	if (h->panic_string) 
	    toku_free(h->panic_string);
	h->panic_string = toku_strdup(panic_string);
    }
    return 0;
}

int toku_brt_set_panic(BRT brt, int panic, char *panic_string) {
    return toku_brt_header_set_panic(brt->h, panic, panic_string);
}

#if 0

int toku_logger_save_rollback_fdelete (TOKUTXN txn, u_int8_t file_was_open, FILENUM filenum, BYTESTRING iname)

int toku_logger_log_fdelete (TOKUTXN txn, const char *fname, FILENUM filenum, u_int8_t was_open)
#endif

// Prepare to remove a dictionary from the database when this transaction is committed:
//  - if cachetable has file open, mark it as in use so that cf remains valid until we're done
//  - mark transaction as NEED fsync on commit
//  - make entry in rollback log
//  - make fdelete entry in recovery log
int toku_brt_remove_on_commit(TOKUTXN txn, DBT* iname_in_env_dbt_p) {
    assert(txn);
    int r;
    const char *iname_in_env = iname_in_env_dbt_p->data;
    CACHEFILE cf = NULL;
    u_int8_t was_open = 0;
    FILENUM filenum   = {0};

    r = toku_cachefile_of_iname_in_env(txn->logger->ct, iname_in_env, &cf);
    if (r == 0) {
	was_open = TRUE;
	filenum = toku_cachefile_filenum(cf);
	struct brt_header *h = toku_cachefile_get_userdata(cf);
	BRT brt;
	//Any arbitrary brt of that header is fine.
	toku_brtheader_lock(h);
	if (!toku_list_empty(&h->live_brts)) {
	    brt = toku_list_struct(toku_list_head(&h->live_brts), struct brt, live_brt_link);
	}
	else {
	    //Header exists, so at least one brt must.	No live means at least one zombie.
	    assert(!toku_list_empty(&h->zombie_brts));
	    brt = toku_list_struct(toku_list_head(&h->zombie_brts), struct brt, zombie_brt_link);
	}
	toku_brtheader_unlock(h);
	r = toku_txn_note_brt(txn, brt);
	if (r!=0) return r;
    }
    else 
	assert(r==ENOENT);

    toku_txn_force_fsync_on_commit(txn);  //If the txn commits, the commit MUST be in the log
				     //before the file is actually unlinked
    {
	BYTESTRING iname_in_env_bs = { .len=strlen(iname_in_env), .data = (char*)iname_in_env };
	// make entry in rollback log
	r = toku_logger_save_rollback_fdelete(txn, was_open, filenum, &iname_in_env_bs);
	assert_zero(r); //On error we would need to remove the CF reference, which is complicated.
    }
    if (r==0)
	// make entry in recovery log
	r = toku_logger_log_fdelete(txn, iname_in_env);
    return r;
}


// Non-transaction version of fdelete
int toku_brt_remove_now(CACHETABLE ct, DBT* iname_in_env_dbt_p) {
    int r;
    const char *iname_in_env = iname_in_env_dbt_p->data;
    CACHEFILE cf;
    r = toku_cachefile_of_iname_in_env(ct, iname_in_env, &cf);
    if (r == 0) {
	r = toku_cachefile_redirect_nullfd(cf);
	assert_zero(r);
    }
    else
	assert(r==ENOENT);
    char *iname_in_cwd = toku_cachetable_get_fname_in_cwd(ct, iname_in_env_dbt_p->data);
    
    r = unlink(iname_in_cwd);  // we need a pathname relative to cwd
    assert_zero(r);
    toku_free(iname_in_cwd);
    return r;
}

int
toku_brt_get_fragmentation(BRT brt, TOKU_DB_FRAGMENTATION report) {
    int r;

    int fd = toku_cachefile_get_and_pin_fd(brt->cf);
    toku_brtheader_lock(brt->h);

    int64_t file_size;
    if (toku_cachefile_is_dev_null_unlocked(brt->cf))
	r = EINVAL;
    else
	r = toku_os_get_file_size(fd, &file_size);
    if (r==0) {
	report->file_size_bytes = file_size;
	toku_block_table_get_fragmentation_unlocked(brt->h->blocktable, report);
    }
    toku_brtheader_unlock(brt->h);
    toku_cachefile_unpin_fd(brt->cf);
    return r;
}

static BOOL is_empty_fast_iter (BRT brt, BRTNODE node) {
    if (node->height > 0) {
	for (int childnum=0; childnum<node->n_children; childnum++) {
            if (BNC_NBYTESINBUF(node, childnum) != 0) {
                return 0; // it's not empty if there are bytes in buffers
            }
	    BRTNODE childnode;
	    {
		void *node_v;
		BLOCKNUM childblocknum = BP_BLOCKNUM(node,childnum);
		u_int32_t fullhash =  compute_child_fullhash(brt->cf, node, childnum);
                struct brtnode_fetch_extra bfe;
                fill_bfe_for_full_read(&bfe, brt->h);
		int rr = toku_cachetable_get_and_pin(
                    brt->cf, 
                    childblocknum, 
                    fullhash, 
                    &node_v, 
                    NULL, 
                    toku_brtnode_flush_callback, 
                    toku_brtnode_fetch_callback, 
                    toku_brtnode_pe_callback, 
                    toku_brtnode_pf_req_callback,
                    toku_brtnode_pf_callback,
                    &bfe, 
                    brt->h
                    );
		assert(rr ==0);
		childnode = node_v;
	    }
	    int child_is_empty = is_empty_fast_iter(brt, childnode);
	    toku_unpin_brtnode(brt, childnode);
	    if (!child_is_empty) return 0;
	}
	return 1;
    } else {
	// leaf:  If the omt is empty, we are happy.
	for (int i = 0; i < node->n_children; i++) {
	    if (toku_omt_size(BLB_BUFFER(node, i))) {
		return FALSE;
	    }
	}
	return TRUE;
    }
}

BOOL toku_brt_is_empty_fast (BRT brt)
// A fast check to see if the tree is empty.  If there are any messages or leafentries, we consider the tree to be nonempty.  It's possible that those
// messages and leafentries would all optimize away and that the tree is empty, but we'll say it is nonempty.
{
    u_int32_t fullhash;
    CACHEKEY *rootp = toku_calculate_root_offset_pointer(brt, &fullhash);
    BRTNODE node;
    //assert(fullhash == toku_cachetable_hash(brt->cf, *rootp));
    {
	void *node_v;
        struct brtnode_fetch_extra bfe;
        fill_bfe_for_full_read(&bfe, brt->h);
	int rr = toku_cachetable_get_and_pin(
            brt->cf, 
            *rootp, 
            fullhash,
            &node_v, 
            NULL, 
            toku_brtnode_flush_callback, 
            toku_brtnode_fetch_callback, 
            toku_brtnode_pe_callback, 
            toku_brtnode_pf_req_callback,
            toku_brtnode_pf_callback,
            &bfe,
            brt->h
            );
	assert_zero(rr);
	node = node_v;
    }
    BOOL r = is_empty_fast_iter(brt, node);
    toku_unpin_brtnode(brt, node);
    return r;
}

int toku_brt_strerror_r(int error, char *buf, size_t buflen)
{
    if (error>=0) {
	return strerror_r(error, buf, buflen);
    } else {
	switch (error) {
	case DB_KEYEXIST:
	    snprintf(buf, buflen, "Key exists");
	    return 0;
	case TOKUDB_CANCELED:
	    snprintf(buf, buflen, "User canceled operation");
	    return 0;
	default:
	    snprintf(buf, buflen, "Unknown error %d", error);
	    errno = EINVAL;
	    return -1;
	}
    }
}


void 
toku_reset_root_xid_that_created(BRT brt, TXNID new_root_xid_that_created) {
    // Reset the root_xid_that_created field to the given value.  
    // This redefines which xid created the dictionary.

    struct brt_header *h = brt->h;    

    // hold lock around setting and clearing of dirty bit
    // (see cooperative use of dirty bit in toku_brtheader_begin_checkpoint())
    toku_brtheader_lock (h);
    h->root_xid_that_created = new_root_xid_that_created;
    h->dirty = 1;
    toku_brtheader_unlock (h);
}
