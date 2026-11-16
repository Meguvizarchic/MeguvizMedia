#ifndef MEGUVIZ_MEDIA_RIFE_INTERPOLATE_PTS_HPP
#define MEGUVIZ_MEDIA_RIFE_INTERPOLATE_PTS_HPP

#include <cstdint>
#include <cstring>

namespace mgm_detail {
// Inputs are nonnegative frame PTS and a previously validated binary64 alpha
// in [0,1]. Evaluate the exact rational represented by alpha, then round the
// resulting nonnegative PTS to nearest tick, with half ticks rounded upward.
inline int64_t interpolate_pts(int64_t left,int64_t right,const double &alpha) noexcept {
    uint64_t bits=0;std::memcpy(&bits,&alpha,sizeof(bits));bits&=UINT64_C(0x7fffffffffffffff);
    const uint64_t exponent=(bits>>52)&UINT64_C(0x7ff);
    uint64_t factor=bits&UINT64_C(0x000fffffffffffff);
    if(exponent)factor|=UINT64_C(1)<<52;
    const unsigned shift=exponent?unsigned(1075-exponent):1074;
    const bool rising=right>=left;
    const uint64_t distance=rising?uint64_t(right)-uint64_t(left):uint64_t(left)-uint64_t(right);
    // Portable 64 x 64 -> 128 multiplication, including MSVC and ARMv7.
    const uint64_t a0=uint32_t(distance),a1=distance>>32,b0=uint32_t(factor),b1=factor>>32;
    const uint64_t p00=a0*b0,p01=a0*b1,p10=a1*b0,p11=a1*b1;
    const uint64_t middle=(p00>>32)+uint32_t(p01)+uint32_t(p10);
    const uint64_t low=(middle<<32)|uint32_t(p00);
    const uint64_t high=p11+(p01>>32)+(p10>>32)+(middle>>32);
    uint64_t whole=0;bool half=false,below=false;
    if(shift<64){
        whole=(low>>shift)|(high<<(64-shift));
        half=((low>>(shift-1))&1)!=0;
        below=(low&((UINT64_C(1)<<(shift-1))-1))!=0;
    }else if(shift==64){
        whole=high;half=(low>>63)!=0;below=(low&UINT64_C(0x7fffffffffffffff))!=0;
    }else if(shift<128){
        const unsigned high_shift=shift-64;
        whole=high>>high_shift;half=((high>>(high_shift-1))&1)!=0;
        below=low!=0||(high&((UINT64_C(1)<<(high_shift-1))-1))!=0;
    }
    // For a falling interval, an exact half tick rounds toward the higher PTS.
    if(half&&(rising||below))++whole;
    return rising?int64_t(uint64_t(left)+whole):int64_t(uint64_t(left)-whole);
}
}
#endif
