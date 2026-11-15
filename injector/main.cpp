// Slinky: the loader's terminal UI and a cross-platform injector.
//
// The page flow, wording, pacing and mascot are reproduced from the reference Java
// launcher (Slinky-source/native/launcher.cpp):
//
//   INITIALIZING -> DOWNLOADING UPDATES -> INJECTING -> DONE / ERROR / SKIPPED
//   and, when no target is found, WAITING.
//
// Usage:
//   Slinky.exe                             find Minecraft, inject the library beside it
//   Slinky.exe <pid|name>               choose the target
//   Slinky.exe <pid|name> <library>     inject that library
//   Slinky.exe --lib <library>          choose the library only
//   Slinky.exe --pid <pid>              choose the target only
//   Slinky.exe --list                   list the Java processes
//
// Linux injects with ptrace + the target's own dlopen; Windows uses
// CreateRemoteThread(LoadLibraryW).
#include "inject.hpp"
#include "tui.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace {

// Set by Ctrl+C. The reference keeps a `cancelled` flag for the same purpose: it stops
// the stage holds, ends the startup wait early and turns the timeout page into STOPPED.
std::atomic<bool> g_cancelled{false};

#ifdef _WIN32
BOOL WINAPI console_control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        g_cancelled.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
void install_signal_handler() {
    std::signal(SIGINT, [](int) { g_cancelled.store(true); });
}
#endif

struct Options {
    bool list = false;
    bool help = false;
    bool no_anim = false;
    bool pid_given = false;
    unsigned long pid = 0;
    std::string target;
    std::string library;
    std::string language = "en";
    bool dev = false;   // -dev: also register the unfinished developer modules   // zh or en, written to ~/.slinky/language before injecting
};

// argv[0] arrives in the ANSI code page (GBK here); the console is switched to UTF-8, so the path has to be
// re-encoded or a Chinese directory name shows up as mojibake in the usage line.
static std::string ansi_to_utf8(const char *text) {
#ifdef _WIN32
    if (!text) return {};
    int wide_length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (wide_length <= 0) return text;
    std::wstring wide((size_t)wide_length - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, &wide[0], wide_length);
    int utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) return text;
    std::string out((size_t)utf8_length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &out[0], utf8_length, nullptr, nullptr);
    return out;
#else
    return text ? std::string(text) : std::string();
#endif
}

void usage(const char *program) {
    std::cout << "用法：" << program << " [选项] [目标] [库文件]\n"
              << "\n"
              << "  目标    进程 PID 或进程名（默认：正在运行的 Minecraft）\n"
              << "  库      客户端 DLL 路径（默认：注入器旁边）\n"
              << "\n"
              << "选项：\n"
              << "  -p, --pid <pid>      指定进程 PID\n"
              << "  -n, --name <name>    指定进程名\n"
              << "  -l, --lib <path>     指定要注入的 DLL\n"
              << "      --list           列出 Java 进程后退出\n"
              << "      --no-anim        纯文本输出，不显示角色画面\n"
              << "  -cn, --chinese      客户端界面用中文（默认英文）\n"
              << "  -en, --english      客户端界面用英文（默认）\n"
              << "      --lang <zh|en>  两者任选一\n"
              << "  -dev, --dev         开发者模式（显示未完成的模块）\n"
              << "  -h, --help           显示这段帮助\n";
}

bool parse_options(int argc, char **argv, Options &options, std::string &error) {
    std::vector<std::string> positional;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto value = [&](const char *what) -> bool {
            if (i + 1 >= argc) {
                error = std::string(what) + " 需要一个值";
                return false;
            }
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "--list") {
            options.list = true;
        } else if (arg == "--no-anim") {
            options.no_anim = true;
        } else if (arg == "-dev" || arg == "--dev" || arg == "--developer") {
            options.dev = true;
        } else if (arg == "-cn" || arg == "--chinese") {
            options.language = "zh";
        } else if (arg == "-en" || arg == "--english") {
            options.language = "en";
        } else if (arg == "--lang") {
            if (!value("--lang")) return false;
            if (options.language != "zh" && options.language != "en") {
                error = "--lang needs zh or en";
                return false;
            }
        } else if (arg == "-p" || arg == "--pid") {
            if (!value("--pid")) return false;
            char *end = nullptr;
            options.pid = std::strtoul(argv[++i], &end, 10);
            options.pid_given = true;
            if (!end || *end || !options.pid) {
                error = "无效的 PID '" + std::string(argv[i]) + "'";
                return false;
            }
        } else if (arg == "-n" || arg == "--name") {
            if (!value("--name")) return false;
            options.target = argv[++i];
        } else if (arg == "-l" || arg == "--lib") {
            if (!value("--lib")) return false;
            options.library = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            error = "未知选项 '" + arg + "'";
            return false;
        } else {
            positional.push_back(arg);
        }
    }

    // One positional that names a library is a library; anything else is a target.
    for (const std::string &item : positional) {
        bool looks_like_lib = item.size() > 4 && (item.rfind(".dll") != std::string::npos ||
                                                  item.rfind(".so") != std::string::npos);
        if (looks_like_lib && options.library.empty()) options.library = item;
        else if (options.target.empty()) options.target = item;
        else if (options.library.empty()) options.library = item;
    }
    return true;
}

std::string base_name(const std::string &path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string default_library() {
    std::error_code ec;
    std::filesystem::path self = std::filesystem::current_path(ec);
#ifdef _WIN32
    // The executable's own directory, so the .exe can be run from anywhere.
    char buffer[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH) self = std::filesystem::path(std::string(buffer, n)).parent_path();
    // SlinkyNative.dll is the shipped name; slinky.dll is still accepted so an
    // existing layout keeps working.
    const char *names[] = {"SlinkyNative.dll", "slinky.dll"};
#else
    const char *names[] = {"libslinky.so", "slinky.so"};
#endif
    for (const char *name : names) {
        std::filesystem::path candidate = self / name;
        if (std::filesystem::exists(candidate, ec)) {
            std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate.string() : canonical.string();
        }
    }
    return {};
}

// The client runs inside the game's process, so the language cannot travel as an argument or as an
// environment variable the injector sets. It is written to ~/.slinky/language instead and read once when the
// click GUI is first created, which is where the CJK font is registered too.
// Hidden developer modules (Scaffold among them) are only registered when this marker exists. It lives next
// to the language file so the DLL can find it with the same path helper.
static void write_dev_flag(bool dev) {
#ifdef _WIN32
    wchar_t profile[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir(profile, n);
    dir += L"\\.slinky";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\dev";
    if (!dev) { DeleteFileW(path.c_str()); return; }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, "1", 1, &written, nullptr);
    CloseHandle(file);
#else
    (void)dev;
#endif
}

static void write_language(const std::string &language) {
#ifdef _WIN32
    if (language.empty()) return;
    wchar_t profile[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir(profile, n);
    dir += L"\\.slinky";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\language";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, language.c_str(), (DWORD)language.size(), &written, nullptr);
    CloseHandle(file);
#else
    (void)language;
#endif
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The reference refuses to inject into a process with no threads at all and reports it
// in exactly this wording. A healthy JVM always has one, so this only trips on a
// process that is exiting or was never a JVM.
bool has_thread(unsigned long pid) {
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (!snapshot || snapshot == INVALID_HANDLE_VALUE) return true;   // cannot tell: do not block
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    bool found = false;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == (DWORD)pid) {
                found = true;
                break;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
#else
    // /proc/<pid>/task lists the threads; an empty directory means none.
    std::error_code ec;
    std::filesystem::directory_iterator it("/proc/" + std::to_string(pid) + "/task", ec);
    return !ec && it != std::filesystem::directory_iterator();
#endif
}

void print_list(const std::vector<Injector::Process> &processes) {    // On Windows every Java application is java.exe/javaw.exe, so the name alone cannot
    // tell the game from a launcher; the GAME column is the one that matters.
    std::cout << "   PID  游戏  进程名               窗口\n";
    for (const auto &process : processes) {
        std::string detail = process.command_line.empty() ? process.window_title : process.command_line;
        if (detail.size() > 80) detail = detail.substr(0, 77) + "...";
        std::cout << std::setw(6) << process.pid << "  " << (process.is_minecraft ? " yes " : "  -  ") << "  "
                  << std::setw(20) << std::left << process.name << " " << detail << "\n";
    }
    bool any_game = false;
    for (const auto &process : processes) any_game = any_game || process.is_minecraft;
    if (!any_game)
        std::cout << "\n" << "没有进程加载了 LWJGL：Minecraft 没有运行，或者还在启动中。\n";
}

// ---- startup status, read from the client's own log ----
// A DLL can load into the game and still fail to bootstrap, so the outcome is taken
// from the lines the native client writes to %TEMP%\slinky.log instead of from the
// injection call alone. The markers are the client's own, from its source:
//   hook_installer.cpp      "classhooks: installed, hook stays enabled, first_error=%d"
//                           "classhooks: install called with bad jvm=%p"
//   init.cpp                "Initialization failed: %u"
//   hooks.cpp               "GUI setup failed (no NanoVG context)" / "(font load)"
//   java_natives_frame.cpp  "unload: starting unload thread"
// The reference launcher's "FINAL STATUS:" lines belong to the Java client and are
// never written here, so they are not matched.
enum class StartupStatus { Waiting, Running, AlreadyRunning, Failed, Reloaded };

StartupStatus status_from_line(const std::string &line) {
    // A deliberate unload is a clean end rather than a failure, and it happens after a
    // successful start, so it is reported separately.
    if (line.find("unload: starting unload thread") != std::string::npos) return StartupStatus::Reloaded;
    if (line.find("Initialization failed:") != std::string::npos ||
        line.find("classhooks: install called with bad jvm") != std::string::npos ||
        line.find("GUI setup failed") != std::string::npos)
        return StartupStatus::Failed;
    // Hooks staying enabled with no error is the point where the client is actually
    // live: every line before it can still be rolled back.
    if (line.find("classhooks: installed, hook stays enabled") != std::string::npos) return StartupStatus::Running;
    return StartupStatus::Waiting;
}

// Polls the log for the final status, with the mascot still walking. Returns true only
// when the client confirmed it started. `process_closed` is set when the game exits
// before any status arrived, which the reference reports on its own page.
bool wait_for_startup(Tui::Screen &screen, unsigned long pid, StartupStatus &status, bool &process_closed,
                      std::streamoff start_offset) {
    process_closed = false;
    const std::string path = Injector::NativeLogPathFor(pid);
    // Only lines written after the injection count. The log is shared by every process
    // that ever loaded the client, so an older "classhooks: installed" line from a
    // previous session would otherwise be read as this injection's success.
    std::streamoff offset = start_offset;
    std::string pending;
    status = StartupStatus::Waiting;
    auto began = std::chrono::steady_clock::now();

    auto consume = [&](const std::string &line) {
        StartupStatus next = status_from_line(line);
        if (next == StartupStatus::Waiting) return;
        // A failure is final: a later "installed" line cannot undo it. A success is kept
        // unless a failure already arrived, so the first real verdict wins.
        if (status == StartupStatus::Waiting) status = next;
        else if (next == StartupStatus::Failed) status = next;
    };

    // 250 s, the reference's ceiling.
    std::chrono::steady_clock::time_point settled_at{};
    bool have_settled_at = false;

    while (!g_cancelled.load() && std::chrono::steady_clock::now() - began < std::chrono::seconds(250)) {
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                in.seekg(0, std::ios::end);
                std::streamoff size = in.tellg();
                if (size >= 0 && size < offset) {        // the log was recreated
                    offset = 0;
                    pending.clear();
                    status = StartupStatus::Waiting;
                    have_settled_at = false;
                }
                in.seekg(offset, std::ios::beg);
                std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                offset += (std::streamoff)chunk.size();
                pending += chunk;
                size_t newline;
                while ((newline = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, newline + 1);
                    pending.erase(0, newline + 1);
                    if (line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);   // BOM
                    consume(line);
                }
            }
        }

        if (status != StartupStatus::Waiting) {
            if (!have_settled_at) {
                have_settled_at = true;
                settled_at = std::chrono::steady_clock::now();
            }
            // The client writes several lines around startup, and a failure can follow
            // the success line, so a verdict is held for half a second to let the rest
            // of the burst arrive before the page is drawn.
            if (std::chrono::steady_clock::now() - settled_at >= std::chrono::milliseconds(120)) break;
        } else if (!has_thread(pid)) {
            // The game went away before saying anything: reported on its own page rather
            // than waiting out the full timeout.
            process_closed = true;
            break;
        }
        if (screen.Animated()) screen.RepaintMascot();
        sleep_ms(40);       // poll: 150 ms meant up to 150 ms of dead time before the verdict
    }
    if (!pending.empty()) consume(pending);
    return status == StartupStatus::Running || status == StartupStatus::Reloaded;
}
// Page shown once the log reports an outcome.
void report_status(Tui::Screen &screen, StartupStatus status) {
    screen.NewPage();
    switch (status) {
    case StartupStatus::Running:
    case StartupStatus::Reloaded:
        screen.Section("完成");
        screen.Emit(Tui::green(), "加载成功。");
        break;
    case StartupStatus::AlreadyRunning:
        screen.Section("已跳过");
        screen.Emit(Tui::yellow(), "Slinky 已经在这个游戏里运行了。");
        screen.Emit(Tui::white(), "请先在 Slinky 菜单里卸载，然后重新运行 Slinky。");
        break;
    case StartupStatus::Failed:
        screen.SetFailed(true);
        screen.NewPage();
        screen.Section("错误", Tui::errorRed());
        screen.Emit(Tui::white(), "Slinky 启动未能完成。");
        screen.SupportNote();
        break;
    default:
        break;
    }
    // This page ends the run, so nothing repaints afterwards and the buffer would
    // never reach the console.
    screen.FlushPage();
}

// The DOWNLOADING UPDATES page: the whole "Client - Done" row appears at once, about a
// second after the page opens, exactly as the reference paces it.
void download_updates_page(Tui::Screen &screen) {
    screen.NewPage();
    screen.Section("正在更新");
    screen.FlashStage(120);
    screen.PlainDetail("客户端", "完成", Tui::white());
    screen.HoldStage(0.05);
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    // The interface is UTF-8; without this the Chinese text comes out as mojibake.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    Options options;
    std::string error;
#ifdef _WIN32
    SetConsoleCtrlHandler(console_control, TRUE);
#else
    install_signal_handler();
#endif
    if (!parse_options(argc, argv, options, error)) {
        std::cerr << "Slinky: " << error << "\n\n";
        usage(ansi_to_utf8(argv[0]).c_str());
        return 1;
    }
    if (options.help) {
        usage(ansi_to_utf8(argv[0]).c_str());
        return 0;
    }

    if (options.list) {
        print_list(Injector::ListProcesses());
        return 0;
    }

    const bool interactive = Tui::EnableAnsi() && Tui::StdoutIsTty() && !options.no_anim;
    Tui::Screen screen(interactive);

    // ---- decide where the client library comes from ----
    // Order: an explicit --lib argument, then the copy carried inside this executable,
    // then a file beside it. The bundled copy is what makes Slinky.exe usable on its
    // own; the other two exist so a developer can swap the client without relinking.
    std::string extract_error;
    if (!options.library.empty()) {
        std::error_code ec;
        if (!std::filesystem::exists(options.library, ec)) {
            std::cerr << "Slinky: 找不到库文件：" << options.library << "\n";
            return 1;
        }
    } else if (Injector::HasBundledLibrary()) {
        options.library = Injector::ExtractBundledLibrary(extract_error);
    } else {
        options.library = default_library();
    }

    if (options.library.empty()) {
        std::string line = extract_error.empty()
                               ? std::string("注入器旁边找不到 SlinkyNative.dll。")
                               : extract_error;
        if (interactive) {
            screen.EnterScreen();
            screen.SetFailed(true);
            screen.NewPage();
            screen.Section("错误", Tui::errorRed());
            screen.Emit(Tui::white(), line);
            screen.SupportNote();
            screen.FlushPage();
            screen.WaitForDismissal();
        } else {
            std::cerr << "Slinky: " << line << "\n";
        }
        return 1;
    }

    // Written before the target is resolved: the language choice does not depend on the game being
    // found, and a failed injection should still leave the next run's setting in place.
    write_language(options.language);
    write_dev_flag(options.dev);

    // ---- find the target, before the run takes over the screen ----
    Injector::Process target;
    bool found = false;
    if (options.pid_given) {
        found = Injector::FindTarget(std::to_string(options.pid), target, error);
        if (found) target.pid = options.pid;
    } else if (!options.target.empty()) {
        found = Injector::FindTarget(options.target, target, error);
    } else {
        found = Injector::FindMinecraft(target, error);
    }

    auto fail_page = [&](const char *heading, const std::string &line1, const std::string &line2 = {},
                         bool support = true) {
        if (interactive) {
            screen.EnterScreen();
            screen.SetFailed(true);
            screen.NewPage();
            screen.Section(heading, Tui::errorRed());
            screen.Emit(Tui::white(), line1);
            if (!line2.empty()) screen.Emit(Tui::white(), line2);
            if (support) screen.SupportNote();
            screen.FlushPage();
            screen.WaitForDismissal();
        } else {
            std::cerr << "Slinky: " << line1;
            if (!line2.empty()) std::cerr << " " << line2;
            std::cerr << "\n";
        }
    };

    if (!found) {
        // The reference has two separate outcomes here: nothing found shows WAITING,
        // while more than one candidate means the target is ambiguous and it shows an
        // ERROR that tells the user to close the others. A name or pid was given
        // explicitly, so the reason is reported as-is instead.
        bool ambiguous = options.target.empty() && !options.pid_given && Injector::CountMinecraftCandidates() > 1;
        if (interactive) {
            screen.EnterScreen();
            screen.SetFailed(true);
            screen.NewPage();
            if (ambiguous) {
                screen.Section("错误", Tui::errorRed());
                screen.Emit(Tui::white(), error.c_str());
                screen.Emit(Tui::white(), "请关掉其它 Java 程序后重新运行 Slinky。");
            } else {
                // One line instead of the reference's two: the same fact said twice was
                // the bulk of this page, and the heading already says it is waiting. The
                // underlying reason is kept as a dim detail, since it is the only thing
                // that distinguishes "not started" from "started but no LWJGL yet".
                screen.Section("等待中");
                screen.Emit(Tui::white(), "Minecraft 1.8.9 没有在运行。");
                if (!error.empty()) screen.Detail("原因", error, Tui::dim());
            }
            screen.FlushPage();
            screen.WaitForDismissal();
        } else {
            std::cerr << "Slinky: " << error << "\n" << Injector::EnvironmentHint() << "\n";
        }
        return 1;
    }

    // A name cannot confirm the game: every Java application is java.exe/javaw.exe, so a
    // launcher window would pass a name test. Only LWJGL loaded in the process proves it.
    if (!target.is_minecraft) {
        std::string name = target.name.empty() ? std::to_string(target.pid) : target.name;
        std::string line1 = std::string("这不是 Minecraft：") + name +
                            " has no LWJGL loaded.";
        std::string line2 = "Inject into the game process itself, not a launcher.";
        if (!target.window_title.empty())
            line1 += std::string(" Window title: ") + target.window_title;
        fail_page("错误", line1, line2, false);
        return 1;
    }

    // ---- INITIALIZING flash, then the download page, then INJECTING ----
    if (interactive) {
        screen.EnterScreen();
        screen.NewPage();
        screen.Section("初始化中");
        screen.FlashStage(80);
        download_updates_page(screen);
    } else {
        std::cout << "Injecting into pid " << target.pid << "...\n";
    }

    if (interactive) {
        screen.NewPage();
        screen.Section("注入中");
        screen.Emit(Tui::white(), "正在注入 Minecraft...");
        // The status line has to be readable before the work starts: injection is fast
        // enough that the page would otherwise be replaced almost immediately.
        screen.HoldStage(0.05);
    }

    if (!has_thread(target.pid)) {
        fail_page("错误",
                  "Main thread not found. You are likely injecting into an incompatible Minecraft version.",
                  std::string(), false);
        return 1;
    }

    std::string last_log;
    std::mutex log_mutex;
    std::atomic<bool> done{false};
    std::atomic<bool> injected{false};

    // The log is shared, so the position is taken before the injection starts and only
    // what is appended afterwards is read as this run's outcome.
    std::streamoff log_start = Injector::NativeLogSize(target.pid);

    std::thread worker([&]() {
        bool ok = Injector::Inject(target.pid, options.library, [&](const std::string &line) {
            std::lock_guard<std::mutex> lock(log_mutex);
            last_log = line;
        });
        injected.store(ok);
        done.store(true);
    });

    if (interactive) {
        // Keep the mascot walking while the worker runs.
        while (!done.load()) {
            screen.RepaintMascot();
            sleep_ms(150);
        }
    }
    worker.join();

    if (!injected.load()) {
        std::string detail;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            detail = last_log;
        }
        fail_page("错误", detail.empty() ? std::string("Injection failed.") : detail,
                  Injector::EnvironmentHint());
        return 1;
    }

    // ---- confirm through the client's own log, as the reference does ----
    if (interactive) {
        StartupStatus status = StartupStatus::Waiting;
        bool process_closed = false;
        bool confirmed = wait_for_startup(screen, target.pid, status, process_closed, log_start);

        if (confirmed) {
            // The dog holds a still frame now that the outcome is known, and the
            // countdown rewrites the success line in place for five seconds.
            screen.SetWalking(false);
            screen.NewPage();
            screen.Section("完成");
            screen.Emit(Tui::green(), "加载成功。");
            screen.CountdownClose(1, "加载成功。", true);
            return 0;
        }

        if (process_closed) {
            screen.SetFailed(true);
            screen.NewPage();
            screen.Section("错误", Tui::errorRed());
            screen.Emit(Tui::white(), "选中的游戏进程在启动完成前关闭了。");
            screen.SupportNote();
            screen.FlushPage();
            screen.WaitForDismissal();
            return 1;
        }

        if (status == StartupStatus::AlreadyRunning) {
            report_status(screen, status);
            screen.WaitForDismissal();
            return 2;
        }
        if (status == StartupStatus::Failed) {
            report_status(screen, status);
            screen.WaitForDismissal();
            return 1;
        }

        // No status line ever arrived: the reference shows STOPPED when the user
        // interrupted and TIMEOUT when the wait simply ran out, and keeps the log path
        // on screen so the reason can still be found.
        screen.SetFailed(true);
        screen.NewPage();
        if (g_cancelled.load()) {
            screen.Section("已停止");
            screen.Emit(Tui::yellow(), "已停止等待，启动未确认。");
        } else {
            screen.Section("超时", Tui::yellow());
            screen.Emit(Tui::yellow(), "Slinky 始终没有确认启动完成。");
        }
        screen.Detail("保留日志", Injector::NativeLogPathFor(target.pid), Tui::dim());
        screen.FlushPage();
        screen.WaitForDismissal();
        return 1;
    }

    std::cout << "Injected successfully into pid " << target.pid << ".\n";
    return 0;
}
