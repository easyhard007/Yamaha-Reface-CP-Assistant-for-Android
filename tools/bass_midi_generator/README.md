# MIDI-GPT 合并贝斯 MIDI 生成器

这个电脑端工具直接读取 `tools/drum_midi/` 中合并好的完整鼓 MIDI，加入持续的
C 大三和弦作为和声条件，再让 MIDI-GPT 生成一条完整的 Finger Bass MIDI。
模型生成的音高、时值和力度都会保留，不会统一改成 C4。模型只用于电脑端离线
制谱，不进入 Android App，也不会读写 App 的资源目录。

MIDI-GPT 的 `yellow_small` 检查点一次最多处理 8 小节。工具会在内部用带 2 小节
上下文的重叠窗口处理长文件，最后只输出一个连续 MIDI；不会按 variation 生成
多份候选。输入中的 tempo、拍号和 marker 会保留，方便后续自行切割。

安装依赖：

```powershell
python -m venv --system-site-packages .tmp/midigpt-env
.tmp/midigpt-env/Scripts/python -m pip install -r tools/bass_midi_generator/requirements.txt
```

生成完整贝斯 MIDI：

```powershell
.tmp/midigpt-env/Scripts/python tools/bass_midi_generator/generate.py `
  --input "tools/drum_midi/DIVA_BALLAD_57_BPM_4#4.mid" `
  --output "tools/generated_bass_midi/DIVA_BALLAD_57_BPM_4#4_bass_midigpt.mid" `
  --checkpoint <yellow_small-final.safetensors>
```

输出旁边会生成同名 `.json` 报告，记录小节数、窗口、音符数和实际生成音高范围。
