/* -*- Mode: C -*- */
/* Copyright (C) 2018 by Henrik Theiling, License: GPLv3, see LICENSE file */

#ifndef CP_VEC_TAM_H_
#define CP_VEC_TAM_H_

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <hob3lbase/def.h>

#define CP_ARR_T(TYPE) \
    union{ \
        struct { \
            TYPE *data; \
            size_t size; \
        }; \
        size_t word[2]; \
    }

#define CP_VEC_T(TYPE) \
    union{ \
        struct { \
            TYPE *data; \
            size_t size; \
            size_t alloc; \
        }; \
        CP_ARR_T(TYPE) arr; \
        size_t word[3]; \
    }

typedef CP_VEC_T(void) cp_v_t;
typedef CP_VEC_T(size_t) cp_v_size_t;

typedef CP_ARR_T(double) cp_a_double_t;
typedef CP_ARR_T(size_t) cp_a_size_t;

typedef CP_ARR_T(unsigned short) cp_a_u16_t;

typedef struct {
    size_t p[3];
} cp_size3_t;

typedef CP_VEC_T(cp_size3_t) cp_v_size3_t;

#define CP_V_INIT { .word = { 0 } }

#define CP_A_INIT_WITH(_d,_s) {{ .data = _d, .size = _s }}

/**
 * Helper macro to allos cp_v_each to have optional arguments.
 */
#define cp_v_each_1_(i,v,skipA,skipZ,...) \
    cp_size_each(i, (v)->size, skipA, skipZ)

/**
 * Iterator expression (for 'for') for a vector.
 *
 * See cp_size_each() for details.
 */
#define cp_v_each(i,...) cp_v_each_1_(i, __VA_ARGS__, 0, 0)

#endif /* CP_VEC_TAM_H_ */
