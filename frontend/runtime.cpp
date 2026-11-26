/*
 * PowerMac Emulator - front-end runtime hooks.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "runtime.h"

#include <core/hostevents.h>
#include <devices/floppy/swim3.h>
#include <machines/machinebase.h>

#include <atomic>
#include <chrono>
#include <thread>

bool g_emulator_paused = false;

static std::atomic<uint32_t> g_activity_seq[4];
static std::atomic<bool>     g_activity_write[4];

void frontend_activity(DeviceActivity kind, bool write)
{
    int idx = (int)kind;
    if (idx < 0 || idx >= 4)
        return;
    g_activity_write[idx].store(write, std::memory_order_relaxed);
    g_activity_seq[idx].fetch_add(1, std::memory_order_relaxed);
}

uint32_t frontend_activity_serial(DeviceActivity kind)
{
    int idx = (int)kind;
    if (idx < 0 || idx >= 4)
        return 0;
    return g_activity_seq[idx].load(std::memory_order_relaxed);
}

bool frontend_activity_write(DeviceActivity kind)
{
    int idx = (int)kind;
    if (idx < 0 || idx >= 4)
        return false;
    return g_activity_write[idx].load(std::memory_order_relaxed);
}

void frontend_idle()
{
    frontend_pump_events();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void frontend_pump_events()
{
#ifdef POWERMAC_QT_UI
    frontend_qt_pump();
#endif
}

void frontend_eject_cdrom()
{
    CdromImageEvent ev{};
    ev.eject = true;
    EventManager::get_instance()->post_cdrom_event(ev);
}

bool frontend_swap_cdrom(const std::string& path)
{
    if (path.empty())
        return false;

    CdromImageEvent eject{};
    eject.eject = true;
    EventManager::get_instance()->post_cdrom_event(eject);

    CdromImageEvent insert{};
    insert.image_path = path;
    EventManager::get_instance()->post_cdrom_event(insert);

    return insert.result == CdromInsertionResult::SUCCESS;
}

bool frontend_swap_floppy(const std::string& path, bool write_protect)
{
    if (path.empty() || !gMachineObj)
        return false;
    auto* swim = dynamic_cast<Swim3::Swim3Ctrl*>(
        gMachineObj->get_comp_by_name_optional("Swim3"));
    if (!swim)
        return false;
    return swim->insert_floppy(path, write_protect);
}

void frontend_eject_floppy()
{
    if (!gMachineObj)
        return;
    auto* swim = dynamic_cast<Swim3::Swim3Ctrl*>(
        gMachineObj->get_comp_by_name_optional("Swim3"));
    if (swim)
        swim->eject_floppy();
}

#ifndef POWERMAC_QT_UI
void frontend_qt_init() {}
void frontend_qt_pump() {}
void frontend_qt_embed_window(void*, void*, int, int) {}
void frontend_qt_display_destroyed() {}
void frontend_qt_update_display_size(int, int) {}
void frontend_qt_present_frame(const uint32_t*, int, int, bool) {}
void frontend_qt_present_blank() {}
void frontend_qt_release_caps() {}
#endif
