/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

/** @file Rust userspace-network-backed Ethernet backend. */

#include <devices/ethernet/slirp_backend.h>

#include <loguru.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
void* slirp_native_create(const uint8_t* dns, size_t dns_count, uint32_t mtu);
void slirp_native_destroy(void* net);
int slirp_native_input(void* net, const uint8_t* frame, size_t len);
int slirp_native_recv(void* net, uint8_t* out, size_t cap);
}

static std::vector<uint8_t> get_host_dns_servers() {
    std::vector<uint8_t> result;
#ifdef _WIN32
    ULONG size = 0;
    if (GetNetworkParams(nullptr, &size) == ERROR_BUFFER_OVERFLOW) {
        std::vector<uint8_t> buffer(size);
        FIXED_INFO* info = reinterpret_cast<FIXED_INFO*>(buffer.data());
        if (GetNetworkParams(info, &size) == ERROR_SUCCESS) {
            for (IP_ADDR_STRING* entry = &info->DnsServerList; entry;
                 entry = entry->Next) {
                in_addr address {};
                if (inet_pton(AF_INET, entry->IpAddress.String, &address) == 1) {
                    const uint8_t* octets =
                        reinterpret_cast<const uint8_t*>(&address);
                    result.insert(result.end(), octets, octets + 4);
                    if (result.size() >= 8)
                        break;
                }
            }
        }
    }
#endif
    if (result.empty()) {
        result = {1, 1, 1, 1, 8, 8, 8, 8};
    }
    return result;
}

SlirpBackend::SlirpBackend() = default;

SlirpBackend::~SlirpBackend() {
    this->shutdown();
}

bool SlirpBackend::init(FrameCallback rx_cb) {
    this->shutdown();
    this->rx_callback = std::move(rx_cb);

    std::vector<uint8_t> dns = get_host_dns_servers();
    this->native = slirp_native_create(dns.data(), dns.size() / 4, 1500);
    if (!this->native) {
        LOG_F(ERROR, "Slirp: failed to initialize Rust network backend");
        return false;
    }

    this->schedule_poll();
    LOG_F(INFO, "Slirp: Rust user-mode network started (10.0.2.0/24)");
    return true;
}

void SlirpBackend::shutdown() {
    if (this->poll_timer.active) {
        TimerManager::get_instance()->cancel_timer(this->poll_timer);
    }
    if (this->native) {
        slirp_native_destroy(this->native);
        this->native = nullptr;
    }
}

void SlirpBackend::input_frame(const uint8_t* frame, int len) {
    if (this->native && frame && len > 0) {
        slirp_native_input(this->native, frame, (size_t)len);
    }
}

void SlirpBackend::schedule_poll() {
    TimerManager::get_instance()->add_absolute_timer(
        this->poll_timer, 0, MSECS_TO_NSECS(2),
        [this](uint64_t, uint64_t) { this->poll(); });
}

void SlirpBackend::poll() {
    if (!this->native)
        return;

    uint8_t buffer[2048];
    int drained = 0;
    while (drained < 64) {
        int len = slirp_native_recv(this->native, buffer, sizeof(buffer));
        if (len <= 0)
            break;
        if (this->rx_callback)
            this->rx_callback(buffer, len);
        drained++;
    }

    if (this->native)
        this->schedule_poll();
}
