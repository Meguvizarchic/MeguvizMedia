#pragma once

#include <meguviz/compute.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include "thread_identity.hpp"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__unix__) || defined(__ANDROID__)
#include <time.h>
#endif

// Internal, fixed-capacity ownership ledger. The caller must reserve before the
// first GL/Vulkan operation, bind actual eglGetCurrentDisplay/Context values,
// and attach a platform pointer before it can become externally in use. The
// owner pin keeps the JNI/engine/AHB owner alive; code pin is an independently
// verified loader reference (or a verified main-executable lifetime object).
// Neither pointer identity nor a Vulkan idle result proves GL/producer retire.
class MgmCreationOrphans final {
public:
    enum class Kind : uint8_t { GlPoll, GlClose, BackgroundService, BackgroundFinalize };
    enum class Outcome : uint8_t { Success, Retryable, Uncertain };
    struct Pins {
        std::shared_ptr<void> owner,code;
        bool empty()const noexcept{return !owner&&!code;}
    };
    class Claim final {
        friend class MgmCreationOrphans;
        uint64_t receipt_=0, serial_=0, threadToken_=0;
        Kind kind_=Kind::GlPoll;
        std::thread::id thread_{};
        void* platform_=nullptr;
        std::shared_ptr<void> owner_,code_;
    public:
        Claim()=default;
        Claim(const Claim&)=delete;
        Claim& operator=(const Claim&)=delete;
        bool active()const noexcept{return receipt_!=0;}
        void* platform()const noexcept{return platform_;}
        uint64_t receipt()const noexcept{return receipt_;}
        const std::shared_ptr<void>& owner()const noexcept{return owner_;}
        const std::shared_ptr<void>& code()const noexcept{return code_;}
    };
private:
    enum class Phase : uint8_t { Empty, Reserved, Creating, Adoptable, Orphan, Done, Stuck };
    struct Entry {
        uint64_t receipt=0, nextSerial=0, ownerToken=0;
        std::thread::id ownerThread{};
        uintptr_t display=0,context=0;
        Phase phase=Phase::Empty;
        void* platform=nullptr;
        std::shared_ptr<void> owner,code;
        std::array<uint64_t,4> claims{};
        bool glClosed=false,backgroundClosed=false;
    };
    std::mutex mutex_;
    std::array<Entry,8> entries_{};
    static size_t slot(Kind kind){return static_cast<size_t>(kind);}
    // One namespace per loaded Media image. The first observed tick becomes
    // the floor and is never issued. On sequential unload/reload the new floor
    // is at least the old image's final issued tick. Raw ids must never be
    // exchanged across simultaneously loaded DSOs or process/boot boundaries.
    static std::atomic<uint64_t>& nextReceipt(){static std::atomic<uint64_t> value{0};return value;}
    static MgcStatus publishClock(std::atomic<uint64_t>& last,uint64_t now,uint64_t& out){
        if(!now)return MGC_BUSY;
        if(now>=uint64_t((std::numeric_limits<int64_t>::max)()))return MGC_BACKPRESSURE;
        uint64_t value=last.load(std::memory_order_relaxed);
        for(unsigned attempt=0;attempt<8;++attempt){
            if(value>=uint64_t((std::numeric_limits<int64_t>::max)()))return MGC_BACKPRESSURE;
            if(value&&now<=value)return MGC_BUSY;
            if(last.compare_exchange_weak(value,now,std::memory_order_relaxed)){
                if(!value)return MGC_BUSY; // establish the image's floor
                out=now;return MGC_SUCCESS;
            }
        }
        return MGC_BUSY;
    }
    static MgcStatus readClock(uint64_t& out){
#if defined(_WIN32)
        LARGE_INTEGER value{};if(!QueryPerformanceCounter(&value)||value.QuadPart<=0)return MGC_BUSY;
        out=uint64_t(value.QuadPart);return MGC_SUCCESS;
#elif defined(__unix__) || defined(__ANDROID__)
        timespec value{};if(clock_gettime(CLOCK_MONOTONIC,&value)!=0||value.tv_sec<0||
            value.tv_nsec<0||value.tv_nsec>=1000000000L)return MGC_BUSY;
        const auto seconds=uint64_t(value.tv_sec),fraction=uint64_t(value.tv_nsec);
        const auto limit=uint64_t((std::numeric_limits<int64_t>::max)());
        if(seconds>(limit-fraction)/1000000000ULL)return MGC_BACKPRESSURE;
        out=seconds*1000000000ULL+fraction;return out?MGC_SUCCESS:MGC_BUSY;
#else
        (void)out;return MGC_UNSUPPORTED_CAPABILITY;
#endif
    }
    // std::thread::id may be reused after a thread exits. The shared Media-image
    // TLS token disambiguates thread lifetimes across these translation units.
    static MgcStatus currentThreadToken(uint64_t& out){
        out=mgm_thread_token();return out?MGC_SUCCESS:MGC_BUSY;
    }
    Entry* find(uint64_t id){for(auto& entry:entries_)if(entry.phase!=Phase::Empty&&entry.receipt==id)return &entry;return nullptr;}
    static MgcStatus ownerStatus(const Entry& entry,uintptr_t display,uintptr_t context){
        if(entry.ownerThread!=std::this_thread::get_id()||entry.display!=display||entry.context!=context)return MGC_INCOMPATIBLE_DOMAIN;
        uint64_t token=0;const auto status=currentThreadToken(token);if(status)return status;
        return token==entry.ownerToken?MGC_SUCCESS:MGC_INCOMPATIBLE_DOMAIN;
    }
    static bool anyClaims(const Entry& entry){for(auto claim:entry.claims)if(claim)return true;return false;}
    static void clearEntry(Entry& entry,std::shared_ptr<void>& owner,std::shared_ptr<void>& code){
        owner=std::move(entry.owner);code=std::move(entry.code);
        entry.receipt=0;entry.nextSerial=entry.ownerToken=0;entry.ownerThread={};entry.display=entry.context=0;
        entry.platform=nullptr;entry.claims={};entry.glClosed=entry.backgroundClosed=false;entry.phase=Phase::Empty;
    }
public:
    // A registry with unresolved receipts must never be destroyed. Construct a
    // single process-lifetime instance via new; the bounded ledger is reclaimed
    // by process teardown, while each acknowledged receipt releases its pins.
    MgmCreationOrphans()=default;
    ~MgmCreationOrphans()=delete;
    static MgmCreationOrphans* create() noexcept{try{return new(std::nothrow) MgmCreationOrphans;}catch(...){return nullptr;}}
    // The receipt is a positive JNI jlong. Returning BACKPRESSURE performs no
    // platform work and consumes no owner/code reference.
    MgcStatus reserve(uintptr_t display,uintptr_t context,const std::shared_ptr<void>& owner,
                      const std::shared_ptr<void>& code,uint64_t* receipt){
        if(receipt)*receipt=0;
        if(!receipt||!display||!context||!owner||!code)return MGC_INVALID_ARGUMENT;
        uint64_t threadToken=0;auto threadStatus=currentThreadToken(threadToken);if(threadStatus)return threadStatus;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;
        Entry* free=nullptr;for(auto& entry:entries_)if(entry.phase==Phase::Empty){free=&entry;break;}
        if(!free)return MGC_BACKPRESSURE;
        uint64_t tick=0,id=0;auto status=readClock(tick);if(status)return status;
        status=publishClock(nextReceipt(),tick,id);if(status)return status;
        free->receipt=id;free->ownerThread=std::this_thread::get_id();free->ownerToken=threadToken;free->display=display;free->context=context;
        free->owner=owner;free->code=code;free->phase=Phase::Reserved;*receipt=id;return MGC_SUCCESS;
    }
    MgcStatus begin(uint64_t id,uintptr_t display,uintptr_t context){
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
        if(auto status=ownerStatus(*e,display,context))return status;
        if(e->phase!=Phase::Reserved)return MGC_BUSY;e->phase=Phase::Creating;return MGC_SUCCESS;
    }
    // Attach immediately after platform allocation, before that allocation can
    // submit work. Ownership remains with the strong owner, not this raw view.
    MgcStatus attachPlatform(uint64_t id,uintptr_t display,uintptr_t context,void* platform){
        if(!platform)return MGC_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
        if(auto status=ownerStatus(*e,display,context))return status;
        if(e->phase!=Phase::Creating||e->platform)return MGC_BUSY;e->platform=platform;return MGC_SUCCESS;
    }
    MgcStatus adopt(uint64_t id,uintptr_t display,uintptr_t context){
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
        if(auto status=ownerStatus(*e,display,context))return status;
        if(e->phase!=Phase::Creating||!e->platform)return MGC_BUSY;e->phase=Phase::Adoptable;return MGC_SUCCESS;
    }
    // Only call after the JNI result array is allocated and the receiving
    // registry owns the successfully created Session. Commit is not retire.
    MgcStatus commit(uint64_t id,uintptr_t display,uintptr_t context,Pins& transferred){
        if(!transferred.empty())return MGC_INVALID_ARGUMENT;
        {std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
            if(auto status=ownerStatus(*e,display,context))return status;
            if(e->phase!=Phase::Adoptable||anyClaims(*e))return MGC_BUSY;clearEntry(*e,transferred.owner,transferred.code);}
        return MGC_SUCCESS; // caller owns pins and chooses a nonblocking drop
    }
    // A failed creation, including JNI array allocation failure, keeps both
    // owner and code pins until independently established GL + backend retire.
    MgcStatus abort(uint64_t id,uintptr_t display,uintptr_t context){
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
        if(auto status=ownerStatus(*e,display,context))return status;
        if(e->phase==Phase::Orphan)return MGC_SUCCESS;
        if(e->phase!=Phase::Reserved&&e->phase!=Phase::Creating&&e->phase!=Phase::Adoptable)return MGC_BUSY;
        e->phase=Phase::Orphan;return MGC_SUCCESS;
    }
    // GlPoll must not destroy platform. GlClose may destroy it and is only
    // granted when every concurrent background/platform access has finished.
    // BackgroundService may run beside GlPoll, but not GlClose. Finalize is
    // granted only after real GL close; all driver work happens outside lock.
    MgcStatus claim(uint64_t id,Kind kind,uintptr_t display,uintptr_t context,Claim& out){
        if(out.active()||slot(kind)>=4)return MGC_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
        const bool gl=kind==Kind::GlPoll||kind==Kind::GlClose;
        if(gl){if(auto status=ownerStatus(*e,display,context))return status;}
        uint64_t threadToken=0;const auto threadStatus=currentThreadToken(threadToken);if(threadStatus)return threadStatus;
        if(!gl&&e->ownerToken==threadToken)return MGC_INCOMPATIBLE_DOMAIN;
        if(e->phase==Phase::Stuck)return MGC_INTERNAL_ERROR;
        if(e->phase!=Phase::Orphan)return MGC_BUSY;
        auto& c=e->claims;const auto n=slot(kind);
        if(c[n]||e->nextSerial==std::numeric_limits<uint64_t>::max())return MGC_BUSY;
        if(kind==Kind::GlPoll&&(e->glClosed||c[slot(Kind::GlClose)]||c[slot(Kind::BackgroundFinalize)]))return MGC_BUSY;
        if(kind==Kind::GlClose&&(e->glClosed||anyClaims(*e)))return MGC_BUSY;
        if(kind==Kind::BackgroundService&&(e->glClosed||c[slot(Kind::GlClose)]||c[slot(Kind::BackgroundFinalize)]))return MGC_BUSY;
        if(kind==Kind::BackgroundFinalize&&(!e->glClosed||e->backgroundClosed||anyClaims(*e)))return MGC_BUSY;
        c[n]=++e->nextSerial;out.receipt_=id;out.serial_=c[n];out.kind_=kind;out.thread_=std::this_thread::get_id();out.threadToken_=threadToken;
        out.platform_=e->platform;out.owner_=e->owner;out.code_=e->code;return MGC_ACCEPTED;
    }
    // Success is an assertion by the caller that the specific claimed action
    // returned successful physical retirement; unknown effects must use
    // Uncertain and remain pinned. A missing finish retains the claim forever.
    MgcStatus finish(Claim& claim,Outcome outcome,uintptr_t display=0,uintptr_t context=0){
        if(!claim.active()||slot(claim.kind_)>=4||claim.thread_!=std::this_thread::get_id()||
           (outcome!=Outcome::Success&&outcome!=Outcome::Retryable&&outcome!=Outcome::Uncertain))return MGC_INVALID_ARGUMENT;
        uint64_t threadToken=0;const auto threadStatus=currentThreadToken(threadToken);if(threadStatus)return threadStatus;
        if(threadToken!=claim.threadToken_)return MGC_INVALID_ARGUMENT;
        std::shared_ptr<void> owner,code;MgcStatus result=MGC_SUCCESS;
        {std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(claim.receipt_);if(!e)return MGC_INVALID_ARGUMENT;
            const auto n=slot(claim.kind_);if(e->claims[n]!=claim.serial_||e->phase!=Phase::Orphan)return MGC_INVALID_ARGUMENT;
            const bool gl=claim.kind_==Kind::GlPoll||claim.kind_==Kind::GlClose;
            const auto ownerResult=gl?ownerStatus(*e,display,context):MGC_SUCCESS;
            if(ownerResult){e->phase=Phase::Stuck;result=ownerResult;}
            else if(outcome==Outcome::Uncertain){e->phase=Phase::Stuck;result=MGC_INTERNAL_ERROR;}
            else if(outcome==Outcome::Success){
                if(claim.kind_==Kind::GlClose){e->glClosed=true;e->platform=nullptr;}
                if(claim.kind_==Kind::BackgroundFinalize)e->backgroundClosed=true;
            }
            e->claims[n]=0;
            if(e->phase==Phase::Orphan&&e->glClosed&&e->backgroundClosed&&!anyClaims(*e))e->phase=Phase::Done;
            owner=std::move(claim.owner_);code=std::move(claim.code_);claim.receipt_=claim.serial_=claim.threadToken_=0;claim.platform_=nullptr;
        }
        return result;
    }
    // SUCCESS means the exact receipt reached DONE, never merely unknown or
    // evicted. DONE occupies a slot until explicit acknowledgement.
    MgcStatus poll(uint64_t id,uintptr_t display,uintptr_t context){
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
        if(auto status=ownerStatus(*e,display,context))return status;
        return e->phase==Phase::Done?MGC_SUCCESS:e->phase==Phase::Stuck?MGC_INTERNAL_ERROR:MGC_BUSY;
    }
    MgcStatus acknowledge(uint64_t id,uintptr_t display,uintptr_t context,Pins& transferred){
        if(!transferred.empty())return MGC_INVALID_ARGUMENT;
        {std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
            if(auto status=ownerStatus(*e,display,context))return status;
            if(e->phase!=Phase::Done||anyClaims(*e))return MGC_BUSY;clearEntry(*e,transferred.owner,transferred.code);}
        return MGC_SUCCESS;
    }
    // Used by createReserved before starting work; the copy pins the exact
    // previously reserved JNI owner. Orphan progress must use a Claim instead.
    MgcStatus getOwner(uint64_t id,uintptr_t display,uintptr_t context,std::shared_ptr<void>& owner){
        if(owner)return MGC_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;auto* e=find(id);if(!e)return MGC_INVALID_ARGUMENT;
        if(auto status=ownerStatus(*e,display,context))return status;
        if(e->phase!=Phase::Reserved&&e->phase!=Phase::Creating&&e->phase!=Phase::Adoptable)return MGC_BUSY;
        owner=e->owner;return MGC_SUCCESS;
    }
};
