/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/* $Id: rbt.c,v 1.61 1999/10/13 22:50:39 marka Exp $ */

/* Principal Authors: DCL */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <isc/assertions.h>
#include <isc/boolean.h>
#include <isc/error.h>
#include <isc/mem.h>
#include <isc/result.h>

#include <dns/rbt.h>
#include <dns/result.h>
#include <dns/fixedname.h>

#define RBT_MAGIC		0x5242542BU /* RBT+. */
#define VALID_RBT(rbt)		((rbt) != NULL && (rbt)->magic == RBT_MAGIC)

#define CHAIN_MAGIC		0x302d302dU /* 0-0-. */
#define VALID_CHAIN(chain)	((chain) != NULL && \
				 (chain)->magic == CHAIN_MAGIC)

struct dns_rbt {
	unsigned int		magic;
	isc_mem_t *		mctx;
	dns_rbtnode_t *		root;
	void			(*data_deleter)(void *, void *);
	void *			deleter_arg;
};

#define RED 0
#define BLACK 1

/*
 * Elements of the rbtnode structure.
 */
#define LEFT(node)	 	((node)->left)
#define RIGHT(node)		((node)->right)
#define DOWN(node)		((node)->down)
#define DATA(node)		((node)->data)
#define FINDCALLBACK(node)	((node)->find_callback)
#define COLOR(node) 		((node)->color)
#define NAMELEN(node)		((node)->namelen)
#define OFFSETLEN(node)		((node)->offsetlen)
#define ATTRS(node)		((node)->attributes)
#define PADBYTES(node)		((node)->padbytes)

/*
 * Structure elements from the rbtdb.c, not
 * used as part of the rbt.c algorithms.
 */
#define DIRTY(node)	((node)->dirty)
#define WILD(node)	((node)->wild)
#define LOCK(node)	((node)->locknum)
#define REFS(node)	((node)->references)

/*
 * The variable length stuff stored after the node.
 */
#define NAME(node)	((unsigned char *)((node) + 1))
#define OFFSETS(node)	(NAME(node) + NAMELEN(node))

#define NODE_SIZE(node)	(sizeof(*node) + \
			 NAMELEN(node) + OFFSETLEN(node) + PADBYTES(node))

/*
 * Color management.
 */
#define IS_RED(node)		((node) != NULL && (node)->color == RED)
#define IS_BLACK(node)		((node) == NULL || (node)->color == BLACK)
#define MAKE_RED(node)		((node)->color = RED)
#define MAKE_BLACK(node)	((node)->color = BLACK)

/*
 * Chain management.
 */
#define ADD_ANCESTOR(chain, node) \
do { \
	if ((chain)->ancestor_count == (chain)->ancestor_maxitems &&	\
	    get_ancestor_mem(chain) != DNS_R_SUCCESS) {		\
		dns_rbtnodechain_invalidate(chain);		\
		return (DNS_R_NOMEMORY);			\
	}							\
	(chain)->ancestors[(chain)->ancestor_count++] = (node);	\
} while (0)

#define ADD_LEVEL(chain, node) \
			(chain)->levels[(chain)->level_count++] = (node)

/*
 * The following macros directly access normally private name variables.
 * These macros are used to avoid a lot of function calls in the critical
 * path of the tree traversal code.
 */

#define NODENAME(node, name) \
do { \
	(name)->length = NAMELEN(node); \
	(name)->labels = OFFSETLEN(node); \
	(name)->ndata = NAME(node); \
	(name)->offsets = OFFSETS(node); \
	(name)->attributes = ATTRS(node); \
	(name)->attributes |= DNS_NAMEATTR_READONLY; \
} while (0)

#define FAST_ISABSOLUTE(name) \
	(((name)->attributes & DNS_NAMEATTR_ABSOLUTE) ? ISC_TRUE : ISC_FALSE)

#define FAST_COUNTLABELS(name) \
	((name)->labels)

#ifdef DEBUG
#define inline
/*
 * A little something to help out in GDB.
 */
dns_name_t Name(dns_rbtnode_t *node);
dns_name_t 
Name(dns_rbtnode_t *node) {
	dns_name_t name;

	dns_name_init(&name, NULL);
	if (node != NULL)
		NODENAME(node, &name);

	return (name);
}
#endif

#if defined(ISC_MEM_DEBUG) || defined(RBT_MEM_TEST)
#undef DNS_RBT_ANCESTORBLOCK
#define DNS_RBT_ANCESTORBLOCK 1	/* To give the reallocation code a workout. */
#endif

/*
 * Forward declarations.
 */
static dns_result_t create_node(isc_mem_t *mctx,
				dns_name_t *name, dns_rbtnode_t **nodep);

static dns_result_t join_nodes(dns_rbt_t *rbt,
			       dns_rbtnode_t *node, dns_rbtnode_t *parent,
			       dns_rbtnode_t **rootp);

static inline dns_result_t get_ancestor_mem(dns_rbtnodechain_t *chain);
static inline void put_ancestor_mem(dns_rbtnodechain_t *chain);

static inline void rotate_left(dns_rbtnode_t *node, dns_rbtnode_t *parent,
			       dns_rbtnode_t **rootp);
static inline void rotate_right(dns_rbtnode_t *node, dns_rbtnode_t *parent,
				dns_rbtnode_t **rootp);

static void dns_rbt_addonlevel(dns_rbtnode_t *node,
				       dns_rbtnode_t *current, int order,
				       dns_rbtnode_t **rootp,
				       dns_rbtnodechain_t *chain);
static void dns_rbt_deletefromlevel(dns_rbt_t *rbt,
				    dns_rbtnode_t *delete,
				    dns_rbtnodechain_t *chain);
static void dns_rbt_deletetree(dns_rbt_t *rbt, dns_rbtnode_t *node);

static dns_result_t zapnode_and_fixlevels(dns_rbt_t *rbt,
					  dns_rbtnode_t *node,
					  isc_boolean_t recurse,
					  dns_rbtnodechain_t *chain);

/*
 * Initialize a red/black tree of trees.
 */
dns_result_t
dns_rbt_create(isc_mem_t *mctx, void (*deleter)(void *, void *),
	       void *deleter_arg, dns_rbt_t **rbtp)
{
	dns_rbt_t *rbt;

	REQUIRE(mctx != NULL);
	REQUIRE(rbtp != NULL && *rbtp == NULL);
	REQUIRE(deleter == NULL ? deleter_arg == NULL : 1);

	rbt = (dns_rbt_t *)isc_mem_get(mctx, sizeof(*rbt));
	if (rbt == NULL)
		return (DNS_R_NOMEMORY);

	rbt->mctx = mctx;
	rbt->data_deleter = deleter;
	rbt->deleter_arg = deleter_arg;
	rbt->root = NULL;
	rbt->magic = RBT_MAGIC;

	*rbtp = rbt;

	return (DNS_R_SUCCESS);
}

/*
 * Deallocate a red/black tree of trees.
 */
void
dns_rbt_destroy(dns_rbt_t **rbtp) {
	dns_rbt_t *rbt;

	REQUIRE(rbtp != NULL && VALID_RBT(*rbtp));

	rbt = *rbtp;

	dns_rbt_deletetree(rbt, rbt->root);

	rbt->magic = 0;

	isc_mem_put(rbt->mctx, rbt, sizeof(*rbt));

	*rbtp = NULL;
}

/*
 * The next three functions for chains, get_ancestor_mem, put_ancestor_mem
 * and chain_name, appear early in this file so they can be effectively
 * inlined by the other rbt functions that use them.
 */

static inline dns_result_t
get_ancestor_mem(dns_rbtnodechain_t *chain) {
	dns_rbtnode_t **ancestor_mem;
	int oldsize, newsize;

	oldsize = chain->ancestor_maxitems * sizeof(dns_rbtnode_t *);
	newsize = oldsize + DNS_RBT_ANCESTORBLOCK * sizeof(dns_rbtnode_t *);

	if (oldsize == 0) {
		chain->ancestors = chain->ancestor_block;
	} else {
		ancestor_mem = isc_mem_get(chain->mctx, newsize);

		if (ancestor_mem == NULL)
			return (DNS_R_NOMEMORY);

		memcpy(ancestor_mem, chain->ancestors, oldsize);

		if (chain->ancestor_maxitems > DNS_RBT_ANCESTORBLOCK)
			isc_mem_put(chain->mctx, chain->ancestors, oldsize);

		chain->ancestors = ancestor_mem;
	}

	chain->ancestor_maxitems += DNS_RBT_ANCESTORBLOCK;

	return (DNS_R_SUCCESS);
}

/*
 * This is used by functions that are popping the chain off their
 * own stack, and so do not need to have ancestor_maxitems or the
 * ancestors pointer reset.  Functions that will be reusing a chain
 * structure need to call dns_rbtnodechain_reset() instead.
 */
static inline void
put_ancestor_mem(dns_rbtnodechain_t *chain) {
	if (chain->ancestor_maxitems > DNS_RBT_ANCESTORBLOCK)
		isc_mem_put(chain->mctx, chain->ancestors,
			    chain->ancestor_maxitems
			    * sizeof(dns_rbtnode_t *));
}

static inline dns_result_t
chain_name(dns_rbtnodechain_t *chain, dns_name_t *name,
	   isc_boolean_t include_chain_end)
{
	dns_name_t nodename;
	dns_result_t result;
	unsigned int i;

	dns_name_init(&nodename, NULL);

	/*
	 * XXX DCL Is this too devilish, initializing name like this?
	 */
	result = dns_name_concatenate(NULL, NULL, name, NULL);
	if (result != DNS_R_SUCCESS)
		return result;

	for (i = 0; i < chain->level_count; i++) {
		NODENAME(chain->levels[i], &nodename);
		result = dns_name_concatenate(&nodename, name, name, NULL);

		if (result != DNS_R_SUCCESS)
			break;
	}

	if (result == DNS_R_SUCCESS &&
	    include_chain_end && chain->end != NULL) {
		NODENAME(chain->end, &nodename);
		result = dns_name_concatenate(&nodename, name, name, NULL);
	}

	return (result);
}

static inline dns_result_t
move_chain_to_last(dns_rbtnodechain_t *chain, dns_rbtnode_t *node) {
	while (1) {
		/*
		 * Go as far right and then down as much as possible,
		 * as long as the rightmost node has a down pointer.
		 */
		while (RIGHT(node) != NULL) {
			ADD_ANCESTOR(chain, node);
			node = RIGHT(node);
		}

		if (DOWN(node) == NULL)
			break;

		ADD_ANCESTOR(chain, NULL);
		ADD_LEVEL(chain, node);
		node = DOWN(node);
	}

	chain->end = node;

	return (DNS_R_SUCCESS);
}

/*
 * Add 'name' to tree, initializing its data pointer with 'data'.
 */

dns_result_t
dns_rbt_addnode(dns_rbt_t *rbt, dns_name_t *name, dns_rbtnode_t **nodep) {
	/*
	 * Does this thing have too many variables or what?
	 */
	dns_rbtnode_t **root, *parent, *child, *current, *new_current;
	dns_name_t *add_name, current_name, *prefix, *suffix;
	dns_fixedname_t fixedcopy, fixedprefix, fixedsuffix;
	dns_offsets_t current_offsets;
	dns_namereln_t compared;
	dns_result_t result = DNS_R_SUCCESS;
	dns_rbtnodechain_t chain;
	unsigned int common_labels, common_bits, add_bits;
	int order;

	REQUIRE(VALID_RBT(rbt));
	REQUIRE(FAST_ISABSOLUTE(name));
	REQUIRE(nodep != NULL && *nodep == NULL);

	/*
	 * Create a copy of the name so the original name structure is
	 * not modified.  The name data needs to be modifiable when
	 * a node is split on a bitstring label.
	 */
	dns_fixedname_init(&fixedcopy);
	add_name = dns_fixedname_name(&fixedcopy);
	dns_name_clone(name, add_name);

	if (rbt->root == NULL) {
		result = create_node(rbt->mctx, add_name, &new_current);
		if (result == DNS_R_SUCCESS) {
			rbt->root = new_current;
			*nodep = new_current;
		}
		return (result);
	}

	dns_rbtnodechain_init(&chain, rbt->mctx);
	ADD_ANCESTOR(&chain, NULL);

	dns_fixedname_init(&fixedprefix);
	dns_fixedname_init(&fixedsuffix);
	prefix = dns_fixedname_name(&fixedprefix);
	suffix = dns_fixedname_name(&fixedsuffix);

	root = &rbt->root;
	parent = NULL;
	current = NULL;
	child = *root;
	dns_name_init(&current_name, current_offsets);
	do {
		current = child;

		NODENAME(current, &current_name);
		compared = dns_name_fullcompare(add_name, &current_name,
						&order,
						&common_labels, &common_bits);

		if (compared == dns_namereln_equal) {
			*nodep = current;
			result = DNS_R_EXISTS;
			break;

		}

		if (compared == dns_namereln_none) {
			if (order < 0) {
				parent = current;
				child = LEFT(current);
				ADD_ANCESTOR(&chain, current);

			} else if (order > 0) {
				parent = current;
				child = RIGHT(current);
				ADD_ANCESTOR(&chain, current);

			}

		} else {
			/*
			 * This name has some suffix in common with the
			 * name at the current node.  If the name at
			 * the current node is shorter, that means the
			 * new name should be in a subtree.  If the
			 * name at the current node is longer, that means
			 * the down pointer to this tree should point
			 * to a new tree that has the common suffix, and
			 * the non-common parts of these two names should
			 * start a new tree.
			 */

			if (compared == dns_namereln_subdomain) {
				/*
				 * All of the exising labels are in common,
				 * so the new name is in a subtree.
				 * Whack off the common labels for the 
				 * not-in-common part to be searched for
				 * in the next level.
				 */
				result = dns_name_split(add_name,
							common_labels,
							common_bits,
							add_name, NULL);

				if (result != DNS_R_SUCCESS)
					break;
				
				/*
				 * Follow the down pointer (possibly NULL).
				 */
				root = &DOWN(current);
				parent = NULL;
				child = DOWN(current);
				ADD_ANCESTOR(&chain, NULL);
				ADD_LEVEL(&chain, current);

			} else {
				/*
				 * The number of labels in common is fewer
				 * than the number of labels at the current
				 * node, so the current node must be adjusted
				 * to have just the common suffix, and a down
				 * pointer made to a new tree.
				 */

				INSIST(compared == dns_namereln_commonancestor
				       || compared == dns_namereln_contains);

				/*
				 * Ensure the number of levels in the tree
				 * does not exceed the number of logical
				 * levels allowed by DNSSEC.
				 *
				 * XXX DCL need a better error result?
				 */
				if (chain.level_count ==
				    (sizeof(chain.levels) / 
				     sizeof(*chain.levels))) {
					result = DNS_R_NOSPACE;
					break;
				}

				/*
				 * Split the name into two parts, a prefix
				 * which is the not-in-common parts of the
				 * two names and a suffix that is the common
				 * parts of them.
				 */
				result = dns_name_split(&current_name,
							common_labels,
							common_bits,
							prefix, suffix);

				if (result == DNS_R_SUCCESS)
					result = create_node(rbt->mctx, suffix,
							     &new_current);

				if (result != DNS_R_SUCCESS)
					break;

				/* 
				 * Reproduce the tree attributes of the
				 * current node.
				 */
				LEFT(new_current) = LEFT(current);
				RIGHT(new_current) = RIGHT(current);
				COLOR(new_current) = COLOR(current);

				/*
				 * Fix pointers that were to the current node.
				 */
				if (parent != NULL) {
					if (LEFT(parent) == current)
						LEFT(parent) = new_current;
					else
						RIGHT(parent) = new_current;
				}
				if (*root == current)
					*root = new_current;

				/*
				 * Now make the new root of the subtree
				 * as the not-in-common labels of the current
				 * node, keeping the same memory location so
				 * as not to break any external references to
				 * the node.  The down pointer and name data
				 * are preserved, while left and right
				 * pointers are nullified when the node is
				 * established as the start of the next level.
				 *
				 * The name stored at the node is effectively
				 * truncated in place by setting the shorter
				 * name length, moving the offsets to the
				 * end of the truncated name, and then
				 * updating PADBYTES to reflect the truncation.
				 *
				 * When bitstring labels are involved, things
				 * are just a tad more complicated (aren't
				 * they always?) because the splitting
				 * has shifted the bits that this name needs,
				 * as well as adjusted the bit count.
				 * So there are convolutions to deal with it.
				 * There are compromises here between
				 * abstraction and efficiency.
				 */

				if (common_bits > 0) {
					dns_label_t label;

					dns_name_getlabel(prefix,
					      FAST_COUNTLABELS(&current_name) -
							  common_labels,
							  &label);

					INSIST(dns_label_type(&label) ==
					       dns_labeltype_bitstring);

					memcpy(NAME(current) +
					       (label.base - prefix->ndata),
					       label.base,
					       label.length);

					dns_name_getlabel(add_name,
						   FAST_COUNTLABELS(add_name) -
							  common_labels,
							  &label);
					INSIST(dns_label_type(&label) ==
					       dns_labeltype_bitstring);

					add_bits = dns_label_countbits(&label);
				} else
					add_bits = 0;

				NAMELEN(current) = prefix->length;
				OFFSETLEN(current) = prefix->labels;
				memcpy(OFFSETS(current), prefix->offsets,
				       prefix->labels);
				PADBYTES(current) +=
				       (current_name.length - prefix->length) +
				       (current_name.labels - prefix->labels);

				/*
				 * Set up the new root of the next level.
				 * By definition it will not be the top
				 * level tree, so clear DNS_NAMEATTR_ABSOLUTE.
				 */
				DOWN(new_current) = current;
				root = &DOWN(new_current);
				ADD_ANCESTOR(&chain, NULL);
				ADD_LEVEL(&chain, new_current);

				LEFT(current) = NULL;
				RIGHT(current) = NULL;

				MAKE_BLACK(current);
				ATTRS(current) &= ~DNS_NAMEATTR_ABSOLUTE;

				if (common_labels ==
				    FAST_COUNTLABELS(add_name) &&
				    common_bits == add_bits) {
					/*
					 * The name has been added by pushing
					 * the not-in-common parts down to
					 * a new level.
					 */
					*nodep = new_current;
					put_ancestor_mem(&chain);
					return (DNS_R_SUCCESS);

				} else {
					/*
					 * The current node has no data,
					 * because it is just a placeholder.
					 * Its data pointer is already NULL
					 * from create_node()), so there's
					 * nothing more to do to but add it to
					 * the ancestor chain, because it will
					 * be the parent node of the new node.
					 */
					ADD_ANCESTOR(&chain, current);

					/* The not-in-common parts of the new
					 * name will be inserted into the new
					 * level following this loop (unless
					 * result != DNS_R_SUCCESS, which
					 * is tested after the loop ends).
					 */
					result = dns_name_split(add_name, 
								common_labels,
								common_bits,
								add_name,
								NULL);

				
					break;
				}

			}

		}

	} while (child != NULL);

	if (result == DNS_R_SUCCESS)
		result = create_node(rbt->mctx, add_name, &new_current);

	if (result == DNS_R_SUCCESS) {
		dns_rbt_addonlevel(new_current, current, order, root, &chain);
		*nodep = new_current;
	}

	put_ancestor_mem(&chain);

	return (result);
}

/*
 * Add a name to the tree of trees, associating it with some data.
 */
dns_result_t
dns_rbt_addname(dns_rbt_t *rbt, dns_name_t *name, void *data) {
	dns_result_t result;
	dns_rbtnode_t *node;

	REQUIRE(VALID_RBT(rbt));
	REQUIRE(FAST_ISABSOLUTE(name));

	node = NULL;

	result = dns_rbt_addnode(rbt, name, &node);

	/*
	 * dns_rbt_addnode will report the node exists even when
	 * it does not have data associated with it, but the
	 * dns_rbt_*name functions all behave depending on whether
	 * there is data associated with a node.
	 */
	if (result == DNS_R_SUCCESS ||
	    (result == DNS_R_EXISTS && DATA(node) == NULL)) {
		DATA(node) = data;
		result = DNS_R_SUCCESS;
	}

	return (result);
}

/*
 * Find the node for "name" in the tree of trees.
 */
dns_result_t
dns_rbt_findnode(dns_rbt_t *rbt, dns_name_t *name, dns_name_t *foundname,
		 dns_rbtnode_t **node, dns_rbtnodechain_t *chain,
		 isc_boolean_t empty_data_ok, dns_rbtfindcallback_t callback,
		 void *callback_arg)
{
	dns_rbtnode_t *current;
	dns_rbtnodechain_t localchain;
	dns_name_t *search_name, current_name, *callback_name;
	dns_fixedname_t fixedcallbackname, fixedsearchname;
	dns_namereln_t compared;
	dns_result_t result, saved_result;
	unsigned int common_labels, common_bits;
	int order;

	REQUIRE(VALID_RBT(rbt));
	REQUIRE(FAST_ISABSOLUTE(name));
	REQUIRE(node != NULL && *node == NULL);

	/* 
	 * If there is a chain it needs to appear to be in a sane state,
	 * otherwise a chain is still needed to generate foundname and
	 * callback_name.
	 */
	if (chain == NULL) {
		chain = &localchain;
		dns_rbtnodechain_init(chain, rbt->mctx);
	} else
		dns_rbtnodechain_reset(chain);

	dns_fixedname_init(&fixedcallbackname);
	callback_name = dns_fixedname_name(&fixedcallbackname);

	/*
	 * search_name is the name segment being sought in each tree level.
	 * By using a fixedname, the search_name will definitely have offsets
	 * and a buffer for use by any splitting that happens in the middle
	 * of a bitstring label.  By using dns_name_clone, no name data is
	 * copied unless a bitstring split occurs.
	 */
	dns_fixedname_init(&fixedsearchname);
	search_name = dns_fixedname_name(&fixedsearchname);
	dns_name_clone(name, search_name);

	dns_name_init(&current_name, NULL);

	ADD_ANCESTOR(chain, NULL);

	saved_result = DNS_R_SUCCESS;
	current = rbt->root;
	while (current != NULL) {
		NODENAME(current, &current_name);
		compared = dns_name_fullcompare(search_name, &current_name,
                                                &order,
                                                &common_labels, &common_bits);

		if (compared == dns_namereln_equal)
			break;

                if (compared == dns_namereln_none) {
			ADD_ANCESTOR(chain, current);

			/*
			 * Standard binary search tree movement.
			 */
			if (order < 0)
				current = LEFT(current);
			else
				current = RIGHT(current);

		} else {
			/*
			 * The names have some common suffix labels.
			 *
			 * If the number in common are equal in length to
			 * the current node's name length, then follow the
			 * down pointer and search in the new tree.
			 */
			if (compared == dns_namereln_subdomain) {
				/*
				 * Whack off the current node's common parts
				 * for the name to search in the next level.
				 */
				result = dns_name_split(search_name,
							common_labels,
							common_bits,
							search_name, NULL);
				if (result != DNS_R_SUCCESS) {
					dns_rbtnodechain_reset(chain);
					return (result);
				}

				/*
				 * This might be the closest enclosing name.
				 */
				if (empty_data_ok || DATA(current) != NULL)
					*node = current;

				/*
				 * Point the chain to the next level.   This
				 * needs to be done before 'current' is pointed
				 * there because the callback in the next
				 * block of code needs the current 'current',
				 * but in the event the callback requests that
				 * the search be stopped then the
				 * DNS_R_PARTIALMATCH code at the end of this
				 * function needs the chain pointed to the
				 * next level.
				 */
				ADD_ANCESTOR(chain, NULL);
				ADD_LEVEL(chain, current);

				/*
				 * The caller may want to interrupt the
				 * downward search when certain special nodes
				 * are traversed.  If this is a special node,
				 * the callback is used to learn what the
				 * caller wants to do.
				 */
				if (callback != NULL &&
				    FINDCALLBACK(current)) {
					result = chain_name(chain,
							    callback_name,
							    ISC_FALSE);
					if (result != DNS_R_SUCCESS) {
						dns_rbtnodechain_reset(chain);
						return (result);
					}

					result = (callback)(current,
							    callback_name,
							    callback_arg);
					if (result != DNS_R_CONTINUE) {
						saved_result = result;
						/*
						 * Treat this node as if it
						 * had no down pointer.
						 */
						current = NULL;
						break;
					}
				}

				/*
				 * Finally, head to the next tree level.
				 */
				current = DOWN(current);

			} else {
				/*
				 * Though there are labels in common, the
				 * entire name at this node is not common
				 * with the search name so the search
				 * name does not exist in the tree.
				 * Add this node to the ancestor chain
				 * to simplify things for the chain fixing
				 * logic below then end the loop.
				 */
				INSIST(compared == dns_namereln_commonancestor
				       || compared == dns_namereln_contains);

				ADD_ANCESTOR(chain, current);
				current = NULL;
			}
		}
	}

	if (current != NULL) {
		/*
		 * Found an exact match.
		 */
		chain->end = current;
		chain->level_matches = chain->level_count - 1;

		if (foundname != NULL)
			result = chain_name(chain, foundname, ISC_TRUE);
		else
			result = DNS_R_SUCCESS;

		if (result == DNS_R_SUCCESS) {
			*node = current;
			result = saved_result;
		} else
			*node = NULL;

	} else {
		/*
		 * Did not find an exact match.
		 */
		if (*node != NULL) {
			/*
			 * ... but found a partially matching superdomain.
			 * Unwind the chain to the partial match node
			 * to set level_matches to the level above the node,
			 * and then to derive the name.
			 */
			chain->level_matches = chain->level_count - 1;

			while (chain->levels[chain->level_matches] != *node) {
				INSIST(chain->level_matches > 0);
				chain->level_matches--;
			}


			if (foundname != NULL) {
				unsigned int saved_count = chain->level_count;

				chain->level_count = chain->level_matches + 1;

				result = chain_name(chain, foundname,
						    ISC_FALSE);

				chain->level_count = saved_count;
			} else
				result = DNS_R_SUCCESS;

			if (result == DNS_R_SUCCESS)
				result = DNS_R_PARTIALMATCH;

		} else {
			chain->level_matches = -2;
			result = DNS_R_NOTFOUND;
		}

		if (chain != &localchain) {
			/*
			 * Since there was no exact match, the chain argument
			 * needs to be pointed at the DNSSEC predecessor of
			 * the search name.
			 *
			 * First, point current to the node that stopped the
			 * search, and remove that node from the ancestor
			 * history.
			 */

			INSIST(chain->ancestor_count > 0);
			current = chain->ancestors[--chain->ancestor_count];

			if (current == NULL) {
				/*
				 * Attempted to follow a down pointer that was
				 * NULL, which means the searched for name was
				 * a subdomain of a terminal name in the tree.
				 * Since there are no existing subdomains to
				 * order against, the terminal name is the
				 * predecessor.
				 */
				INSIST(chain->level_count > 0);
				INSIST(chain->level_matches <
				       chain->level_count);
				chain->end =
					chain->levels[--chain->level_count];
			} else {
				dns_result_t result2;

				/*
				 * Reached a point within a level tree that
				 * positively indicates the name is not
				 * present, but the stop node could be either
				 * less than the desired name (order > 0) or
				 * greater than the desired name (order < 0).
				 *
				 * If the stop node is less, it is not
				 * necessarily the predecessor.  If the stop
				 * node has a down pointer, then the real
				 * predecessor is at the end of a level below
				 * (not necessarily the next level).
				 * Move down levels until the rightmost node
				 * does not have a down pointer.
				 *
				 * When the stop node is greater, it is
				 * the successor.  All the logic for finding
				 * the predecessor is handily encapsulated
				 * in dns_rbtnodechain_prev.  In the event
				 * that the search name is less than anything
				 * else in the tree, the chain is reset.
				 * XXX DCL What is the best way for the caller
				 *         to know that the search name has
				 *         no predecessor?
				 */

				if (order > 0) {
					if (DOWN(current) != NULL) {
						ADD_ANCESTOR(chain, NULL);
						ADD_LEVEL(chain, current);

						result2 =
						      move_chain_to_last(chain,
								DOWN(current));

						if (result2 != DNS_R_SUCCESS)
							result = result2;
					} else
						/*
						 * Ah, the pure and simple
						 * case.  The stop node is the
						 * predecessor.
						 */
						chain->end = current;

				} else {
					INSIST(order < 0);

					chain->end = current;

					result2 = dns_rbtnodechain_prev(chain,
									NULL,
									NULL);
					if (result2 == DNS_R_SUCCESS ||
					    result2 == DNS_R_NEWORIGIN)
						; 	/* Nothing */
					else if (result2 == DNS_R_NOMORE) {
						/*
						 * There is no predecessor.
						 */
						dns_rbtnodechain_reset(chain);
						chain->level_count = -1;
						chain->level_matches = -2;

					} else
						result = result2;
				}

			}
		}
	}

	if (chain == &localchain)
		put_ancestor_mem(chain);

	return (result);
}

/*
 * Get the data pointer associated with 'name'.
 */
dns_result_t
dns_rbt_findname(dns_rbt_t *rbt, dns_name_t *name,
		 dns_name_t *foundname, void **data) {
	dns_rbtnode_t *node = NULL;
	dns_result_t result;

	REQUIRE(data != NULL && *data == NULL);

	result = dns_rbt_findnode(rbt, name, foundname, &node, NULL,
				  ISC_FALSE, NULL, NULL);

	if (node != NULL && DATA(node) != NULL)
		*data = DATA(node);
	else
		result = DNS_R_NOTFOUND;
		
	return (result);
}

/*
 * Delete a name from the tree of trees.
 */
dns_result_t
dns_rbt_deletename(dns_rbt_t *rbt, dns_name_t *name, isc_boolean_t recurse) {
	dns_rbtnode_t *node = NULL;
	dns_result_t result;
	dns_rbtnodechain_t chain;

	REQUIRE(VALID_RBT(rbt));
	REQUIRE(FAST_ISABSOLUTE(name));

	/*
	 * Find the node, building the ancestor chain.
	 *
	 * When searching, the name might not have an exact match:
	 * consider a.b.a.com, b.b.a.com and c.b.a.com as the only
	 * elements of a tree, which would make layer 1 a single
	 * node tree of "b.a.com" and layer 2 a three node tree of
	 * a, b, and c.  Deleting a.com would find only a partial depth
	 * match in the first layer.  Should it be a requirement that
	 * that the name to be deleted have data?  For now, it is.
	 *
	 * ->dirty, ->locknum and ->references are ignored; they are
	 * solely the province of rbtdb.c.
	 */
	dns_rbtnodechain_init(&chain, rbt->mctx);
	result = dns_rbt_findnode(rbt, name, NULL, &node, &chain, ISC_FALSE,
				  NULL, NULL);

	/*
	 * The guts of this routine are in a separate function (which
	 * is called only once, by this function) to make freeing the
	 * ancestor memory easier, since there are several different
	 * exit points from the level checking logic.
	 */
	if (result == DNS_R_SUCCESS) {
		if (DATA(node) != NULL)
			result = zapnode_and_fixlevels(rbt, node,
						       recurse, &chain);
		else
			result = DNS_R_NOTFOUND;

	} else if (result == DNS_R_PARTIALMATCH)
		result = DNS_R_NOTFOUND;

	put_ancestor_mem(&chain);

	return (result);
}

/*
 *
 */
static dns_result_t
zapnode_and_fixlevels(dns_rbt_t *rbt, dns_rbtnode_t *node,
		      isc_boolean_t recurse, dns_rbtnodechain_t *chain) {
	dns_rbtnode_t *down, *parent, **rootp;
	dns_result_t result;

	down = DOWN(node);

	if (down != NULL) {
		if (recurse) {
			dns_rbt_deletetree(rbt, down);
			down = NULL;

		} else {
			if (rbt->data_deleter != NULL)
				rbt->data_deleter(DATA(node),
						  rbt->deleter_arg);
			DATA(node) = NULL;

			if (LEFT(down) != NULL || RIGHT(down) != NULL)
				/*
				 * This node cannot be removed because it
				 * points down to a level that has more than
				 * one node, so it must continue to serve
				 * as the root for that level.  All that
				 * could be done was to blast its data.
				 */
				return (DNS_R_SUCCESS);

			/*
			 * There is a down pointer to a level with a single
			 * item.  That item's name can be joined with the name
			 * on this level.
			 */
			INSIST(chain->ancestor_count > 0);
			rootp = chain->level_count > 0 ?
				&DOWN(chain->levels[chain->level_count - 1]) :
				&rbt->root;
			parent = chain->ancestors[chain->ancestor_count - 1];

			result = join_nodes(rbt, node, parent, rootp);

			return (result);
		}
	}

	/*
	 * This node now has no down pointer (either because it didn't
	 * have one to start, or because it was recursively removed).
	 * So now the node needs to be removed from this level.
	 */
	dns_rbt_deletefromlevel(rbt, node, chain);

	if (rbt->data_deleter != NULL)
		rbt->data_deleter(DATA(node), rbt->deleter_arg);
	isc_mem_put(rbt->mctx, node, NODE_SIZE(node));

	/*
	 * Everything is successful, unless the next block fails.
	 */
	result = DNS_R_SUCCESS;

	/*
	 * If there is one node left on this level, and the node one level up
	 * that points down to here has no data, then those two nodes can be
	 * merged.  The focus for exploring this criteria is shifted up one
	 * level.
	 */
	node = chain->level_count > 0 ?
		chain->levels[chain->level_count - 1] : NULL;

	if (node != NULL && DATA(node) == NULL &&
	    LEFT(DOWN(node)) == NULL && RIGHT(DOWN(node)) == NULL) {
		rootp = chain->level_count > 1 ?
			&DOWN(chain->levels[chain->level_count - 2]) :
			&rbt->root;
		/*
		 * The search to find the original node went through the
		 * node that is now being examined.  It might have been
		 *
		 * current_node -down-to-> deleted_node      ... or ...
		 *
		 * current_node -down-to-> remaining_node -left/right-to->
		 *						deleted_node
		 *
		 * In the first case, ancestor_count - 1 is NULL and - 2
		 * is the parent of current_node (possibly also NULL).
		 * In the second case, ancestor_count - 1 is remaining_node,
		 * - 2, is NULL and - 3 is the parent of current_node.
		 */
		parent = chain->ancestors[chain->ancestor_count - 1] == NULL ?
			 chain->ancestors[chain->ancestor_count - 2] :
			 chain->ancestors[chain->ancestor_count - 3];

		result = join_nodes(rbt, node, parent, rootp);
	}

	return (result);
}

void
dns_rbt_namefromnode(dns_rbtnode_t *node, dns_name_t *name) {

	REQUIRE(name->offsets == NULL);

	NODENAME(node, name);
}

static dns_result_t
create_node(isc_mem_t *mctx, dns_name_t *name, dns_rbtnode_t **nodep) {
	dns_rbtnode_t *node;
	isc_region_t region;
	unsigned int labels;

	REQUIRE(name->offsets != NULL);

	dns_name_toregion(name, &region);
	labels = FAST_COUNTLABELS(name);
	ENSURE(labels > 0);

	/* 
	 * Allocate space for the node structure, the name, and the offsets.
	 */
	node = (dns_rbtnode_t *)isc_mem_get(mctx, sizeof(*node) +
					    region.length + labels);
					    
	if (node == NULL)
		return (DNS_R_NOMEMORY);

	RIGHT(node) = NULL;
	LEFT(node) = NULL;
	DOWN(node) = NULL;
	DATA(node) = NULL;

	LOCK(node) = 0;
	REFS(node) = 0;
	DIRTY(node) = 0;
	WILD(node) = 0;

	MAKE_BLACK(node);
	FINDCALLBACK(node) = 0;

	/*
	 * The following is stored to make reconstructing a name from the
	 * stored value in the node easy:  the length of the name, the number
	 * of labels, whether the name is absolute or not, the name itself,
	 * and the name's offsets table.
	 *
	 * XXX RTH
	 *	The offsets table could be made smaller by eliminating the
	 *	first offset, which is always 0.  This requires changes to
	 * 	lib/dns/name.c.
	 */
	NAMELEN(node) = region.length;
	PADBYTES(node) = 0;
	OFFSETLEN(node) = labels;
	ATTRS(node) = name->attributes;

	memcpy(NAME(node), region.base, region.length);
	memcpy(OFFSETS(node), name->offsets, labels);

	*nodep = node;

	return (DNS_R_SUCCESS);
}

static dns_result_t
join_nodes(dns_rbt_t *rbt,
	   dns_rbtnode_t *node, dns_rbtnode_t *parent, dns_rbtnode_t **rootp)
{
	dns_rbtnode_t *down, *newnode;
	dns_result_t result;
	dns_fixedname_t fixed_newname;
	dns_name_t *newname, prefix, suffix;
	unsigned int newlength, oldlength;

	REQUIRE(VALID_RBT(rbt));
	REQUIRE(node != NULL);
	REQUIRE(DATA(node) == NULL && DOWN(node) != NULL);

	down = DOWN(node);

	dns_name_init(&prefix, NULL);
	dns_name_init(&suffix, NULL);
	dns_fixedname_init(&fixed_newname);

	NODENAME(down, &prefix);
	NODENAME(node, &suffix);

	newname = dns_fixedname_name(&fixed_newname);

	result = dns_name_concatenate(&prefix, &suffix, newname, NULL);
	if (result != DNS_R_SUCCESS)
		return (result);

	/*
	 * Check whether the space needed for the joined names can
	 * fit within the space already available in the down node,
	 * so that any external references to the down node are preserved.
	 *
	 * Currently this is not very meaningful since preservation
	 * of the address of the down node cannot be guaranteed.
	 */
	newlength = newname->length + newname->labels;
	oldlength = NAMELEN(down) + OFFSETLEN(down);
	if (newlength > oldlength + PADBYTES(down))
		result = create_node(rbt->mctx, newname, &newnode);

	else {
		memcpy(NAME(down), newname->ndata, newname->length);
		PADBYTES(down) -= newlength - oldlength;
		NAMELEN(down) = newname->length;
		OFFSETLEN(down) = newname->labels;
		memcpy(OFFSETS(down), newname->offsets, newname->labels);

		ATTRS(down) = newname->attributes;

		newnode = down;
		result = DNS_R_SUCCESS;
	}

	if (result == DNS_R_SUCCESS) {
		COLOR(newnode) = COLOR(node);
		RIGHT(newnode) = RIGHT(node);
		LEFT(newnode)  = LEFT(node);

		DOWN(newnode) = DOWN(down);
		DATA(newnode) = DATA(down);

		/*
		 * Fix the pointers to the original node.
		 */
		if (parent != NULL) {
			if (LEFT(parent) == node)
				LEFT(parent) = newnode;
			else
				RIGHT(parent) = newnode;

		} else
			*rootp = newnode;

		isc_mem_put(rbt->mctx, node, NODE_SIZE(node));

		if (newnode != down)
			isc_mem_put(rbt->mctx, down, NODE_SIZE(down));
	}

	return (result);
}

static inline void
rotate_left(dns_rbtnode_t *node, dns_rbtnode_t *parent, dns_rbtnode_t **rootp) {
	dns_rbtnode_t *child;

	REQUIRE(node != NULL);
	REQUIRE(rootp != NULL);

	child = RIGHT(node);
	REQUIRE(child != NULL);

	RIGHT(node) = LEFT(child);
	LEFT(child) = node;

	if (parent != NULL) {
		if (LEFT(parent) == node)
			LEFT(parent) = child;
		else
			RIGHT(parent) = child;
	} else
		*rootp = child;
}

static inline void
rotate_right(dns_rbtnode_t *node, dns_rbtnode_t *parent, dns_rbtnode_t **rootp)
{
	dns_rbtnode_t *child;

	REQUIRE(node != NULL);
	REQUIRE(rootp != NULL);

	child = LEFT(node);
	REQUIRE(child != NULL);

	LEFT(node)   = RIGHT(child);
	RIGHT(child) = node;

	if (parent != NULL) {
		if (LEFT(parent) == node)
			LEFT(parent) = child;
		else
			RIGHT(parent) = child;
	} else
		*rootp = child;
}

/*
 * This is the real workhorse of the insertion code, because it does the
 * true red/black tree on a single level.
 */
static void
dns_rbt_addonlevel(dns_rbtnode_t *node,
		   dns_rbtnode_t *current, int order,
		   dns_rbtnode_t **rootp, dns_rbtnodechain_t *chain)
{
	dns_rbtnode_t *child, *root, *tmp, *parent, *grandparent;
	dns_name_t add_name, current_name;
	dns_offsets_t add_offsets, current_offsets;
	unsigned int depth;

	REQUIRE(rootp != NULL);
	REQUIRE(node != NULL    && LEFT(node) == NULL && RIGHT(node) == NULL);
	REQUIRE(current != NULL && LEFT(node) == NULL && RIGHT(node) == NULL);

	root = *rootp;
	if (root == NULL) {
		MAKE_BLACK(node);
		*rootp = node;
		return;
	}

	child = root;

	dns_name_init(&add_name, add_offsets);
	NODENAME(node, &add_name);

	dns_name_init(&current_name, current_offsets);
	NODENAME(current, &current_name);

	if (order < 0)
		LEFT(current) = node;
	else
		RIGHT(current) = node;
	MAKE_RED(node);

	depth = chain->ancestor_count - 1;
	
	while (node != root && IS_RED(chain->ancestors[depth])) {
		INSIST(depth > 0);

		parent = chain->ancestors[depth];
		grandparent = chain->ancestors[depth - 1];

		if (parent == LEFT(grandparent)) {
			child = RIGHT(grandparent);
			if (child != NULL && IS_RED(child)) {
				MAKE_BLACK(parent);
				MAKE_BLACK(child);
				MAKE_RED(grandparent);
				node = grandparent;
				depth -= 2;
			} else {
				if (node == RIGHT(parent)) {
					rotate_left(parent, grandparent,
						    &root);
					tmp = node;
					node = parent;
					parent = tmp;
					chain->ancestors[depth] = parent;
				}
				MAKE_BLACK(parent);
				MAKE_RED(grandparent);
				INSIST(depth > 1);
				rotate_right(grandparent,
					     chain->ancestors[depth - 2],
					     &root);
			}
		} else {
			child = LEFT(grandparent);
			if (child != NULL && IS_RED(child)) {
				MAKE_BLACK(parent);
				MAKE_BLACK(child);
				MAKE_RED(grandparent);
				node = grandparent;
				depth -= 2;
			} else {
				if (node == LEFT(parent)) {
					rotate_right(parent, grandparent,
						     &root);
					tmp = node;
					node = parent;
					parent = tmp;
					chain->ancestors[depth] = parent;
				}
				MAKE_BLACK(parent);
				MAKE_RED(grandparent);
				INSIST(depth > 1);
				rotate_left(grandparent,
					    chain->ancestors[depth - 2],
					    &root);
			}
		}
	}

	MAKE_BLACK(root);
	*rootp = root;

	return;
}

/*
 * This is the real workhorse of the deletion code, because it does the
 * true red/black tree on a single level.
 *
 * The ancestor and level history _must_ be set with dns_rbt_findnode for
 * this function to work properly.
 */
static void
dns_rbt_deletefromlevel(dns_rbt_t *rbt, dns_rbtnode_t *delete,
			dns_rbtnodechain_t *chain) {
	dns_rbtnode_t *sibling, *parent, *grandparent, *child;
	dns_rbtnode_t *successor, **rootp;
	int depth;

	REQUIRE(VALID_RBT(rbt));
	REQUIRE(delete);
	REQUIRE(chain->ancestor_count > 0);

	parent = chain->ancestors[chain->ancestor_count - 1];

	if (chain->level_count > 0)
		rootp = &DOWN(chain->levels[chain->level_count - 1]);
	else
		rootp = &rbt->root;

	/*
	 * Verify that the ancestor/level history is (apparently) correct.
	 */
	REQUIRE((parent == NULL && *rootp == delete) ||
		(parent != NULL && 
		 (LEFT(parent) == delete || RIGHT(parent) == delete)));

	child = NULL;

	if (LEFT(delete) == NULL)
		if (RIGHT(delete) == NULL) {
			if (chain->ancestors[chain->ancestor_count - 1]
			    == NULL) {
				/*
				 * This is the only item in the tree.
				 */
				*rootp = NULL;
				return;
			}
		} else
			/*
			 * This node has one child, on the right.
			 */
			child = RIGHT(delete);

	else if (RIGHT(delete) == NULL)
		/*
		 * This node has one child, on the left.
		 */
		child = LEFT(delete);

	else {
		dns_rbtnode_t holder, *tmp = &holder;

		/*
		 * This node has two children, so it cannot be directly
		 * deleted.  Find its immediate in-order successor and
		 * move it to this location, then do the deletion at the
		 * old site of the successor.
		 */
		depth = chain->ancestor_count++;
		successor = RIGHT(delete);
		while (LEFT(successor) != NULL) {
			chain->ancestors[chain->ancestor_count++] = successor;
			successor = LEFT(successor);

		}

		/*
		 * The successor cannot possibly have a left child;
		 * if there is any child, it is on the right.
		 */
		if (RIGHT(successor))
			child = RIGHT(successor);

		/* Swap the two nodes; it would be simpler to just replace
		 * the value being deleted with that of the successor,
		 * but this rigamarole is done so the caller has complete
		 * control over the pointers (and memory allocation) of
		 * all of nodes.  If just the key value were removed from
		 * the tree, the pointer to the node would would be
		 * unchanged.
		 */

		/*
		 * First, put the successor in the tree location of the
		 * node to be deleted.
		 */

		memcpy(tmp, successor, sizeof(dns_rbtnode_t));

		chain->ancestors[depth] = successor;
		parent = chain->ancestors[depth - 1];

		if (parent)
			if (LEFT(parent) == delete)
				LEFT(parent) = successor;
			else
				RIGHT(parent) = successor;
		else
			*rootp = successor;

		LEFT(successor)  = LEFT(delete);
		RIGHT(successor) = RIGHT(delete);
		COLOR(successor) = COLOR(delete);

		/*
		 * Now relink the node to be deleted into the
		 * successor's previous tree location.
		 */
		parent = chain->ancestors[chain->ancestor_count - 1];
		if (parent == successor)
			RIGHT(parent) = delete;
		else
			LEFT(parent) = delete;

		/*
		 * Original location of successor node has no left.
		 */

		LEFT(delete)  = NULL;
		RIGHT(delete) = RIGHT(tmp);
		COLOR(delete) = COLOR(tmp);
	}

	parent = chain->ancestors[chain->ancestor_count - 1];

	/*
	 * Remove the node by removing the links from its parent.
	 */
	if (parent != NULL) {
		if (LEFT(parent) == delete) {
			LEFT(parent) = child;
			sibling = RIGHT(parent);
		} else {
			RIGHT(parent) = child;
			sibling = LEFT(parent);
		}

	} else {
		/*
		 * This is the root being deleted, and at this point
		 * it is known to have just one child.
		 */
		sibling = NULL;
		*rootp = child;
	} 

	/*
	 * Fix color violations.
	 */
	if (IS_BLACK(delete)) {
		dns_rbtnode_t *parent;
		depth = chain->ancestor_count - 1;

		while (child != *rootp && IS_BLACK(child)) {
			parent = chain->ancestors[depth--];
			grandparent = chain->ancestors[depth];

			if (LEFT(parent) == child) {
				sibling = RIGHT(parent);
				if (IS_RED(sibling)) {
					MAKE_BLACK(sibling);
					MAKE_RED(parent);
					rotate_left(parent, grandparent,
						    rootp);
					sibling = RIGHT(parent);
				}
				if (IS_BLACK(LEFT(sibling)) &&
				    IS_BLACK(RIGHT(sibling))) {
					MAKE_RED(sibling);
					child = parent;
				} else {
					if (IS_BLACK(RIGHT(sibling))) {
						MAKE_BLACK(LEFT(sibling));
						MAKE_RED(sibling);
						rotate_right(sibling,
							     grandparent,
							     rootp);
						sibling = RIGHT(parent);
					}
					COLOR(sibling) = COLOR(parent);
					MAKE_BLACK(parent);
					MAKE_BLACK(RIGHT(sibling));
					rotate_left(parent, grandparent,
						    rootp);
					child = *rootp;
				}
			} else {
				sibling = LEFT(parent);
				if (IS_RED(sibling)) {
					MAKE_BLACK(sibling);
					MAKE_RED(parent);
					rotate_right(parent, grandparent,
						     rootp);
					sibling = LEFT(parent);
				}
				if (IS_BLACK(LEFT(sibling)) &&
				    IS_BLACK(RIGHT(sibling))) {
					MAKE_RED(sibling);
					child = parent;
				} else {
					if (IS_BLACK(LEFT(sibling))) {
						MAKE_BLACK(RIGHT(sibling));
						MAKE_RED(sibling);
						rotate_left(sibling,
							    grandparent,
							    rootp);
						sibling = LEFT(parent);
					}
					COLOR(sibling) = COLOR(parent);
					MAKE_BLACK(parent);
					MAKE_BLACK(LEFT(sibling));
					rotate_right(parent, grandparent,
						     rootp);
					child = *rootp;
				}
			}

		}

		if (IS_RED(child))
			MAKE_BLACK(child);
	}
}

/*
 * This should only be used on the root of a tree, because no color fixup
 * is done at all.
 *
 * NOTE: No root pointer maintenance is done, because the function is only
 * used for two cases:
 * + deleting everything DOWN from a node that is itself being deleted, and
 * + deleting the entire tree of trees from dns_rbt_destroy.
 * In each case, the root pointer is no longer relevant, so there
 * is no need for a root parameter to this function.
 *
 * If the function is ever intended to be used to delete something where
 * a pointer needs to be told that this tree no longer exists,
 * this function would need to adjusted accordingly.
 */
static void
dns_rbt_deletetree(dns_rbt_t *rbt, dns_rbtnode_t *node) {

	REQUIRE(VALID_RBT(rbt));

	if (node == NULL)
		return;

	if (LEFT(node) != NULL)
		dns_rbt_deletetree(rbt, LEFT(node));
	if (RIGHT(node) != NULL)
		dns_rbt_deletetree(rbt, RIGHT(node));
	if (DOWN(node) != NULL)
		dns_rbt_deletetree(rbt, DOWN(node));

	if (DATA(node) != NULL && rbt->data_deleter != NULL)
		rbt->data_deleter(DATA(node), rbt->deleter_arg);

	isc_mem_put(rbt->mctx, node, NODE_SIZE(node));
}

static void
dns_rbt_indent(int depth) {
	int i;

	for (i = 0; i < depth; i++)
		putchar('\t');
}

static void
dns_rbt_printnodename(dns_rbtnode_t *node) {
	char *buffer[255];
	isc_buffer_t target;
	isc_region_t r;
	dns_name_t name;
	dns_offsets_t offsets;

	r.length = NAMELEN(node);
	r.base = NAME(node);

	dns_name_init(&name, offsets);
	dns_name_fromregion(&name, &r);

	isc_buffer_init(&target, buffer, 255, ISC_BUFFERTYPE_TEXT);

	/*
	 * ISC_FALSE means absolute names have the final dot added.
	 */
	dns_name_totext(&name, ISC_FALSE, &target);

	printf("%.*s", (int)target.used, (char *)target.base);
}

static void
dns_rbt_printtree(dns_rbtnode_t *root, dns_rbtnode_t *parent, int depth) {
	dns_rbt_indent(depth);

	if (root != NULL) {
		dns_rbt_printnodename(root);
		printf(" (%s", IS_RED(root) ? "RED" : "black");
		if (parent) {
			printf(" from ");
			dns_rbt_printnodename(parent);
		}
		printf(")\n");
		depth++;

		if (DOWN(root)) {
			dns_rbt_indent(depth);
			printf("++ BEG down from ");
			dns_rbt_printnodename(root);
			printf("\n");
			dns_rbt_printtree(DOWN(root), NULL, depth);
			dns_rbt_indent(depth);
			printf("-- END down from ");
			dns_rbt_printnodename(root);
			printf("\n");
		}

		if (IS_RED(root) && IS_RED(LEFT(root)))
		    printf("** Red/Red color violation on left\n");
		dns_rbt_printtree(LEFT(root), root, depth);

		if (IS_RED(root) && IS_RED(RIGHT(root)))
		    printf("** Red/Red color violation on right\n");
		dns_rbt_printtree(RIGHT(root), root, depth);

	} else
		printf("NULL\n");
}

void
dns_rbt_printall(dns_rbt_t *rbt) {
	REQUIRE(VALID_RBT(rbt));

	dns_rbt_printtree(rbt->root, NULL, 0);
}

/*
 * Chain Functions
 */

void
dns_rbtnodechain_init(dns_rbtnodechain_t *chain, isc_mem_t *mctx) {
	/*
	 * Initialize 'chain'.
	 */

	REQUIRE(chain != NULL);

	chain->mctx = mctx;
	chain->end = NULL;
	chain->ancestors = chain->ancestor_block;
	chain->ancestor_count = 0;
	chain->ancestor_maxitems = DNS_RBT_ANCESTORBLOCK;
	chain->level_count = 0;
	chain->level_matches = 0;

	chain->magic = CHAIN_MAGIC;
}

dns_result_t
dns_rbtnodechain_current(dns_rbtnodechain_t *chain, dns_name_t *name,
			 dns_name_t *origin, dns_rbtnode_t **node) {
	dns_result_t result = DNS_R_SUCCESS;

	REQUIRE(VALID_CHAIN(chain));

	if (node != NULL)
		*node = chain->end;

	if (chain->end == NULL)
		return (DNS_R_NOTFOUND);

	if (name != NULL) {
		NODENAME(chain->end, name);

		if (chain->level_count == 0) {
			/*
			 * Eliminate the root name, except when name is ".".
			 */
			if (FAST_COUNTLABELS(name) > 1) {
				INSIST(FAST_ISABSOLUTE(name));

				/*
				 * XXX EVIL.  But what _should_ I do?
				 */
				name->labels--;
				name->length--;
				name->attributes &= ~DNS_NAMEATTR_ABSOLUTE;
			}
		}
	}

	if (origin != NULL) {
		if (chain->level_count > 0)
			result = chain_name(chain, origin, ISC_FALSE);
		else
			result = dns_name_concatenate(NULL, dns_rootname,
						      origin, NULL);
	}

	return (result);
}

dns_result_t
dns_rbtnodechain_prev(dns_rbtnodechain_t *chain, dns_name_t *name,
		      dns_name_t *origin)
{
	dns_rbtnode_t *current, *previous, *predecessor;
	dns_result_t result = DNS_R_SUCCESS;
	isc_boolean_t new_origin = ISC_FALSE;

	REQUIRE(VALID_CHAIN(chain) && chain->end != NULL);

	predecessor = NULL;

	current = chain->end;

	if (LEFT(current) != NULL) {
		ADD_ANCESTOR(chain, current);
		current = LEFT(current);

		while (RIGHT(current) != NULL) {
			ADD_ANCESTOR(chain, current);
			current = RIGHT(current);
		}

		predecessor = current;

	} else {
		while (chain->ancestors[chain->ancestor_count - 1] != NULL) {
			INSIST(chain->ancestor_count > 1);
			previous = current;
			current = chain->ancestors[--chain->ancestor_count];

			if (RIGHT(current) == previous) {
				predecessor = current;
				break;
			}
		}
	}	

	if (predecessor != NULL) {
		if (DOWN(predecessor) != NULL) {
			/*
			 * The predecessor is really down at least one level.
			 * Go down and as far right as possible, and repeat
			 * as long as the rightmost node has a down pointer.
			 */
			do {
				/*
				 * XXX DCL Need to do something about origins
				 * here. See whether to go down, and if so
				 * whether it is truly what Bob calls a
				 * new origin.
				 */
				ADD_ANCESTOR(chain, NULL);
				ADD_LEVEL(chain, predecessor);
				predecessor = DOWN(predecessor);

				/* XXX DCL duplicated from above; clever
				 * way to unduplicate? */

				while (RIGHT(predecessor) != NULL) {
					ADD_ANCESTOR(chain, predecessor);
					predecessor = RIGHT(predecessor);
				}
			} while (DOWN(predecessor) != NULL);

			/* XXX DCL probably needs work on the concept */
			if (origin != NULL)
				new_origin = ISC_TRUE;
		}

	} else if (chain->level_count > 0) {
		/*
		 * Got to the root of this level without having traversed
		 * any right links.  Ascend the tree one level.
		 */
		INSIST(chain->level_count > 0 &&
		       chain->ancestor_count > 0);
		predecessor = chain->levels[--chain->level_count];
		chain->ancestor_count--;

		/* XXX DCL probably needs work on the concept */
		/*
		 * Don't declare an origin change when the new origin is "."
		 * at the top level tree, because "." is declared as the origin
		 * for the second level tree.
		 */
		if (origin &&
		    (chain->level_count > 0 || OFFSETLEN(predecessor) > 1))
			new_origin = ISC_TRUE;
	}

	if (predecessor != NULL) {
		chain->end = predecessor;

		if (new_origin) {
			result = dns_rbtnodechain_current(chain, name, origin,
							  NULL);
			if (result == DNS_R_SUCCESS)
				result = DNS_R_NEWORIGIN;

		} else
			result = dns_rbtnodechain_current(chain, name, NULL,
							  NULL);

	} else
		result = DNS_R_NOMORE;

	return (result);
}

dns_result_t
dns_rbtnodechain_next(dns_rbtnodechain_t *chain, dns_name_t *name,
		      dns_name_t *origin)
{
	dns_rbtnode_t *current, *previous, *successor;
	dns_result_t result = DNS_R_SUCCESS;
	isc_boolean_t new_origin = ISC_FALSE;

	REQUIRE(VALID_CHAIN(chain) && chain->end != NULL);

	successor = NULL;

	current = chain->end;

	if (DOWN(current) != NULL) {
		/*
		 * Don't declare an origin change when the new origin is "."
		 * at the second level tree, because "." is already declared
		 * as the origin for the top level tree.
		 */
		if (chain->level_count > 0 ||
		    OFFSETLEN(current) > 1)
			new_origin = ISC_TRUE;

		ADD_ANCESTOR(chain, NULL);
		ADD_LEVEL(chain, current);
		current = DOWN(current);

		while (LEFT(current) != NULL) {
			ADD_ANCESTOR(chain, current);
			current = LEFT(current);
		}

		successor = current;

	} else if (RIGHT(current) == NULL) {
		/*
		 * The successor is up, either in this level or a previous one.
		 * Head back toward the root of the tree, looking for any path
		 * that was via a left link; the successor is the node that has
		 * that left link.  In the event the root of the level is
		 * reached without having traversed any left links, ascend one
		 * level and look for either a right link off the point of
		 * ascent, or search for a left link upward again, repeating
		 * ascents until either case is true.
		 */
		do {
			while (chain->ancestors[chain->ancestor_count - 1] !=
			       NULL) {
				INSIST(chain->ancestor_count > 1);
				previous = current;
				current =
				     chain->ancestors[--chain->ancestor_count];

				if (LEFT(current) == previous) {
					successor = current;
					break;
				}
			}

			if (successor == NULL) {
				/*
				 * Reached the root without having traversed
				 * any left pointers, so this level is done.
				 */
				if (chain->level_count == 0)
					break;

				INSIST(chain->ancestor_count > 0);
				current = chain->levels[--chain->level_count];
				chain->ancestor_count--;
				new_origin = ISC_TRUE;

				if (RIGHT(current) != NULL)
					break;
			}
		} while (successor == NULL);
	}

	if (successor == NULL && RIGHT(current) != NULL) {
		ADD_ANCESTOR(chain, current);
		current = RIGHT(current);

		while (LEFT(current) != NULL) {
			ADD_ANCESTOR(chain, current);
			current = LEFT(current);
		}

		successor = current;
	}

	if (successor != NULL) {
		chain->end = successor;

		/*
		 * It is not necessary to use dns_rbtnodechain_current like
		 * the other functions because this function will never
		 * find a node in the topmost level.  This is because the
		 * root level will never be more than one name, and everything
		 * in the megatree is a successor to that node, down at
		 * the second level or below.
		 */

		if (name != NULL)
			NODENAME(chain->end, name);

		if (new_origin) {
			if (origin != NULL)
				result = chain_name(chain, origin, ISC_FALSE);

			if (result == DNS_R_SUCCESS)
				result = DNS_R_NEWORIGIN;

		} else
			result = DNS_R_SUCCESS;

	} else
		result = DNS_R_NOMORE;

	return (result);
}

dns_result_t
dns_rbtnodechain_first(dns_rbtnodechain_t *chain, dns_rbt_t *rbt,
		       dns_name_t *name, dns_name_t *origin)

{
	dns_result_t result;

	REQUIRE(VALID_RBT(rbt));
	REQUIRE(VALID_CHAIN(chain));

	dns_rbtnodechain_reset(chain);

	ADD_ANCESTOR(chain, NULL);

	chain->end = rbt->root;

	result = dns_rbtnodechain_current(chain, name, origin, NULL);

	if (result == DNS_R_SUCCESS)
		result = DNS_R_NEWORIGIN;

	return (result);
}

dns_result_t
dns_rbtnodechain_last(dns_rbtnodechain_t *chain, dns_rbt_t *rbt,
		       dns_name_t *name, dns_name_t *origin)

{
	dns_result_t result;

	REQUIRE(name != NULL && origin != NULL);
	REQUIRE(VALID_RBT(rbt));
	REQUIRE(VALID_CHAIN(chain));

	dns_rbtnodechain_reset(chain);

	ADD_ANCESTOR(chain, NULL);

	result = move_chain_to_last(chain, rbt->root);
	if (result != DNS_R_SUCCESS)
		return (result);

	result = dns_rbtnodechain_current(chain, name, origin, NULL);

	if (result == DNS_R_SUCCESS)
		result = DNS_R_NEWORIGIN;

	return (result);
}


void
dns_rbtnodechain_reset(dns_rbtnodechain_t *chain) {
	/*
	 * Free any dynamic storage associated with 'chain', and then
	 * reinitialize 'chain'.
	 */

	REQUIRE(VALID_CHAIN(chain));

	put_ancestor_mem(chain);
	chain->end = NULL;
	chain->ancestors = chain->ancestor_block;
	chain->ancestor_count = 0;
	chain->ancestor_maxitems = DNS_RBT_ANCESTORBLOCK;
	chain->level_count = 0;
	chain->level_matches = 0;
}

void
dns_rbtnodechain_invalidate(dns_rbtnodechain_t *chain) {
	/*
	 * Free any dynamic storage associated with 'chain', and then
	 * invalidate 'chain'.
	 */

	dns_rbtnodechain_reset(chain);

	chain->magic = 0;
}
