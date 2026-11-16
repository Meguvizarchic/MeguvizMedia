#ifndef MEGUVIZ_MEDIA_ALPHA_HPP
#define MEGUVIZ_MEDIA_ALPHA_HPP

#include <cstdint>
#include <cstring>
#include <limits>

namespace mgm_detail {
/* Compare the input's IEEE-754 representation, not FP registers: DAZ/FTZ
 * must not turn a negative subnormal into an admissible zero. Both zero signs
 * remain valid, and downstream interpolation keeps its existing precision. */
inline bool valid_alpha(const double &value) noexcept {
    static_assert(sizeof(double)==sizeof(std::uint64_t)&&
        std::numeric_limits<double>::is_iec559&&std::numeric_limits<double>::digits==53,
        "Media requires IEEE-754 binary64 double");
    std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));
    constexpr std::uint64_t sign=UINT64_C(0x8000000000000000);
    constexpr std::uint64_t one=UINT64_C(0x3ff0000000000000);
    const std::uint64_t magnitude=bits&~sign;
    return magnitude==0||(!(bits&sign)&&magnitude<=one);
}
inline bool valid_alpha_float(const float &value) noexcept {
    static_assert(sizeof(float)==sizeof(std::uint32_t)&&
        std::numeric_limits<float>::is_iec559&&std::numeric_limits<float>::digits==24,
        "Media requires IEEE-754 binary32 float");
    std::uint32_t bits=0;std::memcpy(&bits,&value,sizeof(bits));
    constexpr std::uint32_t sign=UINT32_C(0x80000000),one=UINT32_C(0x3f800000);
    const std::uint32_t magnitude=bits&~sign;
    return magnitude==0||(!(bits&sign)&&magnitude<=one);
}
/* Widen by representation rather than a float instruction: DAZ may otherwise
 * turn a valid positive binary32 subnormal into double zero before RIFE sees it. */
inline double alpha_from_float(const float &value) noexcept {
    static_assert(sizeof(float)==sizeof(std::uint32_t)&&sizeof(double)==sizeof(std::uint64_t)&&
        std::numeric_limits<float>::is_iec559&&std::numeric_limits<float>::digits==24&&
        std::numeric_limits<double>::is_iec559&&std::numeric_limits<double>::digits==53,
        "Media requires IEEE-754 binary32 and binary64");
    std::uint32_t bits=0;std::memcpy(&bits,&value,sizeof(bits));
    const std::uint64_t sign=std::uint64_t(bits&UINT32_C(0x80000000))<<32;
    const std::uint32_t exponent=(bits>>23)&UINT32_C(0xff),fraction=bits&UINT32_C(0x7fffff);
    std::uint64_t wide=sign;
    if(exponent==0&&fraction){
        unsigned highest=0;for(std::uint32_t v=fraction;v>1;v>>=1)++highest;
        wide|=std::uint64_t(highest+874)<<52;
        wide|=std::uint64_t(fraction^(UINT32_C(1)<<highest))<<(52-highest);
    }else if(exponent==UINT32_C(0xff)){
        wide|=UINT64_C(0x7ff0000000000000)|(std::uint64_t(fraction)<<29);
    }else if(exponent){
        wide|=std::uint64_t(exponent+896)<<52;wide|=std::uint64_t(fraction)<<29;
    }
    double result;std::memcpy(&result,&wide,sizeof(result));return result;
}
}
#endif
