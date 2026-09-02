# Corelock: Game Priority & Thread Isolation Optimizer

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=for-the-badge&logo=cplusplus" alt="C++20" />
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D6.svg?style=for-the-badge&logo=windows" alt="Platform" />
  <img src="https://img.shields.io/badge/Renderer-DirectX%2011%20%2B%20ImGui-7C4DFF.svg?style=for-the-badge" alt="DirectX 11" />
  <img src="https://img.shields.io/badge/Anti--Cheat-100%25%20Safe%20(Out--of--Process)-00E676.svg?style=for-the-badge" alt="Anti-Cheat Safe" />
  <img src="https://img.shields.io/badge/License-MIT-green.svg?style=for-the-badge" alt="License" />
</p>

<p align="center">
  <b>A low-level, modular Windows systems performance framework that optimizes CPU core affinity, thread scheduling priority, and dynamic power states for target games.</b><br>
  Operates completely <b>out-of-process</b> with <b>zero risk of triggering anti-cheat systems</b> (BattlEye, EasyAntiCheat, Vanguard, Ricochet, VAC).
</p>

---

## ⚡ Overview & Architecture

```mermaid
flowchart TD
    subgraph Windows_Kernel [Windows Kernel & Hardware]
        IRQ[Hardware IRQs & Network DPCs] -->|Queued by Default| CPU0[Logical CPU 0]
        PWR[Windows Power Subsystem]
        MMCSS_SVC[Multimedia Class Scheduler Service]
    end

    subgraph Corelock_App [Corelock Optimizer Engine (Out-of-Process)]
        Tracker[Async Process Lifecycle Tracker]
        StateSnap[Fault-Tolerant State Machine]
        GUI[DirectX 11 & ImGui Dashboard]
    end

    subgraph Target_Game [Target Game Process (e.g. CS2 / Valorant)]
        GameThreads[Game Render & Physics Threads]
    end

    Tracker -->|OpenProcess with Least Privilege| Target_Game
    StateSnap -->|1. Capture Baseline Snapshot| Target_Game
    Corelock_App -->|2. Offload away from CPU 0| GameThreads
    GameThreads -->|Assigned Dedicated Execution| CPU_Rest[Logical Cores 1 .. N-1]
    Corelock_App -->|3. Elevate Priority| Target_Game
    Corelock_App -->|4. Activate GUID_MIN_POWER_SAVINGS| PWR
    Corelock_App -->|5. Register MMCSS 'Games' Profile| MMCSS_SVC
    Tracker -->|WaitForMultipleObjects: 0% Idle CPU| Target_Game
    Target_Game -->|On Exit: Immediate Rollback| StateSnap
```

---

## 🎨 Graphical User Interface (DirectX 11 + Dear ImGui)

Corelock includes a desktop dashboard crafted with a **Cyberpunk / Titanium Gamer Aesthetic**:

- **Hardware CPU Topology Visualizer**: An interactive grid displaying all system logical cores (C0, C1, C2, ...). Highlights **Core 0** in radiant amber (`ROLE: OS DPC & Hardware Interrupts Only`) when isolated, and active game simulation cores in neon emerald.
- **One-Click Process Scanner**: Live snapshot enumeration (`CreateToolhelp32Snapshot`) with instant text filtering to pick running games without typing.
- **Dynamic Optimization Controls**:
  - `Isolate Logical CPU 0`: Reserve CPU 0 for OS DPCs, audio buffers, and network card interrupts.
  - `Isolate Physical Core 0`: SMT/HyperThreading-aware detection via `GetLogicalProcessorInformationEx`.
  - `HIGH_PRIORITY_CLASS`: Boost Windows thread dispatching without realtime locks.
  - `Dynamic Power Scheme`: Automatically engages High Performance (`GUID_MIN_POWER_SAVINGS`) during gameplay and reverts upon exit.
  - `MMCSS Integration`: Registers with Multimedia Class Scheduler Service (`L"Games"` profile).
- **Diagnostics Event Console**: Live scrollable event feed capturing timestamps, error codes, and state transitions.
- **Active Telemetry Card**: Real-time inspection of active PIDs, affinity masks, power GUIDs, and anti-cheat compliance.

---

## 🛡️ Anti-Cheat Safety Guarantee

| Anti-Cheat Check | Corelock Implementation | Safety Status |
|---|---|---|
| **DLL Injection** | Does not inject code or load external DLLs into target games | :white_check_mark: **100% Safe** |
| **Memory Access** | Never calls `ReadProcessMemory` or `WriteProcessMemory` | :white_check_mark: **100% Safe** |
| **API Hooking** | Zero tampering with game binaries or DirectX/Vulkan trampolines | :white_check_mark: **100% Safe** |
| **Kernel Drivers** | Zero third-party unsigned kernel drivers loaded | :white_check_mark: **100% Safe** |
| **Win32 Privileges** | Minimum required mask (`PROCESS_SET_INFORMATION \| PROCESS_QUERY_LIMITED_INFORMATION \| SYNCHRONIZE`) | :white_check_mark: **100% Safe** |

Compatible with **BattlEye**, **EasyAntiCheat (EAC)**, **Riot Vanguard**, **Call of Duty Ricochet**, and **Valve Anti-Cheat (VAC)**.

---

## 🔬 Engineering Deep-Dive: Why Isolate CPU 0?

On modern Windows systems, hardware interrupt requests (IRQs) for network interface cards (NICs), storage controllers (NVMe/SATA), GPU drivers, and Deferred Procedure Calls (DPCs) are overwhelmingly handled by **CPU 0**.

When a competitive game's main simulation or render thread executes on CPU 0:
1. Incoming high-frequency network packets or GPU driver interrupts preempt the game thread at the hardware level.
2. The game loop stalls momentarily waiting for the DPC queue to drain.
3. This creates **1% low and 0.1% low frametime spikes (micro-stutters)**, even when average FPS appears high.

**Corelock's Solution:** By masking out Core 0 (or all logical threads belonging to Physical Core 0 on SMT/HyperThreading CPUs), the game runs uninhibited across all remaining cores while the OS handles hardware interrupts seamlessly on Core 0.

---

## 📁 Repository Structure

```
Corelock/
├── .github/
│   ├── workflows/build.yml     # Automated GitHub Actions CI for Windows x64
│   └── ISSUE_TEMPLATE/         # Bug report & feature request templates
├── third_party/
│   └── imgui/                  # Dear ImGui with Win32 & DirectX 11 backends
├── OptimizerCore.hpp           # RAII wrappers, configuration, snapshots, GameOptimizer
├── OptimizerCore.cpp           # Toolhelp32 tracker, zero-CPU async waits, power & affinity logic
├── GuiApp.hpp                  # DirectX 11 + Dear ImGui desktop application header
├── GuiApp.cpp                  # Cyberpunk UI implementation, core visualizer grid, event loop
├── main.cpp                    # Unified entry point (GUI default, headless CLI via --cli)
├── CMakeLists.txt              # CMake C++20 build script linking D3D11, DXGI, Avrt, Powrprof
├── .gitignore                  # Comprehensive gitignore for C++, MSVC, and CMake
├── LICENSE                     # MIT License
├── CONTRIBUTING.md             # Contribution guidelines and coding standards
├── SECURITY.md                 # Anti-cheat compliance and security model
└── README.md                   # Repository documentation
```

---

## 🛠️ Build Instructions

### Prerequisites
- **Operating System**: Windows 10 (Build 1903+) or Windows 11
- **Compiler**: MSVC v142 / v143 (Visual Studio 2019 or 2022) with C++20 support
- **Build System**: CMake 3.20+ or Visual Studio IDE

### Building with CMake
```cmd
# 1. Clone repository
git clone https://github.com/phaticusthiccy/Corelock.git
cd Corelock

# 2. Configure with CMake
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Build executable
cmake --build build --config Release
```
The compiled executable will be located in `build/Release/GameOptimizer.exe`.

---

## 🎮 Launch Options

### 1. Launch DirectX 11 GUI (Default)
Double-click `GameOptimizer.exe` in Windows Explorer or run:
```cmd
.\GameOptimizer.exe
```

Pre-load target game:
```cmd
.\GameOptimizer.exe cs2.exe
```

### 2. Headless CLI Mode (Automation / Low-Spec)
Run with the `--cli` flag:
```cmd
.\GameOptimizer.exe --cli cs2.exe
```

Test immediately with any running desktop app:
```cmd
.\GameOptimizer.exe --cli notepad.exe
```

---

## 📄 License
This project is licensed under the [MIT License](LICENSE).
