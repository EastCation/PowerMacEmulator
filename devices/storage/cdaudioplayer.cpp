/*
 * PowerMac Emulator - CD-DA audio playback (cubeb).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <devices/storage/cdaudioplayer.h>

#include <utils/imgfile.h>

#include <cubeb/cubeb.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#endif
#include <loguru.hpp>


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr size_t SAMPLES_PER_SECTOR = 1176; // 588 stereo frames
constexpr size_t RING_SAMPLES       = 1u << 18;
}

struct CdAudioPlayer::Impl {
    bool com_initialized = false;
    cubeb*        ctx    = nullptr;
    cubeb_stream* stream = nullptr;

    std::atomic<bool> active{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> ended{false};
    std::atomic<bool> worker_stop{false};

    std::thread worker;
    std::mutex  mtx;
    std::vector<int16_t> ring;
    size_t cap = 0, head = 0, count = 0;

    std::string file;
    uint64_t start_off = 0, end_off = 0, read_off = 0;
    uint32_t start_lba = 0;

    using Clock = std::chrono::steady_clock;
    Clock::time_point start_time;
    Clock::time_point pause_time;
    std::chrono::nanoseconds paused_ns{0};
    uint32_t end_lba = 0;

    static long data_cb(cubeb_stream*, void* user, const void*, void* output,
                        long nframes);
    static void state_cb(cubeb_stream*, void*, cubeb_state) {}

    void reader();
};

long CdAudioPlayer::Impl::data_cb(cubeb_stream*, void* user, const void*,
                                  void* output, long nframes)
{
    Impl* p = static_cast<Impl*>(user);
    int16_t* out = static_cast<int16_t*>(output);
    size_t need = size_t(nframes) * 2;

    if (!p->active.load() || p->paused.load()) {
        std::memset(out, 0, need * sizeof(int16_t));
        return nframes;
    }

    size_t copied = 0;
    {
        std::lock_guard<std::mutex> lock(p->mtx);
        copied = std::min(need, p->count);
        for (size_t i = 0; i < copied; i++)
            out[i] = p->ring[(p->head + i) % p->cap];
        p->head = (p->head + copied) % p->cap;
        p->count -= copied;

    }

    if (copied < need)
        std::memset(out + copied, 0, (need - copied) * sizeof(int16_t));

    return nframes;
}

void CdAudioPlayer::Impl::reader()
{
    ImgFile file_reader;
    if (!file_reader.open(file)) {
        LOG_F(ERROR, "CD-DA: could not open %s", file.c_str());
        ended = true;
        active = false;
        return;
    }

    std::vector<uint8_t> sector(2352);
    int16_t samples[SAMPLES_PER_SECTOR];

    while (!worker_stop.load()) {
        if (paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (!stream) {
            // No audio device: the wall-clock position still advances.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        size_t space;
        {
            std::lock_guard<std::mutex> lock(mtx);
            space = cap - count;
        }
        if (space < SAMPLES_PER_SECTOR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        if (read_off + 2352 > end_off) {
            break;
        }

        if (file_reader.read(sector.data(), read_off, 2352) != 2352) {
            break;
        }

        if (stream) {
            for (size_t i = 0; i < SAMPLES_PER_SECTOR; i++) {
                samples[i] = int16_t(uint16_t(sector[i * 2]) |
                                     (uint16_t(sector[i * 2 + 1]) << 8));
            }

            std::lock_guard<std::mutex> lock(mtx);
            if (cap - count >= SAMPLES_PER_SECTOR) {
                for (size_t i = 0; i < SAMPLES_PER_SECTOR; i++)
                    ring[(head + count + i) % cap] = samples[i];
                count += SAMPLES_PER_SECTOR;
            }
        }

        read_off += 2352;
    }
}

CdAudioPlayer::CdAudioPlayer() : impl(std::make_unique<Impl>())
{
    impl->cap = RING_SAMPLES;
    impl->ring.assign(impl->cap, 0);

#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE)
        impl->com_initialized = true;
#endif

    int init_res = cubeb_init(&impl->ctx, "PowerMac CD Audio", nullptr);
    LOG_F(INFO, "CD-DA: cubeb_init result %d", init_res);
    if (init_res != CUBEB_OK) {
        LOG_F(WARNING, "CD-DA: could not initialize cubeb; audio will be silent");
        impl->ctx = nullptr;
        return;
    }

    cubeb_stream_params params = {
        .format   = CUBEB_SAMPLE_S16NE,
        .rate     = 44100,
        .channels = 2,
        .layout   = CUBEB_LAYOUT_STEREO,
        .prefs    = CUBEB_STREAM_PREF_NONE
    };

    int res = cubeb_stream_init(impl->ctx, &impl->stream, "CD-DA",
                                nullptr, nullptr, nullptr, &params, 256,
                                Impl::data_cb, Impl::state_cb, impl.get());
    LOG_F(INFO, "CD-DA: cubeb_stream_init result %d", res);
    if (res != CUBEB_OK) {
        LOG_F(WARNING, "CD-DA: could not open audio stream (%d); audio will be silent", res);
        impl->stream = nullptr;
        return;
    }

    int start_res = cubeb_stream_start(impl->stream);
    LOG_F(INFO, "CD-DA: cubeb_stream_start result %d", start_res);
    if (start_res != CUBEB_OK) {
        cubeb_stream_destroy(impl->stream);
        impl->stream = nullptr;
    }
}

CdAudioPlayer::~CdAudioPlayer()
{
    stop();
    if (impl->stream) {
        cubeb_stream_stop(impl->stream);
        cubeb_stream_destroy(impl->stream);
        impl->stream = nullptr;
    }
    if (impl->ctx) {
        cubeb_destroy(impl->ctx);
        impl->ctx = nullptr;
    }
#ifdef _WIN32
    if (impl->com_initialized) {
        CoUninitialize();
        impl->com_initialized = false;
    }
#endif
}

bool CdAudioPlayer::start(const std::string& file, uint64_t start_offset,
                          uint64_t end_offset, uint32_t start_lba)
{
    stop();

    if (file.empty() || end_offset <= start_offset)
        return false;

    impl->file      = file;
    impl->start_off = start_offset;
    impl->read_off  = start_offset;
    impl->end_off   = end_offset;
    impl->start_lba = start_lba;
    impl->end_lba = start_lba +
        uint32_t((end_offset - start_offset) / 2352);
    impl->start_time = Impl::Clock::now();
    impl->paused_ns = std::chrono::nanoseconds{0};

    {
        std::lock_guard<std::mutex> lock(impl->mtx);
        impl->head = 0;
        impl->count = 0;
    }

    impl->worker_stop = false;
    impl->active = true;
    impl->paused = false;
    impl->ended = false;
    impl->worker = std::thread(&Impl::reader, impl.get());
    return true;
}

void CdAudioPlayer::pause(bool p)
{
    bool was_paused = impl->paused.exchange(p);
    if (p && !was_paused) {
        impl->pause_time = Impl::Clock::now();
    } else if (!p && was_paused) {
        impl->paused_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            Impl::Clock::now() - impl->pause_time);
    }
}

void CdAudioPlayer::stop()
{
    impl->worker_stop = true;
    if (impl->worker.joinable())
        impl->worker.join();

    {
        std::lock_guard<std::mutex> lock(impl->mtx);
        impl->head = 0;
        impl->count = 0;
    }

    impl->active = false;
    impl->paused = false;
    impl->ended = false;
}

bool CdAudioPlayer::active() const
{
    return impl->active.load();
}

bool CdAudioPlayer::playing() const
{
    if (!impl->active.load() || impl->paused.load())
        return false;
    return current_lba() < impl->end_lba;
}

bool CdAudioPlayer::paused() const
{
    return impl->paused.load();
}

uint32_t CdAudioPlayer::current_lba() const
{
    if (!impl->active.load())
        return impl->start_lba;

    auto now = Impl::Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - impl->start_time) - impl->paused_ns;
    if (impl->paused.load()) {
        elapsed -= std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - impl->pause_time);
    }
    if (elapsed.count() < 0)
        elapsed = std::chrono::nanoseconds{0};

    uint64_t sectors = uint64_t(elapsed.count()) / 13333000ULL;
    uint64_t lba = uint64_t(impl->start_lba) + sectors;
    if (lba > impl->end_lba)
        lba = impl->end_lba;
    return uint32_t(lba);
}
