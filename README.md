# 校园智能分类回收站

本项目包含校园分类回收站的学生卡识别、称重和瓶数检测、4G DTU 通信、学生积分
服务器以及显示屏固件。

## 目录

- `server/`：Python TCP 服务、SQLite 数据库、网页管理台和自动化测试。
- `firmware_main/`：ESP32-S3 主控源码，负责 NFC、称重、红外、DTU 和屏幕通信。
- `firmware_display/`：ESP32-S3 8048S070C 显示屏源码，负责 LVGL 仪表盘。
- `firmware_release/`：本次编译通过的主控与显示屏应用固件。
- `docs/architecture.md`：系统数据流和当前功能边界。

## 系统链路

```text
学生卡 -> 主控 -> 4G DTU -> 服务器/SQLite -> 主控 -> 显示屏
```

主控先发送 `card_start`，服务器查询姓名与积分后回传；投放结束后主控发送
`settle`，服务器以事件号去重并更新积分。服务器目前不是主动推送架构。

## 服务器启动

需要 Python 3.11 或更高版本，运行时只使用标准库。

```powershell
cd server
py server.py --open-browser
```

TCP 服务监听 `9000`，网页管理台监听 `127.0.0.1:8765`。若需通过局域网访问网页，
请启用认证：

```powershell
$env:ADMIN_USERNAME = "teacher"
$env:ADMIN_PASSWORD = "请换成强密码"
py server.py --http-host 0.0.0.0
```

不要把 `recycling.db` 或日志提交到公开仓库；运行数据和个人信息不应随源码上传。

## 固件编译

安装 Python 和 PlatformIO 后：

```powershell
cd firmware_main
py -m platformio run

cd ..\firmware_display
py -m platformio run
```

具体接线、上传端口和串口协议分别见两个固件目录内的 `README.md`。

## 验证状态

- 服务器 `py server.py --selftest`：通过。
- 网页与 TCP 自动化测试：2 项通过。
- 主控固件：PlatformIO 编译通过。
- 显示屏固件：PlatformIO 编译通过。

当前发布包完成的是代码、自动化测试和固件编译验证，不等于所有实体板卡均已完成
硬件联调。烧录后仍需检查 NFC、称重、红外、DTU 和屏幕回传链路。

## 上传 GitHub

将本目录作为仓库根目录即可。`.gitignore` 已排除数据库、日志、PlatformIO 缓存、
本地密码文件和编译中间文件。固件已通过编译验证，可以在 GitHub Release 中单独
上传，也可以随仓库一起提交。
