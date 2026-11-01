# nxnative-android

本项目基于 [shirai91/nx2android](https://github.com/shirai91/nx2android) 的 Android 原生移植和 [seedds/nxnative](https://github.com/seedds/nxnative) 创建，保留其运行时、游戏配置和 MIT 许可。新增直接导入 NSP/XCI 文件、按所选游戏匹配更新/DLC，以及项目启动入口。

**[中文使用说明](README.zh-CN.md)** · 双击 `nxnative-android.cmd` 启动，或运行 `python scripts/build_apk_from_nsp.py <NSP文件或游戏目录> -k <prod.keys>`。

默认产物为包含游戏数据的 **单个 APK**；大型游戏可选择 `--data-mode external`，生成 **APK + 游戏数据目录**。当前单 APK 写入器使用 ZIP32，完整 APK 必须小于 4 GiB。NSP、XCI 均可直接导入；兼容性取决于逐个适配的 Android 游戏配置。支持触屏虚拟手柄、系统屏幕键盘和蓝牙手柄；连接实体手柄时自动隐藏虚拟手柄，断开后恢复用户的显示设置。以下保留 nx2android 的原始使用和开发说明，具体新增用法见中文说明。

## nx2android upstream

Port your own Nintendo Switch games to Android, natively: the game's own arm64 code runs on the device, with the Switch's system services stood in for by a host and NVN graphics replayed on Vulkan. Pick the game's folder, click **Port**, and get an APK plus the game's data, installed on your device if you like. Bring your own game files and keys; nothing from any game is in this repository.

This is the Android port of [nxnative](https://github.com/seedds/nxnative), which does the same on iPhone and iPad. How it is put together: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Games

Each game is a profile in `nxnative/dm/games/<title ID>/` with an `android.tsv`. Which games play: [docs/PORT_STATUS.md](docs/PORT_STATUS.md); which ones each release has been checked to port: [docs/RELEASE_CHECK.md](docs/RELEASE_CHECK.md). The device's GPU must read BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR).

## For players: port a game without programming

1. Download `nx2android-<version>-windows-x64.zip` from [Releases](https://github.com/shirai91/nx2android/releases/latest), unzip it and start **nx2android.exe**.
2. **Setup** tab: click **Download missing tools** (the Android NDK, the .NET SDK and platform-tools, about 1 GB, from Google and Microsoft), then set your **prod.keys**. Every line turns green.
3. **Port** tab: choose the folder with the game's NSP or XCI files (base game, update, DLC) and click **Port**.
4. Tick **Install on the device when done** with the device plugged in and USB debugging on, or copy the APK and the data folder yourself.

The step-by-step guide is [packaging/INSTALL.md](packaging/INSTALL.md). You need your own game files and `prod.keys`, dumped from your own Switch; the tool never ships or downloads either. NSZ files must be decompressed to NSP first.

## Setup from the repository

Install [Python](https://www.python.org) 3.11 or later and `python -m pip install -r requirements.txt`, then `python -m porter tools --download` (or install them yourself):
- [Android NDK r27c](https://developer.android.com/ndk/downloads) (only the NDK: no Android Studio, SDK or JDK)
- [.NET SDK 10](https://dotnet.microsoft.com/download)
- Android [platform-tools](https://developer.android.com/tools/releases/platform-tools) (adb), to install from the tool
- your own `prod.keys`, dumped from your Switch

## Port a game

The app:

```
python -m gui
```

Check the **Setup** tab, choose the game's folder (the base game NSP or XCI, its update and DLC) on the **Port** tab, and click **Port**.

Or the command line:

```
python -m porter doctor
python -m porter port "D:\Games\NEO" -k "D:\keys\prod.keys" --install
```

The APK (`NeoTheWorldEndsWithYou.apk`) and the data pack (`NeoTheWorldEndsWithYou-data/`) appear in the game folder (`-o` picks another). The first port takes a while (extraction, rewriting the game's code, translating its shaders); later ones reuse what has not changed. `python -m porter -h` lists the commands.

## Layout

- `porter/`: the port pipeline (`python -m porter`).
- `gui/`: the desktop app (`python -m gui`).
- `nxnative/`: nxnative, kept as a git subtree of `seedds/nxnative` (squashed), with the Android runtime in `nxnative/dm/android/` and the game profiles in `nxnative/dm/games/`.
- `packaging/`: the player's bundle (`python packaging/build_bundle.py`).
- `devtools/`: scripts for bringing up a game on a connected device (`devtools/device.json` names the device: `{"serial": "...", "package": "..."}`).
- `probes/`, `RESEARCH.md`: the feasibility study and the first on-device probes.
- `.githooks/`: the commit-msg hook (see Commit messages).

## Building the bundle

```
python -m pip install -r requirements.txt pyinstaller
python packaging/build_bootloader.py
python packaging/build_bundle.py
```

`build_bootloader.py` compiles PyInstaller's launcher on this PC (it needs the Microsoft C++ Build Tools), because a few antivirus engines flag the prebuilt one every PyInstaller app shares. Run it again after installing or upgrading PyInstaller.

`dist/nx2android/` and `dist/nx2android-<version>.zip`: the app, the committed `nxnative/` and the install guide; no toolchains, keys or game files.

## Tests

```
python -m unittest discover tests
```

## Commit messages

[Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/), with commitlint's `config-conventional` types and limits:

```
<type>[(<scope>)][!]: <description>

[body, after one blank line]

[footers: Co-Authored-By: …, BREAKING CHANGE: <what breaks>]
```

- type: `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`, `ci`, `chore`, `style` or `revert`, lower case.
- scope: optional and free, the part touched: `vk`, `android`, `porter`, `gui`, `profile`, `status`, `release`…
- description: no full stop at the end; it may start with a capital (game and API names). Header, body and footer lines at most 100 characters (a lone URL may run longer).
- `!` before the `:` or a `BREAKING CHANGE:` footer marks a breaking change.

```
fix(vk): a transfer read where a run of rasterizer-discard draws ends (XC3 grass flicker)
feat(profile): Tales of Eternia Remastered playable on update 2.0.0 (v65536)
docs(status): bundle 8372959 ported Tales of Eternia Remastered from scratch
chore(release): version 0.1.1
```

`.githooks/commit-msg` refuses other messages (merges, reverts and `fixup!` commits from git pass). Turn it on once per clone:

```
git config core.hooksPath .githooks
```

The commits before 2026-10-28 predate the rule.

## Updating nxnative

```
git subtree pull --prefix nxnative https://github.com/seedds/nxnative.git main --squash
```

## License

MIT, as nxnative (see `nxnative/LICENSE`). Third-party code keeps its own license.
