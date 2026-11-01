# Games

Tested on a Lenovo Legion Y700 (Snapdragon 8 Elite, Adreno 830). The device's GPU must read BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR).

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
| The Legend of Zelda: Breath of the Wild | `01007EF00011E000` | **work in progress**: 1.9.0 on TB323FU (SM8850P / Adreno 840, Android 16); docked 1920x1080 output, game dynamic internal resolution, 60 fps target, HOLD off. Integrated builds pass 111 unit tests and Vulkan validation with 0 errors; final performance and release-install acceptance in progress. Full gameplay remains unverified. |

Performance results and historical experiments: [perf-log.md](perf-log.md); current acceptance: [perf-plan.md](perf-plan.md).
