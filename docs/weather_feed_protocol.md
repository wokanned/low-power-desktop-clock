# WDATA：STM32 大字天气与预警事务协议

**当前串口操作前提（2026-11-04）**：默认 AUTO 不持续遥测，睡眠时不能保证串口接收。先在上电30秒或晃动后的10秒窗口发送 `POWER HOLD`，收到 `OK POWER HOLD` 后再执行本文示例；结束调试可发送 `POWER AUTO`。仅 `CONSOLE ON` 不会保持清醒。详见[低功耗说明](low_power.md)。

B1～B5修复证据和另外5项已确认待实现需求，见[代码与需求差距](current_code_gaps.md)；构建/主机验证不代表板测通过。

文档核对日期：2026-11-04；协议接入日期：2026-11-02。适用于 `app/weather_feed.*`。这是 USART2 上新增的 RAM 业务；旧 `WEATHER`、`FORECAST` 和整秒 `TIME` 保持独立兼容。`WDATA` 不操作 Flash，也不授予旧 `FORECAST` 的校时许可。实际构建、镜像身份和硬件验证范围以 [验证记录](测试数据.md) 为准。

USART2 继续 115200 / 8N1 / UTF-8。先发送 `CONSOLE ON`，使用实际 CR、LF 或 CRLF 结束每行；每发一行等待该行应答。**一行正文最多223字节，不含行结束符**。所有命令和字段区分大小写；数值为十进制整数，不带单位；无浮点解析。事务只在文字控制台中由调试任务调用，文本应答仍由 debugTask 独占发送。

## 批次与命令

每次发送一个城市的完整快照。最多3天、3条预警、128个非内置字符的26×26单色字形；超过容量应向发送端报错，不能悄悄裁掉预警或正文。新批次应使用新的非零32位 `id`。一批最长空闲60秒，合法记录或完全重复的上一条记录刷新空闲计时；超时、串口接收错误或显式取消丢弃未提交候选，已发布内容继续保留。收集期间 CONSOLE OFF 返回 BUSY，须先提交或 WDATA CANCEL 再退出控制台。

```text
WDATA BEGIN id,city,day_count,alert_count,state,checked,last_success,glyph_count
WDATA NOW id,seq,condition,temp,min,max,rh,wind_direction,speed,bft_min,bft_max,source_time
WDATA NOW id,seq,NA
WDATA METRICS id,seq,feels,uv,aqi_cn,rain_1h,pressure,visibility
WDATA DAY id,seq,index,date,condition,min,max,rain_probability
WDATA ALERT id,seq,index,type,color,severity,urgency,important,issued,effective,expires,id_bytes,replaces_bytes,title_bytes,source_bytes,body_bytes,guide_bytes
WDATA TEXT id,seq,index,field,offset,hex
WDATA GLYPH id,seq,index,codepoint,hex
WDATA COMMIT id,next_seq,crc32
WDATA CANCEL
WDATA STATUS
```

`BEGIN` 成功回复 `OK WDATA BEGIN`；每条数据记录成功回复 `OK WDATA id,seq`；完整校验和发布成功回复 `OK WDATA COMMIT`。`CANCEL` 回复 `OK WDATA CANCEL`。`STATUS` 回复 `OK WDATA STATUS published_id,busy,next_seq`，其中 `next_seq` 仅在 `busy=1` 时表示当前候选下一条记录号。`BEGIN` 不算一条数据记录。

`seq` 从0开始、逐条加1，最大65535。NOW、METRICS各必须且只能一条；DAY索引0起连续；ALERT索引0起连续；TEXT须在对应ALERT元数据之后；GLYPH索引0起连续。NOW、METRICS与其余不同类别记录之间可交错。每个预警每个文本字段独立按字节 offset 顺序追加。

发送超时后可重发**完全相同的上一条** `id,seq` 数据行，固件重发ACK而不追加数据或CRC。其他旧序号、乱序或内容不同的相同序号拒绝。提交ACK丢失后可直接重发同一 `COMMIT`，匹配最近成功的 id、记录数和CRC就重发成功。重发整个最近成功批次也可重ACK；内容冲突的相同 id 回复 `ERR WDATA CONFLICT`，须取消候选并改用新 id。幂等重ACK不重新切页、不重新记录同步、不刷新数据TTL。

## 字段与单位

| 字段 | 取值与含义 |
|---|---|
| city | `0` 深圳，`1` 湛江 |
| state | `0` UNKNOWN尚无查询结论，`1` ACTIVE有预警，`2` NONE查询成功无预警，`3` OFFLINE查询失败，`4` CLEARED已解除 |
| checked / last_success | 北京时间 `YYYY-MM-DDTHH:MM:SS`，未知用 `-`；兼容分钟时间并补 `:00`。成功状态1/2/4必须有checked，固件把last_success设为checked；失败状态3保留同城上次成功时间 |
| condition | 沿用旧天气码0～13；0未知、1晴、2多云、3阴、4小雨、5中雨、6大雨、7雷阵雨、8小雪、9中雪、10大雪、11雨夹雪、12雾、13霾 |
| temp / min / max / feels | 0.1°C，范围−999～999；max必须不小于min |
| rh | 相对湿度整数百分比，0～100；不是API的0～1比例 |
| wind_direction | 旧协议方向码0～8；0无风，非零风速不能配0 |
| speed | 0.1m/s，0～999；保留物理风速语义 |
| bft_min / bft_max | 蒲福风级0～12，min≤max。**独立于speed**，需发送端采用API风级或标准换算，不能把speed直接当风级 |
| source_time | 实况来源更新时间，旧格式 `YYYY-MM-DDTHH:MM`；只显示，不校时 |
| uv | UV指数0.1，0～250，即0～25.0 |
| aqi_cn | 中国标准AQI整数，0～500；不能传其他国家AQI而不转换 |
| rain_1h | 过去1小时降水量0.1mm，0～9999；与未来降水概率不同 |
| pressure | 整数hPa，100～1200 |
| visibility | 0.1km，0～9999 |
| date | `YYYY-MM-DD`，3日范围必须连续，2000～2099年有效公历日期 |
| rain_probability | 降水概率整数百分比0～100，缺测用 `NA` |

METRICS六个数值分别允许 `NA`，对应有效位为0；不能用0表示缺测。NOW缺任一必要实况字段时发送整体 `NA`，预报和数值页仍可独立使用。0℃、0mm、0%均是合法观测值。

完整快照只缓存在RAM。上次成功COMMIT起三小时按单调tick标记整体陈旧，跨tick回绕受支持；RTC校时不重置TTL。陈旧数据的显示样式由页面层处理，不能把旧观测当作新观测。当前仍保留WDATA优先并标STALE，不会自动回退；用户已确认的当天预报回退尚待实现，见[代码与需求差距](current_code_gaps.md)。

## 预警元数据、关系与状态

| 字段 | 取值 |
|---|---|
| type | `0` RAIN，`1` STORM，`2` WIND，`3` TYPHOON，`4` HEAT，`5` FOG，`6` SNOW，`7` FLOOD，`8` OTHER |
| color | `0` UNKNOWN，`1` WHITE，`2` BLUE，`3` YELLOW，`4` ORANGE，`5` RED；缺颜色不能假定蓝色 |
| severity | `0`未知，`1`minor，`2`moderate，`3`severe，`4`extreme |
| urgency | `0`未知，`1`past，`2`future，`3`expected，`4`immediate |
| important | `0/1`，由发送端显式给出是否需要自动查看；本版没有写死橙红色等重要性阈值，不触发响铃 |
| issued / effective / expires | 北京时间完整日期和秒；未知为 `-`。不虚构有效截止时间。兼容分钟输入；effective不能晚于expires |
| id | 官方预警ID，UTF-8最多63字节，非空 |
| replaces | 被本条更新/替换的旧预警ID，最多63字节，可空 |
| title | 官方标题，最多127字节，非空 |
| source | 发布机构，最多95字节，可空 |
| body | 官方正文，最多768字节，非空 |
| guide | 防御指南，最多512字节，可空 |

ALERT最后六个长度是 **UTF-8字节数**，对应 id、replaces、title、source、body、guide，必须与完整TEXT长度一致。TEXT的field依次为0～5；offset是该字段已收字节数；每片1～80字节，用2字符十六进制表示每个字节。不接受嵌入NUL。UTF-8字节可以跨片拆分，但COMMIT必须通过完整UTF-8校验，包括拒绝过长编码、代理区、非法连续字节和超Unicode范围。正文和指南允许CR、LF或CRLF原始段落换行（通过hex发送），页面层保留段落再通用分页；其余ASCII控制字符不接受。

成功的ACTIVE批次必须含1～3条预警，其余状态BEGIN的alert_count必须为0。每次成功查询给出当前完整预警集合；上次存在而这次省去的ID表示已取消/解除。由ACTIVE变成成功NONE时显示CLEARED；重试失败必须使用OFFLINE。同城OFFLINE保留旧预警、文字、字形及last_success，不能把失败当作“无预警”。查询城市改变时不混用另一城市的缓存。

重要预警首次出现、内容更新或升级时生成一次 `attention_revision`；相同ID/内容的轮询不会重发注意事件。replaces匹配旧ID且其余元数据/文本相同时也不重复抢屏。提交与pending生效共用优先级比较：先选severity较大、其次urgency较大条目，完全相同保留较小索引；B5修复的主机证据见[修复状态](current_code_gaps.md)。checked变化和批次id变化不算预警内容更新。发布时已到期的预警不抢屏；未来生效的条目标记pending，defaultTask提供的可信RTC观察到生效后才产生注意事件。RTC到期处理只由debugTask调用 `weather_feed_expire()`，没有跨任务RTC IO；到期事件不清空正文，便于保留来源，元数据标记expired。没有到期字段时保留，直到成功查询明确移除。

## 动态中文字形与资源

固件自带ASCII 32～126和度符号U+00B0的字形。预警标题、机构、正文和指南中的其他Unicode字符必须在同一批次附上GLYPH，缺字COMMIT失败，不能用方块假装正文完整。

GLYPH的codepoint是Unicode码点的十进制整数。字形为26×26、1bit、先行后列连续打包，MSB优先，1黑0白，共676位，85字节；最后一个字节低4位必须0。整字170个hex字符，一行仍在223字节以内。每批最多128个不同非内置码点，不接受重复码点、ASCII/度符号替代字形或代理区码点。文本包含多少次同一字符只需一份字形。发送端应使用许可允许分发/传输的字形来源；本仓库不复制Windows中文字库。

两个完整静态批次各约17KB，包含文字和字形；没有动态分配、没有大栈数组。绘制器通过 `weather_feed_acquire(revision,&lease)` 钉住不可变批次，绘制后立即release，不跨屏幕BUSY等待持有。COMMIT只在短临界区切换已校验候选；下一次BEGIN若准备复用的旧批次仍被钉住，回复BUSY。图像渲染、长文本校验、CRC和大对象初始化都在中断开放时完成。

## CRC与可发送示例

CRC采用标准IEEE CRC32，与Python `zlib.crc32()` 一致：初值0xFFFFFFFF、反射多项式0xEDB88320、末尾异或0xFFFFFFFF。按成功接收顺序累计每条数据记录的**完整ASCII命令行**（含 `WDATA` 前缀、id和seq），每行再加一个字节LF（0x0A）。即使串口用CRLF结束，也只累计LF。BEGIN、COMMIT、CANCEL、STATUS、失败记录和重ACK的重复记录均不累计。

以下示例显示湛江多云实况、六格数值、连续三日预报和成功无预警。每行单独发并等待ACK；CRC `B2DB3C44` 已独立计算，可直接使用：

```text
CONSOLE ON
WDATA BEGIN 1,1,3,0,2,2026-11-02T10:35:00,-,0
WDATA NOW 1,0,2,235,180,270,72,2,50,3,4,2026-11-02T10:30
WDATA METRICS 1,1,251,60,42,0,1012,180
WDATA DAY 1,2,0,2026-11-02,2,180,270,20
WDATA DAY 1,3,1,2026-11-03,4,190,260,80
WDATA DAY 1,4,2,2026-11-04,3,200,280,0
WDATA COMMIT 1,5,B2DB3C44
```

只需查看协议收集状态可发 `WDATA STATUS`。调试中放弃候选可发 `WDATA CANCEL`；后续发送新id即可，不需要清空有效缓存。

## 错误与验证边界

错误分别为 `ERR WDATA LENGTH`、`ARGUMENT`、`BUSY`、`TRANSACTION`、`SEQUENCE`、`CRC`、`INCOMPLETE`、`CONFLICT`、`TIMEOUT`。CRC、缺字段、缺字或非法UTF-8不发布，上一批有效内容保留。修正失败记录时仍使用当前期望seq；若已接受记录内容需更改，应取消后用新批次重发。

`tests/run_weather_feed_host.ps1` 编译真实协议/模型，验证单位、NA、连续日期、事务序号、重试、CRC、UTF-8、字形完整性、预警更新去重/离线保留/到期、生效和tick回绕。该主机测试不证明真实串口吞吐、显示光学效果、任务栈裕量或目标板行为；本协议不宣称API联网、烧录或实屏已完成。
