/*
 * Copyright (c) 2015, Kenneth MacKay
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Minimal ECDSA P-256 verification for embedded bootloader.
 * Uses Jacobian projective coordinates to eliminate expensive modular
 * inverse calls from point operations.  Only uECC_verify() is exported.
 *
 * Fixes over original implementation:
 *   - Correct K = 2^256 - p reduction constant
 *   - Correct borrow detection in 256-bit subtraction (LSW-first loop)
 *   - Jacobian coordinates: no mod_inv in point_double / point_add
 *   - Compact code optimized for -Oz compilation
 */

#include "uECC.h"
#include <string.h>

static void (*g_progress_cb)(void) = 0;

void uECC_set_progress_cb(void (*cb)(void))
{
    g_progress_cb = cb;
}

static void progress(void)
{
    if (g_progress_cb != 0) {
        g_progress_cb();
    }
}

/* =========================================================================
 * Internal types
 * ========================================================================= */

/* 256-bit value: 8 x 32-bit words, little-endian (word[0 = LSW) */
typedef uint32_t w256[8];

/* Jacobian point: affine (x,y) = (X/Z^2, Y/Z^3).  Z=0 → point at infinity */
typedef struct { w256 x, y, z; } jpoint_t;

/* =========================================================================
 * Curve constants (little-endian word order)
 * ========================================================================= */

/* Field prime p = 2^256 - 2^224 + 2^192 + 2^96 - 1 */
static const w256 P = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF
};

/* Curve order n */
static const w256 N = {
    0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
    0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF
};

/* Reduction constant R = 2^256 - p  (for iterative mod reduction) */
static const w256 Rp = {
    0x00000001, 0x00000000, 0x00000000, 0xFFFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 0x00000000
};

/* Reduction constant R = 2^256 - n */
static const w256 Rn = {
    0x039CDAAF, 0x0C46353D, 0x58E8617B, 0x43190552,
    0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000000
};

/* Exponent p - 2 (for Fermat-inverse mod p) */
static const w256 Pm2 = {
    0xFFFFFFFD, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF
};

/* Exponent n - 2 (for Fermat-inverse mod n) */
static const w256 Nm2 = {
    0xFC63254F, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
    0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF
};

/* Generator point G (uncompressed SEC1: 04 || Gx || Gy) */
static const uint8_t G_bytes[65] = {
    0x04,
    0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47,
    0xF8, 0xBC, 0xE6, 0xE5, 0x63, 0xA4, 0x40, 0xF2,
    0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0,
    0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96,
    0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B,
    0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
    0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE,
    0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5
};

/* =========================================================================
 * 256-bit bignum helpers (little-endian word order)
 * ========================================================================= */

static int bn_is_zero(const w256 a)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < 8; i++) r |= a[i];
    return r == 0;
}

static int bn_gte(const w256 a, const w256 b)
{
    int i;
    for (i = 7; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 1;
}

static uint32_t bn_add(w256 r, const w256 a, const w256 b)
{
    uint64_t c = 0;
    int i;
    for (i = 0; i < 8; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

/* Subtract: r = a - b.  Returns borrow (1 on underflow).
 * NOTE: iterates LSW → MSW for correct borrow propagation. */
static uint32_t bn_sub(w256 r, const w256 a, const w256 b)
{
    uint64_t borrow = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t diff = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)diff;
        borrow = (diff >> 63) & 1;
    }
    return (uint32_t)borrow;
}

/* =========================================================================
 * Schoolbook 256 × 256 → 512 multiplication
 * ========================================================================= */

static void bn_mul512(uint32_t lo[8], uint32_t hi[8],
                      const w256 a, const w256 b)
{
    uint32_t p[16];
    int i, j;
    uint64_t carry;

    memset(p, 0, sizeof(p));
    for (i = 0; i < 8; i++) {
        carry = 0;
        for (j = 0; j < 8; j++) {
            carry += (uint64_t)a[i] * b[j] + p[i + j];
            p[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        p[i + 8] = (uint32_t)carry;
    }
    for (i = 0; i < 8; i++) {
        lo[i] = p[i];
        hi[i] = p[i + 8];
    }
}

/* =========================================================================
 * Modular arithmetic (generic over modulus)
 * ========================================================================= */

/*
 * Modular multiplication: r = a * b mod mod.
 *
 * Uses iterative reduction: split 512-bit product into lo (256-bit) and
 * hi (256-bit), then compute  r = lo + hi * R  where R = 2^256 mod mod.
 * Repeat until hi = 0 (converges in ≤ 8 rounds for P-256), then do one
 * conditional subtraction of mod.
 */
static void modmul(w256 r, const w256 a, const w256 b,
                   const w256 R, const w256 mod)
{
    w256 hi;
    uint32_t acc[17];
    uint32_t wr_lo[8], wr_hi[8];
    int i, rnd;
    uint64_t carry;

    memset(acc, 0, sizeof(acc));
    bn_mul512(acc, acc + 8, a, b);

    for (rnd = 0; rnd < 16; rnd++) {
        uint32_t z = 0;
        for (i = 8; i < 17; i++) {
            z |= acc[i];
        }
        if (!z) {
            break;
        }

        memcpy(hi, acc + 8, sizeof(w256));
        bn_mul512(wr_lo, wr_hi, hi, R);

        carry = 0;
        for (i = 0; i < 8; i++) {
            carry += (uint64_t)acc[i] + wr_lo[i];
            acc[i] = (uint32_t)carry;
            carry >>= 32;
        }
        for (i = 0; i < 8; i++) {
            carry += wr_hi[i];
            acc[8 + i] = (uint32_t)carry;
            carry >>= 32;
        }
        acc[16] = (uint32_t)carry;
    }

    memcpy(r, acc, sizeof(w256));
    /* Bound the subtract loop: a broken reduction must not hang 0x37. */
    for (i = 0; i < 8 && bn_gte(r, mod); i++) {
        bn_sub(r, r, mod);
    }
}

/* Modular inverse via Fermat: r = a^(mod-2) mod mod (right-to-left). */
static void modinv(w256 r, const w256 a,
                   const w256 R, const w256 mod, const w256 exp)
{
    w256 base, res;
    int i, j;

    memcpy(base, a, sizeof(w256));
    memset(res, 0, sizeof(res));
    res[0] = 1;

    for (i = 0; i < 8; i++) {
        for (j = 0; j < 32; j++) {
            if ((exp[i] >> j) & 1) {
                modmul(res, res, base, R, mod);
            }
            modmul(base, base, base, R, mod);
        }
        progress();
    }
    memcpy(r, res, sizeof(w256));
}

static void modadd(w256 r, const w256 a, const w256 b, const w256 mod)
{
    uint32_t c = bn_add(r, a, b);
    if (c || bn_gte(r, mod)) bn_sub(r, r, mod);
}

static void modsub(w256 r, const w256 a, const w256 b, const w256 mod)
{
    uint32_t borrow = bn_sub(r, a, b);
    if (borrow) bn_add(r, r, mod);
}

/* Thin wrappers for field (mod p) and scalar (mod n) operations */
static void fmul(w256 r, const w256 a, const w256 b) { modmul(r, a, b, Rp, P); }
static void finv(w256 r, const w256 a)               { modinv(r, a, Rp, P, Pm2); }
static void fadd(w256 r, const w256 a, const w256 b) { modadd(r, a, b, P); }
static void fsub(w256 r, const w256 a, const w256 b) { modsub(r, a, b, P); }
static void smul(w256 r, const w256 a, const w256 b) { modmul(r, a, b, Rn, N); }
static void sinv(w256 r, const w256 a)               { modinv(r, a, Rn, N, Nm2); }

/* =========================================================================
 * Byte ↔ word conversion (SEC1 big-endian bytes ↔ LE 32-bit words)
 * ========================================================================= */

static void bytes_to_w256(w256 w, const uint8_t *b)
{
    int i;
    for (i = 0; i < 8; i++) {
        int off = (7 - i) * 4;
        w[i] = ((uint32_t)b[off] << 24) | ((uint32_t)b[off+1] << 16) |
               ((uint32_t)b[off+2] << 8) | (uint32_t)b[off+3];
    }
}

/* =========================================================================
 * Jacobian point operations (a = -3 for P-256)
 * ========================================================================= */

/* Doubling: r = 2·p
 * Uses a = -3 optimisation: D = 3·(X + Z²)·(X - Z²) */
static void jp_double(jpoint_t *r, const jpoint_t *p)
{
    w256 A, Zsq, B, C, D, t1, t2;

    if (bn_is_zero(p->z) || bn_is_zero(p->y)) {
        memset(r, 0, sizeof(jpoint_t));
        return;
    }

    fmul(A,   p->y, p->y);         /* A = Y² */
    fmul(Zsq, p->z, p->z);         /* Z² */

    fmul(B, p->x, A);              /* B = X·A */
    fadd(B, B, B);                  /* 2·X·A */
    fadd(B, B, B);                  /* B = 4·X·A */

    fmul(C, A, A);                  /* A² */
    fadd(C, C, C);
    fadd(C, C, C);
    fadd(C, C, C);                  /* C = 8·A² */

    fadd(t1, p->x, Zsq);           /* X + Z² */
    fsub(t2, p->x, Zsq);           /* X - Z² */
    fmul(D, t1, t2);               /* (X+Z²)(X-Z²) */
    fadd(t1, D, D);
    fadd(D, t1, D);                 /* D = 3·(X²-Z⁴) */

    fmul(t1, D, D);                 /* D² */
    fsub(t1, t1, B);               /* D² - B */
    fsub(r->x, t1, B);             /* X3 = D² - 2·B */

    fsub(t1, B, r->x);             /* B - X3 */
    fmul(t1, D, t1);               /* D·(B-X3) */
    fsub(r->y, t1, C);             /* Y3 = D·(B-X3) - C */

    fmul(t1, p->y, p->z);
    fadd(r->z, t1, t1);            /* Z3 = 2·Y·Z */
}

/* General addition: r = p + q  (both Jacobian) */
static void jp_add(jpoint_t *r, const jpoint_t *p, const jpoint_t *q)
{
    w256 Z1sq, Z2sq, Z1cu, Z2cu;
    w256 U1, S1, U2, S2;
    w256 H, Hsq, Hcu, RR, t1, t2;

    if (bn_is_zero(p->z)) { *r = *q; return; }
    if (bn_is_zero(q->z)) { *r = *p; return; }

    fmul(Z1sq, p->z, p->z);
    fmul(Z2sq, q->z, q->z);
    fmul(Z1cu, Z1sq, p->z);
    fmul(Z2cu, Z2sq, q->z);

    fmul(U1, p->x, Z2sq);
    fmul(S1, p->y, Z2cu);
    fmul(U2, q->x, Z1sq);
    fmul(S2, q->y, Z1cu);

    fsub(H,  U2, U1);
    fsub(RR, S2, S1);

    if (bn_is_zero(H)) {
        if (bn_is_zero(RR)) { jp_double(r, p); return; }
        memset(r, 0, sizeof(jpoint_t));
        return;
    }

    fmul(Hsq, H, H);
    fmul(Hcu, Hsq, H);
    fmul(t1, U1, Hsq);             /* U1·H² */

    fmul(t2, RR, RR);              /* R² */
    fsub(t2, t2, Hcu);             /* R² - H³ */
    fsub(t2, t2, t1);
    fsub(r->x, t2, t1);            /* X3 = R² - H³ - 2·U1·H² */

    fsub(t1, t1, r->x);            /* U1·H² - X3 */
    fmul(t1, RR, t1);              /* R·(U1·H² - X3) */
    fmul(t2, S1, Hcu);             /* S1·H³ */
    fsub(r->y, t1, t2);            /* Y3 */

    fmul(t1, p->z, q->z);
    fmul(r->z, t1, H);             /* Z3 = Z1·Z2·H */
}

/* Jacobian → affine: x = X/Z²,  y = Y/Z³ */
static void jp_to_affine(w256 ax, w256 ay, const jpoint_t *p)
{
    w256 zi, zi2, zi3;
    if (bn_is_zero(p->z)) {
        memset(ax, 0, sizeof(w256));
        memset(ay, 0, sizeof(w256));
        return;
    }
    finv(zi, p->z);
    fmul(zi2, zi, zi);
    fmul(zi3, zi2, zi);
    fmul(ax, p->x, zi2);
    fmul(ay, p->y, zi3);
}

/* Scalar multiplication: r = k·p  (left-to-right double-and-add) */
static void point_mul(jpoint_t *r, const jpoint_t *p, const w256 k)
{
    jpoint_t acc, tmp;
    int i, j, started = 0;

    memset(&acc, 0, sizeof(acc));
    for (i = 7; i >= 0; i--) {
        for (j = 31; j >= 0; j--) {
            int bit = (k[i] >> j) & 1;
            if (started) {
                jp_double(&tmp, &acc);
                acc = tmp;
            }
            if (bit) {
                if (!started) {
                    acc = *p;
                    started = 1;
                } else {
                    jp_add(&tmp, &acc, p);
                    acc = tmp;
                }
            }
        }
        progress();
    }
    *r = acc;
}

/* =========================================================================
 * ECDSA P-256 signature verification
 * ========================================================================= */

int uECC_verify(const uint8_t *public_key,
                const uint8_t *message_hash,
                const uint8_t *signature)
{
    w256 r_val, s_val, e_val, w_val, u1, u2;
    w256 pub_x, pub_y, gen_x, gen_y;
    w256 aff_x, aff_y;
    jpoint_t pub_pt, gen_pt, pt1, pt2, res;
    w256 v;

    if (public_key[0] != 0x04) return 0;

    bytes_to_w256(r_val, signature);
    bytes_to_w256(s_val, signature + 32);

    if (bn_is_zero(r_val) || bn_is_zero(s_val)) return 0;
    if (bn_gte(r_val, N) || bn_gte(s_val, N)) return 0;

    bytes_to_w256(pub_x, public_key + 1);
    bytes_to_w256(pub_y, public_key + 33);
    bytes_to_w256(gen_x, G_bytes + 1);
    bytes_to_w256(gen_y, G_bytes + 33);
    bytes_to_w256(e_val, message_hash);
    if (bn_gte(e_val, N)) bn_sub(e_val, e_val, N);

    sinv(w_val, s_val);            /* w = s⁻¹ mod n */
    smul(u1, e_val, w_val);        /* u1 = e·w mod n */
    smul(u2, r_val, w_val);        /* u2 = r·w mod n */

    /* Generator → Jacobian (Z = 1) */
    memcpy(gen_pt.x, gen_x, sizeof(w256));
    memcpy(gen_pt.y, gen_y, sizeof(w256));
    memset(gen_pt.z, 0, sizeof(w256));
    gen_pt.z[0] = 1;

    /* Public key → Jacobian (Z = 1) */
    memcpy(pub_pt.x, pub_x, sizeof(w256));
    memcpy(pub_pt.y, pub_y, sizeof(w256));
    memset(pub_pt.z, 0, sizeof(w256));
    pub_pt.z[0] = 1;

    /* R = u1·G + u2·Q */
    point_mul(&pt1, &gen_pt, u1);
    point_mul(&pt2, &pub_pt, u2);
    jp_add(&res, &pt1, &pt2);

    if (bn_is_zero(res.z)) return 0;

    jp_to_affine(aff_x, aff_y, &res);

    /* v = R.x mod n */
    memcpy(v, aff_x, sizeof(w256));
    if (bn_gte(v, N)) bn_sub(v, v, N);

    /* Signature valid iff v == r */
    return (memcmp(v, r_val, sizeof(w256)) == 0) ? 1 : 0;
}
