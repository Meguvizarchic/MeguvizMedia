#include "../src/alpha.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "mxcsr_daz.h"

static float from_bits(std::uint32_t bits){float value;std::memcpy(&value,&bits,sizeof(value));return value;}
static std::uint64_t bits_of(double value){std::uint64_t bits;std::memcpy(&bits,&value,sizeof(bits));return bits;}
struct FloatMode {
#if defined(MGM_TEST_X86_DAZ)
    unsigned saved=_mm_getcsr();
    bool available=mgm_test_has_daz_ftz()!=0;
    void ordinary(){_mm_setcsr(saved&~0x8040u);}
    void daz(){if(available)_mm_setcsr(saved|0x8040u);}
    ~FloatMode(){_mm_setcsr(saved);}
#else
    void ordinary(){} void daz(){}
#endif
};
int main(){
    FloatMode mode;mode.ordinary();
    // Every binary32 subnormal has an exact binary64 representation. Compute
    // the reference with DAZ disabled, then run the actual helper with DAZ/FTZ.
    constexpr std::uint32_t count=UINT32_C(0x00800000);
    std::vector<std::uint64_t> reference(count);
    for(std::uint32_t bits=0;bits<count;++bits){
        volatile float input=from_bits(bits);const double value=static_cast<double>(input);
        reference[bits]=bits_of(value);
    }
    mode.daz();
    for(std::uint32_t bits=0;bits<count;++bits){
        float input=from_bits(bits);
        if(!mgm_detail::valid_alpha_float(input)||bits_of(mgm_detail::alpha_from_float(input))!=reference[bits]){
            std::fprintf(stderr,"float subnormal mismatch bits=%08x\n",bits);return 1;
        }
    }
    struct FloatCase{std::uint32_t input;bool valid;std::uint64_t wide;};
    const FloatCase cases[]={
        {UINT32_C(0x80000000),true,UINT64_C(0x8000000000000000)},
        {UINT32_C(0x80000001),false,0},
        {UINT32_C(0x00800000),true,UINT64_C(0x3810000000000000)},
        {UINT32_C(0x3f7fffff),true,UINT64_C(0x3fefffffe0000000)},
        {UINT32_C(0x3f800000),true,UINT64_C(0x3ff0000000000000)},
        {UINT32_C(0x3f800001),false,0},
        {UINT32_C(0x7f800000),false,0},{UINT32_C(0x7fc00001),false,0}};
    for(const auto &c:cases){float input=from_bits(c.input);
        if(mgm_detail::valid_alpha_float(input)!=c.valid||(c.valid&&bits_of(mgm_detail::alpha_from_float(input))!=c.wide)){
            std::fprintf(stderr,"float boundary mismatch bits=%08x\n",c.input);return 1;}}
    struct DoubleCase{std::uint64_t bits;bool valid;};
    const DoubleCase doubles[]={
        {0,true},{UINT64_C(0x8000000000000000),true},{1,true},{UINT64_C(0x8000000000000001),false},
        {UINT64_C(0x3fefffffffffffff),true},{UINT64_C(0x3ff0000000000000),true},
        {UINT64_C(0x3ff0000000000001),false},{UINT64_C(0x7ff0000000000000),false},
        {UINT64_C(0x7ff0000000000001),false},{UINT64_C(0xfff8000000000001),false}};
    for(const auto &c:doubles){double input;std::memcpy(&input,&c.bits,sizeof(input));if(mgm_detail::valid_alpha(input)!=c.valid){std::fprintf(stderr,"double boundary mismatch\n");return 1;}}
#if defined(MGM_TEST_X86_DAZ)
    std::puts(mode.available?"PASS 8388608 binary32 subnormal values and boundaries under DAZ/FTZ":
        "PASS 8388608 binary32 subnormal values and boundaries; DAZ/FTZ unavailable");
#else
    std::puts("PASS 8388608 binary32 subnormal values and boundaries; no DAZ control");
#endif
    return 0;
}
