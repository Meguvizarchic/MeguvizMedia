#pragma once
#include <meguviz/compute.h>
#include <meguviz/detail/code_image_lease.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#if defined(__unix__) || defined(__ANDROID__)
#include <time.h>
#endif

// Fixed ownership slots for the legacy synchronous C constructor. The image
// lease is acquired and released on a non-GL thread. A separate host loader
// reference must remain alive through every prepare/service call and return.
template<class Lease=meguviz::detail::CodeImageLease>
class MgmAndroidCreationPool final {
public:
    enum class State : uint8_t { Empty, Preparing, Ready, Reserved, Claiming, Active, Orphan, OwnerBusy, BackendBusy, Retired, Releasing, Retry };
    static constexpr uint64_t CANCELED=uint64_t(1)<<63,COMMITTED=uint64_t(1)<<62;
    static_assert(std::atomic<State>::is_always_lock_free,"creation admission must not block the GL owner");
    static_assert(std::atomic<uint64_t>::is_always_lock_free&&std::atomic<uintptr_t>::is_always_lock_free,
                  "creation owner identity must not block the GL owner");
    struct Entry { std::atomic<State> state{State::Empty}; Lease lease; void* owner=nullptr;
        std::atomic<uint64_t> owner_token{0},reservation{0},gate{0};std::atomic<uintptr_t> display{0},context{0}; };
    std::array<Entry,8> entries{};
    std::atomic<uint64_t> last_tick{0};
    bool hasReady()const noexcept {for(const auto& e:entries)if(e.state.load(std::memory_order_acquire)==State::Ready)return true;return false;}
    MgcStatus nextReservation(uint64_t* out) noexcept {
        if(!out)return MGC_INVALID_ARGUMENT;*out=0;uint64_t now=0;
#if defined(_WIN32)
        LARGE_INTEGER tick{};if(!QueryPerformanceCounter(&tick)||tick.QuadPart<=0)return MGC_BUSY;now=uint64_t(tick.QuadPart);
#elif defined(__unix__) || defined(__ANDROID__)
        timespec tick{};if(clock_gettime(CLOCK_MONOTONIC,&tick)!=0||tick.tv_sec<0||tick.tv_nsec<0||tick.tv_nsec>=1000000000L)return MGC_BUSY;
        const auto limit=uint64_t((std::numeric_limits<int64_t>::max)()),seconds=uint64_t(tick.tv_sec),fraction=uint64_t(tick.tv_nsec);
        if(seconds>(limit-fraction)/1000000000ULL)return MGC_BACKPRESSURE;now=seconds*1000000000ULL+fraction;
#else
        return MGC_UNSUPPORTED_CAPABILITY;
#endif
        if(!now)return MGC_BUSY;if(now>=COMMITTED)return MGC_BACKPRESSURE;
        auto previous=last_tick.load(std::memory_order_relaxed);
        for(unsigned i=0;i<8;++i){if(previous&&now<=previous)return MGC_BUSY;
            if(last_tick.compare_exchange_weak(previous,now,std::memory_order_relaxed)){
                if(!previous)return MGC_BUSY;*out=now;return MGC_SUCCESS;
            }}
        return MGC_BUSY;
    }
    MgcStatus prepare(const void* address,const char* anchor) noexcept {
        if(!address)return MGC_INVALID_ARGUMENT;
        for(auto& e:entries){auto expected=State::Empty;
            if(!e.state.compare_exchange_strong(expected,State::Preparing,std::memory_order_acq_rel))continue;
            const auto kind=e.lease.acquire(address,anchor);
            if(kind==meguviz::detail::CodeImageKind::DynamicLibrary||kind==meguviz::detail::CodeImageKind::MainExecutable){
                e.state.store(State::Ready,std::memory_order_release);return MGC_SUCCESS;
            }
            const bool reset=e.lease.reset();e.state.store(reset?State::Empty:State::Retry,std::memory_order_release);
            return MGC_UNSUPPORTED_CAPABILITY;
        }
        return MGC_BACKPRESSURE;
    }
    Entry* acquire(void* owner,uint64_t token,uintptr_t display,uintptr_t context) noexcept {
        if(!owner||!token||!display||!context)return nullptr;
        for(auto& e:entries){auto expected=State::Ready;
            if(!e.state.compare_exchange_strong(expected,State::Claiming,std::memory_order_acq_rel))continue;
            e.gate.store(0,std::memory_order_relaxed);e.owner=owner;e.owner_token.store(token,std::memory_order_relaxed);e.display.store(display,std::memory_order_relaxed);
            e.context.store(context,std::memory_order_relaxed);e.state.store(State::Active,std::memory_order_release);return &e;
        }
        return nullptr;
    }
    MgcStatus reserve(uint64_t* out) noexcept {
        if(!out)return MGC_INVALID_ARGUMENT;*out=0;uint64_t id=0;auto status=nextReservation(&id);if(status)return status;
        for(auto& e:entries){auto expected=State::Ready;
            if(!e.state.compare_exchange_strong(expected,State::Claiming,std::memory_order_acq_rel))continue;
            e.reservation.store(id,std::memory_order_relaxed);e.gate.store(id,std::memory_order_relaxed);e.state.store(State::Reserved,std::memory_order_release);*out=id;return MGC_SUCCESS;
        }
        return MGC_BUSY;
    }
    Entry* acquireReserved(void* owner,uint64_t token,uintptr_t display,uintptr_t context,uint64_t reservation) noexcept {
        if(!owner||!token||!display||!context||!reservation)return nullptr;
        for(auto& e:entries){auto expected=State::Reserved;
            if(!e.state.compare_exchange_strong(expected,State::Claiming,std::memory_order_acq_rel))continue;
            if(e.reservation.load(std::memory_order_relaxed)!=reservation||e.gate.load(std::memory_order_acquire)!=reservation){e.state.store(State::Reserved,std::memory_order_release);continue;}
            e.owner=owner;e.owner_token.store(token,std::memory_order_relaxed);e.display.store(display,std::memory_order_relaxed);
            e.context.store(context,std::memory_order_relaxed);e.state.store(State::Active,std::memory_order_release);return &e;
        }
        return nullptr;
    }
    MgcStatus releaseReservation(uint64_t reservation) noexcept {
        if(!reservation)return MGC_INVALID_ARGUMENT;
        for(auto& e:entries){auto state=e.state.load(std::memory_order_acquire);
            if(state==State::Reserved){auto expected=State::Reserved;
                if(!e.state.compare_exchange_strong(expected,State::Claiming,std::memory_order_acq_rel)){state=expected;}
                else{const bool match=e.reservation.load(std::memory_order_relaxed)==reservation;
                    e.state.store(match?State::Retired:State::Reserved,std::memory_order_release);if(match)return MGC_SUCCESS;continue;}}
            // Claiming with this token may be the constructor or a competing
            // release. Do not report success until its outcome is known.
            if(state==State::Claiming&&e.reservation.load(std::memory_order_acquire)==reservation)return MGC_BUSY;
        }
        return MGC_SUCCESS; // consumed or already retired; never touch another generation
    }
    MgcStatus cancel(uint64_t reservation) noexcept {
        if(!reservation||reservation>=COMMITTED)return MGC_INVALID_ARGUMENT;
        for(auto& e:entries){auto gate=e.gate.load(std::memory_order_acquire);
            if(gate==reservation){if(e.gate.compare_exchange_strong(gate,reservation|CANCELED,std::memory_order_acq_rel))return MGC_SUCCESS;}
            if(gate==(reservation|CANCELED))return MGC_SUCCESS;
            if(gate==(reservation|COMMITTED))return MGC_BUSY;
        }
        return MGC_SUCCESS; // stale or already retired
    }
    static MgcStatus commit(Entry& entry,uint64_t reservation) noexcept {
        auto expected=reservation;
        if(entry.gate.compare_exchange_strong(expected,reservation|COMMITTED,std::memory_order_acq_rel))return MGC_SUCCESS;
        return expected==(reservation|CANCELED)?MGC_CANCELLED:MGC_BUSY;
    }
    static bool claim(Entry& e,State from,State busy) noexcept {
        return e.state.compare_exchange_strong(from,busy,std::memory_order_acq_rel);
    }
    static void orphan(Entry& e) noexcept { e.state.store(State::Orphan,std::memory_order_release); }
    static void finish(Entry& e,State next) noexcept {
        if(next==State::Retired){e.owner=nullptr;e.owner_token.store(0,std::memory_order_relaxed);
            e.display.store(0,std::memory_order_relaxed);e.context.store(0,std::memory_order_relaxed);}
        e.state.store(next,std::memory_order_release);
    }
    void discardReady() noexcept {
        for(auto& e:entries){auto ready=State::Ready;
            e.state.compare_exchange_strong(ready,State::Retired,std::memory_order_acq_rel);}
    }
    // The host must keep an independent loader/execution reference through
    // this call: dlclose of our last reference while executing here is unsafe.
    MgcStatus collect() noexcept {
        bool pending=false,failed=false;
        for(auto& e:entries){auto state=e.state.load(std::memory_order_acquire);
            if(state==State::Retired||state==State::Retry){auto expected=state;
                if(e.state.compare_exchange_strong(expected,State::Releasing,std::memory_order_acq_rel)){
                    const bool ok=e.lease.reset();if(ok){e.reservation.store(0,std::memory_order_relaxed);e.gate.store(0,std::memory_order_relaxed);}e.state.store(ok?State::Empty:State::Retry,std::memory_order_release);
                    failed|=!ok;continue;
                }
                state=expected;
            }
            // A healthy live session owns its slot but does not require the
            // background orphan scheduler to spin.
            if(state!=State::Empty&&state!=State::Ready&&state!=State::Reserved&&state!=State::Active)pending=true;
        }
        return failed?MGC_INTERNAL_ERROR:pending?MGC_BUSY:MGC_SUCCESS;
    }
};
