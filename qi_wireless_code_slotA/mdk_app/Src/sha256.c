/**
  **************************************************************************
  * @file     sha256.c
  * @brief    SHA-256 hash implementation (software, standalone)
  **************************************************************************
  *
  * @note     Minimal, portable SHA-256 implementation.
  *           Uses only 32-bit arithmetic — no 64-bit division/modulo.
  *           Optimized for code size on Cortex-M4.
  *
  * Copyright (c) 2025, Project Authors. All rights reserved.
  * Licensed under BSD 2-Clause.
  *
  **************************************************************************
  */

#include "sha256.h"
#include <string.h>

/* private constants ---------------------------------------------------------*/

/** @brief  SHA-256 round constants K[0..63] */
static const uint32_t K[64] = {
  0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
  0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
  0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
  0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
  0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
  0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
  0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
  0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
  0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
  0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
  0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
  0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
  0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
  0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
  0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
  0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U
};

/* private macros ------------------------------------------------------------*/

/** @brief  Right rotate */
#define ROTR32(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))

/** @brief  SHA-256 logical functions */
#define CH(x, y, z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIGMA0(x)      (ROTR32(x,  2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define SIGMA1(x)      (ROTR32(x,  6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define sigma0(x)      (ROTR32(x,  7) ^ ROTR32(x, 18) ^ ((x) >>  3))
#define sigma1(x)      (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

/* private functions ---------------------------------------------------------*/

/**
 * @brief  Process one 64-byte (512-bit) block of data
 * @param  state: 8-word hash state (H0..H7), updated in-place
 * @param  block: 64-byte input block
 * @retval none
 */
static void sha256_transform(uint32_t *state, const uint8_t *block)
{
  uint32_t W[64];
  uint32_t a, b, c, d, e, f, g, h;
  uint32_t t1, t2;
  uint32_t i;

  /* Prepare message schedule W[0..63] */
  for (i = 0; i < 16; i++) {
    W[i] = ((uint32_t)block[i * 4] << 24) |
           ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) |
           (uint32_t)block[i * 4 + 3];
  }
  for (i = 16; i < 64; i++) {
    W[i] = sigma1(W[i - 2]) + W[i - 7] + sigma0(W[i - 15]) + W[i - 16];
  }

  /* Initialize working variables */
  a = state[0]; b = state[1]; c = state[2]; d = state[3];
  e = state[4]; f = state[5]; g = state[6]; h = state[7];

  /* 64 rounds */
  for (i = 0; i < 64; i++) {
    t1 = h + SIGMA1(e) + CH(e, f, g) + K[i] + W[i];
    t2 = SIGMA0(a) + MAJ(a, b, c);
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  /* Add compressed chunk to hash state */
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* exported functions --------------------------------------------------------*/

void sha256_init(sha256_ctx_t *ctx)
{
  /* FIPS 180-4, Section 5.3.3 — SHA-256 initial hash value */
  ctx->state[0] = 0x6A09E667U;
  ctx->state[1] = 0xBB67AE85U;
  ctx->state[2] = 0x3C6EF372U;
  ctx->state[3] = 0xA54FF53AU;
  ctx->state[4] = 0x510E527FU;
  ctx->state[5] = 0x9B05688CU;
  ctx->state[6] = 0x1F83D9ABU;
  ctx->state[7] = 0x5BE0CD19U;
  ctx->bit_count  = 0;
  ctx->buffer_len = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, uint32_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  uint32_t remaining;

  /* update total bit count */
  ctx->bit_count += (uint64_t)len * 8U;

  /* if we have buffered data, try to fill the buffer first */
  if (ctx->buffer_len > 0U) {
    remaining = SHA256_BLOCK_SIZE - ctx->buffer_len;
    if (len < remaining) {
      memcpy(&ctx->buffer[ctx->buffer_len], p, len);
      ctx->buffer_len += len;
      return;
    }
    memcpy(&ctx->buffer[ctx->buffer_len], p, remaining);
    sha256_transform(ctx->state, ctx->buffer);
    p   += remaining;
    len -= remaining;
    ctx->buffer_len = 0;
  }

  /* process full blocks directly from input */
  while (len >= SHA256_BLOCK_SIZE) {
    sha256_transform(ctx->state, p);
    p   += SHA256_BLOCK_SIZE;
    len -= SHA256_BLOCK_SIZE;
  }

  /* buffer any remaining bytes */
  if (len > 0U) {
    memcpy(ctx->buffer, p, len);
    ctx->buffer_len = len;
  }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t *hash)
{
  uint32_t i;
  uint64_t bit_count;

  /* save total bit count before we start padding */
  bit_count = ctx->bit_count;

  /* padding: append 0x80 byte followed by zeros, then 8-byte big-endian length.
   * We operate directly on the buffer to avoid per-byte update() calls. */

  /* append 0x80 */
  ctx->buffer[ctx->buffer_len++] = 0x80U;

  /* if not enough room for the 8-byte length field, flush this block */
  if (ctx->buffer_len > 56U) {
    memset(&ctx->buffer[ctx->buffer_len], 0, SHA256_BLOCK_SIZE - ctx->buffer_len);
    sha256_transform(ctx->state, ctx->buffer);
    ctx->buffer_len = 0;
  }

  /* pad remaining space with zeros (up to byte 55) */
  memset(&ctx->buffer[ctx->buffer_len], 0, 56U - ctx->buffer_len);

  /* append 64-bit big-endian total bit count at bytes 56..63 */
  ctx->buffer[56] = (uint8_t)(bit_count >> 56);
  ctx->buffer[57] = (uint8_t)(bit_count >> 48);
  ctx->buffer[58] = (uint8_t)(bit_count >> 40);
  ctx->buffer[59] = (uint8_t)(bit_count >> 32);
  ctx->buffer[60] = (uint8_t)(bit_count >> 24);
  ctx->buffer[61] = (uint8_t)(bit_count >> 16);
  ctx->buffer[62] = (uint8_t)(bit_count >> 8);
  ctx->buffer[63] = (uint8_t)(bit_count);

  /* process the final block */
  sha256_transform(ctx->state, ctx->buffer);

  /* produce the final hash value (big-endian) */
  for (i = 0; i < 8U; i++) {
    hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
    hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
    hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
    hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
  }
}

void sha256_hash(const void *data, uint32_t len, uint8_t *hash)
{
  sha256_ctx_t ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, hash);
}
