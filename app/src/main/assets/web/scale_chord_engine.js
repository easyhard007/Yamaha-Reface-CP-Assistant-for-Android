

// ==========================================
// 调性与灯光和弦识别引擎 (Scale & Chord Engine v4.0)
// 引入: 物理低音音程分析 (Figured Bass Anchor)
// ==========================================

const PITCH_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const MAJOR_SCALE_NAMES = ['C', 'Db', 'D', 'Eb', 'E', 'F', 'Gb', 'G', 'Ab', 'A', 'Bb', 'B'];
const MINOR_SCALE_NAMES = ['A', 'Bb', 'B', 'C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'G#'];

function getSingleNoteName(note) {
    return `${PITCH_NAMES[note % 12]}${Math.floor(note / 12) - 2}`;
}

// --------------------------------------------------
// 【新增核心功能】：利用当前全局调性，裁决 7(omit3) 是大是小
// --------------------------------------------------
function resolveOmit3SeventhChord(bassPc) {
    // 假设还未识别出调性，默认按最常见的属七返回
    if (typeof globalScaleRoot === 'undefined' || globalScaleRoot === "-") {
        return "7(omit3)";
    }

    // 获取当前调性根音的 Pitch Class (0-11)
    const scaleRootPc = Tonal.Note.chroma(globalScaleRoot);
    
    // 计算当前和弦根音与调性根音的半音差
    const rootDiff = (bassPc - scaleRootPc + 12) % 12;

    // 根据大调顺阶和弦法则：
    // I(0), IV(5), V(7) 级的基础三和弦是大三和弦 -> 应该补充为 7(omit3)
    // ii(2), iii(4), vi(9) 级的基础三和弦是小三和弦 -> 应该补充为 m7(omit3)
    if ([0, 5, 7].includes(rootDiff)) {
        return "7(omit3)"; // 虽然大调 I 和 IV 是 maj7，但在 "0,7,10" 的骨架下，10是小七度，说明这就是一个属七和弦或蓝调离调
    } else if ([2, 4, 9].includes(rootDiff)) {
        return "m7(omit3)";
    } else {
        // 对于其他的离调根音（如 bIII, bVI 等），保守返回 7(omit3)
        return "7(omit3)";
    }
}


// --------------------------------------------------
// 【新增核心功能】：调性双音/单音推断引擎 (Diatonic Dyad Implication)
// 专门对付只有 1~2 个 Pitch Class 的残缺和弦
// --------------------------------------------------
function getDiatonicDyadChord(midiNotes) {
    if (typeof globalScaleRoot === 'undefined' || globalScaleRoot === "-") return null;

    // 提取去重后的 Pitch Class (0-11)
    const pcs = Array.from(new Set(midiNotes.map(n => n % 12))).sort((a, b) => a - b);
    
    const scaleRootPc = Tonal.Note.chroma(globalScaleRoot);
    
    // 计算相对于当前调性根音的半音程差 (0-11)
    const diffs = pcs.map(pc => (pc - scaleRootPc + 12) % 12).sort((a, b) => a - b);

    // 辅助函数：根据音级差找回真实的音名
    const getNoteNameByDiff = (diff) => {
        const targetPc = (scaleRootPc + diff) % 12;
        // 这里可以直接用 PITCH_NAMES，因为后面会被统一清洗
        return PITCH_NAMES[targetPc]; 
    };

    // 规则 1 & 2：只有 1 个有效的音名 (如单音，或 C4+C5 八度)
    if (diffs.length === 1) {
        const d = diffs[0];
        const rootName = getNoteNameByDiff(d);
        // 大调顺阶三和弦映射表：I(M), ii(m), iii(m), IV(M), V(M), vi(m), vii(dim)
        switch(d) {
            case 0: return rootName;       // I
            case 2: return rootName + "m"; // ii
            case 4: return rootName + "m"; // iii
            case 5: return rootName;       // IV
            case 7: return rootName;       // V (大三)
            case 9: return rootName + "m"; // vi
            case 11: return rootName + "dim";// vii
            default: return null;          // 离调单音，不作推断
        }
    }

    // 规则 3：刚好 2 个不同的音名 (Dyads)
    if (diffs.length === 2) {
        // 生成逗号分隔的特征字符串进行精准匹配
        const sig = diffs.join(','); 
        
        switch(sig) {
            case "0,4":  return getNoteNameByDiff(0);           // [0]M (C,E -> C)
            case "0,9":  return getNoteNameByDiff(9) + "m";     // [+9]m (C,A -> Am)
            case "0,11": return getNoteNameByDiff(0) + "maj7";  // [0]maj7 (C,B -> Cmaj7)
            case "2,5":  return getNoteNameByDiff(2) + "m";     // [+2]m (D,F -> Dm)
            case "2,11": return getNoteNameByDiff(7);           // [+7]M (D,B -> G)
            case "4,7":  return getNoteNameByDiff(4) + "m";     // [+4]m (E,G -> Em)
            case "5,9":  return getNoteNameByDiff(5);           // [+5]M (F,A -> F)
            case "7,11": return getNoteNameByDiff(7);           // [+7]M (G,B -> G)
            default:     return null;                           // 未匹配模板
        }
    }

    return null;
}
// --------------------------------------------------

// ================= 1. 新增：低音锚点备选引擎 =================

function getBassAnchorChord(midiNotes) {
    if (!midiNotes || midiNotes.length === 0) return null;
    
    // 获取最低音
    let bassNote = midiNotes[0];
    
    // 判定范围 C0 - E3
    // 假设中央 C(C3) 是 60，那么 C0 是 24，E3 是 64。
    // 如果按之前权重计算的 C2=36，那么 C0=12，E3=52。这里我们采用宽泛限制 64 (E4以内都算作有效左手区)
    // 你可以根据实际弹奏习惯调整这个阈值
    if (bassNote > 64) return null; 
    
    let bassPc = bassNote % 12;
    
    // 找次低音（必须是音名不同的音）
    let secondNote = midiNotes.find(n => n % 12 !== bassPc);
    
    // 规则 (1)：只弹了1个音，或者弹了同名八度，直接返回 5和弦
    if (!secondNote) {
        return { rootPc: bassPc, name: MAJOR_SCALE_NAMES[bassPc] + "5" };
    }
    
    let secondPc = secondNote % 12;
    let interval = (secondPc - bassPc + 12) % 12;
    
    let rootPc = bassPc;
    let suffix = "";

    // 规则 (2)：根据音程推导和弦
    switch (interval) {
        case 1:
        case 2:
            return null; // 弹错了，小二/大二度不构成基础和弦底座
        case 3:
            suffix = "m"; // 小三度 -> 小三和弦
            break;
        case 4:
            suffix = "";  // 大三度 -> 大三和弦
            break;
        case 5:
            rootPc = secondPc; 
            suffix = "5"; // 纯四度 -> 五和弦转位 (如 C+F -> F5)
            break;
        case 6:
            rootPc = (bassPc - 4 + 12) % 12; 
            suffix = "7"; // 增四度 -> 属七和弦 (如 C+F# -> Ab7)
            break;
        case 7:
            suffix = "5"; // 纯五度 -> 五和弦
            break;
        case 8:
            rootPc = (bassPc - 4 + 12) % 12; 
            suffix = "";  // 小六度 -> 大三第一转位 (如 C+Ab -> Ab)
            break;
        case 9:
            rootPc = (bassPc - 3 + 12) % 12; 
            suffix = "m"; // 大六度 -> 小三第一转位 (如 C+A -> Am)
            break;
        case 10:
            // 小七度：属七或小七？检查高音区有没有大三或小三度
            let higherNotes = midiNotes.filter(n => n > 64);
            let hasM3 = higherNotes.some(n => n % 12 === (bassPc + 4) % 12);
            let hasm3 = higherNotes.some(n => n % 12 === (bassPc + 3) % 12);
            if (hasm3 && !hasM3) suffix = "m7";
            else suffix = "7"; // 默认或有大三度，都视为属七
            break;
        case 11:
            suffix = "maj7"; // 大七度 -> 大七和弦
            break;
    }

    return { 
        rootPc: rootPc, 
        name: MAJOR_SCALE_NAMES[rootPc] + suffix 
    };
}

// ================= 2. 和弦置信度与过滤 =================

function evaluateConfidence(chordName, bassMidi) {
    let conf = 1.0; 
    const bassName = Tonal.Midi.midiToNoteName(bassMidi, { pitchClass: true }); 
    
    if (chordName.includes('/')) {
        conf -= 0.1; 
        if (!chordName.endsWith('/' + bassName)) conf -= 0.3;
    } else {
        if (!chordName.startsWith(bassName)) conf -= 0.2; 
    }
    
    if (chordName.match(/[A-Z]m?[0-9]*A/)) conf -= 0.4; 
    
    return Math.max(0.1, conf); 
}

// 2. 核心功能：和弦降维归类 (Classification v3.0 - 绞杀大写M幽灵版)
function classifyChord(chordName) {
    let base = chordName.split('/')[0];
    
    // 【核心清洗】：在清洗变化音的同时，彻底干掉孤立的大写 M，以及 add 附缀
    // 注意：不要误杀 maj 里面的 m 或 M7 里的 M，所以我们要精确匹配！
    base = base
        .replace(/[b#][59]/g, '')     // 杀变化音 b5, #5, b9, #9
        .replace(/b13/g, '')          // 杀 b13
        .replace(/#11/g, '')          // 杀 #11
        .replace(/add[0-9]+/g, '');   // 杀 add9, add11 等

    const rootMatch = base.match(/^[A-G][#b]?/);
    if (!rootMatch) return chordName; 
    
    const root = rootMatch[0];
    let suffix = base.substring(root.length); 

    // 【关键】：如果经过上面的清洗，后缀就剩下一个孤零零的大写 "M" 
    // (这是 Tonal.js 对大三和弦的倔强)，我们强行把它清空！
    if (suffix === 'M') {
        suffix = '';
    }

    // d) 根据后缀进行严格的 5 大类强制归类
    // 1. 大七和弦 (Major 7th)：包含 maj, M7, M9, maj9 等
    if (suffix.includes('maj') || suffix.includes('M7') || suffix.includes('M9')) {
        return root + 'maj7';
    } 
    // 2. 小七和弦 (Minor 7th)：以 m/min 开头，且带有 7, 9, 11
    else if ((suffix.includes('m') || suffix.includes('min')) && suffix.match(/[79]|11|13/)) {
        return root + 'm7';
    } 
    // 3. 属七和弦 (Dominant 7th)：没有任何 m/maj，直接带 7, 9, 11, 13
    else if (suffix.match(/[79]|11|13/)) {
        return root + '7';
    } 
    // 4. 小三和弦 (Minor Triad)：只有 m, min，没有任何数字
    else if (suffix === 'm' || suffix === 'min' || suffix === 'm(maj7)') {
        return root + 'm';
    } 
    // 5. 大三和弦 (Major Triad)：完全没有后缀
    else {
        return root; 
    }
}

// === 3. 处理琴键输入，生成排名列表 ===
function processChordsForLight(midiNotes) {
    if (midiNotes.length < 2) return [];

    let rawResults = new Map();
    let currentNotes = [...midiNotes]; 
    const originalBassMidi = midiNotes[0]; 

    // ----------------------------------------------------
    // 阵营 A：Tonal.js 及其高音修剪 (Top-Note Pruning)
    // ----------------------------------------------------
    while (currentNotes.length >= 2) {
        let pitchClasses = Array.from(new Set(currentNotes.map(n => Tonal.Midi.midiToNoteName(n, {pitchClass:true}))));
        let bassClass = Tonal.Midi.midiToNoteName(originalBassMidi, {pitchClass:true});
        pitchClasses = pitchClasses.filter(p => p !== bassClass);
        pitchClasses.unshift(bassClass); 

        let tonalResults = Tonal.Chord.detect(pitchClasses);
        tonalResults.forEach(chord => {
            if (!rawResults.has(chord)) {
                let conf = evaluateConfidence(chord, originalBassMidi);
                let pruningPenalty = (midiNotes.length - currentNotes.length) * 0.5;
                conf = Math.max(0.1, conf - pruningPenalty);
                rawResults.set(chord, conf);
            }
        });
        currentNotes.pop();
    }

    let processedList = [];
    
    // 计算 Tonal 阵营的得分并加入列表
    rawResults.forEach((conf, chord) => {
		
		chord = chord.replace(/^([A-G][#b]?)M/, '$1');
		
        let len = chord.length;
        let penalty = 0;

        if (chord.match(/[#b][59]|#11|b13|6/)) penalty += 0.8;
        if (chord.includes('bb') || chord.includes('##')) penalty += 0.2;

        let score = (conf * 5.0) - penalty; 
		
		// 遇到括号直接一刀切！比如 Cmaj7(omit3) 直接变成 Cmaj7
        let cleanedChord = chord.replace(/\(.*?\)/g, '');
		
		// 我们不再需要 classified 这个额外的字段了，
		// 甚至连 processedList 的结构也可以简化，不过为了兼容前端 UI，
        // 我们暂时把 cleanedChord 塞进 classified 字段传出去。
        processedList.push({ 
            original: chord, 
            confidence: conf.toFixed(2), 
            length: chord.length, 
            score: score, 
            classified: cleanedChord  // <--- 让清洗后的结果直接充当之前的“归类”
        });
    });

    // ----------------------------------------------------
    // 阵营 B：黑字典特征向量匹配 (Shell Voicings)
    // ----------------------------------------------------
    const bassName = Tonal.Midi.midiToNoteName(originalBassMidi, { pitchClass: true }); 
    const bassPc = originalBassMidi % 12; // 获取根音音级 (0-11)
    const intervals = Array.from(new Set(midiNotes.map(n => (n - originalBassMidi) % 12))).sort((a,b)=>a-b);
    const signature = intervals.join(','); 

    const CUSTOM_CHORDS = {
        "0,7,11": "maj7(omit3)", 
        "0,2,7,11": "maj9(omit3)", 
        "0,4,11": "maj7(omit5)", 
        "0,3,10": "m7(omit5)", 
    //    "0,8": "aug(omit3)",
    //    "0,6": "dim(omit3)", 
        "0,7": "5"
        // 删掉了写死的 "0,7,10": "7(omit3)"，改为动态解析
    };

    if (CUSTOM_CHORDS[signature]) {
        let dictSuffix = CUSTOM_CHORDS[signature];
        if (signature === "0,7,10") {
            dictSuffix = resolveOmit3SeventhChord(bassPc);
        }

        if (dictSuffix) {
            let dictName = bassName + dictSuffix;
            let existing = processedList.find(r => r.original === dictName);
            
            let dictConf = 1.0;
            let dictScore = (dictConf * 5.0) - 0; 
            
            if (existing) {
                existing.source = 'Tonal + Dict';
                if (existing.score < dictScore) {
                    existing.score = dictScore;
                    existing.confidence = "1.00 [Dict Boost]";
                }
            } else {
                processedList.push({
                    original: dictName,
                    confidence: "1.00", 
                    length: dictName.length,     
                    score: dictScore,     
                    // 【关键替换】
                    classified: dictName.replace(/\(.*?\)/g, ''),
                    source: '[Dict]' 
                });
            }
        }
    }
	
	
	// ----------------------------------------------------
    // 阵营 C：调性双音推断 (Diatonic Implication)
    // 专门拯救被 Tonal 放弃、且没命中黑字典的 1~2 音组合
    // ----------------------------------------------------
    const diatonicImpliedName = getDiatonicDyadChord(midiNotes);
    if (diatonicImpliedName) {
        let existing = processedList.find(r => r.original === diatonicImpliedName);
        
        // 赋予极高置信度。因为它基于全局调性上下文，比 Tonal 的瞎猜准确得多
        let diatonicScore = (1.0 * 5.0) - 0; // 满分 5.0
        
        if (existing) {
            // 如果 Tonal 也恰好猜中了，加上联合印记
            existing.source += ' + Diatonic';
            if (existing.score < diatonicScore) {
                existing.score = diatonicScore;
                existing.confidence = "1.00 [Diatonic Boost]";
            }
        } else {
            // 如果没人猜中，它作为新王登基！
			processedList.push({
                original: diatonicImpliedName,
                confidence: "1.00", 
                length: diatonicImpliedName.length,     
                score: diatonicScore,     
                // 【关键替换】
                classified: diatonicImpliedName.replace(/\(.*?\)/g, ''),
                source: '[Diatonic]' 
            });
        }
    }
	
	

    // 按照最终得分排序 (高分在上)
    processedList.sort((a, b) => b.score - a.score);
    
    return processedList;
}


// ================= 2. 动态调性识别 (基于 KS 算法) =================

// 二进制大调+小调掩码模板（KS算法）
const SCALE_TEMPLATE = [112.68, 4.91, 107, 7.71, 106.98, 107.62, 5.06, 109.94, 6.37, 106.35, 5.63, 106.05];
const SCALE_VECTORS = [];

for (let i = 0; i < 12; i++) {
    let vec = new Array(12).fill(0);
    for (let j = 0; j < 12; j++) {
        vec[(i + j) % 12] = SCALE_TEMPLATE[j];
    }
    SCALE_VECTORS.push({
        // 绑定正确的等音名（消除 D# 大调等）
        rootName: MAJOR_SCALE_NAMES[i], 
        majorName: MAJOR_SCALE_NAMES[i],
        minorName: MINOR_SCALE_NAMES[i],
        vector: vec
    });
}

// 内存中维护的 12 音名动态权重池
let currentPitchWeights = new Array(12).fill(0.0);
let globalScaleRoot = "-"; 

// --- 【新增：时间维度衰减机制】 ---
let lastNoteTime = performance.now();

// 每 500ms 检查一次
setInterval(() => {
    let now = performance.now();
    // 如果超过 5 秒没有任何琴键按下
    if (now - lastNoteTime > 5000) {
        let totalWeight = currentPitchWeights.reduce((a,b) => a + b, 0);
        // 如果权重池里还有数据，就开始持续的挂机衰减
        if (totalWeight > 0.01) {
            for (let i = 0; i < 12; i++) {
                // 每 0.5 秒衰减 10% (乘以 0.9)
                // 这样既能慢慢遗忘之前的调性，又不会导致UI瞬间断崖式清空
                currentPitchWeights[i] *= 0.90; 
            }
            // 触发 UI 刷新，让 Debug 面板里的数字平滑下降
            if (typeof refreshKeyboardUI === 'function') refreshKeyboardUI();
        }
    }
}, 500);


function registerNoteForScale(note) {
    // 每次弹琴，重置最后弹琴时间
    lastNoteTime = performance.now();

    for (let i = 0; i < 12; i++) currentPitchWeights[i] *= 0.98; //衰减系数

    let weight = 2.0 - ((note - 12) / 96.0) * 1.9;
    weight = Math.max(0.1, Math.min(2.0, weight));
    
    currentPitchWeights[note % 12] += weight;
}

function getScaleDebugData() {
    let totalWeight = currentPitchWeights.reduce((a,b) => a + b, 0);
    
    // 降低清空阈值，因为 KS 算法的基数放大了
    if (totalWeight < 0.1) {
        globalScaleRoot = "-";
        return { weights: currentPitchWeights, scales: [], bestText: "-" };
    }

    let scaleScores = [];
    for (let i = 0; i < 12; i++) {
        let score = 0;
        for (let j = 0; j < 12; j++) {
            score += currentPitchWeights[j] * SCALE_VECTORS[i].vector[j];
        }
        scaleScores.push({
            majorName: SCALE_VECTORS[i].majorName,
            minorName: SCALE_VECTORS[i].minorName,
            rootName: SCALE_VECTORS[i].rootName,
            score: score
        });
    }

    scaleScores.sort((a, b) => b.score - a.score);
    globalScaleRoot = scaleScores[0].rootName; 

    return {
        weights: currentPitchWeights,
        scales: scaleScores,
        bestText: `${scaleScores[0].majorName}大调 / ${scaleScores[0].minorName}小调`
    };
}


// ================= 3. 级数转换与等音纠错 =================
// 辅助函数：将和弦的根音翻转为等音名 (如 Gb 变成 F#)

function getEnharmonicChord(chordName) {
    const rootMatch = chordName.match(/^[A-G][#b]?/);
    if (!rootMatch) return chordName;

    const root = rootMatch[0];
    const suffix = chordName.substring(root.length);

    const enharmonicMap = {
        'C#': 'Db', 'Db': 'C#',
        'D#': 'Eb', 'Eb': 'D#',
        'F#': 'Gb', 'Gb': 'F#',
        'G#': 'Ab', 'Ab': 'G#',
        'A#': 'Bb', 'Bb': 'A#',
        'E#': 'F',  'F': 'E#', 
        'B#': 'C',  'C': 'B#'
    };

    if (enharmonicMap[root]) {
        return enharmonicMap[root] + suffix;
    }
    return chordName;
}

// 核心级数翻译函数 (支持转位斜杠低音级数)
function getRomanNumeral(chordName) {
    if (globalScaleRoot === "-") return chordName; 

    // 【新增逻辑 1】：提取斜杠后缀的低音 (Bass Note)
    let baseChord = chordName;
    let bassNote = "";
    
    if (chordName.includes('/')) {
        const parts = chordName.split('/');
        baseChord = parts[0];
        bassNote = parts[1]; // 如 "C", "Bb" 等
    }

    try {
        // --- 处理主干和弦 (baseChord) ---
        const romanArr = Tonal.Progression.toRomanNumerals(globalScaleRoot, [baseChord]);
        let mainRoman = (romanArr && romanArr.length > 0) ? romanArr[0] : "";

        // 处理重降/重升的等音翻转
        if (mainRoman.includes('bb') || mainRoman.includes('##')) {
            const flippedChord = getEnharmonicChord(baseChord);
            const romanArr2 = Tonal.Progression.toRomanNumerals(globalScaleRoot, [flippedChord]);
            let roman2 = (romanArr2 && romanArr2.length > 0) ? romanArr2[0] : "";
            
            if (roman2 && !roman2.includes('bb') && !roman2.includes('##')) {
                mainRoman = roman2;
            }
        }

        // 如果主干翻译失败，直接返回原名
        if (mainRoman === "") return chordName;

        // --- 【新增逻辑 2】：如果存在转位低音，单独翻译它 ---
        if (bassNote !== "") {
            // Tonal 没有直接把单音翻译为级数的方法，但我们可以利用一个取巧的办法：
            // 把这个单音当做一个大三和弦去翻译，比如 "C" 翻译出 "I"，然后我们只要前面的罗马数字
            const bassRomanArr = Tonal.Progression.toRomanNumerals(globalScaleRoot, [bassNote]);
            let bassRoman = (bassRomanArr && bassRomanArr.length > 0) ? bassRomanArr[0] : "";

            if (bassRoman.includes('bb') || bassRoman.includes('##')) {
                const flippedBass = getEnharmonicChord(bassNote);
                const bassRomanArr2 = Tonal.Progression.toRomanNumerals(globalScaleRoot, [flippedBass]);
                let bassRoman2 = (bassRomanArr2 && bassRomanArr2.length > 0) ? bassRomanArr2[0] : "";
                if (bassRoman2 && !bassRoman2.includes('bb') && !bassRoman2.includes('##')) {
                    bassRoman = bassRoman2;
                }
            }

            // 【新增逻辑 3】：拼装！将翻译出来的低音级数接到主级数后面
            if (bassRoman !== "") {
                // 如果是单音翻译的，Tonal 会给它一个大三的壳子，比如翻译出来是 "I"
                // 这样拼起来就完美了：iii/I
                return `${mainRoman}/${bassRoman}`;
            } else {
                // 如果单音翻译失败，保留原始单音字母：iii/C
                return `${mainRoman}/${bassNote}`;
            }
        }

        // 如果没有斜杠，直接返回主干级数
        return mainRoman;

    } catch(e) {
        console.error("[Roman Engine] 报错: ", e);
    }
    
    return chordName; 
}