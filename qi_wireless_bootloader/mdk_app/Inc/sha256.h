/**
  **************************************************************************
  * @file     sha256.h
  * @brief    SHA-256 hash implementation (software, standalone)
  **************************************************************************
  *
  * @note     Minimal SHA-256 implementation for bootloader image verification.
  *           No external dependencies beyond <stdint.h> and <string.h>.
  *           ~1.5 KB Flash, ~100 bytes stack.  Performance on Cortex-M4F @180 MHz:
  *           ~30 ms for 48 KB input (acceptable for one-time boot verification).
  *
  * Copyright (c) 2025, Project Authors. All rights reserved.
  * Licensed under BSD 2-Clause.
  *
  **************************************************************************
  */

#ifndef __SHA256_H
#define __SHA256_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* exported constants --------------------------------------------------------*/

#define SHA256_BLOCK_SIZE   64U    /*!< input block size in bytes */
#define SHA256_DIGEST_SIZE  32U    /*!< output hash size in bytes */

/* exported types ------------------------------------------------------------*/

/**
 * @brief  SHA-256 context structure
 */
typedef struct
{
  uint32_t state[8];        /*!< intermediate hash state (H0..H7) */
  uint64_t bit_count;       /*!< total bits processed so far */
  uint8_t  buffer[64];      /*!< partial input block buffer */
  uint32_t buffer_len;      /*!< bytes currently in buffer */
} sha256_ctx_t;

/* exported functions --------------------------------------------------------*/

/**
 * @brief  initialize SHA-256 context with standard IV
 * @param  ctx: pointer to context to initialize
 * @retval none
 */
void sha256_init(sha256_ctx_t *ctx);

/**
 * @brief  feed data into the SHA-256 hash computation
 * @param  ctx:  pointer to active SHA-256 context
 * @param  data: pointer to input data
 * @param  len:  number of bytes to process
 * @retval none
 */
void sha256_update(sha256_ctx_t *ctx, const void *data, uint32_t len);

/**
 * @brief  finalize and produce the 32-byte SHA-256 digest
 * @param  ctx:  pointer to active SHA-256 context
 * @param  hash: output buffer (must be >= 32 bytes)
 * @retval none
 */
void sha256_final(sha256_ctx_t *ctx, uint8_t *hash);

/**
 * @brief  one-shot SHA-256 hash (convenience wrapper)
 * @param  data:      pointer to input data
 * @param  len:       number of bytes to hash
 * @param  hash:      output buffer (must be >= 32 bytes)
 * @retval none
 */
void sha256_hash(const void *data, uint32_t len, uint8_t *hash);

#ifdef __cplusplus
}
#endif

#endif /* __SHA256_H */
