/* The tree: workspaces, splits and windows.
 *
 * Everything a tiling compositor does is a small operation on one tree.  A
 * node is either a window or a split holding other nodes; a split divides its
 * rectangle between its children, and each child divides its own.  Nothing
 * overlaps, nothing is placed by hand, and the geometry of every window on the
 * screen falls out of one walk from the root.
 *
 * The one decision with any taste in it is where a new window goes.  i3 puts
 * it beside the focused one, in the focused one's container, and so does this.
 * A workspace with nothing on it splits along its longest side, which on a
 * screen wider than it is tall means the second window appears to the right --
 * the arrangement people expect without having asked for it.
 */

#include "sway.h"

struct workspace *ws_current(void)
{
    return &sway.workspaces[sway.current];
}

static struct node *node_create(int is_view)
{
    struct node *n = malloc(sizeof(*n));
    if (!n)
        return NULL;

    memset(n, 0, sizeof(*n));
    n->is_view = is_view;
    return n;
}

/* Which way a rectangle should be divided if nobody has said.
 *
 * The longest side, measured in pixels rather than in characters: a screen is
 * 640x400 and a split down the middle of it gives two windows that are still
 * wider than they are tall, which is what a window is for. */
static enum layout natural_layout(int w, int h)
{
    return (w >= h) ? L_SPLITH : L_SPLITV;
}

void layout_init(void)
{
    for (int i = 0; i < MAX_WORKSPACES; i++) {
        struct workspace *ws = &sway.workspaces[i];

        ws->number = i + 1;
        wsnprintf(ws->name, sizeof(ws->name), "%d", i + 1);

        ws->root = node_create(0);
        if (ws->root)
            ws->root->layout = natural_layout((int)sway.screen.width,
                                              sway.usable_h);
        ws->focus      = NULL;
        ws->fullscreen = NULL;
    }
}

/* ------------------------------------------------------------------ *
 *  Walking
 * ------------------------------------------------------------------ */

static void node_append(struct node *parent, struct node *child)
{
    child->parent = parent;
    child->next   = NULL;

    if (!parent->children) {
        parent->children = child;
        return;
    }

    struct node *at = parent->children;
    while (at->next)
        at = at->next;
    at->next = child;
}

static void node_insert_after(struct node *sibling, struct node *child)
{
    child->parent = sibling->parent;
    child->next   = sibling->next;
    sibling->next = child;
}

static void node_detach(struct node *n)
{
    struct node *p = n->parent;
    if (!p)
        return;

    if (p->children == n) {
        p->children = n->next;
    } else {
        for (struct node *at = p->children; at; at = at->next)
            if (at->next == n) {
                at->next = n->next;
                break;
            }
    }

    n->parent = NULL;
    n->next   = NULL;
}

struct node *layout_first_leaf(struct node *n)
{
    if (!n)
        return NULL;
    if (n->is_view)
        return n;

    for (struct node *c = n->children; c; c = c->next) {
        struct node *leaf = layout_first_leaf(c);
        if (leaf)
            return leaf;
    }
    return NULL;
}

static int count_leaves(const struct node *n)
{
    if (!n)
        return 0;
    if (n->is_view)
        return 1;

    int total = 0;
    for (const struct node *c = n->children; c; c = c->next)
        total += count_leaves(c);
    return total;
}

int layout_view_count(const struct workspace *ws)
{
    return count_leaves(ws->root);
}

/* Which workspace a node belongs to, by walking up to a root. */
static struct workspace *workspace_of(struct node *n)
{
    while (n && n->parent)
        n = n->parent;

    for (int i = 0; i < MAX_WORKSPACES; i++)
        if (sway.workspaces[i].root == n)
            return &sway.workspaces[i];
    return NULL;
}

/* A split with one child is a split that is not dividing anything.  Collapsing
 * it keeps the tree from growing a spine of pointless containers as windows
 * come and go -- which would otherwise make `focus parent` climb through
 * levels that mean nothing. */
static void collapse(struct node *n)
{
    while (n && n->parent && !n->is_view) {
        struct node *parent = n->parent;

        if (!n->children) {
            node_detach(n);
            free(n);
            n = parent;
            continue;
        }

        if (!n->children->next) {
            struct node *only = n->children;

            only->parent = parent;
            only->next   = n->next;

            if (parent->children == n) {
                parent->children = only;
            } else {
                for (struct node *at = parent->children; at; at = at->next)
                    if (at->next == n) {
                        at->next = only;
                        break;
                    }
            }

            free(n);
            n = parent;
            continue;
        }

        break;
    }
}

/* ------------------------------------------------------------------ *
 *  Adding and removing
 * ------------------------------------------------------------------ */

void layout_add_view(struct view *v)
{
    struct workspace *ws = ws_current();
    struct node      *n  = node_create(1);

    if (!n || !ws->root)
        return;

    n->view = v;
    v->node = n;

    if (ws->focus && ws->focus->parent) {
        /* Beside the focused window, in its container: what i3 does, and what
         * makes a second window appear next to the one being worked in rather
         * than somewhere across the screen. */
        node_insert_after(ws->focus, n);
    } else {
        node_append(ws->root, n);
    }

    ws->focus = n;
    layout_focus(v);
    layout_arrange();
}

void layout_remove_view(struct view *v)
{
    struct node *n = v->node;
    if (!n)
        return;

    struct workspace *ws = workspace_of(n);
    struct node      *parent = n->parent;

    /* Where the focus should land: the next window in the same container,
     * failing that the previous one, failing that whatever is left. */
    struct node *successor = n->next;
    if (!successor && parent) {
        for (struct node *c = parent->children; c && c != n; c = c->next)
            successor = c;
    }

    node_detach(n);
    free(n);
    v->node = NULL;

    if (ws) {
        if (ws->fullscreen == v)
            ws->fullscreen = NULL;

        if (ws->focus == n)
            ws->focus = NULL;
    }

    collapse(parent);

    if (ws && !ws->focus) {
        struct node *leaf = successor ? layout_first_leaf(successor) : NULL;
        if (!leaf)
            leaf = layout_first_leaf(ws->root);
        ws->focus = leaf;
    }

    if (sway.focused == v)
        sway.focused = NULL;

    if (ws == ws_current())
        layout_focus(ws->focus ? ws->focus->view : NULL);

    layout_arrange();
}

/* ------------------------------------------------------------------ *
 *  Geometry
 * ------------------------------------------------------------------ */

static void arrange_node(struct node *n, int x, int y, int w, int h)
{
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;

    if (n->is_view) {
        struct view *v = n->view;
        int gap = sway.config.gaps_inner;

        v->x = x + gap;
        v->y = y + gap;
        v->w = w - 2 * gap;
        v->h = h - 2 * gap;

        if (v->w < 1) v->w = 1;
        if (v->h < 1) v->h = 1;

        shell_configure(v);
        return;
    }

    int count = 0;
    for (struct node *c = n->children; c; c = c->next)
        count++;
    if (count == 0)
        return;

    /* Divided evenly, with the remainder spread over the first few children
     * rather than dumped on the last one -- so three windows across 640 pixels
     * come out 214, 213, 213 instead of 213, 213, 214. */
    if (n->layout == L_SPLITH) {
        int each = w / count, extra = w % count, at = x, i = 0;

        for (struct node *c = n->children; c; c = c->next, i++) {
            int width = each + (i < extra ? 1 : 0);
            arrange_node(c, at, y, width, h);
            at += width;
        }
    } else {
        int each = h / count, extra = h % count, at = y, i = 0;

        for (struct node *c = n->children; c; c = c->next, i++) {
            int height = each + (i < extra ? 1 : 0);
            arrange_node(c, x, at, w, height);
            at += height;
        }
    }
}

void layout_arrange(void)
{
    struct workspace *ws = ws_current();
    int gap = sway.config.gaps_outer;

    if (!ws->root)
        return;

    if (ws->fullscreen) {
        struct view *v = ws->fullscreen;

        v->x = 0;
        v->y = 0;
        v->w = (int)sway.screen.width;
        v->h = (int)sway.screen.height;
        shell_configure(v);
    }

    arrange_node(ws->root, gap, sway.usable_y + gap,
                 (int)sway.screen.width - 2 * gap, sway.usable_h - 2 * gap);

    sway.dirty = 1;
}

/* ------------------------------------------------------------------ *
 *  Focus
 * ------------------------------------------------------------------ */

void layout_focus(struct view *v)
{
    struct view *old = sway.focused;

    if (old == v)
        return;

    sway.focused = v;
    if (v && v->node)
        workspace_of(v->node)->focus = v->node;

    shell_focus_changed(old, v);
    sway.dirty = 1;
}

/* The nearest leaf in a direction.
 *
 * Measured from the middle of the focused window: the candidate has to be on
 * the correct side, and of those the closest wins, with ties broken by how far
 * off the perpendicular axis it is.  That is what makes `focus down` from a
 * tall window on the left reach the window directly below it rather than one
 * across the screen that happens to start a pixel lower. */
static struct node *nearest(struct node *from, enum direction dir)
{
    struct workspace *ws = ws_current();
    struct node      *best = NULL;
    int               best_score = 0;

    int fx = from->x + from->w / 2;
    int fy = from->y + from->h / 2;

    struct node *stack[64];
    int          top = 0;

    if (ws->root)
        stack[top++] = ws->root;

    while (top > 0) {
        struct node *n = stack[--top];

        if (!n->is_view) {
            for (struct node *c = n->children; c && top < 64; c = c->next)
                stack[top++] = c;
            continue;
        }
        if (n == from)
            continue;

        int cx = n->x + n->w / 2;
        int cy = n->y + n->h / 2;
        int along, across;

        switch (dir) {
        case DIR_LEFT:  along = fx - cx; across = cy - fy; break;
        case DIR_RIGHT: along = cx - fx; across = cy - fy; break;
        case DIR_UP:    along = fy - cy; across = cx - fx; break;
        default:        along = cy - fy; across = cx - fx; break;
        }

        if (along <= 0)
            continue;                    /* the wrong side */

        if (across < 0)
            across = -across;

        int score = along + across * 2;
        if (!best || score < best_score) {
            best       = n;
            best_score = score;
        }
    }

    return best;
}

void layout_focus_direction(enum direction dir)
{
    struct workspace *ws = ws_current();

    if (!ws->focus)
        return;

    struct node *n = nearest(ws->focus, dir);
    if (n)
        layout_focus(n->view);
}

/* Move the focused window one place in a direction.
 *
 * Within its own container when that container runs the right way, and out
 * into the parent when it does not -- which is how a window in a vertical
 * stack gets moved to the left of the whole stack rather than nowhere. */
void layout_move_direction(enum direction dir)
{
    struct workspace *ws = ws_current();
    struct node      *n  = ws->focus;

    if (!n || !n->parent)
        return;

    int forward = (dir == DIR_RIGHT || dir == DIR_DOWN);
    enum layout want = (dir == DIR_LEFT || dir == DIR_RIGHT) ? L_SPLITH
                                                             : L_SPLITV;

    struct node *parent = n->parent;

    while (parent) {
        if (parent->layout == want) {
            struct node *prev = NULL;
            for (struct node *c = parent->children; c && c != n; c = c->next)
                prev = c;

            if (forward && n->next) {
                struct node *after = n->next;
                node_detach(n);
                node_insert_after(after, n);
                layout_arrange();
                return;
            }
            if (!forward && prev) {
                struct node *before = NULL;
                for (struct node *c = parent->children; c && c != prev;
                     c = c->next)
                    before = c;

                node_detach(n);
                if (before) {
                    node_insert_after(before, n);
                } else {
                    n->parent   = parent;
                    n->next     = parent->children;
                    parent->children = n;
                }
                layout_arrange();
                return;
            }
        }

        /* Not in this container: try the one above, taking the window with
         * us. */
        if (!parent->parent)
            break;

        struct node *grandparent = parent->parent;
        node_detach(n);

        if (forward)
            node_insert_after(parent, n);
        else {
            n->parent = grandparent;
            if (grandparent->children == parent) {
                n->next = parent;
                grandparent->children = n;
            } else {
                struct node *before = grandparent->children;
                while (before->next != parent)
                    before = before->next;
                n->next     = parent;
                before->next = n;
            }
        }

        collapse(parent);
        layout_arrange();
        return;
    }

    layout_arrange();
}

/* ------------------------------------------------------------------ *
 *  Splitting
 * ------------------------------------------------------------------ */

/* Wrap the focused window in a new container running the other way, so the
 * next window opened lands inside it.  That is what `splitv` means: not "split
 * now" but "the next one goes below". */
void layout_split(enum layout how)
{
    struct workspace *ws = ws_current();
    struct node      *n  = ws->focus;

    if (!n || !n->parent)
        return;

    struct node *split = node_create(0);
    if (!split)
        return;

    split->layout = how;
    split->parent = n->parent;
    split->next   = n->next;

    if (n->parent->children == n) {
        n->parent->children = split;
    } else {
        for (struct node *at = n->parent->children; at; at = at->next)
            if (at->next == n) {
                at->next = split;
                break;
            }
    }

    n->parent        = split;
    n->next          = NULL;
    split->children  = n;

    layout_arrange();
}

void layout_toggle_split(void)
{
    struct workspace *ws = ws_current();

    if (!ws->focus || !ws->focus->parent)
        return;

    layout_split(ws->focus->parent->layout == L_SPLITH ? L_SPLITV : L_SPLITH);
}

/* Change the container the focused window is in, rather than making a new one:
 * `layout splitv` rearranges what is already there. */
void layout_set_layout(enum layout how)
{
    struct workspace *ws = ws_current();

    if (!ws->focus || !ws->focus->parent)
        return;

    ws->focus->parent->layout = how;
    layout_arrange();
}

/* ------------------------------------------------------------------ *
 *  Workspaces
 * ------------------------------------------------------------------ */

void layout_switch_workspace(int number)
{
    if (number < 1 || number > MAX_WORKSPACES)
        return;
    if (number - 1 == sway.current)
        return;

    /* Every window on the workspace being left loses the keyboard, which the
     * protocol says with wl_keyboard.leave.  A client that is not told stays
     * convinced it has the focus and goes on drawing a focused cursor. */
    layout_focus(NULL);

    sway.current = number - 1;

    struct workspace *ws = ws_current();
    if (!ws->focus)
        ws->focus = layout_first_leaf(ws->root);

    layout_focus(ws->focus ? ws->focus->view : NULL);
    layout_arrange();
}

void layout_move_to_workspace(int number)
{
    struct workspace *ws = ws_current();

    if (number < 1 || number > MAX_WORKSPACES || number - 1 == sway.current)
        return;
    if (!ws->focus)
        return;

    struct view      *v      = ws->focus->view;
    struct workspace *target = &sway.workspaces[number - 1];
    struct node      *n      = ws->focus;

    struct node *successor = n->next;
    struct node *parent    = n->parent;

    node_detach(n);
    ws->focus = NULL;
    if (ws->fullscreen == v)
        ws->fullscreen = NULL;
    collapse(parent);

    if (!target->root)
        return;

    if (target->focus)
        node_insert_after(target->focus, n);
    else
        node_append(target->root, n);
    target->focus = n;

    /* The window is somewhere else now, so the focus here has to land on
     * something that is still here. */
    struct node *leaf = successor ? layout_first_leaf(successor) : NULL;
    if (!leaf)
        leaf = layout_first_leaf(ws->root);
    ws->focus = leaf;

    layout_focus(leaf ? leaf->view : NULL);
    layout_arrange();
}
