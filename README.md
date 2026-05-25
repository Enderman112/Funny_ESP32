# Funny_ESP32

ESP32-S3 RLCD 4.2寸屏幕驱动项目，基于 ESP-IDF v6.0.1 和 LVGL。

## 功能

- RLCD 反射式 LCD 屏幕驱动
- LVGL 图形界面支持
- 自动编译发布（GitHub Actions）

## 硬件

- ESP32-S3
- RLCD 4.2寸屏幕（400x300）

### 引脚配置

| 功能 | GPIO |
|------|------|
| MOSI | 12 |
| SCK | 11 |
| DC | 5 |
| CS | 40 |
| RST | 41 |
| TE | 6 |

## 编译

```bash
idf.py build
```

## 烧录

```bash
# 合并固件一键烧录
esptool.py write_flash 0x0 release/FunnyEsp32.bin

# 或分别烧录
esptool.py write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 screen.bin
```

## GitHub Actions

每次 push 到 main 分支会自动：
1. 编译固件
2. 生成合并固件 `FunnyEsp32.bin`
3. 发布到 GitHub Releases

## 目录结构

```
├── components/
│   ├── display_bsp/    # 屏幕驱动
│   └── lvgl_bsp/       # LVGL 初始化
├── main/
│   ├── main.cpp        # 主程序
│   └── user_config.h   # 引脚配置
└── CMakeLists.txt
```
