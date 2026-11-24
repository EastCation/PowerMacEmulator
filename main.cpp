/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team
          (See CREDITS.MD for more details)

(You may also contact divingkxt or powermax2286 on Discord)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// The main runfile - main.cpp
// This is where the magic begins

#include <core/hostevents.h>
#include <core/timermanager.h>
#include <cpu/ppc/ppcdisasm.h>
#include <cpu/ppc/ppcemu.h>
#include <cpu/ppc/ppcmmu.h>
#include <debugger/debugger.h>
#include <devices/common/ofnvram.h>
#include <devices/common/viacuda.h>
#include <machines/machinebase.h>
#include <machines/machinefactory.h>
#include <utils/profiler.h>
#include <utils/imgfile.h>
#include <devices/storage/cdimage.h>
#include <devices/storage/cdaudioplayer.h>
#include <frontend.h>
#include <frontend/runtime.h>
#include <main.h>

#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <optional>
#include <vector>
#include <thread>
#include <filesystem>
#include <CLI11.hpp>
#include <SDL.h>
#include <loguru.hpp>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
using namespace std;

extern bool g_auto_grab_mouse;
extern bool g_swap_command_option;

static void sigint_handler(int signum) {
    power_off(po_signal_interrupt);
}

static void sigabrt_handler(int signum) {
    LOG_F(INFO, "Shutting down...");

    delete gMachineObj.release();
    cleanup();
}

static string appDescription = string(
    "\nDingusPPC - Alpha 1.04c (10/4/2026)          "
    "\nWritten by divingkatae, maximumspatium,      "
    "\njoevt, mihaip, kkaisershot, et. al.          "
    "\n(c) 2018-2026 The DingusPPC Dev Team.        "
    "\nThis is a build intended for testing.        "
    "\nUse at your own discretion.                  "
    "\n"
);

static uint32_t keyboard_id = 0;

static std::optional<std::chrono::year_month_day> parse_calendar_date(const std::string& value)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        return std::nullopt;
    for (size_t i = 0; i < value.size(); i++) {
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9'))
            return std::nullopt;
    }

    std::chrono::year_month_day date{
        std::chrono::year{std::stoi(value.substr(0, 4))},
        std::chrono::month{unsigned(std::stoi(value.substr(5, 2)))},
        std::chrono::day{unsigned(std::stoi(value.substr(8, 2)))}
    };
    if (!date.ok() || int(date.year()) == 0)
        return std::nullopt;
    return date;
}

static std::optional<std::chrono::local_seconds> parse_start_date(std::string value, bool is_deterministic)
{
    if (value.empty()) {
        if (!is_deterministic)
            return std::nullopt;
        // March 24, 2001 was the public release date of Mac OS X.
        value = "2001-03-24";
    }

    auto date = *parse_calendar_date(value);
    int seconds_of_day = 12 * 60 * 60; // Noon
    if (!is_deterministic) {
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        auto local_time = *std::localtime(&now);
        seconds_of_day = local_time.tm_hour * 60 * 60 + local_time.tm_min * 60 + local_time.tm_sec;
    }
    return std::chrono::local_days{date} + std::chrono::seconds{seconds_of_day};
}

/// Check for an existing directory (returns error message if check fails)
class WorkingDirectoryValidator : public CLI::detail::ExistingDirectoryValidator {
public:
    WorkingDirectoryValidator() {
        func_ = [](std::string& filename) {
            std::string result = CLI::ExistingDirectory.operator()(filename);
            if (result.empty()) {
                #ifdef _WIN32
                _chdir(filename.c_str());
                #else
                chdir(filename.c_str());
                #endif
            }
            return result;
        };
    }
};

const WorkingDirectoryValidator WorkingDirectory;

void run_machine(
    std::string machine_str, char *rom_data, size_t rom_size, uint32_t execution_mode
    ,const std::vector<std::string> &env_vars
    ,bool deterministic_interactive
    ,uint32_t profiling_interval_ms
);

// Locate a boot ROM even if the configured path is stale (for example a VM
// moved between folders). Bare file names are also searched next to the
// executable and in the project's roms/ directory.
static std::string resolve_boot_rom_path(const std::string& path) {
    if (path.empty())
        return path;

    std::error_code ec;
    std::filesystem::path p(path);
    if (std::filesystem::exists(p, ec))
        return path;

#ifdef POWERMAC_PROJECT_DIR
    if (p.is_relative()) {
        std::filesystem::path c = std::filesystem::path(POWERMAC_PROJECT_DIR) / p;
        if (std::filesystem::exists(c, ec))
            return c.string();
    }
#endif

    std::string base = p.filename().string();
    if (!base.empty()) {
        std::filesystem::path dir = std::filesystem::current_path(ec);
        for (int i = 0; i < 8; i++) {
            std::filesystem::path c = dir / base;
            if (std::filesystem::exists(c, ec))
                return c.string();
            std::filesystem::path parent = dir.parent_path();
            if (parent == dir)
                break;
            dir = parent;
        }
#ifdef POWERMAC_PROJECT_DIR
        for (const auto& sub : { std::filesystem::path(POWERMAC_PROJECT_DIR) / "roms",
                                 std::filesystem::path(POWERMAC_PROJECT_DIR) }) {
            std::filesystem::path c = sub / base;
            if (std::filesystem::exists(c, ec))
                return c.string();
        }
#endif
    }

    return path;
}

int main(int argc, char** argv) {

    uint32_t execution_mode = interpreter;

    CLI::App app(appDescription);
    app.allow_extras();

    app.set_help_all_flag("--help-all", "Print this help message, help for subcommands, and exit");

    bool debugger_skip = true;
    bool debugger_enter = false;
    bool deterministic_interactive = false;
    string deterministic_mode = "strict";
    string start_date;
    string keyboard_string = "Eng_USA";

    const std::map<std::string, int> kbd_map{
        {"Eng_USA", 0}, {"Eng_GBR", 1}, {"Fra_FRA", 10}, {"Deu_DEU", 20},
        {"Ita_ITA", 30},{"Spa_ESP", 40}, {"Jpn_JPN", 80},
    };

    string bootrom_path("bootrom.bin");
    string working_directory_path(".");

    auto emu = app.add_subcommand("", "Emulation");
    auto execution_mode_group = emu->add_option_group("execution mode")
        ->require_option(-1);
    execution_mode_group->add_flag("-r,--run", debugger_skip,
        "Run the emulator immediately (skip enterring the built-in debugger)");
    execution_mode_group->add_flag("-d,--debugger", debugger_enter,
        "Enter the built-in debugger");
    emu->add_option("-k,--keyboard", keyboard_string, "Specify keyboard ID");
    emu->add_option("-w,--workingdir", working_directory_path, "Specifies working directory")
        ->check(WorkingDirectory)->capture_default_str();
    auto bootrom_opt = emu->add_option("-b,--bootrom", bootrom_path, "Specifies BootROM path")
        ->check(CLI::ExistingFile)->capture_default_str();
    emu->add_flag("--realtime", g_realtime,
        "Advance guest timers using the host clock");
    auto deterministic_opt = emu->add_flag("--deterministic", is_deterministic,
        "Use deterministic execution");
    emu->add_option("--start-date", start_date,
        "Set the guest RTC start date (YYYY-MM-DD), keeping the current local time; "
        "deterministic mode defaults to 2001-03-24 at noon")
        ->check(CLI::Validator([](std::string& value) {
            return parse_calendar_date(value) ? std::string() :
                std::string("Expected a valid date in YYYY-MM-DD format");
        }, "YYYY-MM-DD"));
    emu->add_option("--deterministic-mode", deterministic_mode,
        "Select deterministic features (strict or interactive)")
        ->needs(deterministic_opt)
        ->check(CLI::IsMember({"strict", "interactive"}));

    bool              log_to_stderr = false;
    loguru::Verbosity log_verbosity = loguru::Verbosity_INFO;
    bool              log_no_uptime = false;
    emu->add_flag("--log-to-stderr", log_to_stderr,
        "Send internal logging to stderr (instead of dingusppc.log)");
    emu->add_option("--log-verbosity", log_verbosity,
        "Adjust logging verbosity (default is 0 a.k.a. INFO)")
        ->check(CLI::Number);
    emu->add_flag("--log-no-uptime", log_no_uptime,
        "Disable the uptime preamble of logged messages");

    std::vector<std::string> env_vars;
    emu->add_option("--setenv", env_vars, "Set Open Firmware variables at startup")
        ->take_all();

    uint32_t profiling_interval_ms = 0;
#ifdef CPU_PROFILING
    emu->add_option("--profiling-interval-ms", profiling_interval_ms,
        "Specifies periodic interval (in ms) at which to output CPU profiling information");
#endif

    string       machine_str;
    CLI::Option* machine_opt = emu->add_option("-m,--machine",
        machine_str, "Specify machine ID");

    std::string config_path = frontend::default_config_path();
    bool        no_config   = false;
    emu->add_option("-c,--config", config_path,
        "Read machine settings from an INI file");
    emu->add_flag("--no-config", no_config,
        "Do not read the configuration file");

    emu->add_flag("--auto-grab-mouse", g_auto_grab_mouse,
        "The guest cursor causes mouse to be grabbed");
    emu->add_flag("--swap-command-option", g_swap_command_option,
        "Swap the Command and Option keys (physical Alt/AltGr becomes Command)");

    auto list_cmd = app.add_subcommand("list",
        "Display available machine configurations and exit");

    string sub_arg;

    list_cmd->add_option("machines", sub_arg, "List supported machines");
    list_cmd->add_option("properties", sub_arg, "List available properties");

    std::string   vhd_file;
    uint64_t      vhd_mb = 2048;
    auto vhd_cmd = app.add_subcommand("createvhd",
        "Create a sparse (dynamic) VHD hard disk image and exit");
    vhd_cmd->add_option("file", vhd_file, "Output .vhd file")->required();
    vhd_cmd->add_option("size_mb", vhd_mb, "Disk size in MiB")->check(CLI::PositiveNumber);

    std::string testvhd_file;
    uint64_t    testvhd_mb = 64;
    auto testvhd_cmd = app.add_subcommand("testvhd",
        "Create and verify a dynamic VHD image, then exit");
    testvhd_cmd->add_option("file", testvhd_file, "Output .vhd file")->required();
    testvhd_cmd->add_option("size_mb", testvhd_mb, "Disk size in MiB")->check(CLI::PositiveNumber);

    std::string testcd_file;
    double      testcd_play = 0.0;
    auto testcd_cmd = app.add_subcommand("testcd",
        "Inspect a CD-DA image (CUE/CCD); optionally play the first audio track");
    testcd_cmd->add_option("file", testcd_file, "CD image descriptor")->required();
    testcd_cmd->add_option("--play", testcd_play, "Play first audio track for N seconds");

    CLI11_PARSE(app, argc, argv);

    if (*vhd_cmd) {
        if (!vhd_create_dynamic(vhd_file, vhd_mb * 1024ULL * 1024ULL)) {
            cerr << "Could not create VHD image: " << vhd_file << endl;
            return 1;
        }
        cout << "Created dynamic VHD: " << vhd_file
             << " (" << vhd_mb << " MiB)" << endl;
        return 0;
    }

    if (*testcd_cmd) {
        loguru::g_stderr_verbosity = loguru::Verbosity_INFO;
        loguru::init(argc, argv);
        CdImage img;
        std::string err;
        if (!img.load(testcd_file, err)) {
            cerr << "CD image error: " << err << endl;
            return 1;
        }
        cout << "Tracks: " << img.tracks().size()
             << ", lead-out LBA: " << img.leadout_lba()
             << ", data file: " << img.data_file() << endl;
        for (const auto& t : img.tracks()) {
            cout << "  track " << t.number << "  "
                 << (t.audio ? "AUDIO" : "DATA ") << "  LBA "
                 << t.start_lba << ".." << (t.start_lba + t.length_lba)
                 << "  file offset " << t.file_offset << endl;
        }
        if (testcd_play > 0.0) {
            for (const auto& t : img.tracks()) {
                if (!t.audio)
                    continue;
                CdAudioPlayer player;
                uint64_t off = 0;
                if (!img.audio_offset(t.start_lba, off)) {
                    cerr << "Could not compute audio offset" << endl;
                    return 1;
                }
                uint64_t end = off + uint64_t(t.length_lba) * t.sector_size;
                if (!player.start(img.data_file(), off, end, t.start_lba)) {
                    cerr << "Could not start CD-DA playback" << endl;
                    return 1;
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(int(testcd_play * 1000)));
                cout << "Played track " << t.number << " to LBA "
                     << player.current_lba() << endl;
                player.stop();
                break;
            }
        }
        return 0;
    }

    if (*testvhd_cmd) {
        uint64_t size = testvhd_mb * 1024ULL * 1024ULL;
        if (!vhd_create_dynamic(testvhd_file, size)) {
            cerr << "Could not create VHD image: " << testvhd_file << endl;
            return 1;
        }
        ImgFile img;
        if (!img.open(testvhd_file)) {
            cerr << "Could not open created VHD." << endl;
            return 1;
        }
        std::vector<uint8_t> pat(512), rd(512);
        std::vector<uint64_t> offs = {
            0,
            2ULL * 1024 * 1024,
            5ULL * 1024 * 1024 + 123 * 512,
            size - 512,
        };
        int idx = 0;
        for (uint64_t off : offs) {
            for (size_t i = 0; i < pat.size(); i++)
                pat[i] = uint8_t((idx * 37 + int(i)) & 0xFF);
            if (img.write(pat.data(), off, pat.size()) != pat.size()) {
                cerr << "VHD write failed at offset " << off << endl;
                return 1;
            }
            idx++;
        }
        img.close();
        // Re-open to make sure the BAT/footer were persisted.
        if (!img.open(testvhd_file)) {
            cerr << "Could not re-open VHD." << endl;
            return 1;
        }
        idx = 0;
        for (uint64_t off : offs) {
            rd.assign(512, 0);
            if (img.read(rd.data(), off, rd.size()) != rd.size()) {
                cerr << "VHD read failed at offset " << off << endl;
                return 1;
            }
            for (size_t i = 0; i < rd.size(); i++) {
                uint8_t want = uint8_t((idx * 37 + int(i)) & 0xFF);
                if (rd[i] != want) {
                    cerr << "VHD verify mismatch at offset " << off
                         << " byte " << i << endl;
                    return 1;
                }
            }
            idx++;
        }
        cout << "VHD verify OK: " << testvhd_file
             << " (" << testvhd_mb << " MiB)" << endl;
        return 0;
    }

    frontend::set_active_config_path(config_path);

    // Load the 86Box-style configuration (powermac.ini by default). Command
    // line options take precedence over values read from the file.
    bool have_config = !no_config && frontend::load_config(config_path);
    if (have_config) {
        frontend::apply_machine_defaults();
        if (bootrom_opt->count() == 0 && !frontend::cfg().rom.empty())
            bootrom_path = frontend::cfg().rom;
        if (working_directory_path == "." && !frontend::cfg().working_dir.empty()) {
            working_directory_path = frontend::cfg().working_dir;
#ifdef _WIN32
            _chdir(working_directory_path.c_str());
#else
            chdir(working_directory_path.c_str());
#endif
        }
        if (!is_deterministic)
            g_realtime = frontend::cfg().realtime;
        if (!g_swap_command_option)
            g_swap_command_option = frontend::cfg().swap_cmd_opt;
        if (!g_auto_grab_mouse)
            g_auto_grab_mouse = frontend::cfg().auto_grab;
        if (keyboard_string == "Eng_USA" && !frontend::cfg().keyboard.empty())
            keyboard_string = frontend::cfg().keyboard;
        frontend::build_properties();
    }

    // Resolve the ROM before the CLI validator checks it. This also repairs
    // stale paths in VMs that were moved between folders.
    if (bootrom_opt->count() == 0)
        bootrom_path = resolve_boot_rom_path(bootrom_path);

    deterministic_interactive = deterministic_mode == "interactive";
    if (g_realtime && is_deterministic && !deterministic_interactive) {
        return app.exit(CLI::ValidationError("--realtime",
            "Use --deterministic-mode=interactive with realtime timing"));
    }

    if (*list_cmd) {
        if (sub_arg == "machines") {
            MachineFactory::list_machines();
        } else if (sub_arg == "properties") {
            MachineFactory::list_properties();
        } else {
            cout << "Unknown list subcommand " << sub_arg << endl;
        }
        return 0;
    }

    if (bootrom_opt->count() == 0) {
        // it was not specified on the command line, so validate the default file name.
        std::string msg = bootrom_opt->get_validator()->operator()(bootrom_path);
        if (!msg.empty()) {
            CLI::ParseError e(msg, CLI::ExitCodes::ValidationError);
            return app.exit(e);
        }
    }

    if (debugger_enter || !debugger_skip) {
        execution_mode = debugger;
    }

    if (auto start_time = parse_start_date(start_date, is_deterministic))
        ViaCuda::set_start_time(*start_time);

    /* initialize logging */
    loguru::g_preamble_date    = false;
    loguru::g_preamble_time    = false;
    loguru::g_preamble_thread  = false;
    loguru::g_preamble_uptime  = !log_no_uptime;

    if (execution_mode == interpreter && !log_to_stderr) {
        loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
        loguru::init(argc, argv);
        loguru::add_file("dingusppc.log", loguru::Append, log_verbosity);
    } else {
        loguru::g_stderr_verbosity = log_verbosity;
        loguru::init(argc, argv);
    }

    bootrom_path = resolve_boot_rom_path(bootrom_path);
    auto rom_data = std::unique_ptr<char[]>(new char[4 * 1024 * 1024]);
    memset(&rom_data[0], 0, static_cast<size_t>(4 * 1024 * 1024));
    size_t rom_size = MachineFactory::read_boot_rom(bootrom_path, &rom_data[0]);
    if (!rom_size) {
        std::string msg = "Could not open the boot ROM:\n\n    " + bootrom_path +
            "\n\nEdit powermac.ini ([Machine] rom=...) or pass -b <file>.";
        frontend::ui_message_loop("PowerMac Emulator", msg);
        return 1;
    }

    string machine_str_from_rom = MachineFactory::machine_name_from_rom(&rom_data[0], rom_size);
    if (machine_str_from_rom.empty()) {
        LOG_F(ERROR, "Could not autodetect machine from ROM.");
    } else {
        LOG_F(INFO, "Machine detected from ROM as: %s", machine_str_from_rom.c_str());
    }
    if (*machine_opt) {
        LOG_F(INFO, "Machine option was passed in: %s", machine_str.c_str());
    } else if (have_config) {
        machine_str = frontend::cfg().machine;
        LOG_F(INFO, "Machine selected from configuration: %s", machine_str.c_str());
    } else {
        machine_str = machine_str_from_rom;
    }
        if (machine_str.empty()) {
        LOG_F(ERROR, "Must specify a machine or provide a supported ROM.");
            return 1;
    }

    // Hook to allow properties to be read from the command-line, regardless
    // of when they are registered.
    MachineFactory::get_setting_value = [&](const std::string& name) -> std::optional<std::string> {
        CLI::App sa;
        sa.allow_extras();

        std::string value;
        sa.add_option("--" + name, value)->expected(0,1);
        try {
            sa.parse(app.remaining_for_passthrough());
        } catch (const CLI::Error& e) {
            ABORT_F("Cannot parse CLI: %s", e.get_name().c_str());
        }

        if (sa.count("--" + name) > 0) {
            return value;
        }

        // Fall back to the configuration file (if one was loaded).
        return frontend::property_override(name);
    };

    frontend::build_properties();
    if (MachineFactory::register_machine_settings(machine_str) < 0) {
        return 1;
    }

    cout << "BootROM path: " << bootrom_path << endl;
    cout << "Execution mode: " << execution_mode << endl;
    if (g_realtime)
        cout << "Guest timers use the host clock." << endl;
    if (is_deterministic) {
        cout << "Using deterministic execution mode; disk, NVRAM, and PRAM changes will not be saved." << endl;
        if (deterministic_interactive) {
            cout << "Mouse and keyboard input enabled; execution will not be fully deterministic." << endl;
        } else {
            cout << "Mouse and keyboard input will be ignored." << endl;
        }
    }

#ifdef POWERMAC_QT_UI
    // Create the Qt application/main window. It will host the SDL display
    // once the machine's video device configures itself.
    frontend_qt_init();
#endif

    if (!init()) {
        LOG_F(ERROR, "Cannot initialize");
        return 1;
    }

    // initialize global profiler object
    gProfilerObj.reset(new Profiler());

    // graceful handling of fatal errors
    loguru::set_fatal_handler([](const loguru::Message& message) {
        // Make sure the reason for the failure is visible (it may have been
        // sent to the logfile only).
        cerr << message.preamble << message.indentation << message.prefix << message.message << endl;
        set_power_off_reason(po_enter_debugger);
        DppcDebugger::get_instance()->enter_debugger();

        // Ensure that NVRAM and other state is persisted before we terminate.
        delete gMachineObj.release();
    });

    // redirect SIGINT to our own handler
    signal(SIGINT, sigint_handler);

    // redirect SIGABRT to our own handler
    signal(SIGABRT, sigabrt_handler);

    keyboard_id = kbd_map.at(keyboard_string);

    while (true) {
        run_machine(
            machine_str,
            &rom_data[0],
            rom_size,
            execution_mode,
            env_vars,
            deterministic_interactive,
            profiling_interval_ms);
        if (power_off_reason == po_restarting) {
            LOG_F(INFO, "Restarting...");

            if (!no_config && frontend::load_config(config_path)) {
                frontend::set_active_config_path(config_path);
                frontend::apply_machine_defaults();
                if (bootrom_opt->count() == 0 && !frontend::cfg().rom.empty()) {
                    bootrom_path = resolve_boot_rom_path(frontend::cfg().rom);
                    memset(&rom_data[0], 0, static_cast<size_t>(4 * 1024 * 1024));
                    rom_size = MachineFactory::read_boot_rom(bootrom_path, &rom_data[0]);
                    if (!rom_size) {
                        std::string msg = "Could not open the boot ROM:\n\n    " +
                            bootrom_path + "\n\nEdit powermac.ini or pass -b <file>.";
                        frontend::ui_message_loop("PowerMac Emulator", msg);
                        break;
                    }
                }
                if (!*machine_opt)
                    machine_str = frontend::cfg().machine;
                if (!is_deterministic)
                    g_realtime = frontend::cfg().realtime;
                g_swap_command_option = frontend::cfg().swap_cmd_opt;
                g_auto_grab_mouse     = frontend::cfg().auto_grab;
                keyboard_string       = frontend::cfg().keyboard;
                auto kbd_it = kbd_map.find(keyboard_string);
                keyboard_id = (kbd_it != kbd_map.end()) ? kbd_it->second : 0;
            }
            frontend::build_properties();
            if (MachineFactory::register_machine_settings(machine_str) < 0)
                break;
            power_on = true;
            continue;
        }
        if (power_off_reason == po_shutting_down) {
            if (execution_mode != debugger) {
                LOG_F(INFO, "Shutdown.");
                break;
            }
            LOG_F(INFO, "Shutdown...");
            power_on = true;
            continue;
        }
        break;
    }

    // if we didn't delete this then delete it now
    delete gMachineObj.release();

    cleanup();

    return 0;
}

void run_machine(std::string machine_str, char* rom_data,
    size_t rom_size,
    uint32_t execution_mode,
    const std::vector<std::string> &env_vars,
    bool deterministic_interactive,
    uint32_t
#ifdef CPU_PROFILING
     profiling_interval_ms
#endif
) {
    if (MachineFactory::create_machine_for_id(machine_str, rom_data, rom_size) < 0) {
        return;
    }

    if (!env_vars.empty()) {
        OfConfigUtils ofnvram;
        if (!ofnvram.init()) {
            for (const auto &env_var : env_vars) {
                auto pos = env_var.find('=');
                if (pos != std::string::npos) {
                    std::string name = env_var.substr(0, pos);
                    std::string value = env_var.substr(pos + 1);
                    if (ofnvram.setenv(name, value)) {
                        LOG_F(INFO, "Set Open Firmware variable %s to %s", name.c_str(), value.c_str());
                    } else {
                        LOG_F(WARNING, "Cannot set Open Firmware variable %s to %s", name.c_str(), value.c_str());
                    }
                } else {
                    LOG_F(WARNING, "Invalid format for --setenv: %s", env_var.c_str());
                }
            }
        } else {
            LOG_F(WARNING, "Cannot initialize NVRAM wrapper, will not set Open Firmware variables");
        }
    }

    static TimerInfo deterministic_timer;
    if (is_deterministic && !deterministic_interactive) {
        EventManager::get_instance()->disable_input_handlers();
        // Log the PC and instruction every second to make it easier to validate
        // that execution is the same every time.
        TimerManager::get_instance()->add_cyclic_timer(deterministic_timer, MSECS_TO_NSECS(1000), [](uint64_t, uint64_t) {
            // We may be running after an exceptions or RFI changed the address
            // translation context. In those cases the PC address cannot be
            // translated (it would either be stale or invalid).
            if (exec_flags & (EXEF_EXCEPTION | EXEF_RFI)) {
                LOG_F(INFO, "TS=%016llu PC=0x%08x transition pending to PC=0x%08x",
                      get_virt_time_ns(), ppc_state.pc, ppc_next_instruction_address);
                return;
            }

            PPCDisasmContext ctx;
            ctx.instr_code = ppc_read_instruction(mmu_translate_imem(ppc_state.pc));
            ctx.instr_addr = ppc_state.pc;
            ctx.simplified = false;
            auto op_name = disassemble_single(&ctx);
            LOG_F(INFO, "TS=%016llu PC=0x%08x executing %s", get_virt_time_ns(), ppc_state.pc, op_name.c_str());
        });
    }

    // Optional periodic guest PC trace for diagnosing hangs. Enable by setting
    // the POWERMAC_PC_LOG environment variable.
    static TimerInfo pc_log_timer;
    const bool pc_log_enabled = std::getenv("POWERMAC_PC_LOG") != nullptr;
    if (pc_log_enabled) {
        TimerManager::get_instance()->add_cyclic_timer(pc_log_timer, MSECS_TO_NSECS(500), [](uint64_t, uint64_t) {
            if (exec_flags & (EXEF_EXCEPTION | EXEF_RFI)) {
                LOG_F(INFO, "PCTRACE pending PC=0x%08x next=0x%08x LR=0x%08x SRR0=0x%08x MSR=0x%08x",
                      ppc_state.pc, ppc_next_instruction_address,
                      ppc_state.spr[SPR::LR], ppc_state.spr[SPR::SRR0], ppc_state.msr);
                return;
            }
            PPCDisasmContext ctx;
            ctx.instr_code = ppc_read_instruction(mmu_translate_imem(ppc_state.pc));
            ctx.instr_addr = ppc_state.pc;
            ctx.simplified = false;
            auto op_name = disassemble_single(&ctx);
            LOG_F(INFO, "PCTRACE PC=0x%08x LR=0x%08x SRR0=0x%08x MSR=0x%08x SP=0x%08x %s",
                  ppc_state.pc, ppc_state.spr[SPR::LR], ppc_state.spr[SPR::SRR0],
                  ppc_state.msr, ppc_state.gpr[1], op_name.c_str());
            if (ppc_state.pc >= 0x000CF400 && ppc_state.pc < 0x000CF700) {
                LOG_F(INFO, "PCTRACE regs r23=0x%08x r25=0x%08x r26=0x%08x r27=0x%08x r28=0x%08x "
                      "m23=0x%08x m25=0x%08x m28=0x%08x m27+26=0x%08x",
                      ppc_state.gpr[23], ppc_state.gpr[25], ppc_state.gpr[26],
                      ppc_state.gpr[27], ppc_state.gpr[28],
                      uint32_t(mem_read_dbg(ppc_state.gpr[23], 4)),
                      uint32_t(mem_read_dbg(ppc_state.gpr[25], 4)),
                      uint32_t(mem_read_dbg(ppc_state.gpr[28], 4)),
                      uint32_t(mem_read_dbg(ppc_state.gpr[27] + ppc_state.gpr[26], 4)));
            }
        });
    }

    EventManager::get_instance()->set_keyboard_locale(keyboard_id);

    // set up system wide event polling using
    // default Macintosh polling rate of 11 ms
    static TimerInfo event_timer;
    TimerManager::get_instance()->add_cyclic_timer(event_timer, MSECS_TO_NSECS(11), [](uint64_t, uint64_t) {
        EventManager::get_instance()->poll_events();
    });

    if (frontend::cfg().boot_cd) {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            SDL_Event ev{};
            ev.type = SDL_KEYDOWN;
            ev.key.state = SDL_PRESSED;
            ev.key.repeat = 0;
            ev.key.keysym.sym = SDLK_c;
            ev.key.keysym.scancode = SDL_SCANCODE_C;
            SDL_PushEvent(&ev);
            std::this_thread::sleep_for(std::chrono::seconds(12));
            ev.type = SDL_KEYUP;
            ev.key.state = SDL_RELEASED;
            SDL_PushEvent(&ev);
        }).detach();
    }

#ifdef CPU_PROFILING
    uint32_t profiling_timer;
    if (profiling_interval_ms > 0) {
        profiling_timer = TimerManager::get_instance()->add_cyclic_timer(MSECS_TO_NSECS(profiling_interval_ms),
            [](uint64_t, uint64_t)
        {
            gProfilerObj->print_profile("PPC_CPU");
        });
    }
#endif

    switch (execution_mode) {
    case interpreter:
        set_power_off_reason(po_starting_up);
        DppcDebugger::get_instance()->enter_debugger();
        break;
    case threaded_int:
        set_power_off_reason(po_starting_up);
        DppcDebugger::get_instance()->enter_debugger();
        break;
    case debugger:
        set_power_off_reason(po_enter_debugger);
        DppcDebugger::get_instance()->enter_debugger();
        break;
    default:
        LOG_F(ERROR, "Invalid EXECUTION MODE");
        return;
    }

    LOG_F(INFO, "Cleaning up...");
    TimerManager::get_instance()->cancel_timer(event_timer);
#ifdef CPU_PROFILING
    if (profiling_interval_ms > 0) {
        TimerManager::get_instance()->cancel_timer(profiling_timer);
    }
#endif
    if (is_deterministic && !deterministic_interactive) {
        TimerManager::get_instance()->cancel_timer(deterministic_timer);
    }
    if (pc_log_enabled) {
        TimerManager::get_instance()->cancel_timer(pc_log_timer);
    }
    TimerManager::get_instance()->cancel_all_timers();
    EventManager::get_instance()->disconnect_handlers();
    delete gMachineObj.release();
}
