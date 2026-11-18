#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace qcom::ahb {
inline constexpr unsigned width = 480;
inline constexpr unsigned height = 272;
inline constexpr unsigned packedWidth = width / 4;

enum class Retirement { Complete, NoSubmit, NotReady, Timeout, DeviceLost };

class Generation {
public:
    bool begin(uint64_t id)
    {
        if (!reusable() || id == 0 || id <= lastId)
            return false;
        id_ = lastId = id;
        return true;
    }

    bool publish(uint64_t id)
    {
        if (id != id_ || id_ == 0 || published)
            return false;
        published = true;
        return true;
    }

    bool pin(uint64_t id, uint64_t job)
    {
        if (!owns(id) || !published || closed || job == 0 || job_ != 0)
            return false;
        job_ = job;
        return true;
    }

    bool retire(uint64_t id, uint64_t job, Retirement proof)
    {
        if (!owns(id) || job_ == 0 || job != job_)
            return false;
        if (proof != Retirement::Complete && proof != Retirement::NoSubmit)
            return false;
        job_ = 0;
        resetIfReleased();
        return true;
    }

    bool closeProducer(uint64_t id)
    {
        if (!owns(id))
            return false;
        closed = true;
        resetIfReleased();
        return true;
    }

    bool observeGlComplete(uint64_t id)
    {
        if (!owns(id))
            return false;
        glComplete = true;
        resetIfReleased();
        return true;
    }

    bool reusable() const { return id_ == 0; }

private:
    bool owns(uint64_t id) const { return id != 0 && id == id_; }

    void resetIfReleased()
    {
        if (!closed || !glComplete || job_ != 0) return;
        id_ = 0;
        published = closed = glComplete = false;
    }

    uint64_t id_ = 0;
    uint64_t lastId = 0;
    uint64_t job_ = 0;
    bool published = false;
    bool closed = false;
    bool glComplete = false;
};

template<class Operations>
class FenceFd {
public:
    FenceFd() = default;
    explicit FenceFd(int value) : value_(value) {}
    FenceFd(const FenceFd &) = delete;
    FenceFd &operator=(const FenceFd &) = delete;

    FenceFd(FenceFd &&other) noexcept : value_(other.release()) {}
    FenceFd &operator=(FenceFd &&other) noexcept
    {
        if (this != &other) {
            reset();
            value_ = other.release();
        }
        return *this;
    }

    ~FenceFd() { reset(); }

    int get() const { return value_; }

    // VK acquires the duplicated descriptor only if import succeeded.
    template<class Import>
    bool transferToVulkan(Import import)
    {
        if (value_ < 0 || !import(value_)) return false;
        release();
        return true;
    }

private:
    int release()
    {
        const int old = value_;
        value_ = -1;
        return old;
    }
    void reset()
    {
        if (value_ >= 0) Operations::close(release());
    }
    int value_ = -1;
};

inline uint8_t gray(unsigned x, unsigned y, unsigned frame)
{
    return static_cast<uint8_t>(x * 13u + y * 17u + (x ^ (3u * y)) + frame * 37u);
}

struct Luma {
    std::vector<uint8_t> fine;
    std::vector<uint8_t> coarse;
};

// CPU byte oracle only. The AHB path reads the compact texture on the GPU.
inline Luma unpack(const uint8_t *bytes, size_t length, size_t rowStride, bool topDown)
{
    if (!bytes || rowStride < width || length < width || (length - width) / (height - 1) < rowStride)
        throw std::invalid_argument("invalid packed luma stride or byte length");
    Luma out;
    out.fine.resize(width * height);
    out.coarse.resize(width * height / 4);
    for (unsigned y = 0; y < height; ++y) {
        const unsigned row = topDown ? height - 1 - y : y;
        for (unsigned x = 0; x < width; ++x)
            out.fine[y * width + x] = bytes[static_cast<size_t>(row) * rowStride + x];
    }
    for (unsigned y = 0; y < height / 2; ++y)
        for (unsigned x = 0; x < width / 2; ++x) {
            const unsigned top = 2 * y * width + 2 * x;
            const unsigned sum = unsigned(out.fine[top]) + out.fine[top + 1] +
                                 out.fine[top + width] + out.fine[top + width + 1];
            out.coarse[y * (width / 2) + x] = static_cast<uint8_t>((sum + 2) / 4);
        }
    return out;
}
}

