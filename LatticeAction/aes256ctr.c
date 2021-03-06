/*
  crypto_stream_aes256ctr
  based heavily on public-domain code by Romain Dolbeau
  Different handling of nonce+counter than original version
  using separated 96-bit nonce and internal 32-bit counter, starting from zero
  Public Domain
*/

#include <stdint.h>
#include <immintrin.h>
#include "aes256ctr.h"

static inline void aesni_encrypt4(unsigned char *out,
                                  __m128i *n,
                                  const __m128i rkeys[16])
{
  __m128i f0,f1,f2,f3,t;

  /* Load current counter value */
  __m128i nv0i = _mm_load_si128(n);

  /* Increase counter in 4 consecutive blocks */
  t  = _mm_set_epi8(8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7);
  f0 = _mm_shuffle_epi8(_mm_add_epi32(nv0i,_mm_set_epi64x(0,0)),t);
  f1 = _mm_shuffle_epi8(_mm_add_epi32(nv0i,_mm_set_epi64x(1,0)),t);
  f2 = _mm_shuffle_epi8(_mm_add_epi32(nv0i,_mm_set_epi64x(2,0)),t);
  f3 = _mm_shuffle_epi8(_mm_add_epi32(nv0i,_mm_set_epi64x(3,0)),t);

  /* Write counter for next iteration, increased by 4 */
  _mm_store_si128(n,_mm_add_epi32(nv0i,_mm_set_epi64x(4,0)));

  /* Actual AES encryption, 4x interleaved */
  t  = _mm_load_si128(&rkeys[0]);
  f0 = _mm_xor_si128(f0,t);
  f1 = _mm_xor_si128(f1,t);
  f2 = _mm_xor_si128(f2,t);
  f3 = _mm_xor_si128(f3,t);

  for (int i = 1; i < 14; i++) {
    t  = _mm_load_si128(&rkeys[i]);
    f0 = _mm_aesenc_si128(f0,t);
    f1 = _mm_aesenc_si128(f1,t);
    f2 = _mm_aesenc_si128(f2,t);
    f3 = _mm_aesenc_si128(f3,t);
  }

  t  = _mm_load_si128(&rkeys[14]);
  f0 = _mm_aesenclast_si128(f0,t);
  f1 = _mm_aesenclast_si128(f1,t);
  f2 = _mm_aesenclast_si128(f2,t);
  f3 = _mm_aesenclast_si128(f3,t);

  /* Write results */
  _mm_storeu_si128((__m128i*)(out+  0),f0);
  _mm_storeu_si128((__m128i*)(out+ 16),f1);
  _mm_storeu_si128((__m128i*)(out+ 32),f2);
  _mm_storeu_si128((__m128i*)(out+ 48),f3);
}

void aes256ctr_init(aes256ctr_ctx *state,
                    const unsigned char *key,
                    uint16_t nonce)
{
  __m128i key0 = _mm_loadu_si128((__m128i *)(key+0));
  __m128i key1 = _mm_loadu_si128((__m128i *)(key+16));
  __m128i temp0, temp1, temp2, temp4;
  int idx = 0;

  state->n = _mm_set_epi64x(0, (uint64_t)((nonce >> 8) | (nonce << 8)) << 48);

  state->rkeys[idx++] = key0;
  temp0 = key0;
  temp2 = key1;
  temp4 = _mm_setzero_si128();

#define BLOCK1(IMM)                                                     \
  temp1 = _mm_aeskeygenassist_si128(temp2, IMM);                        \
  state->rkeys[idx++] = temp2;                                          \
  temp4 = (__m128i)_mm_shuffle_ps((__m128)temp4, (__m128)temp0, 0x10);  \
  temp0 = _mm_xor_si128(temp0, temp4);                                  \
  temp4 = (__m128i)_mm_shuffle_ps((__m128)temp4, (__m128)temp0, 0x8c);  \
  temp0 = _mm_xor_si128(temp0, temp4);                                  \
  temp1 = (__m128i)_mm_shuffle_ps((__m128)temp1, (__m128)temp1, 0xff);  \
  temp0 = _mm_xor_si128(temp0, temp1)

#define BLOCK2(IMM)                                                     \
  temp1 = _mm_aeskeygenassist_si128(temp0, IMM);                        \
  state->rkeys[idx++] = temp0;                                          \
  temp4 = (__m128i)_mm_shuffle_ps((__m128)temp4, (__m128)temp2, 0x10);  \
  temp2 = _mm_xor_si128(temp2, temp4);                                  \
  temp4 = (__m128i)_mm_shuffle_ps((__m128)temp4, (__m128)temp2, 0x8c);  \
  temp2 = _mm_xor_si128(temp2, temp4);                                  \
  temp1 = (__m128i)_mm_shuffle_ps((__m128)temp1, (__m128)temp1, 0xaa);  \
  temp2 = _mm_xor_si128(temp2, temp1)

  BLOCK1(0x01);
  BLOCK2(0x01);

  BLOCK1(0x02);
  BLOCK2(0x02);

  BLOCK1(0x04);
  BLOCK2(0x04);

  BLOCK1(0x08);
  BLOCK2(0x08);

  BLOCK1(0x10);
  BLOCK2(0x10);

  BLOCK1(0x20);
  BLOCK2(0x20);

  BLOCK1(0x40);
  state->rkeys[idx++] = temp0;
}

void aes256ctr_select(aes256ctr_ctx *state, uint16_t nonce) {
  state->n = _mm_set_epi64x(0, (uint64_t)((nonce << 8) | (nonce >> 8)) << 48);
}

void aes256ctr_squeezeblocks(unsigned char *out,
                             unsigned long long nblocks,
                             aes256ctr_ctx *state)
{
  unsigned long long i;

  for(i=0;i<nblocks;i++) {
    aesni_encrypt4(out, &state->n, state->rkeys);
    out += 64;
  }
}

void aes256ctr_prf(unsigned char *out,
                   unsigned long long outlen,
                   const unsigned char *seed,
                   unsigned char nonce)
{
  unsigned int i;
  unsigned char buf[64];
  aes256ctr_ctx state;

  aes256ctr_init(&state, seed, nonce);

  while(outlen >= 64) {
    aesni_encrypt4(out, &state.n, state.rkeys);
    outlen -= 64;
  }

  if(outlen) {
    aesni_encrypt4(buf, &state.n, state.rkeys);
    for(i=0;i<outlen;i++)
      out[i] = buf[i];
  }
}

