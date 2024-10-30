/**********************************************************************
 * Copyright (c) 2015 Pieter Wuille, Andrew Poelstra                  *
 * Distributed under the MIT software license, see the accompanying   *
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.*
 **********************************************************************/

#ifndef SECP256K1_ECMULT_CONST_IMPL_H
#define SECP256K1_ECMULT_CONST_IMPL_H

#include "scalar.h"
#include "group.h"
#include "ecmult_const.h"
#include "ecmult_impl.h"

#ifdef USE_ENDOMORPHISM
    #define WNAF_BITS 128
#else
    #define WNAF_BITS 256
#endif
#define WNAF_SIZE(w) ((WNAF_BITS + (w) - 1) / (w))

/* This is like `ECMULT_TABLE_GET_GE` but is constant time */
#define ECMULT_CONST_TABLE_GET_GE(r,pre,n,w) do { \
    int m; \
    int abs_n = (n) * (((n) > 0) * 2 - 1); \
    int idx_n = abs_n / 2; \
    secp256k1_fe neg_y; \
    VERIFY_CHECK(((n) & 1) == 1); \
    VERIFY_CHECK((n) >= -((1 << ((w)-1)) - 1)); \
    VERIFY_CHECK((n) <=  ((1 << ((w)-1)) - 1)); \
    VERIFY_SETUP(secp256k1_fe_clear(&(r)->x)); \
    VERIFY_SETUP(secp256k1_fe_clear(&(r)->y)); \
    for (m = 0; m < ECMULT_TABLE_SIZE(w); m++) { \
        /* This loop is used to avoid secret data in array indices. See
         * the comment in ecmult_gen_impl.h for rationale. */ \
        secp256k1_fe_cmov(&(r)->x, &(pre)[m].x, m == idx_n); \
        secp256k1_fe_cmov(&(r)->y, &(pre)[m].y, m == idx_n); \
    } \
    (r)->infinity = 0; \
    secp256k1_fe_negate(&neg_y, &(r)->y, 1); \
    secp256k1_fe_cmov(&(r)->y, &neg_y, (n) != abs_n); \
} while(0)


/** Convert a number to WNAF notation.
 *  The number becomes represented by sum(2^{wi} * wnaf[i], i=0..WNAF_SIZE(w)+1) - return_val.
 *  It has the following guarantees:
 *  - each wnaf[i] an odd integer between -(1 << w) and (1 << w)
 *  - each wnaf[i] is nonzero
 *  - the number of words set is always WNAF_SIZE(w) + 1
 *
 *  Adapted from `The Width-w NAF Method Provides Small Memory and Fast Elliptic Scalar
 *  Multiplications Secure against Side Channel Attacks`, Okeya and Tagaki. M. Joye (Ed.)
 *  CT-RSA 2003, LNCS 2612, pp. 328-443, 2003. Springer-Verlagy Berlin Heidelberg 2003
 *
 *  Numbers reference steps of `Algorithm SPA-resistant Width-w NAF with Odd Scalar` on pp. 335
 */
static int secp256k1_wnaf_const(int *wnaf, secp256k1_scalar s, int w) {
    int global_sign;
    int skew = 0;
    int word = 0;

    /* 1 2 3 */
    int u_last;
    int u;

    int flip;
    int bit;
    secp256k1_scalar neg_s;
    int not_neg_one;
    /* Note that we cannot handle even numbers by negating them to be odd, as is
     * done in other implementations, since if our scalars were specified to have
     * width < 256 for performance reasons, their negations would have width 256
     * and we'd lose any performance benefit. Instead, we use a technique from
     * Section 4.2 of the Okeya/Tagaki paper, which is to add either 1 (for even)
     * or 2 (for odd) to the number we are encoding, returning a skew value indicating
     * this, and having the caller compensate after doing the multiplication. */

    /* Negative numbers will be negated to keep their bit representation below the maximum width */
    flip = secp256k1_scalar_is_high(&s);
    /* We add 1 to even numbers, 2 to odd ones, noting that negation flips parity */
    bit = flip ^ !secp256k1_scalar_is_even(&s);
    /* We check for negative one, since adding 2 to it will cause an overflow */
    secp256k1_scalar_negate(&neg_s, &s);
    not_neg_one = !secp256k1_scalar_is_one(&neg_s);
    secp256k1_scalar_cadd_bit(&s, bit, not_neg_one);
    /* If we had negative one, flip == 1, s.d[0] == 0, bit == 1, so caller expects
     * that we added two to it and flipped it. In fact for -1 these operations are
     * identical. We only flipped, but since skewing is required (in the sense that
     * the skew must be 1 or 2, never zero) and flipping is not, we need to change
     * our flags to claim that we only skewed. */
    global_sign = secp256k1_scalar_cond_negate(&s, flip);
    global_sign *= not_neg_one * 2 - 1;
    skew = 1 << bit;

    /* 4 */
    u_last = secp256k1_scalar_shr_int(&s, w);
    while (word * w < WNAF_BITS) {
        int sign;
        int even;

        /* 4.1 4.4 */
        u = secp256k1_scalar_shr_int(&s, w);
        /* 4.2 */
        even = ((u & 1) == 0);
        sign = 2 * (u_last > 0) - 1;
        u += sign * even;
        u_last -= sign * even * (1 << w);

        /* 4.3, adapted for global sign change */
        wnaf[word++] = u_last * global_sign;

        u_last = u;
    }
    wnaf[word] = u * global_sign;

    VERIFY_CHECK(secp256k1_scalar_is_zero(&s));
    VERIFY_CHECK(word == WNAF_SIZE(w));
    return skew;
}


static void secp256k1_ecmult_const(secp256k1_gej *r, const secp256k1_ge *a, const secp256k1_scalar *scalar) {
    secp256k1_ge pre_a[ECMULT_TABLE_SIZE(WINDOW_A)];
    secp256k1_ge tmpa;
    secp256k1_fe Z;

    int skew_1;
    int wnaf_1[1 + WNAF_SIZE(WINDOW_A - 1)];
#ifdef USE_ENDOMORPHISM
    secp256k1_ge pre_a_lam[ECMULT_TABLE_SIZE(WINDOW_A)];
    int wnaf_lam[1 + WNAF_SIZE(WINDOW_A - 1)];
    int skew_lam;
    secp256k1_scalar q_1, q_lam;
#endif

    int i;
    secp256k1_scalar sc = *scalar;

    /* build wnaf representation for q. */
#ifdef USE_ENDOMORPHISM
    /* split q into q_1 and q_lam (where q = q_1 + q_lam*lambda, and q_1 and q_lam are ~128 bit) */
    secp256k1_scalar_split_lambda(&q_1, &q_lam, &sc);
    skew_1   = secp256k1_wnaf_const(wnaf_1,   q_1,   WINDOW_A - 1);
    skew_lam = secp256k1_wnaf_const(wnaf_lam, q_lam, WINDOW_A - 1);
#else
    skew_1   = secp256k1_wnaf_const(wnaf_1, sc, WINDOW_A - 1);
#endif

    /* Calculate odd multiples of a.
     * All multiples are brought to the same Z 'denominator', which is stored
     * in Z. Due to secp256k1' isomorphism we can do all operations pretending
     * that the Z coordinate was 1, use affine addition formulae, and correct
     * the Z coordinate of the result once at the end.
     */
    secp256k1_gej_set_ge(r, a);
    secp256k1_ecmult_odd_multiples_table_globalz_windowa(pre_a, &Z, r);
    for (i = 0; i < ECMULT_TABLE_SIZE(WINDOW_A); i++) {
        secp256k1_fe_normalize_weak(&pre_a[i].y);
    }
#ifdef USE_ENDOMORPHISM
    for (i = 0; i < ECMULT_TABLE_SIZE(WINDOW_A); i++) {
        secp256k1_ge_mul_lambda(&pre_a_lam[i], &pre_a[i]);
    }
#endif

    /* first loop iteration (separated out so we can directly set r, rather
     * than having it start at infinity, get doubled several times, then have
     * its new value added to it) */
    i = wnaf_1[WNAF_SIZE(WINDOW_A - 1)];
    VERIFY_CHECK(i != 0);
    ECMULT_CONST_TABLE_GET_GE(&tmpa, pre_a, i, WINDOW_A);
    secp256k1_gej_set_ge(r, &tmpa);
#ifdef USE_ENDOMORPHISM
    i = wnaf_lam[WNAF_SIZE(WINDOW_A - 1)];
    VERIFY_CHECK(i != 0);
    ECMULT_CONST_TABLE_GET_GE(&tmpa, pre_a_lam, i, WINDOW_A);
    secp256k1_gej_add_ge(r, r, &tmpa);
#endif
    /* remaining loop iterations */
    for (i = WNAF_SIZE(WINDOW_A - 1) - 1; i >= 0; i--) {
        int n;
        int j;
        for (j = 0; j < WINDOW_A - 1; ++j) {
            secp256k1_gej_double_nonzero(r, r, NULL);
        }

        n = wnaf_1[i];
        ECMULT_CONST_TABLE_GET_GE(&tmpa, pre_a, n, WINDOW_A);
        VERIFY_CHECK(n != 0);
        secp256k1_gej_add_ge(r, r, &tmpa);
#ifdef USE_ENDOMORPHISM
        n = wnaf_lam[i];
        ECMULT_CONST_TABLE_GET_GE(&tmpa, pre_a_lam, n, WINDOW_A);
        VERIFY_CHECK(n != 0);
        secp256k1_gej_add_ge(r, r, &tmpa);
#endif
    }

    secp256k1_fe_mul(&r->z, &r->z, &Z);

    {
        /* Correct for wNAF skew */
        secp256k1_ge correction = *a;
        secp256k1_ge_storage correction_1_stor;
#ifdef USE_ENDOMORPHISM
        secp256k1_ge_storage correction_lam_stor;
#endif
        secp256k1_ge_storage a2_stor;
        secp256k1_gej tmpj;
        secp256k1_gej_set_ge(&tmpj, &correction);
        secp256k1_gej_double_var(&tmpj, &tmpj, NULL);
        secp256k1_ge_set_gej(&correction, &tmpj);
        secp256k1_ge_to_storage(&correction_1_stor, a);
#ifdef USE_ENDOMORPHISM
        secp256k1_ge_to_storage(&correction_lam_stor, a);
#endif
        secp256k1_ge_to_storage(&a2_stor, &correction);

        /* For odd numbers this is 2a (so replace it), for even ones a (so no-op) */
        secp256k1_ge_storage_cmov(&correction_1_stor, &a2_stor, skew_1 == 2);
#ifdef USE_ENDOMORPHISM
        secp256k1_ge_storage_cmov(&correction_lam_stor, &a2_stor, skew_lam == 2);
#endif

        /* Apply the correction */
        secp256k1_ge_from_storage(&correction, &correction_1_stor);
        secp256k1_ge_neg(&correction, &correction);
        secp256k1_gej_add_ge(r, r, &correction);

#ifdef USE_ENDOMORPHISM
        secp256k1_ge_from_storage(&correction, &correction_lam_stor);
        secp256k1_ge_neg(&correction, &correction);
        secp256k1_ge_mul_lambda(&correction, &correction);
        secp256k1_gej_add_ge(r, r, &correction);
#endif
    }
}

static int secp256k1_ecmult_const_xonly(secp256k1_fe* r, const secp256k1_fe *n, const secp256k1_fe *d, const secp256k1_scalar *q, int known_on_curve) {

    /* This algorithm is a generalization of Peter Dettman's technique for
     * avoiding the square root in a random-basepoint x-only multiplication
     * on a Weierstrass curve:
     * https://mailarchive.ietf.org/arch/msg/cfrg/7DyYY6gg32wDgHAhgSb6XxMDlJA/
     *
     *
     * === Background: the effective affine technique ===
     *
     * Let phi_u be the isomorphism that maps (x, y) on secp256k1 curve y^2 = x^3 + 7 to
     * x' = u^2*x, y' = u^3*y on curve y'^2 = x'^3 + u^6*7. This new curve has the same order as
     * the original (it is isomorphic), but moreover, has the same addition/doubling formulas, as
     * the curve b=7 coefficient does not appear in those formulas (or at least does not appear in
     * the formulas implemented in this codebase, both affine and Jacobian). See also Example 9.5.2
     * in https://www.math.auckland.ac.nz/~sgal018/crypto-book/ch9.pdf.
     *
     * This means any linear combination of secp256k1 points can be computed by applying phi_u
     * (with non-zero u) on all input points (including the generator, if used), computing the
     * linear combination on the isomorphic curve (using the same group laws), and then applying
     * phi_u^{-1} to get back to secp256k1.
     *
     * Switching to Jacobian coordinates, note that phi_u applied to (X, Y, Z) is simply
     * (X, Y, Z/u). Thus, if we want to compute (X1, Y1, Z) + (X2, Y2, Z), with identical Z
     * coordinates, we can use phi_Z to transform it to (X1, Y1, 1) + (X2, Y2, 1) on an isomorphic
     * curve where the affine addition formula can be used instead.
     * If (X3, Y3, Z3) = (X1, Y1) + (X2, Y2) on that curve, then our answer on secp256k1 is
     * (X3, Y3, Z3*Z).
     *
     * This is the effective affine technique: if we have a linear combination of group elements
     * to compute, and all those group elements have the same Z coordinate, we can simply pretend
     * that all those Z coordinates are 1, perform the computation that way, and then multiply the
     * original Z coordinate back in.
     *
     * The technique works on any a=0 short Weierstrass curve. It is possible to generalize it to
     * other curves too, but there the isomorphic curves will have different 'a' coefficients,
     * which typically does affect the group laws.
     *
     *
     * === Avoiding the square root for x-only point multiplication ===
     *
     * In this function, we want to compute the X coordinate of q*(n/d, y), for
     * y = sqrt((n/d)^3 + 7). Its negation would also be a valid Y coordinate, but by convention
     * we pick whatever sqrt returns (which we assume to be a deterministic function).
     *
     * Let g = y^2*d^3 = n^3 + 7*d^3. This also means y = sqrt(g/d^3).
     * Further let v = sqrt(d*g), which must exist as d*g = y^2*d^4 = (y*d^2)^2.
     *
     * The input point (n/d, y) also has Jacobian coordinates:
     *
     *     (n/d, y, 1)
     *   = (n/d * v^2, y * v^3, v)
     *   = (n/d * d*g, y * sqrt(d^3*g^3), v)
     *   = (n/d * d*g, sqrt(y^2 * d^3*g^3), v)
     *   = (n*g, sqrt(g/d^3 * d^3*g^3), v)
     *   = (n*g, sqrt(g^4), v)
     *   = (n*g, g^2, v)
     *
     * It is easy to verify that both (n*g, g^2, v) and its negation (n*g, -g^2, v) have affine X
     * coordinate n/d, and this holds even when the square root function doesn't have a
     * deterministic sign. We choose the (n*g, g^2, v) version.
     *
     * Now switch to the effective affine curve using phi_v, where the input point has coordinates
     * (n*g, g^2). Compute (X, Y, Z) = q * (n*g, g^2) there.
     *
     * Back on secp256k1, that means q * (n*g, g^2, v) = (X, Y, v*Z). This last point has affine X
     * coordinate X / (v^2*Z^2) = X / (d*g*Z^2). Determining the affine Y coordinate would involve
     * a square root, but as long as we only care about the resulting X coordinate, no square root
     * is needed anywhere in this computation.
     */

    secp256k1_fe g, i;
    secp256k1_ge p;
    secp256k1_gej rj;

    /* Compute g = (n^3 + B*d^3). */
    secp256k1_fe_sqr(&g, n);
    secp256k1_fe_mul(&g, &g, n);
    if (d) {
        secp256k1_fe b;
        VERIFY_CHECK(!secp256k1_fe_normalizes_to_zero(d));
        secp256k1_fe_sqr(&b, d);
        VERIFY_CHECK(CURVE_B <= 8); /* magnitude of b will be <= 8 after the next call */
        secp256k1_fe_mul_int(&b, CURVE_B);
        secp256k1_fe_mul(&b, &b, d);
        secp256k1_fe_add(&g, &b);
        if (!known_on_curve) {
            /* We need to determine whether (n/d)^3 + 7 is square.
             *
             *     is_square((n/d)^3 + 7)
             * <=> is_square(((n/d)^3 + 7) * d^4)
             * <=> is_square((n^3 + 7*d^3) * d)
             * <=> is_square(g * d)
             */
            secp256k1_fe c;
            secp256k1_fe_mul(&c, &g, d);
            if (!secp256k1_fe_is_square_var(&c)) return 0;
        }
    } else {
        secp256k1_fe_add_int(&g, CURVE_B);
        if (!known_on_curve) {
            /* g at this point equals x^3 + 7. Test if it is square. */
            if (!secp256k1_fe_is_square_var(&g)) return 0;
        }
    }

    /* Compute base point P = (n*g, g^2), the effective affine version of (n*g, g^2, v), which has
     * corresponding affine X coordinate n/d. */
    secp256k1_fe_mul(&p.x, &g, n);
    secp256k1_fe_sqr(&p.y, &g);
    p.infinity = 0;

    /* Perform x-only EC multiplication of P with q. */
    VERIFY_CHECK(!secp256k1_scalar_is_zero(q));
    secp256k1_ecmult_const(&rj, &p, q);
    VERIFY_CHECK(!secp256k1_gej_is_infinity(&rj));

    /* The resulting (X, Y, Z) point on the effective-affine isomorphic curve corresponds to
     * (X, Y, Z*v) on the secp256k1 curve. The affine version of that has X coordinate
     * (X / (Z^2*d*g)). */
    secp256k1_fe_sqr(&i, &rj.z);
    secp256k1_fe_mul(&i, &i, &g);
    if (d) secp256k1_fe_mul(&i, &i, d);
    secp256k1_fe_inv(&i, &i);
    secp256k1_fe_mul(r, &rj.x, &i);

    return 1;
}

#endif /* SECP256K1_ECMULT_CONST_IMPL_H */
