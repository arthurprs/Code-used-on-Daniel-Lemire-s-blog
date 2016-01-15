// gcc -fno-inline -march=native -std=c99 -Wall -Wextra -O3 -g  popcnt.c -o popcnt

// NOTE: Works with gcc, but broken with ICC and Clang.  Neither accepts
// the inline assembly 'q' modifier: http://stackoverflow.com/questions/34459803
// Should be possible to work around this by passing extra variables, though.

#include <x86intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RDTSC_START(cycles)                                             \
    do {                                                                \
        uint32_t cyc_high, cyc_low;                                     \
        __asm volatile("cpuid\n"                                        \
                       "rdtsc\n"                                        \
                       "mov %%edx, %0\n"                                \
                       "mov %%eax, %1" :                                \
                       "=r" (cyc_high),                                 \
                       "=r"(cyc_low) :                                  \
                       : /* no read only */                             \
                       "%rax", "%rbx", "%rcx", "%rdx" /* clobbers */    \
                       );                                               \
        (cycles) = ((uint64_t)cyc_high << 32) | cyc_low;                \
    } while (0)

#define RDTSC_STOP(cycles)                                              \
    do {                                                                \
        uint32_t cyc_high, cyc_low;                                     \
        __asm volatile("rdtscp\n"                                       \
                       "mov %%edx, %0\n"                                \
                       "mov %%eax, %1\n"                                \
                       "cpuid" :                                        \
                       "=r"(cyc_high),                                  \
                       "=r"(cyc_low) :                                  \
                       /* no read only registers */ :                   \
                       "%rax", "%rbx", "%rcx", "%rdx" /* clobbers */    \
                       );                                               \
        (cycles) = ((uint64_t)cyc_high << 32) | cyc_low;                \
    } while (0)

static __attribute__ ((noinline))
uint64_t rdtsc_overhead_func(uint64_t dummy) {
    return dummy;
}

uint64_t global_rdtsc_overhead = (uint64_t) UINT64_MAX;

#define RDTSC_SET_OVERHEAD(test, repeat)			      \
  do {								      \
    uint64_t cycles_start, cycles_final, cycles_diff;		      \
    uint64_t min_diff = UINT64_MAX;				      \
    for (size_t i = 0; i < repeat; i++) {			      \
      __asm volatile("" ::: /* pretend to clobber */ "memory");	      \
      RDTSC_START(cycles_start);				      \
      test;							      \
      RDTSC_STOP(cycles_final);                                       \
      cycles_diff = (cycles_final - cycles_start);		      \
      if (cycles_diff < min_diff) min_diff = cycles_diff;	      \
    }								      \
    global_rdtsc_overhead = min_diff;				      \
    printf("rdtsc_overhead set to %ld\n", global_rdtsc_overhead);     \
  } while (0)							      \
 

/*
 * Prints the best number of operations per cycle where
 * test is the function call, answer is the expected answer generated by
 * test, repeat is the number of times we should repeat and num_ops is the
 * number of operations represented by test.
 */
#define RDTSC_BEST(test, answer, repeat, num_ops)			\
  do {									\
    if (global_rdtsc_overhead == UINT64_MAX) {				\
      RDTSC_SET_OVERHEAD(rdtsc_overhead_func(1), repeat);		\
    }									\
    printf("%s: ", #test);						\
    fflush(NULL);							\
    uint64_t cycles_start, cycles_final, cycles_diff;			\
    int wrong_answer = 0;						\
    uint64_t min_diff = UINT64_MAX;					\
    for (size_t i = 0; i < repeat; i++) {				\
      __asm volatile("" ::: /* pretend to clobber */ "memory");		\
      RDTSC_START(cycles_start);					\
      if (test != answer) wrong_answer = 1;				\
      RDTSC_STOP(cycles_final);                                         \
      cycles_diff = (cycles_final - cycles_start);			\
      if (cycles_diff < min_diff) min_diff = cycles_diff;		\
    }									\
    if (min_diff <= global_rdtsc_overhead) min_diff = 0;		\
    else min_diff = min_diff - global_rdtsc_overhead;			\
    float cycles_per_op = min_diff / (double)(num_ops);			\
    printf(" %.2f cycles per operation", cycles_per_op);		\
    printf(" (%ld cycles / %ld ops)", min_diff, (uint64_t) num_ops);	\
    if (wrong_answer) printf(" [ERROR]");				\
    printf("\n");							\
    fflush(NULL);							\
  } while (0)


#define BITSET_BITS (1 << 16)
#define BITSET_BYTES (BITSET_BITS / 8)

typedef __m256i ymm_t;

// Count set bits in 32 byte chunks from [ptr, end)
// while (ptr != end) {
//   count = popcnt(ptr[0-32])
//   total = total + count
//   ptr += 32
// }
#define ASM_SUM_POPCNT(total, tmp1, tmp2, tmp3, tmp4, neg, end)         \
    __asm volatile ("1:\n"                                              \
                    "popcnt 0(%6,%5,1), %1\n"                           \
                    "popcnt 8(%6,%5,1), %2\n"                           \
                    "popcnt 16(%6,%5,1), %3\n"                          \
                    "popcnt 24(%6,%5,1), %4\n"                          \
                    "add %1, %0\n"                                      \
                    "add %2, %0\n"                                      \
                    "add %3, %0\n"                                      \
                    "add %4, %0\n"                                      \
                    "add $32, %5\n"                                     \
                    "jnz 1b\n" :                                        \
                    "+&r" (total),                                      \
                    "=&r" (tmp1),                                       \
                    "=&r" (tmp2),                                       \
                    "=&r" (tmp3),                                       \
                    "=&r" (tmp4),                                       \
                    "+&r" (neg) :                                       \
                    "r" (end)                                           \
                    )



#define ASM_POPCNT_AVX2_512(bytes, ymm1, ymm2, ymm3, ymm4, total,       \
                          zero, popcnt, neg, ptr, mask, shuf)           \
    __asm volatile ("vpxor %5, %5, %5\n"                                \
                    "vpxor %6, %6, %6\n"                                \
                    "mov $16, %7\n"                                     \
                    "1:\n"                                              \
                    "vpxor %0, %0, %0\n"                                \
                    "2:\n"                                              \
                    "vmovdqu 0(%9), %1\n"                               \
                    "vmovdqu 32(%9), %2\n"                              \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vmovdqu 64(%9), %1\n"                              \
                    "vmovdqu 96(%9), %2\n"                              \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vmovdqu 128(%9), %1\n"                             \
                    "vmovdqu 160(%9), %2\n"                             \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vmovdqu 192(%9), %1\n"                             \
                    "vmovdqu 224(%9), %2\n"                             \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vmovdqu 256(%9), %1\n"                             \
                    "vmovdqu 288(%9), %2\n"                             \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vmovdqu 320(%9), %1\n"                             \
                    "vmovdqu 352(%9), %2\n"                             \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vmovdqu 384(%9), %1\n"                             \
                    "vmovdqu 416(%9), %2\n"                             \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vmovdqu 448(%9), %1\n"                             \
                    "vmovdqu 480(%9), %2\n"                             \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vpsadbw %6, %0, %0\n"                              \
                    "vpaddd %0, %5, %5\n"                               \
                    "add $512, %9\n"                                    \
                    "dec %7\n"                                          \
                    "jnz 1b\n"                                          \
                    "4:\n"                                              \
                    "vextracti128 $1, %5, %q0\n"                        \
                    "vpaddd %q5, %q0, %q0\n"                            \
                    "vmovhlps %q0, %q0, %q1\n"                          \
                    "vpaddd %q0, %q1, %q0\n"                            \
                    "movd %q0, %7" :                                    \
                    "=&x" (bytes),                                      \
                    "=&x" (ymm1),                                       \
                    "=&x" (ymm2),                                       \
                    "=&x" (ymm3),                                       \
                    "=&x" (ymm4),                                       \
                    "=&x" (total),                                      \
                    "=&x" (zero),                                       \
                    "=&r" (popcnt),                                     \
                    "=&r" (neg),                                        \
                    "+&r" (ptr) :                                       \
                    "x" (mask),                                         \
                    "x" (shuf)                                          \
                    )

#define ASM_POPCNT_AVX2_128(bytes, ymm1, ymm2, ymm3, ymm4, total,       \
                            zero, popcnt, neg, ptr, mask, shuf)         \
    __asm volatile ("vpxor %5, %5, %5\n"                                \
                    "vpxor %6, %6, %6\n"                                \
                    "mov $16, %7\n"                                     \
                    "1:\n"                                              \
                    "vpxor %0, %0, %0\n"                                \
                    "mov $-512, %8\n"                                   \
                    "add $512, %9\n"                                    \
                    "2:\n"                                              \
                    "vmovdqu 0(%9,%8,1), %1\n"                          \
                    "vmovdqu 32(%9,%8,1), %2\n"                         \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "vmovdqu 64(%9,%8,1), %1\n"                         \
                    "vmovdqu 96(%9,%8,1), %2\n"                         \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "add $128, %8\n"                                    \
                    "jnz 2b\n"                                          \
                    "vpsadbw %6, %0, %0\n"                              \
                    "vpaddd %0, %5, %5\n"                               \
                    "dec %7\n"                                          \
                    "jnz 1b\n"                                          \
                    "4:\n"                                              \
                    "vextracti128 $1, %5, %q0\n"                        \
                    "vpaddd %q5, %q0, %q0\n"                            \
                    "vmovhlps %q0, %q0, %q1\n"                          \
                    "vpaddd %q0, %q1, %q0\n"                            \
                    "movd %q0, %7" :                                    \
                    "=&x" (bytes),                                      \
                    "=&x" (ymm1),                                       \
                    "=&x" (ymm2),                                       \
                    "=&x" (ymm3),                                       \
                    "=&x" (ymm4),                                       \
                    "=&x" (total),                                      \
                    "=&x" (zero),                                       \
                    "=&r" (popcnt),                                     \
                    "=&r" (neg),                                        \
                    "+&r" (ptr) :                                       \
                    "x" (mask),                                         \
                    "x" (shuf)                                          \
                    )

#define ASM_POPCNT_AVX2_64(bytes, ymm1, ymm2, ymm3, ymm4, total,        \
                           zero, popcnt, neg, ptr, mask, shuf)          \
    __asm volatile ("vpxor %5, %5, %5\n"                                \
                    "vpxor %6, %6, %6\n"                                \
                    "mov $16, %7\n"                                     \
                    "1:\n"                                              \
                    "vpxor %0, %0, %0\n"                                \
                    "mov $-512, %8\n"                                   \
                    "add $512, %9\n"                                    \
                    "2:\n"                                              \
                    "vmovdqu 0(%9,%8,1), %1\n"                          \
                    "vmovdqu 32(%9,%8,1), %2\n"                         \
                    "vpsrld $4, %1, %3\n"                               \
                    "vpsrld $4, %2, %4\n"                               \
                    "vpand %10, %1, %1\n"                               \
                    "vpand %10, %2, %2\n"                               \
                    "vpand %10, %3, %3\n"                               \
                    "vpand %10, %4, %4\n"                               \
                    "vpshufb %1, %11, %1\n"                             \
                    "vpshufb %2, %11, %2\n"                             \
                    "vpshufb %3, %11, %3\n"                             \
                    "vpshufb %4, %11, %4\n"                             \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "vpaddb %3, %0, %0\n"                               \
                    "vpaddb %4, %0, %0\n"                               \
                    "add $64, %8\n"                                     \
                    "jnz 2b\n"                                          \
                    "vpsadbw %6, %0, %0\n"                              \
                    "vpaddd %0, %5, %5\n"                               \
                    "dec %7\n"                                          \
                    "jnz 1b\n"                                          \
                    "4:\n"                                              \
                    "vextracti128 $1, %5, %q0\n"                        \
                    "vpaddd %q5, %q0, %q0\n"                            \
                    "vmovhlps %q0, %q0, %q1\n"                          \
                    "vpaddd %q0, %q1, %q0\n"                            \
                    "movd %q0, %7" :                                    \
                    "=&x" (bytes),                                      \
                    "=&x" (ymm1),                                       \
                    "=&x" (ymm2),                                       \
                    "=&x" (ymm3),                                       \
                    "=&x" (ymm4),                                       \
                    "=&x" (total),                                      \
                    "=&x" (zero),                                       \
                    "=&r" (popcnt),                                     \
                    "=&r" (neg),                                        \
                    "+&r" (ptr) :                                       \
                    "x" (mask),                                         \
                    "x" (shuf)                                          \
                    )

#define ASM_POPCNT_AVX2_32(bytes, ymm1, ymm2, total, zero,              \
                           popcnt, neg, ptr, mask, shuf)                \
    __asm volatile ("vpxor %3, %3, %3\n"                                \
                    "vpxor %4, %4, %4\n"                                \
                    "mov $16, %5\n"                                     \
                    "1:\n"                                              \
                    "vpxor %0, %0, %0\n"                                \
                    "mov $-512, %6\n"                                   \
                    "add $512, %7\n"                                    \
                    "2:\n"                                              \
                    "vmovdqu 0(%7,%6,1), %1\n"                          \
                    "vpsrld $4, %1, %2\n"                               \
                    "vpand %8, %1, %1\n"                                \
                    "vpand %8, %2, %2\n"                                \
                    "vpshufb %1, %9, %1\n"                              \
                    "vpshufb %2, %9, %2\n"                              \
                    "vpaddb %1, %0, %0\n"                               \
                    "vpaddb %2, %0, %0\n"                               \
                    "add $32, %6\n"                                     \
                    "jnz 2b\n"                                          \
                    "vpsadbw %4, %0, %0\n"                              \
                    "vpaddd %0, %3, %3\n"                               \
                    "dec %5\n"                                          \
                    "jnz 1b\n"                                          \
                    "4:\n"                                              \
                    "vextracti128 $1, %3, %q0\n"                        \
                    "vpaddd %q3, %q0, %q0\n"                            \
                    "vmovhlps %q0, %q0, %q1\n"                          \
                    "vpaddd %q0, %q1, %q0\n"                            \
                    "movd %q0, %5" :                                    \
                    "=&x" (bytes),                                      \
                    "=&x" (ymm1),                                       \
                    "=&x" (ymm2),                                       \
                    "=&x" (total),                                      \
                    "=&x" (zero),                                       \
                    "=&r" (popcnt),                                     \
                    "=&r" (neg),                                        \
                    "+&r" (ptr) :                                       \
                    "x" (mask),                                         \
                    "x" (shuf)                                          \
                    )

uint64_t bitset_count_popcnt(uint8_t *bitset) {
    uint64_t count1, count2, count3, count4;
    uint64_t total_count = 0;
    uint8_t *end = bitset + BITSET_BYTES;
    int64_t neg = -(BITSET_BYTES);

    ASM_SUM_POPCNT(total_count, count1, count2, count3, count4, neg, end);

    return total_count;
}

uint64_t intrinsics_countloop4(uint8_t *bitset) {
    // these are precomputed hamming weights (weight(0), weight(1)...)
    const __m256i shuf = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                          0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const __m256i  mask = _mm256_set1_epi8(0x0f); // low 4 bits of each byte
    __m256i total = _mm256_setzero_si256();
    __m256i zero = _mm256_setzero_si256();
    const int inner = 4;
    for(unsigned  k = 0; k < BITSET_BYTES/(sizeof(__m256i)*inner); k++) {
        __m256i innertotal = _mm256_setzero_si256();
        for(int j = 0; j < inner; ++j) {
            __m256i ymm1 = _mm256_lddqu_si256((const __m256i *)bitset + k*inner + j);
            __m256i ymm2 = _mm256_srli_epi32(ymm1,4); // shift right, shiftingin zeroes
            ymm1 = _mm256_and_si256(ymm1,mask); // contains even 4 bits
            ymm2 = _mm256_and_si256(ymm2,mask); // contains odd 4 bits
            ymm1 = _mm256_shuffle_epi8(shuf,ymm1);// use table look-up to sum the 4 bits
            ymm2 = _mm256_shuffle_epi8(shuf,ymm2);
            innertotal = _mm256_add_epi8(innertotal,ymm1);// inner total values in each byte are bounded by 8 * inner
            innertotal = _mm256_add_epi8(innertotal,ymm2);// inner total values in each byte are bounded by 8 * inner
        }
       innertotal = _mm256_sad_epu8(zero,innertotal);// produces 4 64-bit counters (having values in [0,8 * inner * 4])
        total= _mm256_add_epi64(total,innertotal); // add the 4 64-bit counters to previous counter
    }
    return _mm256_extract_epi64(total,0)+_mm256_extract_epi64(total,1)+_mm256_extract_epi64(total,2)+_mm256_extract_epi64(total,3);
}

uint64_t intrinsics_count4(uint8_t *bitset) {
    // these are precomputed hamming weights (weight(0), weight(1)...)
    const __m256i shuf = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                          0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const __m256i  mask = _mm256_set1_epi8(0x0f); // low 4 bits of each byte
    __m256i total = _mm256_setzero_si256();
    __m256i zero = _mm256_setzero_si256();
    const int inner = 4;
    for(unsigned  k = 0; k < BITSET_BYTES/(sizeof(__m256i)*inner); k++) {
        __m256i innertotal = _mm256_setzero_si256();
        {
            __m256i ymm1 = _mm256_lddqu_si256((const __m256i *)bitset + k*inner + 0);
            __m256i ymm2 = _mm256_srli_epi32(ymm1,4); // shift right, shiftingin zeroes
            ymm1 = _mm256_and_si256(ymm1,mask); // contains even 4 bits
            ymm2 = _mm256_and_si256(ymm2,mask); // contains odd 4 bits
            ymm1 = _mm256_shuffle_epi8(shuf,ymm1);// use table look-up to sum the 4 bits
            ymm2 = _mm256_shuffle_epi8(shuf,ymm2);
            innertotal = _mm256_add_epi8(innertotal,ymm1);// inner total values in each byte are bounded by 8 * inner
            innertotal = _mm256_add_epi8(innertotal,ymm2);// inner total values in each byte are bounded by 8 * inner
        }
        {
            __m256i ymm1 = _mm256_lddqu_si256((const __m256i *)bitset + k*inner + 1);
            __m256i ymm2 = _mm256_srli_epi32(ymm1,4); // shift right, shiftingin zeroes
            ymm1 = _mm256_and_si256(ymm1,mask); // contains even 4 bits
            ymm2 = _mm256_and_si256(ymm2,mask); // contains odd 4 bits
            ymm1 = _mm256_shuffle_epi8(shuf,ymm1);// use table look-up to sum the 4 bits
            ymm2 = _mm256_shuffle_epi8(shuf,ymm2);
            innertotal = _mm256_add_epi8(innertotal,ymm1);// inner total values in each byte are bounded by 8 * inner
            innertotal = _mm256_add_epi8(innertotal,ymm2);// inner total values in each byte are bounded by 8 * inner
        }
        {
            __m256i ymm1 = _mm256_lddqu_si256((const __m256i *)bitset + k*inner + 2);
            __m256i ymm2 = _mm256_srli_epi32(ymm1,4); // shift right, shiftingin zeroes
            ymm1 = _mm256_and_si256(ymm1,mask); // contains even 4 bits
            ymm2 = _mm256_and_si256(ymm2,mask); // contains odd 4 bits
            ymm1 = _mm256_shuffle_epi8(shuf,ymm1);// use table look-up to sum the 4 bits
            ymm2 = _mm256_shuffle_epi8(shuf,ymm2);
            innertotal = _mm256_add_epi8(innertotal,ymm1);// inner total values in each byte are bounded by 8 * inner
            innertotal = _mm256_add_epi8(innertotal,ymm2);// inner total values in each byte are bounded by 8 * inner
        }
        {
            __m256i ymm1 = _mm256_lddqu_si256((const __m256i *)bitset + k*inner + 3);
            __m256i ymm2 = _mm256_srli_epi32(ymm1,4); // shift right, shiftingin zeroes
            ymm1 = _mm256_and_si256(ymm1,mask); // contains even 4 bits
            ymm2 = _mm256_and_si256(ymm2,mask); // contains odd 4 bits
            ymm1 = _mm256_shuffle_epi8(shuf,ymm1);// use table look-up to sum the 4 bits
            ymm2 = _mm256_shuffle_epi8(shuf,ymm2);
            innertotal = _mm256_add_epi8(innertotal,ymm1);// inner total values in each byte are bounded by 8 * inner
            innertotal = _mm256_add_epi8(innertotal,ymm2);// inner total values in each byte are bounded by 8 * inner
        }
        innertotal = _mm256_sad_epu8(zero,innertotal);// produces 4 64-bit counters (having values in [0,8 * inner * 4])
        total= _mm256_add_epi64(total,innertotal); // add the 4 64-bit counters to previous counter
    }
    return _mm256_extract_epi64(total,0)+_mm256_extract_epi64(total,1)+_mm256_extract_epi64(total,2)+_mm256_extract_epi64(total,3);
}


uint64_t bitset_count_avx2_32(uint8_t *bitset) {
    ymm_t bytes, ymm1, ymm2, total, zero;
    uint64_t popcnt, neg;

    /* 0000 0001 0010 0011 0100 0101 0110 0111 1000 1000 1001 1010 1011 1100 1101 1110 1111 */
    const ymm_t shuf = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const ymm_t mask = _mm256_set1_epi8(0x0f); // low 4 bits of each byte

    ASM_POPCNT_AVX2_32(bytes, ymm1, ymm2, total, zero,
                       popcnt, neg, bitset, mask, shuf);

    return popcnt;
}

uint64_t bitset_count_avx2_64(uint8_t *bitset) {
    ymm_t bytes, ymm1, ymm2, ymm3, ymm4, total, zero;
    uint64_t popcnt, neg;

    /* 0000 0001 0010 0011 0100 0101 0110 0111 1000 1000 1001 1010 1011 1100 1101 1110 1111 */
    const ymm_t shuf = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const ymm_t mask = _mm256_set1_epi8(0x0f); // low 4 bits of each byte

    ASM_POPCNT_AVX2_64(bytes, ymm1, ymm2, ymm3, ymm4, total,
                       zero, popcnt, neg, bitset, mask, shuf);

    return popcnt;
}

uint64_t bitset_count_avx2_128(uint8_t *bitset) {
    ymm_t bytes, ymm1, ymm2, ymm3, ymm4, total, zero;
    uint64_t popcnt, neg;

    /* 0000 0001 0010 0011 0100 0101 0110 0111 1000 1000 1001 1010 1011 1100 1101 1110 1111 */
    const ymm_t shuf = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const ymm_t mask = _mm256_set1_epi8(0x0f); // low 4 bits of each byte

    ASM_POPCNT_AVX2_128(bytes, ymm1, ymm2, ymm3, ymm4, total,
                        zero, popcnt, neg, bitset, mask, shuf);

    return popcnt;
}

uint64_t bitset_count_avx2_512(uint8_t *bitset) {
    ymm_t bytes, ymm1, ymm2, ymm3, ymm4, total, zero;
    uint64_t popcnt, neg;

    /* 0000 0001 0010 0011 0100 0101 0110 0111 1000 1000 1001 1010 1011 1100 1101 1110 1111 */
    const ymm_t shuf = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const ymm_t mask = _mm256_set1_epi8(0x0f); // low 4 bits of each byte

    ASM_POPCNT_AVX2_512(bytes, ymm1, ymm2, ymm3, ymm4, total,
                        zero, popcnt, neg, bitset, mask, shuf);

    return popcnt;
}


void *aligned_malloc(size_t alignment, size_t size) {
    void *aligned;
    if (posix_memalign(&aligned, alignment, size)) exit(1);
    return aligned;
}

#define RDTSC_REPEAT 100

int main(/* int argc, char **argv */) {
    uint8_t *bitset = aligned_malloc(sizeof(ymm_t), BITSET_BYTES);
    memset(bitset, 0xF1, BITSET_BYTES);

    uint64_t count = bitset_count_popcnt(bitset);

    RDTSC_BEST(intrinsics_count4(bitset), count, RDTSC_REPEAT, BITSET_BYTES / 8);
    RDTSC_BEST(intrinsics_countloop4(bitset), count, RDTSC_REPEAT, BITSET_BYTES / 8);
    RDTSC_BEST(bitset_count_popcnt(bitset), count, RDTSC_REPEAT, BITSET_BYTES / 8);
    RDTSC_BEST(bitset_count_avx2_32(bitset), count, RDTSC_REPEAT, BITSET_BYTES / 8);
    RDTSC_BEST(bitset_count_avx2_64(bitset), count, RDTSC_REPEAT, BITSET_BYTES / 8);
    RDTSC_BEST(bitset_count_avx2_128(bitset), count, RDTSC_REPEAT, BITSET_BYTES / 8);
    RDTSC_BEST(bitset_count_avx2_512(bitset), count, RDTSC_REPEAT, BITSET_BYTES / 8);

    return 0;
}
