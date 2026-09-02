# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 1.0.x   | :white_check_mark: |

## Security & Anti-Cheat Compliance Model

Corelock is designed from the ground up for esports and online multiplayer gaming compatibility:

- **100% Out-of-Process**: Corelock does not inject dynamic link libraries (DLLs) or allocate memory inside target game address spaces.
- **Zero Memory Inspection**: Corelock does not read or write process memory (`ReadProcessMemory` / `WriteProcessMemory` are never called).
- **Zero API Hooking**: Corelock never tampers with kernel hooks or DirectX/Vulkan API trampolines.
- **Official Win32 APIs**: Thread priority adjustments, affinity masks, and power schemes are configured through standard, documented Microsoft Windows APIs.

Because of this architecture, Corelock poses **zero risk of triggering anti-cheat integrity systems** (such as BattlEye, EasyAntiCheat, Riot Vanguard, Ricochet, or Valve Anti-Cheat).

## Reporting Vulnerabilities

If you discover a potential security flaw or unintended privilege escalation, please do not open a public GitHub issue. Instead, submit a report via GitHub Security Advisories or contact the maintainers directly.
