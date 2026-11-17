# 系统架构

```text
学生卡
  -> PN5180 NFC
  -> ESP32-S3 主控
  -> M100M-C2 4G DTU
  -> TCP 9000 服务器
  -> SQLite 学生与积分数据
  -> TCP 响应返回主控
  -> UART2
  -> 8048S070C 显示屏
```

## 服务器协议

服务器与主控使用 UTF-8、每行一条 JSON：

- `card_start`：主控上传学生卡 UID，服务器返回姓名和累计积分。
- `settle`：主控上传重量、瓶数、本次积分和唯一事件号，服务器返回结算确认。
- `ping` / `pong`：用于通信检查。

TCP 端口默认为 `9000`，HTTP 管理台端口默认为 `8765`。

## 主控与显示屏

主控和显示屏通过 UART2、`115200 8N1` 通信。服务器返回学生资料后，主控发送：

```text
STUDENT_INFO|<card_uid>|<total_points>|<name>
SETTLE_RESULT|<card_uid>|<total_points>|<success>
SENSOR|<weight_kg_or_dash>|<bottle_count>
```

## 当前边界

服务器目前采用主控先请求、服务器再回包的传输方式。网页修改学生资料后，主控要在
下一次发送 `card_start` 或 `settle` 时才能获得最新内容；服务器没有向已连接设备
主动推送到网页修改或批量名单更新的功能。
