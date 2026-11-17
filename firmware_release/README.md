# 2026-11-17 应用固件

本目录包含本次源代码验证时生成的两份 ESP32-S3 应用固件：

| 文件 | 设备 | 应用镜像大小 |
|---|---|---:|
| `campus-recycling-main-20261117.bin` | 主控板，ESP32-S3-DevKitC-1 | 421,152 bytes |
| `campus-recycling-display-20261117.bin` | 8048S070C 显示屏 | 2,047,344 bytes |

这些是 PlatformIO 生成的 `firmware.bin` 应用镜像。更新已经完成首次分区和
Bootloader 烧录的设备时，可使用 PlatformIO 的 `upload` 目标，或按开发板的
应用分区地址烧录。空白芯片建议直接使用对应源码工程执行完整上传，避免只写应用
镜像而缺少 Bootloader 或分区表。

主控示例：

```powershell
cd ..\firmware_main
py -m platformio run -t upload --upload-port COM13
```

显示屏示例：

```powershell
cd ..\firmware_display
py -m platformio run -t upload --upload-port COM9
```

烧录显示屏前应断开 P1 串口数据线。当前二进制通过编译验证，尚未对每一块实体
板卡逐一复核；烧录后仍需检查 NFC、称重、红外、DTU 和屏幕回传链路。
