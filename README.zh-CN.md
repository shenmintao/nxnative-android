# nxnative-android

把自己的 Switch 游戏转换为 Android 应用。基于 [shirai91/nx2android](https://github.com/shirai91/nx2android) 和 [seedds/nxnative](https://github.com/seedds/nxnative)：游戏 ARM64 代码原生运行，运行时提供 Horizon 服务，并把 NVN 图形调用转换为 Vulkan。

当前候选版本 **0.3.0rc2**，仓库为 [shenmintao/nxnative-android](https://github.com/shenmintao/nxnative-android)。仓库和下载包不包含游戏或密钥。

## 启动与导入

Windows 用户从 [Releases](https://github.com/shenmintao/nxnative-android/releases) 下载 `nxnative-android-<版本>-windows-x64.zip`，完整解压后启动 **nxnative-android.exe**，无需另装 Python。Setup 页可下载 Android NDK r27c、.NET SDK 10 和 platform-tools，然后选择自己的 `prod.keys`。

源码需要 Python 3.12 或更新版本：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m nxnative_android tools --download
.\.venv\Scripts\python.exe -m nxnative_android gui
```

也可以双击源码根目录的 `nxnative-android.cmd`，优先使用 `.venv`。项目入口默认把工具、设置、APK 签名密钥和转换器放在启动目录的 `tools/player/`，可用 `NX2A_HOME` 改位置。请保留签名密钥，以便更新 APK 时保留存档；内部 `porter` 命令和环境变量名称继续兼容上游。

1. Setup 页检查通过后，在 Port 页用 **Import NSP / XCI…** 选择游戏文件，或 Browse 选择一个游戏的目录。单文件模式匹配同目录的同游戏更新和 DLC；目录模式只放一个游戏。NSZ/XCZ 会临时解压，需要额外空间。
2. 核对版本、兼容状态和输出目录，点击 Port。
3. 默认生成包含数据的单个 APK，首次启动展开数据。完整 APK 必须小于 **4 GiB**（ZIP32）。大型游戏请选择 **APK + external data**，生成 APK 和数据目录；BotW 约 16 GiB 数据需要外置模式。
4. 设备开启 USB 调试并授权电脑后，可勾选 **Install on the device when done**，同时安装 APK、传输数据。

命令行示例（源码把 `.exe` 换成 `.cmd`）：

```powershell
.\nxnative-android.exe profiles
.\nxnative-android.exe doctor
.\nxnative-android.exe inspect "D:\Games\Game\base.nsp" --json
.\nxnative-android.exe convert "D:\Games\Game\base.xci" -k "D:\keys\prod.keys" --data-mode external --install
```

源码也可运行 `python scripts/build_apk_from_nsp.py <文件或目录> -k <prod.keys>`。

## 操作与存档

支持触屏虚拟手柄、Android 屏幕键盘和实体手柄。连接实体手柄自动隐藏虚拟手柄，断开后恢复用户设置；暂停菜单可调整按键布局。

本版接入真实陀螺仪与加速度计，并保留震动。BotW 望远镜已由用户实际转动平板确认“方向正确，放稳后稳定”；触屏反馈和游戏落地震动均有设备服务记录。其他手机仍需核对传感器方向。

暂停菜单 Game 页可导出、导入存档，桌面端 Saves 页可传输到电脑。导入前会导出原存档。卸载应用会删除应用数据，请先把存档复制到电脑。

## BotW 1.9.0 实测

设备 **TB323FU／SM8850P／Adreno 840／Android 16，均衡模式**；自有初始台地存档，底座输出 **1920×1080**，保留游戏原有 **1280×720–1600×900 动态内部渲染分辨率**，实测窗口为 1600×900；60 帧目标、HOLD 关闭。

| 计划 2 基线，早于本次首次加载改进 | 平均 fps | 迟到间隔（>17.5 ms） | >50 ms 间隔 |
|---|---:|---:|---:|
| 固定视角，两次各 120 秒 | 59.895 | 0.647% | 0 |
| 移动路线，两次各 120 秒 | 59.830 | 0.829% | 0 |
| 首次启动，移动 120 秒 | 59.875 | 0.751% | 0 |

最终选择 **上游通用加锁轮询 spin wait 规则**，关闭额外同步线程绑核，保留原回放线程设置。同条件旧模块为 58.065 fps、迟到 3.579%，仅上游规则为 59.913 fps、迟到 0.570%，长帧合计从 45 次降为 0。我们自己的轮询原型和额外绑核均未显示超出噪声的增益。

保留图形与计算管线预编译、ADPF 改进和上游渲染修复。预编译列表 1,209 条，暖启动移动测量中运行时计算管线创建为 0；会跳过绘制的异步编译默认关闭。

上述基线核心 **150 项单元测试全过**，Vulkan 验证层 **0 错误、3 条未使用片元输出警告**，release 存取档冒烟通过。最终 30 分钟持续测试按用户要求跳过。完整流程未验证，小腿与地面遮挡问题仍在，状态保持 **wip**。不代表原生内部 1080p，也不代表其他设备有相同帧率。

**首次加载跟进（2026-11-02）**：原短路线以外仍有卡顿；扩大路线的逐帧诊断中，11 次 >50 ms 间隔有 6 次主要关联图形管线创建。本候选版移植了“程序创建时后台预取”和“绘制命令前瞻＋有限并行编译”，准备未完成时仍等待正确管线。**156 项源码测试通过，冷首次使用的 Vulkan 验证 0 错误、3 条原有警告**。一次普通 120 秒移动观察为 59.135 fps、16 次 >50 ms 间隔，卡顿仍存在。按用户要求取消 OFF/ON/ON/OFF，性能收益未量化。[跟进记录与适用范围](docs/perf-hitch-followup.md)。

## 设备兼容范围

上述成绩属于 SM8850P；上游另有 Snapdragon 8 Elite／Adreno 830 的游戏记录。前代 SM8750 使用同一通用运行时，但需要单独验证性能。

**天玑 9400／Immortalis-G925 正在独立分支适配**。已连接 V2405A 的驱动支持 ASTC、几何着色器、曲面细分，不支持 BC 纹理采样及 descriptor buffer。渲染器已有 CPU BC 解码和普通描述符回退，当前安装器仍限制配置的 `bcn` 要求；不能把高通结果当作 Mali 的支持或性能承诺。

其他游戏以 [游戏状态](docs/PORT_STATUS.md) 和 `profiles` 为准。[英文首页图库](README.md#games) 保留上游测试记录，本次没有重新测试所有游戏。

## 开发与来源

运行 `python -m unittest discover tests`。性能验收见 [计划 2](docs/perf-plan-2.md)，结论见 [perf-log](docs/perf-log.md)，设计见 [ARCHITECTURE](docs/ARCHITECTURE.md)。许可证沿用 [MIT](nxnative/LICENSE)，第三方组件保留各自许可证。
