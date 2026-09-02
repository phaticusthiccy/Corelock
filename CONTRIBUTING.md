# Contributing to Corelock

Thank you for your interest in contributing to **Corelock: Game Priority & Thread Isolation Optimizer**!

## Code of Conduct
We are committed to providing a welcoming, constructive, and harassment-free environment for all contributors.

## Architectural Guidelines

When submitting pull requests or modifying engine code, please adhere to these core principles:

1. **Zero Anti-Cheat Trigger Guarantee**:
   - Never introduce code injection, DLL loading into external processes, memory reading/writing (`ReadProcessMemory`/`WriteProcessMemory`), or inline hooks.
   - All optimizations must remain strictly **out-of-process** via standard Win32 management APIs.

2. **Least Privilege Principle**:
   - Always open process handles with the minimal required access mask (`PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE`). Never use `PROCESS_ALL_ACCESS`.

3. **Zero-CPU Asynchronous Waits**:
   - Never use busy loops or unconstrained polling while monitoring running games.
   - Utilize Win32 kernel event synchronization (`WaitForMultipleObjects`) and cooperative cancellation tokens (`std::stop_token`).

4. **Fault-Tolerant State Reversion**:
   - Every system or process modification (priority, affinity, power scheme, MMCSS) must have a corresponding rollback handler that executes cleanly upon game termination, optimizer exit, or crash signals.

5. **Modern C++20 Standards**:
   - Use RAII wrappers for all OS resources.
   - Format code cleanly and follow consistent naming conventions.

## Development Workflow

1. Fork the repository and create your feature branch (`git checkout -b feature/amazing-feature`).
2. Build and test with CMake:
   ```cmd
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   ```
3. Commit your changes with concise, descriptive commit messages.
4. Push to your branch and open a Pull Request.
