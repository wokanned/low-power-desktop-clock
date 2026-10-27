# Copilot Repository Instructions (Low-Risk Text Only)

## Scope and Safety Boundary
- This repository uses low-risk text customizations only.
- Do not assume any hook, MCP, or script-based automation is enabled.
- Treat this file and `.github/instructions/*.instructions.md` as guidance text, not executable policy.

## Repository Purpose
- DeskClock is an STM32L471RET6 + FreeRTOS desktop clock with a GDEY029T94-FL03 e-paper display, built on CubeMX HAL.
- Primary development flow: maintain peripheral configuration in CubeMX, edit and review in VS Code, then build, download, and debug through Keil MDK-ARM (ARMCC).
- Use `docs/totalPLAN.md`, section `当前开发状态（2026-10-27）`, as the shared project-status entry. Module documents retain detailed contracts and dated verification records.
- The current software prototype integrates RTC/clock display, ADXL345 telemetry, W25Q128 configuration storage, weather injection, and TIME synchronization over USART2. The onboard ESP32 remains connected to USART1 without an application protocol. ESP32 networking/NTP, environmental and battery sampling, button/alarm behavior, and whole-device low-power operation remain pending.
- The 2026-10-27 weather and TIME integration passed the recorded host checks and a complete default BW Keil build (zero errors/warnings). The image is in `build/time-sync-keil/mode-0/` and has not been flashed. See `docs/测试数据.md` for image identity and verification scope. Do not transfer historical build or board results to the current working tree.

## Source Layout (High Level)
- `Core/`: CubeMX-generated startup/system/peripheral init and main entry.
- `app/`: application logic, scheduling, data handling.
- `device/`: device/register protocol logic (hardware-agnostic as much as possible).
- `platform/`: HAL-bound platform adaptation for SPI/I2C/UART/GPIO/delay.
- `middleware/`: planned location for reusable protocol/algorithm/logging modules; the directory is not currently implemented.
- `docs/`: human-readable standards and process documentation.

## Build Facts
- `MDK-ARM/DeskClock.uvprojx` is the only configured whole-firmware build entry. User sources, include paths, and target-wide compile definitions are maintained there.
- The project currently has no CMake presets, Ninja configuration, or configured GCC firmware build. Add instructions for another firmware build system only when that system actually exists.
- The `tests/run_*_host*.ps1` scripts run host GCC tests. These support software verification but do not replace ARMCC whole-firmware linking, image inspection, download, or board tests. `tests/build_vendor_modes.ps1` instead invokes local Keil builds for selected modes; it does not flash hardware.
- Production defaults are `EPD_VENDOR_DEMO_MODE=0` and `EPD_CLOCK_USE_GRAY4=0`: black-and-white clock/weather pages, with normal clock refresh once per minute. Keep these macros consistent across the target.
- `build/`, compiler intermediates, caches, and editor user-state files are generated artifacts and should normally stay out of Git via `.gitignore`.
- Vendor/generated assets such as `Drivers/`, `Middlewares/`, `Core/`, startup files, and linker scripts may be tracked or excluded by project policy, but exclusions must remain reproducible through documented source/version/regeneration steps.

## Project Synchronization
- MCU or board changes require checking startup, linker memory layout, CMSIS device settings, peripheral mappings, and the `.ioc` file against the actual hardware.
- Keep `device/` and `platform/` module pairs aligned with the selected board resources.
- Add or move user `.c` sources in the Keil project groups and update include paths and call entry points as needed; a file appearing in the editor does not prove it participates in the build.
- After CubeMX regeneration, verify preserved `USER CODE`, FreeRTOS startup calls, interrupt ownership, RTC initialization protection, display pin mappings, and all user sources in Keil.

## Editing Rules
- Prefer business and driver edits in `app/`, `device/`, and `platform/`; use `middleware/` only when adding a reusable capability that belongs there.
- In `Core/` generated files, modify only `USER CODE BEGIN/END` regions unless explicitly requested.
- Keep business logic out of generated sections; call into user modules instead.
- When adding or moving user sources, synchronize `MDK-ARM/DeskClock.uvprojx`, include paths, and startup/task calls as part of that change.
- Treat startup files, linker settings, and `MDK-ARM/` metadata as regeneration-sensitive assets. Keep edits limited to required source integration, toolchain synchronization, or chip/board changes.
- When MCU model changes, treat `Core/`, startup, and linker assets as regeneration targets; do not hand-port generated init blocks unless required.

## AI Start Rules
- Read `AGENTS.md` for repository guidance and `docs/totalPLAN.md` for the current status before treating a module as complete.
- Treat `docs/嵌软开发规范.md`, `docs/ic开发规范.md`, this file, and `.github/instructions/*.instructions.md` as the minimum input set for new engineering and other non-trivial tasks.
- For any new device or sensor integration task, read both `docs/嵌软开发规范.md` and `docs/ic开发规范.md` before searching, planning, or editing.
- For other non-trivial tasks, read `docs/嵌软开发规范.md` first; if the task touches register logic, init sequencing, parsing, recovery, or bus protocol details, also read `docs/ic开发规范.md` before editing.
- Start by locating the correct layer (`app/`, `device/`, `platform/`, `middleware/`, `Core/`) and keep changes inside that boundary.
- Check `docs/` early when information is incomplete. Besides the standards, it may contain device notes, bring-up records, migration checklists, reference material, and test data.
- If the task description omits key facts such as MCU or board, bus resources, target device, bare-metal vs RTOS, blocking limits, low-power needs, interrupt or FIFO requirements, the main build path, or the validation method, search the repository first. If the gap remains, ask the user or mark it as pending confirmation. Do not fill in missing facts from experience.
- For repository hygiene tasks such as `.gitignore`, initial import, or commit-scope cleanup, inspect which files are actually tracked or untracked before editing rules; prefer keeping task-relevant source, config, and docs while excluding build artifacts and unrelated generated outputs.
- Before writing hardware facts such as register defaults, timing waits, protocol extra bits, power-state behavior, or board wiring assumptions, search the repository docs and current code to confirm them.
- If those facts are still unclear after searching, ask the user or explicitly mark them as pending confirmation; do not guess and write them as settled facts.
- For new devices, driver refactors, or new data paths, check runtime robustness before code generation. At minimum, cover startup timing, timeout exit, fault recovery, status reporting, and publish gating after cold start or reinitialization.
- If runtime thresholds, waits, or recovery conditions are not confirmed by datasheets, repository docs, existing code, or validation records, do not present guessed values as facts. Keep the protection path, use an engineering default if needed, and mark the value as pending confirmation.
- When adding or moving source files, sync Keil project groups, include paths, and call entry points. If another firmware build system is introduced later, synchronize its source lists too.
- Do not assume vendor libraries or generated assets can be dropped from version control just because they are large; first confirm whether the repository is optimizing for one-clone reproducibility or a lighter archive, and document any exclusion path in `docs/`.
- For new-engineering or large refactors, propose the split (`device/platform/app` or equivalent) and validation steps first, then make small verifiable edits.
- Prefer small, verifiable edits. Validate each step quickly instead of batching large unverified changes.
- Treat test interfaces such as VOFA, displays, or serial diagnostics as temporary bring-up aids. The formal path should stay single and stable by default.
- Treat repository instructions as context reducers, not as a substitute for build results, datasheets, or validated project docs.

## AI Response Style Rules
- Write user-facing documentation in Chinese; preserve protocol commands, APIs, paths, code, and original logs. Use `docs/README.md` as the document navigation entry and include usable examples in protocol guides.
- When writing Chinese user-facing prose, prefer short sentences and common words.
- Avoid quote-plus-A-B-C phrasing when a plain sentence or a simple list is enough.
- Use fewer colons, dashes, and semicolons unless structure truly needs them.
- Avoid very long compound sentences.
- Avoid stiff words such as `链路`, `回路`, `闭环`, and `耦合` unless they are technically necessary.
- Use basic transitions. Do not overuse repeated negatives, especially repeated `不是`.
- Use short headings and flat lists when they make summaries, structures, or steps easier to scan.
- Sentence rhythm can vary. Do not force artificial parallel phrasing.
- Prefer headings followed directly by useful content. Avoid filler lines between a heading and the real point.
- When editing human-facing specifications or standards, make them read like team rules rather than prompt text.
- When expressing boundaries, prefer direct statements about what is allowed and what is not allowed instead of stacked negations.

## Validation Strategy
- Choose checks for the change: document/reference consistency for text-only edits; relevant host tests, Keil ARMCC build/link, and appropriate board tests for firmware changes.
- Do not assume hardware is connected.
- Report source integration, host verification, exact firmware build, download, and board acceptance separately. Record unexecuted steps explicitly.
- Dated historical logs and images apply only to their identified version and configuration.

## References
- Human-facing documentation index: `docs/README.md`.
- Build identity, host-test records, and historical data index: `docs/测试数据.md`.
- Repository guidance: `AGENTS.md`.
- Current project status and plan: `docs/totalPLAN.md`.
- Project-level human spec: `docs/嵌软开发规范.md`.
- Driver-level human spec: `docs/ic开发规范.md`.
