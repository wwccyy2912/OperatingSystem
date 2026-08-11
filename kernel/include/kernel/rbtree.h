/*
 * rbtree.h - Red-black tree for CFS scheduler
 * Copyright (c) 2026 OpSys Project
 *
 * Left-leaning red-black tree. Nodes are embedded in the owning
 * struct (intrusive). The tree is ordered by a u64 key (vruntime
 * for CFS). Lower key = higher scheduling priority.
 */

#ifndef KERNEL_RBTREE_H
#define KERNEL_RBTREE_H

#include <kernel/types.h>

/* RB tree node colors */
typedef enum { RB_RED = 0, RB_BLACK = 1 } rb_color_t;

/* RB tree node (embedded in the owning struct, e.g. thread_t) */
typedef struct rb_node {
    struct rb_node *left;
    struct rb_node *right;
    struct rb_node *parent;
    rb_color_t      color;
    u64             key;      /* Sort key (vruntime for CFS) */
    bool            in_tree;  /* True if currently linked in a tree */
} rb_node_t;

/* RB tree root */
typedef struct rb_root {
    struct rb_node *node;     /* Root node (NULL = empty tree) */
    u64             count;    /* Number of nodes in tree */
} rb_root_t;

/* Initialize an empty tree */
static inline void rb_init(rb_root_t *root)
{
    root->node = NULL;
    root->count = 0;
}

/* Initialize a node (call before first insert) */
static inline void rb_init_node(rb_node_t *n)
{
    n->left = n->right = n->parent = NULL;
    n->color = RB_RED;
    n->key = 0;
    n->in_tree = false;
}

/* Insert a node into the tree. Returns 0 on success. */
int rb_insert(rb_root_t *root, rb_node_t *node);

/* Remove a node from the tree. */
void rb_remove(rb_root_t *root, rb_node_t *node);

/* Find the node with the minimum key (leftmost). */
rb_node_t *rb_min(rb_root_t *root);

/* Find the node with the maximum key (rightmost). */
rb_node_t *rb_max(rb_root_t *root);

/* Check if the tree is empty. */
static inline bool rb_empty(rb_root_t *root)
{
    return root->node == NULL;
}

/* Get the next node in-order (for iteration). */
rb_node_t *rb_next(rb_node_t *node);

#endif /* KERNEL_RBTREE_H */
