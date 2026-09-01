/*
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details: <https://www.gnu.org/licenses/>.
 *
 * rbtree.c - Red-black tree implementation
 * Copyright (c) 2026 OpSys Project
 *
 * Standard CLRS-style red-black tree.  Uses a static sentinel NIL
 * node internally so that delete-fixup works correctly when the
 * replacement node is NULL.  The public API uses NULL for empty trees.
 *
 * All operations: O(log n).
 *
 * ------------------------------------------------------------------
 * Structure (rbtree):
 *   Intrusive rb_node_t {left, right, parent, color, key, in_tree}
 *   embedded in the owner (e.g. thread_t); rb_root_t {node, count}.
 *   Invariants: root black, no red-red path, equal black-height, so
 *   height <= 2*log2(n+1). Static NIL sentinel for internal use only.
 *
 * How it works:
 *   RbInsert(): plain BST insert (duplicates go right), then
 *   RbInsertFixup() with recolor / RbRotateLeft / RbRotateRight
 *   (3 cases). RbRemove(): RbTransplant() + tree_minimum() successor,
 *   then RbDeleteFixup() resolving the double-black (4 cases); NIL
 *   lets a NULL replacement be fixed up. rb_min/rb_max/rb_next give
 *   ordered iteration.
 *
 * Purpose:
 *   Generic ordered u64-key tree; currently the CFS scheduler's ready
 *   queue (sched.c s_ready_tree, key = vruntime, lower = higher prio).
 *
 * Caveats:
 *   RbInsert() has no in_tree check — callers must not double-insert
 *   (see sched.c). Public API uses NULL, internals use NIL; keep the
 *   two views consistent.
 * ------------------------------------------------------------------
 */

#include <kernel/rbtree.h>

/* ------------------------------------------------------------------ */
/*  Sentinel (internal only)                                           */
/* ------------------------------------------------------------------ */

static rb_node_t _nil = {
    .left   = NULL,
    .right  = NULL,
    .parent = NULL,
    .color  = RB_BLACK,
    .key    = 0,
};

#define NIL (&_nil)

/* ------------------------------------------------------------------ */
/*  Left rotation                                                     */
/* ------------------------------------------------------------------ */

static void RbRotateLeft(rb_root_t *root, rb_node_t *x) {
    rb_node_t *y = x->right;

    x->right = y->left;
    if (y->left != NIL)
        y->left->parent = x;

    y->parent = x->parent;
    if (x->parent == NULL)
        root->node = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;

    y->left   = x;
    x->parent = y;
}

/* ------------------------------------------------------------------ */
/*  Right rotation                                                    */
/* ------------------------------------------------------------------ */

static void RbRotateRight(rb_root_t *root, rb_node_t *x) {
    rb_node_t *y = x->left;

    x->left = y->right;
    if (y->right != NIL)
        y->right->parent = x;

    y->parent = x->parent;
    if (x->parent == NULL)
        root->node = y;
    else if (x == x->parent->right)
        x->parent->right = y;
    else
        x->parent->left = y;

    y->right  = x;
    x->parent = y;
}

/* ------------------------------------------------------------------ */
/*  Insert fixup                                                      */
/* ------------------------------------------------------------------ */

static void RbInsertFixup(rb_root_t *root, rb_node_t *z) {
    while (z->parent && z->parent->color == RB_RED) {
        rb_node_t *gp = z->parent->parent;

        if (z->parent == gp->left) {
            rb_node_t *uncle = gp->right;

            if (uncle->color == RB_RED) {
                /* Case 1: uncle is red — recolor */
                z->parent->color = RB_BLACK;
                uncle->color     = RB_BLACK;
                gp->color        = RB_RED;
                z                = gp;
            } else {
                if (z == z->parent->right) {
                    /* Case 2: zig-zag — rotate to make zig-zig */
                    z = z->parent;
                    RbRotateLeft(root, z);
                }
                /* Case 3: zig-zig — rotate grandparent */
                z->parent->color = RB_BLACK;
                gp->color        = RB_RED;
                RbRotateRight(root, gp);
            }
        } else {
            /* Symmetric: parent is right child of grandparent */
            rb_node_t *uncle = gp->left;

            if (uncle->color == RB_RED) {
                z->parent->color = RB_BLACK;
                uncle->color     = RB_BLACK;
                gp->color        = RB_RED;
                z                = gp;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    RbRotateRight(root, z);
                }
                z->parent->color = RB_BLACK;
                gp->color        = RB_RED;
                RbRotateLeft(root, gp);
            }
        }
    }

    root->node->color = RB_BLACK;
}

/* ------------------------------------------------------------------ */
/*  Insert                                                            */
/* ------------------------------------------------------------------ */

int RbInsert(rb_root_t *root, rb_node_t *node) {
    if (!root || !node)
        return -1;

    node->left    = NIL;
    node->right   = NIL;
    node->parent  = NULL;
    node->color   = RB_RED;
    node->in_tree = true;

    /* BST insert */
    rb_node_t *y = NULL;
    rb_node_t *x = root->node;

    while (x && x != NIL) {
        y = x;
        if (node->key < x->key)
            x = x->left;
        else
            x = x->right; /* duplicates go right */
    }

    node->parent = y;
    if (y == NULL)
        root->node = node; /* empty tree */
    else if (node->key < y->key)
        y->left = node;
    else
        y->right = node;

    root->count++;
    RbInsertFixup(root, node);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Transplant (replace subtree rooted at u with subtree rooted at v)  */
/* ------------------------------------------------------------------ */

static void RbTransplant(rb_root_t *root, rb_node_t *u, rb_node_t *v) {
    if (u->parent == NULL)
        root->node = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;

    v->parent = u->parent;
}

/* ------------------------------------------------------------------ */
/*  Tree minimum                                                      */
/* ------------------------------------------------------------------ */

static rb_node_t *tree_minimum(rb_node_t *x) {
    while (x->left != NIL)
        x = x->left;
    return x;
}

/* ------------------------------------------------------------------ */
/*  Delete fixup                                                      */
/* ------------------------------------------------------------------ */

/*
 * Fix up a double-black at node x.  When x is the sentinel NIL,
 * we track its parent and side separately (since NIL has no parent
 * pointer).
 */
static void RbDeleteFixup(rb_root_t *root, rb_node_t *x, rb_node_t *x_parent, bool x_is_left) {
    while (x != root->node && x->color == RB_BLACK) {
        if (x_is_left) {
            rb_node_t *w = x_parent->right; /* sibling */

            if (w->color == RB_RED) {
                /* Case 1: sibling is red */
                w->color        = RB_BLACK;
                x_parent->color = RB_RED;
                RbRotateLeft(root, x_parent);
                w = x_parent->right;
            }

            if (w->left->color == RB_BLACK && w->right->color == RB_BLACK) {
                /* Case 2: both nephews black */
                w->color = RB_RED;
                x        = x_parent;
                x_parent = x->parent;
                if (x_parent == NULL)
                    break;
                x_is_left = (x == x_parent->left);
            } else {
                if (w->right->color == RB_BLACK) {
                    /* Case 3: right nephew black, left red */
                    w->left->color = RB_BLACK;
                    w->color       = RB_RED;
                    RbRotateRight(root, w);
                    w = x_parent->right;
                }
                /* Case 4: right nephew red */
                w->color        = x_parent->color;
                x_parent->color = RB_BLACK;
                w->right->color = RB_BLACK;
                RbRotateLeft(root, x_parent);
                x = root->node;
            }
        } else {
            /* Symmetric: x is right child */
            rb_node_t *w = x_parent->left;

            if (w->color == RB_RED) {
                w->color        = RB_BLACK;
                x_parent->color = RB_RED;
                RbRotateRight(root, x_parent);
                w = x_parent->left;
            }

            if (w->right->color == RB_BLACK && w->left->color == RB_BLACK) {
                w->color = RB_RED;
                x        = x_parent;
                x_parent = x->parent;
                if (x_parent == NULL)
                    break;
                x_is_left = (x == x_parent->left);
            } else {
                if (w->left->color == RB_BLACK) {
                    w->right->color = RB_BLACK;
                    w->color        = RB_RED;
                    RbRotateLeft(root, w);
                    w = x_parent->left;
                }
                w->color        = x_parent->color;
                x_parent->color = RB_BLACK;
                w->left->color  = RB_BLACK;
                RbRotateRight(root, x_parent);
                x = root->node;
            }
        }
    }

    x->color = RB_BLACK;
}

/* ------------------------------------------------------------------ */
/*  Remove                                                            */
/* ------------------------------------------------------------------ */

void RbRemove(rb_root_t *root, rb_node_t *z) {
    if (!root || !z || z == NIL)
        return;
    if (!z->in_tree)
        return;

    rb_node_t *y                = z;
    rb_color_t y_original_color = y->color;
    rb_node_t *x;
    rb_node_t *x_parent;
    bool       x_is_left;

    if (z->left == NIL) {
        x         = z->right;
        x_parent  = z->parent;
        x_is_left = (x_parent != NULL && z == x_parent->left);
        RbTransplant(root, z, z->right);
    } else if (z->right == NIL) {
        x         = z->left;
        x_parent  = z->parent;
        x_is_left = (x_parent != NULL && z == x_parent->left);
        RbTransplant(root, z, z->left);
    } else {
        y                = tree_minimum(z->right);
        y_original_color = y->color;
        x                = y->right;

        if (y->parent == z) {
            /* x might be NIL; set its parent explicitly */
            x_parent  = y;
            x_is_left = false; /* x is y->right */
            x->parent = y;
        } else {
            x_parent  = y->parent;
            x_is_left = (x == x_parent->left);
            RbTransplant(root, y, y->right);
            y->right         = z->right;
            y->right->parent = y;
        }
        RbTransplant(root, z, y);
        y->left         = z->left;
        y->left->parent = y;
        y->color        = z->color;
    }

    root->count--;
    z->in_tree = false;

    if (y_original_color == RB_BLACK)
        RbDeleteFixup(root, x, x_parent, x_is_left);
}

/* ------------------------------------------------------------------ */
/*  Public helpers                                                     */
/* ------------------------------------------------------------------ */

rb_node_t *rb_min(rb_root_t *root) {
    if (!root || root->node == NIL)
        return NULL;
    return tree_minimum(root->node);
}

rb_node_t *rb_max(rb_root_t *root) {
    if (!root || root->node == NIL)
        return NULL;

    rb_node_t *x = root->node;
    while (x->right != NIL)
        x = x->right;
    return x;
}

rb_node_t *rb_next(rb_node_t *node) {
    if (!node || node == NIL)
        return NULL;

    if (node->right != NIL) {
        node = node->right;
        while (node->left != NIL)
            node = node->left;
        return node;
    }

    rb_node_t *p = node->parent;
    while (p && node == p->right) {
        node = p;
        p    = p->parent;
    }
    return p;
}
