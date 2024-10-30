/**********************************************************************
 * Copyright (c) 2013, 2014 Pieter Wuille                             *
 * Distributed under the MIT software license, see the accompanying   *
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.*
 **********************************************************************/

#ifndef SECP256K1_FIELD_H
#define SECP256K1_FIELD_H

/** Field element module.
 *
 *  Field elements can be represented in several ways, but code accessing
 *  it (and implementations) need to take certain properties into account:
 *  - Each field element can be normalized or not.
 *  - Each field element has a magnitude, which represents how far away
 *    its representation is away from normalization. Normalized elements
 *    always have a magnitude of 1, but a magnitude of 1 doesn't imply
 *    normality.
 */

#if defined HAVE_CONFIG_H
#include "libsecp256k1-config.h"
#endif

#if defined(USE_FIELD_10X26)
#include "field_10x26.h"
#elif defined(USE_FIELD_5X52)
#include "field_5x52.h"
#else
#error "Please select field implementation"
#endif

#include "util.h"

#ifdef VERIFY
/* Magnitude and normalized value for constants. */
#define SECP256K1_FE_VERIFY_CONST(d7, d6, d5, d4, d3, d2, d1, d0) \
    /* Magnitude is 0 for constant 0; 1 otherwise. */ \
    , (((d7) | (d6) | (d5) | (d4) | (d3) | (d2) | (d1) | (d0)) != 0) \
    /* Normalized is 1 unless sum(d_i<<(32*i) for i=0..7) exceeds field modulus. */ \
    , (!(((d7) & (d6) & (d5) & (d4) & (d3) & (d2)) == 0xfffffffful && ((d1) == 0xfffffffful || ((d1) == 0xfffffffe && (d0 >= 0xfffffc2f)))))
#else
#define SECP256K1_FE_VERIFY_CONST(d7, d6, d5, d4, d3, d2, d1, d0)
#endif

/** This expands to an initializer for a secp256k1_fe valued sum((i*32) * d_i, i=0..7) mod p.
 *
 * It has magnitude 1, unless d_i are all 0, in which case the magnitude is 0.
 * It is normalized, unless sum(2^(i*32) * d_i, i=0..7) >= p.
 *
 * SECP256K1_FE_CONST_INNER is provided by the implementation.
 */
#define SECP256K1_FE_CONST_V2(d7, d6, d5, d4, d3, d2, d1, d0) {SECP256K1_FE_CONST_INNER((d7), (d6), (d5), (d4), (d3), (d2), (d1), (d0)) SECP256K1_FE_VERIFY_CONST((d7), (d6), (d5), (d4), (d3), (d2), (d1), (d0)) }

#ifndef VERIFY
/* In non-VERIFY mode, we #define the fe operations to be identical to their
 * internal field implementation, to avoid the potential overhead of a
 * function call (even though presumably inlinable). */
#  define secp256k1_fe_set_b32_mod 			secp256k1_fe_impl_set_b32_mod
#  define secp256k1_fe_half 				secp256k1_fe_impl_half
#  define secp256k1_fe_add_int 			    secp256k1_fe_impl_add_int
#  define secp256k1_fe_is_square_var 		secp256k1_fe_impl_is_square_var
#endif /* !defined(VERIFY) */

static const secp256k1_fe secp256k1_fe_one = SECP256K1_FE_CONST_V2(0, 0, 0, 0, 0, 0, 0, 1);
/** Normalize a field element. */
static void secp256k1_fe_normalize(secp256k1_fe *r);

/** Weakly normalize a field element: reduce it magnitude to 1, but don't fully normalize. */
static void secp256k1_fe_normalize_weak(secp256k1_fe *r);

/** Normalize a field element, without constant-time guarantee. */
static void secp256k1_fe_normalize_var(secp256k1_fe *r);

/** Verify whether a field element represents zero i.e. would normalize to a zero value. The field
 *  implementation may optionally normalize the input, but this should not be relied upon. */
static int secp256k1_fe_normalizes_to_zero(const secp256k1_fe *r);

/** Verify whether a field element represents zero i.e. would normalize to a zero value. The field
 *  implementation may optionally normalize the input, but this should not be relied upon. */
static int secp256k1_fe_normalizes_to_zero_var(const secp256k1_fe *r);

/** Set a field element equal to a small integer. Resulting field element is normalized. */
static void secp256k1_fe_set_int(secp256k1_fe *r, int a);

/** Sets a field element equal to zero, initializing all fields. */
static void secp256k1_fe_clear(secp256k1_fe *a);

/** Verify whether a field element is zero. Requires the input to be normalized. */
static int secp256k1_fe_is_zero(const secp256k1_fe *a);

/** Check the "oddness" of a field element. Requires the input to be normalized. */
static int secp256k1_fe_is_odd(const secp256k1_fe *a);

/** Compare two field elements. Requires magnitude-1 inputs. */
static int secp256k1_fe_equal(const secp256k1_fe *a, const secp256k1_fe *b);

/** Same as secp256k1_fe_equal, but may be variable time. */
static int secp256k1_fe_equal_var(const secp256k1_fe *a, const secp256k1_fe *b);

/** Compare two field elements. Requires both inputs to be normalized */
static int secp256k1_fe_cmp_var(const secp256k1_fe *a, const secp256k1_fe *b);

/** Set a field element equal to 32-byte big endian value. If successful, the resulting field element is normalized. */
static int secp256k1_fe_set_b32(secp256k1_fe *r, const unsigned char *a);

/** Convert a field element to a 32-byte big endian value. Requires the input to be normalized */
static void secp256k1_fe_get_b32(unsigned char *r, const secp256k1_fe *a);

/** Set a field element equal to the additive inverse of another. Takes a maximum magnitude of the input
 *  as an argument. The magnitude of the output is one higher. */
static void secp256k1_fe_negate(secp256k1_fe *r, const secp256k1_fe *a, int m);

/** Add a small integer to a field element.
 *
 * Performs {r += a}. The magnitude of r increases by 1, and normalized is cleared.
 * a must be in range [0,0x7FFF].
 */
static void secp256k1_fe_add_int(secp256k1_fe *r, int a);

/** Multiplies the passed field element with a small integer constant. Multiplies the magnitude by that
 *  small integer. */
static void secp256k1_fe_mul_int(secp256k1_fe *r, int a);

/** Adds a field element to another. The result has the sum of the inputs' magnitudes as magnitude. */
static void secp256k1_fe_add(secp256k1_fe *r, const secp256k1_fe *a);

/** Sets a field element to be the product of two others. Requires the inputs' magnitudes to be at most 8.
 *  The output magnitude is 1 (but not guaranteed to be normalized). */
static void secp256k1_fe_mul(secp256k1_fe *r, const secp256k1_fe *a, const secp256k1_fe * SECP256K1_RESTRICT b);

/** Sets a field element to be the square of another. Requires the input's magnitude to be at most 8.
 *  The output magnitude is 1 (but not guaranteed to be normalized). */
static void secp256k1_fe_sqr(secp256k1_fe *r, const secp256k1_fe *a);

/** If a has a square root, it is computed in r and 1 is returned. If a does not
 *  have a square root, the root of its negation is computed and 0 is returned.
 *  The input's magnitude can be at most 8. The output magnitude is 1 (but not
 *  guaranteed to be normalized). The result in r will always be a square
 *  itself. */
static int secp256k1_fe_sqrt(secp256k1_fe *r, const secp256k1_fe *a);

/** Checks whether a field element is a quadratic residue. */
static int secp256k1_fe_is_quad_var(const secp256k1_fe *a);

/** Sets a field element to be the (modular) inverse of another. Requires the input's magnitude to be
 *  at most 8. The output magnitude is 1 (but not guaranteed to be normalized). */
static void secp256k1_fe_inv(secp256k1_fe *r, const secp256k1_fe *a);

/** Potentially faster version of secp256k1_fe_inv, without constant-time guarantee. */
static void secp256k1_fe_inv_var(secp256k1_fe *r, const secp256k1_fe *a);

/** Calculate the (modular) inverses of a batch of field elements. Requires the inputs' magnitudes to be
 *  at most 8. The output magnitudes are 1 (but not guaranteed to be normalized). The inputs and
 *  outputs must not overlap in memory. */
static void secp256k1_fe_inv_all_var(secp256k1_fe *r, const secp256k1_fe *a, size_t len);

/** Convert a field element to the storage type. */
static void secp256k1_fe_to_storage(secp256k1_fe_storage *r, const secp256k1_fe *a);

/** Convert a field element back from the storage type. */
static void secp256k1_fe_from_storage(secp256k1_fe *r, const secp256k1_fe_storage *a);

/** If flag is true, set *r equal to *a; otherwise leave it. Constant-time. */
static void secp256k1_fe_storage_cmov(secp256k1_fe_storage *r, const secp256k1_fe_storage *a, int flag);

/** If flag is true, set *r equal to *a; otherwise leave it. Constant-time. */
static void secp256k1_fe_cmov(secp256k1_fe *r, const secp256k1_fe *a, int flag);

/** Check invariants on a field element (no-op unless VERIFY is enabled). */
static void secp256k1_fe_verify(const secp256k1_fe *a);
#define SECP256K1_FE_VERIFY(a) secp256k1_fe_verify(a)

/** Check that magnitude of a is at most m (no-op unless VERIFY is enabled). */
static void secp256k1_fe_verify_magnitude(const secp256k1_fe *a, int m);
#define SECP256K1_FE_VERIFY_MAGNITUDE(a, m) secp256k1_fe_verify_magnitude(a, m)

/** Determine whether a is a square (modulo p).
 *
 * On input, a must be a valid field element.
 */
static int secp256k1_fe_is_square_var(const secp256k1_fe *a);

/** Halve the value of a field element modulo the field prime in constant-time.
 *
 * On input, r must be a valid field element.
 * On output, r will be normalized and have magnitude floor(m/2) + 1 where m is
 * the magnitude of r on input.
 */
static void secp256k1_fe_half(secp256k1_fe *r);

/** Set a field element equal to the element represented by a provided 32-byte big endian value
 * interpreted modulo p.
 *
 * On input, r does not need to be initialized. a must be a pointer to an initialized 32-byte array.
 * On output, r = a (mod p). It will have magnitude 1, and not be normalized.
 */
static void secp256k1_fe_set_b32_mod(secp256k1_fe *r, const unsigned char *a);

#endif /* SECP256K1_FIELD_H */
