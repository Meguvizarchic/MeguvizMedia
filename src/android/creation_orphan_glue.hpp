#pragma once
#include "creation_guard.hpp"

// Internal orchestration only: callbacks, JNI registration and actual GL/GPU
// retirement are supplied by the caller. The host must keep a separate loader
// and execution reference across prepare/collect and their return.
class MgmCreationOrphanGlue final {
    using Guards=MgmCreationGuards<>;
    enum class State:uint8_t { Empty,Preparing,Prepared,Live,Finished,Collecting };
    struct Context {
        Guards::Handle guard;
        std::shared_ptr<void> owner;
        MgmCreationOrphans::Pins returned;
        uint64_t receipt=0;
        uint64_t ownerToken=0;
        std::thread::id ownerThread{};
        uintptr_t display=0,context=0;
    };
public:
    using Handle=std::shared_ptr<Context>;
    struct Work {
        MgmCreationOrphans::Claim claim;
        bool finishing=false;
        MgmCreationOrphans::Outcome outcome=MgmCreationOrphans::Outcome::Retryable;
    };
private:
    struct Entry { State state=State::Empty;Handle context; };
    std::mutex mutex_;
    std::array<Entry,8> entries_{};
    Guards* guards_=nullptr;
    MgmCreationOrphans* ledger_=nullptr;
    MgmCreationOrphanGlue()=default;
    ~MgmCreationOrphanGlue()=default; // private: failed initialization only
    Entry* find(const Handle& h){for(auto& e:entries_)if(e.context==h&&h)return &e;return nullptr;}
public:
    static MgmCreationOrphanGlue* create() noexcept {
        try{
            auto* p=new(std::nothrow) MgmCreationOrphanGlue;if(!p)return nullptr;
            p->guards_=Guards::create();
            if(!p->guards_){delete p;return nullptr;}
            p->ledger_=MgmCreationOrphans::create();
            if(!p->ledger_){
                // Not published and no receipt/loader work exists yet. If an
                // invariant is ever violated, retain the object fail-closed.
                if(Guards::discardUnpublished(p->guards_))delete p;
                return nullptr;
            }
            return p;
        }catch(...){return nullptr;}
    }
    // Background, before GL side effects. The pool pins this context even if
    // the recipient disappears. Return Handle to background for eventual drop.
    MgcStatus prepare(const void* address,const char* anchor,Handle& out){
        if(!address||out)return MGC_INVALID_ARGUMENT;Handle h;
        try{h=std::make_shared<Context>();}catch(...){return MGC_OUT_OF_MEMORY;}
        Entry* entry=nullptr;
        {std::lock_guard<std::mutex> lock(mutex_);
            for(auto& e:entries_)if(e.state==State::Empty){entry=&e;break;}
            if(!entry)return MGC_BACKPRESSURE;entry->context=h;entry->state=State::Preparing;
        }
        const auto result=guards_->prepare(address,anchor,h->guard);Handle removed;
        {std::lock_guard<std::mutex> lock(mutex_);
            if(result==MGC_SUCCESS){entry->state=State::Prepared;out=h;}
            else{removed=std::move(entry->context);entry->state=State::Empty;}
        }
        return result;
    }
    // GL owner: the positive receipt is available before begin/driver work.
    // BUSY leaves h prepared and may be retried without repeating side effects.
    MgcStatus reserve(const Handle& h,uintptr_t display,uintptr_t context,
                      const std::shared_ptr<void>& owner,uint64_t* receipt){
        if(receipt)*receipt=0;if(!h||!owner||!receipt)return MGC_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;
        auto* e=find(h);if(!e||e->state!=State::Prepared)return MGC_INVALID_ARGUMENT;
        auto status=guards_->reserve(*ledger_,display,context,owner,h->guard,receipt);
        if(status==MGC_SUCCESS){h->owner=owner;h->receipt=*receipt;h->ownerToken=mgm_thread_token();
            h->ownerThread=std::this_thread::get_id();h->display=display;h->context=context;e->state=State::Live;}
        return status;
    }
    // The caller may know only the receipt after JNI allocation failure.
    // Retrieve the durable handle only on its exact original GL owner/context.
    MgcStatus lookup(uint64_t id,uintptr_t display,uintptr_t context,Handle& out){
        if(!id||out)return MGC_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;
        for(const auto& e:entries_)if(e.state==State::Live&&e.context->receipt==id){
            const auto token=mgm_thread_token();if(!token)return MGC_BUSY;
            if(e.context->ownerThread!=std::this_thread::get_id()||e.context->ownerToken!=token||
               e.context->display!=display||e.context->context!=context)return MGC_INCOMPATIBLE_DOMAIN;
            out=e.context;return MGC_SUCCESS;
        }
        return MGC_INVALID_ARGUMENT;
    }
    MgcStatus begin(uint64_t id,uintptr_t d,uintptr_t c){return ledger_->begin(id,d,c);}
    MgcStatus attach(uint64_t id,uintptr_t d,uintptr_t c,void* platform){return ledger_->attachPlatform(id,d,c,platform);}
    MgcStatus adopt(uint64_t id,uintptr_t d,uintptr_t c){return ledger_->adopt(id,d,c);}
    MgcStatus abort(uint64_t id,uintptr_t d,uintptr_t c){return ledger_->abort(id,d,c);}
    MgcStatus poll(uint64_t id,uintptr_t d,uintptr_t c){return ledger_->poll(id,d,c);}
    MgcStatus claim(uint64_t id,MgmCreationOrphans::Kind kind,uintptr_t d,uintptr_t c,Work& work){
        if(work.finishing||work.claim.active())return MGC_INVALID_ARGUMENT;
        return ledger_->claim(id,kind,d,c,work.claim);
    }
    // Execute the claimed operation once outside locks, then call complete.
    // BUSY preserves both claim and its original outcome for same-thread retry;
    // it is forbidden to invoke the driver operation a second time on retry.
    MgcStatus complete(Work& work,MgmCreationOrphans::Outcome outcome,uintptr_t d=0,uintptr_t c=0){
        if(!work.claim.active()||(outcome!=MgmCreationOrphans::Outcome::Success&&
           outcome!=MgmCreationOrphans::Outcome::Retryable&&outcome!=MgmCreationOrphans::Outcome::Uncertain))return MGC_INVALID_ARGUMENT;
        if(work.finishing&&work.outcome!=outcome)return MGC_INVALID_ARGUMENT;
        work.finishing=true;work.outcome=outcome;
        auto result=ledger_->finish(work.claim,outcome,d,c);
        if(!work.claim.active())work.finishing=false;
        return result;
    }
    // The context holds returned pins until it reaches the background collector.
    MgcStatus commit(const Handle& h,uintptr_t d,uintptr_t c){return transfer(h,d,c,false);}
    MgcStatus acknowledge(const Handle& h,uintptr_t d,uintptr_t c){return transfer(h,d,c,true);}
    MgcStatus transfer(const Handle& h,uintptr_t d,uintptr_t c,bool ack){
        if(!h)return MGC_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;
        auto* e=find(h);if(!e||e->state!=State::Live)return MGC_INVALID_ARGUMENT;
        auto result=ack?ledger_->acknowledge(h->receipt,d,c,h->returned):ledger_->commit(h->receipt,d,c,h->returned);
        if(result==MGC_SUCCESS)e->state=State::Finished;return result;
    }
    // Cancel a prepared ticket that has not started platform creation.
    MgcStatus cancelPrepared(const Handle& h){
        if(!h)return MGC_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return MGC_BUSY;
        auto* e=find(h);if(!e||e->state!=State::Prepared)return MGC_INVALID_ARGUMENT;e->state=State::Finished;return MGC_SUCCESS;
    }
    // Background only, with a host/JVM image reference through return. All
    // caller Handles must have been dropped before the context can retire.
    MgcStatus collect(){
        bool pending=false;
        for(auto& e:entries_){Handle retired;
            {std::lock_guard<std::mutex> lock(mutex_);
                if(e.state==State::Finished&&e.context.use_count()==1){retired=std::move(e.context);e.state=State::Collecting;}
                else if(e.state!=State::Empty)pending=true;
            }
            if(!retired)continue;
            retired.reset(); // owner, returned pins and guard drop on background
            {std::lock_guard<std::mutex> lock(mutex_);e.state=State::Empty;}
        }
        auto status=guards_->collect();if(status!=MGC_SUCCESS)return status;return pending?MGC_BUSY:MGC_SUCCESS;
    }
};
