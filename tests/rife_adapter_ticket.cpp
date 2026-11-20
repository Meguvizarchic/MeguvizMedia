#include "frame_private.h"
#include <meguviz/media_rife.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::cerr << "Line " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (false)

struct MgrPlan {};
struct MgrSession {};
struct MgcTicket { std::atomic<unsigned> refs{0}; };
struct MgcLease { std::atomic<unsigned> refs{1}; };

namespace {
constexpr uint64_t reused_id = 71;
struct Task {
    MgrRequest request{};
    MgrCompletionFn completion = nullptr;
    void *context = nullptr;
    MgcContextFn release = nullptr;
};
MgrSession session;
std::array<Task, 2> tasks;
std::array<MgcTicket, 2> tickets;
std::atomic<unsigned> accepted{0}, callbacks{0};
std::atomic<MgcTicket *> cancelled{nullptr};
std::mutex gate;
std::condition_variable changed;
bool first_retired = false, may_return = false;
bool close_probe = false;
unsigned premature_close_calls = 0;

void complete(unsigned slot) {
    const auto &task = tasks[slot];
    task.completion(task.context, reused_id, MGC_CANCELLED, nullptr);
    task.release(task.context);
    mgc_lease_release(task.request.left);
    mgc_lease_release(task.request.right);
}
}

MgcStatus mgr_session_create(MgrPlan *, const MgcSessionDesc *, MgrSession **out) {
    *out = &session;
    return MGC_SUCCESS;
}
MgcStatus mgr_session_close(MgrSession *) {
    if (!close_probe) return MGC_SUCCESS;
    ++premature_close_calls;
    // Leave the effect available for the rest of the negative control.
    return MGC_BUSY;
}
MgcStatus mgr_submit(MgrSession *, const MgrRequest *request, MgrCompletionFn cb,
                     void *context, MgcContextFn retain, MgcContextFn release, MgcTicket **out) {
    const auto slot = accepted.fetch_add(1);
    CHECK(slot < tasks.size());
    CHECK(request->request_id == reused_id);
    tasks[slot] = {*request, cb, context, release};
    retain(context);
    tickets[slot].refs = 1;
    if (slot == 0) {
        complete(0);
        std::unique_lock<std::mutex> lock(gate);
        first_retired = true;
        changed.notify_one();
        CHECK(changed.wait_for(lock, std::chrono::seconds(5), [] { return may_return; }));
    }
    *out = &tickets[slot];
    return MGC_ACCEPTED;
}
MgcStatus mgr_cancel(MgrSession *, MgcTicket *ticket) {
    CHECK(ticket && ticket->refs > 0);
    cancelled = ticket;
    return MGC_SUCCESS;
}
MgcStatus mgr_drain(MgrSession *, uint64_t) { return MGC_SUCCESS; }
MgcStatus mgr_result_describe(MgrResult *, MgrResultInfo *) { return MGC_INTERNAL_ERROR; }
MgcStatus mgr_result_observation(MgrResult *, MgrJobObservation *) { return MGC_INTERNAL_ERROR; }
void mgr_result_release(MgrResult *) { CHECK(false); }

void mgc_ticket_retain(MgcTicket *ticket) { ++ticket->refs; }
void mgc_ticket_release(MgcTicket *ticket) { CHECK(ticket->refs.fetch_sub(1) > 0); }
void mgc_lease_retain(MgcLease *lease) { ++lease->refs; }
void mgc_lease_release(MgcLease *lease) { CHECK(lease->refs.fetch_sub(1) > 1); }
MgcStatus mgc_lease_info(MgcLease *, MgcLeaseInfo *info) {
    info->format = MGC_FORMAT_RGB8;
    info->width = info->height = 32;
    return MGC_SUCCESS;
}
MgcStatus mgm_frame_create(const MgmFrameDesc *, MgmFrame **) { return MGC_INTERNAL_ERROR; }
void mgm_frame_retain(MgmFrame *frame) { ++frame->refs; }
void mgm_frame_release(MgmFrame *frame) { CHECK(frame->refs.fetch_sub(1) > 1); }
MgcStatus mgm_frame_describe(MgmFrame *frame, MgmFrameDesc *desc) {
    CHECK(desc->header.struct_size == sizeof(*desc));
    *desc = frame->desc;
    return MGC_SUCCESS;
}

int main() {
    MgrPlan plan;
    MgmEffect *effect = nullptr;
    MgmEffectApi api{};
    api.header = MGC_HEADER_INIT(MgmEffectApi);
    CHECK(mgm_rife_effect_create(&plan, &effect, &api) == MGC_SUCCESS);
    std::array<MgcLease, 2> content;
    std::array<MgmFrame, 2> frames;
    for (size_t i = 0; i < frames.size(); ++i) {
        auto &desc = frames[i].desc;
        desc.header = MGC_HEADER_INIT(MgmFrameDesc);
        desc.content = &content[i];
        desc.epoch = 1;
        desc.timebase = {1, 1000};
        desc.color.header = MGC_HEADER_INIT(MgmColor);
        desc.color.transfer = MGM_TRANSFER_SDR;
        desc.color.bit_depth = 8;
        desc.color.range = MGM_RANGE_FULL;
    }
    MgmEffectRequest request{};
    request.header = MGC_HEADER_INIT(MgmEffectRequest);
    request.epoch = 1;
    request.request_id = reused_id;
    request.left = &frames[0];
    request.right = &frames[1];
    auto submit = [&] {
        mgm_frame_retain(request.left);
        mgm_frame_retain(request.right);
        CHECK(api.submit(effect, &request, [](void *, uint64_t id, MgcStatus status, MgmFrame *out) {
            CHECK(id == reused_id && status == MGC_CANCELLED && !out);
            ++callbacks;
        }, nullptr, nullptr, nullptr) == MGC_ACCEPTED);
    };
    std::thread first(submit);
    {
        std::unique_lock<std::mutex> lock(gate);
        CHECK(changed.wait_for(lock, std::chrono::seconds(5), [] { return first_retired; }));
    }
    close_probe = true;
    CHECK(mgm_rife_effect_release(effect) == MGC_BUSY);
    close_probe = false;
    submit();
    {
        std::lock_guard<std::mutex> lock(gate);
        may_return = true;
    }
    changed.notify_one();
    first.join();
    CHECK(api.cancel(effect, reused_id) == MGC_SUCCESS);
    const bool target_is_current = cancelled.load() == &tickets[1];
    complete(1);
    CHECK(callbacks == 2 && mgm_rife_effect_release(effect) == MGC_SUCCESS);
    for (size_t i = 0; i < frames.size(); ++i) CHECK(frames[i].refs == 1 && content[i].refs == 1);
    const auto old_refs = tickets[0].refs.load(), current_refs = tickets[1].refs.load();
    std::cout << "{\"cancelled_current\":" << (target_is_current ? "true" : "false")
              << ",\"premature_close_calls\":" << premature_close_calls
              << ",\"old_ticket_refs\":" << old_refs << ",\"current_ticket_refs\":" << current_refs << "}\n";
    return target_is_current && premature_close_calls == 0 && old_refs == 0 && current_refs == 0 ? 0 : 2;
}
