/*
 * PowerMac Emulator - standalone virtual machine manager.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "vmmanager.h"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Required by utils/imgfile_sdl.cpp (the emulator normally provides this).
bool is_deterministic = false;
bool g_realtime = false;

namespace {

namespace fs = std::filesystem;

bool              g_show_create = false;
bool              g_confirm_delete = false;
std::vector<vm::VmEntry> g_list;
int               g_selected = -1;
std::string       g_root;
std::string       g_error;
std::string       g_status;

bool              g_edit_mode = false;
std::string       g_edit_name;

std::string       g_new_name;
std::string       g_new_rom;
std::string       g_new_cd;
std::string       g_new_hdd;
std::string       g_new_machine = "pm8500";
std::string       g_new_cpu     = "750";
std::string       g_new_hdd_bus = "curio";
std::string       g_new_cd_bus  = "curio";
std::string       g_new_gpu     = "none";
int               g_new_ram     = 2;
int               g_new_disk    = 2;

std::string       g_emulator_exe;

int ram_to_index(int mb)
{
    static const int vals[] = { 16, 32, 64, 128, 256, 512 };
    for (int i = 0; i < 6; i++)
        if (vals[i] == mb)
            return i;
    return 3;
}

bool InputTextString(const char* label, std::string& value,
                     ImGuiInputTextFlags flags = 0, size_t max_len = 1024)
{
    std::vector<char> buf(max_len + 1, 0);
    strncpy(buf.data(), value.c_str(), max_len);
    bool changed = ImGui::InputText(label, buf.data(), buf.size(), flags);
    if (changed)
        value = buf.data();
    return changed;
}

std::string exe_dir()
{
    const char* base = SDL_GetBasePath();
    return base ? std::string(base) : std::string("./");
}

void find_emulator()
{
    std::string dir = exe_dir();
    g_emulator_exe = dir + "PowerMacEmulator";
#ifdef _WIN32
    g_emulator_exe += ".exe";
#endif
}

std::string first_matching(const std::string& dir, const std::string& ext)
{
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return "";
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file())
            continue;
        std::string p = de.path().string();
        if (p.size() >= ext.size() &&
            p.compare(p.size() - ext.size(), ext.size(), ext) == 0)
            return p;
    }
    return "";
}

void refresh()
{
    g_list = vm::scan();
    if (g_selected >= int(g_list.size()))
        g_selected = int(g_list.size()) - 1;
    g_error.clear();
}

void open_create()
{
    g_edit_mode = false;
    g_edit_name.clear();
    g_new_name.clear();
    g_new_machine = "pm8500";
    g_new_cpu     = "750";
    g_new_ram     = 2;
    g_new_disk    = 2;
    g_new_hdd.clear();
    g_new_hdd_bus = "curio";
    g_new_cd_bus  = "curio";
    g_new_gpu     = "none";

    std::string rom = first_matching("roms", ".ROM");
    if (rom.empty())
        rom = first_matching("roms", ".rom");
    g_new_rom = rom;

    std::string iso = first_matching("test-isos", ".iso");
    g_new_cd = iso;

    g_error.clear();
    g_show_create = true;
}

void open_edit()
{
    if (g_selected < 0 || g_selected >= int(g_list.size()))
        return;

    vm::VmSettings s;
    std::string err;
    if (!vm::load_vm_settings(g_list[size_t(g_selected)].name, s, err)) {
        g_error = err;
        return;
    }

    g_edit_mode  = true;
    g_edit_name  = g_list[size_t(g_selected)].name;
    g_new_name   = g_edit_name;
    g_new_machine = s.machine;
    g_new_cpu     = s.cpu;
    g_new_ram     = ram_to_index(s.ram_mb);
    g_new_rom     = s.rom;
    g_new_cd      = s.scsi_cd;
    g_new_hdd     = s.scsi_hdd;
    g_new_hdd_bus = s.hdd_bus;
    g_new_cd_bus  = s.cd_bus;
    g_new_gpu     = s.pci_gpu;

    g_error.clear();
    g_show_create = true;
}

void draw_create()
{
    if (!g_show_create)
        return;

    ImGui::SetNextWindowSize(ImVec2(660, 590), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    const char* title = g_edit_mode ? "Edit Virtual Machine" : "New Virtual Machine";
    if (!ImGui::Begin(title, &g_show_create, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    static const int ram_vals[] = { 16, 32, 64, 128, 256, 512 };
    static const int disk_gb[]  = { 0, 1, 2, 4, 8, 16 };

    InputTextString("Name", g_new_name,
                    g_edit_mode ? ImGuiInputTextFlags_ReadOnly : 0);

    int mach = (g_new_machine == "pm9500") ? 1 : 0;
    const char* machines[] = { "Power Macintosh 8500", "Power Macintosh 9500" };
    if (ImGui::Combo("Model", &mach, machines, IM_ARRAYSIZE(machines)))
        g_new_machine = mach ? "pm9500" : "pm8500";

    int cpu = 0;
    if (g_new_cpu == "604e") cpu = 1;
    else if (g_new_cpu == "750") cpu = 2;
    const char* cpus[] = { "PowerPC 604", "PowerPC 604e",
                           "PowerPC 750 (G3, required by Mac OS 9.2)" };
    if (ImGui::Combo("CPU", &cpu, cpus, IM_ARRAYSIZE(cpus))) {
        static const char* ids[] = { "604", "604e", "750" };
        g_new_cpu = ids[cpu];
    }

    const char* rams[] = { "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", "512 MB" };
    ImGui::Combo("Memory", &g_new_ram, rams, IM_ARRAYSIZE(rams));

    if (!g_edit_mode) {
        const char* disks[] = { "None", "1 GB", "2 GB", "4 GB", "8 GB", "16 GB" };
        ImGui::Combo("Hard disk (dynamic VHD)", &g_new_disk, disks, IM_ARRAYSIZE(disks));
    }

    InputTextString("Boot ROM", g_new_rom);
    InputTextString("Install CD image (optional)", g_new_cd);

    if (g_edit_mode) {
        InputTextString("Hard disk image(s) (separate with ;)", g_new_hdd);
        int hb = (g_new_hdd_bus == "mesh") ? 1 : 0;
        const char* hbs[] = { "External SCSI (Curio)", "Internal Fast SCSI (MESH)" };
        if (ImGui::Combo("Hard disk bus", &hb, hbs, IM_ARRAYSIZE(hbs)))
            g_new_hdd_bus = hb ? "mesh" : "curio";
    }

    int cb = (g_new_cd_bus == "mesh") ? 1 : 0;
    const char* cbs[] = { "External SCSI (Curio)", "Internal Fast SCSI (MESH)" };
    if (ImGui::Combo("CD-ROM bus", &cb, cbs, IM_ARRAYSIZE(cbs)))
        g_new_cd_bus = cb ? "mesh" : "curio";

    int gpu = 0;
    if (g_new_gpu == "AtiRageGT")       gpu = 1;
    else if (g_new_gpu == "AtiRagePro") gpu = 2;
    else if (g_new_gpu == "AtiMach64Gx") gpu = 3;
    const char* gpus[] = { "None", "ATI Rage GT", "ATI Rage Pro", "ATI Mach64 GX" };
    if (ImGui::Combo("PCI video (9500, slot D2)", &gpu, gpus, IM_ARRAYSIZE(gpus))) {
        static const char* ids[] = { "none", "AtiRageGT", "AtiRagePro", "AtiMach64Gx" };
        g_new_gpu = ids[gpu];
    }

    ImGui::Spacing();
    ImGui::TextWrapped("Mac OS 9.2 requires a G3 or later CPU. Select the "
                       "PowerPC 750 option for 9.2; use 604/604e for Mac OS 8, "
                       "9.0/9.1 and Rhapsody.");
    if (!g_edit_mode)
        ImGui::TextWrapped("The hard disk is a sparse VHD image and only uses host "
                           "space for blocks that the guest has written.");

    if (!g_error.empty())
        ImGui::TextColored(ImVec4(0.85f, 0.1f, 0.1f, 1.0f), "%s", g_error.c_str());

    ImGui::Separator();
    if (g_edit_mode) {
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            vm::VmSettings s;
            s.machine  = g_new_machine;
            s.cpu      = g_new_cpu;
            s.ram_mb   = ram_vals[g_new_ram];
            s.rom      = g_new_rom;
            s.scsi_hdd = g_new_hdd;
            s.scsi_cd  = g_new_cd;
            s.hdd_bus  = g_new_hdd_bus;
            s.cd_bus   = g_new_cd_bus;
            s.pci_gpu  = g_new_gpu;
            std::string err;
            if (vm::update_vm(g_edit_name, s, err)) {
                g_show_create = false;
                refresh();
                g_status = "Saved " + g_edit_name;
            } else {
                g_error = err;
            }
        }
    } else {
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            std::string err;
            if (vm::create_vm(g_new_name, g_new_machine, g_new_cpu,
                              ram_vals[g_new_ram], disk_gb[g_new_disk],
                              g_new_rom, g_new_cd, err)) {
                g_show_create = false;
                refresh();
                g_status = "Virtual machine created.";
            } else {
                g_error = err;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        g_show_create = false;

    ImGui::End();
}

} // namespace

int main(int argc, char** argv)
{
    std::string base_arg, root_arg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--base" && i + 1 < argc)
            base_arg = argv[++i];
        else if (a == "--root" && i + 1 < argc)
            root_arg = argv[++i];
    }
    if (!base_arg.empty()) {
        std::error_code ec;
        std::filesystem::current_path(base_arg, ec);
    }

    find_emulator();
    vm::set_program_dir(exe_dir());

    // When started directly from build/bin, look for the project root two
    // levels up so that roms/, test-isos/ and vms/ are found automatically.
    if (base_arg.empty()) {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (!fs::exists(cwd / "roms", ec)) {
            fs::path exedir(exe_dir());
            if (exedir.filename() == "bin") {
                fs::path proj = exedir.parent_path().parent_path();
                if (fs::exists(proj / "roms", ec))
                    fs::current_path(proj, ec);
            }
        }
    }

    vm::load_launcher_settings();
    if (!root_arg.empty())
        vm::set_root_dir(root_arg);
    g_root = vm::root_dir();
    refresh();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PowerMac VM Manager",
                                 SDL_GetError(), nullptr);
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "PowerMac Emulator - Virtual Machine Manager",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 900, 620,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PowerMac VM Manager",
                                 SDL_GetError(), nullptr);
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PowerMac VM Manager",
                                 SDL_GetError(), nullptr);
        SDL_Quit();
        return 1;
    }

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

    if (!ImGui_ImplSDL2_InitForSDLRenderer(window, renderer) ||
        !ImGui_ImplSDLRenderer2_Init(renderer)) {
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT)
                running = false;
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("VM Manager", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("PowerMac Emulator - Virtual Machine Manager");
        ImGui::Separator();

        InputTextString("VM root folder", g_root);
        ImGui::SameLine();
        if (ImGui::Button("Set root")) {
            vm::set_root_dir(g_root);
            g_root = vm::root_dir();
            refresh();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh"))
            refresh();

        ImGui::Separator();

        float list_h = ImGui::GetContentRegionAvail().y - 60.0f;
        if (list_h < 120.0f) list_h = 120.0f;
        ImGui::BeginChild("vm_list", ImVec2(0, list_h), true);
        for (int i = 0; i < int(g_list.size()); i++) {
            const auto& e = g_list[size_t(i)];
            std::string label = e.name + "    [" +
                (e.machine.empty() ? "?" : e.machine) + "]";
            if (ImGui::Selectable(label.c_str(), g_selected == i,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                g_selected = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    std::string err;
                    if (vm::launch_vm(e.name, g_emulator_exe, err))
                        g_status = "Started " + e.name;
                    else
                        g_error = err;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", e.folder.c_str());
        }
        if (g_list.empty())
            ImGui::TextDisabled("No virtual machines. Click \"New VM...\" to create one.");
        ImGui::EndChild();

        bool have_sel = g_selected >= 0 && g_selected < int(g_list.size());

        ImGui::BeginDisabled(!have_sel);
        if (ImGui::Button("Start", ImVec2(110, 0)) && have_sel) {
            std::string err;
            if (vm::launch_vm(g_list[size_t(g_selected)].name, g_emulator_exe, err))
                g_status = "Started " + g_list[size_t(g_selected)].name;
            else
                g_error = err;
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Folder", ImVec2(130, 0)) && have_sel)
            vm::open_folder(g_list[size_t(g_selected)].folder);
        ImGui::SameLine();
        if (ImGui::Button("Edit...", ImVec2(90, 0)) && have_sel)
            open_edit();
        ImGui::SameLine();
        if (ImGui::Button("Delete...", ImVec2(110, 0)) && have_sel)
            g_confirm_delete = true;
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("New VM...", ImVec2(120, 0)))
            open_create();

        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(90, 0)))
            running = false;

        if (!g_status.empty())
            ImGui::TextDisabled("%s", g_status.c_str());
        if (!g_error.empty())
            ImGui::TextColored(ImVec4(0.85f, 0.1f, 0.1f, 1.0f), "%s", g_error.c_str());

        if (g_confirm_delete && have_sel) {
            ImGui::OpenPopup("Delete Virtual Machine?");
            g_confirm_delete = false;
        }
        if (ImGui::BeginPopupModal("Delete Virtual Machine?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete \"%s\" and all its disk images?",
                        have_sel ? g_list[size_t(g_selected)].name.c_str() : "?");
            ImGui::Separator();
            if (ImGui::Button("Delete", ImVec2(110, 0))) {
                std::string err;
                if (have_sel && vm::delete_vm(g_list[size_t(g_selected)].name, err)) {
                    refresh();
                    g_status = "Virtual machine deleted.";
                } else {
                    g_error = err;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        draw_create();

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
