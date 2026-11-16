#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

// One monotonic namespace per linked Media image. C++17 inline local statics
// are shared by its translation units; separate Media DSOs own separate handle
// registries and never exchange these tokens. A reused OS thread id receives a
// new token. Exhaustion or excessive initialization contention fails closed.
inline std::atomic<uint64_t>& mgm_thread_token_counter() noexcept {
    static std::atomic<uint64_t> next{1};return next;
}
inline uint64_t mgm_allocate_thread_token() noexcept {
    auto& next=mgm_thread_token_counter();auto value=next.load(std::memory_order_relaxed);
    for(unsigned attempt=0;attempt<64;++attempt){
        if(!value||value==std::numeric_limits<uint64_t>::max())return 0;
        if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;
    }
    return 0;
}
inline uint64_t mgm_thread_token() noexcept {
    thread_local uint64_t token=0;if(!token)token=mgm_allocate_thread_token();return token;
}
