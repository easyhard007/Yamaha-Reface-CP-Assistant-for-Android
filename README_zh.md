# Yamaha Reface CP Assistant for Android（中文版）

专为 **Yamaha Reface CP** 键盘设计的原生 Android 辅助应用。它通过 MIDI 连接您的乐器，提供实时和弦检测、升降调、智能自动踏板、低音增强、箱鼓节奏引擎与自动伴奏、节拍跟踪与 tap tempo，以及基于 SoundFont 的音频回放——全部由低延迟原生音频引擎（[Oboe](https://github.com/google/oboe)）和内嵌 WebView UI 驱动。

本项目是基于 Web 的 [Yamaha-Reface-CP-Assistant](references/Yamaha-Reface-CP-Assistant) 的 Android 移植版本，使用 Kotlin + JNI/C++ 原生音频架构重新构建。

## 功能特性

- 🎹 **实时和弦检测** —— 加权 chroma 向量模板匹配，支持 17 种和弦类型与转位和弦（斜杠和弦）。
- 🥁 **箱鼓节奏引擎** —— 程序化生成箱鼓律动（bass/tone/tip/slap），力度感应 round-robin 采样，独立乐器混响/EQ。
- 🎸 **贝斯助手** —— 基于和弦根音的贝斯合成器，支持制音/闷音技法、和弦音概率选择、能量感应动态变化。
- 🎵 **自动伴奏** —— 基于 style 文件的伴奏轨，支持场景切换（intro/main/fill/ending）。
- ⏱️ **节拍跟踪与速度检测** —— PLL 速度检测 + 栅栏模板相位分析，散点图实时显示 BPM。
- 👆 **Tap Tempo** —— 点击 sync-zone 手动打拍定速，自动以 50% 音量启动节奏。
- 🔁 **转调（Transpose）** —— 无缝升降调，不影响实际演奏的音符。
- ✂️ **分割点（Split）** —— 可调节的键盘分割点。
- 🦶 **自动延音（Auto-Sustain）** —— 智能自动踏板，按音乐逻辑保持音符。
- 🔊 **低音增强（Bass Enhance）** —— 加入合成的低音层，丰富低频表现。
- 🎼 **SoundFont（`.sf2`）回放** —— 通过原生 Oboe 音频引擎（FluidLite）渲染高质量乐器音色。
- 🖥️ **虚拟钢琴** —— 屏幕键盘，实时显示音符。

## 环境要求

- 支持 **MIDI**（`android.software.midi`）的 Android 设备，系统 **Android 8.1（API 27）** 及以上。
- 与 Yamaha Reface CP 的 USB/MIDI 连接（通常需要 USB-OTG 数据线）。
- Android Studio（含 NDK 与 CMake 3.22.1）用于从源码构建。

## ⚠️ 手动放置 SoundFont（必须）

由于 SoundFont 文件体积较大（约 356 MB），本仓库**未包含**该文件。在构建之前，您必须手动下载，否则应用将没有乐器声音。

1. 下载名为 **`JJazzLab-SoundFont.sf2`** 的文件。
2. 将其放入以下文件夹：

   ```
   Yamaha Reface CP Assistant for Android\app\src\main\assets\
   ```

   最终路径应为：

   ```
   app\src\main\assets\JJazzLab-SoundFont.sf2
   ```

3. 重新构建项目。应用在运行时会加载此 SoundFont 以合成乐器音色。

> 大多数 `.sf2` 文件因体积过大被 `.gitignore` 排除，但 `033FingerBass-LiveHQNaturalGM.sf2`（贝斯音源）已纳入版本管理。

## ⚠️ 手动放置 Style 文件（可选）

自动伴奏支持 **Yamaha `.sty` 格式**的伴奏文件。将其放入：

```
app\src\main\assets\styles\
```

这些 style 文件同样可从 **JJazzLab** 获取。

## 关于 JJazzLab

本应用使用的 SoundFont 与 style 文件来自 **JJazzLab**。JJazzLab 是一个**开源应用**，其源代码仓库地址为：

- **https://github.com/jjazzboss/JJazzLab**

关于 SoundFont 的使用条款，请参阅 JJazzLab 仓库及其许可证。

## 构建方法

1. 克隆本仓库。
2. 完成上方的[手动放置 SoundFont](#-手动放置-soundfont必须)步骤。
3. 在 Android Studio 中打开项目（NDK 和 CMake 3.22.1 会自动配置）。
4. 在支持 MIDI 的 Android 设备上构建并运行。

> 注意：`app/build.gradle.kts` 中包含指向本地 keystore 的 release 签名配置。在构建 release 版本前，请根据您自己的签名设置进行调整或移除。

## 使用方法

1. 在设备上启动应用。
2. 打开**设置**（`?` 按钮），选择 **SoundFont** 与 **Instrument（乐器）**。
3. 选择连接到 Reface CP 的 **MIDI 端口**，然后点击**启动**。
4. 弹奏您的 Reface CP —— 和弦、节奏、贝斯将实时响应。

### 伴奏辅助模式（默认）

根据您的 MIDI 输入自动生成 **Cajon 箱鼓打击乐** 和 **和弦驱动的贝斯** 伴奏：

- **能量滑条** —— 控制箱鼓律动的节奏密度。
- **伴奏音量** —— 箱鼓的混音电平。设为 0 时，点击 **sync-zone** 可手动打拍定速（tap tempo）；应用也会根据您的弹奏速度自动推算 BPM。
- **贝斯音量** —— 贝斯合成器的混音电平。当 Reface CP 上 Bass Enhance 关闭时，CC#81 旋钮改为控制贝斯音量。
- **Sync-zone** —— 点击回到第 1 拍（伴奏音量 > 0）或 tap tempo 打拍（伴奏音量 = 0）。

### 自动伴奏模式

播放 **Yamaha `.sty` 格式**的结构化伴奏轨：

1. 构建前将 `.sty` 文件放入 `app/src/main/assets/styles/`。
2. 在 UI 中切换到自动伴奏模式。
3. **选择 style** —— 应用加载其场景（intro / main / fill / ending）。
4. 点击**场景按钮**切换段落，伴奏自动跟随检测到的和弦与速度播放。

## 技术栈

- **Kotlin** + AndroidX
- **原生 C++（JNI）** 音频引擎，使用 **Oboe** + **FluidLite** 实现低延迟 SoundFont 渲染
- **WebView** UI（HTML/CSS/JS，Canvas 2D 散点图）
- CMake 3.22.1，16 KB 页对齐（适配 Android 15+）

## 许可证

本项目许可证请参阅 [LICENSE](LICENSE) 文件。随附的 SoundFont 及第三方库保留其各自的许可证。
