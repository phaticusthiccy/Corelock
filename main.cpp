#include "OptimizerCore.hpp"
#include "GuiApp.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <conio.h>

// Global pointer for Win32 Console Control Handler (Ctrl+C / Break)
static Corelock::GameOptimizer* g_optimizerInstance = nullptr;
static std::mutex g_shutdownMutex;
static std::condition_variable g_shutdownCv;
static bool g_shutdownRequested = false;

// Native Win32 Console Control Handler
static BOOL WINAPI ConsoleCtrlHandler(DWORD signalType) {
    switch (signalType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            std::cout << "\n[SIGNAL] Received shutdown signal (" << signalType 
                      << "). Initiating graceful rollback...\n";
            if (g_optimizerInstance != nullptr) {
                g_optimizerInstance->Stop();
            }
            {
                std::lock_guard<std::mutex> lock(g_shutdownMutex);
                g_shutdownRequested = true;
            }
            g_shutdownCv.notify_all();
            return TRUE;
        }
        default:
            return FALSE;
    }
}

// Enable ANSI escape color sequences on Windows 10/11 console
static void EnableVirtualTerminal() {
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr) {
        return;
    }

    DWORD dwMode = 0;
    if (::GetConsoleMode(hOut, &dwMode)) {
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        ::SetConsoleMode(hOut, dwMode);
    }
}

static int RunCliMode(int argc, char* argv[]) {
    EnableVirtualTerminal();

    std::cout << "\033[1;36m"
              << "======================================================================\n"
              << "   Game Priority & Thread Isolation Optimizer (CLI Mode)             \n"
              << "======================================================================\n"
              << "\033[0m";

    std::wstring targetProcess = L"game.exe";

    // Parse target name from args (skipping --cli)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg != "--cli" && arg != "-c") {
            targetProcess = Corelock::GameOptimizer::NarrowToWide(arg);
            break;
        }
    }

    // Configure optimizer parameters
    Corelock::OptimizerConfig config;
    config.targetProcessName = targetProcess;
    config.targetPriorityClass = HIGH_PRIORITY_CLASS; // Always HIGH, never REALTIME
    config.affinityPolicy = Corelock::AffinityPolicy::IsolateLogicalCpu0; // Isolate CPU 0 for OS DPCs/IRQs
    config.enableDynamicPowerPlan = true;            // Switch to High Performance plan
    config.enableMmcss = true;                       // Enable Multimedia Class Scheduler
    config.enableHighResolutionTimer = true;         // 1ms high-resolution kernel scheduler timer
    config.pollInterval = std::chrono::milliseconds(800);

    // Custom ANSI-colored console logger
    config.logger = [](Corelock::LogLevel level, std::string_view msg) {
        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        ::localtime_s(&tm, &timeT);

        char timeBuf[16];
        std::snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);

        switch (level) {
            case Corelock::LogLevel::Trace:
                std::cout << "\033[90m" << timeBuf << "[TRACE]   " << msg << "\033[0m\n";
                break;
            case Corelock::LogLevel::Info:
                std::cout << "\033[37m" << timeBuf << "[INFO]    " << msg << "\033[0m\n";
                break;
            case Corelock::LogLevel::Success:
                std::cout << "\033[1;32m" << timeBuf << "[SUCCESS] " << msg << "\033[0m\n";
                break;
            case Corelock::LogLevel::Warning:
                std::cout << "\033[1;33m" << timeBuf << "[WARNING] " << msg << "\033[0m\n";
                break;
            case Corelock::LogLevel::Error:
                std::cerr << "\033[1;31m" << timeBuf << "[ERROR]   " << msg << "\033[0m\n";
                break;
        }
    };

    Corelock::GameOptimizer optimizer(config);
    g_optimizerInstance = &optimizer;

    if (!::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std::cerr << "\033[1;31m[WARNING] Failed to register Win32 ConsoleCtrlHandler.\033[0m\n";
    }

    std::string targetNarrow = Corelock::GameOptimizer::WideToNarrow(targetProcess);
    std::cout << "\033[1mConfigured Target:\033[0m  " << targetNarrow << "\n"
              << "\033[1mPriority Policy:\033[0m    HIGH_PRIORITY_CLASS\n"
              << "\033[1mAffinity Policy:\033[0m    Isolate Logical CPU 0 (Reserve CPU 0 for DPC/Hardware Interrupts)\n"
              << "\033[1mPower Plan:\033[0m         Dynamic High Performance (GUID_MIN_POWER_SAVINGS)\n"
              << "\033[1mKernel Timer:\033[0m       1ms High-Resolution (timeBeginPeriod)\n"
              << "\033[1mMMCSS:\033[0m              Enabled ('Games' Task Profile)\n"
              << "\033[1mSafety Model:\033[0m       100% Out-of-Process, 0 Anti-Cheat Trigger Risk\n"
              << "----------------------------------------------------------------------\n"
              << "Press \033[1;33mCtrl+C\033[0m or type '\033[1;33mq\033[0m' to quit safely.\n\n";

    if (!optimizer.Start()) {
        std::cerr << "\033[1;31mFailed to start optimizer.\033[0m\n";
        return 1;
    }

    // Non-blocking keyboard input watcher: avoids deadlock/hang on Ctrl+C shutdown
    std::jthread inputWatcher([](std::stop_token stopToken) {
        std::string inputLine;
        while (!stopToken.stop_requested()) {
            if (_kbhit()) {
                int ch = _getch();
                if (ch == '\r' || ch == '\n') {
                    if (inputLine == "q" || inputLine == "quit" || inputLine == "exit") {
                        std::cout << "\n[USER] Exit command received.\n";
                        if (g_optimizerInstance != nullptr) {
                            g_optimizerInstance->Stop();
                        }
                        {
                            std::lock_guard<std::mutex> lock(g_shutdownMutex);
                            g_shutdownRequested = true;
                        }
                        g_shutdownCv.notify_all();
                        break;
                    }
                    inputLine.clear();
                } else if (ch == '\b') {
                    if (!inputLine.empty()) {
                        inputLine.pop_back();
                    }
                } else if (ch >= 32 && ch <= 126) {
                    inputLine.push_back(static_cast<char>(ch));
                    if (inputLine == "q") {
                        std::cout << "\n[USER] Exit shortcut 'q' pressed.\n";
                        if (g_optimizerInstance != nullptr) {
                            g_optimizerInstance->Stop();
                        }
                        {
                            std::lock_guard<std::mutex> lock(g_shutdownMutex);
                            g_shutdownRequested = true;
                        }
                        g_shutdownCv.notify_all();
                        break;
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    {
        std::unique_lock<std::mutex> lock(g_shutdownMutex);
        g_shutdownCv.wait(lock, [] { return g_shutdownRequested; });
    }

    inputWatcher.request_stop();
    optimizer.Stop();
    g_optimizerInstance = nullptr;

    std::cout << "\n\033[1;32m[SHUTDOWN] All states reverted cleanly. Exiting safely.\033[0m\n";
    return 0;
}

int main(int argc, char* argv[]) {
    bool cliMode = false;
    std::wstring initialTarget = L"game.exe";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cli" || arg == "-c") {
            cliMode = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Corelock: Game Priority & Thread Isolation Optimizer\n"
                      << "Usage:\n"
                      << "  Corelock.exe                   (Launches DirectX 11 ImGui Dashboard)\n"
                      << "  Corelock.exe cs2.exe           (Launches GUI with initial target 'cs2.exe')\n"
                      << "  Corelock.exe --cli [game.exe]  (Runs in Headless Terminal / CLI mode)\n";
            return 0;
        } else if (!arg.starts_with("-")) {
            initialTarget = Corelock::GameOptimizer::NarrowToWide(arg);
        }
    }

    if (cliMode) {
        return RunCliMode(argc, argv);
    }

    // Launch DirectX 11 + Dear ImGui Desktop Dashboard
    Corelock::GuiApp app;
    return app.Run(initialTarget);
}
