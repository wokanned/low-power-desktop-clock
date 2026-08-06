---
description: "Use when editing CubeMX generated files, HAL init, USER CODE blocks, .ioc, startup/linker assets, or MDK-ARM project files."
applyTo: "Core/**/*.c,Core/**/*.h,*.ioc,startup*.s,STM32*.ld,MDK-ARM/**"
---

# CubeMX Generated File Safety Rules

- Treat `Core/` as generated code by default.
- Only edit inside `USER CODE BEGIN/END` blocks unless the user explicitly asks otherwise.
- Put complex logic in `app/`, `device/`, `platform/`, or `middleware/`, then call it from USER CODE.
- Do not make speculative edits to `.ioc`.
- Do not modify Keil project files in `MDK-ARM/` unless task explicitly requires toolchain sync or board-download troubleshooting.
- Before editing `.ioc`, generated init code, or `MDK-ARM/` project files, search the current repo docs and build files to confirm the intended ownership and synchronization path.
- If regeneration risk, board wiring assumptions, or toolchain ownership is still unclear after searching, stop and ask rather than guessing.
- After changing generated files, explicitly mention regeneration risk.
- If MCU family/model changes, prefer re-generating `Core/`, startup, and project metadata from CubeMX, then re-attaching user modules at stable call points.
- Keep a small migration checklist for chip changes: clock tree, IRQ names, peripheral instances, DMA channels, and linker memory map.
