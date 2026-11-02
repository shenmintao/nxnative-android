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
| The Legend of Zelda: Breath of the Wild | `01007EF00011E000` | **work in progress**: 1.9.0 on TB323FU (SM8850P / Adreno 840, Android 16), balanced mode; docked 1920x1080 output, stock DRS sampled at 1600x900. Historical Plan 2 baseline fixed 59.895 fps / moving 59.830 fps. Candidate 0.3.0rc2 adds program prefetch and bounded upcoming-pipeline builds; 156 source tests, Vulkan zero errors / three unused-output warnings, real gyro, rumble and isolated release `bcd840c1...` save/load smoke pass. One ordinary 120 s moving observation: 59.135 fps, 16 >50 ms / five >100 ms intervals, max 200.011 ms. Four-window comparison waived; gain unquantified and hitches remain. Final 30-minute soak waived; full gameplay unverified and lower-leg ground occlusion remains. |

Performance results and historical experiments: [perf-log.md](perf-log.md); completed acceptance: [perf-plan-2.md](perf-plan-2.md).
