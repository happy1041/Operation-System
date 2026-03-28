#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

/* 17.14 fixed-point number representation.
   p = 17 integer bits, q = 14 fractional bits, 1 sign bit.
   f = 2^q = 1 << 14 = 16384。 */

#define FP_Q 14
#define FP_F (1 << FP_Q)

/* Convert integer n to fixed-point. */
#define FP_FROM_INT(n) ((n) * FP_F)

/* Convert fixed-point x to integer (round toward zero). */
#define FP_TO_INT_TRUNC(x) ((x) / FP_F)

/* Convert fixed-point x to integer (round to nearest). */
#define FP_TO_INT_ROUND(x) \
    ((x) >= 0 ? ((x) + FP_F / 2) / FP_F : ((x) - FP_F / 2) / FP_F)

/* Add two fixed-point numbers. */
#define FP_ADD(x, y) ((x) + (y))

/* Subtract fixed-point y from x. */
#define FP_SUB(x, y) ((x) - (y))

/* Add fixed-point x and integer n. */
#define FP_ADD_INT(x, n) ((x) + (n) * FP_F)

/* Subtract integer n from fixed-point x. */
#define FP_SUB_INT(x, n) ((x) - (n) * FP_F)

/* Multiply two fixed-point numbers. */
#define FP_MUL(x, y) ((int)(((int64_t)(x)) * (y) / FP_F))

/* Multiply fixed-point x by integer n. */
#define FP_MUL_INT(x, n) ((x) * (n))

/* Divide fixed-point x by fixed-point y. */
#define FP_DIV(x, y) ((int)(((int64_t)(x)) * FP_F / (y)))

/* Divide fixed-point x by integer n. */
#define FP_DIV_INT(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */
