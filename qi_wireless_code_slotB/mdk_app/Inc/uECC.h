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

#ifndef _UECC_H_
#define _UECC_H_

#include <stdint.h>

/**
 * @brief  Verify an ECDSA signature over secp256r1 (P-256).
 * @param  public_key: 65-byte uncompressed SEC1 public key point (04 || x || y)
 * @param  message_hash: 32-byte SHA-256 hash of the message
 * @param  signature: 64-byte IEEE P1363 signature (R || S, each 32 bytes)
 * @return 1 if the signature is valid, 0 if invalid
 */
int uECC_verify(const uint8_t *public_key,
                const uint8_t *message_hash,
                const uint8_t *signature);

/**
 * @brief  Optional progress hook for long uECC_verify (Fermat + point_mul).
 *         Boot uses this to refresh SIT1145 and re-send NRC 0x78.
 *         Pass NULL to clear. Not used on the APP path.
 */
void uECC_set_progress_cb(void (*cb)(void));

#endif /* _UECC_H_ */
