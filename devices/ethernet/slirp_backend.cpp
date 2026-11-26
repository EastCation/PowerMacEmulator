/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

/** @file Minimal libslirp-backed Ethernet backend. */

#include <devices/ethernet/slirp_backend.h>

#ifndef POWERMAC_HAVE_SLIRP
SlirpBackend::SlirpBackend() = default;
SlirpBackend::~SlirpBackend() = default;
bool SlirpBackend::init(FrameCallback) { return false; }
void SlirpBackend::shutdown() {}
void SlirpBackend::input_frame(const uint8_t*, int) {}
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <slirp/libslirp.h>

#include <loguru.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>

struct SlirpBackend::Timer {
    TimerInfo ti;
    SlirpTimerId id;
    SlirpTimerCb cb = nullptr;
    void* cb_opaque = nullptr;
    SlirpBackend* owner = nullptr;
};

SlirpBackend::SlirpBackend() = default;

SlirpBackend::~SlirpBackend() {
    this->shutdown();
}

bool SlirpBackend::init(FrameCallback rx_cb) {
    this->shutdown();
    this->rx_callback = std::move(rx_cb);

    SlirpCb* cb = new SlirpCb{};
    this->slirp_cb = cb;
    cb->send_packet = [](const void* buf, size_t len, void* opaque) -> slirp_ssize_t {
        SlirpBackend* self = static_cast<SlirpBackend*>(opaque);
        if (self->rx_callback && buf && len)
            self->rx_callback(static_cast<const uint8_t*>(buf), (int)len);
        return (slirp_ssize_t)len;
    };
    cb->guest_error = [](const char* msg, void* opaque) {
        LOG_F(WARNING, "Slirp guest error: %s", msg ? msg : "");
    };
    cb->clock_get_ns = [](void* opaque) -> int64_t {
        return (int64_t)TimerManager::get_instance()->current_time_ns();
    };
    cb->timer_new = [](SlirpTimerCb cb_fn, void* cb_opaque, void* opaque) -> void* {
        Timer* t = new Timer();
        t->id = SLIRP_TIMER_RA;
        t->cb = cb_fn;
        t->cb_opaque = cb_opaque;
        t->owner = static_cast<SlirpBackend*>(opaque);
        t->owner->timers.push_back(t);
        return t;
    };
    cb->timer_free = [](void* timer, void* opaque) {
        Timer* t = static_cast<Timer*>(timer);
        if (!t) return;
        SlirpBackend* self = static_cast<SlirpBackend*>(opaque);
        if (t->ti.active)
            TimerManager::get_instance()->cancel_timer(t->ti);
        self->timers.erase(std::remove(self->timers.begin(), self->timers.end(), t),
                           self->timers.end());
        delete t;
    };
    cb->timer_mod = [](void* timer, int64_t expire_time, void* opaque) {
        Timer* t = static_cast<Timer*>(timer);
        if (!t) return;
        if (t->ti.active)
            TimerManager::get_instance()->cancel_timer(t->ti);
        uint64_t expire_ns = (uint64_t)expire_time * NS_PER_MSEC;
        TimerManager::get_instance()->add_absolute_timer(
            t->ti, expire_ns, 0,
            [t](uint64_t, uint64_t) {
                if (t->cb) {
                    t->cb(t->cb_opaque);
                } else if (t->owner && t->owner->slirp) {
                    slirp_handle_timer(t->owner->slirp, t->id, t->cb_opaque);
                }
            });
    };
    cb->register_poll_fd = [](int fd, void* opaque) {};
    cb->unregister_poll_fd = [](int fd, void* opaque) {};
    cb->notify = [](void* opaque) {};
    cb->timer_new_opaque = [](SlirpTimerId id, void* cb_opaque, void* opaque) -> void* {
        Timer* t = new Timer();
        t->id = id;
        t->cb_opaque = cb_opaque;
        t->owner = static_cast<SlirpBackend*>(opaque);
        t->owner->timers.push_back(t);
        return t;
    };

    SlirpConfig cfg{};
    cfg.version = 5;
    cfg.restricted = 0;
    cfg.in_enabled = true;
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    cfg.in6_enabled = false;
    cfg.vhostname = "PowerMac";
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);
    cfg.if_mtu = 1500;
    cfg.if_mru = 1500;
    cfg.disable_host_loopback = false;
    cfg.enable_emu = false;
    cfg.disable_dns = false;
    cfg.disable_dhcp = false;
    cfg.mfr_id = 0;

    this->slirp = slirp_new(&cfg, cb, this);
    if (!this->slirp) {
        LOG_F(ERROR, "Slirp: failed to initialize libslirp");
        return false;
    }

    this->schedule_poll();
    LOG_F(INFO, "Slirp: user-mode network started (10.0.2.0/24)");
    return true;
}

void SlirpBackend::shutdown() {
    if (this->poll_timer.active)
        TimerManager::get_instance()->cancel_timer(this->poll_timer);
    for (Timer* timer : this->timers) {
        if (timer->ti.active)
            TimerManager::get_instance()->cancel_timer(timer->ti);
        delete timer;
    }
    this->timers.clear();
    if (this->slirp) {
        slirp_cleanup(this->slirp);
        this->slirp = nullptr;
    }
    delete static_cast<SlirpCb*>(this->slirp_cb);
    this->slirp_cb = nullptr;
}

void SlirpBackend::input_frame(const uint8_t* frame, int len) {
    if (this->slirp && frame && len > 0)
        slirp_input(this->slirp, frame, len);
}

void SlirpBackend::schedule_poll() {
    TimerManager::get_instance()->add_absolute_timer(
        this->poll_timer, 0, MSECS_TO_NSECS(10),
        [this](uint64_t, uint64_t) { this->poll(); });
}

void SlirpBackend::poll() {
    if (!this->slirp || this->polling)
        return;
    this->polling = true;
    this->poll_entries.clear();

    auto add_poll = +[](int fd, int events, void* opaque) -> int {
        SlirpBackend* self = static_cast<SlirpBackend*>(opaque);
        self->poll_entries.push_back(PollEntry{fd, events, 0});
        return (int)self->poll_entries.size() - 1;
    };
    auto get_revents = +[](int idx, void* opaque) -> int {
        SlirpBackend* self = static_cast<SlirpBackend*>(opaque);
        if (idx < 0 || idx >= (int)self->poll_entries.size())
            return 0;
        return self->poll_entries[idx].revents;
    };

    uint32_t timeout = 0;
    slirp_pollfds_fill(this->slirp, &timeout, add_poll, this);

    fd_set readfds, writefds, exceptfds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&exceptfds);
    int max_fd = -1;

    for (size_t i = 0; i < this->poll_entries.size(); i++) {
        PollEntry& entry = this->poll_entries[i];
        entry.revents = 0;
        if (entry.fd < 0 || entry.fd >= FD_SETSIZE)
            continue;
        if (entry.events & SLIRP_POLL_IN)
            FD_SET(entry.fd, &readfds);
        if (entry.events & SLIRP_POLL_OUT)
            FD_SET(entry.fd, &writefds);
        if (entry.events & (SLIRP_POLL_ERR | SLIRP_POLL_HUP))
            FD_SET(entry.fd, &exceptfds);
        max_fd = std::max(max_fd, entry.fd);
    }

    timeval tv{};
    int select_error = 0;
    if (max_fd >= 0) {
        int ret = select(max_fd + 1, &readfds, &writefds, &exceptfds, &tv);
        if (ret < 0) {
            select_error = 1;
        } else if (ret > 0) {
            for (size_t i = 0; i < this->poll_entries.size(); i++) {
                PollEntry& entry = this->poll_entries[i];
                if (entry.fd < 0 || entry.fd >= FD_SETSIZE)
                    continue;
                if (FD_ISSET(entry.fd, &readfds))
                    entry.revents |= SLIRP_POLL_IN;
                if (FD_ISSET(entry.fd, &writefds))
                    entry.revents |= SLIRP_POLL_OUT;
                if (FD_ISSET(entry.fd, &exceptfds))
                    entry.revents |= SLIRP_POLL_ERR;
            }
        }
    }

    slirp_pollfds_poll(this->slirp, select_error, get_revents, this);
    this->polling = false;
}

#endif // POWERMAC_HAVE_SLIRP
