/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <fenv.h>
#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hdrgrading/foundation/fpenv.h"
#include "hdrgrading/foundation/math.h"

/* テスト側でホストの FTZ/DAZ を直接立てる (被試験コードとは別に書く) */
#if defined(__x86_64__) || defined(_M_X64) || (defined(__i386__) && defined(__SSE__)) ||           \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define TEST_HAS_FLUSH 1
#define TEST_FLUSH_X86 1
#define FLUSH_BITS     UINT64_C(0x8040) /* MXCSR FTZ (15) | DAZ (6) */
#define CONTROL_MASK   UINT64_C(0xffc0) /* bits 0-5 are status flags */
#define PINNED         UINT64_C(0x1f80) /* RC = nearest, FTZ = DAZ = 0, all masked */
#elif defined(__aarch64__)
#define TEST_HAS_FLUSH 1
#define FLUSH_BITS     (UINT64_C(1) << 24) /* FPCR FZ (inputs and results) */
#define CONTROL_MASK   UINT64_MAX
#define PINNED         UINT64_C(0) /* FPCR: every control cleared */
#endif

/* FLT_MIN * 0.5 = 2^-127 (サブノーマル)、1 + 0.75 ulp (0 方向なら 1、最近接なら 1 + 2^-23) */
static volatile float v_min = FLT_MIN, v_half = 0.5f, v_one = 1.0f, v_tiny = 0x1.8p-24f;

#if defined(TEST_HAS_FLUSH)
/* サブノーマルの入力 2^-130 */
static volatile float v_sub = 0x1p-130f;
static uint64_t saved_state;
#endif

#if defined(TEST_FLUSH_X86)
static inline uint64_t raw_read(void)
{
    return (uint64_t)_mm_getcsr();
}

static inline void raw_write(uint64_t value)
{
    _mm_setcsr((unsigned int)value);
}
#elif defined(TEST_HAS_FLUSH)
static inline uint64_t raw_read(void)
{
    uint64_t value;

    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));

    return value;
}

static inline void raw_write(uint64_t value)
{
    __asm__ __volatile__("msr fpcr, %0" : : "r"(value) : "memory");
}
#endif

/* 呼び出しのスレッドが固定の状態か (被試験コードとは別に読む。ほかでは丸めの向きだけ) */
static inline bool is_pinned(void)
{
#if defined(TEST_HAS_FLUSH)
    return (raw_read() & CONTROL_MASK) == PINNED;
#elif defined(FE_TONEAREST)
    return fegetround() == FE_TONEAREST;
#else
    return true;
#endif
}

static inline uint32_t bits_of(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));

    return bits;
}

/* ホストの既定状態では enter / leave は何も書き換えない。入れ子も no-op */
static void test_default_state_is_noop(void)
{
    hdrg_fpenv_t outer, inner;

    hdrg_fpenv_enter(&outer);
    TEST_ASSERT_FALSE(outer.changed);
    TEST_ASSERT_TRUE(is_pinned());

    hdrg_fpenv_enter(&inner);
    TEST_ASSERT_FALSE(inner.changed);
    hdrg_fpenv_leave(&inner);
    TEST_ASSERT_TRUE(is_pinned());

    hdrg_fpenv_leave(&outer);
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x00400000), bits_of(v_min * v_half));
    TEST_ASSERT_EQUAL_HEX32(bits_of(1.0f + 0x1p-23f), bits_of(v_one + v_tiny));
}

/* ホストが FTZ/DAZ を立てていても enter の中では IEEE どおりサブノーマルを扱い、leave で戻る */
static void test_flush_pinned_and_restored(void)
{
#if defined(TEST_HAS_FLUSH)
    hdrg_fpenv_t outer, inner;
    uint64_t host;

    raw_write(saved_state | FLUSH_BITS);
    host = raw_read();

    /* 前提: ホスト側の設定で実際にフラッシュされる。エミュレータ (Valgrind の VEX など) は
     * FZ / FTZ を再現しないので、その場合は検証できない */
    if (bits_of(v_min * v_half) != 0) {
        raw_write(saved_state);
        TEST_IGNORE_MESSAGE("flush-to-zero is not honored on this CPU / emulator");
    }

    TEST_ASSERT_FALSE(is_pinned());
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(v_min * v_half));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(v_sub * v_one));
#if !(defined(_MSC_VER) && !defined(__clang__) && defined(_M_IX86))
    /* 32-bit cl may narrow the double to float on the x87, which MXCSR's FTZ does not flush */
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(hdrg_math_exp2f(-140.0f)));
#endif

    hdrg_fpenv_enter(&outer);
    TEST_ASSERT_TRUE(outer.changed);
    TEST_ASSERT_TRUE(is_pinned());
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x00400000), bits_of(v_min * v_half));
    TEST_ASSERT_EQUAL_HEX32(bits_of(0x1p-130f), bits_of(v_sub * v_one));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x00000200), bits_of(hdrg_math_exp2f(-140.0f)));

    hdrg_fpenv_enter(&inner);
    TEST_ASSERT_FALSE(inner.changed);
    hdrg_fpenv_leave(&inner);
    TEST_ASSERT_TRUE(is_pinned());

    hdrg_fpenv_leave(&outer);
    TEST_ASSERT_FALSE(is_pinned());
    TEST_ASSERT_EQUAL_HEX64(host & CONTROL_MASK, raw_read() & CONTROL_MASK);
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(v_min * v_half));
    TEST_ASSERT_EQUAL_HEX32(0, bits_of(v_sub * v_one));
#else
    TEST_IGNORE_MESSAGE("no FTZ/DAZ control on this target");
#endif
}

/*
 * 丸めモードはすべての経路 (fenv.h のフォールバックを含む) で最近接偶数に固定される。
 * WASM は丸めモードを持たない (fesetround は状態だけを変え、演算は常に最近接) ので、そこでは
 * fenv.h の状態の保存・固定・復元だけを確かめる。x86 の fegetround は x87 の制御語を読み、
 * MXCSR だけを扱う本体の経路とは無関係なので、状態の照合は x86 以外で行う
 */
static void test_rounding_pinned_and_restored(void)
{
#if defined(FE_TOWARDZERO) && defined(FE_TONEAREST)
    hdrg_fpenv_t env;
    bool effective;

    if (fesetround(FE_TOWARDZERO) != 0 || fegetround() != FE_TOWARDZERO) {
        TEST_IGNORE_MESSAGE("FE_TOWARDZERO is not supported");
    }

    effective = bits_of(v_one + v_tiny) == bits_of(1.0f);
    TEST_ASSERT_FALSE(is_pinned());

    hdrg_fpenv_enter(&env);
    TEST_ASSERT_TRUE(env.changed);
    TEST_ASSERT_TRUE(is_pinned());
    TEST_ASSERT_EQUAL_HEX32(bits_of(1.0f + 0x1p-23f), bits_of(v_one + v_tiny));
    TEST_ASSERT_EQUAL_HEX32(bits_of(2.0f), bits_of(hdrg_math_exp2f(1.0f)));
#if !defined(TEST_FLUSH_X86)
    TEST_ASSERT_EQUAL_INT(FE_TONEAREST, fegetround());
#endif

    hdrg_fpenv_leave(&env);
    TEST_ASSERT_FALSE(is_pinned());
#if !defined(TEST_FLUSH_X86)
    TEST_ASSERT_EQUAL_INT(FE_TOWARDZERO, fegetround());
#endif

    if (effective) {
        TEST_ASSERT_EQUAL_HEX32(bits_of(1.0f), bits_of(v_one + v_tiny));
    } else {
        TEST_MESSAGE("the rounding mode does not affect arithmetic here; checked the fenv state");
    }
#else
    TEST_IGNORE_MESSAGE("no FE_TOWARDZERO on this target");
#endif
}

void setUp(void)
{
#if defined(TEST_HAS_FLUSH)
    saved_state = raw_read();
#endif
}

/* 失敗で途中から抜けても次のテストに状態を持ち越さない */
void tearDown(void)
{
#if defined(FE_TONEAREST)
    (void)fesetround(FE_TONEAREST);
#endif

#if defined(TEST_HAS_FLUSH)
    raw_write(saved_state);
#endif
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_state_is_noop);
    RUN_TEST(test_flush_pinned_and_restored);
    RUN_TEST(test_rounding_pinned_and_restored);

    return UNITY_END();
}
