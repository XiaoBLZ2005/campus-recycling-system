# NFC + CMCU-06 称重 + 红外对射正式程序

这个工程把 PN5180 NFC 扫描、CMCU-06 Modbus-RTU 称重和红外对射检测合并在同一个 ESP32-S3 固件中。

## 接线

| PN5180 | ESP32-S3 |
|---|---|
| NSS | GPIO10 |
| MOSI | GPIO11 |
| SCK | GPIO12 |
| MISO | GPIO13 |
| BUSY | GPIO14 |
| RST | GPIO15 |
| +5V | 5V |
| +3.3V | 3V3 |
| GND | GND |

| TTL 转 RS485 | ESP32-S3 |
|---|---|
| Vin | 5V |
| GND | GND |
| RX | GPIO4 |
| TX | GPIO5 |

8048S070C 屏幕的 P1 串口接线：

| 屏幕 P1 | ESP32-S3 |
|---|---|
| GND | GND |
| RX0 | GPIO18（UART2 TX） |
| TX0 | GPIO17（UART2 RX） |
| +5V | 5V 或外部稳压 5V |

CMCU-06 与转换板通信使用 `9600 8N1`、Modbus 地址 `1`。CMCU-06 的 `+5V`、`GND` 必须和 ESP32 共电源、共地。

| YED-M100M-C2 | ESP32-S3 |
|---|---|
| VIN | 独立 3.8V/2A 电源 |
| GND | GND |
| RXD | GPIO6（UART0 TX） |
| TXD | GPIO7（UART0 RX） |
| DTR、RST、RDY | 暂不连接 |

DTU 使用 `115200 8N1`。DTU 电源地、ESP32 地、屏幕地和称重转换板地必须共地。
不要给 M100M-C2 的 VIN 接 5V，其额定输入为 3.3～4.2V。

| 红外对射 | ESP32-S3 |
|---|---|
| 两线发射端红线 | 3V3 |
| 两线发射端黑线 | GND |
| 三线接收端红线 | 3V3 |
| 三线接收端黑线 | GND |
| 三线接收端白线 | GPIO16 |

实测光路通畅时 GPIO16 为高电平，遮挡时为低电平。红外任务每 5 ms 采样，连续 50 ms 保持新电平才确认状态；一次遮挡至少保持 150 ms 才计为一个瓶子，恢复光路并稳定 350 ms 后才允许计数下一个瓶子。这样可避免瓶口、标签或手部抖动造成重复计数。

## 串口输出

刷卡时输出：

```text
CARD PROTOCOL: ISO14443-A
CARD UID: ...
```

重量更新时输出：

```text
WEIGHT status=OK raw=221 grams=221.1 age_ms=6 samples=42
```

通信失败时输出：

```text
WEIGHT status=NO_RESPONSE attempts=43
```

红外状态变化时输出：

```text
BEAM status=BLOCKED changes=1 last_change_ms=5321
BEAM status=CLEAR changes=2 last_change_ms=6084
```

主板同时通过 UART2（`115200 8N1`）每 250 ms 向屏幕发送：

```text
SENSOR|0.221|4
SENSOR|--|4
```

第一个字段是千克重量；称重模块无有效响应时发送 `--`。第二个字段是红外遮挡次数，每检测到一个物体通过光路时增加 1，恢复光路本身不计数。

识别到学生卡时，主板另外发送 `CARD|<uid>`。屏幕收到后保持显示本次投放完成及当前计算出的积分。

当前按 `RAW × 1.0006` 换算为克。NFC 由主循环扫描，称重和红外检测分别由独立 FreeRTOS 任务处理，称重超时不会长时间阻塞刷卡或红外检测。

当前学生卡均为 ISO15693。PN5180 初始化后保持 ISO15693 射频配置连续轮询，
不再每轮切换 ISO14443-A，实测无卡扫描周期由约 317 ms 降到约 42 ms。
同一张卡需要连续停留 500 ms 才会上报刷卡事件，并且离开读卡区持续 400 ms
后才允许下一次刷卡，以减少挥手和卡片边缘抖动造成的误触发。

## 4G 服务器同步

DTU 配成 TCP 透传后，主控通过每行一条 UTF-8 JSON 与服务器通信。DTU
配置中的服务器地址和端口决定实际后端，主控固件不再硬编码服务器 IP。

第一次刷卡发送 `card_start`，服务器返回学生姓名和累计积分。主控随后向屏幕
发送 `STUDENT_INFO|<card_uid>|<total_points>|<name>`，显示姓名、完整卡号和
累计积分；第二次刷卡
或 60 秒超时后发送 `settle`。每条消息包含 `msg_id`，主控每 2 秒重试，
服务器以 `event_id` 去重，防止重复结算。

可部署的后端见 `../dtu_server`。完成名单导入后启动服务器，并把 DTU 通道
改为该服务器的公网地址和端口。

## 编译与烧录

```powershell
py -m platformio run
py -m platformio run -t upload --upload-port COM13
py -m platformio device monitor --port COM13 -b 115200
```

首次编译会自动下载 PlatformIO 依赖；`PN5180Robust` 已随工程放在 `lib/` 中。
