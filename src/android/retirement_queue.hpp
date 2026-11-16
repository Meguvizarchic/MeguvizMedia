#pragma once
#include <meguviz/media_android.h>
#include <array>
#include <atomic>

// One GL owner publishes bounded references; background service may release
// them on any non-GL thread. A receipt stays reserved until the owner consumes
// DONE. No callback or resource destructor runs under a queue lock.
class MgmRetirementQueue final {
    enum State : unsigned { EMPTY, PREPARING, QUEUED, RUNNING, DONE, STUCK };
    struct Entry {
        std::atomic<unsigned> state{EMPTY};
        MgmFrame* frame=nullptr; MgcLease* lease=nullptr; MgcResource* resource=nullptr; MgcDomain* domain=nullptr;
        bool receipt=false;
    };
    std::array<Entry,96> entries_{};
public:
    int reserve() noexcept {
        for(size_t i=0;i<entries_.size();++i){auto& item=entries_[i];unsigned expected=EMPTY;
            if(!item.state.compare_exchange_strong(expected,PREPARING,std::memory_order_acquire))continue;
            return int(i);}
        return -1;
    }
    void cancel(int index) noexcept {entries_[size_t(index)].state.store(EMPTY,std::memory_order_release);}
    void publish(int index,MgmFrame* frame,MgcLease* lease,MgcResource* resource,bool receipt) noexcept {
        auto& item=entries_[size_t(index)];item.frame=frame;item.lease=lease;item.resource=resource;item.domain=nullptr;item.receipt=receipt;
        item.state.store(QUEUED,std::memory_order_release);
    }
    int enqueue(MgmFrame* frame,MgcLease* lease,MgcResource* resource,bool receipt) noexcept {
        if(!frame&&!lease&&!resource)return -1;
        const int index=reserve();if(index>=0)publish(index,frame,lease,resource,receipt);return index;
    }
    int enqueue_domain(MgcDomain* domain) noexcept {
        if(!domain)return -1;const int index=reserve();if(index<0)return -1;
        auto& item=entries_[size_t(index)];item.frame=nullptr;item.lease=nullptr;item.resource=nullptr;item.domain=domain;item.receipt=true;
        item.state.store(QUEUED,std::memory_order_release);return index;
    }
    bool completed(int index) const noexcept {
        return index>=0&&size_t(index)<entries_.size()&&entries_[size_t(index)].state.load(std::memory_order_acquire)==DONE;
    }
    bool consume(int index) noexcept {
        if(index<0||size_t(index)>=entries_.size())return false;
        auto& item=entries_[size_t(index)];unsigned expected=DONE;
        return item.state.compare_exchange_strong(expected,EMPTY,std::memory_order_acq_rel);
    }
    bool empty() const noexcept {for(const auto& item:entries_)if(item.state.load(std::memory_order_acquire)!=EMPTY)return false;return true;}
    MgcStatus service() noexcept {
        bool waiting=false;
        for(auto& item:entries_){unsigned expected=QUEUED;
            if(item.state.compare_exchange_strong(expected,RUNNING,std::memory_order_acq_rel)){
                try {
                    if(item.frame)mgm_frame_release(item.frame);
                    if(item.lease)mgc_lease_release(item.lease);
                    if(item.resource)mgc_resource_release(item.resource);
                    if(item.domain)mgc_domain_release(item.domain);
                    item.state.store(item.receipt?DONE:EMPTY,std::memory_order_release);
                }catch(...){item.state.store(STUCK,std::memory_order_release);return MGC_INTERNAL_ERROR;}
            }else if(expected!=EMPTY&&expected!=DONE)waiting=true;
        }
        return waiting?MGC_BUSY:MGC_SUCCESS;
    }
};
