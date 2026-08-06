---
description: "Use when modifying app/device/platform/middleware sources or their CMake files. Enforces layering boundaries and build synchronization for this STM32 project."
applyTo: "app/**/*.c,app/**/*.h,device/**/*.c,device/**/*.h,platform/**/*.c,platform/**/*.h,middleware/**/*.c,middleware/**/*.h,CMakeLists.txt"
---

# Project Structure Rules

- `app/` owns business flow and runtime behavior.
- `device/` owns register protocol and device semantics.
- `platform/` owns HAL binding and board resource operations.
- `middleware/` owns reusable protocol/algorithm/logging functions.

- Prefer simple, stable lowercase snake_case file names.
- `app/` modules should use business/topic names such as `baro`, `imu`, `tam`, not generic names like `app`.
- `device/` modules should use `type_chip` when the chip name alone is not enough, for example `imu_qmi8658a`, `tam_qmc5883p`, `baro_spl06`.
- `platform/` modules should use `stm32_chip` for STM32-bound adapters, for example `stm32_qmi8658a`, `stm32_qmc5883p`, `stm32_spl06`.
- Header and source base names must match exactly.
- Avoid redundant overlong names such as repeating both layer and platform meaning in one file name unless disambiguation is required.

- Do not call HAL APIs directly from `device/`.
- Do not place register-level manipulation in `app/`.
- Do not place business policy in `platform/`.
- Before adding a new module or editing register-related code, search existing `device/`, `platform/`, `app/`, docs, and build entries to confirm the current repository pattern.
- If register facts, timing assumptions, or build ownership are still unclear after searching, ask or mark them as pending instead of guessing.
- When new source files are added, update `CMakeLists.txt`.
- Remind user that Keil project file grouping may also require manual updates.
- Keep module naming device-agnostic where practical (for example, generic bus/service interfaces in `platform/`, chip-specific logic in `device/`).
- When replacing a sensor/IC, prefer adding a new `device/<chip>.c` plus minimal `platform` adapter changes instead of rewriting `app/` flow.
