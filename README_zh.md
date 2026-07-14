# Yamaha Reface CP Assistant for Android（中文版）

专为 **Yamaha Reface CP** 键盘设计的原生 Android 辅助应用。它通过 MIDI 连接您的乐器，提供实时和弦/调性可视化、升降调、智能自动踏板、低音增强以及基于 SoundFont 的音频回放——全部由低延迟原生音频引擎（[Oboe](https://github.com/google/oboe)）和内嵌的 Web UI 驱动。

本项目是基于 Web 的 [Yamaha-Reface-CP-Assistant](references/Yamaha-Reface-CP-Assistant) 的 Android 移植版本，使用 Kotlin + JNI/C++ 原生音频架构重新构建。

## 功能特性

- 🎹 **实时和弦与调性追踪** —— TSD（主/下属/属）和弦功能可视化，并实时识别调性/音阶。
- 🌈 **色彩映射灯光面板** —— 和弦与调性映射为颜色，呈现在交互式灯光面板上。
- 🔁 **转调（Transpose）** —— 无缝升降调，不影响实际演奏的音符。
- ✂️ **分割点（Split）** —— 可调节的键盘分割点。
- 🦶 **自动延音（Auto-Sustain）** —— 智能自动踏板，按音乐逻辑保持音符。
- 🔊 **低音增强（Bass Enhance）** —— 加入合成的低音层，丰富低频表现。
- 🎼 **SoundFont（`.sf2`）回放** —— 通过原生 Oboe 音频引擎渲染高质量乐器音色。
- 🖥️ **虚拟钢琴与 MIDI 监视器** —— 屏幕键盘及实时 MIDI 调试监视器。

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

> `.sf2` 文件已在 `.gitignore` 中列出，即使您在本地添加了该文件，Git 也不会对其进行跟踪。

## 关于 JJazzLab

本应用使用的 SoundFont 来自 **JJazzLab**。JJazzLab 是一个**开源应用**，其源代码仓库地址为：

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
2. 打开**设置**（`?` 按钮）。
3. 选择 **SoundFont** 与 **Instrument（乐器）**。
4. 选择连接到 Reface CP 的 **MIDI 端口**，然后点击**启动**。
5. 弹奏您的 Reface CP —— 和弦、调性、灯光面板和音频输出将实时响应。

## 技术栈

- **Kotlin** + AndroidX
- **原生 C++（JNI）** 音频引擎，使用 **Oboe** 实现低延迟 SoundFont 渲染
- **WebView** UI（HTML/CSS/JS），基于 [Tonal.js](https://github.com/tonaljs/tonal)、[Three.js](https://threejs.org/) 和 [iro.js](https://iro.js.org/)
- CMake 3.22.1，16 KB 页对齐（适配 Android 15+）

## 许可证

本项目许可证请参阅 [LICENSE](LICENSE) 文件。随附的 SoundFont 及第三方库保留其各自的许可证。
