#include "../src/android/creation_pool.hpp"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>
#define REQUIRE(x) do { if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::abort();} } while(0)

struct FakeLease {
    static std::atomic<int> alive,fail_reset;
    bool held=false;
    meguviz::detail::CodeImageKind acquire(const void*,const char*) noexcept {
        held=true;++alive;return meguviz::detail::CodeImageKind::DynamicLibrary;
    }
    bool reset() noexcept {
        if(fail_reset.exchange(0))return false;
        if(held){held=false;--alive;}return true;
    }
};
std::atomic<int> FakeLease::alive{0},FakeLease::fail_reset{0};

int main(){
    using Pool=MgmAndroidCreationPool<FakeLease>;using S=Pool::State;
    Pool pool;int owners[8]{};
    REQUIRE(pool.acquire(&owners[0],1,1,1)==nullptr);
    for(int i=0;i<8;++i)REQUIRE(pool.prepare(&pool,"anchor")==MGC_SUCCESS);
    REQUIRE(pool.prepare(&pool,"anchor")==MGC_BACKPRESSURE);
    REQUIRE(FakeLease::alive==8);
    std::array<Pool::Entry*,8> entries{};
    std::atomic<int> cursor{0};std::vector<std::thread> workers;
    for(int n=0;n<8;++n)workers.emplace_back([&]{const int i=cursor.fetch_add(1);entries[i]=pool.acquire(&owners[i],i+1,2,3);});
    for(auto& worker:workers)worker.join();
    for(auto* e:entries)REQUIRE(e&&e->state.load()==S::Active);
    for(size_t i=0;i<entries.size();++i)for(size_t j=i+1;j<entries.size();++j)REQUIRE(entries[i]!=entries[j]);
    REQUIRE(pool.collect()==MGC_SUCCESS); // live sessions do not spin service
    Pool::orphan(*entries[0]);
    REQUIRE(Pool::claim(*entries[0],S::Orphan,S::BackendBusy));
    REQUIRE(!Pool::claim(*entries[0],S::Orphan,S::OwnerBusy));
    Pool::finish(*entries[0],S::Orphan);
    REQUIRE(Pool::claim(*entries[0],S::Orphan,S::OwnerBusy));
    Pool::finish(*entries[0],S::Retired);
    FakeLease::fail_reset=1;REQUIRE(pool.collect()==MGC_INTERNAL_ERROR);
    REQUIRE(entries[0]->state.load()==S::Retry&&FakeLease::alive==8);
    REQUIRE(pool.collect()==MGC_SUCCESS&&FakeLease::alive==7);
    REQUIRE(pool.prepare(&pool,"anchor")==MGC_SUCCESS);
    for(size_t i=1;i<entries.size();++i){REQUIRE(Pool::claim(*entries[i],S::Active,S::OwnerBusy));Pool::finish(*entries[i],S::Retired);}
    REQUIRE(pool.collect()==MGC_SUCCESS&&FakeLease::alive==1); // unused READY remains pinned
    pool.discardReady();REQUIRE(pool.collect()==MGC_SUCCESS&&FakeLease::alive==0);
    // A reservation cannot be stolen by the old READY consumer, and a stale
    // cancellation cannot consume a subsequently issued reservation.
    auto reserve=[&](uint64_t& id){id=0;for(int n=0;n<10000;++n){const auto s=pool.reserve(&id);if(s==MGC_SUCCESS)return;REQUIRE(s==MGC_BUSY);}std::abort();};
    REQUIRE(pool.prepare(&pool,"anchor")==MGC_SUCCESS);uint64_t first=0;reserve(first);REQUIRE(first>0);
    REQUIRE(pool.acquire(&owners[0],1,1,1)==nullptr);
    REQUIRE(pool.acquireReserved(&owners[0],1,1,1,first+1)==nullptr);
    REQUIRE(pool.releaseReservation(first+1)==MGC_SUCCESS);
    auto* held=pool.acquireReserved(&owners[0],1,1,1,first);REQUIRE(held);
    REQUIRE(pool.releaseReservation(first)==MGC_SUCCESS&&held->state.load()==S::Active);
    REQUIRE(Pool::claim(*held,S::Active,S::OwnerBusy));Pool::finish(*held,S::Retired);REQUIRE(pool.collect()==MGC_SUCCESS);
    REQUIRE(pool.prepare(&pool,"anchor")==MGC_SUCCESS);uint64_t second=0;reserve(second);REQUIRE(second>first);
    REQUIRE(pool.releaseReservation(first)==MGC_SUCCESS);
    held=pool.acquireReserved(&owners[0],1,1,1,second);REQUIRE(held);REQUIRE(Pool::claim(*held,S::Active,S::OwnerBusy));Pool::finish(*held,S::Retired);REQUIRE(pool.collect()==MGC_SUCCESS);
    for(int i=0;i<128;++i){
        REQUIRE(pool.prepare(&pool,"anchor")==MGC_SUCCESS);uint64_t id=0;reserve(id);
        std::atomic<bool> go{false};Pool::Entry* acquired=nullptr;MgcStatus released=MGC_INTERNAL_ERROR;
        std::thread a([&]{while(!go.load(std::memory_order_acquire)){}acquired=pool.acquireReserved(&owners[0],1,1,1,id);});
        std::thread b([&]{while(!go.load(std::memory_order_acquire)){}do{released=pool.releaseReservation(id);}while(released==MGC_BUSY);});
        go.store(true,std::memory_order_release);a.join();b.join();REQUIRE(released==MGC_SUCCESS);
        if(acquired){REQUIRE(acquired->state.load()==S::Active);REQUIRE(Pool::claim(*acquired,S::Active,S::OwnerBusy));Pool::finish(*acquired,S::Retired);}
        REQUIRE(pool.collect()==MGC_SUCCESS);
    }
    for(int i=0;i<128;++i){
        REQUIRE(pool.prepare(&pool,"anchor")==MGC_SUCCESS);uint64_t id=0;reserve(id);
        auto* entry=pool.acquireReserved(&owners[0],1,1,1,id);REQUIRE(entry);
        std::atomic<bool> go{false};MgcStatus committed=MGC_INTERNAL_ERROR,canceled=MGC_INTERNAL_ERROR;
        std::thread a([&]{while(!go.load(std::memory_order_acquire)){}committed=Pool::commit(*entry,id);});
        std::thread b([&]{while(!go.load(std::memory_order_acquire)){}canceled=pool.cancel(id);});
        go.store(true,std::memory_order_release);a.join();b.join();
        REQUIRE((committed==MGC_SUCCESS&&canceled==MGC_BUSY)||(committed==MGC_CANCELLED&&canceled==MGC_SUCCESS));
        REQUIRE(Pool::claim(*entry,S::Active,S::OwnerBusy));Pool::finish(*entry,S::Retired);REQUIRE(pool.collect()==MGC_SUCCESS);
    }
    uint64_t last=0;REQUIRE(pool.prepare(&pool,"anchor")==MGC_SUCCESS);reserve(last);REQUIRE(pool.releaseReservation(last)==MGC_SUCCESS);REQUIRE(pool.collect()==MGC_SUCCESS);
    Pool reloaded;REQUIRE(reloaded.prepare(&reloaded,"anchor")==MGC_SUCCESS);uint64_t after=0;
    for(int i=0;i<10000&&!after;++i){auto status=reloaded.reserve(&after);REQUIRE(status==MGC_BUSY||status==MGC_SUCCESS);}
    REQUIRE(after>last);REQUIRE(reloaded.releaseReservation(last)==MGC_SUCCESS);
    auto* reloadedOwner=reloaded.acquireReserved(&owners[0],1,1,1,after);REQUIRE(reloadedOwner);
    REQUIRE(Pool::claim(*reloadedOwner,S::Active,S::OwnerBusy));Pool::finish(*reloadedOwner,S::Retired);REQUIRE(reloaded.collect()==MGC_SUCCESS);
    REQUIRE(FakeLease::alive==0);
    std::puts("PASS bounded preparation, exclusive service, release retry and reservation races");
}
