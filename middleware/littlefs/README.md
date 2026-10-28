# littlefs 上游来源

本目录导入 [littlefs-project/littlefs](https://github.com/littlefs-project/littlefs) 的 **v2.9.3**。导入文件为 `lfs.c`、`lfs.h`、`lfs_util.c`、`lfs_util.h` 和 `LICENSE.md`，保留上游文件内容与版权声明。许可证为 BSD-3-Clause，完整条件以 [LICENSE.md](LICENSE.md) 为准。

各文件的 SHA-256 记录在 [upstream.json](upstream.json)。该清单记录此次导入内容，不是上游签名或独立认证。版本升级时应同时审查源码差异、来源、清单、移植配置和测试，不能只修改版本字符串。

`tools/fetch_littlefs.py` 是维护者显式使用的获取工具；正常构建不联网、不自动更新第三方源码。重新获取后应核对原清单，不能把新生成的哈希本身当成版本内容未改变的证明。

项目移植与业务策略放在本目录外的 `middleware/file_store.*` 和 `app/storage.*`。固定分区、静态缓存、回调边界及使用方式见 [文件系统说明](../../docs/filesystem.md)。编译选择 `LFS_NO_MALLOC`，文件必须使用提供静态文件缓存的 `lfs_file_opencfg()`；库日志由构建宏关闭，不能从库中直接占用调试串口。

保留本文件和 `upstream.json` 不改变上游四个源码文件。主机模拟测试只能验证所覆盖的逻辑，目标板掉电、电气、擦写时限及整机功耗仍需单独验收。
