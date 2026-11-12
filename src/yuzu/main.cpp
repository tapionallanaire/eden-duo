// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <QApplication>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "startup_checks.h"

#if YUZU_ROOM
#include <cstring>
#include "dedicated_room/yuzu_room.h"
#endif

#ifndef _WIN32
#include <sys/resource.h>
#else
#include <QScreen>
#endif

#if defined(__APPLE__)
#include <climits>
#include <cstdlib>
#include <cstring>
#endif

#include "main_window.h"

// TODO(crueter): This is very annoying, see if there are any other solutions.
static Qt::HighDpiScaleFactorRoundingPolicy GetHighDpiRoundingPolicy() {
#ifdef _WIN32
    // For Windows, we want to avoid scaling artifacts on fractional scaling ratios.
    // This is done by setting the optimal scaling policy for the primary screen.

    // Create a temporary QApplication.
    int temp_argc = 0;
    char** temp_argv = nullptr;
    QApplication temp{temp_argc, temp_argv};

    // Get the current screen geometry.
    const QScreen* primary_screen = QGuiApplication::primaryScreen();
    if (primary_screen == nullptr) {
        return Qt::HighDpiScaleFactorRoundingPolicy::PassThrough;
    }

    const QRect screen_rect = primary_screen->geometry();
    const qreal real_ratio = primary_screen->devicePixelRatio();
    const qreal real_width = std::trunc(screen_rect.width() * real_ratio);
    const qreal real_height = std::trunc(screen_rect.height() * real_ratio);

    // Recommended minimum width and height for proper window fit.
    // Any screen with a lower resolution than this will still have a scale of 1.
    constexpr qreal minimum_width = 1350.0;
    constexpr qreal minimum_height = 900.0;

    const qreal width_ratio = std::max(1.0, real_width / minimum_width);
    const qreal height_ratio = std::max(1.0, real_height / minimum_height);

    // Get the lower of the 2 ratios and truncate, this is the maximum integer scale.
    const qreal max_ratio = std::trunc(std::min(width_ratio, height_ratio));
    return max_ratio > real_ratio ? Qt::HighDpiScaleFactorRoundingPolicy::Round
                                  : Qt::HighDpiScaleFactorRoundingPolicy::Floor;
#else
    // Other OSes should be better than Windows at fractional scaling.
    return Qt::HighDpiScaleFactorRoundingPolicy::PassThrough;
#endif
}

// The NX2 switches are read from the environment (EDEN_NX2_*), which a GUI launch has no way to
// set. nx2_switches.txt holds "NAME=VALUE" lines, one per line, '#' comments. A variable already
// in the real environment always wins, so a launcher script can still override one run; between
// files the later one wins, so the copy next to the executable (shipped with the build) can be
// overridden by %APPDATA%\eden\nx2_switches.txt without editing the one that gets replaced.
static void ParseSwitchFile(const std::filesystem::path& path,
                            std::map<std::string, std::string>& entries) {
    std::ifstream file{path};
    if (!file) {
        return;
    }
    constexpr const char* blank = " \t\r";
    std::string line;
    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(blank));
        line.erase(line.find_last_not_of(blank) + 1);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        name.erase(name.find_last_not_of(blank) + 1);
        value.erase(0, value.find_first_not_of(blank));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!name.empty()) {
            entries.insert_or_assign(std::move(name), std::move(value));
        }
    }
}

static std::filesystem::path GetSwitchExeDirectory(const char* argv0) {
#ifdef _WIN32
    return Common::FS::GetExeDirectory();
#else
    std::error_code ec;
#ifdef __linux__
    if (auto exe = std::filesystem::read_symlink("/proc/self/exe", ec); !ec) {
        return exe.parent_path();
    }
#endif
    const auto exe = std::filesystem::absolute(argv0, ec);
    return ec ? std::filesystem::path{} : exe.parent_path();
#endif
}

static void LoadNx2Switches(const char* argv0) {
    // Later files overwrite earlier entries; the real environment is checked only at the end.
    std::map<std::string, std::string> entries;
    if (const auto exe_dir = GetSwitchExeDirectory(argv0); !exe_dir.empty()) {
        ParseSwitchFile(exe_dir / "nx2_switches.txt", entries);
    }
#ifdef _WIN32
    const auto config_dir = Common::FS::GetAppDataRoamingDirectory() / EDEN_DIR;
#else
    const auto config_dir = Common::FS::GetDataDirectory("XDG_CONFIG_HOME") / EDEN_DIR;
#endif
    ParseSwitchFile(config_dir / "nx2_switches.txt", entries);
    for (const auto& [name, value] : entries) {
        if (std::getenv(name.c_str()) != nullptr) {
            continue;
        }
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }
}

int main(int argc, char* argv[]) {
    LoadNx2Switches(argv[0]);
#if YUZU_ROOM
    bool launch_room = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--room") == 0) {
            launch_room = true;
        }
    }

    if (launch_room) {
        LaunchRoom(argc, argv, true);
        return 0;
    }
#endif

    bool has_broken_vulkan = false;
    bool is_child = false;
    if (CheckEnvVars(&is_child)) {
        return 0;
    }

    if (StartupChecks(argv[0], &has_broken_vulkan,
                      Settings::values.perform_vulkan_check.GetValue())) {
        return 0;
    }

#ifdef YUZU_CRASH_DUMPS
    Breakpad::InstallCrashHandler();
#endif

    // Increases the maximum open file limit.
    // TODO: This should be common to all frontends.
#ifdef _WIN32
    // MSVCRT limits this to 2048 for some inexplicable (and likely arcane) reason,
    // so we have to account for that as well.
#ifdef __MSVCRT__
    _setmaxstdio(2048);
#else
    _setmaxstdio(8192);
#endif // __MSVCRT__
#elif defined(__unix__) || defined(__APPLE__)
    // Raise the open file limit to at least 8192, or the hard limit.
    // Most sane systems should not hit the hard limit here.
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = std::min<rlim_t>(1048576, rl.rlim_max);
        setrlimit(RLIMIT_NOFILE, &rl);
    }
#endif // _WIN32

#if defined(__APPLE__)
    // Convert the relative path to an absolute path before the chdir
    char resolved[PATH_MAX];
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "-input-profile") == 0) {
            ++i;
        } else if (argv[i][0] != '-' && argv[i][0] != '/' && realpath(argv[i], resolved)) {
            argv[i] = strdup(resolved);
        }
    }

    // If you start a bundle (binary) on OSX without the Terminal, the working directory is "/".
    // But since we require the working directory to be the executable path for the location of
    // the user folder in the Qt Frontend, we need to cd into that working directory
    const auto bin_path = Common::FS::GetBundleDirectory() / "..";
    chdir(Common::FS::PathToUTF8String(bin_path).c_str());
#endif

    QtCommon::SetupApplication();

    auto rounding_policy = GetHighDpiRoundingPolicy();
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(rounding_policy);

#ifdef _WIN32
    QApplication::setStyle(QStringLiteral("windowsvista"));
#endif

    QApplication app(argc, argv);

    // Qt changes the locale and causes issues in float conversion using std::to_string() when
    // generating shaders
    setlocale(LC_ALL, "C");

    int result = 0;
    {
        MainWindow main_window{has_broken_vulkan};
        // After settings have been loaded by MainWindow, apply the filter
        main_window.show();

        app.connect(&app, &QGuiApplication::applicationStateChanged, &main_window, &MainWindow::OnAppFocusStateChanged);
        result = app.exec();
    }

    // forcefully tear down core before static destructors run
    QtCommon::Shutdown();
    return result;
}
