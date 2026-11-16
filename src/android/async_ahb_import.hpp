#pragma once
#include "thread_identity.hpp"
#include <meguviz/compute_android.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

// Internal bounded worker primitive; not yet a public or complete session API.
// The integrator must keep an independent host loader reference through every
// final call and supply strong domain/code/AHB/content pins. This primitive
// cannot unload its own executing DSO or prove EGL/decoder retirement.
namespace recovery_candidate {
struct ImportOps {
    MgcStatus (*importAhb)(MgcDomain*,const MgcAndroidImportDesc*,MgcLease**)=nullptr;
    MgcStatus (*domainState)(MgcDomain*,MgcDomainStateInfo*)=nullptr;
    MgcStatus (*leaseInfo)(MgcLease*,MgcLeaseInfo*)=nullptr;
    void (*releaseLease)(MgcLease*)=nullptr;
};
using ImportProbe = MgcStatus (*)(MgcDomain*,MgcLease*,void*,bool*);
struct ImportTicket {uint32_t index=UINT32_MAX;uint64_t serial=0;};
struct ImportPins {
    MgcDomain* domain=nullptr;
    std::shared_ptr<void> domainOwner,codeOwner,bufferOwner,contentOwner;
};
enum class ImportPhase { EMPTY, RESERVED, ATTACHED, IMPORTING, IMPORTED, PROBING, READY, TRANSFERRED, FAILED, CHECKING, RELEASING, STUCK, DONE };

class AsyncAhbImport final : public std::enable_shared_from_this<AsyncAhbImport> {
    enum Gate : uint64_t { OPEN=0, CANCELED=1, TRANSFERRED=2 };
    struct Slot {
        std::atomic<uint64_t> gate{0};uint64_t serial=0;ImportPhase phase=ImportPhase::EMPTY;
        ImportPins pins{};MgcAndroidImportDesc desc{};MgcLease* lease=nullptr;
        uint64_t expectedDomain=0,expectedEpoch=0,leaseGeneration=0;
        MgcStatus status=MGC_SUCCESS,probeFailure=MGC_SUCCESS;bool probePending=false,callbackActive=false;
    };
    const std::thread::id glOwner_=std::this_thread::get_id();
    const uint64_t glOwnerToken_=mgm_thread_token();
    const ImportOps ops_;std::mutex mutex_;std::array<Slot,4> slots_{};
    uint64_t next_=0;std::shared_ptr<AsyncAhbImport> selfPin_;
    bool owner()const{return glOwnerToken_ && mgm_thread_token()==glOwnerToken_ && std::this_thread::get_id()==glOwner_;}
    static uint64_t bits(uint64_t serial,Gate gate){return (serial<<2)|uint64_t(gate);}
    Slot* find(ImportTicket t){return t.index<slots_.size()&&t.serial&&slots_[t.index].serial==t.serial?&slots_[t.index]:nullptr;}
    MgcStatus finishProbeLocked(Slot& s,ImportTicket t,MgcStatus probe,bool physicallyRetired){
        if(probe!=MGC_SUCCESS&&probe!=MGC_BUSY&&probe!=MGC_TIMEOUT&&
           (s.probeFailure==MGC_SUCCESS||probe==MGC_DEVICE_LOST))s.probeFailure=probe;
        if(!physicallyRetired||probe==MGC_BUSY||probe==MGC_TIMEOUT){s.probePending=true;s.phase=ImportPhase::PROBING;return MGC_BUSY;}
        s.probePending=false;
        if(s.probeFailure!=MGC_SUCCESS||probe!=MGC_SUCCESS||s.gate.load(std::memory_order_acquire)!=bits(t.serial,OPEN)){
            s.status=s.probeFailure!=MGC_SUCCESS?s.probeFailure:MGC_CANCELLED;s.phase=ImportPhase::FAILED;return s.status;}
        s.phase=ImportPhase::READY;return MGC_SUCCESS;
    }
public:
    explicit AsyncAhbImport(ImportOps ops):ops_(ops){}
    // The domain, code and caller's model/owner refs are already strong; a
    // rejected reservation consumes none. No Vulkan/GL callback runs here.
    MgcStatus reserve(const ImportPins& pins,uint64_t domainId,uint64_t epoch,ImportTicket* out){
        if(!out)return MGC_INVALID_ARGUMENT;*out={};
        if(!owner()||!pins.domain||!pins.domainOwner||!pins.codeOwner||!domainId||!epoch||
           !ops_.importAhb||!ops_.domainState||!ops_.leaseInfo||!ops_.releaseLease)return MGC_INVALID_ARGUMENT;
        std::shared_ptr<AsyncAhbImport> self;try{self=shared_from_this();}catch(...){return MGC_INVALID_ARGUMENT;}
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock)return MGC_BUSY;
        if(next_>=std::numeric_limits<uint64_t>::max()/4)return MGC_BACKPRESSURE;
        for(uint32_t i=0;i<slots_.size();++i){auto& s=slots_[i];if(s.phase!=ImportPhase::EMPTY)continue;
            s.serial=++next_;s.expectedDomain=domainId;s.expectedEpoch=epoch;s.pins=pins;s.status=MGC_SUCCESS;
            s.probeFailure=MGC_SUCCESS;s.probePending=false;s.callbackActive=false;s.lease=nullptr;s.desc={};s.phase=ImportPhase::RESERVED;
            s.gate.store(bits(s.serial,OPEN),std::memory_order_release);selfPin_=self;*out={i,s.serial};return MGC_ACCEPTED;}
        return MGC_BACKPRESSURE;
    }
    // Caller retains its original buffer/content references on every return.
    // Native sync-FD transfer needs its own owned-dup protocol and is rejected
    // here until implemented; -1 must mean real readiness from the producer.
    MgcStatus attach(ImportTicket t,const MgcAndroidImportDesc& d,const std::shared_ptr<void>& bufferOwner,
                     const std::shared_ptr<void>& contentOwner){
        if(!owner()||!d.buffer||!bufferOwner||d.acquire_fence_fd!=-1||!d.content_id||
           ((d.retain_content||d.release_content)&&!contentOwner))return MGC_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock)return MGC_BUSY;
        auto* s=find(t);if(!s||s->phase!=ImportPhase::RESERVED)return MGC_INVALID_ARGUMENT;
        if(s->gate.load(std::memory_order_acquire)!=bits(t.serial,OPEN))return MGC_CANCELLED;
        s->pins.bufferOwner=bufferOwner;s->pins.contentOwner=contentOwner;s->desc=d;s->phase=ImportPhase::ATTACHED;return MGC_SUCCESS;
    }
    // Lock-free linearization against transfer, including ABA-resistant serial.
    // If transfer already won, its new owner must run the normal retirement.
    MgcStatus cancel(ImportTicket t){
        if(t.index>=slots_.size()||!t.serial||t.serial>std::numeric_limits<uint64_t>::max()/4)return MGC_INVALID_ARGUMENT;
        auto& gate=slots_[t.index].gate;uint64_t expected=bits(t.serial,OPEN);
        if(gate.compare_exchange_strong(expected,bits(t.serial,CANCELED),std::memory_order_acq_rel))return MGC_SUCCESS;
        return expected==bits(t.serial,CANCELED)?MGC_SUCCESS:MGC_BUSY;
    }
    // A worker must retain the registry itself through arbitrary driver waits.
    MgcStatus runImport(ImportTicket t){
        if(owner())return MGC_INCOMPATIBLE_DOMAIN;
        auto self=shared_from_this();MgcDomain* domain=nullptr;MgcAndroidImportDesc desc{};
        {std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);if(!s||s->phase!=ImportPhase::ATTACHED)return MGC_INVALID_ARGUMENT;
            if(s->gate.load(std::memory_order_acquire)!=bits(t.serial,OPEN)){s->phase=ImportPhase::FAILED;s->status=MGC_CANCELLED;return MGC_CANCELLED;}
            s->phase=ImportPhase::IMPORTING;domain=s->pins.domain;desc=s->desc;}
        MgcLease* lease=nullptr;MgcStatus status=MGC_INTERNAL_ERROR;
        try{status=ops_.importAhb(domain,&desc,&lease);}catch(...){status=MGC_INTERNAL_ERROR;}
        MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);
        MgcLeaseInfo info{};info.header=MGC_HEADER_INIT(MgcLeaseInfo);
        MgcStatus domainStatus=MGC_INTERNAL_ERROR,infoStatus=MGC_INVALID_ARGUMENT;
        try{domainStatus=ops_.domainState(domain,&state);if(lease)infoStatus=ops_.leaseInfo(lease,&info);}catch(...){domainStatus=MGC_INTERNAL_ERROR;}
        {std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);
            if(!s||s->phase!=ImportPhase::IMPORTING)std::terminate(); // slot cannot recycle while worker runs
            s->lease=lease;
            if(!domainStatus&&state.lost)status=MGC_DEVICE_LOST;
            else if(status==MGC_SUCCESS&&!lease)status=MGC_INTERNAL_ERROR;
            else if(status==MGC_SUCCESS&&domainStatus)status=domainStatus;
            else if(status==MGC_SUCCESS&&infoStatus)status=infoStatus;
            else if(status==MGC_SUCCESS&&(state.domain_id!=s->expectedDomain||state.device_epoch!=s->expectedEpoch||
               info.domain_id!=state.domain_id||info.domain_generation!=state.domain_generation))status=MGC_INCOMPATIBLE_DOMAIN;
            if(status!=MGC_DEVICE_LOST&&s->gate.load(std::memory_order_acquire)!=bits(t.serial,OPEN))status=MGC_CANCELLED;
            s->status=status;s->leaseGeneration=info.domain_generation;s->phase=status==MGC_SUCCESS?ImportPhase::IMPORTED:ImportPhase::FAILED;}
        return status;
    }
    // Borrow the held lease exclusively on a worker without publishing it.
    // A callback must report true only after real physical retirement. If it
    // returns or throws without such proof, the slot stays pinned PROBING.
    MgcStatus runProbe(ImportTicket t,ImportProbe callback,void* opaque){
        if(owner())return MGC_INCOMPATIBLE_DOMAIN;if(!callback)return MGC_INVALID_ARGUMENT;
        auto self=shared_from_this();MgcDomain* domain=nullptr;MgcLease* lease=nullptr;
        {std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);
            if(!s||s->phase!=ImportPhase::IMPORTED)return MGC_INVALID_ARGUMENT;
            if(s->gate.load(std::memory_order_acquire)!=bits(t.serial,OPEN)){s->phase=ImportPhase::FAILED;s->status=MGC_CANCELLED;return MGC_CANCELLED;}
            s->phase=ImportPhase::PROBING;s->probePending=true;s->callbackActive=true;domain=s->pins.domain;lease=s->lease;}
        bool retired=false;MgcStatus status=MGC_INTERNAL_ERROR;
        try{status=callback(domain,lease,opaque,&retired);}catch(...){status=MGC_INTERNAL_ERROR;retired=false;}
        std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);
        if(!s||s->phase!=ImportPhase::PROBING||!s->callbackActive)std::terminate();
        s->callbackActive=false;return finishProbeLocked(*s,t,status,retired);
    }
    // Receipt must come from actual background probe/retirement, not a timeout,
    // mere submission, or Android/GL fence. BUSY/TIMEOUT are poll results only;
    // a logical probe failure must be passed as that failure and stays failed.
    MgcStatus finishProbe(ImportTicket t,MgcStatus probe,bool physicallyRetired){
        if(owner())return MGC_INCOMPATIBLE_DOMAIN;
        std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);
        if(!s||(s->phase!=ImportPhase::IMPORTED&&s->phase!=ImportPhase::PROBING))return MGC_INVALID_ARGUMENT;
        if(s->callbackActive)return MGC_BUSY;
        return finishProbeLocked(*s,t,probe,physicallyRetired);
    }
    // Atomically claims the lease vs cancel. The recipient must transfer it to
    // a session whose GL and Vulkan retirement obligations are independently
    // retained; this is not a decoder-generation or screen-present proof.
    MgcStatus take(ImportTicket t,MgcLease** out){
        if(!owner()||!out)return MGC_INVALID_ARGUMENT;*out=nullptr;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock)return MGC_BUSY;auto* s=find(t);
        if(!s||s->phase!=ImportPhase::READY)return MGC_INVALID_ARGUMENT;
        // This is restricted to the actual nonblocking, callback-free
        // mgc_domain_state implementation; a general user callback is invalid.
        MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);MgcStatus queried=MGC_INTERNAL_ERROR;
        try{queried=ops_.domainState(s->pins.domain,&state);}catch(...){return MGC_INTERNAL_ERROR;}
        if(queried)return queried;if(state.lost)return MGC_DEVICE_LOST;
        if(state.domain_id!=s->expectedDomain||state.device_epoch!=s->expectedEpoch)return MGC_INCOMPATIBLE_DOMAIN;
        uint64_t expected=bits(t.serial,OPEN);
        if(!s->gate.compare_exchange_strong(expected,bits(t.serial,TRANSFERRED),std::memory_order_acq_rel))return MGC_CANCELLED;
        *out=s->lease;s->lease=nullptr;s->phase=ImportPhase::TRANSFERRED;return MGC_SUCCESS;
    }
    // The transferred recipient must have really released its lease/session;
    // until then this slot retains domain, buffer, content and code pins.
    MgcStatus sessionRetired(ImportTicket t,bool proof){
        if(owner())return MGC_INCOMPATIBLE_DOMAIN;std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);
        if(!s||s->phase!=ImportPhase::TRANSFERRED)return MGC_INVALID_ARGUMENT;
        if(!proof)return MGC_BUSY;s->phase=ImportPhase::FAILED;return MGC_SUCCESS;
    }
    // Background cleanup only. A pending import/probe cannot be retired here.
    // If releaseLease blocks, the slot and this object remain pinned RUNNING.
    MgcStatus retire(ImportTicket t){
        if(owner())return MGC_INCOMPATIBLE_DOMAIN;auto self=shared_from_this();MgcLease* lease=nullptr;ImportPins pins;
        MgcDomain* domain=nullptr;ImportPhase previous=ImportPhase::EMPTY;
        {std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);if(!s)return MGC_INVALID_ARGUMENT;
            if(s->phase==ImportPhase::IMPORTING||s->phase==ImportPhase::PROBING||s->phase==ImportPhase::TRANSFERRED||s->probePending)return MGC_BUSY;
            if(s->phase==ImportPhase::CHECKING||s->phase==ImportPhase::RELEASING)return MGC_BUSY;
            if(s->phase==ImportPhase::STUCK)return MGC_INTERNAL_ERROR;if(s->phase==ImportPhase::DONE)return MGC_SUCCESS;
            if(s->phase!=ImportPhase::FAILED&&s->gate.load(std::memory_order_acquire)==bits(t.serial,OPEN))return MGC_BUSY;
            previous=s->phase;s->phase=ImportPhase::CHECKING;domain=s->pins.domain;}
        MgcDomainStateInfo state{};state.header=MGC_HEADER_INIT(MgcDomainStateInfo);MgcStatus observed=MGC_INTERNAL_ERROR;
        try{observed=ops_.domainState(domain,&state);}catch(...){observed=MGC_INTERNAL_ERROR;}
        {std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);if(!s||s->phase!=ImportPhase::CHECKING)std::terminate();
            if(observed!=MGC_SUCCESS||(state.lost&&!state.device_retired)){s->phase=previous;return observed?observed:MGC_BUSY;}
            s->phase=ImportPhase::RELEASING;lease=s->lease;s->lease=nullptr;pins=std::move(s->pins);}
        try{if(lease)ops_.releaseLease(lease);}catch(...){
            std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);if(!s||s->phase!=ImportPhase::RELEASING)std::terminate();
            s->lease=lease;s->pins=std::move(pins);s->phase=ImportPhase::STUCK;return MGC_INTERNAL_ERROR;
        }
        pins.contentOwner.reset();pins.bufferOwner.reset();pins.domainOwner.reset();pins.codeOwner.reset();
        {std::lock_guard<std::mutex> lock(mutex_);auto* s=find(t);if(!s||s->phase!=ImportPhase::RELEASING)std::terminate();s->phase=ImportPhase::DONE;}
        return MGC_SUCCESS;
    }
    // Consume once on GL owner. No resource destructor runs under this lock.
    MgcStatus consume(ImportTicket t){
        if(!owner())return MGC_INCOMPATIBLE_DOMAIN;auto self=shared_from_this();std::shared_ptr<AsyncAhbImport> pin;
        {std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock)return MGC_BUSY;auto* s=find(t);
            if(!s||s->phase==ImportPhase::EMPTY)return MGC_INVALID_ARGUMENT;if(s->phase!=ImportPhase::DONE)return MGC_BUSY;
            s->phase=ImportPhase::EMPTY;s->gate.store(bits(t.serial,CANCELED),std::memory_order_release);
            bool empty=true;for(const auto& item:slots_)if(item.phase!=ImportPhase::EMPTY)empty=false;
            if(empty)pin=std::move(selfPin_);}
        return MGC_SUCCESS;
    }
};
}
