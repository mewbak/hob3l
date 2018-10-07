/* -*- Mode: C -*- */
/* Copyright (C) 2018 by Henrik Theiling, License: GPLv3, see LICENSE file */
/*
 * This is adapted from Francisco Martinez del Rio (2011), v1.4.1.
 * See: http://www4.ujaen.es/~fmartin/bool_op.html
 *
 * The inside/outside idea is the same as described by Sean Conelly in his
 * polybooljs project.
 * See: https://github.com/voidqk/polybooljs
 *
 * The Conelly idea is also a bit complicated, and this library uses xor based
 * bit masks instead, which may be less obvious, but also allows the algorithm
 * to handle polygons with self-overlapping edges.  This feature is not
 * exploited, but I could remove an error case.
 *
 * This implements most of the algorithm using dictionaries instead of, say,
 * a heap for the pqueue.  This avoids 'realloc' and makes it easier to use
 * pool memory.  BSTs worst case is just as good (and we do not need to merge
 * whole pqueue trees, but have only insert/remove operations).
 *
 * The polygons output by this algorithm have no predefined point
 * direction and are always non-self-intersecting and disjoint (except
 * for single points) but there may be holes.  The subsequent triangulation
 * algorithm does not care about point order -- it determines the
 * inside/outside information implicitly and outputs triangles in the correct
 * point order.  But for generating the connective triangles between two 2D
 * layers for the STL output, the paths output by this algorithm must have
 * the correct point order so that STL can compute the correct normal for those
 * triangles.  Therefore, this algorithm also takes care of getting the path
 * point order right.
 */

#define DEBUG 0

#include <stdio.h>
#include <cpmat/dict.h>
#include <cpmat/list.h>
#include <cpmat/ring.h>
#include <cpmat/mat.h>
#include <cpmat/pool.h>
#include <cpmat/alloc.h>
#include <cpmat/vec.h>
#include <cpmat/panic.h>
#include <csg2plane/csg2.h>
#include <csg2plane/ps.h>
#include "internal.h"

/**
 * Whether to optimise some trivial cases.
 * This can be disabled to debug the main algorithm.
 *
 * 0 = no optimisations
 * 1 = empty polygon optimisation
 * 2 = bounding box optimisation
 * 3 = x-coordinate max optimisation => nothing more to do
 * 4 = x-coordinate max optimisation => copy all the rest
 */
#define OPT 3 /* FIXME: 4 is currently buggy */

typedef enum {
    E_NORMAL,
    E_IGNORE,
    E_SAME,
    E_DIFF
} event_type_t;

typedef struct event event_t;

/**
 * Points found by algorithm
 */
typedef struct {
    cp_dict_t node_pt;

    cp_vec2_t coord;
    cp_loc_t loc;

    /**
     * Index in output point array.
     * Initialised to CP_SIZE_MAX.
     */
    size_t idx;

    /**
     * Number of times this point is used in the resulting polygon. */
    size_t path_cnt;
} point_t;

typedef CP_VEC_T(point_t*) v_point_p_t;

/**
 * Events when the algorithm progresses.
 * Points with more info in the left-right plain sweep.
 */
struct event {
    /**
     * Storage in s and chain is mutually exclusive so we
     * use a union here.
     */
    union {
        /**
         * Node for storing in ctxt::s */
        cp_dict_t node_s;

        /**
         * Node for connecting nodes into a ring (there is no
         * root node, but polygon starts are found by using
         * ctxt::poly and starting from the edge that was inserted
         * there. */
        cp_ring_t node_chain;
    };

    /**
     * Storage in q, end, and poly is mutually exclusive,
     * so we use a union here.
     */
    union {
        /** Node for storing in ctxt::q */
        cp_dict_t node_q;
        /** Node for storing in ctxt::end */
        cp_dict_t node_end;
        /** Node for storing in ctxt::poly */
        cp_list_t node_poly;
    };

    cp_loc_t loc;
    point_t *p;
    event_t *other;

    struct {
        /**
         * Mask of poly IDs that have this edge.  Due to overlapping
         * edges, this is a set.  For self-overlapping edges, the
         * corresponding bit is the lowest bit of the overlapped edge
         * count.
         * This mask can be used to compute 'above' from 'below',
         * because a polygon edge will change in/out for a polygon:
         * above = below ^ owner.
         */
        size_t owner;

        /**
         * Mask of whether 'under' this edge, it is 'inside' of the
         * polygon.  Each bit corresponds to inside/outside of the
         * polygon ID corresponding to that bit number.  This is only
         * maintained while the edge is in s, otherwise, only owner and
         * start are used.
         */
        size_t below;
    } in;

    /**
     * Whether this is a left edge (false = right edge)*/
    bool left;

    /**
     * Whether the event point is already part of a path. */
    bool used;

    /**
     * line formular cache to compute intersections with the same
     * precision throughout the algorithm */
    struct {
        /** slope */
        double a;
        /** offset */
        double b;
        /** false: use ax+b; true: use ay+b */
        bool swap;
    } line;

#ifdef PSTRACE
    /**
     * For debug printing */
    size_t debug_tag;
#endif
};

#define _LINE_X(swap,c) ((c)->v[(swap)])
#define _LINE_Y(swap,c) ((c)->v[!(swap)])

/**
 * Accessor of the X or Y coordinate, depending on line.swap.
 * This returns X if not swapped, Y otherwise.
 */
#define LINE_X(e,c) _LINE_X((e)->line.swap, c)

/**
 * Accessor of the X or Y coordinate, depending on line.swap.
 * This returns Y if not swapped, X otherwise.
 */
#define LINE_Y(e,c) _LINE_Y((e)->line.swap, c)


typedef CP_VEC_T(event_t*) v_event_p_t;

/**
 * All data needed during the algorithms runtime.
 */
typedef struct {
    /** Memory pool to use */
    cp_pool_t *pool;

    /** Error output */
    cp_err_t *err;

    /** new points found by the algorithm */
    cp_dict_t *pt;

    /** pqueue of events */
    cp_dict_t *q;

    /** sweep line status */
    cp_dict_t *s;

    /** output segments in a dictionary of open ends */
    cp_dict_t *end;

    /** list of output polygon points (note that a single polyogn
      * may be inserted multiple times) */
    cp_list_t poly;

    /** bounding boxes of polygons */
    cp_vec2_minmax_t bb[2];

    /** minimal max x coordinate of input polygons */
    cp_dim_t minmaxx;

    /** io: which bool operation? */
    cp_bool_op_t op;

    /** in: mark of all negated polygons */
    size_t mask_neg;

    /** in: mark of all polygons */
    size_t mask_all;

    /** current z height (for debugging) */
} ctxt_t;

__unused
static char const *__coord_str(char *s, size_t n, cp_vec2_t const *x)
{
    if (x == NULL) {
        return "NULL";
    }
    snprintf(s, n, FD2, CP_V01(*x));
    s[n-1] = 0;
    return s;
}

#define coord_str(p) __coord_str((char[50]){0}, 50, p)

__unused
static char const *__pt_str(char *s, size_t n, point_t const *x)
{
    if (x == NULL) {
        return "NULL";
    }
    snprintf(s, n, FD2, CP_V01(x->coord));
    s[n-1] = 0;
    return s;
}

#define pt_str(p) __pt_str((char[50]){}, 50, p)

__unused
static char const *ev_type_str(unsigned x)
{
    switch (x) {
    case E_NORMAL: return "normal";
    case E_IGNORE: return "ignore";
    case E_SAME:   return "same";
    case E_DIFF:   return "diff";
    }
    return "?";
}

__unused
static char const *__ev_str(char *s, size_t n, event_t const *x)
{
    if (x == NULL) {
        return "NULL";
    }
    if (x->left) {
        snprintf(s, n, "#("FD2"--"FD2")  %#"_Pz"x %#"_Pz"x",
            CP_V01(x->p->coord),
            CP_V01(x->other->p->coord),
            x->in.owner,
            x->in.below);
    }
    else {
        snprintf(s, n, " ("FD2"--"FD2")# %#"_Pz"x %#"_Pz"x",
            CP_V01(x->other->p->coord),
            CP_V01(x->p->coord),
            x->in.owner,
            x->in.below);
    }
    s[n-1] = 0;
    return s;
}

#define ev_str(x) __ev_str((char[80]){}, 80, x)

#ifdef PSTRACE
static void debug_print_chain(
    event_t *e0,
    size_t tag)
{
    if (e0->debug_tag == tag) {
        return;
    }

    e0->debug_tag = tag;
    cp_printf(cp_debug_ps, "newpath %g %g moveto", CP_PS_XY(e0->p->coord));

    event_t *e1 = CP_BOX_OF(cp_ring_step(&e0->node_chain, 0), event_t, node_chain);
    if (e0 == e1) {
        e1 = CP_BOX_OF(cp_ring_step(&e0->node_chain, 1), event_t, node_chain);
    }
    assert(e0 != e1);

    e1->debug_tag = tag;
    cp_printf(cp_debug_ps, " %g %g lineto", CP_PS_XY(e1->p->coord));

    event_t *ey __unused = e0;
    event_t *ez __unused = e1;
    bool close = false;
    for (cp_ring_each(_ei, &e0->node_chain, &e1->node_chain)) {
        event_t *ei = CP_BOX_OF(_ei, event_t, node_chain);
        ez = ei;
        ei->debug_tag = tag;
        cp_printf(cp_debug_ps, " %g %g lineto", CP_PS_XY(ei->p->coord));
        close = !cp_ring_is_end(_ei);
    }
    if (close) {
        cp_printf(cp_debug_ps, " closepath");
    }
    cp_printf(cp_debug_ps, " stroke\n");
    if (!close && !cp_ring_is_end(&e0->node_chain)) {
        /* find other end */
        cp_printf(cp_debug_ps, "newpath %g %g moveto", CP_PS_XY(e0->p->coord));
        for (cp_ring_each(_ei, &e1->node_chain, &e0->node_chain)) {
            event_t *ei = CP_BOX_OF(_ei, event_t, node_chain);
            ey = ei;
            ei->debug_tag = tag;
            cp_printf(cp_debug_ps, " %g %g lineto", CP_PS_XY(ei->p->coord));
        }
        cp_printf(cp_debug_ps, " stroke\n");
    }

    if (!close) {
        cp_debug_ps_dot(CP_PS_XY(ey->p->coord), 7);
        cp_debug_ps_dot(CP_PS_XY(ez->p->coord), 7);
    }
}
#endif

#if DEBUG || defined(PSTRACE)

static void debug_print_s(
    ctxt_t *c,
    char const *msg,
    event_t *es)
{
#if DEBUG
    LOG("S %s\n", msg);
    for (cp_dict_each(_e, c->s)) {
        event_t *e = CP_BOX_OF(_e, event_t, node_s);
        LOG("S: %s\n", ev_str(e));
    }
#endif

#ifdef PSTRACE
    /* output to postscript */
    if (cp_debug_ps_page_begin()) {
        /* print info */
        cp_printf(cp_debug_ps, "30 30 moveto (CSG: %s) show\n", msg);
        cp_printf(cp_debug_ps, "30 55 moveto (%s) show\n", ev_str(es));

        /* sweep line */
        cp_printf(cp_debug_ps, "0.8 setgray 1 setlinewidth\n");
        cp_printf(cp_debug_ps,
            "newpath %g dup 0 moveto %u lineto stroke\n",
            CP_PS_X(es->p->coord.x),
            CP_PS_PAPER_Y);
        if (!es->left) {
            cp_printf(cp_debug_ps,
                "2 setlinewidth newpath %g %g moveto %g %g lineto stroke\n",
                CP_PS_XY(es->p->coord),
                CP_PS_XY(es->other->p->coord));
        }

        /* pt */
        cp_printf(cp_debug_ps, "0.8 setgray\n");
        for (cp_dict_each(_p, c->pt)) {
            point_t *p = CP_BOX_OF(_p, point_t, node_pt);
            cp_debug_ps_dot(CP_PS_XY(p->coord), 3);
        }

        /* s */
        cp_printf(cp_debug_ps, "3 setlinewidth\n");
        size_t i = 0;
        for (cp_dict_each(_e, c->s)) {
            event_t *e = CP_BOX_OF(_e, event_t, node_s);
            cp_printf(cp_debug_ps,
                "0 %g 0 setrgbcolor\n", cp_double(i % 3) * 0.5);
            cp_debug_ps_dot(CP_PS_XY(e->p->coord), 3);
            cp_printf(cp_debug_ps,
                "newpath %g %g moveto %g %g lineto stroke\n",
                CP_PS_XY(e->p->coord),
                CP_PS_XY(e->other->p->coord));
            i++;
        }

        /* chain */
        cp_printf(cp_debug_ps, "4 setlinewidth\n");
        i = 0;
        for (cp_dict_each(_e, c->end)) {
            cp_printf(cp_debug_ps, "1 %g 0 setrgbcolor\n", cp_double(i % 3) * 0.3);
            event_t *e0 = CP_BOX_OF(_e, event_t, node_end);
            cp_debug_ps_dot(CP_PS_XY(e0->p->coord), 4);
            debug_print_chain(e0, cp_debug_ps_page_cnt);
            i++;
        }

        /* poly */
        cp_printf(cp_debug_ps, "2 setlinewidth\n");
        i = 0;
        for (cp_list_each(_e, &c->poly)) {
            cp_printf(cp_debug_ps, "0 %g 0.8 setrgbcolor\n", cp_double(i % 3) * 0.5);
            event_t *e0 = CP_BOX_OF(_e, event_t, node_poly);
            cp_debug_ps_dot(CP_PS_XY(e0->p->coord), 4);
            debug_print_chain(e0, ~cp_debug_ps_page_cnt);
            i++;
        }

        /* end page */
        cp_ps_page_end(cp_debug_ps);
    }
#endif
}

#else
#define debug_print_s(...) ((void)0)
#endif

/**
 * Compare two points
 */
static int pt_cmp(
    point_t const *a,
    point_t const *b)
{
    if (a == b) {
        return 0;
    }
    return cp_vec2_lex_pt_cmp(&a->coord, &b->coord);
}

/**
 * Compare a vec2 with a point in a dictionary.
 */
static int pt_cmp_d(
    cp_vec2_t *a,
    cp_dict_t *_b,
    void *user __unused)
{
    point_t *b = CP_BOX_OF(_b, point_t, node_pt);
    return cp_vec2_lex_pt_cmp(a, &b->coord);
}

static cp_dim_t rasterize(cp_dim_t v)
{
#if 0
    return v;
#else
    return cp_pt_epsilon * round(v / cp_pt_epsilon);
#endif
}

/**
 * Allocate a new point and remember in our point dictionary.
 *
 * This will either return a new point or one that was found already.
 */
static point_t *pt_new(
    ctxt_t *c,
    cp_loc_t loc,
    cp_vec2_t const *_coord)
{
    cp_vec2_t coord = {
       .x = rasterize(_coord->x),
       .y = rasterize(_coord->y),
    };

    /* normalise coordinates around 0 to avoid funny floats */
    if (cp_equ(coord.x, 0)) { coord.x = 0; }
    if (cp_equ(coord.y, 0)) { coord.y = 0; }

    cp_dict_ref_t ref;
    cp_dict_t *pt = cp_dict_find_ref(&ref, &coord, c->pt, pt_cmp_d, NULL, 0);
    if (pt != NULL) {
        return CP_BOX_OF(pt, point_t, node_pt);
    }

    point_t *p = CP_POOL_NEW(c->pool, *p);
    p->loc = loc;
    p->coord = coord;
    p->idx = CP_SIZE_MAX;

    LOG("new pt: %s\n", pt_str(p));

    cp_dict_insert_ref(&p->node_pt, &ref, &c->pt);
    return p;
}

/**
 * Allocate a new event
 */
static event_t *ev_new(
    ctxt_t *c,
    cp_loc_t loc,
    point_t *p,
    bool left,
    event_t *other)
{
    event_t *r = CP_POOL_NEW(c->pool, *r);
    r->loc = loc;
    r->p = p;
    r->left = left;
    r->other = other;
    return r;
}

/**
 * bottom/top compare of edge pt1--pt2 vs point pt: bottom is smaller, top is larger
 */
static inline int pt2_pt_cmp(
    point_t const *a1,
    point_t const *a2,
    point_t const *b)
{
    return cp_vec2_right_normal3_z(&a1->coord, &a2->coord, &b->coord);
}

static inline point_t *left(event_t const *ev)
{
    return ev->left ? ev->p : ev->other->p;
}

static inline point_t *right(event_t const *ev)
{
    return ev->left ? ev->other->p : ev->p;
}

/**
 * Event order in Q: generally left (small) to right (large):
 *    - left coordinates before right coordinates
 *    - bottom coordinates before top coordinates
 *    - right ends before left ends
 *    - points below an edge before points above an edge
 */
static int ev_cmp(event_t const *e1, event_t const *e2)
{
    /* Different points compare with different comparison */
    if (e1->p != e2->p) {
        int i = pt_cmp(e1->p, e2->p);
        assert((i != 0) && "Same coordinates found in different point objects");
        return i;
    }

    /* right vs left endpoint?  right comes first (= is smaller) */
    int i = e1->left - e2->left;
    if (i != 0) {
        return i;
    }

    /* same endpoint, same direction: lower edge comes first
     * Note that this might still return 0, making the events equal.
     * This is OK, it's collinear segments with the same endpoint and
     * direction.  These will be split later, processing order does
     * not matter.
     */
    return pt2_pt_cmp(left(e1), right(e1), e2->other->p);
}

/**
 * Segment order in S: generally bottom (small) to top (large)
 *
 * This was ported from a C++ Less() comparison, which seems to
 * pass the new element as second argument.  Our data structures
 * pass the new element as first argument, and in some cases,
 * this changes the order of edges (if the left end point of the
 * new edge is on an existing edge).  Therefore, we have
 * __seg_cmp() and seg_cmp() to swap arguments.
 * Well, this essentially means that this function is broken, because
 * it should hold that seg_cmp(a,b) == -seg_cmp(b,a), but it doesn't.
 * Some indications is clearly mapping -1,0,+1 to -1,-1,+1...
 */
static int __seg_cmp(event_t const *e1, event_t const *e2)
{
    /* Only left edges are inserted into S */
    assert(e1->left);
    assert(e2->left);

    if (e1 == e2) {
        return 0;
    }

    int e1_p_cmp = pt2_pt_cmp(e1->p, e1->other->p, e2->p);
    int e1_o_cmp = pt2_pt_cmp(e1->p, e1->other->p, e2->other->p);

    LOG("seg_cmp: %s vs %s: %d %d\n", ev_str(e1), ev_str(e2), e1_p_cmp, e1_o_cmp);

    if ((e1_p_cmp != 0) || (e1_o_cmp != 0)) {
        /* non-collinear */
        /* If e2->p is on e1, use right endpoint location to compare */
        if (e1_p_cmp == 0) {
            return e1_o_cmp;
        }

        /* different points */
        if (ev_cmp(e1, e2) > 0) {
            /* e2 is above e2->p? => e1 is below */
            return pt2_pt_cmp(e2->p, e2->other->p, e1->p) >= 0 ? -1 : +1;
        }

        /* e1 came first */
        return e1_p_cmp <= 0 ? -1 : +1;
    }

    /* segments are collinear. some consistent criterion is used for comparison */
    if (e1->p == e2->p) {
        return (e1 < e2) ? -1 : +1;
    }

    /* compare events */
    return ev_cmp(e1, e2);
}

static int seg_cmp(event_t const *e2, event_t const *e1)
{
    return -__seg_cmp(e1,e2);
}

/** dict version of ev_cmp for node_q */
static int ev_cmp_q(
    cp_dict_t *_e1,
    cp_dict_t *_e2,
    void *user __unused)
{
    event_t *e1 = CP_BOX_OF(_e1, event_t, node_q);
    event_t *e2 = CP_BOX_OF(_e2, event_t, node_q);
    return ev_cmp(e1, e2);
}
/** dict version of seg_cmp for node_s */
static int seg_cmp_s(
    cp_dict_t *_e1,
    cp_dict_t *_e2,
    void *user __unused)
{
    event_t *e1 = CP_BOX_OF(_e1, event_t, node_s);
    event_t *e2 = CP_BOX_OF(_e2, event_t, node_s);
    return seg_cmp(e1, e2);
}

static void q_insert(
    ctxt_t *c,
    event_t *e)
{
    assert((pt_cmp(e->p, e->other->p) < 0) == e->left);
    cp_dict_insert(&e->node_q, &c->q, ev_cmp_q, NULL, 1);
}

static void s_insert(
    ctxt_t *c,
    event_t *e)
{
    cp_dict_t *o __unused = cp_dict_insert(&e->node_s, &c->s, seg_cmp_s, NULL, 0);
    assert(o == NULL);
}

static void s_remove(
    ctxt_t *c,
    event_t *e)
{
    cp_dict_remove(&e->node_s, &c->s);
}

__unused
static void get_coord_on_line(
    cp_vec2_t *r,
    event_t *e,
    cp_vec2_t const *p)
{
    LINE_X(e,r) = LINE_X(e,p);
    LINE_Y(e,r) = e->line.b + (e->line.a * LINE_X(e,p));
}

static void q_add_orig(
    ctxt_t *c,
    cp_loc_t loc,
    cp_vec2_t *coord1,
    cp_vec2_t *coord2,
    unsigned poly_id)
{
    point_t *p1 = pt_new(c, loc, coord1);
    point_t *p2 = pt_new(c, loc, coord2);

    if (p1 == p2) {
        /* edge consisting of only one point (or two coordinates
         * closer than pt_epsilon collapsed) */
        return;
    }

    event_t *e1 = ev_new(c, loc, p1, true,  NULL);
    e1->in.owner = ((size_t)1) << poly_id;

    event_t *e2 = ev_new(c, loc, p2, false, e1);
    e2->in = e1->in;
    e1->other = e2;

    if (pt_cmp(e1->p, e2->p) > 0) {
        e1->left = false;
        e2->left = true;
    }

    /* compute origin and slope */
    cp_vec2_t d;
    d.x = e2->p->coord.x - e1->p->coord.x;
    d.y = e2->p->coord.y - e1->p->coord.y;
    e1->line.swap = cp_lt(fabs(d.x), fabs(d.y));
    e1->line.a = LINE_Y(e1, &d) / LINE_X(e1, &d);
    e1->line.b = LINE_Y(e1, &e1->p->coord) - (e1->line.a * LINE_X(e1, &e1->p->coord));
    assert(cp_leq(e1->line.a, +1));
    assert(cp_geq(e1->line.a, -1) ||
        CONFESS("a=%g (%g,%g--%g,%g)",
            e1->line.a, e1->p->coord.x, e1->p->coord.y, e2->p->coord.x, e2->p->coord.y));

    /* other direction edge is on the same line */
    e2->line = e1->line;

#ifndef NDEBUG
    /* check computation */
    cp_vec2_t g;
    get_coord_on_line(&g, e1, &e2->p->coord);
    assert(cp_vec2_equ(&g, &e2->p->coord));
    get_coord_on_line(&g, e2, &e1->p->coord);
    assert(cp_vec2_equ(&g, &e1->p->coord));
#endif

    /* Insert.  For 'equal' entries, order does not matter */
    q_insert(c, e1);
    q_insert(c, e2);
}

#ifndef NDEBUG
#  define divide_segment(c,e,p)  __divide_segment(__FILE__, __LINE__, c, e, p)
#else
#  define __divide_segment(f,l,c,e,p) divide_segment(c,e,p)
#endif

static void __divide_segment(
    char const *file __unused,
    int line __unused,
    ctxt_t *c,
    event_t *e,
    point_t *p)
{
    assert(p != e->p);
    assert(p != e->other->p);

    assert(e->left);
    event_t *o = e->other;

    assert(!cp_dict_is_member(&o->node_s));

    /*
     * Split an edge at a point p on that edge (we assume that p is correct -- no
     * check is done).
     *      p              p
     * e-------.       e--.l--.
     *  `-------o       `--r`--o
     */

    event_t *r = ev_new(c, p->loc, p, false, e);
    event_t *l = ev_new(c, p->loc, p, true,  o);

    /* relink buddies */
    o->other = l;
    e->other = r;
    assert(r->other == e);
    assert(l->other == o);

    /* copy in/out tracking -- the caller must set this up appropriately */
    r->in = e->in;
    l->in = o->in;

    /* copy edge slope and offset */
    l->line = r->line = e->line;

    /* If the middle point is rounded, the order of l and o may
     * switch.  This must not happen with e--r, because e is already
     * processed, so we'd need to go back in time to fix.
     * Any caller must make sure that p is in the correct place wrt.
     * e, in particular 'find_intersection', which computes a new point.
     */
    if (ev_cmp(l, o) > 0) {
        /* for the unprocessed part, we can fix the anomality by swapping. */
        o->left = true;
        l->left = false;
    }

    /* For e--r, we cannot handle this case here: blame the caller. */
    assert((ev_cmp(e,r) < 0) ||
        CONFESS("%s:%d:\n\tp=%s\n\te=%s\n\tl=%s", file, line, pt_str(p), ev_str(e), ev_str(l)));

    /* handle new events later */
    q_insert(c, l);
    q_insert(c, r);
}

/**
 * Compare two nodes for insertion into c->end.
 * For correct insertion order (selection of end node for
 * comparison), be sure to connect the node before
 * inserting.
 */
static int pt_cmp_end_d(
    cp_dict_t *_a,
    cp_dict_t *_b,
    void *user __unused)
{
    event_t *a = CP_BOX_OF(_a, event_t, node_end);
    event_t *b = CP_BOX_OF(_b, event_t, node_end);
    return pt_cmp(a->p, b->p);
}

/**
 * Try to insert a node into a the polygon chain end store.
 * If a duplicate is found, extract and return it instead of inserting e.
 */
static event_t *chain_insert_or_extract(
    ctxt_t *c,
    event_t *e)
{
    LOG("insert %s\n", ev_str(e));
    cp_dict_t *_r = cp_dict_insert(&e->node_end, &c->end, pt_cmp_end_d, NULL, 0);
    if (_r == NULL) {
        return NULL;
    }
    cp_dict_remove(_r, &c->end);
    return CP_BOX_OF(_r, event_t, node_end);
}

/**
 * Connect an edge e to a polygon point o1 that may already be
 * connected to more points.
 */
static void chain_join(
    event_t *o1,
    event_t *e)
{
    LOG("join   %s with %s\n", ev_str(o1), ev_str(e));
    assert(cp_ring_is_end(&o1->node_chain));
    assert(cp_ring_is_end(&e->node_chain));
    cp_ring_join(&o1->node_chain, &e->node_chain);
}

/**
 * Insert into polygon output list */
static void poly_add(
    ctxt_t *c,
    event_t *e)
{
    LOG("poly   %s\n", ev_str(e));
    assert(!cp_dict_is_member(&e->node_q));
    assert(!cp_dict_is_member(&e->node_end));
    cp_list_init(&e->node_poly);
    cp_list_insert(&c->poly, &e->node_poly);
}

/**
 * Add a point to a path.
 * If necessary, allocate a new point */
static void path_add_point(
    cp_csg2_poly_t *r,
    cp_csg2_path_t *p,
    event_t *e)
{
    assert(!cp_ring_is_end(&e->node_chain) && "Polygon chain is too short or misformed");

    /* mark event used in polygon */
    assert(!e->used);
    e->used = true;

    /* possibly allocate a point */
    size_t idx = e->p->idx;
    if (idx == CP_SIZE_MAX) {
        cp_vec2_loc_t *v = cp_v_push0(&r->point);
        e->p->idx = idx = cp_v_idx(&r->point, v);
        v->coord = e->p->coord;
        v->loc = e->p->loc;
    }
    assert(idx < r->point.size);

    /* append point to path */
    cp_v_push(&p->point_idx, idx);
}

/**
 * Construct the poly from the chains */
static void path_make(
    ctxt_t *c __unused,
    cp_csg2_poly_t *r,
    cp_csg2_path_t *p,
    event_t *e0)
{
    assert(p->point_idx.size == 0);
    event_t *ex = CP_BOX_OF(cp_ring_step(&e0->node_chain, 0), event_t, node_chain);
    event_t *e1 = CP_BOX_OF(cp_ring_step(&e0->node_chain, 1), event_t, node_chain);

    /* make it so that e1 equals e0->other, and ex is the other end */
    assert((e1->p == e0->other->p) || (ex->p == e0->other->p));
    if (ex->p == e0->other->p) {
        /* for some reason, none of my tests triggers this, but I cannot see
         * why it couldn't happen */
        CP_SWAP(&e1, &ex);
    }
    assert(e1->p == e0->other->p);

    /* Four cases that collapse to two (no need to check whether e1 or ex is above):
     * If e0-e1 is below e0-ex, and e0->in.below==0, then move along e0->e1.
     * If e0-e1 is above e0-ex, and e0->in.below==1, then move along e1->e0.
     * If e0-e1 is below e0-ex, and e0->in.below==1, then move along e1->e0.
     * If e0-e1 is above e0-ex, and e0->in.below==0, then move along e0->e1.
     */
    if (e0->in.below) {
        CP_SWAP(&e0, &e1);
    }

    /* first and second point */
    path_add_point(r, p, e0);
    path_add_point(r, p, e1);
    for (cp_ring_each(_ei, &e0->node_chain, &e1->node_chain)) {
        event_t *ei = CP_BOX_OF(_ei, event_t, node_chain);
        path_add_point(r, p, ei);
    }

    assert((p->point_idx.size >= 3) && "Polygon chain is too short");
}

/**
 * Construct the poly from the chains */
static void poly_make(
    cp_csg2_poly_t *r,
    ctxt_t *c,
    cp_loc_t loc)
{
    CP_ZERO(r);
    cp_csg2_init((cp_csg2_t*)r, CP_CSG2_POLY, loc);

    assert((c->end == NULL) && "Some poly chains are still open");

    for (cp_list_each(_e, &c->poly)) {
        event_t *e = CP_BOX_OF(_e, event_t, node_poly);
        if (!e->used) {
            cp_csg2_path_t *p = cp_v_push0(&r->path);
            path_make(c, r, p, e);
        }
    }
}

/**
 * Add an edge to the output edge.  Only right events are added.
 */
static void chain_add(
    ctxt_t *c,
    event_t *e)
{
    LOG("out:   %s (%p)\n", ev_str(e), e);

    /* the event should left and neither point should be s or q */
    assert(!e->left);
    assert(pt_cmp(e->p, e->other->p) >= 0);
    assert(!cp_dict_is_member(&e->node_s));
    assert(!cp_dict_is_member(&e->node_q));
    assert(!cp_dict_is_member(&e->other->node_s));
    assert(!cp_dict_is_member(&e->other->node_q));

    cp_ring_init(&e->node_chain);
    cp_ring_init(&e->other->node_chain);

    /*
     * This algorithm combines output edges into a polygon ring.  We
     * know that the events come in from left (bottom) to right (top),
     * i.e., we have a definitive direction.  Only right points are
     * added.
     *
     * Edges are inserted by their next connection point that will
     * come in into the c->end using node_end.  Partial polygon chains
     * consisting of more than one point will have both ends in that
     * set.
     *
     * The first edge of a new polygon is added by its left point,
     * because we know that the next connection will be to that
     * point.  Once an edge is connected, its right point will be
     * inserted because its left point is already connected, so it cannot
     * be connected again.
     *
     * A new edge first searches c->end by its left point to find a place to
     * attach.  If found, that point is extracted from c->end, connected to
     * the new edge using (using node_chain), and the new edge is inserted
     * by its right point, waiting for another edge to connect.
     *
     * If another point with the same coordinates is found when trying
     * to insert an edge, that node is extracted from c->end, the two
     * ends are connected and the new edge is not inserted because
     * both ends are connected.  This may or may not close a polygon
     * complete.
     *
     * To gather polygons, once an edge connects two ends, it is inserted
     * into the list c->poly using the node node_poly.  Polygons may have
     * multiple nodes in this list, but an O(n) search is necessary to
     * output them anyway, so this does not hurt.
     *
     * For connecting nodes, the ring data structure is used so that
     * the order by which chains are linked does not matter -- we
     * might otherwise end up trying to connect chains of opposing
     * direction.  Rings handle this, plus our implementation supports
     * 'end' nodes, which our list implementation does not.
     *
     * In total, this takes no extra space except for c->end and c->poly,
     * and takes O(n log n) time with n edges found by the algorithm.
     */

    /* Find the left point in the end array.  Note: we search by
     * 'e->other->p', while we will insert by 'e->p'. */
    assert(e->other->left);
    event_t *o1 = chain_insert_or_extract(c, e->other);
    event_t *o2 = chain_insert_or_extract(c, e);

    switch ((o1 != NULL) | ((o2 != NULL) << 1)) {
    case 0: /* none found: new chain */
        /* connect left and right point to make initial pair */
        e->p->path_cnt++;
        e->other->p->path_cnt++;
        chain_join(e, e->other);
        assert(cp_ring_is_pair(&e->node_chain, &e->other->node_chain));
        break;

    case 3: /* both found: closed */
        /* close chain */
        chain_join(o1, o2);
        assert(!cp_ring_is_end(&o1->node_chain));
        assert(!cp_ring_is_end(&o2->node_chain));
        /* put in poly list */
        poly_add(c, o2);
        /* At this point, from o2->in.below and the z normal of the triangle
         * o1, o2, o2->other, we can derive what is inside and what is
         * outside. */
        assert(o1 != o2->other);
        break;

    case 1: /* o1 found, o2 not found: connect */
        e->p->path_cnt++;
        chain_join(o1, e);
        assert(!cp_ring_is_end(&o1->node_chain));
        assert( cp_ring_is_end(&e->node_chain));
        break;

    case 2: /* o2 found, o1 not found: connect */
        e->other->p->path_cnt++;
        chain_join(o2, e->other);
        assert(!cp_ring_is_end(&o2->node_chain));
        assert( cp_ring_is_end(&e->other->node_chain));
        break;
    }
}

static void intersection_add_ev(
    event_t **sev,
    size_t *sev_cnt,
    event_t *e1,
    event_t *e2)
{
    if (e1->p == e2->p) {
        sev[(*sev_cnt)++] = NULL;
    }
    else if (ev_cmp(e1, e2) > 0) {
        sev[(*sev_cnt)++] = e2;
        sev[(*sev_cnt)++] = e1;
    }
    else {
        sev[(*sev_cnt)++] = e1;
        sev[(*sev_cnt)++] = e2;
    }
}

static void intersection_point(
    cp_vec2_t *r,
    cp_f_t ka, cp_f_t kb, bool ks,
    cp_f_t ma, cp_f_t mb, bool ms)
{
    if (fabs(ka) < fabs(ma)) {
        CP_SWAP(&ka, &ma);
        CP_SWAP(&kb, &mb);
        CP_SWAP(&ks, &ms);
    }
    /* ka is closer to +-1 than ma; ma is closer to 0 than ka */

    if (ks != ms) {
        if (cp_equ(ma,0)) {
            _LINE_X(ks,r) = mb;
            _LINE_Y(ks,r) = (ka * mb) + kb;
            return;
        }
        /* need to switch one of the two into opposite axis.  better do this
         * with ka/kb/ks, because we're closer to +-1 there */
        assert(!cp_equ(ka,0));
        ka = 1/ka;
        kb *= -ka;
        ks = ms;
    }

    assert(!cp_equ(ka, ma) && "parallel lines should be handled in find_intersection, not here");
    assert((ks == ms) || cp_equ(ma,0));
    double q = (mb - kb) / (ka - ma);
    _LINE_X(ks,r) = q;
    _LINE_Y(ks,r) = (ka * q) + kb;
}

static bool dim_between(cp_dim_t a, cp_dim_t b, cp_dim_t c)
{
    return (a < c) ? (cp_leq(a,b) && cp_leq(b,c)) : (cp_geq(a,b) && cp_geq(b,c));
}

/**
 * Returns:
 *
 * non-NULL:
 *     single intersection point within segment bounds
 *
 * NULL:
 *     *collinear == false:
 *         parallel
 *
 *     *collinear == true:
 *         collinear, but not tested for actual overlapping
 */
static point_t *find_intersection(
    bool *collinear,
    ctxt_t *c,
    event_t *e0,
    event_t *e1)
{
    assert(e0->left);
    assert(e1->left);

    *collinear = false;

    point_t *p0  = e0->p;
    point_t *p0b = e0->other->p;
    point_t *p1  = e1->p;
    point_t *p1b = e1->other->p;

    /* Intersections are always calculated from the original input data so that
     * no errors add up. */

    /* parallel/collinear? */
    if ((e0->line.swap == e1->line.swap) && cp_equ(e0->line.a, e1->line.a)) {
        /* properly parallel? */
        *collinear = cp_equ(e0->line.b, e1->line.b);
        return NULL;
    }

    /* get intersection point */
    cp_vec2_t i;
    intersection_point(
        &i,
        e0->line.a, e0->line.b, e0->line.swap,
        e1->line.a, e1->line.b, e1->line.swap);

    cp_vec2_t i_orig = i;
    i.x = rasterize(i.x);
    i.y = rasterize(i.y);

    /* check whether i is on e0 and e1 */
    if (!dim_between(p0->coord.x, i.x, p0b->coord.x) ||
        !dim_between(p0->coord.y, i.y, p0b->coord.y) ||
        !dim_between(p1->coord.x, i.x, p1b->coord.x) ||
        !dim_between(p1->coord.y, i.y, p1b->coord.y))
    {
        return NULL;
    }

    /* Now possibly move the new point so that the relationship between
     * eX->p and i remains the same as between eX->p and eX->other->p.
     * If the relationship changes, we are probably very close to a vertical
     * line, so increase i.x.   This needs to be done before hashing the
     * point using pt_new.  Other parts of the code rely on the fact that this
     * is done here, because it may not be changable later.  We only need to do
     * this if i is close to the left point, because the right ones are
     * not inserted yet. */
    int cmp_p0_i = cp_vec2_lex_pt_cmp(&p0->coord, &i);
    if (cmp_p0_i == 0) {
        return p0;
    }
    assert(cp_vec2_lex_pt_cmp(&p0->coord, &p0b->coord) < 0);
    if (cmp_p0_i > 0) {
        i.x = rasterize(i_orig.x + 1.5*cp_pt_epsilon);
    }
    assert((cp_vec2_lex_pt_cmp(&p0->coord, &p0b->coord) == cp_vec2_lex_pt_cmp(&p0->coord, &i)) ||
        CONFESS("e0=%s, i=%s", ev_str(e0), coord_str(&i)));

    /* same fixing for other edge */
    int cmp_p1_i = cp_vec2_lex_pt_cmp(&p1->coord, &i);
    if (cmp_p1_i == 0) {
        return p1;
    }
    assert(cp_vec2_lex_pt_cmp(&p1->coord, &p1b->coord) < 0);
    if (cmp_p1_i > 0) {
        i.x = rasterize(i_orig.x + 1.5*cp_pt_epsilon);
    }
    assert((cp_vec2_lex_pt_cmp(&p1->coord, &p1b->coord) == cp_vec2_lex_pt_cmp(&p1->coord, &i)) ||
        CONFESS("e1=%s, i=%s", ev_str(e1), coord_str(&i)));

    /* Finally, make a new point (or an old point -- pt_new will check whether we have
     * this already) */
    return pt_new(c, p0->loc, &i);
}

static bool coord_between(
    cp_vec2_t const *a,
    cp_vec2_t const *b,
    cp_vec2_t const *c)
{
    if (!dim_between(a->x, b->x, c->x)) {
        return false;
    }
    if (!dim_between(a->y, b->y, c->y)) {
        return false;
    }
    cp_dim_t dx = c->x - a->x;
    cp_dim_t dy = c->y - a->y;
    if (fabs(dx) > fabs(dy)) {
        assert(!cp_pt_equ(a->x, c->x));
        cp_dim_t t = (b->x - a->x) / dx;
        cp_dim_t y = a->y + (t * dy);
        return cp_pt_equ(y, b->y);
    }
    else {
        assert(!cp_pt_equ(a->y, c->y));
        cp_dim_t t = (b->y - a->y) / dy;
        cp_dim_t x = a->x + (t * dx);
        return cp_pt_equ(x, b->x);
    }
}

static bool pt_between(
    point_t const *a,
    point_t const *b,
    point_t const *c)
{
    if (a == b) {
        return true;
    }
    if (b == c) {
        return true;
    }
    if (a == b) {
        return false;
    }
    return coord_between(&a->coord, &b->coord, &c->coord);
}

static bool ev4_overlap(
    event_t *el,
    event_t *ol,
    event_t *eh,
    event_t *oh)
{
    /*
     * The following cases exist:
     * (1) el........ol        (6) eh........oh
     *          eh...oh                 el...ol
     *
     * (2) el........ol        (7) eh........oh
     *     eh...oh                 el...ol
     *
     * (3) el........ol        (8) eh........oh
     *        eh..oh                  el..ol
     *
     * (4) el........ol        (9) eh........oh
     *          eh........oh            el........ol
     *
     * We do not care about the following ones, because they need
     * a collinearity check anyway (i.e., these must return false):
     *
     * (5) el...ol            (10) eh...oh
     *          eh...oh                 el...ol
     */
    if (pt_between(el->p, eh->p, ol->p)) { /* (1),(2),(3),(4),(5),(7) */
        if (pt_between(el->p, oh->p, ol->p)) { /* (1),(2),(3) */
            return true;
        }
        if (pt_between(eh->p, ol->p, oh->p)) { /* (4),(5) */
            return ol != eh; /* exclude (5) */
        }
        /* (7) needs to be checked, so no 'return false' here */
    }

    if (pt_between(eh->p, el->p, oh->p)) { /* (2),(6),(7),(8),(9),(10) */
        if (pt_between(eh->p, ol->p, oh->p)) { /* (6),(7),(8) */
            return true;
        }
        if (pt_between(el->p, oh->p, ol->p)) { /* (9),(10) */
            return oh != el;
        }
    }

    return false;
}

static void check_intersection(
    ctxt_t *c,
    /** the lower edge in s */
    event_t *el,
    /** the upper edge in s */
    event_t *eh)
{
    event_t *ol = el->other;
    event_t *oh = eh->other;
    assert( el->left);
    assert( eh->left);
    assert( cp_dict_is_member(&el->node_s));
    assert( cp_dict_is_member(&eh->node_s));
    assert(!ol->left);
    assert(!oh->left);
    assert(!cp_dict_is_member(&ol->node_s));
    assert(!cp_dict_is_member(&oh->node_s));

    /* A simple comparison of line.a to decide about overlap will not work, i.e.,
     * because the criterion needs to be consistent with point coordinate comparison,
     * otherwise we may run into problems elsewhere.  I.e., we cannot first check for
     * collinearity and only then check for overlap.  But we need to base the
     * decision of overlap on point coordinate comparison.  So we will first try
     * for overlap, then we'll try to find a proper intersection point.
     * 'find_intersection' will, therefore, not have to deal with the case of overlap.
     * If the edges are collinear (e.g., based on an line.a criterion), it will mean
     * that the lines are paralllel or collinear but with a gap in between, i.e., they
     * will not overlap.
     *
     * The whole 'overlap' check explicitly does not use the 'normal_z' or 'line.a'
     * checks to really base this on cp_pt_equ().
     *
     * Now, if el and eh are indeed overlapping, Whether el or eh is the 'upper' edge
     * may have been decided based on a rounding error, so either case must be handled
     * correctly.
     */

    if (!ev4_overlap(el, ol, eh, oh)) {
        bool collinear;
        point_t *ip = find_intersection(&collinear, c, el, eh);

        LOG("#intersect = %p %u\n", ip, collinear);

        if (ip != NULL) {
            /* If the lines meet in one point, it's ok */
            if ((el->p == eh->p) || (ol->p == oh->p)) {
                return;
            }

            if (ip == el->p) {
                /* This means that we need to reclassify the upper line again (which
                 * we thought was below, but due to rounding, it now turns out to be
                 * completely above).  The easiest is to remove it again from S
                 * and through it back into Q to try again later. */
                s_remove(c, el);
                q_insert(c, el);
            }
            else if (ip != ol->p) {
                divide_segment(c, el, ip);
            }

            if (ip == eh->p) {
                /* Same corder case as above: we may have classified eh too early. */
                s_remove(c, eh);
                q_insert(c, eh);
            }
            else if (ip != oh->p) {
                divide_segment(c, eh, ip);
            }

            return;
        }

        if (!collinear) {
            return;
        }
        assert(0);
    }

    /* check */
    assert(pt_cmp(el->p, ol->p) < 0);
    assert(pt_cmp(eh->p, oh->p) < 0);
    assert(pt_cmp(ol->p, eh->p) >= 0);
    assert(pt_cmp(oh->p, el->p) >= 0);

    /* overlap */
    event_t *sev[4];
    size_t sev_cnt = 0;
    intersection_add_ev(sev, &sev_cnt, el, eh);
    intersection_add_ev(sev, &sev_cnt, ol, oh);
    assert(sev_cnt >= 2);
    assert(sev_cnt <= cp_countof(sev));

    size_t owner = (eh->in.owner ^ el->in.owner);
    size_t below = el->in.below;
    size_t above = below ^ owner;

    /* We do not need to care about resetting other->in.below, because it is !left
     * and is not part of S yet, and in.below will be reset upon insertion. */
    if (sev_cnt == 2) {
        /*  eh.....oh
         *  el.....ol
         */
        assert(sev[0] == NULL);
        assert(sev[1] == NULL);
        eh->in.owner = oh->in.owner = owner;
        eh->in.below = below;

        el->in.owner = ol->in.owner = 0;
        assert(el->in.below == below);

        return;
    }
    if (sev_cnt == 3) {
        /* sev:  0    1    2
         *       eh........NULL    ; sh == eh, shl == eh
         *            el...NULL
         * OR
         *            eh...NULL
         *       el........NULL    ; sh == el, shl == el
         * OR
         *     NULL........oh      ; sh == oh, shl == eh
         *     NULL...ol
         * OR
         *     NULL...oh
         *     NULL........ol      ; sh == ol, shl == el
         */
        assert(sev[1] != NULL);
        assert((sev[0] == NULL) || (sev[2] == NULL));

        /* ignore the shorter one */
        sev[1]->in.owner = sev[1]->other->in.owner = 0;

        /* split the longer one, marking the double side as overlapping: */
        event_t *sh  = sev[0] ?: sev[2];
        event_t *shl = sev[0] ?: sev[2]->other;
        sh->other->in.owner = owner;
        sh->other->in.below = below;
        if (shl == el) {
            assert((sev[1] == eh) || (sev[1] == oh));
            eh->in.below = above;
        }

        divide_segment(c, shl, sev[1]->p);
        return;
    }

    assert(sev_cnt == 4);
    assert(sev[0] != NULL);
    assert(sev[1] != NULL);
    assert(sev[2] != NULL);
    assert(sev[3] != NULL);
    assert(
        ((sev[0] == el) && (sev[1] == eh)) ||
        ((sev[0] == eh) && (sev[1] == el)));
    assert(
        ((sev[2] == ol) && (sev[3] == oh)) ||
        ((sev[2] == oh) && (sev[3] == ol)));

    if (sev[0] != sev[3]->other) {
        /*        0   1   2   3
         *            eh......oh
         *        el......ol
         * OR:
         *        eh......oh
         *            el......ol
         */
        assert(
            ((sev[0] == el) && (sev[1] == eh) && (sev[2] == ol) && (sev[3] == oh)) ||
            ((sev[0] == eh) && (sev[1] == el) && (sev[2] == oh) && (sev[3] == ol)));

        sev[1]->in.owner = 0;
        if (sev[1] == eh) {
            sev[1]->in.below = above;
        }
        sev[2]->in.owner = owner;
        sev[2]->in.below = below;

        divide_segment(c, sev[0], sev[1]->p);
        divide_segment(c, sev[1], sev[2]->p);
        return;
    }

    /*        0   1   2   3
     *            eh..oh
     *        el..........ol
     * OR:
     *        eh..........oh
     *            el..ol
     */
    assert(
        ((sev[0] == el) && (sev[1] == eh) && (sev[2] == oh) && (sev[3] == ol)) ||
        ((sev[0] == eh) && (sev[1] == el) && (sev[2] == ol) && (sev[3] == oh)));
    assert(sev[1]->other == sev[2]);

    sev[1]->in.owner = sev[2]->in.owner = 0;
    if (sev[1] == eh) {
        sev[1]->in.below = sev[2]->in.below = above;
    }
    divide_segment(c, sev[0], sev[1]->p);

    sev[3]->other->in.owner = owner;
    sev[3]->other->in.below = below;
    divide_segment(c, sev[3]->other, sev[2]->p);
}

static inline event_t *s_next(
    event_t *e)
{
    if (e == NULL) {
        return NULL;
    }
    return CP_BOX0_OF(cp_dict_next(&e->node_s), event_t, node_s);
}

static inline event_t *s_prev(
    event_t *e)
{
    if (e == NULL) {
        return NULL;
    }
    return CP_BOX0_OF(cp_dict_prev(&e->node_s), event_t, node_s);
}

static void ev_left(
    ctxt_t *c,
    event_t *e)
{
    assert(!cp_dict_is_member(&e->node_s));
    assert(!cp_dict_is_member(&e->other->node_s));
    LOG("insert_s: %p (%p)\n", e, e->other);
    s_insert(c, e);

    event_t *prev = s_prev(e);
    assert(e->left);
    assert((prev == NULL) || prev->left);

    if (prev == NULL) {
        /* should be set up correctly from q phase */
        e->in.below = 0;
    }
    else {
        /* use previous edge's above for this edge's below info */
        e->in.below = prev->in.below ^ prev->in.owner;
    }

    debug_print_s(c, "left after insert", e);

    event_t *next = s_next(e);
    if (next != NULL) {
        check_intersection(c, e, next);
    }
    /* The previous 'check_intersection' may have kicked out 'e' from S due
     * to rounding, so check that e is still in S before trying to intersect.
     * If not, it is back in Q and we'll handle this later. */
    if ((prev != NULL) && cp_dict_is_member(&e->node_s)) {
        check_intersection(c, prev, e);
    }

    debug_print_s(c, "left after intersect", e);
}

static bool odd_parity(size_t s)
{
    s ^= s << 1;
    s ^= s << 2;
    s ^= s << 4;
    s ^= s << 8;
    s ^= s << 16;
#if __SIZEOF_POINTER__ == 8
    s ^= s << 32;
#endif
    return s & 1;
}

static void ev_right(
    ctxt_t *c,
    event_t *e)
{
    event_t *sli = e->other;
    event_t *next = s_next(sli);
    event_t *prev = s_prev(sli);

    /* first remove from s */
    LOG("remove_s: %p (%p)\n", e->other, e);
    s_remove(c, sli);
    assert(!cp_dict_is_member(&e->node_s));
    assert(!cp_dict_is_member(&e->other->node_s));

    /* now add to out */
    /*
     * xor is done: popcount(owner)&1 ==  1
     *
     * Others are done by comparing below and above masks: if both lead to different
     * results about in/out, then we need this edge.
     *
     *                         ~(ab)==00   ab!=00   ~(ab)^1==00   popcount(owner)&1==1
     *    ab  ~(ab)  ~(ab)^1   a&b         a|b      a&~b          xor
     *    00  11     10        0           0        0
     *    01  10     11        0           1        0
     *    10  01     00        0           1        1
     *    11  00     01        1           1        0
     */
    size_t below = sli->in.below;
    size_t above = sli->in.below ^ sli->in.owner;
    bool below_in = false;
    bool above_in = false;
    switch (c->op) {
    case CP_OP_ADD:
        below_in = (below != 0);
        above_in = (above != 0);
        break;

    case CP_OP_CUT:
    case CP_OP_SUB:
        below_in = ((below ^ c->mask_neg ^ c->mask_all) == 0);
        above_in = ((above ^ c->mask_neg ^ c->mask_all) == 0);
        break;

    case CP_OP_XOR:
        below_in = odd_parity(below);
        above_in = odd_parity(above);
        break;
    }
    if (below_in != above_in) {
        e->in.below = e->other->in.below = below_in;
        chain_add(c, e);
    }

    if ((next != NULL) && (prev != NULL)) {
        check_intersection(c, prev, next);
    }

    debug_print_s(c, "right after intersect", e);
}

static inline event_t *q_extract_min(ctxt_t *c)
{
    return CP_BOX0_OF(cp_dict_extract_min(&c->q), event_t, node_q);
}

extern void cp_csg2_op_poly(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_poly_t *r,
    cp_loc_t loc,
    cp_csg2_poly_t *a,
    cp_csg2_poly_t *b,
    cp_bool_op_t op)
{
    TRACE("#a.path=%"_Pz"u #b.path=%"_Pz"u", a->path.size, b->path.size);

#if OPT >= 1
    /* trivial case: empty polygon */
    if ((a->path.size == 0) || (b->path.size == 0)) {
        LOG("one polygon is empty\n");
        switch (op) {
        case CP_OP_CUT:
            return;

        case CP_OP_SUB:
            CP_SWAP(r, a);
            return;

        case CP_OP_ADD:
        case CP_OP_XOR:
            CP_SWAP(r, a->path.size == 0 ? b : a);
            return;
        }
        CP_DIE("Unrecognised operation");
    }
#endif

    /* make context */
    ctxt_t c = {
        .pool = pool,
        .err = t,
        .op = op,
        .bb = { CP_MINMAX_EMPTY, CP_MINMAX_EMPTY },
        .mask_all = 3,
        .mask_neg = (op == CP_OP_SUB) ? 2 : 0,
    };
    cp_list_init(&c.poly);

    /* trivial case: bounding box does not overlap */
    cp_csg2_poly_minmax(&c.bb[0], a);
    cp_csg2_poly_minmax(&c.bb[1], b);
    c.minmaxx = cp_min(c.bb[0].max.x, c.bb[1].max.x);

#if OPT >= 2
    if (cp_gt(c.bb[0].min.x, c.bb[1].max.x) ||
        cp_gt(c.bb[1].min.x, c.bb[0].max.x) ||
        cp_gt(c.bb[0].min.y, c.bb[1].max.y) ||
        cp_gt(c.bb[1].min.y, c.bb[0].max.y))
    {
        LOG("bounding boxes do not overlap: copy\n");
        switch (op) {
        case CP_OP_CUT:
            return;

        case CP_OP_SUB:
            CP_SWAP(r, a);
            return;

        case CP_OP_ADD:
        case CP_OP_XOR:
            CP_SWAP(r, a);
            cp_csg2_poly_merge(r, b);
            return;
        }
        assert(0 && "Unrecognised operation");
    }
#endif

    /* initialise queue */
    LOG("poly 0: #path=%"_Pz"u\n", a->path.size);
    for (cp_v_each(i, &a->path)) {
        cp_csg2_path_t *p = &cp_v_nth(&a->path, i);
        for (cp_v_each(j, &p->point_idx)) {
            cp_vec2_loc_t *pj = cp_csg2_path_nth(a, p, j);
            cp_vec2_loc_t *pk = cp_csg2_path_nth(a, p, cp_wrap_add1(j, p->point_idx.size));
            q_add_orig(&c, pj->loc, &pj->coord, &pk->coord, 0);
        }
    }
    LOG("poly 1: #path=%"_Pz"u\n", b->path.size);
    for (cp_v_each(i, &b->path)) {
        cp_csg2_path_t *p = &cp_v_nth(&b->path, i);
        for (cp_v_each(j, &p->point_idx)) {
            cp_vec2_loc_t *pj = cp_csg2_path_nth(b, p, j);
            cp_vec2_loc_t *pk = cp_csg2_path_nth(b, p, cp_wrap_add1(j, p->point_idx.size));
            q_add_orig(&c, pj->loc, &pj->coord, &pk->coord, 1);
        }
    }
    LOG("start\n");

    /* run algorithm */
    size_t ev_cnt __unused = 0;
    for (;;) {
        event_t *e = q_extract_min(&c);
        if (e == NULL) {
            break;
        }

        LOG("\nevent %"_Pz"u: %s o=(%#"_Pz"x %#"_Pz"x)\n",
            ++ev_cnt,
            ev_str(e),
            e->other->in.owner,
            e->other->in.below);

#if OPT >= 3
        /* trivial: all the rest is cut away */
        if ((op == CP_OP_CUT && cp_pt_gt(e->p->coord.x, c.minmaxx)) ||
            (op == CP_OP_SUB && cp_pt_gt(e->p->coord.x, c.bb[0].max.x)))
        {
            break;
        }
#endif

#if OPT >= 4
        /* trivial: nothing more to merge */
        if ((op == CP_OP_ADD && cp_pt_gt(e->p->coord.x, c.minmaxx))) {
            if (!e->left) {
                CP_ZERO(&e->node_s);
                CP_ZERO(&e->other->node_s);
                chain_add(&c, e);
            }
            while ((e = q_extract_min(&c)) != NULL) {
                if (!e->left) {
                    CP_ZERO(&e->node_s);
                    CP_ZERO(&e->other->node_s);
                    chain_add(&c, e);
                }
            }
            break;
        }
#endif

        /* do real work on event */
        if (e->left) {
            ev_left(&c, e);
        }
        else {
            ev_right(&c, e);
        }
    }

    poly_make(r, &c, loc);
}

static bool csg2_op_poly(
    cp_csg2_poly_t *o,
    cp_csg2_poly_t *a)
{
    TRACE();
    CP_SWAP(o, a);
    return true;
}

static bool csg2_op_csg2(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    size_t zi,
    cp_csg2_poly_t *o,
    cp_csg2_t *a);

#define AUTO_POLY(oi, loc) \
    cp_csg2_poly_t oi; \
    CP_ZERO(&oi); \
    cp_csg2_init((cp_csg2_t*)&oi, CP_CSG2_POLY, loc);

static bool csg2_op_v_csg2(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    size_t zi,
    cp_csg2_poly_t *o,
    cp_loc_t loc,
    cp_v_csg2_p_t *a)
{
    TRACE("n=%"_Pz"u", a->size);
    for (cp_v_each(i, a)) {
        cp_csg2_t *ai = cp_v_nth(a,i);

        if (i == 0) {
            if (!csg2_op_csg2(pool, t, r, zi, o, ai)) {
                return false;
            }
        }
        else {
            AUTO_POLY(oi, ai->loc);
            if (!csg2_op_csg2(pool, t, r, zi, &oi, ai)) {
                return false;
            }

            cp_csg2_op_poly(pool, t, o, loc, o, &oi, CP_OP_ADD);
        }
    }
    return true;
}

static bool csg2_op_add(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    size_t zi,
    cp_csg2_poly_t *o,
    cp_csg2_add_t *a)
{
    TRACE();
    return csg2_op_v_csg2(pool, t, r, zi, o, a->loc, &a->add);
}

static bool csg2_op_layer(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    cp_csg2_poly_t *o,
    cp_csg2_layer_t *a)
{
    TRACE();
    return csg2_op_add(pool, t, r, a->zi, o, &a->root);
}

static bool csg2_op_sub(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    size_t zi,
    cp_csg2_poly_t *o,
    cp_csg2_sub_t *a)
{
    TRACE();
    if (!csg2_op_add(pool, t, r, zi, o, &a->add)) {
        return false;
    }

    AUTO_POLY(os, a->sub.loc);
    if (!csg2_op_add(pool, t, r, zi, &os, &a->sub)) {
        return false;
    }

    cp_csg2_op_poly(pool, t, o, a->loc, o, &os, CP_OP_SUB);
    return true;
}

static bool csg2_op_cut(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    size_t zi,
    cp_csg2_poly_t *o,
    cp_csg2_cut_t *a)
{
    TRACE();
    for (cp_v_each(i, &a->cut)) {
        cp_csg2_add_t *b = cp_v_nth(&a->cut, i);
        if (i == 0) {
            if (!csg2_op_add(pool, t, r, zi, o, b)) {
                return false;
            }
        }
        else {
            AUTO_POLY(oc, b->loc);
            if (!csg2_op_add(pool, t, r, zi, &oc, b)) {
                return false;
            }
            cp_csg2_op_poly(pool, t, o, b->loc, o, &oc, CP_OP_CUT);
        }
    }

    return true;
}

static bool csg2_op_stack(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    size_t zi,
    cp_csg2_poly_t *o,
    cp_csg2_stack_t *a)
{
    TRACE();

    cp_csg2_layer_t *l = cp_csg2_stack_get_layer(a, zi);
    if (l == NULL) {
        return true;
    }
    if (zi != l->zi) {
        assert(l->zi == 0); /* not visited: must be empty */
        return true;
    }

    assert(zi == l->zi);
    return csg2_op_layer(pool, t, r, o, l);
}

static bool csg2_op_csg2(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    size_t zi,
    cp_csg2_poly_t *o,
    cp_csg2_t *a)
{
    TRACE();
    switch (a->type) {
    case CP_CSG2_CIRCLE:
        CP_NYI("circle");
        return false;

    case CP_CSG2_POLY:
        return csg2_op_poly(o, cp_csg2_poly(a));

    case CP_CSG2_ADD:
        return csg2_op_add(pool, t, r, zi, o, cp_csg2_add(a));

    case CP_CSG2_SUB:
        return csg2_op_sub(pool, t, r, zi, o, cp_csg2_sub(a));

    case CP_CSG2_CUT:
        return csg2_op_cut(pool, t, r, zi, o, cp_csg2_cut(a));

    case CP_CSG2_STACK:
        return csg2_op_stack(pool, t, r, zi, o, cp_csg2_stack(a));
    }

    CP_DIE("2D object type");
    return false;
}

extern bool cp_csg2_op_add_layer(
    cp_pool_t *pool,
    cp_err_t *t,
    cp_csg2_tree_t *r,
    cp_csg2_tree_t *a,
    size_t zi)
{
    TRACE();
    cp_csg2_stack_t *s = cp_csg2_stack(r->root);
    assert(zi < s->layer.size);

    AUTO_POLY(o, NULL);
    if (!csg2_op_csg2(pool, t, r, zi, &o, a->root)) {
        return false;
    }
    LOG("#o.point: %"_Pz"u\n", o.point.size);

    if (o.point.size > 0) {
        /* new layer */
        cp_csg2_layer_t *layer = cp_csg2_stack_get_layer(s, zi);
        assert(layer != NULL);
        cp_csg2_add_init_perhaps(&layer->root, NULL);

        layer->zi = zi;

        cp_v_nth(&r->flag, zi) |= CP_CSG2_FLAG_NON_EMPTY;

        /* use new polygon */
        cp_csg2_t *_o2 = cp_csg2_new(CP_CSG2_POLY, NULL);
        cp_csg2_poly_t *o2 = &_o2->poly;
        CP_SWAP(&o, o2);

        /* single polygon per layer */
        cp_v_push(&layer->root.add, _o2);
    }

    return true;
}

extern void cp_csg2_op_tree_init(
    cp_csg2_tree_t *r,
    cp_csg2_tree_t const *a)
{
    TRACE();
    r->root = cp_csg2_new(CP_CSG2_STACK, NULL);
    r->thick = a->thick;
    r->opt = a->opt;

    size_t cnt = a->z.size;

    cp_csg2_stack_t *c = cp_csg2_stack(r->root);
    cp_v_init0(&c->layer, cnt);

    cp_v_init0(&r->z, cnt);
    cp_v_copy_arr(&r->z, 0, &a->z, 0, cnt);

    cp_v_init0(&r->flag, cnt);
}
