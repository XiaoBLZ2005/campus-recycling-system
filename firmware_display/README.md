# 校园智能分类回收站屏幕

ESP32-S3 8048S070C 参考图版本仪表盘。页面包含：

- 顶部回收站名称与设备在线状态
- 学生卡、投放状态和卡号
- 账户积分与本次积分
- 今日纸类重量和塑料瓶数量
- 底部刷卡、称重、积分到账指引

## 串口数据

通过 COM12、`115200 8N1` 发送，每条命令以回车或换行结束：

```text
CARD|<card-id>[|<points>]
CARD_EVENT|<card-id>|<event-id>
MEASURE|<paper|plastic>|<kg>|<bottles>
SENSOR|<kg-or-dash>|<ir-count>
STUDENT_INFO|<card-id>|<total>|<name>
STUDENT_POINTS|<card-id>|<total>
SETTLE_RESULT|<card-id>|<total>|<1=success|0=failure>
POINTS|<total>
DEMO
RESET
HELP
```

`SENSOR` 只刷新界面上原有的重量和数量，不会增加“今日投放次数”。例如
`SENSOR|0.480|7` 会实时显示 `0.48 kg` 和 `7 件`；秤无有效数据时发送
`SENSOR|--|7`，重量保持上一次有效值。

本次积分按“瓶子数量 × 5 + 非负重量kg × 10”计算并四舍五入为整数。

学生第一次刷卡时，屏幕记录当时秤重和累计瓶数作为基线，并把本次显示值清零。
随后新增重量与瓶数都按“当前值 - 基线”计算。同一张卡再次刷卡时结算并显示
“投放完成，（本次积分）积分已到账”。下一位学生第一次刷卡会重新建立基线。
如果开始投放后 60 秒内没有再次刷卡结算，屏幕会自动按当时数据结算；完成
提示显示 5 秒后自动退回“请刷卡”界面。投放期间右上角显示 60 秒倒计时，
结算后同一位置切换为 5 秒返回倒计时。

未刷卡或结算完成时，屏幕只记录传感器原始值，不刷新重量与数量。只有第一次
刷卡进入投放会话后才开始刷新本次数据。

主控发送 `CARD_EVENT` 后等待屏幕回复
`ACK|CARD_EVENT|<event-id>`。重复序号会被屏幕忽略并再次确认，避免串口偶发
丢包导致首刷事件永久丢失；收到新事件时屏幕会立即强制刷新一次。

主控收到服务器返回的学生资料或结算确认后，通过
`STUDENT_INFO|<card-id>|<total>|<name>` 更新姓名、完整卡号和累计积分；
同时仍发送 `STUDENT_POINTS` 以兼容旧版显示固件。这两条命令不会改变投放阶段。
第二次刷卡后，屏幕先显示“正在上传本次积分”，只有收到
`SETTLE_RESULT|...|1` 才显示“积分已到账”；失败或超时不会自行增加账户积分。

## 中文文字

`src/fonts/` 中保存了根据截图所需文字生成的 16px、22px、28px
中文字体，不依赖系统字体，也不会显示方框。

## 编译和烧录

```powershell
py -m platformio run
py -m platformio run -t upload --upload-port COM9
```

屏幕的 `P1 RX0/TX0` 与下载串口共用 UART，烧录前要断开 P1 数据线，
否则主控持续发送的 `SENSOR` 数据会造成下载校验失败。实际 COM 号以
`py -m platformio device list` 显示的 CH340 端口为准。

发布包已包含本工程使用的 `esp32-8048S070C` 板卡定义和本地
`esp32_smartdisplay` 驱动。首次编译时 PlatformIO 会下载 LVGL 与工具链。

本项目不启用触摸。
