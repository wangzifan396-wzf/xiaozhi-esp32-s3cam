# 小智 AI 聊天机器人 — 增强定制版

基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 ESP32-S3 CAM 开发板定制版本。

## 硬件配置

- **主控**: ESP32-S3 N16R8 CAM 开发板（44pin）
- **屏幕**: 1.54 寸 TFT SPI ST7789（240x240）
- **摄像头**: OV3660
- **音频**: ZTS6672 I2S 麦克风 + MAX98357A I2S 功放 + 喇叭
- **存储**: 板载 MicroSD 卡槽（SDMMC 1-bit）

## 新增功能

### 照片系统
- 拍照自动存 SD 卡，按年月分文件夹 `/sdcard/photos/2026-07/时间.jpg`
- 屏幕浏览照片：「看照片」「下一张」「上一张」「看第一张」
- 灵活定时拍照：「3 秒后拍一张」「每隔 5 秒拍一张拍 3 张」「停下」

### 记忆系统
- 聊天记录按天存储 `/sdcard/chats/日期.log`，每次开机自动加分割线
- 搜索历史对话：「搜一下之前关于 XX 的聊天」
- 回顾最近对话：「我们刚才聊了什么」

### 语音备忘录
- 「帮我记住：大门密码是 8888」
- 「大门密码是啥」「我记了哪些东西」

### 多唤醒词
- menuconfig 里用分号分隔，同时识别多个唤醒词
- 例如：「小智小智;小智同学;嘿小智」

### 屏幕优化
- LVGL 软件旋转（左转 90°）
- 摄像头预览镜像修正

## 引脚接线

| 模块 | 信号 | GPIO |
|------|------|------|
| 屏幕 ST7789 | MOSI/CLK/DC/RST/CS/BL | 20/19/47/21/45/14 |
| 麦克风 ZTS6672 | WS/SCK/SD | 1/2/42 |
| 喇叭 MAX98357A | DOUT/BCLK/LRCK | 46/3/41 |
| 摄像头 OV3660 | D0~D7 等 | 4~18 |
| SD 卡 (SDMMC) | CMD/CLK/D0 | 38/39/40 |

## 编译

```bash
# ESP-IDF v5.5.4
idf.py set-target esp32s3
idf.py menuconfig
# 选择: Board Type → Bread Compact WiFi + LCD + Camera
# LCD Type → ST7789 240x240
# 可选: 自定义唤醒词 → Wake Word Type → Multinet model
idf.py build flash
```

## 致谢

本项目基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 开源项目修改，感谢原作者及社区贡献。
