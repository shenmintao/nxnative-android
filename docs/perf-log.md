# BotW 底座 1080p60 执行记录

## 2026-11-01：阶段 0
- 目标：BotW 1.9.0（v1114112）、TB323FU / Android 16；底座输出 1920×1080，内部动态 1280×720–1600×900，`DM_RENDER_SCALE` 不设，目标 60 fps。
- 场景：初始台地自有存档 `plateau-open-area-owned-seed.tar`（SHA-256 `ef69bda1…`）；每轮重置副本，主存档不参与游戏运行。
- 验收：预热 60 s、SurfaceFlinger 实际呈现测 120 s；固定 ×2 ≥59.5 fps、>17.5 ms ≤1%、无 >50 ms；移动 ×2 ≥59 fps、>17.5 ms ≤2%、>50 ms ≤2 次、无 >100 ms；另按计划做 30 分钟浸泡、验证层和正式版冒烟。
- 分支 `perf/docked-1080p60`；初始提交 `dde9db82`，与 `backup/codex-goal-2026-11-01` 比较，仅增加根 AGENTS.md 和 perf-plan.md。
- 新脚本：`<BUILD>/android/bench/bench.py`、`present_trace.py`；支持所需参数、基线组、按轮原始数据和 results.csv；A/B 固定视角顺序为 OFF/ON/ON/OFF。
- 离线验证：存档、1,063 个预热 key、驱动缓存、Perfetto 工具及回退 APK 校验通过；测量/恢复保护测试 9/9；仓库 unittest 104 项，0 失败、22 跳过（默认 PATH 无主机 C 编译器）。
- 路线：左摇杆前/后各 1 s，再右摇杆水平 0.3 s；无跳跃和上下转视角，开始/结束截图在阶段 1 首次移动时验看，最多修两次。
- 限制：现有 APK 只按 600 帧日志提供管线创建计数；CSV 明确标注范围，不能把它当成精确驱动编译次数，2a 验收仍需补齐证据。
- 本阶段未操作平板；下一步只读确认设备空闲，然后执行阶段 1。
