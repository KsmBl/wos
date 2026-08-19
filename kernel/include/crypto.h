/* The cryptography WPA2 needs, and nothing else.
 *
 * Joining a protected network is, underneath, four pieces of arithmetic:
 *
 *   - the passphrase becomes a 256-bit pairwise master key by PBKDF2 over
 *     HMAC-SHA1, salted with the network's name and stretched 4096 times;
 *   - the master key and the two nonces become the session keys through a
 *     SHA-1 based pseudo-random function;
 *   - each handshake message carries a message integrity code, HMAC-SHA1
 *     truncated to sixteen bytes, that proves both sides derived the same key;
 *   - the group key arrives wrapped with AES in the RFC 3394 scheme, and the
 *     data frames themselves are protected with AES-CCM.
 *
 * So: SHA-1, HMAC, PBKDF2, AES, key unwrap, CCM.  All of them are small, all
 * of them have published test vectors, and the self-test checks every one
 * against those vectors at boot -- which matters more here than usual, because
 * a subtly wrong SHA-1 does not fail loudly, it just produces a handshake the
 * access point silently refuses.
 */
#ifndef WOS_CRYPTO_H
#define WOS_CRYPTO_H

#include "types.h"

#define SHA1_DIGEST_LEN 20
#define SHA1_BLOCK_LEN  64

/* ------------------------------------------------------------------ *
 *  SHA-1
 * ------------------------------------------------------------------ */

typedef struct {
    uint32_t state[5];
    uint64_t length;              /* message length in bytes, for the pad  */
    uint8_t  buf[SHA1_BLOCK_LEN];
    uint32_t buffered;            /* bytes sitting in buf                  */
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const void *data, uint32_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t out[SHA1_DIGEST_LEN]);

/* The whole thing in one call, for the common case. */
void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST_LEN]);

/* ------------------------------------------------------------------ *
 *  HMAC-SHA1, and what is built on it
 * ------------------------------------------------------------------ */

/* Keyed hash over a single buffer. */
void hmac_sha1(const uint8_t *key, uint32_t key_len,
               const void *data, uint32_t data_len,
               uint8_t out[SHA1_DIGEST_LEN]);

/* The same over several buffers in sequence, which is what the handshake
 * wants: the pseudo-random function hashes a label, a separator, some
 * addresses and nonces, and a counter, and copying all of that into one
 * buffer first would be pure bookkeeping. */
void hmac_sha1_vector(const uint8_t *key, uint32_t key_len,
                      int count, const uint8_t *const *data,
                      const uint32_t *len,
                      uint8_t out[SHA1_DIGEST_LEN]);

/* PBKDF2 with HMAC-SHA1 as the underlying function.  For WPA2 this is called
 * with 4096 iterations and a 32-byte output, and it is genuinely slow -- that
 * is the point of it -- so it runs once when a network is joined and the
 * result is worth keeping. */
void pbkdf2_sha1(const char *passphrase, const uint8_t *salt, uint32_t salt_len,
                 uint32_t iterations, uint8_t *out, uint32_t out_len);

/* The pseudo-random function from 802.11, clause 12: HMAC-SHA1 run repeatedly
 * with an increasing counter byte and the output concatenated, then truncated.
 * `label` is a NUL-terminated string and its terminator is part of the input,
 * which is easy to get wrong and impossible to notice afterwards. */
void sha1_prf(const uint8_t *key, uint32_t key_len, const char *label,
              const uint8_t *data, uint32_t data_len,
              uint8_t *out, uint32_t out_len);

/* ------------------------------------------------------------------ *
 *  AES
 * ------------------------------------------------------------------ */

#define AES_BLOCK_LEN 16
#define AES_MAX_ROUNDS 14

typedef struct {
    uint32_t round_key[4 * (AES_MAX_ROUNDS + 1)];
    int      rounds;
} aes_ctx_t;

/* Expand a 16-, 24- or 32-byte key.  Returns false for any other length.
 *
 * Both directions read the same schedule -- decryption walks it backwards
 * rather than needing one of its own -- so these two are the same function
 * under two names, kept apart because a caller reads better for saying which
 * way it means to go. */
bool aes_set_encrypt_key(aes_ctx_t *ctx, const uint8_t *key, uint32_t key_len);
bool aes_set_decrypt_key(aes_ctx_t *ctx, const uint8_t *key, uint32_t key_len);

/* One block, in place-safe form (`in` and `out` may be the same buffer). */
void aes_encrypt_block(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK_LEN],
                       uint8_t out[AES_BLOCK_LEN]);
void aes_decrypt_block(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK_LEN],
                       uint8_t out[AES_BLOCK_LEN]);

/* ------------------------------------------------------------------ *
 *  AES key unwrap (RFC 3394)
 * ------------------------------------------------------------------ */

/* Unwrap `in_len` bytes -- a multiple of eight, at least sixteen -- into
 * `in_len - 8` bytes of plaintext.  This is how the group key travels inside
 * the third handshake message.
 *
 * Returns false when the integrity check value does not come back as the
 * constant RFC 3394 specifies, which is the signal that the key encryption
 * key was wrong: in practice, that the passphrase was wrong. */
bool aes_unwrap_key(const uint8_t *kek, uint32_t kek_len,
                    const uint8_t *in, uint32_t in_len, uint8_t *out);

/* ------------------------------------------------------------------ *
 *  AES-CCM, as CCMP uses it
 * ------------------------------------------------------------------ */

/* Encrypt `len` bytes in place and append an 8-byte authentication tag, over
 * a 13-byte nonce and `aad_len` bytes of additional data that are
 * authenticated but not encrypted (the frame header fields CCMP covers).
 *
 * `out` needs room for `len + 8` bytes. */
void aes_ccm_encrypt(const uint8_t *key, const uint8_t nonce[13],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *in, uint32_t len, uint8_t *out);

/* The reverse.  `len` counts the tag, so the plaintext is `len - 8` bytes.
 * Returns false when the tag does not match, meaning the frame was forged or
 * corrupted and must be dropped rather than delivered. */
bool aes_ccm_decrypt(const uint8_t *key, const uint8_t nonce[13],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *in, uint32_t len, uint8_t *out);

/* ------------------------------------------------------------------ *
 *  Comparison
 * ------------------------------------------------------------------ */

/* Compare two buffers in time that does not depend on where they first
 * differ.  Used for every check on a value an attacker supplies -- a message
 * integrity code, an authentication tag -- because the obvious memcmp returns
 * early and tells them how much of their guess was right. */
bool crypto_equal(const void *a, const void *b, uint32_t len);

#endif /* WOS_CRYPTO_H */
