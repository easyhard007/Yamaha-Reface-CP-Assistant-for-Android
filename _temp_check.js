
        // ================= 全局状态与常量 =================
        window.midiAccess = null; 
        window.midiOutput = null; 
        window.isRunning = false;
        let autoConnectTimer = null;

        window.transposeValue = 0;
        window.split_point = 52; // 高低音区分割点
        window.autoSustainEnabled = false;
        window.currentDeviceID = 0; // 默认设备编号 0 (Reface CP)
        window.bassEnhanceEnabled = false;
        window.bassEnhanceRatio = 0.5; // 默认 50%

        const noSleep = new NoSleep();

        // ================= 全局颜色状态 (用于驱动背景与 UI) =================
        window.visualState = {
            h: 260, // 初始色相 (蓝紫)
            s: 100,
            l: 100,
            envelope: 0 // 能量包络 (0-1)
        };

        // 【新增】：幽灵音频上下文与保活振荡器
        let wakelockAudioCtx = null;
        let wakelockOscillator = null;


        window.physicalPedalDown = false; // 物理踏板状态
        window.autoPedalDown = false;     // 自动踏板状态
        window.isPedalDown = false;       // 逻辑合并状态 (两者之一为 true 则为 true)


        window.activeNotes = new Set();      
        window.pedalHeldNotes = new Set();   
        window.allActiveNotes = new Set();
        window.lowNotes = new Set();

        const noteStartTimes = new Map(); // debug：用于记录每个 MIDI note 的按下起始时间

        // MIDI 编号转音名工具 (用于 UI 显示)
        function midiToNoteName(midi) {
            const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
            const octave = Math.floor(midi / 12) - 1; // MIDI 60 是 C4
            const name = names[midi % 12];
            return name + octave;
        }

        // 踏板态同步函数
        function syncPedalState() {
            const newState = window.physicalPedalDown || window.autoPedalDown;

            // 如果踏板从“踩下”状态变为“完全释放”状态，清理延音缓存
            if (window.isPedalDown && !newState) {
                window.pedalHeldNotes.clear();
                updateCombinedNotes();
                refreshKeyboardUI();
            }

            window.isPedalDown = newState;
        }

        function changeSplitPoint(delta) {
            window.split_point += delta;
            // 限制在合理范围 (如 A0 到 C8)
            if (window.split_point < 21) window.split_point = 21;
            if (window.split_point > 108) window.split_point = 108;

            document.getElementById('split-display').innerText = midiToNoteName(window.split_point);

            // 【新增】：立即刷新 UI，让蓝色小点跟随移动
            if (typeof refreshKeyboardUI === 'function') {
                refreshKeyboardUI();
            }
        }
        // 修改增强比例并同步 UI
        function setEnhanceRatio(ratio) {
            window.bass_enhance_ratio = ratio;
            const btn = document.getElementById('enhance-btn');
            if (btn) {
                btn.innerText = `ENHANCE: ${Math.round(ratio * 100)}%`;
            }
            // 重新计算权重字典，否则高度和音量都不会变
            if (typeof updateBassWeightMap === 'function') {
                updateBassWeightMap();
            }
        }

        // 打开低音增强
        function toggleBassEnhance() {
            window.bassEnhanceEnabled = !window.bassEnhanceEnabled;
            const btn = document.getElementById('enhance-btn');
            if (window.bassEnhanceEnabled) {
                btn.classList.add('btn-active');
            } else {
                btn.classList.remove('btn-active');
            }
            // 【新增】：切换时立即刷新键盘
            if (typeof refreshKeyboardUI === 'function') refreshKeyboardUI();
            if (typeof updateBassWeightMap === 'function')  updateBassWeightMap();
        }

        // ==========================================
        // Style / Rhythm Test
        // ==========================================
        window.styleScenes = [];
        window.styleFileNames = [];
        window.styleSelectedScene = 0;
        window.styleTimer = null;

        function loadTestStyle() {
            if (typeof Android === 'undefined') {
                document.getElementById('style-info').innerText = '⚠ 仅限 Android 设备';
                return;
            }
            clearStyleTimer();
            try {
                var listJson = Android.getStyleFileList();
                window.styleFileNames = JSON.parse(listJson);
                if (window.styleFileNames.length === 0) {
                    document.getElementById('style-info').innerText = '❌ 未找到节奏文件';
                    return;
                }
                if (window.styleFileNames.length === 1) {
                    doLoadStyle(0);
                    return;
                }
                var sel = document.getElementById('style-file-select');
                sel.innerHTML = '';
                for (var i = 0; i < window.styleFileNames.length; i++) {
                    var opt = document.createElement('option');
                    opt.value = i; opt.textContent = window.styleFileNames[i];
                    sel.appendChild(opt);
                }
                sel.style.display = 'inline-block';
                document.getElementById('style-confirm-btn').style.display = 'inline-block';
                document.getElementById('style-info').innerText =
                    '请选择一个节奏文件 (' + window.styleFileNames.length + ' 个可用)';
            } catch(e) {
                document.getElementById('style-info').innerText = '❌ 获取文件列表失败: ' + e.message;
            }
        }

        function confirmLoadStyle() {
            var sel = document.getElementById('style-file-select');
            var idx = parseInt(sel.value);
            document.getElementById('style-file-select').style.display = 'none';
            document.getElementById('style-confirm-btn').style.display = 'none';
            doLoadStyle(idx);
        }

        function doLoadStyle(fileIdx) {
            document.getElementById('style-info').innerText = '正在加载 ' + window.styleFileNames[fileIdx] + ' ...';
            try {
                var json = Android.loadStyleFile(fileIdx);
                var scenes = JSON.parse(json);
                if (scenes.error) {
                    document.getElementById('style-info').innerText = '❌ ' + scenes.error;
                    return;
                }
                window.styleScenes = scenes;
                window.styleSelectedScene = 0; // 默认选中第一个场景
                Android.selectStyleScene(0);
                document.getElementById('style-info').innerText =
                    '✅ 已加载: ' + window.styleFileNames[fileIdx] +
                    ' (' + scenes.length + ' 个场景)';
                renderSceneButtons(scenes, 0);
                renderTrackRows();
                document.getElementById('scene-list').style.display = 'block';
                document.getElementById('style-play-btn').style.display = 'inline-block';
                document.getElementById('style-dump-btn').style.display = 'inline-block';
            } catch(e) {
                document.getElementById('style-info').innerText = '❌ 解析失败: ' + e.message;
            }
        }

        function renderSceneButtons(scenes, selectedIdx) {
            var html = '';
            for (var i = 0; i < scenes.length; i++) {
                var cls = 'glass-btn btn-rect';
                var extraStyle = 'font-size:11px; padding:6px 10px;';
                if (i === selectedIdx) {
                    // selected → 白光
                    extraStyle += 'border:2px solid #fff; box-shadow:0 0 8px rgba(255,255,255,0.5);';
                }
                html += '<div id="scene-btn-' + i + '" class="' + cls +
                    '" onclick="onSceneBtnClick(' + i + ')" style="' + extraStyle + '">' +
                    scenes[i].name + '</div>';
            }
            document.getElementById('scene-buttons').innerHTML = html;
        }

        function onSceneBtnClick(idx) {
            if (typeof Android === 'undefined') return;
            window.styleSelectedScene = idx;
            Android.selectStyleScene(idx);
            if (Android.isStylePlaying()) {
                // 正在播放中 — 只是请求切换, renderSceneButtons 由 timer 更新
            } else {
                renderSceneButtons(window.styleScenes, idx);
            }
        }

        function toggleTestStyle() {
            if (typeof Android === 'undefined') return;
            if (Android.isStylePlaying()) {
                Android.stopStyle();
                clearStyleTimer();
                document.getElementById('style-play-btn').style.background = '#2e7d32';
                document.getElementById('style-play-btn').innerText = '▶ Start';
                document.getElementById('style-info').innerText = '⏹ 已停止';
                renderSceneButtons(window.styleScenes, window.styleSelectedScene);
                renderTrackRows();
            } else {
                Android.selectStyleScene(window.styleSelectedScene);
                Android.startStyle();
                document.getElementById('style-play-btn').style.background = '#c62828';
                document.getElementById('style-play-btn').innerText = '⏹ Stop';
                document.getElementById('style-info').innerText = '▶ ' + window.styleScenes[window.styleSelectedScene].name + ' (C 和弦)';
                startStyleTimer();
            }
        }

        // ——— 定时更新场景按钮状态 ———
        function startStyleTimer() { clearStyleTimer(); window.styleTimer = setInterval(updateStylePoll, 150); }
        function clearStyleTimer() { if (window.styleTimer) { clearInterval(window.styleTimer); window.styleTimer = null; } }
        function updateStylePoll() {
            if (typeof Android === 'undefined') { clearStyleTimer(); return; }
            if (!Android.isStylePlaying()) {
                document.getElementById('style-play-btn').style.background = '#2e7d32';
                document.getElementById('style-play-btn').innerText = '▶ Start';
                clearStyleTimer();
                renderTrackRows();
                return;
            }
            updateStyleBtnStates();
            updateTrackLEDs();
        }


        function renderTrackRows() {
            var insts = window.instrumentNames || [];
            try {
                var json = Android.getStyleChannels();
                var channels = JSON.parse(json);
                if (!channels || !channels.length) { document.getElementById('track-rows').innerHTML = ''; return; }
                var html = '';
                for (var i = 0; i < channels.length; i++) {
                    var ch = channels[i];
                    var instName = (insts[ch.program] || ('Prog ' + ch.program));
                    html += '<div id="track-row-' + ch.channel + '" style="display:flex; align-items:center; gap:4px; padding:2px 0; border-bottom:1px solid #222;">' +
                        '<div class="glass-btn" onclick="toggleTrackMute(' + ch.channel + ')" id="mute-btn-' + ch.channel +
                        '" style="font-size:9px; padding:2px 5px; min-width:24px; text-align:center;">M</div>' +
                        '<span style="color:#ccc; flex:1;">Ch.' + ch.channel + ' ' + instName +
                        ' <span style="color:#666;">(b' + ch.bank + ' p' + ch.program + ')</span></span>' +
                        '<span id="led-' + ch.channel + '" style="display:inline-block; width:8px; height:8px; border-radius:50%; background:#333;"></span>' +
                        '</div>';
                }
                document.getElementById('track-rows').innerHTML = html;
                updateMuteButtons();
            } catch(e) {}
        }
        function toggleTrackMute(ch) { if (typeof Android !== 'undefined') { Android.toggleMute(ch); updateMuteButtons(); } }
        function updateMuteButtons() {
            if (typeof Android === 'undefined') return;
            try {
                var channels = JSON.parse(Android.getStyleChannels());
                for (var i = 0; i < channels.length; i++) {
                    var ch = channels[i].channel;
                    var btn = document.getElementById('mute-btn-' + ch);
                    if (!btn) continue;
                    var muted = Android.isChannelMuted(ch);
                    btn.style.background = muted ? '#c62828' : '';
                }
            } catch(e) {}
        }
        function updateTrackLEDs() {
            if (typeof Android === 'undefined') return;
            var active = Android.getActiveChannels();
            for (var ch = 0; ch < 16; ch++) {
                var led = document.getElementById('led-' + ch);
                if (!led) continue;
                led.style.background = (active & (1 << ch)) ? '#fff' : '#333';
                led.style.boxShadow = (active & (1 << ch)) ? '0 0 4px #fff' : '';
            }
        }

function dumpStyleDebug() {
            if (typeof Android === 'undefined') return;
            var path = Android.dumpStyleDebug();
            document.getElementById('style-info').innerText = '📄 Dump → ' + path;
        }

        function updateStyleBtnStates() {
            var cur = Android.getCurrentStyleScene(), pend = Android.getPendingStyleScene();
            for (var i = 0; i < window.styleScenes.length; i++) {
                var btn = document.getElementById('scene-btn-' + i);
                if (!btn) continue;
                btn.style.boxShadow = ''; btn.style.border = ''; btn.style.animation = '';
                if (i === cur) { btn.style.border = '2px solid #fff'; btn.style.boxShadow = '0 0 12px rgba(255,255,255,0.7)'; }
                else if (i === pend && pend >= 0) { btn.style.border = '2px solid #fff'; btn.style.animation = 'style-blink 0.5s ease-in-out infinite alternate'; }
            }
        }

        
    function onNativeDeviceState(connected, name, deviceIndex) {
        window.isRunning = connected;
        window.midiOutput = connected ? { send: function(bytes) {} } : null;
        updateFabState();
        const label = document.getElementById('midi-label');
        const sel = document.getElementById('midiPorts');
        if (connected) {
            label.innerHTML = '✅ MIDI 端口: ' + name;
            label.style.color = '#00E676';
            if (deviceIndex >= 0 && deviceIndex < _deviceList.length) sel.value = deviceIndex;
        } else {
            label.innerHTML = '请选择MIDI 端口';
            label.style.color = '#FFFFFF';
        }
        const startBtn = document.getElementById('start-btn');
        if (connected) { startBtn.classList.add('success'); }
        else { startBtn.classList.remove('success'); }
    }
    function onNativeSf2List(names) {
        const sel = document.getElementById('sf2Select');
        sel.innerHTML = '';
        try {
            JSON.parse(names).forEach((n, i) => {
                const opt = document.createElement('option'); opt.value = i; opt.text = n;
                sel.appendChild(opt);
            });
        } catch(e) {}
    }
    function onNativeInstList(names) {
        const sel = document.getElementById('instSelect');
        sel.innerHTML = '';
        try {
            var arr = JSON.parse(names);
            window.instrumentNames = arr;
            arr.forEach((n, i) => {
                const opt = document.createElement('option'); opt.value = i; opt.text = n;
                sel.appendChild(opt);
            });
        } catch(e) {}
    }
    function onSf2Change() {
        const idx = document.getElementById('sf2Select').value;
        if (typeof Android !== 'undefined') Android.selectSf2(parseInt(idx));
    }
    function onInstChange() {
        const idx = document.getElementById('instSelect').value;
        if (typeof Android !== 'undefined') Android.selectInstrument(parseInt(idx));
    }
    function onNativeMidi(type, note, velocity, channel) {
        if (!window.isRunning) { window.isRunning = true; updateFabState(); }
        // Feed virtual piano engine (drives background animation brightness)
        if (type === 'noteon' && typeof triggerVirtualNoteOn === 'function') {
            triggerVirtualNoteOn(note, velocity);
        }
        // Update virtual piano energy every call
        if (typeof updateVirtualPianoEngine === 'function') {
            updateVirtualPianoEngine(performance.now());
        }
    }
    function onNativeChordInfo(chord, roman, key, tsd, secondary) {
        _lastChordInfo = { chord, roman, key, tsd };
        // Chord display (primary + secondary like keyboard.js)
        const pressedDiv = document.getElementById('pressed-notes');
        if (pressedDiv && chord !== '--') {
            var secHtml = '';
            if (secondary && secondary !== '' && secondary !== chord) {
                // Check if secondary has a different root
                var secRoot = (secondary.match(/^[A-G][#b]?/) || [''])[0];
                var priRoot = (chord.match(/^[A-G][#b]?/) || [''])[0];
                if (secRoot !== priRoot) {
                    secHtml = '<span style="font-size:0.6em;color:rgba(255,255,255,0.3);margin-right:8px;font-weight:normal;">OR</span>' +
                              '<span class="chord-secondary-text">' + secondary + '</span>';
                }
            }
            pressedDiv.innerHTML = '<div style="height:60%;display:flex;align-items:flex-end;justify-content:center;padding-bottom:2px;">' +
                '<span class="chord-primary-text">' + chord + '</span></div>' +
                '<div style="height:40%;display:flex;align-items:baseline;justify-content:center;padding-top:2px;">' + secHtml + '</div>';
        } else if (pressedDiv && window.allActiveNotes && window.allActiveNotes.size === 0) {
            pressedDiv.innerHTML = '<div style="height:60%;display:flex;align-items:flex-end;justify-content:center;padding-bottom:2px;"><span style="color:rgba(255,255,255,0.4);font-size:clamp(14px,3vh,18px);">等待和弦...</span></div><div style="height:40%;"></div>';
        }
        // Roman numeral
        var rd = document.getElementById('light-roman-display');
        if (rd) rd.innerText = (roman !== '--') ? roman : '-';
        // Key
        var kd = document.getElementById('light-key-display');
        if (kd) kd.innerText = (key !== '--') ? key : '正在识别调性';
        // TSD color (skip if tsd is "--" — scale not yet detected)
        if (typeof applyChordColorByNumeral === 'function' && tsd !== '--' && tsd !== '-') {
            var fg = applyChordColorByNumeral(tsd);
            var fd = document.getElementById('light-function-display');
            if (fd) fd.innerText = fg;
        }
    }
    function onNativeNoteState(json) {
        try {
            const state = JSON.parse(json);
            window.activeNotes = new Set(state.active || []);
            window.pedalHeldNotes = new Set(state.pedal || []);
            window.lowNotes = new Set(state.low || []);
            window.allActiveNotes = new Set(state.all || []);
            window.isPedalDown = state.pedalDown || false;
            if (state.split != null) window.split_point = state.split;
            // Bass enhance sync
            if (state.bassEnhance != null) window.bassEnhanceEnabled = (state.bassEnhance === true || state.bassEnhance === 'true');
            if (state.bassRatio != null) window.bass_enhance_ratio = parseFloat(state.bassRatio) || 0;
            if (state.bassCenter != null) window.bass_enhance_center = parseInt(state.bassCenter) || 43;
            if (state.bassSpread != null) window.bass_enhance_spread = parseInt(state.bassSpread) || 12;
            // Sync bass weight map from C++ (replaces JS updateBassWeightMap)
            if (state.bassWeights) {
                window.BASS_WEIGHT_MAP = {};
                for (var wi = 0; wi < state.bassWeights.length; wi++) {
                    var w = state.bassWeights[wi];
                    if (w > 0) window.BASS_WEIGHT_MAP[wi] = w;
                }
            }
            if (typeof refreshKeyboardUI === 'function') refreshKeyboardUI();
            // Restore key/roman after refreshKeyboardUI overwrites (JS scale detection is disabled)
            if (_lastChordInfo && _lastChordInfo.key !== '--') {
                var rk = document.getElementById('light-key-display');
                var rr = document.getElementById('light-roman-display');
                var rf = document.getElementById('light-function-display');
                if (rk) rk.innerText = _lastChordInfo.key;
                if (rr) rr.innerText = _lastChordInfo.roman;
                if (rf && _lastChordInfo.tsd) {
                    rf.innerText = _lastChordInfo.tsd;
                    // Re-apply TSD colors
                    if (typeof applyChordColorByNumeral === 'function') {
                        applyChordColorByNumeral(_lastChordInfo.tsd);
                    }
                }
            }
            // Update virtual piano engine (drives background brightness)
            if (typeof updateVirtualPianoEngine === 'function') {
                updateVirtualPianoEngine(performance.now());
            }
            // Update ENHANCE button text
            var eb = document.getElementById('enhance-btn');
            if (eb) eb.innerText = 'ENHANCE: ' + Math.round(window.bass_enhance_ratio * 100) + '%';
            const fmt = (s) => s.size ? [...s].sort((a,b)=>a-b).join(',') : '-';
            const da = document.getElementById('dbg-active');
            const dp = document.getElementById('dbg-pedal');
            const dl = document.getElementById('dbg-low');
            if (da) da.textContent = fmt(window.activeNotes);
            if (dp) dp.textContent = fmt(window.pedalHeldNotes);
            if (dl) dl.textContent = fmt(window.lowNotes);
            const pd = document.getElementById('dbg-pedaldown');
            if (pd) { pd.textContent = window.isPedalDown ? 'TRUE' : 'false'; pd.style.color = window.isPedalDown ? '#00E676' : '#ff5252'; }
            const br = document.getElementById('dbg-breaking');
            if (br) { br.textContent = (state.isBreaking) ? 'TRUE' : 'false'; br.style.color = (state.isBreaking) ? '#FF9800' : '#ff5252'; }
            const db = document.getElementById('dbg-bass');
            if (db) { db.textContent = 'ratio=' + (state.bassRatio != null ? state.bassRatio : '?') + ' center=' + (state.bassCenter != null ? state.bassCenter : '?') + ' spread=' + (state.bassSpread != null ? state.bassSpread : '?') + ' enabled=' + (state.bassEnhance || false); }
        } catch(e) {}
    }
    // Disable JS MIDI handling — C++ handles everything
    handleMIDIInput = function() {};
    sustainer = function() {};
    // Prevent keyboard.js from flickering the function display's CSS class
    // Disable JS scale detection — C++ ScaleDetector handles this
    getScaleDebugData = function() { return { bestText: "-", weights: [], scales: [] }; };
    registerNoteForScale = function() {};
    // Patch updateBassWeightMap: remove auto-scroll (red dots drive scrolling)
    if (typeof updateBassWeightMap === 'function') {
        var _origUpdateBassWeightMap = updateBassWeightMap;
        updateBassWeightMap = function() { _origUpdateBassWeightMap(); };
    }
    // Override toggleAutoSustain → use C++ backend
    toggleAutoSustain = function() {
        window.autoSustainEnabled = !window.autoSustainEnabled;
        const btn = document.getElementById('sustain-btn');
        if (window.autoSustainEnabled) {
            btn.classList.add('btn-active');
            if (typeof Android !== 'undefined') Android.toggleAutoSustain(true);
        } else {
            btn.classList.remove('btn-active');
            if (typeof Android !== 'undefined') Android.toggleAutoSustain(false);
        }
    };
    // Override toggleBassEnhance → C++ backend
    toggleBassEnhance = function() {
        window.bassEnhanceEnabled = !window.bassEnhanceEnabled;
        var btn = document.getElementById('enhance-btn');
        if (window.bassEnhanceEnabled) btn.classList.add('btn-active');
        else btn.classList.remove('btn-active');
        if (typeof Android !== 'undefined') Android.toggleBassEnhance(window.bassEnhanceEnabled);
    };
    // Override changeTranspose → use C++ backend + SysEx
    changeTranspose = function(delta) {
        if (typeof Android !== 'undefined') {
            var newVal = Android.changeTranspose(delta);
            document.getElementById('transpose-display').innerText = (newVal > 0 ? '+' : '') + newVal;
        }
    };
    // Override changeSplitPoint → use C++ backend
    changeSplitPoint = function(delta) {
        if (typeof Android !== 'undefined') {
            const newVal = Android.changeSplitPoint(delta);
            window.split_point = newVal;
            document.getElementById('split-display').innerText = midiToNoteName(newVal);
            if (typeof refreshKeyboardUI === 'function') refreshKeyboardUI();
        }
    };
    function onNativeMidiLogEntry(text, isRx) {
        const div = document.getElementById('midi-log');
        if (!div) return;
        const span = document.createElement('span');
        span.style.color = isRx ? '#00E676' : '#FF9800';
        span.textContent = text;
        div.appendChild(span); div.appendChild(document.createElement('br'));
        while (div.children.length > 400) { div.removeChild(div.firstChild); div.removeChild(div.firstChild); }
        const card = document.getElementById('midi-log-card');
        if (card) card.scrollTop = card.scrollHeight;
    }
    // Override: use native device list
    const _origSelectPort = selectPort;
    selectPort = function() {};
    const _origStartControl = startControl;
    startControl = function() {
        const idx = parseInt(document.getElementById('midiPorts').value);
        if (isNaN(idx) || idx < 0 || idx >= _deviceList.length) {
            document.getElementById('status').innerText = '❌ 请先选择设备'; return;
        }
        if (typeof Android !== 'undefined') Android.startDevice(idx);
        document.getElementById('status').innerText = '正在连接...';
    };
    const _origStopControl = stopControl;
    stopControl = function() {
        if (typeof Android !== 'undefined') Android.stopDevice();
    };
    // Override onload: skip Web MIDI, init DOM via bridge
    window.onload = () => {
        if (typeof initKeyboardDOM === 'function') initKeyboardDOM();
        if (typeof initColorPicker === 'function') initColorPicker();
        if (typeof initBackground === 'function') initBackground();
        window.isRunning = true;
    };
    