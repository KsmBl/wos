/* SHA-1, HMAC, PBKDF2, AES, key unwrap and CCM.  See crypto.h.
 *
 * Everything here is written for clarity rather than speed.  The one place
 * where speed would matter is PBKDF2, which runs 8192 HMACs to turn a
 * passphrase into a master key, and that happens once per network joined --
 * a second of arithmetic on a machine that is otherwise idle waiting for an
 * access point to answer.  The AES here is the plain byte-oriented cipher
 * from FIPS-197 rather than the four-table version, for the same reason:
 * frames are protected by the adapter's own hardware in the normal path, and
 * the software cipher only ever unwraps a group key.
 */

#include "crypto.h"
#include "string.h"

/* ------------------------------------------------------------------ *
 *  SHA-1
 * ------------------------------------------------------------------ */

static inline uint32_t rol32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;

        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_init(sha1_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->length   = 0;
    ctx->buffered = 0;
}

void sha1_update(sha1_ctx_t *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    ctx->length += len;

    /* Top up a partly filled buffer first, so the fast path below always
     * starts on a block boundary. */
    if (ctx->buffered) {
        uint32_t want = SHA1_BLOCK_LEN - ctx->buffered;
        uint32_t take = len < want ? len : want;

        memcpy(ctx->buf + ctx->buffered, p, take);
        ctx->buffered += take;
        p += take;
        len -= take;

        if (ctx->buffered < SHA1_BLOCK_LEN)
            return;

        sha1_block(ctx->state, ctx->buf);
        ctx->buffered = 0;
    }

    while (len >= SHA1_BLOCK_LEN) {
        sha1_block(ctx->state, p);
        p += SHA1_BLOCK_LEN;
        len -= SHA1_BLOCK_LEN;
    }

    if (len) {
        memcpy(ctx->buf, p, len);
        ctx->buffered = len;
    }
}

void sha1_final(sha1_ctx_t *ctx, uint8_t out[SHA1_DIGEST_LEN])
{
    uint64_t bits = ctx->length * 8;

    /* Pad with a single one bit, then zeroes, then the length -- and if the
     * length will not fit in this block, spill into another one. */
    uint8_t pad = 0x80;
    sha1_update(ctx, &pad, 1);

    uint8_t zero = 0;
    while (ctx->buffered != SHA1_BLOCK_LEN - 8)
        sha1_update(ctx, &zero, 1);

    uint8_t len_be[8];
    for (int i = 0; i < 8; i++)
        len_be[i] = (uint8_t)(bits >> (56 - 8 * i));

    /* sha1_update would add these eight bytes to the running length, but the
     * length is already committed above, so place them by hand. */
    memcpy(ctx->buf + ctx->buffered, len_be, 8);
    sha1_block(ctx->state, ctx->buf);
    ctx->buffered = 0;

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha1(const void *data, uint32_t len, uint8_t out[SHA1_DIGEST_LEN])
{
    sha1_ctx_t ctx;

    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

/* ------------------------------------------------------------------ *
 *  HMAC-SHA1
 * ------------------------------------------------------------------ */

void hmac_sha1_vector(const uint8_t *key, uint32_t key_len,
                      int count, const uint8_t *const *data,
                      const uint32_t *len,
                      uint8_t out[SHA1_DIGEST_LEN])
{
    uint8_t k[SHA1_BLOCK_LEN];
    uint8_t pad[SHA1_BLOCK_LEN];
    uint8_t inner[SHA1_DIGEST_LEN];
    sha1_ctx_t ctx;

    /* A key longer than the block is replaced by its own digest; anything
     * shorter is zero-extended. */
    memset(k, 0, sizeof(k));
    if (key_len > SHA1_BLOCK_LEN)
        sha1(key, key_len, k);
    else
        memcpy(k, key, key_len);

    for (int i = 0; i < SHA1_BLOCK_LEN; i++)
        pad[i] = k[i] ^ 0x36;

    sha1_init(&ctx);
    sha1_update(&ctx, pad, SHA1_BLOCK_LEN);
    for (int i = 0; i < count; i++)
        sha1_update(&ctx, data[i], len[i]);
    sha1_final(&ctx, inner);

    for (int i = 0; i < SHA1_BLOCK_LEN; i++)
        pad[i] = k[i] ^ 0x5C;

    sha1_init(&ctx);
    sha1_update(&ctx, pad, SHA1_BLOCK_LEN);
    sha1_update(&ctx, inner, SHA1_DIGEST_LEN);
    sha1_final(&ctx, out);
}

void hmac_sha1(const uint8_t *key, uint32_t key_len,
               const void *data, uint32_t data_len,
               uint8_t out[SHA1_DIGEST_LEN])
{
    const uint8_t *addr[1] = { (const uint8_t *)data };
    uint32_t       lens[1] = { data_len };

    hmac_sha1_vector(key, key_len, 1, addr, lens, out);
}

/* ------------------------------------------------------------------ *
 *  PBKDF2 and the 802.11 pseudo-random function
 * ------------------------------------------------------------------ */

/* One block of PBKDF2 output: U1 = HMAC(P, S || i), then each further U is
 * the HMAC of the one before, and all of them are exclusive-ored together. */
static void pbkdf2_block(const char *passphrase, uint32_t pass_len,
                         const uint8_t *salt, uint32_t salt_len,
                         uint32_t iterations, uint32_t index,
                         uint8_t out[SHA1_DIGEST_LEN])
{
    uint8_t counter[4] = {
        (uint8_t)(index >> 24), (uint8_t)(index >> 16),
        (uint8_t)(index >> 8),  (uint8_t)(index)
    };
    const uint8_t *addr[2] = { salt, counter };
    uint32_t       lens[2] = { salt_len, 4 };
    uint8_t        u[SHA1_DIGEST_LEN];

    hmac_sha1_vector((const uint8_t *)passphrase, pass_len, 2, addr, lens, u);
    memcpy(out, u, SHA1_DIGEST_LEN);

    for (uint32_t i = 1; i < iterations; i++) {
        hmac_sha1((const uint8_t *)passphrase, pass_len, u, SHA1_DIGEST_LEN, u);
        for (int j = 0; j < SHA1_DIGEST_LEN; j++)
            out[j] ^= u[j];
    }
}

void pbkdf2_sha1(const char *passphrase, const uint8_t *salt, uint32_t salt_len,
                 uint32_t iterations, uint8_t *out, uint32_t out_len)
{
    uint32_t pass_len = (uint32_t)strlen(passphrase);
    uint32_t done = 0;

    for (uint32_t index = 1; done < out_len; index++) {
        uint8_t  block[SHA1_DIGEST_LEN];
        uint32_t take = out_len - done;

        if (take > SHA1_DIGEST_LEN)
            take = SHA1_DIGEST_LEN;

        pbkdf2_block(passphrase, pass_len, salt, salt_len, iterations, index, block);
        memcpy(out + done, block, take);
        done += take;
    }
}

void sha1_prf(const uint8_t *key, uint32_t key_len, const char *label,
              const uint8_t *data, uint32_t data_len,
              uint8_t *out, uint32_t out_len)
{
    uint8_t counter = 0;
    /* The label's terminating NUL is part of the hashed input.  This is not a
     * quirk of any one implementation -- the standard writes the input as the
     * string followed by a zero octet -- but it is invisible in a capture and
     * produces keys that are wrong in a way nothing reports. */
    const uint8_t *addr[3] = { (const uint8_t *)label, data, &counter };
    uint32_t       lens[3] = { (uint32_t)strlen(label) + 1, data_len, 1 };
    uint32_t       done = 0;

    while (done < out_len) {
        uint8_t  digest[SHA1_DIGEST_LEN];
        uint32_t take = out_len - done;

        if (take > SHA1_DIGEST_LEN)
            take = SHA1_DIGEST_LEN;

        hmac_sha1_vector(key, key_len, 3, addr, lens, digest);
        memcpy(out + done, digest, take);

        done += take;
        counter++;
    }
}

/* ------------------------------------------------------------------ *
 *  AES
 * ------------------------------------------------------------------ */

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Multiply by x in GF(2^8), reducing by the AES polynomial when the top bit
 * falls off the end. */
static inline uint8_t xtime(uint8_t v)
{
    return (uint8_t)((v << 1) ^ ((v & 0x80) ? 0x1B : 0x00));
}

/* Full GF(2^8) multiply, by the obvious shift-and-add.  Only ever called with
 * the small constants the MixColumns matrices contain. */
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t result = 0;

    while (b) {
        if (b & 1)
            result ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return result;
}

static uint32_t sub_word(uint32_t w)
{
    return ((uint32_t)sbox[(w >> 24) & 0xFF] << 24) |
           ((uint32_t)sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)sbox[(w >> 8) & 0xFF] << 8) |
           ((uint32_t)sbox[w & 0xFF]);
}

static bool aes_expand_key(aes_ctx_t *ctx, const uint8_t *key, uint32_t key_len)
{
    uint32_t nk;

    switch (key_len) {
    case 16: nk = 4; break;
    case 24: nk = 6; break;
    case 32: nk = 8; break;
    default: return false;
    }

    ctx->rounds = (int)nk + 6;

    for (uint32_t i = 0; i < nk; i++)
        ctx->round_key[i] = ((uint32_t)key[4 * i] << 24) |
                            ((uint32_t)key[4 * i + 1] << 16) |
                            ((uint32_t)key[4 * i + 2] << 8) |
                            ((uint32_t)key[4 * i + 3]);

    uint8_t rcon = 1;

    for (uint32_t i = nk; i < 4 * ((uint32_t)ctx->rounds + 1); i++) {
        uint32_t t = ctx->round_key[i - 1];

        if (i % nk == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ ((uint32_t)rcon << 24);
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }

        ctx->round_key[i] = ctx->round_key[i - nk] ^ t;
    }

    return true;
}

/* The forward and inverse ciphers here both read the schedule produced above,
 * the inverse simply walking it backwards, so there is one expansion and the
 * two entry points differ only in name. */
bool aes_set_encrypt_key(aes_ctx_t *ctx, const uint8_t *key, uint32_t key_len)
{
    return aes_expand_key(ctx, key, key_len);
}

bool aes_set_decrypt_key(aes_ctx_t *ctx, const uint8_t *key, uint32_t key_len)
{
    return aes_expand_key(ctx, key, key_len);
}

/* The state is the block in column-major order: byte 4c+r is row r of
 * column c, which is what makes ShiftRows a row operation on strided bytes. */
static void add_round_key(uint8_t state[16], const uint32_t *w)
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            state[4 * c + r] ^= (uint8_t)(w[c] >> (24 - 8 * r));
}

void aes_encrypt_block(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK_LEN],
                       uint8_t out[AES_BLOCK_LEN])
{
    uint8_t state[16];

    memcpy(state, in, 16);
    add_round_key(state, ctx->round_key);

    for (int round = 1; round <= ctx->rounds; round++) {
        uint8_t tmp[16];

        for (int i = 0; i < 16; i++)
            state[i] = sbox[state[i]];

        /* ShiftRows: row r moves left by r columns. */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                tmp[4 * c + r] = state[4 * ((c + r) & 3) + r];
        memcpy(state, tmp, 16);

        /* The last round has no MixColumns -- that omission is what makes
         * the cipher invertible with the same structure. */
        if (round != ctx->rounds) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = state[4 * c], a1 = state[4 * c + 1];
                uint8_t a2 = state[4 * c + 2], a3 = state[4 * c + 3];

                state[4 * c]     = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                state[4 * c + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                state[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                state[4 * c + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }

        add_round_key(state, ctx->round_key + 4 * round);
    }

    memcpy(out, state, 16);
}

void aes_decrypt_block(const aes_ctx_t *ctx, const uint8_t in[AES_BLOCK_LEN],
                       uint8_t out[AES_BLOCK_LEN])
{
    uint8_t state[16];

    memcpy(state, in, 16);
    add_round_key(state, ctx->round_key + 4 * ctx->rounds);

    for (int round = ctx->rounds - 1; round >= 0; round--) {
        uint8_t tmp[16];

        /* InvShiftRows: row r moves right by r columns. */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                tmp[4 * c + r] = state[4 * ((c - r + 4) & 3) + r];
        memcpy(state, tmp, 16);

        for (int i = 0; i < 16; i++)
            state[i] = inv_sbox[state[i]];

        add_round_key(state, ctx->round_key + 4 * round);

        if (round != 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = state[4 * c], a1 = state[4 * c + 1];
                uint8_t a2 = state[4 * c + 2], a3 = state[4 * c + 3];

                state[4 * c]     = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^
                                             gmul(a2, 13) ^ gmul(a3, 9));
                state[4 * c + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^
                                             gmul(a2, 11) ^ gmul(a3, 13));
                state[4 * c + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^
                                             gmul(a2, 14) ^ gmul(a3, 11));
                state[4 * c + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^
                                             gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
    }

    memcpy(out, state, 16);
}

/* ------------------------------------------------------------------ *
 *  AES key unwrap (RFC 3394)
 * ------------------------------------------------------------------ */

bool aes_unwrap_key(const uint8_t *kek, uint32_t kek_len,
                    const uint8_t *in, uint32_t in_len, uint8_t *out)
{
    static const uint8_t iv[8] = {
        0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6
    };
    aes_ctx_t ctx;
    uint8_t   a[8];
    uint32_t  n;

    if (in_len < 16 || (in_len % 8) != 0)
        return false;
    if (!aes_set_decrypt_key(&ctx, kek, kek_len))
        return false;

    n = in_len / 8 - 1;

    memcpy(a, in, 8);
    memcpy(out, in + 8, n * 8);

    /* Six passes backwards over the blocks, each undoing one encryption and
     * one exclusive-or of the step counter into the accumulator. */
    for (int j = 5; j >= 0; j--) {
        for (uint32_t i = n; i >= 1; i--) {
            uint8_t  block[16];
            uint32_t t = n * (uint32_t)j + i;

            memcpy(block, a, 8);
            /* t is exclusive-ored into the low end of the 64-bit accumulator,
             * so only the last four bytes can ever be touched. */
            block[4] ^= (uint8_t)(t >> 24);
            block[5] ^= (uint8_t)(t >> 16);
            block[6] ^= (uint8_t)(t >> 8);
            block[7] ^= (uint8_t)(t);
            memcpy(block + 8, out + (i - 1) * 8, 8);

            aes_decrypt_block(&ctx, block, block);

            memcpy(a, block, 8);
            memcpy(out + (i - 1) * 8, block + 8, 8);
        }
    }

    /* The accumulator coming back as the constant is the whole integrity
     * check: a wrong key encryption key leaves it as noise. */
    return crypto_equal(a, iv, 8);
}

/* ------------------------------------------------------------------ *
 *  AES-CCM
 * ------------------------------------------------------------------ */

#define CCM_TAG_LEN   8
#define CCM_NONCE_LEN 13
#define CCM_L         2       /* 15 - nonce length: a 2-byte length field */

static void ccm_xor_block(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        dst[i] ^= src[i];
}

/* Build counter block A_i, which differs from the others only in its trailing
 * two-byte index. */
static void ccm_counter(uint8_t out[16], const uint8_t nonce[CCM_NONCE_LEN],
                        uint16_t index)
{
    out[0] = CCM_L - 1;
    memcpy(out + 1, nonce, CCM_NONCE_LEN);
    out[14] = (uint8_t)(index >> 8);
    out[15] = (uint8_t)(index);
}

/* The CBC-MAC half: B0 carries the flags, nonce and message length; the
 * additional data follows with a two-byte length in front of it, and both it
 * and the message are zero-padded up to a block. */
static void ccm_mac(const aes_ctx_t *ctx, const uint8_t nonce[CCM_NONCE_LEN],
                    const uint8_t *aad, uint32_t aad_len,
                    const uint8_t *plain, uint32_t plain_len,
                    uint8_t mac[16])
{
    uint8_t block[16];

    block[0] = (uint8_t)((aad_len ? 0x40 : 0x00) |
                         (((CCM_TAG_LEN - 2) / 2) << 3) |
                         (CCM_L - 1));
    memcpy(block + 1, nonce, CCM_NONCE_LEN);
    block[14] = (uint8_t)(plain_len >> 8);
    block[15] = (uint8_t)(plain_len);

    aes_encrypt_block(ctx, block, mac);

    if (aad_len) {
        uint8_t  head[2] = { (uint8_t)(aad_len >> 8), (uint8_t)(aad_len) };
        uint32_t offset = 0;

        /* The length prefix and the data are one continuous stream through
         * the chain, so the first block mixes both. */
        memset(block, 0, 16);
        memcpy(block, head, 2);

        uint32_t first = aad_len < 14 ? aad_len : 14;
        memcpy(block + 2, aad, first);
        offset = first;

        ccm_xor_block(mac, block, 16);
        aes_encrypt_block(ctx, mac, mac);

        while (offset < aad_len) {
            uint32_t take = aad_len - offset;

            if (take > 16)
                take = 16;
            memset(block, 0, 16);
            memcpy(block, aad + offset, take);

            ccm_xor_block(mac, block, 16);
            aes_encrypt_block(ctx, mac, mac);
            offset += take;
        }
    }

    for (uint32_t offset = 0; offset < plain_len; offset += 16) {
        uint32_t take = plain_len - offset;

        if (take > 16)
            take = 16;
        memset(block, 0, 16);
        memcpy(block, plain + offset, take);

        ccm_xor_block(mac, block, 16);
        aes_encrypt_block(ctx, mac, mac);
    }
}

/* Counter-mode over the message, starting at index one -- index zero is
 * reserved for masking the tag. */
static void ccm_ctr(const aes_ctx_t *ctx, const uint8_t nonce[CCM_NONCE_LEN],
                    const uint8_t *in, uint32_t len, uint8_t *out)
{
    uint16_t index = 1;

    for (uint32_t offset = 0; offset < len; offset += 16, index++) {
        uint8_t  counter[16], stream[16];
        uint32_t take = len - offset;

        if (take > 16)
            take = 16;

        ccm_counter(counter, nonce, index);
        aes_encrypt_block(ctx, counter, stream);

        for (uint32_t i = 0; i < take; i++)
            out[offset + i] = in[offset + i] ^ stream[i];
    }
}

void aes_ccm_encrypt(const uint8_t *key, const uint8_t nonce[CCM_NONCE_LEN],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *in, uint32_t len, uint8_t *out)
{
    aes_ctx_t ctx;
    uint8_t   mac[16], counter[16], s0[16];

    if (!aes_set_encrypt_key(&ctx, key, 16))
        return;

    ccm_mac(&ctx, nonce, aad, aad_len, in, len, mac);
    ccm_ctr(&ctx, nonce, in, len, out);

    ccm_counter(counter, nonce, 0);
    aes_encrypt_block(&ctx, counter, s0);

    for (int i = 0; i < CCM_TAG_LEN; i++)
        out[len + i] = mac[i] ^ s0[i];
}

bool aes_ccm_decrypt(const uint8_t *key, const uint8_t nonce[CCM_NONCE_LEN],
                     const uint8_t *aad, uint32_t aad_len,
                     const uint8_t *in, uint32_t len, uint8_t *out)
{
    aes_ctx_t ctx;
    uint8_t   mac[16], counter[16], s0[16], tag[CCM_TAG_LEN];
    uint32_t  plain_len;

    if (len < CCM_TAG_LEN)
        return false;
    plain_len = len - CCM_TAG_LEN;

    if (!aes_set_encrypt_key(&ctx, key, 16))
        return false;

    ccm_ctr(&ctx, nonce, in, plain_len, out);

    /* The tag is checked against the plaintext just recovered, so a forged
     * frame is decrypted before it is rejected -- harmless, because nothing
     * looks at `out` unless this returns true. */
    ccm_mac(&ctx, nonce, aad, aad_len, out, plain_len, mac);

    ccm_counter(counter, nonce, 0);
    aes_encrypt_block(&ctx, counter, s0);

    for (int i = 0; i < CCM_TAG_LEN; i++)
        tag[i] = mac[i] ^ s0[i];

    return crypto_equal(tag, in + plain_len, CCM_TAG_LEN);
}

/* ------------------------------------------------------------------ *
 *  Comparison
 * ------------------------------------------------------------------ */

bool crypto_equal(const void *a, const void *b, uint32_t len)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    uint8_t        diff = 0;

    /* Every byte is examined whatever the first one says, so the time taken
     * carries no information about where the difference is. */
    for (uint32_t i = 0; i < len; i++)
        diff |= (uint8_t)(x[i] ^ y[i]);

    return diff == 0;
}
