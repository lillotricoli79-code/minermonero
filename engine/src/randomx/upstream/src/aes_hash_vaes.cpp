/*
Copyright (c) 2018-2019, tevador <tevador@gmail.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the copyright holder nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/* VAES implementation adapted from c0mm4nd/rxminer, commit 5fe52dde2650c623fd0472804152a94711889dce. */
/* RxMiner implementation portions are MIT-licensed; see ../LICENSES/RXMINER-MIT.txt. */
/*
 * VAES 256-bit implementations of the RandomX AES fill/hash helpers.
 *
 * This file is compiled with -mvaes -mavx2 on x86-64 and is only called after
 * a runtime CPU/OS VAES capability check. Each 256-bit VAES operation
 * acts on two independent 128-bit lanes, matching the scalar four-lane code.
 */

#if defined(__x86_64__) || defined(__i386__)

#include "aes_hash.hpp"
#include "intrin_portable.h"

#include <cassert>
#include <cstdint>
#include <immintrin.h>

namespace {

// Same constants as aes_hash.cpp (RandomX AesGenerator1R/4R).
#define AES_HASH_1R_STATE0 0xd7983aad, 0xcc82db47, 0x9fa856de, 0x92b52c0d
#define AES_HASH_1R_STATE1 0xace78057, 0xf59e125a, 0x15c7b798, 0x338d996e
#define AES_HASH_1R_STATE2 0xe8a07ce4, 0x5079506b, 0xae62c7d0, 0x6a770017
#define AES_HASH_1R_STATE3 0x7e994948, 0x79a10005, 0x07ad828d, 0x630a240c
#define AES_HASH_1R_XKEY0 0x06890201, 0x90dc56bf, 0x8b24949f, 0xf6fa8389
#define AES_HASH_1R_XKEY1 0xed18f99b, 0xee1043c6, 0x51f4e03c, 0x61b263d1

#define AES_GEN_1R_KEY0 0xb4f44917, 0xdbb5552b, 0x62716609, 0x6daca553
#define AES_GEN_1R_KEY1 0x0da1dc4e, 0x1725d378, 0x846a710d, 0x6d7caf07
#define AES_GEN_1R_KEY2 0x3e20e345, 0xf4c0794f, 0x9f947ec6, 0x3f1262f1
#define AES_GEN_1R_KEY3 0x49169154, 0x16314c88, 0xb1ba317c, 0x6aef8135

#define AES_GEN_4R_KEY0 0x99e5d23f, 0x2f546d2b, 0xd1833ddb, 0x6421aadd
#define AES_GEN_4R_KEY1 0xa5dfcde5, 0x06f79d53, 0xb6913f55, 0xb20e3450
#define AES_GEN_4R_KEY2 0x171c02bf, 0x0aa4679f, 0x515e7baf, 0x5c3ed904
#define AES_GEN_4R_KEY3 0xd8ded291, 0xcd673785, 0xe78f5d08, 0x85623763
#define AES_GEN_4R_KEY4 0x229effb4, 0x3d518b6d, 0xe3d6a7a6, 0xb5826f73
#define AES_GEN_4R_KEY5 0xb272b7d2, 0xe9024d4e, 0x9c10b3d9, 0xc7566bf3
#define AES_GEN_4R_KEY6 0xf63befa7, 0x2ba9660a, 0xf765a38b, 0xf273c9e7
#define AES_GEN_4R_KEY7 0xc0b0762d, 0x0c06d1fd, 0x915839de, 0x7a7cd609

inline __m128i v128(int i3, int i2, int i1, int i0) {
    return _mm_set_epi32(i3, i2, i1, i0);
}

inline __m256i pair(__m128i lo, __m128i hi) {
    return _mm256_set_m128i(hi, lo);
}

inline __m128i lo128(__m256i v) { return _mm256_castsi256_si128(v); }
inline __m128i hi128(__m256i v) { return _mm256_extracti128_si256(v, 1); }

} // namespace

void rxm_fillAes1Rx4_vaes(void *state, size_t outputSize, void *buffer) {
    assert(outputSize % 64 == 0);
    uint8_t *out = (uint8_t *) buffer;
    uint8_t *end = out + outputSize;

    const __m256i k02 = pair(v128(AES_GEN_1R_KEY0), v128(AES_GEN_1R_KEY2));
    const __m256i k13 = pair(v128(AES_GEN_1R_KEY1), v128(AES_GEN_1R_KEY3));

    __m256i s02 = pair(_mm_load_si128((const __m128i *) state + 0),
                       _mm_load_si128((const __m128i *) state + 2));
    __m256i s13 = pair(_mm_load_si128((const __m128i *) state + 1),
                       _mm_load_si128((const __m128i *) state + 3));

    while (out < end) {
        s02 = _mm256_aesdec_epi128(s02, k02);
        s13 = _mm256_aesenc_epi128(s13, k13);
        _mm_store_si128((__m128i *) out + 0, lo128(s02));
        _mm_store_si128((__m128i *) out + 1, lo128(s13));
        _mm_store_si128((__m128i *) out + 2, hi128(s02));
        _mm_store_si128((__m128i *) out + 3, hi128(s13));
        out += 64;
    }

    _mm_store_si128((__m128i *) state + 0, lo128(s02));
    _mm_store_si128((__m128i *) state + 1, lo128(s13));
    _mm_store_si128((__m128i *) state + 2, hi128(s02));
    _mm_store_si128((__m128i *) state + 3, hi128(s13));
}

template<bool normalized>
static void fillAes4Rx4Impl(void *state, size_t outputSize, void *buffer) {
    assert(outputSize % 64 == 0);
    assert(!normalized || outputSize >= 128);
    uint8_t *out = (uint8_t *) buffer;
    uint8_t *end = out + outputSize;

    const __m256i k04 = pair(v128(AES_GEN_4R_KEY0), v128(AES_GEN_4R_KEY4));
    const __m256i k15 = pair(v128(AES_GEN_4R_KEY1), v128(AES_GEN_4R_KEY5));
    const __m256i k26 = pair(v128(AES_GEN_4R_KEY2), v128(AES_GEN_4R_KEY6));
    const __m256i k37 = pair(v128(AES_GEN_4R_KEY3), v128(AES_GEN_4R_KEY7));

    __m256i s02 = pair(_mm_load_si128((const __m128i *) state + 0),
                       _mm_load_si128((const __m128i *) state + 2));
    __m256i s13 = pair(_mm_load_si128((const __m128i *) state + 1),
                       _mm_load_si128((const __m128i *) state + 3));

    for (unsigned phase = 0; phase < (normalized ? 2u : 1u); ++phase) {
        uint8_t* phaseEnd = normalized && phase == 0 ? (uint8_t*)buffer + 128 : end;
        const __m128i mask = phase == 0
            ? v128(-1, -1, -1, -1)
            : v128(-1, 0xff0707ff, -1, 0xff0707ff);
        const __m256i outputMask = pair(mask, mask);
        while (out < phaseEnd) {
            s02 = _mm256_aesdec_epi128(s02, k04);
            s13 = _mm256_aesenc_epi128(s13, k04);
            s02 = _mm256_aesdec_epi128(s02, k15);
            s13 = _mm256_aesenc_epi128(s13, k15);
            s02 = _mm256_aesdec_epi128(s02, k26);
            s13 = _mm256_aesenc_epi128(s13, k26);
            s02 = _mm256_aesdec_epi128(s02, k37);
            s13 = _mm256_aesenc_epi128(s13, k37);

            // Mask stores only: the unmodified AES states feed the next block.
            const __m256i out02 = normalized ? _mm256_and_si256(s02, outputMask) : s02;
            const __m256i out13 = normalized ? _mm256_and_si256(s13, outputMask) : s13;
            _mm_store_si128((__m128i *) out + 0, lo128(out02));
            _mm_store_si128((__m128i *) out + 1, lo128(out13));
            _mm_store_si128((__m128i *) out + 2, hi128(out02));
            _mm_store_si128((__m128i *) out + 3, hi128(out13));
            out += 64;
        }
    }
}

void rxm_fillAes4Rx4_vaes(void *state, size_t outputSize, void *buffer) {
    fillAes4Rx4Impl<false>(state, outputSize, buffer);
}

void rxm_fillAes4Rx4Program_vaes(void *state, size_t outputSize, void *buffer) {
    fillAes4Rx4Impl<true>(state, outputSize, buffer);
}

void rxm_hashAndFillAes1Rx4_vaes(void *scratchpad, size_t scratchpadSize,
                                 void *hash, void *fill_state) {
    uint8_t *p = (uint8_t *) scratchpad;
    const uint8_t *pEnd = p + scratchpadSize;

    __m256i h02 = pair(v128(AES_HASH_1R_STATE0), v128(AES_HASH_1R_STATE2));
    __m256i h13 = pair(v128(AES_HASH_1R_STATE1), v128(AES_HASH_1R_STATE3));

    const __m256i fk02 = pair(v128(AES_GEN_1R_KEY0), v128(AES_GEN_1R_KEY2));
    const __m256i fk13 = pair(v128(AES_GEN_1R_KEY1), v128(AES_GEN_1R_KEY3));

    __m256i f02 = pair(_mm_load_si128((const __m128i *) fill_state + 0),
                       _mm_load_si128((const __m128i *) fill_state + 2));
    __m256i f13 = pair(_mm_load_si128((const __m128i *) fill_state + 1),
                       _mm_load_si128((const __m128i *) fill_state + 3));

    constexpr int PREFETCH_DISTANCE = 4096;
    const char *prefetchPtr = (const char *) scratchpad + PREFETCH_DISTANCE;
    pEnd -= PREFETCH_DISTANCE;

    for (int i = 0; i < 2; ++i) {
        while (p < pEnd) {
            __m256i in02 = pair(_mm_load_si128((const __m128i *) p + 0),
                                _mm_load_si128((const __m128i *) p + 2));
            __m256i in13 = pair(_mm_load_si128((const __m128i *) p + 1),
                                _mm_load_si128((const __m128i *) p + 3));
            h02 = _mm256_aesenc_epi128(h02, in02);
            h13 = _mm256_aesdec_epi128(h13, in13);

            f02 = _mm256_aesdec_epi128(f02, fk02);
            f13 = _mm256_aesenc_epi128(f13, fk13);
            _mm_store_si128((__m128i *) p + 0, lo128(f02));
            _mm_store_si128((__m128i *) p + 1, lo128(f13));
            _mm_store_si128((__m128i *) p + 2, hi128(f02));
            _mm_store_si128((__m128i *) p + 3, hi128(f13));

            _mm_prefetch(prefetchPtr, _MM_HINT_T0);
            p += 64;
            prefetchPtr += 64;
        }
        prefetchPtr = (const char *) scratchpad;
        pEnd += PREFETCH_DISTANCE;
    }

    _mm_store_si128((__m128i *) fill_state + 0, lo128(f02));
    _mm_store_si128((__m128i *) fill_state + 1, lo128(f13));
    _mm_store_si128((__m128i *) fill_state + 2, hi128(f02));
    _mm_store_si128((__m128i *) fill_state + 3, hi128(f13));

    const __m256i xkey0 = pair(v128(AES_HASH_1R_XKEY0), v128(AES_HASH_1R_XKEY0));
    const __m256i xkey1 = pair(v128(AES_HASH_1R_XKEY1), v128(AES_HASH_1R_XKEY1));

    h02 = _mm256_aesenc_epi128(h02, xkey0);
    h13 = _mm256_aesdec_epi128(h13, xkey0);
    h02 = _mm256_aesenc_epi128(h02, xkey1);
    h13 = _mm256_aesdec_epi128(h13, xkey1);

    _mm_store_si128((__m128i *) hash + 0, lo128(h02));
    _mm_store_si128((__m128i *) hash + 1, lo128(h13));
    _mm_store_si128((__m128i *) hash + 2, hi128(h02));
    _mm_store_si128((__m128i *) hash + 3, hi128(h13));
}

#else
#error "aes_hash_vaes.cpp is x86-only"
#endif
