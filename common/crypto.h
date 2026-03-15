#pragma once
/*
 * Self-contained ChaCha20-Poly1305 AEAD (RFC 8439)
 *
 * No external dependencies. Drop-in header-only.
 *
 * API:
 *   chacha20poly1305_encrypt(out, in, inlen, aad, aadlen, key32, nonce12)
 *   chacha20poly1305_decrypt(out, in, inlen, aad, aadlen, key32, nonce12)
 *     returns 0 on success, -1 on auth failure (decrypt only)
 *
 * Output of encrypt = inlen + 16 bytes (appended Poly1305 tag)
 * Input  of decrypt = ciphertext + 16 byte tag; output = plaintext (inlen-16)
 */

#include <stdint.h>
#include <string.h>

/* ── ChaCha20 ────────────────────────────────────────────────────────────── */

#define ROTL32(v,n) (((v)<<(n))|((v)>>(32-(n))))
#define QR(a,b,c,d) \
    a+=b; d^=a; d=ROTL32(d,16); \
    c+=d; b^=c; b=ROTL32(b,12); \
    a+=b; d^=a; d=ROTL32(d, 8); \
    c+=d; b^=c; b=ROTL32(b, 7)

static inline void chacha20_block(uint32_t out[16], const uint32_t in[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0],x[4],x[ 8],x[12]); QR(x[1],x[5],x[ 9],x[13]);
        QR(x[2],x[6],x[10],x[14]); QR(x[3],x[7],x[11],x[15]);
        QR(x[0],x[5],x[10],x[15]); QR(x[1],x[6],x[11],x[12]);
        QR(x[2],x[7],x[ 8],x[13]); QR(x[3],x[4],x[ 9],x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

static inline void chacha20_init(uint32_t st[16],
                                  const uint8_t key[32],
                                  const uint8_t nonce[12],
                                  uint32_t counter) {
    /* "expa" "nd 3" "2-by" "te k" */
    st[0]=0x61707865; st[1]=0x3320646e; st[2]=0x79622d32; st[3]=0x6b206574;
    for (int i = 0; i < 8; i++) {
        const uint8_t *k = key + i*4;
        st[4+i] = (uint32_t)k[0] | ((uint32_t)k[1]<<8)
                | ((uint32_t)k[2]<<16) | ((uint32_t)k[3]<<24);
    }
    st[12] = counter;
    for (int i = 0; i < 3; i++) {
        const uint8_t *n = nonce + i*4;
        st[13+i] = (uint32_t)n[0] | ((uint32_t)n[1]<<8)
                 | ((uint32_t)n[2]<<16) | ((uint32_t)n[3]<<24);
    }
}

/* XOR src with ChaCha20 keystream into dst. counter=1 per RFC 8439 §2.6 */
static inline void chacha20_xor(uint8_t *dst, const uint8_t *src, size_t len,
                                 const uint8_t key[32], const uint8_t nonce[12],
                                 uint32_t counter) {
    uint32_t st[16], blk[16];
    chacha20_init(st, key, nonce, counter);
    while (len > 0) {
        chacha20_block(blk, st);
        st[12]++;
        size_t n = len < 64 ? len : 64;
        const uint8_t *b = (const uint8_t *)blk;
        for (size_t i = 0; i < n; i++) dst[i] = src[i] ^ b[i];
        dst += n; src += n; len -= n;
    }
}

/* ── Poly1305 ────────────────────────────────────────────────────────────── */

typedef struct { uint32_t r[5]; uint32_t h[5]; uint32_t pad[4]; } poly1305_t;

static inline void poly1305_init(poly1305_t *ctx, const uint8_t key[32]) {
    /* clamp r */
    uint32_t t0 = (uint32_t)key[ 0]|((uint32_t)key[ 1]<<8)|((uint32_t)key[ 2]<<16)|((uint32_t)key[ 3]<<24);
    uint32_t t1 = (uint32_t)key[ 4]|((uint32_t)key[ 5]<<8)|((uint32_t)key[ 6]<<16)|((uint32_t)key[ 7]<<24);
    uint32_t t2 = (uint32_t)key[ 8]|((uint32_t)key[ 9]<<8)|((uint32_t)key[10]<<16)|((uint32_t)key[11]<<24);
    uint32_t t3 = (uint32_t)key[12]|((uint32_t)key[13]<<8)|((uint32_t)key[14]<<16)|((uint32_t)key[15]<<24);
    ctx->r[0] =  t0                & 0x3ffffff;
    ctx->r[1] = ((t0>>26)|(t1<< 6))& 0x3ffff03;
    ctx->r[2] = ((t1>>20)|(t2<<12))& 0x3ffc0ff;
    ctx->r[3] = ((t2>>14)|(t3<<18))& 0x3f03fff;
    ctx->r[4] =  (t3>> 8)          & 0x00fffff;
    ctx->h[0]=ctx->h[1]=ctx->h[2]=ctx->h[3]=ctx->h[4]=0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = key+16+i*4;
        ctx->pad[i]=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    }
}

static inline void poly1305_block(poly1305_t *ctx, const uint8_t *m, uint32_t hibit) {
    uint32_t r0=ctx->r[0],r1=ctx->r[1],r2=ctx->r[2],r3=ctx->r[3],r4=ctx->r[4];
    uint32_t h0=ctx->h[0],h1=ctx->h[1],h2=ctx->h[2],h3=ctx->h[3],h4=ctx->h[4];
    h0 += ((uint32_t)m[ 0]|((uint32_t)m[ 1]<<8)|((uint32_t)m[ 2]<<16)|((uint32_t)m[ 3]<<24)) & 0x3ffffff;
    h1 += (((uint32_t)m[ 3]>>2)|((uint32_t)m[ 4]<<6)|((uint32_t)m[ 5]<<14)|((uint32_t)m[ 6]<<22)) & 0x3ffffff;
    h2 += (((uint32_t)m[ 6]>>4)|((uint32_t)m[ 7]<<4)|((uint32_t)m[ 8]<<12)|((uint32_t)m[ 9]<<20)) & 0x3ffffff;
    h3 += (((uint32_t)m[ 9]>>6)|((uint32_t)m[10]<<2)|((uint32_t)m[11]<<10)|((uint32_t)m[12]<<18)) & 0x3ffffff;
    h4 += (((uint32_t)m[12]>>8)|((uint32_t)m[13]<<0)|((uint32_t)m[14]<<8)|((uint32_t)m[15]<<16)) & 0x3ffffff;
    h4 += hibit;
    uint64_t d0=(uint64_t)h0*r0+(uint64_t)h1*5*r4+(uint64_t)h2*5*r3+(uint64_t)h3*5*r2+(uint64_t)h4*5*r1;
    uint64_t d1=(uint64_t)h0*r1+(uint64_t)h1*  r0+(uint64_t)h2*5*r4+(uint64_t)h3*5*r3+(uint64_t)h4*5*r2;
    uint64_t d2=(uint64_t)h0*r2+(uint64_t)h1*  r1+(uint64_t)h2*  r0+(uint64_t)h3*5*r4+(uint64_t)h4*5*r3;
    uint64_t d3=(uint64_t)h0*r3+(uint64_t)h1*  r2+(uint64_t)h2*  r1+(uint64_t)h3*  r0+(uint64_t)h4*5*r4;
    uint64_t d4=(uint64_t)h0*r4+(uint64_t)h1*  r3+(uint64_t)h2*  r2+(uint64_t)h3*  r1+(uint64_t)h4*  r0;
    uint32_t c; c=(uint32_t)(d0>>26); h0=(uint32_t)d0&0x3ffffff; d1+=c;
                c=(uint32_t)(d1>>26); h1=(uint32_t)d1&0x3ffffff; d2+=c;
                c=(uint32_t)(d2>>26); h2=(uint32_t)d2&0x3ffffff; d3+=c;
                c=(uint32_t)(d3>>26); h3=(uint32_t)d3&0x3ffffff; d4+=c;
                c=(uint32_t)(d4>>26); h4=(uint32_t)d4&0x3ffffff; h0+=c*5;
    c=h0>>26; h0&=0x3ffffff; h1+=c;
    ctx->h[0]=h0;ctx->h[1]=h1;ctx->h[2]=h2;ctx->h[3]=h3;ctx->h[4]=h4;
}

static inline void poly1305_update(poly1305_t *ctx, const uint8_t *m, size_t len) {
    uint8_t buf[16];
    while (len >= 16) { poly1305_block(ctx, m, 1<<24); m+=16; len-=16; }
    if (len) {
        memcpy(buf,m,len); buf[len]=1;
        memset(buf+len+1,0,15-len);
        poly1305_block(ctx, buf, 0);
    }
}

static inline void poly1305_finish(poly1305_t *ctx, uint8_t mac[16]) {
    uint32_t h0=ctx->h[0],h1=ctx->h[1],h2=ctx->h[2],h3=ctx->h[3],h4=ctx->h[4];
    uint32_t c; c=h1>>26; h1&=0x3ffffff; h2+=c;
                c=h2>>26; h2&=0x3ffffff; h3+=c;
                c=h3>>26; h3&=0x3ffffff; h4+=c;
                c=h4>>26; h4&=0x3ffffff; h0+=c*5;
    c=h0>>26; h0&=0x3ffffff; h1+=c;
    uint32_t g0=h0+5,g1=h1+(g0>>26),g2=h2+(g1>>26),g3=h3+(g2>>26),g4=h4+(g3>>26);
    g0&=0x3ffffff;g1&=0x3ffffff;g2&=0x3ffffff;g3&=0x3ffffff;
    uint32_t mask = ~((g4>>26)-1); /* all-ones if g4>=2^26 i.e. h>=p */
    h0=(h0&~mask)|(g0&mask); h1=(h1&~mask)|(g1&mask);
    h2=(h2&~mask)|(g2&mask); h3=(h3&~mask)|(g3&mask); h4=(h4&~mask)|(g4&mask);
    uint64_t f; f=(uint64_t)((h0|(h1<<26))     )+(uint64_t)ctx->pad[0]; mac[ 0]=(uint8_t)f; mac[ 1]=(uint8_t)(f>>8); mac[ 2]=(uint8_t)(f>>16); mac[ 3]=(uint8_t)(f>>24); f>>=32;
    f+=(uint64_t)((h1>>6)|(h2<<20))+ctx->pad[1]; mac[ 4]=(uint8_t)f; mac[ 5]=(uint8_t)(f>>8); mac[ 6]=(uint8_t)(f>>16); mac[ 7]=(uint8_t)(f>>24); f>>=32;
    f+=(uint64_t)((h2>>12)|(h3<<14))+ctx->pad[2]; mac[ 8]=(uint8_t)f; mac[ 9]=(uint8_t)(f>>8); mac[10]=(uint8_t)(f>>16); mac[11]=(uint8_t)(f>>24); f>>=32;
    f+=(uint64_t)((h3>>18)|(h4<<8)) +ctx->pad[3]; mac[12]=(uint8_t)f; mac[13]=(uint8_t)(f>>8); mac[14]=(uint8_t)(f>>16); mac[15]=(uint8_t)(f>>24);
}

/* ── Pad to 16 bytes ─────────────────────────────────────────────────────── */
static inline void poly1305_pad16(poly1305_t *ctx, size_t len) {
    if (len % 16) {
        uint8_t z[16] = {0};
        poly1305_update(ctx, z, 16 - (len % 16));
    }
}

/* ── AEAD construction (RFC 8439 §2.8) ─────────────────────────────────── */

/* Generate Poly1305 one-time key from ChaCha20 counter=0 */
static inline void poly1305_keygen(uint8_t otk[32],
                                    const uint8_t key[32],
                                    const uint8_t nonce[12]) {
    uint32_t st[16], blk[16];
    chacha20_init(st, key, nonce, 0);
    chacha20_block(blk, st);
    memcpy(otk, blk, 32);
}

/*
 * Encrypt in-place style: writes ciphertext + 16-byte tag into out.
 * out must be at least inlen+16 bytes.
 */
static inline void chacha20poly1305_encrypt(uint8_t *out,
                                             const uint8_t *in, size_t inlen,
                                             const uint8_t *aad, size_t aadlen,
                                             const uint8_t key[32],
                                             const uint8_t nonce[12]) {
    uint8_t otk[32];
    poly1305_keygen(otk, key, nonce);

    /* Encrypt with counter=1 */
    chacha20_xor(out, in, inlen, key, nonce, 1);

    /* MAC: pad(aad) || pad(ciphertext) || le64(aadlen) || le64(cipherlen) */
    poly1305_t mac;
    poly1305_init(&mac, otk);
    if (aad && aadlen) poly1305_update(&mac, aad, aadlen);
    poly1305_pad16(&mac, aadlen);
    poly1305_update(&mac, out, inlen);
    poly1305_pad16(&mac, inlen);
    uint8_t lens[16];
    uint64_t a = (uint64_t)aadlen, c = (uint64_t)inlen;
    for (int i=0;i<8;i++){lens[i]=(uint8_t)(a>>(i*8));lens[8+i]=(uint8_t)(c>>(i*8));}
    poly1305_update(&mac, lens, 16);
    poly1305_finish(&mac, out + inlen);
}

/*
 * Decrypt. Returns 0 on success, -1 if tag mismatch.
 * inlen includes the 16-byte tag. out must be at least inlen-16 bytes.
 */
static inline int chacha20poly1305_decrypt(uint8_t *out,
                                            const uint8_t *in, size_t inlen,
                                            const uint8_t *aad, size_t aadlen,
                                            const uint8_t key[32],
                                            const uint8_t nonce[12]) {
    if (inlen < 16) return -1;
    size_t ctlen = inlen - 16;
    const uint8_t *tag = in + ctlen;

    uint8_t otk[32];
    poly1305_keygen(otk, key, nonce);

    poly1305_t mac;
    poly1305_init(&mac, otk);
    if (aad && aadlen) poly1305_update(&mac, aad, aadlen);
    poly1305_pad16(&mac, aadlen);
    poly1305_update(&mac, in, ctlen);
    poly1305_pad16(&mac, ctlen);
    uint8_t lens[16];
    uint64_t a = (uint64_t)aadlen, c = (uint64_t)ctlen;
    for (int i=0;i<8;i++){lens[i]=(uint8_t)(a>>(i*8));lens[8+i]=(uint8_t)(c>>(i*8));}
    poly1305_update(&mac, lens, 16);
    uint8_t computed[16];
    poly1305_finish(&mac, computed);

    /* Constant-time comparison */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= computed[i] ^ tag[i];
    if (diff) return -1;

    chacha20_xor(out, in, ctlen, key, nonce, 1);
    return 0;
}

/*
 * Derive a 32-byte key from a passphrase of any length.
 * Uses ChaCha20 to hash: encrypt 32 zero bytes with the passphrase
 * XOR'd into a fixed key slot, zero nonce.
 */
static inline void derive_key(uint8_t out[32], const char *passphrase) {
    uint8_t raw[32] = {0};
    size_t len = strlen(passphrase);
    for (size_t i = 0; i < len && i < 32; i++) raw[i] ^= (uint8_t)passphrase[i];
    /* Mix: if passphrase > 32 bytes, fold remainder in */
    for (size_t i = 32; i < len; i++) raw[i % 32] ^= (uint8_t)passphrase[i];
    /* ChaCha20-encrypt 32 zeros with raw key, zero nonce → 32 bytes of derived key */
    uint8_t zeros[32] = {0};
    uint8_t nonce[12] = {0};
    chacha20_xor(out, zeros, 32, raw, nonce, 0);
}
