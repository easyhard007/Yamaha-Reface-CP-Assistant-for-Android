# Yamaha Reface CP Assistant for Android（中文版）

这是一个面向 **Yamaha Reface CP** 的原生 Android 辅助应用。应用通过 Android MIDI 与琴连接，实时分析音符、和弦、调性、速度和拍位，并通过低延迟原生音频引擎提供节奏与音色辅助。

当前产品包含两种演奏模式：

- **鼓循环（默认）**：播放随应用打包的立体声 Opus 鼓循环片段，根据当前 BPM 变速，并在小节边界切换段落。
- **箱鼓助手**：原“伴奏辅助模式”的保留与改名版本，继续提供程序化箱鼓律动和和弦驱动的贝斯助手。

原先基于 Yamaha `.sty` 文件的**自动伴奏模式**已经被“鼓循环”取代。`.sty` 解析器和播放器源码仍作为遗留兼容/研究代码保留，但它们已不是当前 UI 中可操作的产品模式。

本项目源自 Web 版 [Yamaha-Reface-CP-Assistant](references/Yamaha-Reface-CP-Assistant)，Android 版本采用 Kotlin、JNI/C++、[Oboe](https://github.com/google/oboe)、FluidLite 与 WebView UI 重新构建。

## 功能特性

- 🥁 **鼓循环** —— 支持 Verse、Verse 2、Pre-Chorus、Chorus、Fill 等片段。绿色填充表示“当前正在播放”，快速闪烁的白色外框表示“目标 variation”；两者可以同时出现在同一按钮上，普通 variation 切换仍在下一小节第一拍执行。
- ⚡ **快速起播与后台高质量渲染** —— Android MediaCodec 解码 Opus；Signalsmith Stretch 负责低延迟应急起播，SBSMS 以“头 0.5 秒、末小节、完整音频”四级状态在独立后台队列中生成高质量版本。
- 🔄 **BPM 匹配** —— 鼓循环按照当前 BPM 做时间拉伸，设计目标是不改变音高；BPM 两侧提供 −/+ 按钮（轻触单步、长按连调并带系统触感反馈），并保留 ×2/÷2。界面请求不再等待 150 ms 状态轮询；播放中会读取各 variation 的 JSON `cut_points`，选择距离当前位置至少 50 ms 的下一个切分点，并在该音频位置精确采用新速度，而不再等待小节第一拍。
- 🎚️ **平滑切换** —— 片段间短交叉淡化、循环边界淡化、停止时 1 秒淡出，并可显示从触发到首次出声的切换延迟。
- 🪘 **箱鼓助手** —— 程序化生成 bass/tone/tip/slap 律动，使用多力度层和 round-robin WAV 采样。
- 🎸 **贝斯助手** —— 根据和弦生成贝斯，包含制音/闷音、能量相关动态和专用贝斯 SoundFont。
- 🎹 **实时和弦与调性检测** —— 加权 chroma 模板匹配、斜杠和弦、和弦组成音、调性与罗马数字分析。
- ⏱️ **节拍与速度检测** —— 32 分音符级调度、Tap Tempo、BPM 显示和滚动音符散点图。
- 🦶 **智能自动延音** —— 自动保持音符，并在检测到和声碰撞时短暂松开踏板。
- 🔊 **低音增强** —— 将加权的低八度音符回发给 Reface CP。
- 🔁 **转调与分割点** —— 通过 Reface CP SysEx 转调，并可调整分析用键盘分割点。
- 🎼 **SoundFont 叠加音色** —— 通过 FluidLite 和 Oboe 混合可选的演奏叠加音色。
- 🖥️ **虚拟钢琴** —— 实时显示音符、踏板、和弦和播放状态。

## 环境要求

- 支持 `android.software.midi`、运行 **Android 8.1（API 27）**或更高版本的 Android 设备。
- Yamaha Reface CP 的 USB/MIDI 连接；通常需要 USB-OTG 转接线。
- 当前构建配置要求 **arm64-v8a** 设备。
- 从源码构建时需要 Android Studio、Android NDK 与 CMake 3.22.1。

## 手动放置 SoundFont（必须）

主 SoundFont 约 356 MB，因此未包含在仓库中。构建前请：

1. 下载 `JJazzLab-SoundFont.sf2`。
2. 将其放到：

   ```text
   app/src/main/assets/JJazzLab-SoundFont.sf2
   ```

3. 重新构建应用。

大多数 `.sf2` 文件都被 `.gitignore` 排除。贝斯助手使用的较小文件 `033FingerBass-LiveHQNaturalGM.sf2` 已纳入仓库。

## 鼓循环资源格式

鼓循环包位于 `app/src/main/assets/drumloops/`。每个包是一个独立文件夹：

```text
drumloops/
└── PACK_NAME/
    ├── PACK_NAME.json
    ├── VERSE_01.opus
    ├── CHORUS_01.opus
    ├── FILLS_01.opus
    └── ...
```

JSON 元数据提供循环包 BPM、拍号、采样率，以及与 Opus 文件名对应的 `variations` 对象。当前随项目提供的循环包是 48 kHz、立体声、4/4，包含 Intro、Verse、Pre-Chorus、Chorus、Fill、Bridge、Ending 和 Pickup 等多类片段。当前主网格开放 Verse、Verse 2、Pre-Chorus、Chorus 和 Fill；其他段落按钮仍是后续扩展入口。

`app/src/main/assets/styles/` 中仍保留旧 `.sty` 资源，供遗留解析/播放代码使用；当前“鼓循环”模式不依赖这些文件。

## 构建方法

1. 克隆本仓库。
2. 完成上方的[手动放置 SoundFont](#手动放置-soundfont必须)。
3. 使用 Android Studio 打开项目。
4. 如果 IDE 提示，请安装对应的 SDK、NDK 与 CMake 组件。
5. 在支持 MIDI 的 arm64 Android 设备上构建并运行。

`app/build.gradle.kts` 当前包含与开发者本机绑定的 release 签名配置。生成自己的发布包前，请替换或移除该配置，不要直接沿用仓库中的本地签名设置。

## 使用方法

1. 将 Reface CP 连接到 Android 设备并启动应用。
2. 打开设置，选择 MIDI 设备、SoundFont 和叠加音色，然后启动连接。
3. 弹奏键盘；音符状态、和弦、速度信息和虚拟键盘会实时更新。

### 鼓循环（默认）

1. 将模式开关保持在 **DRUM LOOPS / 鼓循环**。
2. 选择一个鼓循环包；应用会读取元数据，并把 Opus 资源载入内存。在循环包和待播放片段完整就绪前，播放按钮保持灰色且不可点击。
3. 应用默认选中第一个 Verse 片段。等待 **▶** 变成绿色后点击，即可开始播放并把节拍同步到第一小节。
4. 播放过程中点击其他片段，该片段会进入待播放状态，并在下一小节第一拍切换。
5. 点击 BPM 两侧的 **−** 或 **+** 可单步减/加 1，也可以用 **×2/÷2** 大幅调整。目标 BPM 会立即显示并直接送到准备流程，不再等待原先最长 150 ms 的状态轮询。按住 0.5 秒后约每 50 ms 连续变化一次，并在设备开启触感反馈时产生类似系统键盘的短振动。播放中一次只执行一个变速任务：它立即为当前目标启动 Signalsmith，从当前速度下至少 50 ms 之后的下一个元数据切分点切换；长按产生的新数值只覆盖最新目标，本轮切换完成后立即解锁，若目标仍不同便自动启动下一轮。鼓循环未播放时 BPM 立即生效。
6. 点击 **■**，鼓循环会用 1 秒淡出停止。

片段按钮还会显示播放目标和后台准备状态。未准备的片段先通过 Signalsmith 快速路径起播；Signalsmith 可按“切分点 → 结尾、开头 → 切分点”的顺序解码并拉伸完整一轮。SBSMS 对当前片段和目标片段采用“头 0.5 秒 → 立即连续渲染到完整”的顺序；其他片段采用“头 0.5 秒 → 最后一小节 → 中间部分”的顺序。MediaCodec 暂停时会保留 native SBSMS 流和原始音频续算游标，因此恢复 state 1 后会继续生成头部之后的数据，而不会替换正在播放的0.5秒缓冲。高质量状态 1 可从片头流式播放，状态 2 还可从最后一小节任意时间点起播，状态 3 可从任意时间点起播；旋转渲染会先处理 `t → 结尾`，再处理 `0 → t`，所以回到片头后仍可继续循环。本阶段只完成变速的 cut-point 切换；正在播放的低质量版本如何在任意 cut point 接替为高质量版本留待下一阶段。

### 箱鼓助手

把模式开关切换到 **CAJON ASSIST / 箱鼓助手**。这就是原“伴奏辅助模式”的保留与改名版本。

- **音量**：控制箱鼓采样混音音量。
- **贝斯**：控制和弦驱动贝斯；它与 Bass Enhance 互斥。
- **能量**：设置律动密度下限；实际弹奏的音符密度还可以自动提高能量。
- **Sync 区域**：箱鼓有声时点击会重置到第一拍；箱鼓音量为零时可连续点击进行 Tap Tempo，达到足够次数后会自动启动节奏。

## 技术架构

- **Kotlin / Android MIDI**：设备连接、WebView 桥、Opus 解码、鼓循环渲染调度和 Android 生命周期。
- **JNI / C++17**：MIDI 状态、和声与速度分析、节拍调度、箱鼓/贝斯生成、时间拉伸、循环播放与最终混音。
- **Oboe + FluidLite**：低延迟立体声输出和 SoundFont 合成。
- **SBSMS + Signalsmith Stretch**：后台高质量与按需低延迟时间拉伸。
- **WebView（HTML/CSS/JavaScript）**：演奏界面与可视化。

实现细节请参阅[技术说明.md](技术说明.md)和[代码文件结构说明.md](代码文件结构说明.md)。

## 许可证

项目许可证见 [LICENSE](LICENSE)。随附采样、SoundFont 与第三方库保留各自许可证。
