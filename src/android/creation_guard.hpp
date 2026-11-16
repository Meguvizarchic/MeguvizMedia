#pragma once

#include "creation_orphans.hpp"
#include <meguviz/detail/code_image_lease.hpp>

// Background preparation and reclamation for the code pin in the orphan
// ledger. The caller must hold a separate host/JVM loader and execution
// reference through prepare(), collect(), and their return. These functions
// cannot safely release the last reference to their own executing image.
// Never exchange handles across concurrently loaded Media images.
template<class Lease = meguviz::detail::CodeImageLease>
class MgmCreationGuards final {
    friend class MgmCreationOrphanGlue;
    enum class State : uint8_t { Empty, Acquiring, Ready, Releasing, Retry };
    struct Guard { Lease lease; };
public:
    using Handle = std::shared_ptr<Guard>;
private:
    struct Entry { State state=State::Empty; Handle guard; };
    std::mutex mutex_;
    std::array<Entry,8> entries_{};
    MgmCreationGuards()=default;
    ~MgmCreationGuards()=default;
    // Only invoked during unpublished glue initialization rollback.
    static bool discardUnpublished(MgmCreationGuards* instance) noexcept {
        if(!instance)return true;
        for(const auto& entry:instance->entries_)
            if(entry.state!=State::Empty||entry.guard)return false;
        delete instance;return true;
    }
public:
    static MgmCreationGuards* create() noexcept {
        try{return new(std::nothrow) MgmCreationGuards;}catch(...){return nullptr;}
    }
    // Called on a background thread, before any potentially blocking loader
    // work or GL/Vulkan side effect. Failed cleanup occupies its slot until a
    // later collect() succeeds. `out` must be returned to background for drop.
    MgcStatus prepare(const void* address,const char* exportedAnchor,Handle& out) {
        if(!address||out)return MGC_INVALID_ARGUMENT;
        Handle guard;
        try{guard=std::make_shared<Guard>();}catch(...){return MGC_OUT_OF_MEMORY;}
        Entry* entry=nullptr;
        {std::lock_guard<std::mutex> lock(mutex_);
            for(auto& candidate:entries_)if(candidate.state==State::Empty){entry=&candidate;break;}
            if(!entry)return MGC_BACKPRESSURE;
            entry->guard=guard;entry->state=State::Acquiring;
        }
        const auto kind=guard->lease.acquire(address,exportedAnchor);
        if(kind!=meguviz::detail::CodeImageKind::DynamicLibrary &&
           kind!=meguviz::detail::CodeImageKind::MainExecutable){
            const bool released=guard->lease.reset();
            Handle removed;
            {std::lock_guard<std::mutex> lock(mutex_);
                if(released){removed=std::move(entry->guard);entry->state=State::Empty;}
                else entry->state=State::Retry;
            }
            return MGC_UNSUPPORTED_CAPABILITY;
        }
        {std::lock_guard<std::mutex> lock(mutex_);entry->state=State::Ready;out=guard;}
        return MGC_SUCCESS;
    }
    // GL does not acquire or release a loader reference. The pool retains its
    // copy even on failure or BUSY; it also makes a caller's mistaken last
    // shared_ptr drop safe until background collect().
    MgcStatus reserve(MgmCreationOrphans& orphans,uintptr_t display,uintptr_t context,
                      const std::shared_ptr<void>& owner,const Handle& guard,uint64_t* receipt) {
        if(!guard)return MGC_INVALID_ARGUMENT;
        {std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
            if(!lock.owns_lock())return MGC_BUSY;
            bool found=false;for(const auto& entry:entries_)
                if(entry.state==State::Ready&&entry.guard==guard){found=true;break;}
            if(!found)return MGC_INVALID_ARGUMENT;
        }
        return orphans.reserve(display,context,owner,guard,receipt);
    }
    // Background only. A reference returned from commit/acknowledge and every
    // client Handle must be dropped before collection; retry failures remain
    // bounded and pinned. No mutex is held during loader operations.
    MgcStatus collect() {
        bool pending=false,failed=false;
        for(auto& entry:entries_){
            Guard* guard=nullptr;
            {std::lock_guard<std::mutex> lock(mutex_);
                if((entry.state==State::Ready||entry.state==State::Retry)&&entry.guard.use_count()==1){
                    entry.state=State::Releasing;guard=entry.guard.get();
                }else if(entry.state!=State::Empty)pending=true;
            }
            if(!guard)continue;
            const bool released=guard->lease.reset();Handle removed;
            {std::lock_guard<std::mutex> lock(mutex_);
                if(released){removed=std::move(entry.guard);entry.state=State::Empty;}
                else{entry.state=State::Retry;failed=true;}
            }
        }
        return failed?MGC_INTERNAL_ERROR:pending?MGC_BUSY:MGC_SUCCESS;
    }
};
