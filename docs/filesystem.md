# W25Q128 文件系统与串口维护

更新日期：2026-10-28。当前功能与验证级别统一见 [totalPLAN.md](totalPLAN.md)；驱动、自检和配置记录格式见 [w25q128_storage.md](w25q128_storage.md)。本文描述第一阶段的文件系统接口、维护工具和后续扩展边界。

第一阶段建立 littlefs 文件服务及 USART2 维护通道。现有编译进固件的图片、字形、天气 RAM 缓存保持原来的业务路径；资源文件发布、传感器日志轮转、天气持久缓存及 OTA 执行属于后续阶段。文件可以上传不等于显示模块已能加载该文件。

## 分区与所有权

W25Q128JV 总容量 16 MiB，页编程 256 B，扇区擦除 4096 B。

| 地址范围（包含端点） | 用途 | 大小 |
|---|---|---|
| `0x000000..0x3FFFFF` | OTA 与维护预留 | 4 MiB |
| `0x400000..0xFFDFFF` | littlefs | 3070 个 4 KiB 块，12,574,720 B，约 11.99 MiB |
| `0xFFE000..0xFFFFFF` | 既有 A/B 时制配置 | 8 KiB |

维护自检仍只使用 `0x0000F000..0x0000FFFF`。它位于维护预留区，不能由 OTA 镜像覆盖后再运行破坏性测试。第一阶段不提供任意地址擦写或 OTA 镜像写入命令。正常启动禁止破坏性自检；维护自检和文件系统格式化分别要求明确的 `CONFIRM`。

`storageTask` 独占 Flash 业务事务和 littlefs。调用者通过 `storage_file_request()` 提交含非零 token 的请求；整个请求复制到静态邮箱，不借用调用者的路径或数据缓冲。结果保留到 `storage_file_take_result()` 取走，在此之前拒绝下一文件请求。原配置请求使用独立通道，文件分块、校验分步和后台检查之间继续处理配置。

`storage_get_files_snapshot()` 只复制 RAM，不访问 Flash。`middleware/file_store.*` 不依赖 HAL 或 FreeRTOS，read/program/erase 回调检查分区、对齐与长度；program 不隐式擦除，也不调用不具备原子性的 `w25q128_update()`。底层写擦成功返回前已经完成 WIP 等待和读回检查，sync 按这一完成契约处理。

## 初始化、容量与故障

启动先加载独立配置并发布配置 ready。文件系统挂载或空白检查不要求完成后才显示正常时钟。

- 挂载成功：清理保留的上传临时文件，发布文件容量快照。
- 挂载失败：分段读取文件分区，只有全部为 `0xFF` 才判定未格式化；文件接口在检查中返回未就绪。
- 空白且未格式化：保持正常时钟，容量显示 `N/A`，不因此报告 STORAGE 故障。
- 非空但无法挂载，或读写 IO 失败：报告存储故障。启动绝不自动格式化。
- 格式化仅针对 littlefs 分区，不能擦除配置或 OTA 预留区；上传期间拒绝格式化和维护自检。

文件请求保留 littlefs 错误码；文件故障快照另存该事务首次失败的 W25Q 原始错误，后续清理不会覆盖它。`FS STATUS` 的 `FLASH` 字段输出该原因，0 表示没有记录到底层错误；TIMEOUT、VERIFY、PROTECTED 分别为 -4、-7、-8。`ERRORS` 按原始原因分类，成功重挂载或格式化后清除文件故障的原始原因。仅文件系统本身损坏时，不能沿用此前的 Flash 超时。

文件容量按 littlefs 分配块统计，分母为 3070 块，百分比向上取整。只要有分配块，最低 1%；空文件系统本身也有元数据，所以格式化后可能显示 1%。文件系统空间和“物理上非 `0xFF` 的扇区数量”含义不同。屏幕使用文件系统快照，后台物理扫描不再驱动屏幕的 `SCAN` 字符。

常规写入保留 461 块，即向上取整的 15%。剩余水位为 2609 块，10,686,464 B，约 10.19 MiB。元数据、已打开临时文件和覆盖时同时存在的旧文件也占用这些空间，因此它不是保证可写入等量有效载荷的硬配额。系统不自动删除普通用户文件；空间不足需明确删除文件或由未来业务日志/缓存策略回收。

物理诊断保留“每轮完成后间隔 1 小时”的扫描策略。新增写擦区域均标脏，包括失败但可能已经部分修改的操作；扫描复查这些扇区以更新计数。`storage_set_scan_paused()` 只异步请求在扇区边界暂停物理扫描并保留续扫位置，不确认整个存储服务已静止，也不暂停文件或配置事务。调用它之后仍不能直接挂起 storageTask、关闭 Flash 供电或进入要求总线静止的休眠。未来 PowerMgr 必须增加由存储所有者确认的静止握手，处理在途写擦、sync、总线状态与唤醒恢复；当前尚未接入 Stop 2 协作。维护传输期间扫描让位。当前 SPI1 为 1.25 MHz，空白 16 MiB 全片读取的纯数据下限约 107 秒；后台让出任务不能当成低功耗的证明。整机休眠与扫描能耗仍需板测。

## 路径与文件提交

远端路径为绝对 ASCII 路径，最长 95 B，单个路径分量最长 31 B。拒绝控制字符、非 ASCII、反斜杠、空路径分量、末尾多余 `/`、`.`、`..` 及任意名为 `.upload.tmp` 的分量。根目录 `/` 可查询，不能作为上传、删除、改名目标。路径名不接受中文，但文件内容可以是任意二进制或 UTF-8。

二进制命令允许路径内含空格；文本控制台用空格分隔参数，因此文本文件命令不支持含空格路径，含空格的文件请用 Python 工具维护。

同时只允许一个上传。上传开始声明目标路径、完整长度和标准 CRC32；数据块按绝对 offset 顺序写入保留文件 `/.upload.tmp`，不支持断点续传。普通块 ACK 表示该块处理完成，不能解释为最终目标文件已发布。完成命令检查长度，sync、close 后分步读回核对 CRC32，最后用 littlefs rename 原子替换目标。在 rename 开始前失败或取消时，旧目标保持有效；清理尽力移除临时文件，重启也会清理残留上传。若 IO 失败或复位发生在 rename 提交过程中，恢复后可能保留完整旧文件，也可能已发布完整新文件；未收到成功应答不能保证“仍然只有旧文件”，应重连后核对目标长度与内容。

CRC32 使用 IEEE 反射算法，与 Python `zlib.crc32()` 一致；它检查传输和内容差错，不是 OTA 签名或来源认证。

文件 IO 或损坏故障发生后，当前挂载停止普通操作，也不继续通过它关闭文件或删除临时文件，避免失败写入遗留的缓存触发断言。上传状态在 RAM 中结束，原始故障保留；残留临时文件由成功重新挂载后清理。CRC 或空间不足等未损坏挂载的错误仍按正常取消流程清理。

## 维护工具

仅真实串口操作需要 Python 的 `pyserial`；协议编解码和测试只使用标准库。工具位于 `tools/file_store_cli.py`。连接 USART2（H10，3.3V TTL），默认 115200、8N1。一次只允许一个程序占用该串口，维护时不要同时运行遥测绘图或终端发送。

```powershell
python -m pip install pyserial
python tools/file_store_cli.py --port COM5 status
python tools/file_store_cli.py --port COM5 format --confirm
python tools/file_store_cli.py --port COM5 mkdir /images
python tools/file_store_cli.py --port COM5 upload .\image.bin /images/image.bin
python tools/file_store_cli.py --port COM5 list /images
python tools/file_store_cli.py --port COM5 stat /images/image.bin
python tools/file_store_cli.py --port COM5 download /images/image.bin .\downloaded.bin
python tools/file_store_cli.py --port COM5 rename /images/image.bin /images/new.bin
python tools/file_store_cli.py --port COM5 remove /images/new.bin
python tools/file_store_cli.py --port COM5 selftest --confirm
```

`format --confirm` 删除文件分区全部文件；`selftest --confirm` 擦写上表中的维护自检扇区。二者用途和范围不同。工具不会自动执行它们。

下载先写同目录的 `.part`，成功后发布为目标名称。默认拒绝覆盖已有本地目标或已有 `.part`；确认要覆盖本地目标时加 `--overwrite`。失败时保留 `.part` 便于核查，后续重试前需自行改名或移除它。下载逐帧检查 CRC、offset 和总长度；打印的整文件 CRC32 是本地计算值，没有承诺对正在被其他业务修改的文件提供历史快照。

工具依次发送 `CONSOLE ON`、`FS TRANSFER`，再建立二进制会话。维护期间暂停 JustFloat 遥测；退出后回到文本控制台，若要恢复 JustFloat，可在终端发送 `CONSOLE OFF`。工具不切换 DTR/RTS 来重置设备。

EXIT完成清理后保留7.5秒二进制重放宽限，以接受5秒等待后的重试；只承诺在该窗口内重放最终应答。连续两次运行工具时，CONSOLE ON会按1秒间隔重发并等待进入文本控制台。首次空片的后台检查可能需要数分钟，应等待STATUS为unformatted后再显式format。

每次等待应答 5 秒，最多重发 5 次；每次重发使用相同会话、序号和完整请求。设备已完成请求返回缓存结果，执行中的重复请求返回 BUSY，不重复写入。BUSY 可延后下一次短等待重发，但不能无限延长操作；工具默认每个操作绝对截止 300 秒，可用 `--operation-timeout` 调整。

请求提交被确定拒绝时返回最终错误，只有暂时忙才返回可重试 BUSY。若存储任务未启动，文件请求返回未就绪；EXIT 或空闲退出在确认没有已接受的请求后结束会话，恢复文本控制台。显式 EXIT 的最终错误仍保留重放宽限。已接受的存储请求继续等待安全清理完成。

若断线或截止时间到达而未收到最终应答，工具会明确报告“结果未知”，停止发送新的变更请求。此时写入可能仍在执行，应等待设备完成与会话清理，重新连接后检查文件，不能把超时当成“肯定没写入”并盲目重放。取消同样在存储安全边界处理，不能释放正在执行请求使用的缓冲。正常退出和空闲超时会尝试清理未完成上传；长操作进行中不能仅按 30 秒通信空闲释放请求。

115200 的纯传输上限约 11.5 kB/s，1 MiB 仅数据至少约 91 秒，停等应答与 Flash 操作还会增加耗时。这是维护工具，后续大量资源发布可以另行评估高速串口或 WiFi 文件服务。

## 二进制协议 v1

所有多字节整数均为小端。固定头 36 B，随后 0..256 B payload，最后 4 B 帧 CRC32；帧 CRC 覆盖头的第 4 字节之后（不含 magic）及 payload。Python 格式为 `<4sBBBBIIIIIiHH`。

| 字段 | 类型 | 说明 |
|---|---|---|
| magic | 4 B | ASCII `DCFS` |
| version / op / flags / type | 各 u8 | version=1；请求 flags=0，最终响应=1，执行中 BUSY=2 |
| session / seq | 各 u32 | session 随机非零，seq 单调递增 |
| offset / size / value_crc32 | 各 u32 | 由操作定义 |
| result | i32 | 请求为 0；响应为 0 或负错误码 |
| length / reserved | 各 u16 | payload 长度，reserved=0 |
| payload | 0..256 B | 路径或文件数据，不隐式包含 NUL |
| frame_crc32 | u32 | `zlib.crc32(header[4:] + payload)` |

HELLO 为 op=0、seq=0，成功绑定 session，之后首请求 seq=1。每个最终结果（包括失败）消耗一个序号；BUSY 不消耗。相同序号只有完整请求完全相同时才可重放缓存响应，不能更换 payload 后沿用序号。

| op | 操作 | 请求与最终响应要点 |
|---|---|---|
| 0 | HELLO | 空 payload，绑定会话 |
| 1 | STATUS | 响应 offset=状态、size=总块数、value_crc32=已分配块数；type bit0=上传中、bit1=忙 |
| 2 | LIST | payload=路径，offset=游标；响应 offset=下一游标、type=类型、size=文件长度、payload=名称；空 payload 为结束 |
| 3 | MKDIR | payload=路径 |
| 4 | READ | payload=路径，offset=当前偏移，size=期望读取数（最大 256）；响应 offset=当前块偏移、size=总文件长度、payload=数据 |
| 5 | REMOVE | payload=路径；不递归删除目录 |
| 6 | RENAME | payload=`原路径 + NUL + 新路径` |
| 7 | UPLOAD_BEGIN | payload=目标路径，size=完整长度，value_crc32=完整 CRC32 |
| 8 | UPLOAD_WRITE | payload=文件数据，offset=绝对偏移 |
| 9 | UPLOAD_COMMIT | 空 payload；响应 size/value_crc32=已验证的完整长度/CRC32 |
| 10 | UPLOAD_CANCEL | 空 payload；取消上传，仍保持二进制会话 |
| 11 | FORMAT | payload 为准确的 ASCII `CONFIRM` |
| 12 | STAT | payload=路径；响应 type/size=文件类型/长度 |
| 13 | SELFTEST | payload 为准确的 ASCII `CONFIRM` |
| 14 | EXIT | 清理上传，返回最终应答后回到文本控制台 |

状态值：0 unavailable、1 initializing、2 unformatted、3 ready、4 error。littlefs 文件类型：1 普通文件、2 目录。模块参数/忙/未就绪/CRC/序号/确认错误分别为 -1001 到 -1006；其他 littlefs 或底层错误需结合 STATUS/ERRORS 诊断，不能一律通过格式化处理。

## 内存、耐久性与后续阶段

littlefs 固定为官方 v2.9.3，来源和许可证见 [middleware/littlefs/README.md](../middleware/littlefs/README.md)。块大小 4096，读粒度 16，编程粒度 256，cache 256，lookahead 128，block_cycles 500。读缓存、写缓存、两个文件缓存及 lookahead 共 1152 B；这不是完整模块内存占用，还需计入 littlefs 结构、请求/响应邮箱、位图、串口缓冲及任务栈。动态堆保持 3000 B，文件必须通过静态缓存打开，实际成本由精确构建 map 和板上栈高水位确认。

读取扫描不消耗通常所说的扇区擦写寿命，但占用总线、运行时间和能量。littlefs 提供面向 Flash 的元数据一致性与磨损分散；应用仍应把低频日志批量追加、按大小/日期轮转，设置日志和天气缓存配额，避免每条记录都创建文件或 sync。未来需定义允许丢失的日志时间窗口，不能同时承诺“每条实时落盘”与“完全没有擦写成本”。

后续按资源发布及内部资源回退、日志轮转、天气缓存/WiFi 文件服务、双芯片 OTA 的顺序接入。OTA 前仍需确认真实镜像大小、STM32 bootloader 链接布局、ESP32 自身分区表、签名、低电量禁入、试运行确认和失败回退，4 MiB 预留不能替代这些设计。

## 验证入口与边界

```powershell
python -m unittest discover -s tests -p test_file_store_cli.py -v
python -m unittest discover -s tests -p test_file_store_cli_e2e.py -v
.\tests\run_storage_files_integration_host.ps1
```

第一个测试不打开串口，覆盖编解码、坏帧恢复、丢 ACK 原请求重传、BUSY 与绝对期限、未知提交结果、路径边界、二进制上传/下载、本地文件保护以及退出宽限期后的控制台重连。

第二个测试需要主机 GCC，通过二进制管道连接 Python CLI 与真实 C 端 `file_transfer.c`、`file_store.c` 和 littlefs，Flash 使用 16 MiB NOR 数组模拟；输出位于 `build/file-cli-e2e/`。它覆盖未格式化不自动格式化、任意字节上传覆盖/列目录/下载/改名删除、CRC 错误提交保留旧文件、丢最终 ACK 缓存重放不重复修改和坏帧 CRC 后重试。本测试同样不打开物理串口。

`run_storage_files_integration_host.ps1` 使用真实 `app/storage.c`、配置事务、文件引擎和 littlefs，并以 RTOS 桩与 NOR 数组测试空白启动及已格式化启动；用于核查后台初始化、配置/文件请求配合和故障快照。这与只测试独立文件引擎或 Python/C 传输的覆盖范围不同，仍不提供目标板调度或时序证据。

2026-10-28故障修复后已运行27项独立工具测试和5项跨语言端到端测试。文件引擎通过39个替换/4个格式化断电窗口、10MiB近满盘、统计故障及新增单次写/sync/读回超时恢复；真实storageTask的blank/formatted/diagnostics三场景、四组debug、配置/扫描和时钟服务回归通过。Keil默认黑白与模式7完整构建0错误/0警告，默认Flash预算96560B、静态RAM48128B；完整输入/产物身份见[验证记录](测试数据.md)。这些结果不能代替目标板结论。

目标板还需验证真实 W25Q 写擦与掉电恢复、满盘整理延迟、配置最大等待、UART 实际吞吐、各任务栈裕量、扫描能量和 Stop 2 协作。本阶段无自动烧录或破坏性硬件操作。
