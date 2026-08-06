# Copilot Repository Instructions (Low-Risk Text Only)

## Scope and Safety Boundary
- This repository uses low-risk text customizations only.
- Do not assume any hook, MCP, or script-based automation is enabled.
- Treat this file and `.github/instructions/*.instructions.md` as guidance text, not executable policy.

## Repository Purpose
- STM32 embedded firmware template project generated from CubeMX HAL framework.
- Primary development flow: edit and reason in VS Code, then run CMake-based configure/build validation.
- Keil is an optional future extension path for board download, debugger setup, and hardware-specific troubleshooting.

## Source Layout (High Level)
- `Core/`: CubeMX-generated startup/system/peripheral init and main entry.
- `app/`: application logic, scheduling, data handling.
- `device/`: device/register protocol logic (hardware-agnostic as much as possible).
- `platform/`: HAL-bound platform adaptation for SPI/I2C/UART/GPIO/delay.
- `middleware/`: reusable protocol/algorithm/logging modules.
- `docs/`: human-readable standards and process documentation.

## Build Facts
- CMake presets: `Debug`, `Release` in `CMakePresets.json`.
- Generator: Ninja.
- Toolchain file is under `cmake/` (prefer `cmake/gcc-arm-none-eabi.cmake`).
- User module source files are registered in the root `CMakeLists.txt`.
- CubeMX-generated sources and generated build entries live under `cmake/stm32cubemx/CMakeLists.txt` and should normally be updated by regeneration, not ad-hoc edits.
- `build/`, compiler intermediates, caches, and editor user-state files are generated artifacts and should normally stay out of Git via `.gitignore`.
- Vendor/generated assets such as `Drivers/`, `Middlewares/`, `Core/`, startup files, and linker scripts may be tracked or excluded by project policy, but exclusions must remain reproducible through documented source/version/regeneration steps.

## Template Profile (Update Per New Project)
- MCU family/model: update startup file, linker script, and CMSIS device settings.
- Board resources: update GPIO/SPI/I2C/UART mapping and clock assumptions.
- Device set: update `device/` and `platform/` pairing modules.
- Build entry: ensure new user `.c` sources are added into the root `CMakeLists.txt`; keep generated source lists aligned with `cmake/stm32cubemx/CMakeLists.txt` via CubeMX regeneration when practical.
- IDE sync: if `MDK-ARM/` is introduced later, ensure its project groups still include all active user sources.

## Editing Rules
- Prefer edits in `app/`, `device/`, `platform/`, `middleware/`, and the root `CMakeLists.txt`.
- In `Core/` generated files, modify only `USER CODE BEGIN/END` regions unless explicitly requested.
- Keep business logic out of generated sections; call into user modules instead.
- If adding a new user `.c` file, update the root `CMakeLists.txt`; if `MDK-ARM/` is maintained later, sync its project groups as well.
- Treat `cmake/stm32cubemx/CMakeLists.txt`, startup files, linker scripts, and `MDK-ARM/` metadata as regeneration-sensitive assets; only edit them when the task is explicitly about migration, toolchain sync, or chip/board changes.
- When MCU model changes, treat `Core/`, startup, and linker assets as regeneration targets; do not hand-port generated init blocks unless required.

## AI Start Rules
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
- When adding or moving source files, sync `CMakeLists.txt`, include paths, and call entry points. If a Keil project is present, sync its project groups too.
- Do not assume vendor libraries or generated assets can be dropped from version control just because they are large; first confirm whether the repository is optimizing for one-clone reproducibility or a lighter archive, and document any exclusion path in `docs/`.
- For new-engineering or large refactors, propose the split (`device/platform/app` or equivalent) and validation steps first, then make small verifiable edits.
- Prefer small, verifiable edits. Validate each step quickly instead of batching large unverified changes.
- Treat test interfaces such as VOFA, displays, or serial diagnostics as temporary bring-up aids. The formal path should stay single and stable by default.
- Treat repository instructions as context reducers, not as a substitute for build results, datasheets, or validated project docs.

## AI Response Style Rules
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
- Primary validation is configure/build and static inspection.
- Do not assume hardware is connected.
- If `MDK-ARM/` is introduced later, flashing and hardware verification issues can escalate to the Keil/board workflow.

## References
- Project-level human spec: `docs/嵌软开发规范.md`.
- Driver-level human spec: `docs/ic开发规范.md`.