# 回收箱 DTU + 学生积分网页服务器

这个目录现在包含两部分：

- `TCP 9000`：接收 M100M-C2 DTU 发来的刷卡查询和结算 JSON。
- `HTTP 8765`：在本机浏览器中管理学生卡、查看积分和最近刷卡记录。

数据库使用 Python 自带的 SQLite，数据默认保存在 `recycling.db`。不需要安装
Flask、Node.js 或数据库软件。

## 最简单的启动方法

1. 确认电脑已安装 Python 3.11 或更新版本。
2. 双击 `启动服务器.bat`。
3. 浏览器会自动打开 `http://127.0.0.1:8765/`。

也可以在 PowerShell 中运行：

```powershell
cd server
py server.py --open-browser
```

启动后应同时看到：

```text
web admin: http://127.0.0.1:8765/
DTU TCP listening on 0.0.0.0:9000, database=recycling.db
```

关闭终端或按 `Ctrl+C` 即可停止服务器。

## 网页端能做什么

- 登记学生卡：把 NFC UID 与姓名绑定。
- 查询学生卡：显示卡号、姓名和当前积分。
- 自动读卡：网页保持打开时约 1 秒内显示最新刷卡结果。
- 未登记卡自动弹出补录窗口；已登记卡可直接点击“编辑”修改姓名。
- 修改姓名：已有流水仍会保留。
- 手动结算积分：适合在没有 DTU 时测试。
- 查看最近刷卡和积分流水：每 5 秒自动刷新。

刷卡查询后的数据流是：

```text
学生卡 UID -> ESP32 -> DTU -> 电脑 TCP 9000
           -> SQLite 查询姓名和积分 -> 返回设备显示
```

卡号本身不保存姓名或积分，只作为唯一身份标识；姓名和积分由本服务器数据库映射。

浏览器的自动识别依赖 DTU 已在线，并且 ESP32 能向 TCP 9000 发送
`card_start`。读卡记录使用固件生成的 `event_id` 去重，因此网络重试不会让网页
连续弹出多次登记窗口。

## 导入学生名单

复制 `students.example.csv` 为学生名单 CSV，然后运行：

```powershell
py server.py --database recycling.db --import-csv students.csv
```

CSV 格式：

```csv
card_uid,name,total_points
01:23:45:67:89:AB:CD:EF,张三,0
```

导入时会以 CSV 中的 `total_points` 覆盖该生原有积分。日常增加积分请使用网页或
DTU 结算，不要重复导入旧名单。

## DTU 与手机访问

DTU 的 TCP 通道需要配置为：

```text
电脑局域网 IP:9000
```

例如电脑地址是 `192.168.1.50`，DTU 就连接 `192.168.1.50:9000`。Windows
防火墙需要允许 TCP 9000 入站。网页管理台默认只允许本机访问。

如果设备连接超时，可在以管理员身份打开的 PowerShell 中只放行该端口：

```powershell
New-NetFirewallRule -DisplayName "Recycling DTU 9000" `
  -Direction Inbound -Protocol TCP -LocalPort 9000 -Action Allow
```

如果确实要让同一局域网内的手机打开管理网页：

```powershell
py server.py --http-host 0.0.0.0
```

然后访问 `http://电脑局域网IP:8765/`。如果设置了环境变量
`ADMIN_PASSWORD`，网页会启用 HTTP Basic 认证；未设置时不会启用认证。因此在
可信校园内网中部署时也建议设置密码，不要直接映射到公网。

```powershell
$env:ADMIN_USERNAME = "teacher"
$env:ADMIN_PASSWORD = "请换成强密码"
py server.py --http-host 0.0.0.0
```

## TCP 协议

每行是一条 UTF-8 JSON，响应也以换行结尾。

第一次刷卡：

```json
{"type":"card_start","msg_id":1,"device_id":"...","card_uid":"01:23:45:67:89:AB:CD:EF"}
```

服务器返回姓名和累计积分：

```json
{"type":"student","msg_id":1,"ok":true,"card_uid":"01:23:45:67:89:AB:CD:EF","name":"张三","total_points":128,"registered":true}
```

结算上传：

```json
{"type":"settle","msg_id":2,"event_id":"...","device_id":"...","card_uid":"01:23:45:67:89:AB:CD:EF","name":"张三","weight_kg":0.48,"bottles":2,"points":24,"started_uptime_ms":1000,"ended_uptime_ms":61000}
```

服务器以 `event_id` 去重，在一个数据库事务中写入流水、累加积分并回复
`settle_ack`。同一 `event_id` 重发不会重复加分。

设备在线状态要求 TCP 连接存在，并且最近 45 秒内收到过数据。建议在 M100M
中启用心跳包：HEX 数据 `0A`、间隔 15 秒。服务器会把仅包含换行的心跳作为
有效在线信号；如果关闭心跳且设备长时间没有刷卡，页面可能显示设备离线。

## HTTP API

网页端使用的接口也可以给其他程序调用：

```text
GET  /api/health
GET  /api/device-status
GET  /api/summary
GET  /api/students?q=张三
GET  /api/students/card?card_uid=E0%3A1D%3A...
POST /api/students
PUT  /api/students
DELETE /api/students?card_uid=E0%3A1D%3A...
GET  /api/card-reads?limit=10
GET  /api/transactions?limit=50
POST /api/settlements
```

登记学生：

```json
{"card_uid":"01:23:45:67:89:AB:CD:EF","name":"张三"}
```

网页手动结算：

```json
{"card_uid":"01:23:45:67:89:AB:CD:EF","event_id":"唯一事件号","points":24}
```

## 自检

```powershell
py server.py --selftest
py -m unittest .\test_web_admin.py -v
```

自检覆盖学生查询、积分结算、事件去重、网页接口和静态页面。

## 数据备份

停止服务器后，复制 `recycling.db` 即可备份全部学生和积分流水。恢复时把备份
文件放回本目录，并仍以 `--database recycling.db` 启动。
