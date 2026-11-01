# nxnative-android

将本机的 Switch NSP/XCI 游戏转换成 Android 应用。本项目基于 [nx2android](https://github.com/shirai91/nx2android)，使用其 nxnative Android 移植：ARM64 代码改写、Horizon 服务兼容层、NVN 到 Vulkan 图形后端、触屏手柄、音频和存档。

默认生成包含游戏数据的 `<游戏>.apk`，首次启动会在应用自己的目录展开数据，显示进度；中断后再次启动可以继续。当前 APK 写入器使用 ZIP32，包含数据和签名的完整 APK 必须小于 4 GiB。大型游戏请选择 **APK + external data** 或 `--data-mode external`，生成 `<游戏>.apk` 和 `<游戏>-data/`，安装功能会一起传输。

## 启动

Windows 双击 `nxnative-android.cmd`。脚本优先使用 `.venv`，否则使用 PATH 中的 Python。源码环境需要 Python 3.12 或更新版本（当前上游部分 Python 工具使用了 3.12 语法）、.NET SDK 10、Android NDK r27c，以及 `requirements.txt` 中的依赖。

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m nxnative_android tools --download
.\.venv\Scripts\python.exe -m nxnative_android gui
```

项目入口默认把下载工具、设置、签名密钥和编译后的着色器转换器放在 `tools/player/`，该目录不进入 Git。设置 `NX2A_HOME` 可以指定其他位置；直接运行上游 `python -m porter` 使用上游的默认设置目录。

## 导入并转换

1. 在 Setup 页设置本机的 `prod.keys` 路径，确认 NDK、.NET 和 Python 依赖可用。
2. 在 Port 页点击 **Import NSP / XCI…** 选择游戏文件，或 Browse 选择游戏目录。两种格式都可直接作为完整输入。工具读取容器内的 CNMT，识别本体、更新和 DLC，也支持三者合在一个文件中的包。单文件模式会匹配同目录中同一个游戏的更新和 DLC；其他游戏会被排除。目录模式要求一个目录只放一个游戏。
3. 检查识别出的游戏、版本、兼容状态和输出目录，点击 Port。
4. 默认得到单个 APK；外置数据模式得到 APK 和数据目录。可连接开启 USB 调试的设备，勾选 Install on the device when done 一起安装。

也可以使用与 nxnative 打包脚本对应的命令：

```powershell
.\.venv\Scripts\python.exe scripts/build_apk_from_nsp.py "D:\Games\Game\base.nsp" -k "D:\keys\prod.keys"
```

其他常用命令：

```powershell
.\nxnative-android.cmd doctor
.\nxnative-android.cmd profiles
.\nxnative-android.cmd inspect "D:\Games\Game\base.nsp" --json
.\nxnative-android.cmd convert "D:\Games\Game" -k "D:\keys\prod.keys" --out "D:\Games\Output"
.\nxnative-android.cmd convert "D:\Games\Game" -k "D:\keys\prod.keys" --install
.\nxnative-android.cmd convert "D:\Games\Game\base.xci" -k "D:\keys\prod.keys" --data-mode external --install
.\nxnative-android.cmd convert "D:\Games\Game\base.xci" -k "D:\keys\prod.keys" --data-mode external --fps60
```

游戏和 keys 均从用户指定的本机路径读取。游戏的提取、改写和编译缓存写在游戏文件旁的 `<title ID>_build/`，重复转换会复用有效缓存。NSZ/XCZ 需要先解压为 NSP/XCI；无 ticket 的文件需要在文件名中保留 `[16位title ID]` 以便识别。提供 keys 后，工具会进一步读取内部元数据，确定实际版本和合包中的更新、DLC。

## 控制与着色器

没有实体手柄时可使用屏幕虚拟手柄；检测到蓝牙或 USB 游戏手柄后，自动隐藏虚拟手柄。断开后恢复用户的虚拟手柄显示设置，并清除残留的按键和摇杆状态。游戏的文本输入会调用 Android 屏幕键盘，可输入中文等 Unicode 文本；文本输入与虚拟手柄是否显示独立。

暂停菜单的 **Pad layout** 可循环选择 Auto、Xbox、Nintendo，并保存选择。Auto 根据设备名称和厂商标识识别 Nintendo 手柄，其余使用 Android 标准的 Xbox 布局；Xbox 模式按字母映射 A/B/X/Y，Nintendo 模式保持 Switch 的按键位置。系统屏幕键盘的确认/取消也使用这套映射。多模式手柄若报告的身份与实际键帽不符，可手动切换。

暂停菜单新增 **Show FPS**，可显示帧率并保存开关。转换时可勾选 **Target 60 FPS** 或使用 `--fps60`：优先采用游戏配置已有的帧率适配，否则尝试导入与主程序构建 ID 匹配的 FPSLocker 配置。它改变目标帧率，实际速度仍取决于设备和场景；没有匹配配置时保留游戏原有帧率。

着色器提取和转换为 SPIR-V 在电脑转换阶段进行；平板上的 Vulkan 驱动负责为该 GPU 创建和缓存图形管线。首次遇到新的绘制组合时，平板仍可能有管线编译开销。没有预先提取到的动态着色器沿用上游捕获机制，详见架构说明。

## 兼容范围

以 [当前游戏状态](docs/PORT_STATUS.md) 和 `profiles` 命令为准。已核对 nx2android **`ba91663`** 和 nxnative **`ac9598a`**（2026-10-29），合入相关通用运行时修正，保留本地转换与控制功能。上游包含 11 个标为 playable 的 Android 配置、1 个 boots 配置、1 个 experimental 配置和 1 个 wip 配置；本项目另有下述《旷野之息》wip 配置。上游状态是维护者的测试记录，不代表本机已对这些游戏完成实测。

本项目另加入《旷野之息》1.9.0 的 wip 配置：已在 TB323FU / Adreno 840 上验证开场、中文界面、存取档、手柄和触屏输入，并修复复苏神庙中人物发黑、场景材质被覆盖的问题。通用的 D16 只读深度复制优化，在同一版本的开/关/开对照中测得 45.90 / 34.77 / 45.63 FPS。进一步启用带格式兼容回退的色彩压缩后，同版开/关/开对照为 53.17 / 45.78 / 52.67 FPS，发布版的 60 帧解限诊断为 52.43 FPS。主场景仍为 720p，日常 30 帧配置仍有偶发长帧；尚未达到真正的 1080p60，完整流程和持续性能仍待验证，暂不标为正常可玩。该游戏约 16 GiB 的数据需要使用外置数据模式。

该 Android 配置显式将帧率目标设为 30。打包时勾选 **Target 60 FPS**，或使用命令行 `--fps60`，会覆盖为 60 帧目标；实际帧率仍取决于设备和场景。游戏的移植配置本身已有 60 帧支持，当前看到的 30 帧并非不可更改的引擎上限。

目标设备需要 ARM64 Android、Vulkan 和 BC 压缩纹理支持。上游主要在搭载 Snapdragon 8 Elite / Adreno 830 的 Lenovo Legion Y700 上测试；当前不支持 Mali/PowerVR。Vulkan/OpenGL 游戏渲染接口和需要 Mac 解码步骤的游戏尚不能由这个 Android 后端处理。`--experimental` 可尝试其他游戏，但生成 APK 并不证明它能启动或游玩。

## 验证与来源

```powershell
.\.venv\Scripts\python.exe -m unittest discover tests
```

单元测试使用合成的 PFS0 文件验证导入、游戏选择、更新/DLC 匹配和现有打包逻辑，不包含商业游戏。完整转换和游玩验证需要实际 NSP、匹配的 keys 和目标 ARM64 设备。

核心与上游入口保留在 `porter/`、`gui/`、`nxnative/`。项目入口为 `nxnative_android/`，对应打包脚本为 `scripts/build_apk_from_nsp.py`。上游架构说明见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，原始安装说明见 [packaging/INSTALL.md](packaging/INSTALL.md)。

许可证沿用 nxnative 的 [MIT](nxnative/LICENSE)；第三方组件保留各自许可证，见 [nxnative/dm/third_party](nxnative/dm/third_party)。
