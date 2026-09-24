# 开发、模拟与烧录

第一版：ESP32-S3、Wi-Fi 热点、网页控制、经典 CAN；两台 H6215 和一台 H3510。
真实硬件默认未就绪。编译成功不代表接线、CAN 报文与机构测试通过。

## 电脑上先跑起来

需要 Node.js 22+、CMake 3.20+ 和 C++17 编译器。Windows 可安装 Visual Studio 的“使用 C++ 的桌面开发”；构建脚本会寻找附带的 CMake。

在仓库根目录运行：

```powershell
npm ci
npm run build
npm run build:host
npm run sim
```

打开 [http://127.0.0.1:8080](http://127.0.0.1:8080)。模拟服务仅监听本机，不连接 USB2CAN 或真实电机。
模拟器直接链接 `firmware/components/robot` 的 C++ 控制与协议代码，Node.js 只负责 HTTP/WebSocket 转发及定时驱动。
界面的“模拟模式”始终显示；模拟反馈和 CSV 标记 `simulation`。

操作顺序：

1. 取得控制权 → 复位 / 准备。
2. 行走：使能左右轮，等到两台均显示已使能，再按住方向按钮；松开后目标归零，控制器按配置减速。
3. 刀片：解锁 → 使能 → 等待反馈 → 启动；停止后下次必须重新解锁、使能。
4. 单机：先全部停止并等待反馈停转，进入“单电机调试”，选择电机并进入测试模式，然后复位、使能、按住测试。刀片仍需独立解锁。
5. 配置：全部停止并等待停转，在单电机页读取、编辑并保存。配置内容仅保存到控制器，不会写电机寄存器。
6. 状态与记录页可导出 CSV、注入模拟故障。清除模拟故障后仍须复位、重新使能。

模拟服务保存配置到忽略版本控制的 `.cache/sim-config.json`。重启保留配置，但不保留控制权、目标、使能或刀片解锁。
自动测试使用各自的临时服务，不依赖日常模拟服务的配置。
浏览器失焦、切后台、关闭或刷新会停止当前操作端的控制；重新打开后手动取得控制权。

## 自动验证

```powershell
npm test
npx playwright install chromium
npm run test:ui
```

`npm test` 包括 TypeScript 检查、网页打包、原生 C++ 编译、C++ 核心测试和 WebSocket 集成测试。
`npm run test:ui` 在桌面与手机尺寸 Chromium 上运行实际页面，截图在 `test-results/`。
浏览器自动测试使用独立端口 18080（可用 `UI_TEST_PORT` 改写）和非持久化模拟配置，不占用日常预览的 8080 端口。
C++ 编译器/CMake 不在 PATH 时，Windows 自动探测 Visual Studio；也可用环境变量 `CMAKE` 指定可执行文件。
Linux/macOS 可用系统 CMake 与 C++17 编译器。原生构建只用于验证和模拟，不能烧录到 ESP32。

## ESP32-S3 固件构建

基线固定为 **ESP-IDF v5.5.1**，目标 `esp32s3`。遵照[该版本官方安装说明](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html)安装环境。
构建默认使用 4 MB Flash 分区布局作为编译基线；这不是已确认的开发板规格。首次烧录前必须核实实际型号、Flash、供电和串口。

在 ESP-IDF PowerShell 环境中，从仓库根目录运行：

```powershell
npm ci
npm run build
.\scripts\build-firmware.ps1 -Profile real
```

如没有进入 ESP-IDF 环境，可显式指定安装位置：

```powershell
.\scripts\build-firmware.ps1 -IdfPath C:\Espressif\frameworks\esp-idf-v5.5.1 -ToolsPath C:\Espressif -Profile real
```

具体路径以本机安装为准。脚本通过 ESP-IDF 自己的 `idf_tools.py export` 导入工具路径，不修改系统全局环境。
本次开发环境安装在用户目录 `.cache/conrad-tools/`；未指定 IDF_PATH 时，脚本会自动寻找该目录中的 v5.5.1 与工具链，因此本机可直接运行 `build-firmware.ps1`。

| 构建             | 命令                          | 结果                                                               |
| ---------------- | ----------------------------- | ------------------------------------------------------------------ |
| 真机默认         | `-Profile real`             | `build/firmware-real/`；板级未确认、所有电机未确认，CAN 不初始化 |
| ESP32 上运行模拟 | `-Profile sim`              | `build/firmware-sim/`；模拟驱动，TWAI 不初始化                   |
| 编辑真机配置     | `-Profile real -Menuconfig` | 只编辑对应构建目录的配置，完成后重新构建                           |

两种固件分别使用 `robot_real` 与 `robot_sim` NVS 命名空间，模拟配置不会自动进入真机。
构建目录中的 `sdkconfig` 不提交；热点密码也不提交。
若日后升级 ESP-IDF，需重新执行两个固件构建及全部自动测试。

在 menuconfig 的 **Conrad robot** 中确认板型号、TX/RX GPIO、经典 CAN 波特率、实体停机等条件后，才可打开 `Board ... checked`。
只设置 GPIO 而没有打开核对开关，仍不输出 CAN。网页中的每台电机还需要单独填写并核对配置。
Flash 容量、启动与 USB 控制台选项在 ESP-IDF 对应菜单中按实物配置；不预设照片中看不到的板级连接。

## 首次烧录和热点

以COM4为例：

先按 [hardware.md](hardware.md) 核实主控与供电，用 USB 给主控上电，动力侧保持停用。
在已激活的 ESP-IDF 环境中，从仓库根目录运行（把 `COMx` 替换为识别到的主控串口）：

```powershell
$buildDir = (Resolve-Path .\build\firmware-real).Path
idf.py -C firmware -B $buildDir -D "SDKCONFIG=$buildDir/sdkconfig" -p COMx flash monitor
```

本机 COM4 模拟固件烧录示例（覆盖板上程序；先退出串口监视器）。先进入模拟固件目录，再执行命令；`flash_args` 引用同目录的应用、bootloader 和分区表：

```powershell
cd "C:\Users\95833\Desktop\WayiProject\project\Conrad-ForestFire-Robot\build\firmware-sim"
& "$env:USERPROFILE\.cache\conrad-tools\idf-tools\python_env\idf5.5_py3.12_env\Scripts\python.exe" -m esptool --chip esp32s3 --port COM4 --baud 460800 --before default_reset --after hard_reset write_flash '@flash_args'
```

此命令已于 2026-09-24 完成实板烧录和校验。只需测试网页时不用重复烧录；代码修改后需先重新构建。

更稳妥的方式是使用构建完成后 ESP-IDF 打印的烧录命令，保持同一个构建目录及 sdkconfig。
没有连接真实开发板时，不执行烧录；仓库实现过程不自动选择串口、不擦除设备。

查看重启后的热点密码，第一步是在 PowerShell 打开串口：

```powershell
& "$env:USERPROFILE\.cache\conrad-tools\idf-tools\python_env\idf5.5_py3.12_env\Scripts\python.exe" -m serial.tools.miniterm COM4 115200
```

第二步：

按一下开发板上的RESET，窗口会出现类似：

```text
SSID: Conrad-3391 | password: 新密码 | http://192.168.4.1
```

按 `Ctrl + ]` 退出串口监视器，释放 COM4。热点名称由设备 MAC 派生，同一块板通常不变；随机密码随重启变化。

控制台打印热点名 `Conrad-XXXX`、密码和 `http://192.168.4.1`。
未设置有效密码时，每次启动生成随机密码；固定场地可在 menuconfig 设置私有的 8–63 字符 ASCII 密码。
手机/电脑连接这个热点后用浏览器打开地址；现场不需要路由器、互联网、CDN 或外部字体。
浏览器最多四个会话，只有一个会话能控制运动；其他会话只读，但可以发送全部停止。

首次只验证网页与停止状态；实际电机按 [acceptance.md](acceptance.md) 逐步开放。
配置 NVS 初始化失败不会自动擦除旧数据，配置保存会返回失败；排查日志后再决定恢复方式。

## 代码与故障定位

| 模块                                  | 作用                                                                      |
| ------------------------------------- | ------------------------------------------------------------------------- |
| `firmware/components/robot`         | 独立于 ESP-IDF 的状态机、配置校验、JSON 协议、两型号 CAN 编解码、模拟驱动 |
| `firmware/main`                     | FreeRTOS 10 ms 控制任务、TWAI、NVS、热点、HTTP/WebSocket 和嵌入资源       |
| `web`                               | 三页中文界面、100 ms 心跳与状态轮询、曲线、CSV                            |
| `host` 与 `scripts/simulator.mjs` | 原生 C++ 模拟控制器及本地网页网关                                         |
| `tests`                             | 核心状态机、协议、编解码、故障链路及浏览器交互验证                        |

`board_unverified`：检查板级配置；`motor_offline`：检查动力、CAN、反馈 ID 与模式；`enable_unconfirmed`：未得到有效使能反馈，不应跳过检查；`bus_fault`：真机传输错误保持锁定，检查接线和设备后重启控制器，再手动复位与使能。
故障复位只复位控制器状态，不会自动清除电机内部故障、标定电机、保存零点或更新固件。
