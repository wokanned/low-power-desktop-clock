# 和风天气取数与 DeskClock 接入指南

更新及官方资料核对日期：2026-11-06。返回[文档导航](README.md)。本文集中维护帐号准备、API请求、字段转换、费用和发送端注意事项。串口格式以 [WDATA 协议](weather_feed_protocol.md)为准，界面以[大字天气页](weather_pages.md)为准，工程进度和已知差异分别见 [totalPLAN.md](totalPLAN.md) 与 [current_code_gaps.md](current_code_gaps.md)。

## 1. 先分清取数与设备接入

个人开发者可以使用和风天气服务，无需为本项目注册企业帐号。个人与企业的区别见[开发者类型](https://dev.qweather.com/docs/account/developers/)。帐号的实际可用服务、额度和凭据限制仍需在自己的控制台确认。

在本项目中，PC Python 程序负责 HTTPS 取数、转换和串口发送，STM32 接收完整快照并显示。以后可把发送端移到 ESP32。默认城市需求为湛江，可切深圳；两地应各自选定实际坐标。

截至本文核对，固件已有 WDATA、中文点阵、预警分页和 TIME2/TIME?；仓库尚无持续取数的 Python 天气发送器，也没有 ESP32 联网业务。下面的代码是可另存运行的 API 调试示例，未接串口。本文没有使用真实帐号调用 API，也没有证明目标城市的延迟、精度或实屏效果。

## 2. 准备帐号和配置

按[开始使用](https://dev.qweather.com/docs/start/)与[项目和凭据](https://dev.qweather.com/docs/configuration/project-and-key/)完成以下准备。

1. 注册开发者帐号，创建项目，确认所需服务可以访问。
2. 在控制台“设置”复制自己的 API Host。只填主机名，格式类似 `abcxyz.qweatherapi.com`，实际值以控制台为准。
3. 创建 JWT 凭据或 API KEY 凭据。只使用一种认证方式发送单次请求。
4. 选定城市/地点坐标、语言、刷新频率和费用预算。
5. 先在 PC 独立取数，确认返回结构、单位和预警空结果，再接 STM32。

建议把以下配置交给使用者填写，不要把某位开发者的帐号写进共享代码。

| 配置 | 内容 |
| --- | --- |
| API Host | 当前帐号的独立主机名，所有本文接口共用它 |
| 认证材料 | JWT 的开发者ID、项目ID、凭据ID、私钥路径；或 API KEY |
| 地点 | 城市名、纬度、经度；发送至当前固件时还需对应深圳/湛江城市码 |
| 数据选择 | 实况、逐日、AQI、预警；逐小时/生活指数按需启用 |
| 运行配置 | 拉取周期、超时、重试上限、缓存路径、日志脱敏；接板时再配置COM口 |

使用独立 API Host，详见[官方说明](https://dev.qweather.com/docs/configuration/api-host/)。旧公共域名 `api.qweather.com`、`devapi.qweather.com`、`geoapi.qweather.com` 自2026年起逐步停服，旧教程中的域名不能直接沿用。

### JWT，适合持续使用

JWT 使用 Ed25519 签名。生成一对密钥，将**公钥**登记为项目的 JWT 凭据，私钥留在自己的机器上。记录开发者ID、项目ID、凭据ID，这三个值分别使用，详见[身份认证](https://dev.qweather.com/docs/configuration/authentication/)。

使用 OpenSSL 3+ 时，可以在仓库外的私人配置目录执行。`-out` 避免 PowerShell 文本重定向改变 PEM 编码。

```powershell
openssl genpkey -algorithm ED25519 -out ed25519-private.pem
openssl pkey -in ed25519-private.pem -pubout -out ed25519-public.pem
```

下面是另存为 `make_qweather_token.py` 的示例，需要 `python -m pip install 'PyJWT[crypto]>=2,<3'`。建议在个人虚拟环境安装，仓库当前 requirements 尚未包含该依赖。通过环境变量配置四个 `QW_*` 项后运行；成功时只输出短期 Token，供下一节使用。

```python
import os
import time
from pathlib import Path
import jwt

now = int(time.time())
token = jwt.encode(
    {
        "iss": os.environ["QW_DEVELOPER_ID"],
        "sub": os.environ["QW_PROJECT_ID"],
        "iat": now - 30,
        "exp": now + 900,
    },
    Path(os.environ["QW_PRIVATE_KEY_FILE"]).read_bytes(),
    algorithm="EdDSA",
    headers={"kid": os.environ["QW_CREDENTIAL_ID"]},
)
print(token)
```

`iss` 是开发者ID，`sub` 是项目ID，`kid` 是凭据ID。系统时间要正确；有效期最长24小时，此例只取15分钟。Token到期应重新签发，持续发送器应在内存中自动更新。不要把私钥或Token放进日志、截图或提交记录。

### API KEY，适合先做接口调试

API KEY 同样可以请求 Web API。使用标头 `X-QW-Api-Key`，避免把 Key 放入URL查询参数。JWT 则使用 `Authorization: Bearer <token>`，两种标头不要混发。

官方认证页与凭据页均预告2027年初将限制 API KEY 的每日调用量，但两页写的开始日期有差异，分别为1月1日和2月1日。本文不把其中某一天作为程序保证，持续运行优先采用 JWT，并以控制台通知为准。SDK 5+ 只支持JWT；本文直接使用HTTP API，不需要SDK。

凭据、私钥和本地配置放仓库外，例如个人应用配置目录。本仓库 `.gitignore` 当前没有通用的 `.env`、`*.pem` 规则，不能假设放进项目就会自动忽略。多人各用自己的凭据；分发给他人或烧进ESP32的固件中也不要附带自己的长期私钥。相关建议见[安全指南](https://dev.qweather.com/docs/best-practices/security-guidelines/)。

## 3. 地点与坐标顺序

可以用 [GeoAPI 城市搜索](https://dev.qweather.com/docs/api/geoapi/city-lookup/)确定地点：

```text
GET /geo/v2/city/lookup?location=zhanjiang&adm=guangdong&range=cn&lang=zh
```

检查响应 `code`、`location[]` 中的名称、`adm1`、`adm2`、`lat`、`lon` 和时区，选择实际地点；切深圳时改查 `shenzhen`。市中心坐标与实际居住区并非同一地点，预警也按查询坐标覆盖。GeoAPI 支持选址查询，使用限制不允许批量下载、建立地理信息索引，见[使用限制](https://dev.qweather.com/docs/terms/restriction/)。

| 接口 | 坐标写法 |
| --- | --- |
| 新天气 / AQI / 预警 v1 路径 | `/.../{latitude}/{longitude}`，**纬度在前** |
| GeoAPI 或旧 v7 的 `location` 参数 | `经度,纬度`，**经度在前**，也可用相应LocationID |

v1路径坐标按文档最多保留两位小数。纬度范围−90～90，经度−180～180；本文调试例中的 `39.92/116.41` 是官方北京示例，使用时应替换为自己选定的坐标。城市名不参与 v1 天气路径匹配。

## 4. 本项目应请求哪些数据

以下路径均接在 `https://<自己的API Host>` 后，使用 GET。表中容量是API能力，不是当前固件的容量。

| 用途 | 路径 | 参数和响应重点 |
| --- | --- | --- |
| 实况及六格数值的大部分 | `/weather/v1/current/{latitude}/{longitude}` | `condition`、`temperature`、`feelsLike`、`humidity`、`wind`、`precipitation`、`pressure`、`visibility`、`uvIndex` |
| 高低温和三日列 | `/weather/v1/daily/{latitude}/{longitude}` | `days=3&localTime=true`；`days[]`含整时段高低温、白天和夜间预报；API最多10天 |
| AQI-CN | `/airquality/v1/current/{latitude}/{longitude}` | 从 `indexes[]` 按 `code=cn-mee` 选择，不能取数组第一项 |
| 当前预警集合 | `/weatheralert/v1/current/{latitude}/{longitude}` | `localTime=true&lang=zh`；`metadata.zeroResult`、`alerts[]` |
| 可选逐小时 | `/weather/v1/hourly/{latitude}/{longitude}` | `hours=24&localTime=true`；API最多240小时，当前页面没有小时列表 |
| 可选生活指数 | `/v7/indices/1d` 或 `/v7/indices/3d` | `location`、`type`；这是等级和建议，不用来替代实况UV或AQI数值 |

官方字段说明：[实况](https://dev.qweather.com/docs/api/weather/weather-current/)、[逐日](https://dev.qweather.com/docs/api/weather/weather-daily-forecast/)、[AQI](https://dev.qweather.com/docs/api/air-quality/air-current/)、[预警](https://dev.qweather.com/docs/api/warning/weather-alert/)、[逐小时](https://dev.qweather.com/docs/api/weather/weather-hourly-forecast/)、[生活指数](https://dev.qweather.com/docs/api/indices/indices-forecast/)。

现有六格固定为 FEELS、UV、AQI-CN、RAIN 1h、PRESS、VIS。用户要原始数值，首版无需增加穿衣、洗车或运动建议请求。台风预警属于预警集合；台风路径、历史轨迹和海洋数据另有接口与价格组，不能因能查到台风预警就认为能免费请求轨迹。

### 一次请求的最小例子

Windows PowerShell 使用 `curl.exe`，避免某些版本的 `curl` 别名改变参数。先在当前进程设置 `QW_HOST`、`QW_JWT`、`QW_LAT`、`QW_LON`；其中 JWT 可用上一节脚本生成。这里不填写任何真实凭据。

```powershell
$env:QW_HOST = '<控制台中的主机名，不含https://>'
$env:QW_LAT = '39.92'
$env:QW_LON = '116.41'
$env:QW_DEVELOPER_ID = '<开发者ID>'
$env:QW_PROJECT_ID = '<项目ID>'
$env:QW_CREDENTIAL_ID = '<JWT凭据ID>'
$env:QW_PRIVATE_KEY_FILE = '<仓库外的私钥文件完整路径>'
# 替换占位内容后执行；API KEY调试时改用下一节的安全输入
$env:QW_JWT = python .\make_qweather_token.py
curl.exe --fail-with-body --compressed --max-time 15 -H "Authorization: Bearer $env:QW_JWT" "https://$env:QW_HOST/weather/v1/current/$env:QW_LAT/$env:QW_LON"
```

`--compressed` 处理Gzip。旧版curl没有 `--fail-with-body` 时可用 `--fail`，错误详情用下节Python示例查看。每次请求都消耗相应API调用量；页面切换不应触发新的网络请求。

### Python 取四类数据

将以下内容另存为 `fetch_qweather_example.py`，Python 3.10+可直接运行，无额外依赖。先设置 `QW_HOST` 和地点；配置了 `QW_JWT` 时使用JWT，否则用隐藏输入读取API KEY。它请求一次四类数据，分开报告错误，不自动重试、不写缓存，也不发送串口。

```python
import getpass
import gzip
import json
import os
import re
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

host = os.environ["QW_HOST"].strip()
if not re.fullmatch(r"[A-Za-z0-9.-]+\.qweatherapi\.com", host):
    raise SystemExit("QW_HOST应为控制台分配的qweatherapi.com主机名")
lat, lon = float(os.environ["QW_LAT"]), float(os.environ["QW_LON"])
if not (-90 <= lat <= 90 and -180 <= lon <= 180):
    raise SystemExit("纬度或经度越界")
token = os.environ.get("QW_JWT", "").strip()
auth = ({"Authorization": "Bearer " + token} if token else
        {"X-QW-Api-Key": getpass.getpass("QWeather API KEY: ")})
point = f"{lat:.2f}/{lon:.2f}"
paths = {
    "current": f"/weather/v1/current/{point}?lang=zh",
    "daily": f"/weather/v1/daily/{point}?days=3&localTime=true&lang=zh",
    "air": f"/airquality/v1/current/{point}?lang=zh",
    "alerts": f"/weatheralert/v1/current/{point}?localTime=true&lang=zh",
}

def read_json(response):
    raw = response.read()
    if response.headers.get("Content-Encoding", "").lower() == "gzip":
        raw = gzip.decompress(raw)
    return json.loads(raw.decode("utf-8"))

for name, path in paths.items():
    request = Request("https://" + host + path,
                      headers={**auth, "Accept-Encoding": "gzip"})
    try:
        with urlopen(request, timeout=15) as response:
            data = read_json(response)
        if not isinstance(data, dict):
            raise ValueError("预期API返回JSON对象")
        if data.get("error") or str(data.get("code", "200")) != "200":
            raise ValueError("API业务错误，请检查响应结构与权限")
        print(name, json.dumps(data, ensure_ascii=False))
    except HTTPError as error:
        print(name, "HTTP", error.code)
    except (URLError, TimeoutError, OSError, ValueError, EOFError):
        print(name, "取数失败，请检查网络、压缩格式或JSON响应")
```

调试输出保留了原始数据与归因，适合核对字段。正式发送器还需要下面的缓存、单位转换、预警状态与串口事务处理。失败日志可以记录HTTP状态、官方 `error.type`、`error.invalidParams`，在输出之前脱敏；不要记录认证标头或带凭据URL。首次调试失败时也可以在自己的控制台API调试工具查看完整错误。

若此前设置过JWT，现在想换API KEY调试，先在当前PowerShell执行 `Remove-Item Env:QW_JWT -ErrorAction SilentlyContinue`，再运行Python示例。示例不会在JWT失败后偷偷切换认证方式。

## 5. 转换成 WDATA 前的检查

### 数值、单位与缺测

按响应的 `unit` 校验后转换，再按 WDATA 规定检查范围。缩放采用十进制四舍五入，例如Python `Decimal` 配合 `ROUND_HALF_UP`；不要用截断代替约定舍入，负数也要一致。缺值、`null`、非法单位、非有限数和超出协议范围均不可伪装为0或强行夹到边界。

| WDATA内容 | v1来源 | 转换 |
| --- | --- | --- |
| NOW当前温度 | `temperature.value`，°C | ×10取整数；高低温需合并当日逐日数据，实况接口本身没有当日高低温 |
| NOW相对湿度 | `humidity`，0～1 | ×100取整数百分比 |
| NOW风速 | `wind.speed.value`，m/s | ×10；Bft取 `wind.scale`，单级时min=max |
| METRICS体感 | `feelsLike.value`，°C | ×10 |
| METRICS UV | `uvIndex`，数值指数 | ×10；不要填生活指数的等级，也不要拿日最大UV冒充当前UV |
| METRICS AQI-CN | `indexes[code=cn-mee].aqi` | 中国标准整数；不存在就NA |
| METRICS RAIN 1h | `precipitation.amount.value`，mm | ×10；这是过去一小时累计量，`intensity`为mm/h，两者不互换 |
| METRICS PRESS | `pressure.value`，hPa | 取整数；v1为海平面气压，与旧v7站点气压、本地气压传感器读数需区分 |
| METRICS VIS | `visibility.value`，**m** | ÷100得到0.1km整数，例如18000m→180 |
| DAY高低温 | `days[].temperatureMin/Max.value` | ×10；使用完整时段的根级高低温，别误取白天子对象的最低温 |
| DAY降水概率 | 所选预报时段的 `precipitation.probability`，0～1 | ×100；缺测NA，先明确选的是白天还是夜间 |

单位依据[官方单位表](https://dev.qweather.com/docs/resource/unit/)。旧v7风速是km/h、能见度是km、湿度是百分数，返回值多为字符串；不能复用其缩放规则解析v1。旧城市实况v7已标注即将弃用，不作为新发送器默认方案。

当日高低温取自每日预报，表示预报极值；不能把它们标成当天已经发生的实测最高/最低温。

AQI按标准代码选择，`cn-mee-1h` 是另一种中国小时指数，`us-epa` 是美国标准，均不能直接标成当前约定的 `AQI-CN`。保留原始 `aqiDisplay` 便于核对，固定整数协议无法表示特殊显示值时报告限制。标准及用途见[指数列表](https://dev.qweather.com/docs/api/air-quality/aqi-list/)与[中国AQI说明](https://dev.qweather.com/docs/api/air-quality/china-aqi/)，数据用于参考，不能据此进行正式达标评价。

METRICS六项独立支持NA；NOW任一必要字段不足时，当前协议只支持整体NA。风向需由角度/16方位转换为协议8方位；变向风 `vrb` 没有精确对应，不能写成静风。可在degree有效时按角度取最近8方位；没有可映射方向且有风速时应报告缺失并使用NOW NA。Bft超出固件0～12范围也应报告，不应截成12。风向含义见[风向和等级](https://dev.qweather.com/docs/api/weather/wind-guide/)。

### 天气代码与每日时段

API天气代码与固件0～13不同，必须做显式映射。以下为可以明确对应的例子，未列代码须按[天气现象表](https://dev.qweather.com/docs/api/weather/weather-conditions/)补齐或回退0未知，不得直接发送原始代码。

| API代码 | 固件代码 |
| --- | --- |
| 100晴 | 1晴 |
| 101多云、102少云、103晴间多云 | 2多云，属于小屏归并 |
| 104阴 | 3阴 |
| 305小雨 / 306中雨 / 307大雨 | 4 / 5 / 6 |
| 302雷阵雨 / 303强雷阵雨 | 7雷阵雨，属于小屏归并 |

每日接口分白天和夜间，**没有可直接照抄的全天降水概率字段**。发送器须配置并记录选用的时段。若采用白天 `daytime.condition` 和对应概率，就把该列解释为白天预报；当前页面只写RAIN，若希望明确显示全天含义，应先调整契约和页面。不要取白夜最大值后声称这是官方全天概率。

每日记录日期应依据所选白天时段转换到查询地点本地时区后的日期。根级 `forecastStartTime` 表示时段起点，未保证是本地午夜；直接截取UTC日期会错日。STM32仅支持北京时间城市，按 `Asia/Shanghai` 转换后再格式化为协议时间。请求 `localTime=true` 时仍解析ISO 8601时区，禁止对已有+08:00的时间再加8小时。

### 时间字段还有一个接入缺口

本次核对的v1实况与AQI响应文档没有列出旧v7的 `obsTime` / `updateTime`。`metadata.tag` 是版本标识，[元数据说明](https://dev.qweather.com/docs/resource/metadata/)没有赋予它时间含义。HTTP `Date` 或本地取数时间也不代表观测时间。

现有 WDATA NOW 的 `source_time` 却要求实况来源更新时间，无法传未知。**完整接入前必须解决这个契约差异**：取得供应商明确提供的有效时间，或另行修改协议及界面，明确区分观测时间、成功取数时间和未知。本次只改文档，保持现有协议原义；暂未解决时发送NOW NA，其他有效数值和预报仍可发送。不得把本地当前时间填进去并标成来源更新时间。

## 6. 预警适配

查询结果应先在发送端整理成当前地点完整的有效预警集合，再生成WDATA。以下字段来自[预警API](https://dev.qweather.com/docs/api/warning/weather-alert/)，枚举与容量以串口协议为准。类型映射按[预警事件列表](https://dev.qweather.com/docs/api/warning/alert-events/)维护，处理规则参考[预警说明](https://dev.qweather.com/docs/api/warning/alert-guide/)；新类型保留原文并回退OTHER。

| API字段 | 处理 |
| --- | --- |
| `id` | 官方ID，作为去重与更新关系的依据 |
| `messageType.code` | 分别处理alert、update、cancel；cancel是撤销消息，不能作为新的活动灾害展示 |
| `messageType.supersedes[]` | 更新/撤销所替代的旧ID集合，在发送端完整处理 |
| `eventType.code/name` | 映射RAIN/STORM/WIND/TYPHOON等小屏类型；未识别用OTHER，保留原文 |
| `color.code` | 映射已支持的白/蓝/黄/橙/红；灰、绿、amber、紫、黑等当前无法精确表示，用UNKNOWN并记录原色 |
| `severity` / `urgency` | 按语义映射协议枚举；空值为unknown，不根据颜色猜造 |
| `issuedTime` / `effectiveTime` / `expireTime` | 转北京时间，分别为发布、生效、失效；未知用 `-` |
| `onsetTime` | 预计事件开始时间，与生效时间不同；当前WDATA无独立字段，发送端保留 |
| `headline` / `senderName` | 对应title / source；title须传和校验，当前设备页不显示完整标题 |
| `description` / `instruction` | 对应body / guide，保留原文和换行 |
| `certainty`、`criteria`、`responseTypes`及归因 | 当前协议未逐项承载，发送端保存原始响应，不能宣称设备显示了全部API字段 |

WDATA 的 `replaces` 只能表示一个旧ID，API的supersedes却是数组。多重替换先在发送端移除所有被替代ID，再发送新的完整集合；只有能明确表示的一对一关系才填replaces。其余关系留在发送端记录，不能丢掉数组中未选中的撤销对象。

成功且 `metadata.zeroResult=true` 表示无预警。网络失败、认证失败、解析失败均无权清空旧预警，应发布OFFLINE并保留同城最后成功时间。未知状态用UNKNOWN。当前协议的NONE/CLEARED转换与到期处理见WDATA说明，不在本文另建一套状态机。API有1小时有效的取消消息，发送端要先处理关系，再判断活动集合是否为空。

important由发送端显式给出，固件没有固定橙/红阈值。现有需求是重要消息自动显示、其他消息留底部条、全程无声。具体阈值应作为清楚可查的策略配置；官方严重程度、紧急程度和本地颜色规则可以作为依据，不能把原型示例当成既定官方等级标准。

### 超过设备容量怎么办

当前设备最多3条预警；title/body/guide及中文字形都有界限。具体字节上限、分片、CRC和26×26位图格式仅维护在 [WDATA 协议](weather_feed_protocol.md#动态中文字形与资源)。长度按UTF-8字节，不按中文字符数；未上屏的title同样需要通过字形完整性校验。

API未保证一定落在这些容量内。发送端应在BEGIN之前检查全部记录、字节和不同字符数。超限时明确报告无法完整发布，保留设备上次成功快照。不得只截前三条、截断正文、偷删字符或用NONE假装成功，也不要把本地容量错误冒充API离线。字符池需使用许可允许的字体生成，设备当前最多128个非内置码点；无需为重复汉字反复发送字形。

## 7. 刷新、缓存和费用

下面是**发送器建议初值**，可以配置，尚非已实现调度。每类API独立维护最近成功拉取时间、tag、原始响应和错误状态；城市变更隔离缓存。

| 请求 | 建议周期 | 单地点30天请求估算 |
| --- | --- | --- |
| 实况（含六格中可取得的数值） | 10分钟 | 4320 |
| 每日预报 | 60分钟 | 720 |
| AQI | 30分钟 | 1440 |
| 预警 | 5分钟 | 8640 |
| 合计 | 不含首次选址、重试、其他客户端 | 15120 |

月估算公式为 `天数×24×60÷周期分钟`，四项合计；31天为15624次。两地点全天轮询约翻倍，启动重复拉取和重试另算。能复用一份实况响应就不要为六个格子各发一次请求；tag可用于避免重复转换/传输，但比较tag本身不会免掉已经发生的网络请求。

截至核对日，[价格页](https://dev.qweather.com/docs/finance/pricing/)的“天气和基础服务”组每月前50,000次为0元，后续950,000次为0.0007元/次，再按阶梯累进。天气、预警、AQI、生活指数、Geo等属于同组累计，**不是每个接口、每个项目各有50,000次**。台风和海洋是另一价格组。免费区间不表示自动停止扣费，服务权限、可用额度及实际账单需各使用者自行在控制台确认。

缓存要保持数据年龄。预警查询每5分钟成功一次，并不说明十分钟前失败的实况又变新。WDATA每次新COMMIT会重计整批三小时TTL，所以若一直把旧实况重包发送，设备可能一直不显示STALE。发送器须按各数据源自己的成功时间淘汰旧字段，再组合快照；过期的实况用NOW NA、过期数值用对应NA。要保留旧值同时显示独立年龄，需要另行扩展协议，当前没有各字段年龄。

API失败保留原缓存，不更新该源的成功时间。使用指数退避和抖动，限制总重试；400/401/403等配置问题先停止对应请求并排查，429按限流处理，5xx/超时再退避。状态码详见[错误码](https://dev.qweather.com/docs/resource/error-code/)。分钟级更新或1km分辨率是服务说明，不代表请求现场的误差或延迟已经测得。

| 现象 | 优先检查 |
| --- | --- |
| 400 | 经纬度顺序、范围、小数位、路径/查询参数；`DATA NOT AVAILABLE`还可能是地点无相应数据 |
| 401 | Host与凭据是否属于同一帐号，JWT签名、三个ID、系统时间与有效期；是否混用认证标头 |
| 403 | 余额/可用额度、服务与凭据权限，或所用API已弃用；不要一律当成Key错误 |
| 404 | Host之后的接口路径是否正确；旧教程是否使用了已变更的版本 |
| 429 | QPM限流或累计错误请求；减少请求并退避，检查帐号订阅限制 |
| 无预警、但HTTP成功 | 核对zeroResult与取消关系；这是成功结果，不是通讯故障 |

## 8. 接 STM32 时需要了解什么

1. 先独立完成取数、单位、时区和容量校验，再打开串口，避免网络等待占用设备事务。
2. H10 USART2为115200/8N1、3.3V TTL。AUTO睡眠时首包可能丢失，在启动/动作窗口取得 `OK POWER HOLD` 后再 `CONSOLE ON`。结束会话按[低功耗说明](low_power.md)恢复AUTO；控制台本身不保持清醒。
3. 用WDATA逐行等待ACK，整批校验后COMMIT。重试、序号、CRC和STATUS核对严格按[协议](weather_feed_protocol.md)实现；不要把API JSON直接丢给STM32。
4. 串口仅由一个程序占用。`tools/file_store_cli.py`是文件维护工具，不是天气发送器；它不会替你持续拉API或生成中文预警字形。
5. WDATA只存RAM，断电丢失。若需保存旧1～7天预报，另用[FORECAST](forecast_protocol.md)，不能把WDATA三日与持久预报当成同一批次。
6. 天气API时间只作天气依据。RTC授时要单独使用可信系统时间/NTP及 [TIME2/TIME?](time_protocol.md)，HTTP响应时间不能代替授时。成功ACK也不能证明100ms精度。
7. 当前几项已确认行为仍待改动，包括陈旧天气回退、OFFLINE自动预警续显及故障后补显。以[差异清单](current_code_gaps.md)为准，不把新发送器当作固件已有这些行为的证明。

## 9. 共享或发布前保留的资料

每位使用者应拿到配置说明、自己的凭据申请方法、地点选择结果、请求预算、字段映射版本、字体来源与许可、失败时的行为和复现步骤。共享样例用脱敏JSON或构造数据，标明示例性质；真实ID、地理位置与正文按需要保护。不要打包私人配置、API KEY、Token或私钥。

保留API返回的 `metadata.attributions[]`，兼容旧接口时也保留 `refer.sources/license`。官方[注明来源](https://dev.qweather.com/docs/terms/attribution/)对产品名称、链接及预警/AQI额外来源有要求，特殊设备可以向供应商讨论适用方式。现有设计继续采用用户确认的不在设备页面写“和风天气”的布局；这项视觉决定不等于已经取得特殊许可。本文只记录要求与待核实事项，不自行改设备页面或宣称把来源写进README就已满足要求。

预警响应本身提示可能延迟或过时，使用时以官方发布为准。保存发布机构、原文、发布/生效/失效时间和最近成功查询时间，便于判断版本；不能保证本地零延迟或永不漏警。传输容量、网络失败或未知字段应有可见结果。

交接时分别写明“文档示例已检查”“真实API取数成功”“串口批次提交成功”“目标镜像已烧录”“实屏及RTC精度已验收”。本次已核对官方资料、当前代码/协议和73处本地链接/锚点，检查两个Python代码块语法，并通过8种离线HTTP模拟场景。JWT示例仅做语法检查，未执行真实签名或帐号认证。未调用真实API、连接串口、构建或烧录固件，不改变统一工程进度。
