# Keil 编译中间文件清理

工程根目录的 `clean_keil.bat` 是双击入口，核心脚本为 `tools/clean_keil.ps1`。工具使用 Windows 自带的 PowerShell 5.1，无需安装 Python 或其他依赖。

**双击会直接清理，不再要求确认。** 运行结束后，窗口会保留删除数量、释放空间和失败项。先结束正在进行的编译，再运行清理。

## 使用

在文件资源管理器中双击工程根目录的 `clean_keil.bat`，即可清理当前工程的编译中间文件。

在工程根目录打开 PowerShell，运行下面的命令可先查看清理范围。`-Preview` 只统计，不删除文件。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\clean_keil.ps1 -Preview
```

增加 `-Details` 可查看完整文件清单。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\clean_keil.ps1 -Preview -Details
```

命令行实际清理时，去掉 `-Preview`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\clean_keil.ps1
```

指定其他 Keil 工程时，使用 `-Root`。带空格的路径需要加引号。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\clean_keil.ps1 -Root 'D:\OtherProject' -Preview
```

`-ExecutionPolicy Bypass` 仅用于本次 PowerShell 进程，不会修改系统的执行策略。

## 清理范围

工具根据文件类型清理，不根据日期判断文件是否过时。新旧构建中的符合条件的中间文件都可能被清理。

| 处理方式 | 文件 |
| --- | --- |
| 清理 | `.o`、`.obj`、`.crf`、`.d`、`.dep`、`.lst`、`.lnp` |
| 保留固件和调试镜像 | `.axf`、`.hex`、`.bin`、`.elf` |
| 保留构建和测试记录 | `.map`、`.log`、`.htm`、`.html`，以及其他报告和采集数据 |
| 保留构建输入 | 源码、头文件、Keil 工程配置、`.ioc`、`.sct`、`.ld`、`.scf` 等 |

工具读取 `.uvprojx` 或 `.uvproj` 中的 `OutputDirectory` 和 `ListingPath`，识别工程配置的输出位置；也识别工程范围内的 `build`、`Objects`、`Listings`、`Debug` 和 `Release` 输出目录。工程 XML 中 `FilePath` 明确引用的输入文件会保留。

`FilePath` 的输入文件名含有无法解析的宏或通配符时，会保守保留对应扩展名的中间文件并提示。名字出现在额外链接配置文本中的 `.o`、`.obj` 也会保留，因此实际清理数量可能减少。

清理逐文件进行，保留目录结构。`build` 内的固件、实采数据、预览图片和验证记录会保留，整个 `build` 目录不会被删除。

扫描会跳过 `.git`、`.svn`、`.hg`、`node_modules`、`.venv`、`venv`、`env`，以及含有 `pyvenv.cfg` 的虚拟环境。`.vscode`、`.idea`、`.embeddedskills`、`RTE` 和 `DebugConfig` 也会保留。重解析点会被跳过，包括目录链接与联接点。

输出路径超出指定工程根目录，或包含无法解析的宏时，不将其加入清理范围。磁盘根目录和未识别到 Keil 工程的目录会被拒绝。扫描或工程 XML 解析失败时，会停止本次清理；删除过程中发现文件发生变化时，会保留该文件并显示失败原因。

空间统计按成功删除文件的大小合计。退出码可供其他脚本判断结果。

| 退出码 | 含义 |
| --- | --- |
| `0` | 清理或预览完成 |
| `1` | 扫描、工程配置解析失败，或部分文件清理失败 |
| `2` | 输入目录无效，或未找到 Keil 工程 |

清理完成后，下次编译会重新生成这些中间文件，耗时可能增加。保留下来的固件和报告仍对应其原始构建版本。

## 用于其他工程

将下面两个文件按相同的目录结构复制到目标工程根目录。

```text
目标工程/
├── clean_keil.bat
└── tools/
    └── clean_keil.ps1
```

双击入口以所在工程为默认清理范围，也可直接调用 PowerShell 脚本并用 `-Root` 指定工程。

## 验证

运行清理工具的集成测试。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\test_clean_keil.ps1
```

测试使用临时样例目录验证预览、实际删除和保留规则。真实工程可用 `-Preview` 核对候选数量和预计释放空间；统计值会随之后的构建变化。
