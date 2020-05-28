#include <stdint.h>
#include <immintrin.h>
#include "test/cpucycles.h"
#include "symmetric.h"
#ifdef USE_AES
#include "aes256ctr.h"
#else
#include "fips202x4.h"
#endif
#include "params.h"
#include "reduce.h"
#include "rounding.h"
#include "ntt.h"
#include "poly.h"
#include "rejsample.h"

#ifdef DBENCH
extern const unsigned long long timing_overhead;
extern unsigned long long *tred, *tadd, *tmul, *tround, *tsample, *tpack;
#endif

extern const uint32_t _8x2q[8] asm("_8x2q");

/*************************************************
* Name:        poly_reduce
*
* Description: Inplace reduction of all coefficients of polynomial to
*              representative in [0,2*Q[.
*
* Arguments:   - poly *a: pointer to input/output polynomial
**************************************************/
void poly_reduce(poly *a) {
  DBENCH_START();

  reduce_avx(a->coeffs);

  DBENCH_STOP(*tred);
}

/*************************************************
* Name:        poly_csubq
*
* Description: For all coefficients of in/out polynomial subtract Q if
*              coefficient is bigger than Q.
*
* Arguments:   - poly *a: pointer to input/output polynomial
**************************************************/
void poly_csubq(poly *a) {
  DBENCH_START();

  csubq_avx(a->coeffs);

  DBENCH_STOP(*tred);
}

/*************************************************
* Name:        poly_freeze
*
* Description: Inplace reduction of all coefficients of polynomial to
*              standard representatives.
*
* Arguments:   - poly *a: pointer to input/output polynomial
**************************************************/
void poly_freeze(poly *a) {
  DBENCH_START();

  reduce_avx(a->coeffs);
  csubq_avx(a->coeffs);

  DBENCH_STOP(*tred);
}

/*************************************************
* Name:        poly_add
*
* Description: Add polynomials. No modular reduction is performed.
*
* Arguments:   - poly *c: pointer to output polynomial
*              - const poly *a: pointer to first summand
*              - const poly *b: pointer to second summand
**************************************************/
void poly_add(poly *c, const poly *a, const poly *b)  {
  unsigned int i;
  __m256i vec0, vec1;
  DBENCH_START();

  for(i = 0; i < N; i += 8) {
    vec0 = _mm256_load_si256((__m256i *)&a->coeffs[i]);
    vec1 = _mm256_load_si256((__m256i *)&b->coeffs[i]);
    vec0 = _mm256_add_epi32(vec0, vec1);
    _mm256_store_si256((__m256i *)&c->coeffs[i], vec0);
  }

  DBENCH_STOP(*tadd);
}

/*************************************************
* Name:        poly_sub
*
* Description: Subtract polynomials. Assumes coefficients of second input
*              polynomial to be less than 2*Q. No modular reduction is
*              performed.
*
* Arguments:   - poly *c: pointer to output polynomial
*              - const poly *a: pointer to first input polynomial
*              - const poly *b: pointer to second input polynomial to be
*                               subtraced from first input polynomial
**************************************************/
void poly_sub(poly *c, const poly *a, const poly *b) {
  unsigned int i;
  __m256i vec0, vec1;
  const __m256i twoq = _mm256_load_si256((__m256i *)_8x2q);
  DBENCH_START();

  for(i = 0; i < N; i += 8) {
    vec0 = _mm256_load_si256((__m256i *)&a->coeffs[i]);
    vec1 = _mm256_load_si256((__m256i *)&b->coeffs[i]);
    vec0 = _mm256_add_epi32(vec0, twoq);
    vec0 = _mm256_sub_epi32(vec0, vec1);
    _mm256_store_si256((__m256i *)&c->coeffs[i], vec0);
  }

  DBENCH_STOP(*tadd);
}

/*************************************************
* Name:        poly_shiftl
*
* Description: Multiply polynomial by 2^D without modular reduction. Assumes
*              input coefficients to be less than 2^{32-D}.
*
* Arguments:   - poly *a: pointer to input/output polynomial
**************************************************/
void poly_shiftl(poly *a) {
  unsigned int i;
  __m256i vec;
  DBENCH_START();

  for(i = 0; i < N; i += 8) {
    vec = _mm256_load_si256((__m256i *)&a->coeffs[i]);
    vec = _mm256_slli_epi32(vec, D);
    _mm256_store_si256((__m256i *)&a->coeffs[i], vec);
  }

  DBENCH_STOP(*tmul);
}

/*************************************************
* Name:        poly_ntt
*
* Description: Inplace forward NTT. Output coefficients can be up to
*              16*Q larger than input coefficients.
*
* Arguments:   - poly *a: pointer to input/output polynomial
**************************************************/
void poly_ntt(poly *a) {
  unsigned int i;
  uint64_t __attribute__((aligned(32))) tmp[N];
  DBENCH_START();

  for(i = 0; i < N/32; ++i)
    ntt_levels0t2_avx(tmp + 4*i, a->coeffs + 4*i, zetas + 1);
  for(i = 0; i < N/32; ++i)
    ntt_levels3t8_avx(a->coeffs + 32*i, tmp + 32*i, zetas + 8 + 31*i);

  DBENCH_STOP(*tmul);
}

/*************************************************
* Name:        poly_invntt_tomont
*
* Description: Inplace inverse NTT and multiplication by 2^{32}.
*              Input coefficients need to be less than 2*Q.
*              Output coefficients are less than 2*Q.
*
* Arguments:   - poly *a: pointer to input/output polynomial
**************************************************/
void poly_invntt_tomont(poly *a) {
  unsigned int i;
  uint64_t __attribute__((aligned(32))) tmp[N];
  DBENCH_START();

  for(i = 0; i < N/32; i++)
    invntt_levels0t4_avx(tmp + 32*i, a->coeffs + 32*i, zetas_inv + 31*i);
  for(i = 0; i < N/32; i++)
    invntt_levels5t7_avx(a->coeffs + 4*i, tmp + 4*i, zetas_inv + 248);

  DBENCH_STOP(*tmul);
}

/*************************************************
* Name:        poly_pointwise_montgomery
*
* Description: Pointwise multiplication of polynomials in NTT domain
*              representation and multiplication of resulting polynomial
*              by 2^{-32}. Output coefficients are less than 2*Q if input
*              coefficient are less than 22*Q.
*
* Arguments:   - poly *c: pointer to output polynomial
*              - const poly *a: pointer to first input polynomial
*              - const poly *b: pointer to second input polynomial
**************************************************/
void poly_pointwise_montgomery(poly *c, const poly *a, const poly *b) {
  DBENCH_START();

  pointwise_avx(c->coeffs, a->coeffs, b->coeffs);

  DBENCH_STOP(*tmul);
}

/*************************************************
* Name:        poly_power2round
*
* Description: For all coefficients c of the input polynomial,
*              compute c0, c1 such that c mod Q = c1*2^D + c0
*              with -2^{D-1} < c0 <= 2^{D-1}. Assumes coefficients to be
*              standard representatives.
*
* Arguments:   - poly *a1: pointer to output polynomial with coefficients c1
*              - poly *a0: pointer to output polynomial with coefficients Q + c0
*              - const poly *v: pointer to input polynomial
**************************************************/
void poly_power2round(poly * restrict a1,
                      poly * restrict a0,
                      const poly * restrict a)
{
  unsigned int i;
  DBENCH_START();

  for(i = 0; i < N; ++i)
    a1->coeffs[i] = power2round(a->coeffs[i], &a0->coeffs[i]);

  DBENCH_STOP(*tround);
}

/*************************************************
* Name:        poly_chknorm
*
* Description: Check infinity norm of polynomial against given bound.
*              Assumes input coefficients to be standard representatives.
*
* Arguments:   - const poly *a: pointer to polynomial
*              - uint32_t B: norm bound
*
* Returns 0 if norm is strictly smaller than B and 1 otherwise.
**************************************************/
int poly_chknorm(const poly *a, uint32_t B) {
  unsigned int i;
  uint32_t t;
  DBENCH_START();

  /* It is ok to leak which coefficient violates the bound since
     the probability for each coefficient is independent of secret
     data but we must not leak the sign of the centralized representative. */
  for(i = 0; i < N; ++i) {
    /* Absolute value of centralized representative */
    t = (Q-1)/2 - a->coeffs[i];
    t ^= (int32_t)t >> 31;
    t = (Q-1)/2 - t;

    if(t >= B) {
      DBENCH_STOP(*tsample);
      return 1;
    }
  }

  DBENCH_STOP(*tsample);
  return 0;
}

/*************************************************
* Name:        rej_uniform
*
* Description: Sample uniformly random coefficients in [0, Q-1] by
*              performing rejection sampling using array of random bytes.
*
* Arguments:   - uint32_t *a: pointer to output array (allocated)
*              - unsigned int len: number of coefficients to be sampled
*              - const uint8_t *buf: array of random bytes
*              - unsigned int buflen: length of array of random bytes
*
* Returns number of sampled coefficients. Can be smaller than len if not enough
* random bytes were given.
**************************************************/
static unsigned int rej_uniform_ref(uint32_t *a,
                                    unsigned int len,
                                    const uint8_t *buf,
                                    unsigned int buflen)
{
  unsigned int ctr, pos;
  uint32_t t;
  DBENCH_START();

  ctr = pos = 0;
  while(ctr < len && pos + 3 <= buflen) {
    t  = buf[pos++];
    t |= (uint32_t)buf[pos++] << 8;
    t |= (uint32_t)buf[pos++] << 16;
    t &= 0x7FFFFF;

    if(t < Q)
      a[ctr++] = t;
  }

  DBENCH_STOP(*tsample);
  return ctr;
}

/*************************************************
* Name:        poly_uniform
*
* Description: Sample polynomial with uniformly random coefficients
*              in [0,Q-1] by performing rejection sampling using the
*              output stream of SHAKE256(seed|nonce) or AES256CTR(seed,nonce).
*
* Arguments:   - poly *a: pointer to output polynomial
*              - const uint8_t seed[]: byte array with seed of length SEEDBYTES
*              - uint16_t nonce: 2-byte nonce
**************************************************/
#ifdef USE_AES
void poly_uniform_aes(poly *a,
                      aes256ctr_ctx *state,
                      uint16_t nonce)
{
  unsigned int ctr;
  uint8_t buf[768];

  aes256ctr_select(state, nonce);
  aes256ctr_squeezeblocks(buf, 768/AES256CTR_BLOCKBYTES, state);

  ctr = rej_uniform(a->coeffs, N, buf, 768);

  while(ctr < N) {
    stream128_squeezeblocks(buf, 1, state);
    ctr += rej_uniform_ref(a->coeffs + ctr, N - ctr, buf, AES256CTR_BLOCKBYTES);
  }
}
#else
void poly_uniform_4x(poly *a0,
                     poly *a1,
                     poly *a2,
                     poly *a3,
                     const uint8_t seed[SEEDBYTES],
                     uint16_t nonce0,
                     uint16_t nonce1,
                     uint16_t nonce2,
                     uint16_t nonce3)
{
  unsigned int i, ctr0, ctr1, ctr2, ctr3;
  uint8_t inbuf[4][SEEDBYTES + 2];
  uint8_t outbuf[4][5*SHAKE128_RATE];
  __m256i state[25];

  for(i= 0; i < SEEDBYTES; ++i) {
    inbuf[0][i] = seed[i];
    inbuf[1][i] = seed[i];
    inbuf[2][i] = seed[i];
    inbuf[3][i] = seed[i];
  }
  inbuf[0][SEEDBYTES+0] = nonce0;
  inbuf[0][SEEDBYTES+1] = nonce0 >> 8;
  inbuf[1][SEEDBYTES+0] = nonce1;
  inbuf[1][SEEDBYTES+1] = nonce1 >> 8;
  inbuf[2][SEEDBYTES+0] = nonce2;
  inbuf[2][SEEDBYTES+1] = nonce2 >> 8;
  inbuf[3][SEEDBYTES+0] = nonce3;
  inbuf[3][SEEDBYTES+1] = nonce3 >> 8;

  shake128_absorb4x(state, inbuf[0], inbuf[1], inbuf[2], inbuf[3],
                    SEEDBYTES + 2);
  shake128_squeezeblocks4x(outbuf[0], outbuf[1], outbuf[2], outbuf[3], 5,
                           state);

  ctr0 = rej_uniform(a0->coeffs, N, outbuf[0], 5*SHAKE128_RATE);
  ctr1 = rej_uniform(a1->coeffs, N, outbuf[1], 5*SHAKE128_RATE);
  ctr2 = rej_uniform(a2->coeffs, N, outbuf[2], 5*SHAKE128_RATE);
  ctr3 = rej_uniform(a3->coeffs, N, outbuf[3], 5*SHAKE128_RATE);

  while(ctr0 < N || ctr1 < N || ctr2 < N || ctr3 < N) {
    shake128_squeezeblocks4x(outbuf[0], outbuf[1], outbuf[2], outbuf[3], 1,
                             state);

    ctr0 += rej_uniform_ref(a0->coeffs + ctr0, N - ctr0, outbuf[0],
                            SHAKE128_RATE);
    ctr1 += rej_uniform_ref(a1->coeffs + ctr1, N - ctr1, outbuf[1],
                            SHAKE128_RATE);
    ctr2 += rej_uniform_ref(a2->coeffs + ctr2, N - ctr2, outbuf[2],
                            SHAKE128_RATE);
    ctr3 += rej_uniform_ref(a3->coeffs + ctr3, N - ctr3, outbuf[3],
                            SHAKE128_RATE);
  }
}
#endif

/*************************************************
* Name:        rej_eta
*
* Description: Sample uniformly random coefficients in [-ETA, ETA] by
*              performing rejection sampling using array of random bytes.
*
* Arguments:   - uint32_t *a: pointer to output array (allocated)
*              - unsigned int len: number of coefficients to be sampled
*              - const uint8_t *buf: array of random bytes
*              - unsigned int buflen: length of array of random bytes
*
* Returns number of sampled coefficients. Can be smaller than len if not enough
* random bytes were given.
**************************************************/
static unsigned int rej_eta_ref(uint32_t *a,
                                unsigned int len,
                                const uint8_t *buf,
                                unsigned int buflen)
{
#if ETA > 7
#error "rej_eta() assumes ETA <= 7"
#endif
  unsigned int ctr, pos;
  uint32_t t0, t1;
  DBENCH_START();

  ctr = pos = 0;
  while(ctr < len && pos < buflen) {
#if ETA <= 3
    t0 = buf[pos] & 0x07;
    t1 = buf[pos++] >> 5;
#else
    t0 = buf[pos] & 0x0F;
    t1 = buf[pos++] >> 4;
#endif

    if(t0 <= 2*ETA)
      a[ctr++] = Q + ETA - t0;
    if(t1 <= 2*ETA && ctr < len)
      a[ctr++] = Q + ETA - t1;
  }

  DBENCH_STOP(*tsample);
  return ctr;
}

/*************************************************
* Name:        poly_uniform_eta
*
* Description: Sample polynomial with uniformly random coefficients
*              in [-ETA,ETA] by performing rejection sampling using the
*              output stream of SHAKE256(seed|nonce)
*              or AES256CTR(seed,nonce).
*
* Arguments:   - poly *a: pointer to output polynomial
*              - const uint8_t seed[]: byte array with seed of length
*                                      SEEDBYTES
*              - uint16_t nonce: 2-byte nonce
**************************************************/
#ifdef USE_AES
void poly_uniform_eta_aes(poly *a,
                          aes256ctr_ctx *state,
                          uint16_t nonce)
{
  unsigned int ctr;
  uint8_t buf[192];

  aes256ctr_select(state, nonce);
  aes256ctr_squeezeblocks(buf, 192/AES256CTR_BLOCKBYTES, state);

  ctr = rej_eta(a->coeffs, N, buf, 192);

  while(ctr < N) {
    stream128_squeezeblocks(buf, 1, state);
    ctr += rej_eta_ref(a->coeffs + ctr, N - ctr, buf, STREAM128_BLOCKBYTES);
  }
}
#else
#define POLY_UNIFORM_ETA_NBLOCKS (((N/2*(1U << SETABITS) + ETA)/(2*ETA + 1) \
                                   + STREAM128_BLOCKBYTES) \
                                  /STREAM128_BLOCKBYTES)
void poly_uniform_eta(poly *a,
                      const uint8_t seed[SEEDBYTES],
                      uint16_t nonce)
{
  unsigned int ctr;
  unsigned int buflen = POLY_UNIFORM_ETA_NBLOCKS*STREAM128_BLOCKBYTES;
  uint8_t buf[buflen];
  stream128_state state;

  stream128_init(&state, seed, nonce);
  stream128_squeezeblocks(buf, POLY_UNIFORM_ETA_NBLOCKS, &state);

  ctr = rej_eta(a->coeffs, N, buf, buflen);

  while(ctr < N) {
    stream128_squeezeblocks(buf, 1, &state);
    ctr += rej_eta_ref(a->coeffs + ctr, N - ctr, buf, STREAM128_BLOCKBYTES);
  }
}
void poly_uniform_eta_4x(poly *a0,
                         poly *a1,
                         poly *a2,
                         poly *a3,
                         const uint8_t seed[SEEDBYTES],
                         uint16_t nonce0,
                         uint16_t nonce1,
                         uint16_t nonce2,
                         uint16_t nonce3)
{
  unsigned int i, ctr0, ctr1, ctr2, ctr3;
  uint8_t inbuf[4][SEEDBYTES + 2];
  uint8_t outbuf[4][2*SHAKE128_RATE];
  __m256i state[25];

  for(i= 0; i < SEEDBYTES; ++i) {
    inbuf[0][i] = seed[i];
    inbuf[1][i] = seed[i];
    inbuf[2][i] = seed[i];
    inbuf[3][i] = seed[i];
  }
  inbuf[0][SEEDBYTES+0] = nonce0;
  inbuf[0][SEEDBYTES+1] = nonce0 >> 8;
  inbuf[1][SEEDBYTES+0] = nonce1;
  inbuf[1][SEEDBYTES+1] = nonce1 >> 8;
  inbuf[2][SEEDBYTES+0] = nonce2;
  inbuf[2][SEEDBYTES+1] = nonce2 >> 8;
  inbuf[3][SEEDBYTES+0] = nonce3;
  inbuf[3][SEEDBYTES+1] = nonce3 >> 8;

  shake128_absorb4x(state, inbuf[0], inbuf[1], inbuf[2], inbuf[3],
                    SEEDBYTES + 2);
  shake128_squeezeblocks4x(outbuf[0], outbuf[1], outbuf[2], outbuf[3], 2,
                           state);

  ctr0 = rej_eta(a0->coeffs, N, outbuf[0], 2*SHAKE128_RATE);
  ctr1 = rej_eta(a1->coeffs, N, outbuf[1], 2*SHAKE128_RATE);
  ctr2 = rej_eta(a2->coeffs, N, outbuf[2], 2*SHAKE128_RATE);
  ctr3 = rej_eta(a3->coeffs, N, outbuf[3], 2*SHAKE128_RATE);

  while(ctr0 < N || ctr1 < N || ctr2 < N || ctr3 < N) {
    shake128_squeezeblocks4x(outbuf[0], outbuf[1], outbuf[2], outbuf[3], 1,
                             state);

    ctr0 += rej_eta_ref(a0->coeffs + ctr0, N - ctr0, outbuf[0], SHAKE128_RATE);
    ctr1 += rej_eta_ref(a1->coeffs + ctr1, N - ctr1, outbuf[1], SHAKE128_RATE);
    ctr2 += rej_eta_ref(a2->coeffs + ctr2, N - ctr2, outbuf[2], SHAKE128_RATE);
    ctr3 += rej_eta_ref(a3->coeffs + ctr3, N - ctr3, outbuf[3], SHAKE128_RATE);
  }
}
#endif

/*************************************************
* Name:        rej_gamma1m1
*
* Description: Sample uniformly random coefficients
*              in [-(GAMMA1 - 1), GAMMA1 - 1] by performing rejection sampling
*              using array of random bytes.
*
* Arguments:   - uint32_t *a: pointer to output array (allocated)
*              - unsigned int len: number of coefficients to be sampled
*              - const uint8_t *buf: array of random bytes
*              - unsigned int buflen: length of array of random bytes
*
* Returns number of sampled coefficients. Can be smaller than len if not enough
* random bytes were given.
**************************************************/
static unsigned int rej_gamma1m1_ref(uint32_t *a,
                                     unsigned int len,
                                     const uint8_t *buf,
                                     unsigned int buflen)
{
#if GAMMA1 > (1 << 19)
#error "rej_gamma1m1() assumes GAMMA1 - 1 fits in 19 bits"
#endif
  unsigned int ctr, pos;
  uint32_t t0, t1;
  DBENCH_START();

  ctr = pos = 0;
  while(ctr < len && pos + 5 <= buflen) {
    t0  = buf[pos];
    t0 |= (uint32_t)buf[pos + 1] << 8;
    t0 |= (uint32_t)buf[pos + 2] << 16;
    t0 &= GAMMA1MASK;

    t1  = buf[pos + 2] >> 4;
    t1 |= (uint32_t)buf[pos + 3] << 4;
    t1 |= (uint32_t)buf[pos + 4] << 12;
    t1 &= GAMMA1MASK;

    pos += 5;

    if(t0 <= 2*GAMMA1 - 2)
      a[ctr++] = Q + GAMMA1 - 1 - t0;
    if(t1 <= 2*GAMMA1 - 2 && ctr < len)
      a[ctr++] = Q + GAMMA1 - 1 - t1;
  }

  DBENCH_STOP(*tsample);
  return ctr;
}

/*************************************************
* Name:        poly_uniform_gamma1m1
*
* Description: Sample polynomial with uniformly random coefficients
*              in [-(GAMMA1 - 1), GAMMA1 - 1] by performing rejection
*              sampling on output stream of SHAKE256(seed|nonce)
*              or AES256CTR(seed,nonce).
*
* Arguments:   - poly *a: pointer to output polynomial
*              - const uint8_t seed[]: byte array with seed of length
*                                            CRHBYTES
*              - uint16_t nonce: 16-bit nonce
**************************************************/
#ifdef USE_AES
void poly_uniform_gamma1m1_aes(poly *a,
                               aes256ctr_ctx *state,
                               uint16_t nonce)
{
  unsigned int ctr;
  uint8_t buf[640];

  aes256ctr_select(state, nonce);
  aes256ctr_squeezeblocks(buf, 640/AES256CTR_BLOCKBYTES, state);

  ctr = rej_gamma1m1(a->coeffs, N, buf, 640);

  while(ctr < N) {
    stream256_squeezeblocks(buf, 1, state);
    ctr += rej_gamma1m1_ref(a->coeffs + ctr, N - ctr, buf, AES256CTR_BLOCKBYTES);
  }
}
#else
#define POLY_UNIFORM_GAMMA1M1_NBLOCKS (((N*5/2*(1U << 20)+GAMMA1)/(2*GAMMA1-1) \
                                        + STREAM256_BLOCKBYTES) \
                                       /STREAM256_BLOCKBYTES)
void poly_uniform_gamma1m1(poly *a,
                           const uint8_t seed[CRHBYTES],
                           uint16_t nonce)
{
  unsigned int i, ctr, off;
  unsigned int buflen = POLY_UNIFORM_GAMMA1M1_NBLOCKS*STREAM256_BLOCKBYTES;
  uint8_t buf[buflen + 4];
  stream256_state state;

  stream256_init(&state, seed, nonce);
  stream256_squeezeblocks(buf, POLY_UNIFORM_GAMMA1M1_NBLOCKS, &state);

  ctr = rej_gamma1m1(a->coeffs, N, buf, buflen);

  while(ctr < N) {
    off = buflen % 5;
    for(i = 0; i < off; ++i)
      buf[i] = buf[buflen - off + i];

    buflen = STREAM256_BLOCKBYTES + off;
    stream256_squeezeblocks(buf + off, 1, &state);
    ctr += rej_gamma1m1_ref(a->coeffs + ctr, N - ctr, buf, buflen);
  }
}
void poly_uniform_gamma1m1_4x(poly *a0,
                              poly *a1,
                              poly *a2,
                              poly *a3,
                              const uint8_t seed[CRHBYTES],
                              uint16_t nonce0,
                              uint16_t nonce1,
                              uint16_t nonce2,
                              uint16_t nonce3)
{
  unsigned int i, ctr0, ctr1, ctr2, ctr3;
  uint8_t inbuf[4][CRHBYTES + 2];
  uint8_t outbuf[4][5*SHAKE256_RATE];
  __m256i state[25];

  for(i = 0; i < CRHBYTES; ++i) {
    inbuf[0][i] = seed[i];
    inbuf[1][i] = seed[i];
    inbuf[2][i] = seed[i];
    inbuf[3][i] = seed[i];
  }
  inbuf[0][CRHBYTES + 0] = nonce0;
  inbuf[0][CRHBYTES + 1] = nonce0 >> 8;
  inbuf[1][CRHBYTES + 0] = nonce1;
  inbuf[1][CRHBYTES + 1] = nonce1 >> 8;
  inbuf[2][CRHBYTES + 0] = nonce2;
  inbuf[2][CRHBYTES + 1] = nonce2 >> 8;
  inbuf[3][CRHBYTES + 0] = nonce3;
  inbuf[3][CRHBYTES + 1] = nonce3 >> 8;

  shake256_absorb4x(state, inbuf[0], inbuf[1], inbuf[2], inbuf[3],
                    CRHBYTES + 2);
  shake256_squeezeblocks4x(outbuf[0], outbuf[1], outbuf[2], outbuf[3], 5,
                           state);

  ctr0 = rej_gamma1m1_ref(a0->coeffs, N, outbuf[0], 5*SHAKE256_RATE);
  ctr1 = rej_gamma1m1_ref(a1->coeffs, N, outbuf[1], 5*SHAKE256_RATE);
  ctr2 = rej_gamma1m1_ref(a2->coeffs, N, outbuf[2], 5*SHAKE256_RATE);
  ctr3 = rej_gamma1m1_ref(a3->coeffs, N, outbuf[3], 5*SHAKE256_RATE);

  while(ctr0 < N || ctr1 < N || ctr2 < N || ctr3 < N) {
    shake256_squeezeblocks4x(outbuf[0], outbuf[1], outbuf[2], outbuf[3], 1,
                             state);

    ctr0 += rej_gamma1m1_ref(a0->coeffs + ctr0, N - ctr0, outbuf[0],
                            SHAKE256_RATE);
    ctr1 += rej_gamma1m1_ref(a1->coeffs + ctr1, N - ctr1, outbuf[1],
                            SHAKE256_RATE);
    ctr2 += rej_gamma1m1_ref(a2->coeffs + ctr2, N - ctr2, outbuf[2],
                            SHAKE256_RATE);
    ctr3 += rej_gamma1m1_ref(a3->coeffs + ctr3, N - ctr3, outbuf[3],
                            SHAKE256_RATE);
  }
}
#endif

/*************************************************
* Name:        polyeta_pack
*
* Description: Bit-pack polynomial with coefficients in [-ETA,ETA].
*              Input coefficients are assumed to lie in [Q-ETA,Q+ETA].
*
* Arguments:   - uint8_t *r: pointer to output byte array with at least
*                            POLETA_SIZE_PACKED bytes
*              - const poly *a: pointer to input polynomial
**************************************************/
void polyeta_pack(uint8_t * restrict r, const poly * restrict a) {
#if 2*ETA >= 16
#error "polyeta_pack() assumes 2*ETA < 16"
#endif
  unsigned int i;
  uint8_t t[8];
  DBENCH_START();

#if 2*ETA <= 7
  for(i = 0; i < N/8; ++i) {
    t[0] = Q + ETA - a->coeffs[8*i+0];
    t[1] = Q + ETA - a->coeffs[8*i+1];
    t[2] = Q + ETA - a->coeffs[8*i+2];
    t[3] = Q + ETA - a->coeffs[8*i+3];
    t[4] = Q + ETA - a->coeffs[8*i+4];
    t[5] = Q + ETA - a->coeffs[8*i+5];
    t[6] = Q + ETA - a->coeffs[8*i+6];
    t[7] = Q + ETA - a->coeffs[8*i+7];

    r[3*i+0]  = (t[0] >> 0) | (t[1] << 3) | (t[2] << 6);
    r[3*i+1]  = (t[2] >> 2) | (t[3] << 1) | (t[4] << 4) | (t[5] << 7);
    r[3*i+2]  = (t[5] >> 1) | (t[6] << 2) | (t[7] << 5);
  }
#else
  for(i = 0; i < N/2; ++i) {
    t[0] = Q + ETA - a->coeffs[2*i+0];
    t[1] = Q + ETA - a->coeffs[2*i+1];
    r[i] = t[0] | (t[1] << 4);
  }
#endif

  DBENCH_STOP(*tpack);
}

/*************************************************
* Name:        polyeta_unpack
*
* Description: Unpack polynomial with coefficients in [-ETA,ETA].
*              Output coefficients lie in [Q-ETA,Q+ETA].
*
* Arguments:   - poly *r: pointer to output polynomial
*              - const uint8_t *a: byte array with bit-packed polynomial
**************************************************/
void polyeta_unpack(poly * restrict r, const uint8_t * restrict a) {
  unsigned int i;
  DBENCH_START();

#if 2*ETA <= 7
  for(i = 0; i < N/8; ++i) {
    r->coeffs[8*i+0] = a[3*i+0] & 0x07;
    r->coeffs[8*i+1] = (a[3*i+0] >> 3) & 0x07;
    r->coeffs[8*i+2] = ((a[3*i+0] >> 6) | (a[3*i+1] << 2)) & 0x07;
    r->coeffs[8*i+3] = (a[3*i+1] >> 1) & 0x07;
    r->coeffs[8*i+4] = (a[3*i+1] >> 4) & 0x07;
    r->coeffs[8*i+5] = ((a[3*i+1] >> 7) | (a[3*i+2] << 1)) & 0x07;
    r->coeffs[8*i+6] = (a[3*i+2] >> 2) & 0x07;
    r->coeffs[8*i+7] = (a[3*i+2] >> 5) & 0x07;

    r->coeffs[8*i+0] = Q + ETA - r->coeffs[8*i+0];
    r->coeffs[8*i+1] = Q + ETA - r->coeffs[8*i+1];
    r->coeffs[8*i+2] = Q + ETA - r->coeffs[8*i+2];
    r->coeffs[8*i+3] = Q + ETA - r->coeffs[8*i+3];
    r->coeffs[8*i+4] = Q + ETA - r->coeffs[8*i+4];
    r->coeffs[8*i+5] = Q + ETA - r->coeffs[8*i+5];
    r->coeffs[8*i+6] = Q + ETA - r->coeffs[8*i+6];
    r->coeffs[8*i+7] = Q + ETA - r->coeffs[8*i+7];
  }
#else
  for(i = 0; i < N/2; ++i) {
    r->coeffs[2*i+0] = a[i] & 0x0F;
    r->coeffs[2*i+1] = a[i] >> 4;
    r->coeffs[2*i+0] = Q + ETA - r->coeffs[2*i+0];
    r->coeffs[2*i+1] = Q + ETA - r->coeffs[2*i+1];
  }
#endif

  DBENCH_STOP(*tpack);
}

/*************************************************
* Name:        polyt1_unpack
*
* Description: Unpack polynomial t1 with 9-bit coefficients.
*              Output coefficients are standard representatives.
*
* Arguments:   - poly *r: pointer to output polynomial
*              - const uint8_t *a: byte array with bit-packed polynomial
**************************************************/
void polyt1_unpack(poly * restrict r, const uint8_t * restrict a) {
  unsigned int i;
  DBENCH_START();

  for(i = 0; i < N/8; ++i) {
    r->coeffs[8*i+0] = ((a[9*i+0] >> 0) | ((uint32_t)a[9*i+1] << 8)) & 0x1FF;
    r->coeffs[8*i+1] = ((a[9*i+1] >> 1) | ((uint32_t)a[9*i+2] << 7)) & 0x1FF;
    r->coeffs[8*i+2] = ((a[9*i+2] >> 2) | ((uint32_t)a[9*i+3] << 6)) & 0x1FF;
    r->coeffs[8*i+3] = ((a[9*i+3] >> 3) | ((uint32_t)a[9*i+4] << 5)) & 0x1FF;
    r->coeffs[8*i+4] = ((a[9*i+4] >> 4) | ((uint32_t)a[9*i+5] << 4)) & 0x1FF;
    r->coeffs[8*i+5] = ((a[9*i+5] >> 5) | ((uint32_t)a[9*i+6] << 3)) & 0x1FF;
    r->coeffs[8*i+6] = ((a[9*i+6] >> 6) | ((uint32_t)a[9*i+7] << 2)) & 0x1FF;
    r->coeffs[8*i+7] = ((a[9*i+7] >> 7) | ((uint32_t)a[9*i+8] << 1)) & 0x1FF;
  }

  DBENCH_STOP(*tpack);
}

/*************************************************
* Name:        polyt0_pack
*
* Description: Bit-pack polynomial t0 with coefficients in ]-2^{D-1}, 2^{D-1}].
*              Input coefficients are assumed to lie in ]Q-2^{D-1}, Q+2^{D-1}].
*
* Arguments:   - uint8_t *r: pointer to output byte array with at least
*                            POLT0_SIZE_PACKED bytes
*              - const poly *a: pointer to input polynomial
**************************************************/
void polyt0_pack(uint8_t * restrict r, const poly * restrict a) {
  unsigned int i;
  uint32_t t[4];
  DBENCH_START();

  for(i = 0; i < N/4; ++i) {
    t[0] = Q + (1U << (D-1)) - a->coeffs[4*i+0];
    t[1] = Q + (1U << (D-1)) - a->coeffs[4*i+1];
    t[2] = Q + (1U << (D-1)) - a->coeffs[4*i+2];
    t[3] = Q + (1U << (D-1)) - a->coeffs[4*i+3];

    r[7*i+0]  =  t[0];
    r[7*i+1]  =  t[0] >> 8;
    r[7*i+1] |=  t[1] << 6;
    r[7*i+2]  =  t[1] >> 2;
    r[7*i+3]  =  t[1] >> 10;
    r[7*i+3] |=  t[2] << 4;
    r[7*i+4]  =  t[2] >> 4;
    r[7*i+5]  =  t[2] >> 12;
    r[7*i+5] |=  t[3] << 2;
    r[7*i+6]  =  t[3] >> 6;
  }

  DBENCH_STOP(*tpack);
}

/*************************************************
* Name:        polyt0_unpack
*
* Description: Unpack polynomial t0 with coefficients in ]-2^{D-1}, 2^{D-1}].
*              Output coefficients lie in ]Q-2^{D-1},Q+2^{D-1}].
*
* Arguments:   - poly *r: pointer to output polynomial
*              - const uint8_t *a: byte array with bit-packed polynomial
**************************************************/
void polyt0_unpack(poly * restrict r, const uint8_t * restrict a) {
  unsigned int i;
  DBENCH_START();

  for(i = 0; i < N/4; ++i) {
    r->coeffs[4*i+0]  = a[7*i+0];
    r->coeffs[4*i+0] |= (uint32_t)a[7*i+1] << 8;
    r->coeffs[4*i+0] &= 0x3FFF;

    r->coeffs[4*i+1]  = a[7*i+1] >> 6;
    r->coeffs[4*i+1] |= (uint32_t)a[7*i+2] << 2;
    r->coeffs[4*i+1] |= (uint32_t)a[7*i+3] << 10;
    r->coeffs[4*i+1] &= 0x3FFF;

    r->coeffs[4*i+2]  = a[7*i+3] >> 4;
    r->coeffs[4*i+2] |= (uint32_t)a[7*i+4] << 4;
    r->coeffs[4*i+2] |= (uint32_t)a[7*i+5] << 12;
    r->coeffs[4*i+2] &= 0x3FFF;

    r->coeffs[4*i+3]  = a[7*i+5] >> 2;
    r->coeffs[4*i+3] |= (uint32_t)a[7*i+6] << 6;

    r->coeffs[4*i+0] = Q + (1U << (D-1)) - r->coeffs[4*i+0];
    r->coeffs[4*i+1] = Q + (1U << (D-1)) - r->coeffs[4*i+1];
    r->coeffs[4*i+2] = Q + (1U << (D-1)) - r->coeffs[4*i+2];
    r->coeffs[4*i+3] = Q + (1U << (D-1)) - r->coeffs[4*i+3];
  }

  DBENCH_STOP(*tpack);
}

/*************************************************
* Name:        polyz_pack
*
* Description: Bit-pack polynomial z with coefficients
*              in [-(GAMMA1 - 1), GAMMA1 - 1].
*              Input coefficients are assumed to be standard representatives.
*
* Arguments:   - uint8_t *r: pointer to output byte array with at least
*                            POLZ_SIZE_PACKED bytes
*              - const poly *a: pointer to input polynomial
**************************************************/
void polyz_pack_old(uint8_t * restrict r, const poly * restrict a) {
#if GAMMA1 > (1 << 19)
#error "polyz_pack() assumes GAMMA1 <= 2^{19}"
#endif
  unsigned int i;
  uint32_t t[2];
  DBENCH_START();

  for(i = 0; i < N/2; ++i) {
    /* Map to {0,...,2*GAMMA1 - 2} */
    t[0] = GAMMA1 - 1 - a->coeffs[2*i+0];
    t[0] += ((int32_t)t[0] >> 31) & Q;
    t[1] = GAMMA1 - 1 - a->coeffs[2*i+1];
    t[1] += ((int32_t)t[1] >> 31) & Q;

    r[5*i+0]  = t[0];
    r[5*i+1]  = t[0] >> 8;
    r[5*i+2]  = t[0] >> 16;
    r[5*i+2] |= t[1] << 4;
    r[5*i+3]  = t[1] >> 4;
    r[5*i+4]  = t[1] >> 12;
  }

  DBENCH_STOP(*tpack);
}

void polyz_pack(uint8_t * restrict r, const poly * restrict a) {
#if GAMMA1 > (1 << 19)
#error "polyz_pack() assumes GAMMA1 <= 2^{19}"
#endif
  unsigned int i;
  uint32_t t[4];
  DBENCH_START();

  for(i = 0; i < N/4; ++i) {
    /* Map to {0,...,2*GAMMA1 - 2} */
    t[0] = GAMMA1 - 1 - a->coeffs[4*i+0];
    t[0] += ((int32_t)t[0] >> 31) & Q;
    t[1] = GAMMA1 - 1 - a->coeffs[4*i+1];
    t[1] += ((int32_t)t[1] >> 31) & Q;
    t[2] = GAMMA1 - 1 - a->coeffs[4*i+2];
    t[2] += ((int32_t)t[2] >> 31) & Q;
    t[3] = GAMMA1 - 1 - a->coeffs[4*i+3];
    t[3] += ((int32_t)t[3] >> 31) & Q;

    r[9*i+0]  = t[0];
    r[9*i+1]  = t[0] >> 8;
    r[9*i+2]  = t[0] >> 16;

    r[9*i+2] |= t[1] << 2;
    r[9*i+3]  = t[1] >> 6;
    r[9*i+4]  = t[1] >> 14;
    
    r[9*i+4] |= t[2] << 4;
    r[9*i+5]  = t[2] >> 4;
    r[9*i+6]  = t[2] >> 12;

    r[9*i+6] |= t[3] << 6;
    r[9*i+7]  = t[3] >> 2;
    r[9*i+8]  = t[3] >> 10;
  }

  DBENCH_STOP(*tpack);
}

/*************************************************
* Name:        polyz_unpack
*
* Description: Unpack polynomial z with coefficients
*              in [-(GAMMA1 - 1), GAMMA1 - 1].
*              Output coefficients are standard representatives.
*
* Arguments:   - poly *r: pointer to output polynomial
*              - const uint8_t *a: byte array with bit-packed polynomial
**************************************************/
void polyz_unpack_old(poly * restrict r, const uint8_t * restrict a) {
  unsigned int i;
  DBENCH_START();

  for(i = 0; i < N/2; ++i) {
    r->coeffs[2*i+0]  = a[5*i+0];
    r->coeffs[2*i+0] |= (uint32_t)a[5*i+1] << 8;
    r->coeffs[2*i+0] |= (uint32_t)a[5*i+2] << 16;
    r->coeffs[2*i+0] &= GAMMA1MASK;

    r->coeffs[2*i+1]  = a[5*i+2] >> 4;
    r->coeffs[2*i+1] |= (uint32_t)a[5*i+3] << 4;
    r->coeffs[2*i+1] |= (uint32_t)a[5*i+4] << 12;

    r->coeffs[2*i+0] = GAMMA1 - 1 - r->coeffs[2*i+0];
    r->coeffs[2*i+0] += ((int32_t)r->coeffs[2*i+0] >> 31) & Q;
    r->coeffs[2*i+1] = GAMMA1 - 1 - r->coeffs[2*i+1];
    r->coeffs[2*i+1] += ((int32_t)r->coeffs[2*i+1] >> 31) & Q;
  }

  DBENCH_STOP(*tpack);
}

void polyz_unpack(poly * restrict r, const uint8_t * restrict a) {
  unsigned int i;
  DBENCH_START();

  for(i = 0; i < N/4; ++i) {
    r->coeffs[4*i+0]  = a[9*i+0];
    r->coeffs[4*i+0] |= (uint32_t)a[9*i+1] << 8;
    r->coeffs[4*i+0] |= (uint32_t)a[9*i+2] << 16;
    r->coeffs[4*i+0] &= GAMMA1MASK;

    r->coeffs[4*i+1]  = a[9*i+2] >> 2;
    r->coeffs[4*i+1] |= (uint32_t)a[9*i+3] << 6;
    r->coeffs[4*i+1] |= (uint32_t)a[9*i+4] << 14;
    r->coeffs[4*i+1] &= GAMMA1MASK;

    r->coeffs[4*i+2]  = a[9*i+4] >> 4;
    r->coeffs[4*i+2] |= (uint32_t)a[9*i+5] << 4;
    r->coeffs[4*i+2] |= (uint32_t)a[9*i+6] << 12;
    r->coeffs[4*i+2] &= GAMMA1MASK;

    r->coeffs[4*i+3]  = a[9*i+6] >> 6;
    r->coeffs[4*i+3] |= (uint32_t)a[9*i+7] << 2;
    r->coeffs[4*i+3] |= (uint32_t)a[9*i+8] << 10;

    r->coeffs[4*i+0] = GAMMA1 - 1 - r->coeffs[4*i+0];
    r->coeffs[4*i+0] += ((int32_t)r->coeffs[4*i+0] >> 31) & Q;
    r->coeffs[4*i+1] = GAMMA1 - 1 - r->coeffs[4*i+1];
    r->coeffs[4*i+1] += ((int32_t)r->coeffs[4*i+1] >> 31) & Q;
    r->coeffs[4*i+2] = GAMMA1 - 1 - r->coeffs[4*i+2];
    r->coeffs[4*i+2] += ((int32_t)r->coeffs[4*i+2] >> 31) & Q;
    r->coeffs[4*i+3] = GAMMA1 - 1 - r->coeffs[4*i+3];
    r->coeffs[4*i+3] += ((int32_t)r->coeffs[4*i+3] >> 31) & Q;
  }

  DBENCH_STOP(*tpack);
}

/*************************************************
* Name:        polyw1_pack
*
* Description: Bit-pack polynomial w1 with coefficients in [0, 15].
*              Input coefficients are assumed to be standard representatives.
*
* Arguments:   - uint8_t *r: pointer to output byte array with at least
*                            POLW1_SIZE_PACKED bytes
*              - const poly *a: pointer to input polynomial
**************************************************/
void polyw1_pack(uint8_t * restrict r, const poly * restrict a) {
  unsigned int i;
  DBENCH_START();

  for(i = 0; i < N/2; ++i)
    r[i] = a->coeffs[2*i+0] | (a->coeffs[2*i+1] << 4);

  DBENCH_STOP(*tpack);
}