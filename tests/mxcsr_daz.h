#ifndef MGM_TEST_MXCSR_DAZ_H
#define MGM_TEST_MXCSR_DAZ_H

/* DAZ is not guaranteed on every x86 CPU. FXSAVE's MXCSR_MASK indicates
 * whether setting DAZ and FTZ is legal; do not fault unsupported hosts. */
#if defined(_M_X64) || defined(__x86_64__)
#include <stdint.h>
#include <string.h>
#include <xmmintrin.h>
#if defined(_MSC_VER)
#include <immintrin.h>
#endif
#define MGM_TEST_X86_DAZ 1
static int mgm_test_has_daz_ftz(void){
#if defined(_MSC_VER)
    __declspec(align(16)) unsigned char state[512]={0};
    _fxsave64(state);
#else
    unsigned char state[512] __attribute__((aligned(16)))={0};
    __asm__ volatile("fxsave %0" : "=m"(state));
#endif
    uint32_t mask=0;memcpy(&mask,state+28,sizeof(mask));
    if(!mask)mask=UINT32_C(0x0000ffbf);
    return (mask&UINT32_C(0x8040))==UINT32_C(0x8040);
}
#endif
#endif
