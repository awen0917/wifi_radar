# WiFi Radar — ESP32 室内人员感知系统

基于 ESP32 的 WiFi CSI（Channel State Information）室内人员检测系统。通过分析 WiFi 信道状态信息的子载波幅度方差，实现对室内人员活动的实时感知，并提供 Web 仪表盘可视化与串口数据输出。

| 支持芯片 | ESP32 | ESP32-S2 | ESP32-S3 | ESP32-C3 | ESP32-C5 | ESP32-C6 |
| -------- | ----- | -------- | -------- | -------- | -------- | -------- |

## 功能特性

- **CSI 采集与人员检测**：实时采集 56 个 OFDM 子载波幅度数据，基于方差分析检测人员活动
- **内置 Web 仪表盘**：实时显示 CSI 波形、方差曲线、检测结果等
- **串口波形数据输出**：通过 UART 输出结构化波形数据，便于外部分析
- **WiFi 图形化配置**：AP 热点 + 配置页面，无需代码修改即可切换 WiFi
- **NVS 持久化存储**：WiFi 配置断电不丢失，开机自动连接
- **可调检测参数**：通过 `menuconfig` 自定义检测阈值和窗口大小

## 项目结构

```
wifi_radar/
├── CMakeLists.txt              # 项目构建配置
├── sdkconfig                   # ESP-IDF 配置（自动生成）
├── main/
│   ├── CMakeLists.txt          # 组件注册（嵌入 HTML、依赖库）
│   ├── wifi_radar_main.c      # 主程序源码
│   ├── dashboard.html          # 仪表盘页面（嵌入固件）
│   ├── config.html             # WiFi 配置页面（嵌入固件）
│   └── Kconfig.projbuild       # menuconfig 自定义菜单
└── README.md
```

## 环境要求

- **ESP-IDF**：v5.x（推荐 v5.5.4，与 sdkconfig 匹配）
- **芯片**：ESP32 系列（需支持 WiFi CSI）
- **Python**：3.8+（ESP-IDF 依赖）
- **串口驱动**：CP210x / CH340 等，确保电脑能识别 ESP32 串口

## 快速开始

### 1. 设置 ESP-IDF 环境

```bash
# 如果尚未安装 ESP-IDF，请参考官方文档完成安装
# https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/

# 进入 ESP-IDF 目录，激活环境
cd <你的ESP-IDF安装路径>
# Linux/macOS
. ./export.sh
# Windows (PowerShell)
.\export.ps1
```

### 2. 克隆项目

```bash
git clone <仓库地址> wifi_radar
cd wifi_radar
```

### 3. 配置项目（可选）

```bash
idf.py menuconfig
```

进入 `WiFi Radar Configuration` 菜单，可调整以下参数：

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `WIFI_RADAR_AP_SSID` | `WiFiRadar_Setup` | AP 热点名称 |
| `WIFI_RADAR_AP_PASSWORD` | `12345678` | AP 热点密码（至少 8 位） |
| `WIFI_RADAR_PRESENCE_THRESHOLD` | `8` | 人员检测方差阈值 |
| `WIFI_RADAR_CSI_WINDOW_SIZE` | `30` | 滑动窗口大小 |

### 4. 编译

```bash
idf.py build
```

首次编译会下载依赖组件，耗时较长。后续增量编译很快。

### 5. 烧录

```bash
# 查看可用串口
idf.py -p COM? flash

# 常见串口号：
#   Windows: COM3, COM4, COM5 ...
#   Linux:   /dev/ttyUSB0
#   macOS:   /dev/cu.usbserial-xxxx
```

也可以分步操作：

```bash
# 先编译+烧录
idf.py build
idf.py -p COM3 flash
```

### 6. 串口监视

```bash
idf.py -p COM3 monitor
```

> 按 `Ctrl+]` 退出监视器。也可合并为一步：`idf.py -p COM3 flash monitor`

### 7. 一键编译+烧录+监视

```bash
idf.py -p COM3 flash monitor
```

## 使用流程

### 首次配置

1. 烧录后，ESP32 启动 AP 热点 **WiFiRadar_Setup**（密码 `12345678`）
2. 电脑/手机连接该热点
3. 浏览器自动弹出配置页面，或手动访问 `http://192.168.4.1/config`
4. 选择家中 WiFi 并输入密码，点击保存
5. ESP32 自动连接家中 WiFi，CSI 数据采集开始

### 日常使用

- **仪表盘**：访问 `http://192.168.4.1/`（AP 地址）或 `http://<STA IP>/`
- **重新配置**：访问 `http://192.168.4.1/config` 或 `http://<STA IP>/config`
- **串口数据**：`idf.py monitor` 可查看实时波形与检测数据

## Web API

| 接口 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 仪表盘页面 |
| `/config` | GET | WiFi 配置页面 |
| `/api/status` | GET | 检测状态 JSON |
| `/api/csi` | GET | CSI 实时数据（含子载波波形） |
| `/api/scan` | GET | 扫描周围 WiFi |
| `/api/config` | GET | 获取当前 WiFi 配置 |
| `/api/config` | POST | 保存 WiFi 配置 |

**状态 API 示例** (`/api/status`)：

```json
{
  "presence": true,
  "variance": 12.34,
  "rssi": -52,
  "confidence": 78,
  "ip": "192.168.1.100",
  "connected": true,
  "ap_active": true,
  "csi_count": 12345
}
```

**CSI 数据 API 示例** (`/api/csi`)：

```json
{
  "variance": 12.34,
  "presence": true,
  "rssi": -52,
  "confidence": 78,
  "csi": [12, 34, 56, ..., 23]
}
```

## 串口数据格式

串口每 100ms 输出一帧数据，格式如下：

```
$CSI,<方差>,<有人(1/0)>,<RSSI>,<置信度>\n
$WAV,<子载波0>,<子载波1>,...,<子载波55>\n
```

示例：

```
$CSI,12.34,1,-52,78
$WAV,12,34,56,78,23,45,67,89,11,22,...,33
```

## 信号指标解释

WiFi 雷达通过分析 CSI（信道状态信息）检测人员活动，以下是仪表盘和串口输出中各项指标的含义：

### 指标总览

| 指标 | 含义 | 典型范围 | 解读 |
|------|------|----------|------|
| **信号强度 (RSSI)** | 接收信号强度，单位 dBm | -90 ~ -30 dBm | 值越大（绝对值越小）信号越好，-50 dBm 优于 -80 dBm |
| **CSI 方差** | CSI 子载波幅度的波动程度 | 0 ~ 几十 | \> 8.0 可能有人活动，\> 16.0 高概率有人 |
| **置信度** | 人员检测的可信程度 | 0% ~ 100% | 基于方差趋势计算，越高表示检测结果越可靠 |
| **CSI 子载波波形** | 56 个子载波的幅度值 | -128 ~ 127 | 反映多径信道变化，人员移动会导致波形波动 |
| **方差趋势** | 方差的历史波动程度（二阶方差） | 0 ~ 几十 | 值高表示方差变化剧烈，可能对应持续活动 |
| **RSSI 置信度** | 当前版本未独立计算 RSSI 置信度 | — | 仪表盘显示的置信度基于 CSI 方差，RSSI 仅作参考 |

### 详细说明

#### 1. 信号强度 (RSSI)

- **全称**：Received Signal Strength Indication（接收信号强度指示）
- **单位**：dBm（分贝毫瓦），取值通常为负值
- **参考**：
  - -30 ~ -50 dBm：信号极强
  - -50 ~ -70 dBm：信号良好
  - -70 ~ -85 dBm：信号一般
  - -85 dBm 以下：信号微弱
- **来源**：从 WiFi 驱动直接获取（`info->rx_ctrl.rssi`）
- **作用**：RSSI 本身变化也能反映环境扰动，但本项目主要依赖 CSI 方差进行检测。稳定环境中 RSSI 波动小，剧烈波动可能提示干扰

#### 2. CSI 方差

- **全称**：Channel State Information Variance（信道状态信息方差）
- **计算**：对 56 个子载波的幅度数据求方差
- **物理意义**：衡量 CSI 数据的波动程度。**方差越大，说明信道变化越剧烈**，通常由人员移动引起
- **检测阈值**：`PRESENCE_THRESHOLD = 8.0`（默认，可通过 menuconfig 调整）
  - 方差 > 8.0：可能有人活动（结合方差趋势判断）
  - 方差 > 16.0（阈值 × 2）：直接触发检测
- **典型值**：
  - 静态环境（无人）：方差接近 0
  - 有人走动：方差显著升高（10 ~ 50+）

#### 3. 置信度

- **定义**：人员检测结果的可信程度
- **范围**：0% ~ 100%
- **计算公式**：`置信度 = min(max((方差趋势 / 阈值) × 50, 0), 100)`
- **含义**：
  - 0%：完全不确定
  - 50%：中等置信（方差趋势达到阈值水平）
  - 100%：高度确信有人活动
- **依据**：基于方差趋势（二阶方差）与阈值的比值

#### 4. CSI 子载波波形

- **定义**：每个 OFDM 子载波的幅度值序列
- **数量**：56 个子载波（20 MHz 带宽，HT20 模式）
- **数据格式**：`int8_t`，取值范围 -128 ~ 127
- **输出方式**：
  - 串口：`$WAV,12,34,56,...`
  - Web API：`/api/csi` 返回 JSON 数组
- **可视化**：在仪表盘中绘制为 56 个点的波形图，反映多径信道的变化
- **物理意义**：每个子载波对应一个频率分量，人体对不同频率的反射/散射不同，因此波形变化能反映人员活动

#### 5. 方差趋势（二阶方差）

- **别名**：方差的方差、二阶方差
- **计算方法**：
  1. 维护 30 个历史方差值的滑动窗口
  2. 计算窗口内这些方差的方差
- **物理意义**：**衡量方差本身的波动程度**。高值表示方差变化剧烈，可能对应持续的人员活动
- **与置信度的关系**：置信度直接由方差趋势与阈值的比例决定

#### 6. RSSI 置信度

- **当前状态**：本版本未独立计算 RSSI 置信度
- **可能含义**：
  - 基于 RSSI 稳定性的置信度（RSSI 波动小 → 置信度高）
  - 或指整体检测结果的置信度（已由 `confidence` 字段表示）
- **说明**：仪表盘中显示的"置信度"即为基于 CSI 方差变化的置信度，RSSI 仅作为参考信号强度

### 检测逻辑总结

```
1. 采集 CSI → 提取 56 子载波幅度
2. 计算当前帧方差 (variance)
3. 滑动窗口记录历史方差（30 帧）
4. 计算方差趋势 (var_of_var = 方差的方差)
5. 判断：
   - var_of_var > 8.0  → 有人
   - variance > 16.0   → 有人
   - 否则 → 无人
6. 计算置信度 = (var_of_var / 8.0) × 50，限制在 0~100
```

## 仪表盘显示说明

| 显示元素 | 说明 |
|----------|------|
| **实时波形图** | CSI 子载波幅度（56 个点），波动反映环境变化 |
| **方差曲线** | 历史方差变化，突增表示有人活动 |
| **状态标签** | `EMPTY`（无人）/ `OCCUPIED`（有人） |
| **数字指标** | RSSI、方差、置信度同步实时更新 |

## 常见问题

### 烧录失败

- 检查串口线连接是否正确
- 尝试按住 BOOT 键再烧录
- 降低波特率：`idf.py -p COM3 -b 115200 flash`
- 确认串口驱动已安装

### WiFi 连接失败

- 检查 SSID 和密码是否正确
- 确认路由器支持 2.4GHz（ESP32 不支持 5GHz）
- 路由器连接设备数可能已满
- 串口监视器会显示重连日志

### CSI 数据为 0 或无变化

- 确认 ESP32 已成功连接到路由器（STA 模式获取到 IP）
- CSI 采集需要 WiFi 数据流量，路由器需有活跃通信
- 尝试在路由器侧产生一些流量（如其他设备播放视频）

### 检测灵敏度调整

```bash
idf.py menuconfig
# → WiFi Radar Configuration → Presence Detection Threshold
# 默认 8，增大则更不敏感（减少误报），减小则更敏感（可能增加误报）
```

## 技术参考

- [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/)
- [ESP-IDF 快速入门](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/)
- [ESP-IDF 构建系统](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/api-guides/build-system.html)
- [ESP32 WiFi CSI](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/api-guides/wifi.html#csi-channel-state-information)
- [ESP32 技术论坛](https://esp32.com/)
