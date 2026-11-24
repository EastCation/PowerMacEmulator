/*
 * PowerMac Emulator - virtual machine manager implementation.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "vmmanager.h"

#include <utils/imgfile.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace vm {

namespace {

inline fs::path u8p(const std::string& s)
{
#if defined(__cpp_lib_char8_t)
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()),
                                  s.size()));
#else
    return fs::u8path(s);
#endif
}

inline std::string to_u8(const fs::path& p)
{
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path    g_initial_cwd;
fs::path    g_program_dir;
fs::path    g_launcher_path;
fs::path    g_root;
std::string g_last;

std::string g_pending_config;
std::string g_pending_dir;
bool        g_has_pending = false;

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string to_absolute(const std::string& p)
{
    if (p.empty())
        return p;
    fs::path pp = u8p(p);
    if (pp.is_relative())
        pp = g_initial_cwd / pp;
    return to_u8(pp.lexically_normal());
}

// Resolve a user-supplied ROM/CD path. Bare file names are searched in the
// working directory, next to the manager/emulator executables and in the
// usual roms/ folders. Existing paths are made absolute.
std::string resolve_existing_path(const std::string& p)
{
    if (p.empty())
        return p;

    std::error_code ec;
    fs::path pp = u8p(p);
    if (fs::exists(pp, ec))
        return to_u8(fs::absolute(pp).lexically_normal());

    std::string base = to_u8(pp.filename());
    std::vector<fs::path> dirs;
    if (!g_initial_cwd.empty())
        dirs.push_back(g_initial_cwd);
    if (!g_program_dir.empty()) {
        dirs.push_back(g_program_dir);
        dirs.push_back(g_program_dir.parent_path());
        dirs.push_back(g_program_dir.parent_path().parent_path());
    }

    if (pp.is_relative()) {
        for (const auto& d : dirs) {
            fs::path c = d / pp;
            if (fs::exists(c, ec))
                return to_u8(fs::absolute(c).lexically_normal());
        }
    }

    if (!base.empty()) {
        for (const auto& d : dirs) {
            fs::path c = d / base;
            if (fs::exists(c, ec))
                return to_u8(fs::absolute(c).lexically_normal());
            c = d / "roms" / u8p(base);
            if (fs::exists(c, ec))
                return to_u8(fs::absolute(c).lexically_normal());
            c = d / "test-isos" / u8p(base);
            if (fs::exists(c, ec))
                return to_u8(fs::absolute(c).lexically_normal());
        }
    }

    if (pp.is_relative() && !g_initial_cwd.empty())
        pp = g_initial_cwd / pp;
    return to_u8(pp.lexically_normal());
}

std::string resolve_path_list(const std::string& list)
{
    std::string out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(';', start);
        if (end == std::string::npos)
            end = list.size();
        std::string item = trim(list.substr(start, end - start));
        if (!item.empty()) {
            if (!out.empty())
                out += ";";
            out += resolve_existing_path(item);
        }
        start = end + 1;
    }
    return out;
}

bool write_vm_config(const fs::path& folder, const VmSettings& s, std::string& error)
{
    std::ofstream cfg(folder / "powermac.ini");
    if (!cfg) {
        error = "Could not write powermac.ini.";
        return false;
    }

    cfg << "# PowerMac Emulator virtual machine\n\n";
    cfg << "[Machine]\n";
    cfg << "machine=" << s.machine << "\n";
    cfg << "cpu=" << s.cpu << "\n";
    cfg << "rom=" << resolve_existing_path(s.rom) << "\n";
    cfg << "working_dir=.\n";
    cfg << "ram_mb=" << std::clamp(s.ram_mb, 16, 512) << "\n\n";

    cfg << "[Storage]\n";
    cfg << "scsi_hdd=" << s.scsi_hdd << "\n";
    cfg << "scsi_cd=" << resolve_path_list(s.scsi_cd) << "\n";
    cfg << "hdd_bus=" << s.hdd_bus << "\n";
    cfg << "cd_bus=" << s.cd_bus << "\n";
    cfg << "ide_hdd=\n";
    cfg << "ide_cd=\n\n";

    cfg << "[Network]\n";
    cfg << "enabled=" << (s.net_enabled ? 1 : 0) << "\n\n";

    cfg << "[Video]\n";
    cfg << "pci_gpu=" << s.pci_gpu << "\n";
    cfg << "scale=2\n";
    cfg << "fullscreen=0\n\n";

    cfg << "[Input]\n";
    cfg << "auto_grab=1\n";
    cfg << "swap_command_option=0\n";
    cfg << "keyboard=Eng_USA\n\n";

    cfg << "[Emulation]\n";
    cfg << "realtime=1\n";

    return true;
}

int parse_int_or(const std::string& v, int def)
{
    try { return std::stoi(trim(v)); } catch (...) { return def; }
}

std::string read_ini_value(const fs::path& file, const std::string& section,
                           const std::string& key)
{
    std::ifstream in(file);
    if (!in)
        return "";

    std::string line, cur_section, result;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            cur_section = line.substr(1, line.size() - 2);
            std::transform(cur_section.begin(), cur_section.end(), cur_section.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = trim(line.substr(0, eq));
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (cur_section == section && k == key)
            result = trim(line.substr(eq + 1));
    }
    return result;
}

bool valid_vm_name(const std::string& name, std::string& error)
{
    if (name.empty()) {
        error = "Please enter a virtual machine name.";
        return false;
    }
    for (char c : name) {
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) {
            error = "The name must not contain \\ / : * ? \" < > |";
            return false;
        }
    }
    if (name == "." || name == "..") {
        error = "Invalid name.";
        return false;
    }
    return true;
}

} // namespace

void set_program_dir(const std::string& dir)
{
    if (!dir.empty())
        g_program_dir = fs::path(dir);
}

void load_launcher_settings()
{
    g_initial_cwd = fs::current_path();
    g_launcher_path = g_initial_cwd / "emulator.ini";
    g_root = g_initial_cwd / "vms";
    g_last.clear();

    if (!fs::exists(g_launcher_path))
        return;

    std::string root = read_ini_value(g_launcher_path, "vm", "vms_root");
    if (!root.empty()) {
        fs::path rp = u8p(root);
        if (rp.is_relative())
            rp = g_initial_cwd / rp;
        g_root = rp.lexically_normal();
    }
    g_last = read_ini_value(g_launcher_path, "vm", "last_vm");
}

void save_launcher_settings()
{
    if (g_launcher_path.empty())
        g_launcher_path = fs::current_path() / "emulator.ini";

    std::ofstream out(g_launcher_path);
    if (!out)
        return;
    out << "[VM]\n";
    out << "vms_root=" << to_u8(g_root) << "\n";
    out << "last_vm=" << g_last << "\n";
}

std::string root_dir()
{
    return to_u8(g_root);
}

void set_root_dir(const std::string& root)
{
    if (root.empty())
        return;
    fs::path rp = u8p(root);
    if (rp.is_relative())
        rp = g_initial_cwd / rp;
    g_root = rp.lexically_normal();
    save_launcher_settings();
}

std::string last_vm()
{
    return g_last;
}

std::vector<VmEntry> scan()
{
    std::vector<VmEntry> list;
    std::error_code ec;

    if (!fs::exists(g_root, ec) || !fs::is_directory(g_root, ec))
        return list;

    for (auto it = fs::directory_iterator(g_root, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory())
            continue;

        fs::path cfg = it->path() / "powermac.ini";
        if (!fs::exists(cfg))
            continue;

        VmEntry e;
        e.name        = to_u8(it->path().filename());
        e.folder      = to_u8(it->path());
        e.config_path = to_u8(fs::absolute(cfg));
        e.machine     = read_ini_value(cfg, "machine", "machine");
        e.rom         = read_ini_value(cfg, "machine", "rom");
        list.push_back(e);
    }

    std::sort(list.begin(), list.end(),
              [](const VmEntry& a, const VmEntry& b) { return a.name < b.name; });
    return list;
}

bool create_vm(const std::string& name_in, const std::string& machine,
               const std::string& cpu, int ram_mb, int disk_gb,
               const std::string& rom, const std::string& install_cd,
               std::string& error)
{
    std::string name = trim(name_in);
    if (!valid_vm_name(name, error))
        return false;

    std::error_code ec;
    fs::create_directories(g_root, ec);
    if (ec) {
        error = "Could not create VM root directory: " + ec.message();
        return false;
    }

    fs::path folder = g_root / u8p(name);
    if (fs::exists(folder)) {
        error = "A virtual machine with this name already exists.";
        return false;
    }

    fs::create_directories(folder, ec);
    if (ec) {
        error = "Could not create VM folder: " + ec.message();
        return false;
    }

    int gb = std::clamp(disk_gb, 0, 64);
    std::string disk_name;
    if (gb > 0) {
        disk_name = "disk0.vhd";
        uint64_t bytes = uint64_t(gb) * 1024ULL * 1024ULL * 1024ULL;
        if (!vhd_create_dynamic(to_u8(folder / disk_name), bytes)) {
            error = "Could not create the dynamic VHD disk image.";
            fs::remove_all(folder, ec);
            return false;
        }
    }

    VmSettings s;
    s.machine  = machine;
    s.cpu      = cpu.empty() ? "604" : cpu;
    s.ram_mb   = ram_mb;
    s.rom      = rom;
    s.scsi_hdd = (gb > 0 ? disk_name : "");
    s.scsi_cd  = install_cd;
    s.hdd_bus  = "curio";
    s.cd_bus   = "curio";
    s.pci_gpu  = "none";

    if (!write_vm_config(folder, s, error)) {
        fs::remove_all(folder, ec);
        return false;
    }

    return true;
}

bool load_vm_settings(const std::string& name, VmSettings& out, std::string& error)
{
    for (const auto& e : scan()) {
        if (e.name != name)
            continue;

        fs::path cfg = u8p(e.folder) / "powermac.ini";
        out.machine  = read_ini_value(cfg, "machine", "machine");
        out.cpu      = read_ini_value(cfg, "machine", "cpu");
        out.rom      = read_ini_value(cfg, "machine", "rom");
        out.ram_mb   = parse_int_or(read_ini_value(cfg, "machine", "ram_mb"), 128);
        out.scsi_hdd = read_ini_value(cfg, "storage", "scsi_hdd");
        out.scsi_cd  = read_ini_value(cfg, "storage", "scsi_cd");
        out.hdd_bus  = read_ini_value(cfg, "storage", "hdd_bus");
        out.cd_bus   = read_ini_value(cfg, "storage", "cd_bus");
        out.pci_gpu  = read_ini_value(cfg, "video", "pci_gpu");
        out.net_enabled = read_ini_value(cfg, "network", "enabled") != "0";

        if (out.machine != "pm8500" && out.machine != "pm9500" &&
            out.machine != "pm8600" && out.machine != "pm9600" &&
            out.machine != "pmg3dt" && out.machine != "pmg3twr")
            out.machine = "pm8500";
        if (out.cpu != "604" && out.cpu != "604e" && out.cpu != "750")
            out.cpu = "604e";
        if (out.hdd_bus != "mesh")
            out.hdd_bus = "curio";
        if (out.cd_bus != "mesh")
            out.cd_bus = "curio";
        if (out.pci_gpu.empty())
            out.pci_gpu = "none";
        return true;
    }

    error = "Virtual machine not found.";
    return false;
}

bool update_vm(const std::string& name, const VmSettings& settings,
               std::string& error)
{
    for (const auto& e : scan()) {
        if (e.name != name)
            continue;

        VmSettings s = settings;
        if (s.machine != "pm8500" && s.machine != "pm9500" &&
            s.machine != "pm8600" && s.machine != "pm9600" &&
            s.machine != "pmg3dt" && s.machine != "pmg3twr")
            s.machine = "pm8500";
        if (s.cpu != "604" && s.cpu != "604e" && s.cpu != "750")
            s.cpu = "604e";
        if (s.hdd_bus != "mesh")
            s.hdd_bus = "curio";
        if (s.cd_bus != "mesh")
            s.cd_bus = "curio";
        if (s.pci_gpu.empty())
            s.pci_gpu = "none";

        return write_vm_config(fs::path(e.folder), s, error);
    }

    error = "Virtual machine not found.";
    return false;
}

bool delete_vm(const std::string& name, std::string& error)
{
    for (const auto& e : scan()) {
        if (e.name != name)
            continue;

        std::error_code ec;
        fs::remove_all(u8p(e.folder), ec);
        if (ec) {
            error = "Could not delete VM: " + ec.message();
            return false;
        }
        if (g_last == name) {
            g_last.clear();
            save_launcher_settings();
        }
        return true;
    }
    error = "Virtual machine not found.";
    return false;
}

bool open_folder(const std::string& folder)
{
#ifdef _WIN32
    std::wstring wpath = u8p(folder).wstring();
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr,
                                 SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(rc) > 32;
#elif defined(__APPLE__)
    std::string cmd = "open \"" + folder + "\"";
    return std::system(cmd.c_str()) == 0;
#else
    std::string cmd = "xdg-open \"" + folder + "\"";
    return std::system(cmd.c_str()) == 0;
#endif
}

bool launch_vm(const std::string& name, const std::string& emulator_exe,
               std::string& error)
{
    for (const auto& e : scan()) {
        if (e.name != name)
            continue;

        g_last = name;
        save_launcher_settings();

#ifdef _WIN32
        // Use CreateProcess with CREATE_NO_WINDOW so the emulator's console
        // window does not appear when started from the VM manager.
        std::wstring wexe = u8p(emulator_exe).wstring();
        std::wstring wdir = u8p(e.folder).wstring();
        std::wstring cmdline = L"\"" + wexe + L"\" --run";
        std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
        cmd.push_back(0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(wexe.c_str(), cmd.data(), nullptr, nullptr,
                                 FALSE, CREATE_NO_WINDOW, nullptr,
                                 wdir.c_str(), &si, &pi);
        if (!ok) {
            error = "Could not start the emulator executable.";
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
#else
        std::string cmd = "cd \"" + e.folder + "\" && \"" + emulator_exe +
                          "\" --run >/dev/null 2>&1 &";
        if (std::system(cmd.c_str()) != 0) {
            error = "Could not start the emulator executable.";
            return false;
        }
#endif
        return true;
    }

    error = "Virtual machine not found.";
    return false;
}

bool start_vm(const std::string& name)
{
    for (const auto& e : scan()) {
        if (e.name != name)
            continue;
        g_pending_config = e.config_path;
        g_pending_dir    = e.folder;
        g_has_pending    = true;
        g_last           = name;
        save_launcher_settings();
        return true;
    }
    return false;
}

std::string startup_config_path()
{
    if (g_last.empty())
        return "";
    fs::path cfg = g_root / u8p(g_last) / "powermac.ini";
    if (!fs::exists(cfg))
        return "";
    return to_u8(fs::absolute(cfg));
}

bool take_pending_start(std::string& config_path, std::string& work_dir)
{
    if (!g_has_pending)
        return false;
    config_path = g_pending_config;
    work_dir    = g_pending_dir;
    g_has_pending = false;
    return true;
}

} // namespace vm
