/*
 * PowerMac Emulator - 86Box-style SDL/ImGui user interface.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "frontend.h"

#include <core/hostevents.h>
#include <cpu/ppc/ppcemu.h>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern bool g_auto_grab_mouse;
extern bool g_swap_command_option;

namespace frontend {

namespace {

bool             g_ready        = false;
bool             g_started      = false;
bool             g_show_settings = false;
bool             g_show_about    = false;
bool             g_show_help     = false;
SDL_Window*      g_window       = nullptr;
SDL_Renderer*    g_renderer     = nullptr;
FrontendConfig   g_draft;

// Simple string input helper (avoids depending on imgui_stdlib).
bool InputTextString(const char* label, std::string& value,
                     ImGuiInputTextFlags flags = 0, size_t max_len = 1024,
                     ImGuiInputTextCallback cb = nullptr, void* user_data = nullptr)
{
    std::vector<char> buf(max_len + 1, 0);
    strncpy(buf.data(), value.c_str(), max_len);
    bool changed = ImGui::InputText(label, buf.data(), buf.size(), flags, cb, user_data);
    if (changed)
        value = buf.data();
    return changed;
}

std::vector<std::string> split_semicolon(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ';') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

const char* machine_label(const std::string& id)
{
    if (id == "pm9500") return "Power Macintosh 9500";
    return "Power Macintosh 8500";
}

std::string normalize_gpu(const std::string& g)
{
    if (g == "none")          return "none";
    if (g == "AtiRagePro")    return "AtiRagePro";
    if (g == "AtiMach64Gx")   return "AtiMach64Gx";
    return "AtiRageGT";
}

void apply_draft()
{
    cfg() = g_draft;
    apply_machine_defaults();
    save_config(active_config_path());
    g_show_settings = false;
    power_off(po_restarting);
}

void open_settings()
{
    g_draft = cfg();
    g_show_settings = true;
}

void launch_vm_manager()
{
    std::string dir = SDL_GetBasePath() ? SDL_GetBasePath() : "";
    std::string exe = dir + "PowerMacVMManager";

    // Start the manager next to the emulator executable. The manager itself
    // locates the project root / emulator.ini and the VM folder.
    std::filesystem::path wd(dir);
    std::string workdir = wd.string();

#ifdef _WIN32
    exe += ".exe";
    HINSTANCE rc = ShellExecuteA(nullptr, "open", exe.c_str(), nullptr,
                                 workdir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(rc) <= 32) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PowerMac Emulator",
            "Could not start PowerMacVMManager.exe.\n"
            "Build the PowerMacVMManager target (it is part of this project).", g_window);
    }
#else
    std::string cmd = "cd \"" + workdir + "\" && nohup \"" + exe +
                      "\" >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PowerMac Emulator",
            "Could not start the PowerMacVMManager program.", g_window);
    }
#endif
}

void draw_menu_bar()
{
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Virtual Machine Manager...")) {
            launch_vm_manager();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            power_off(po_quit);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("System")) {
        if (ImGui::MenuItem("Restart", "Ctrl+R")) {
            power_off(po_restarting);
        }
        if (ImGui::MenuItem("Quit", "Alt+F4")) {
            power_off(po_quit);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Devices")) {
        if (ImGui::MenuItem("Settings...", "Ctrl+S")) {
            open_settings();
        }
        ImGui::Separator();
        ImGui::MenuItem("Built-in Ethernet (MACE)", nullptr, false, false);
        ImGui::MenuItem("Built-in Sound (AWACS)", nullptr, false, false);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Full Screen", "Ctrl+F")) {
            EventManager::get_instance()->post_window_event(
                DPPC_WINDOWEVENT_WINDOW_FULL_SCREEN_TOGGLE, SDL_GetWindowID(g_window));
        }
        if (ImGui::MenuItem("Bigger", "Ctrl++")) {
            EventManager::get_instance()->post_window_event(
                DPPC_WINDOWEVENT_WINDOW_BIGGER, SDL_GetWindowID(g_window));
        }
        if (ImGui::MenuItem("Smaller", "Ctrl+-")) {
            EventManager::get_instance()->post_window_event(
                DPPC_WINDOWEVENT_WINDOW_SMALLER, SDL_GetWindowID(g_window));
        }
        if (ImGui::MenuItem("Toggle Smoothing", "Ctrl+Shift+S")) {
            EventManager::get_instance()->post_window_event(
                DPPC_WINDOWEVENT_WINDOW_SCALE_QUALITY_TOGGLE, SDL_GetWindowID(g_window));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Release Mouse", "Middle Mouse")) {
            EventManager::get_instance()->toggle_mouse_grab();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) g_show_help = true;
        if (ImGui::MenuItem("About PowerMac Emulator")) g_show_about = true;
        ImGui::EndMenu();
    }

    // Right aligned status.
    const char* status = "Middle mouse: release mouse   |   Ctrl+G: release   |   Ctrl+F: fullscreen";
    float tw = ImGui::CalcTextSize(status).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - tw - 12.0f);
    ImGui::TextDisabled("%s", status);

    ImGui::EndMainMenuBar();
}

void draw_settings()
{
    if (!g_show_settings)
        return;

    ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("PowerMac Emulator Settings", &g_show_settings,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("settings_tabs")) {
        if (ImGui::BeginTabItem("Machine")) {
            ImGui::Spacing();
            int mach = (g_draft.machine == "pm9500") ? 1 : 0;
            const char* machines[] = { "Power Macintosh 8500", "Power Macintosh 9500" };
            if (ImGui::Combo("Model", &mach, machines, IM_ARRAYSIZE(machines))) {
                g_draft.machine = mach ? "pm9500" : "pm8500";
                if (mach == 1 && g_draft.pci_gpu.empty())
                    g_draft.pci_gpu = "none";
            }

            int cpu = 0;
            if (g_draft.cpu == "604e") cpu = 1;
            else if (g_draft.cpu == "750") cpu = 2;
            const char* cpus[] = { "PowerPC 604", "PowerPC 604e",
                                   "PowerPC 750 (G3 upgrade)" };
            if (ImGui::Combo("CPU", &cpu, cpus, IM_ARRAYSIZE(cpus))) {
                static const char* cpu_ids[] = { "604", "604e", "750" };
                g_draft.cpu = cpu_ids[cpu];
            }

            int ram_idx = 2;
            const int ram_vals[] = { 16, 32, 64, 128, 256, 512 };
            for (int i = 0; i < 6; i++) if (ram_vals[i] == g_draft.ram_mb) ram_idx = i;
            const char* rams[] = { "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", "512 MB" };
            if (ImGui::Combo("Memory", &ram_idx, rams, IM_ARRAYSIZE(rams)))
                g_draft.ram_mb = ram_vals[ram_idx];

            ImGui::Spacing();
            InputTextString("Boot ROM", g_draft.rom);
            InputTextString("Working directory", g_draft.working_dir);

            ImGui::Spacing();
            ImGui::Checkbox("Guest timers use host clock (realtime)", &g_draft.realtime);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Storage")) {
            ImGui::Spacing();
            ImGui::TextDisabled("Internal Fast SCSI (MESH)");
            ImGui::Separator();

            std::string hdd = "";
            for (size_t i = 0; i < g_draft.scsi_hdd.size(); i++) {
                if (i) hdd += ";";
                hdd += g_draft.scsi_hdd[i];
            }
            if (InputTextString("Hard disk images (separate with ;)", hdd))
                g_draft.scsi_hdd = split_semicolon(hdd);

            int hddbus = (g_draft.hdd_bus == "mesh") ? 1 : 0;
            const char* hddbuses[] = { "External SCSI (Curio)",
                                       "Internal Fast SCSI (MESH)" };
            if (ImGui::Combo("Hard disk bus", &hddbus, hddbuses, IM_ARRAYSIZE(hddbuses)))
                g_draft.hdd_bus = hddbus ? "mesh" : "curio";

            std::string cd = "";
            for (size_t i = 0; i < g_draft.scsi_cd.size(); i++) {
                if (i) cd += ";";
                cd += g_draft.scsi_cd[i];
            }
            if (InputTextString("CD-ROM images (separate with ;)", cd))
                g_draft.scsi_cd = split_semicolon(cd);

            int cdbus = (g_draft.cd_bus == "mesh") ? 1 : 0;
            const char* cdbuses[] = { "External SCSI (Curio)",
                                      "Internal Fast SCSI (MESH)" };
            if (ImGui::Combo("CD-ROM bus", &cdbus, cdbuses, IM_ARRAYSIZE(cdbuses)))
                g_draft.cd_bus = cdbus ? "mesh" : "curio";

            ImGui::Spacing();
            ImGui::Checkbox("Network card (MACE Ethernet)", &g_draft.net_enabled);

            ImGui::Spacing();
            ImGui::TextDisabled("Optional PCI IDE (CMD646)");
            ImGui::Separator();
            InputTextString("IDE hard disk image", g_draft.ide_hdd);
            ImGui::TextWrapped("The IDE hard disk is attached to an emulated CMD646 "
                               "PCI controller. Mac OS support depends on the "
                               "installed system software.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Display")) {
            ImGui::Spacing();
            if (g_draft.machine == "pm8500") {
                ImGui::Text("Video: built-in Control (Chaos/VCI), 640x480");
            } else {
                ImGui::Text("Video: built-in Control (compatibility) + optional PCI card");
                int gpu = 0;
                if (g_draft.pci_gpu == "none")        gpu = 0;
                else if (g_draft.pci_gpu == "AtiRagePro")  gpu = 2;
                else if (g_draft.pci_gpu == "AtiMach64Gx") gpu = 3;
                else gpu = 1;
                const char* gpus[] = { "None", "ATI Rage GT", "ATI Rage Pro", "ATI Mach64 GX" };
                if (ImGui::Combo("PCI video card (slot D2)", &gpu, gpus, IM_ARRAYSIZE(gpus))) {
                    const char* ids[] = { "none", "AtiRageGT", "AtiRagePro", "AtiMach64Gx" };
                    g_draft.pci_gpu = ids[gpu];
                }
            }
            ImGui::Spacing();
            int mon = 0;
            const char* mon_ids[]   = { "VGA-SVGA", "Multiscan20in",
                                        "AppleVision1710", "" };
            const char* mon_names[] = { "VGA / SVGA (multiple modes)",
                                        "Multiple Scan 20in",
                                        "AppleVision 1710",
                                        "DDC / EDID (1280x800)" };
            for (int i = 0; i < 4; i++)
                if (g_draft.mon_id == mon_ids[i]) mon = i;
            if (ImGui::Combo("Monitor", &mon, mon_names, IM_ARRAYSIZE(mon_names)))
                g_draft.mon_id = mon_ids[mon];

            ImGui::Spacing();
            int scale_idx = std::clamp(g_draft.scale - 1, 0, 3);
            const char* scales[] = { "1x", "2x", "3x", "4x" };
            if (ImGui::Combo("Initial window scale", &scale_idx, scales, IM_ARRAYSIZE(scales)))
                g_draft.scale = scale_idx + 1;
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Input")) {
            ImGui::Spacing();
            ImGui::Checkbox("Capture mouse when the guest cursor is visible", &g_draft.auto_grab);
            ImGui::Checkbox("Swap Command and Option keys", &g_draft.swap_cmd_opt);
            int kb = 0;
            const char* kbd_ids[]   = { "Eng_USA", "Eng_GBR", "Fra_FRA", "Deu_DEU", "Ita_ITA", "Spa_ESP", "Jpn_JPN" };
            const char* kbd_names[] = { "English (US)", "English (UK)", "French", "German", "Italian", "Spanish", "Japanese" };
            for (int i = 0; i < 7; i++) if (g_draft.keyboard == kbd_ids[i]) kb = i;
            if (ImGui::Combo("Keyboard layout", &kb, kbd_names, IM_ARRAYSIZE(kbd_names)))
                g_draft.keyboard = kbd_ids[kb];
            ImGui::Spacing();
            ImGui::TextDisabled("Middle mouse button always releases the captured mouse.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sound & Network")) {
            ImGui::Spacing();
            ImGui::Text("Ethernet: built-in MACE (AAUI)");
            ImGui::Text("Sound: built-in AWACS/Screamer");
            ImGui::Spacing();
            ImGui::TextWrapped("These controllers are part of the GrandCentral I/O "
                               "controller and are always present on the 8500/9500.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Apply && Restart", ImVec2(160, 0)))
        apply_draft();
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(100, 0))) {
        cfg() = g_draft;
        apply_machine_defaults();
        save_config(active_config_path());
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)))
        g_show_settings = false;

    ImGui::End();
}

void draw_about()
{
    if (!g_show_about)
        return;
    ImGui::SetNextWindowSize(ImVec2(520, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("About PowerMac Emulator", &g_show_about,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("PowerMac Emulator");
    ImGui::TextDisabled("A Power Macintosh 8500/9500 emulator (SDL front-end)");
    ImGui::Separator();
    ImGui::TextWrapped("Emulation engine based on DingusPPC by the DingusPPC "
                       "Development Team (GPLv3).");
    ImGui::TextWrapped("SDL2 display and audio output. User interface inspired by 86Box.");
    ImGui::Spacing();
    ImGui::Text("Machine: %s", machine_label(cfg().machine));
    ImGui::Text("CPU: PowerPC %s", cfg().cpu.c_str());
    ImGui::Text("Memory: %d MB", cfg().ram_mb);
    ImGui::Spacing();
    ImGui::TextWrapped("This emulator does not include any Apple ROM or system software. "
                       "You must supply a compatible Macintosh ROM image.");
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0)))
        g_show_about = false;
    ImGui::End();
}

void draw_help()
{
    if (!g_show_help)
        return;
    ImGui::SetNextWindowSize(ImVec2(520, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Keyboard Shortcuts", &g_show_help,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("Middle mouse button  - release / capture the mouse");
    ImGui::Text("Ctrl+G              - toggle mouse capture");
    ImGui::Text("Ctrl+F              - toggle full screen");
    ImGui::Text("Ctrl+Shift+F        - full screen (no letterbox)");
    ImGui::Text("Ctrl++ / Ctrl+-     - grow / shrink the window");
    ImGui::Text("Ctrl+Shift+S        - toggle nearest/bilinear scaling");
    ImGui::Text("Ctrl+D              - enter the debugger");
    ImGui::Text("Ctrl+Alt+R          - toggle realtime timing");
    ImGui::Separator();
    ImGui::TextWrapped("Guest shortcuts are passed through to Mac OS, except the "
                       "host shortcuts listed above.");
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0)))
        g_show_help = false;
    ImGui::End();
}

} // namespace

bool ui_init(SDL_Window* window, SDL_Renderer* renderer)
{
    if (g_ready)
        return true;
    if (!window || !renderer)
        return false;

    g_window = window;
    g_renderer = renderer;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 3.0f;
    style.FrameRounding  = 2.0f;
    style.GrabRounding   = 2.0f;

    if (!ImGui_ImplSDL2_InitForSDLRenderer(window, renderer)) {
        ImGui::DestroyContext();
        g_window = nullptr; g_renderer = nullptr;
        return false;
    }
    if (!ImGui_ImplSDLRenderer2_Init(renderer)) {
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        g_window = nullptr; g_renderer = nullptr;
        return false;
    }

    g_ready = true;
    g_draft = cfg();
    return true;
}

void ui_shutdown()
{
    if (!g_ready)
        return;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
    g_started = false;
    g_window = nullptr; g_renderer = nullptr;
}

bool ui_is_ready()
{
    return g_ready;
}

void ui_mark_started()
{
    g_started = true;
}

bool ui_has_started()
{
    return g_started;
}

int ui_top_inset()
{
    // Reserve a constant band for the main menu bar. Using a constant keeps
    // the display geometry stable even before the first ImGui frame.
    return g_ready ? 24 : 0;
}

bool ui_process_event(const SDL_Event* event)
{
    if (!g_ready)
        return false;

    ImGui_ImplSDL2_ProcessEvent(event);

    if (event->type == SDL_QUIT) {
        // Let the regular quit handling proceed.
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();

    bool mouse_evt = (event->type == SDL_MOUSEMOTION ||
                      event->type == SDL_MOUSEBUTTONDOWN ||
                      event->type == SDL_MOUSEBUTTONUP ||
                      event->type == SDL_MOUSEWHEEL);

    bool key_evt = (event->type == SDL_KEYDOWN ||
                    event->type == SDL_KEYUP ||
                    event->type == SDL_TEXTINPUT);

    bool dialog_open = g_show_settings || g_show_about || g_show_help;
    if (mouse_evt && (io.WantCaptureMouse || dialog_open))
        return true;
    if (key_evt && (io.WantCaptureKeyboard || dialog_open))
        return true;

    return false;
}

void ui_frame(SDL_Renderer* renderer)
{
    if (!g_ready || !renderer)
        return;

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    draw_menu_bar();
    draw_settings();
    draw_about();
    draw_help();

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
}

void ui_message_loop(const std::string& title, const std::string& text)
{
    bool temp_video = false;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) == 0)
            temp_video = true;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(),
                             text.c_str(), nullptr);
    if (temp_video)
        SDL_Quit();
}

} // namespace frontend
