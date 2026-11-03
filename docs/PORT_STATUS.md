# Games

Upstream records below are primarily from a Lenovo Legion Y700 (Snapdragon 8 Elite / Adreno 830). Local BotW verification is listed separately. Mali / Immortalis is under investigation on a separate branch and is not a verified release target.

| | |
|:---:|:---:|
| ![13 Sentinels: Aegis Rim](images/13-sentinels.jpg)<br>13 Sentinels: Aegis Rim<br><sub>update 1.10 · 60 fps</sub> | ![Advance Wars 1+2: Re-Boot Camp](images/advance-wars-1-2-re-boot-camp.jpg)<br>Advance Wars 1+2: Re-Boot Camp<br><sub>base game 1.0.0 · 60 fps (30 in battle on Switch)</sub> |
| ![Donkey Kong Country Returns HD](images/donkey-kong-country-returns-hd.jpg)<br>Donkey Kong Country Returns HD<br><sub>update 1.1.0 · 60 fps</sub> | ![Graveyard Keeper 2](images/graveyard-keeper-2.jpg)<br>Graveyard Keeper 2<br><sub>update 1.0.4 · 60 fps (30 on Switch)</sub> |
| ![Hades II](images/hades-ii.jpg)<br>Hades II<br><sub>update 1.0 · 60 fps</sub> | ![Mario vs. Donkey Kong](images/mario-vs-donkey-kong.jpg)<br>Mario vs. Donkey Kong<br><sub>update 1.0.1 · 60 fps</sub> |
| ![NEO: The World Ends with You](images/neo-the-world-ends-with-you.jpg)<br>NEO: The World Ends with You<br><sub>update 1.03</sub> | ![Super Mario Bros. Wonder](images/super-mario-bros-wonder.jpg)<br>Super Mario Bros. Wonder<br><sub>update 1.2.1 · 60 fps, dips to 40</sub> |
| ![Tales of Eternia Remastered](images/tales-of-eternia-remastered.jpg)<br>Tales of Eternia Remastered<br><sub>update 2.0.0 · 60 fps</sub> | ![Unicorn Overlord](images/unicorn-overlord.jpg)<br>Unicorn Overlord<br><sub>update 1.05 · 60 fps · **boots**: saving and a full battle not tested yet</sub> |
| ![Unpacking](images/unpacking.jpg)<br>Unpacking<br><sub>update 2.01 · 60 fps · played by touch</sub> | ![Viractal: Will You Trust Your Party?](images/viractal.jpg)<br>Viractal: Will You Trust Your Party?<br><sub>update 1.4.5</sub> |
| ![Xenoblade Chronicles 3](images/xenoblade-chronicles-3.jpg)<br>Xenoblade Chronicles 3<br><sub>update 2.2.0 · 30 fps · **work in progress**: some effects and models don't draw yet</sub> |  |

Which games each release has been checked to port: [RELEASE_CHECK.md](RELEASE_CHECK.md).

## Local bring-up: Breath of the Wild

| Game | Title ID | Status |
|---|---|---|
| The Legend of Zelda: Breath of the Wild | `01007EF00011E000` | **work in progress**: 1.9.0 on TB323FU (SM8850P / Adreno 840, Android 16), balanced; docked1080p/stock DRS sampled1600x900. On2026-11-03 pair-warming release `728b8a49...` passes isolated save/reload/30 s smoke, RED0/default ON; user approves shipping despite unchanged RSS criterion4 FAIL (+168.735MB>100MB). Single OFF/ON heavy23->3, late1.407->0.724%, second gaps1->0; end views/draws diverge, no general FPS gain claim.165 source tests=162 pass/3 optional skips; V0 errors/3 baseline warnings. Retained median pacing has its separate earlier A criterion2 shipping override. Rollbacks06e382a5/bcd840c1; line/integer variants, ~61MB cache/memory and remaining first compiles open. Prediction has no measured benefit. Gyro/rumble verified;30-minute soak waived, full gameplay/lower-leg ground occlusion unverified/unresolved. |

Performance results and historical experiments: [perf-log.md](perf-log.md); completed acceptance: [perf-plan-2.md](perf-plan-2.md).
