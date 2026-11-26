/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

/** @file Minimal libslirp-backed Ethernet backend. */

#ifndef SLIRP_BACKEND_H
#define SLIRP_BACKEND_H

#include <core/timermanager.h>

#include <cinttypes>
#include <functional>
#include <memory>
#include <vector>

struct Slirp;

class SlirpBackend {
public:
    using FrameCallback = std::function<void(const uint8_t* frame, int len)>;

    SlirpBackend();
    ~SlirpBackend();

    bool init(FrameCallback rx_cb);
    void shutdown();

    void input_frame(const uint8_t* frame, int len);

    bool is_ready() const { return this->slirp != nullptr; }

private:
    struct Timer;

    void poll();
    void schedule_poll();

    struct PollEntry {
        int fd;
        int events;
        int revents;
    };

    Slirp* slirp = nullptr;
    void* slirp_cb = nullptr;
    FrameCallback rx_callback;
    std::vector<Timer*> timers;
    std::vector<PollEntry> poll_entries;
    TimerInfo poll_timer;
    bool polling = false;
};

#endif // SLIRP_BACKEND_H
