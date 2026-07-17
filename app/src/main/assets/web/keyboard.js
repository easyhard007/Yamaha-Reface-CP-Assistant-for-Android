// ==========================================
// 虚拟钢琴键盘与 UI 渲染引擎 (Keyboard UI Engine v2.0)
// 新增：自动侦测屏幕宽高比，低于 3:4 切换为 2 八度显示
// ==========================================

const NOTE_START = 24; 
const NOTE_END = 96;   
// 全局变量，现在它是动态的！
let VISIBLE_WHITE_KEYS = 21; 

let lastRenderedScale = "-"; 

function getFunctionGroupText(romanNumeral) {
    if (!romanNumeral || romanNumeral === "-") return "-";
    const cleanNumeral = romanNumeral.replace(/b|#|m|maj|sus|dim|aug|[0-9]/g, "").toUpperCase();
    switch (cleanNumeral) {
        case "I": return "I";
        case "II": return "ii";
        case "III": return "iii";
        case "IV": return "IV";
        case "V": return "V";
        case "VI": return "vi";
        case "VII": return "vii°";
        default: return "-"; 
    }
}


// === 核心逻辑：智能裁切 2 八度 ===
function calculateAndInjectDimensions() {
    const wrapper = document.getElementById('keyboard-wrapper');
    if(!wrapper) return;

    // 获取当前窗口宽高比
    const w = window.innerWidth;
    const h = window.innerHeight;
    const aspectRatio = w / h;

    // 始终显示 3 个八度 (21 白键), 极窄屏才缩到 2 个
    if (aspectRatio < 0.4) {
        VISIBLE_WHITE_KEYS = 14;
        document.documentElement.style.setProperty('--card-radius', '6px');
    } else {
        VISIBLE_WHITE_KEYS = 21;
        document.documentElement.style.setProperty('--card-radius', '10px');
    }

    const wkWidth = wrapper.clientWidth / VISIBLE_WHITE_KEYS;
    document.documentElement.style.setProperty('--wk-width', `${wkWidth}px`);
    document.documentElement.style.setProperty('--bk-width', `${wkWidth * 0.65}px`);
    document.documentElement.style.setProperty('--bk-margin', `-${wkWidth * 0.325}px`);
}

function getWhiteKeyIndex(note) {
    const offsets = [0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6]; 
    return Math.floor((note - 24) / 12) * 7 + offsets[(note - 24) % 12];
}

function initKeyboardDOM() {
    const keyboardDiv = document.getElementById('keyboard');
    if (!keyboardDiv) return;
    keyboardDiv.innerHTML = '';
    
    calculateAndInjectDimensions();

    for (let i = NOTE_START; i <= NOTE_END; i++) { 
        const keyDiv = document.createElement('div');
        keyDiv.id = `key-${i}`;
        keyDiv.className = [1, 3, 6, 8, 10].includes(i % 12) ? 'key black-key' : 'key white-key';
        
        // 【核心修复】：一个琴键只需要一个点！不要画蛇添足加 red/blue
        // 它的颜色会由外部父级 div 是否拥有 .active-red 或 .active-pink 类名，通过 CSS 自动接管！
        const dot = document.createElement('div'); 
        dot.className = 'dot';
        
        keyDiv.appendChild(dot);

        if (![1, 3, 6, 8, 10].includes(i % 12)) {
            const bar = document.createElement('div');
            bar.className = 'enhance-bar';
            bar.id = `enhance-bar-${i}`;
            keyDiv.appendChild(bar);
        }

        keyboardDiv.appendChild(keyDiv);
    }
    const wrapper = document.getElementById('keyboard-wrapper');
    const wkWidth = parseFloat(document.documentElement.style.getPropertyValue('--wk-width'));
    setTimeout(() => wrapper.scrollLeft = getWhiteKeyIndex(48) * wkWidth, 50);
}

// Resize 时重新运算所有比例和八度数量
window.addEventListener('resize', () => {
    calculateAndInjectDimensions();
    // 由于屏幕翻转可能导致键盘变窄，我们需要强制重新拉回当前和弦
    const notesArr = Array.from(window.allActiveNotes).sort((a,b) => a - b);
    if (notesArr.length > 0) handleAutoScroll(notesArr);
});

function handleAutoScroll(notesArray) {
    if (notesArray.length === 0) return;
    const wrapper = document.getElementById('keyboard-wrapper');
    const wkWidth = parseFloat(document.documentElement.style.getPropertyValue('--wk-width'));
    if (!wkWidth) return;
    
    const minVisibleWk = wrapper.scrollLeft / wkWidth;
    const maxVisibleWk = minVisibleWk + VISIBLE_WHITE_KEYS - 1; 
    const lowestWk = getWhiteKeyIndex(notesArray[0]);
    const highestWk = getWhiteKeyIndex(notesArray[notesArray.length - 1]);

    if (lowestWk < minVisibleWk) wrapper.scrollTo({ left: lowestWk * wkWidth, behavior: 'smooth' });
    else if (highestWk > maxVisibleWk) {
        let targetScroll = (highestWk - VISIBLE_WHITE_KEYS + 1) * wkWidth;
        if ((targetScroll / wkWidth) > lowestWk) targetScroll = lowestWk * wkWidth; 
        wrapper.scrollTo({ left: targetScroll, behavior: 'smooth' });
    }
}

// 供外部文件引用的定时器
let chordColorDebounceTimer = null;
let lastColoredRoman = "";

function refreshKeyboardUI() {

    //增加红点用于表示当前按下的音符和延音音符
    for (let i = NOTE_START; i <= NOTE_END; i++) {
        const keyDiv = document.getElementById(`key-${i}`);
        if (!keyDiv) continue;

        if (i === window.split_point) {
                keyDiv.classList.add('is-split');
        } else {
            keyDiv.classList.remove('is-split');
        }

        if (window.activeNotes.has(i)) {
            keyDiv.classList.add('active-red'); keyDiv.classList.remove('active-pink');
        } else if (window.pedalHeldNotes.has(i)) {
            keyDiv.classList.remove('active-red'); keyDiv.classList.add('active-pink');
        } else {
            keyDiv.classList.remove('active-red'); keyDiv.classList.remove('active-pink');
        }
    }

    //用于显示具有enhance的琴键
    for (let i = NOTE_START; i <= NOTE_END; i++) {
        const bar = document.getElementById(`enhance-bar-${i}`);
        if (!bar) continue;

        if (window.bassEnhanceEnabled) {
            const weight = window.BASS_WEIGHT_MAP[i] || 0;
            // 权重为 1 时高度为 33.3% (1/3)
            const heightPercent = (weight * 33.3).toFixed(1);
            bar.style.height = `${heightPercent}%`;
        } else {
            bar.style.height = '0%';
        }
    }

    const notesArr = Array.from(window.allActiveNotes).sort((a,b) => a - b);
    if (notesArr.length > 0) handleAutoScroll(notesArr);

    const textDiv = document.getElementById('pressed-notes');
    const romanDisplay = document.getElementById('light-roman-display');
    const functionDisplay = document.getElementById('light-function-display');
    const keyDisplay = document.getElementById('light-key-display');

    let scaleData = { bestText: "-", weights: new Array(12).fill(0), scales: [] };
    if (typeof getScaleDebugData === 'function') scaleData = getScaleDebugData();

    if (scaleData.bestText !== lastRenderedScale && scaleData.bestText !== "-") {
        lastRenderedScale = scaleData.bestText;
        if(keyDisplay) {
            keyDisplay.innerText = scaleData.bestText;
            keyDisplay.classList.remove('flash-highlight-text');
            void keyDisplay.offsetWidth; 
            keyDisplay.classList.add('flash-highlight-text');
        }
        if (functionDisplay) {
            functionDisplay.classList.remove('flash-highlight-text');
            void functionDisplay.offsetWidth; 
            functionDisplay.classList.add('flash-highlight-text');
        }
    } else {
        if(keyDisplay) keyDisplay.innerText = scaleData.bestText;
    }

    // === UI 更新 ===
    if (notesArr.length === 0) {
        textDiv.innerHTML = `
            <div style="height: 60%; display: flex; align-items: flex-end; justify-content: center; padding-bottom: 2px;">
                <span style="color:rgba(255,255,255,0.4); font-size:clamp(14px, 3vh, 18px);">等待和弦...</span>
            </div>
            <div style="height: 40%;"></div>
        `;
        if (romanDisplay) romanDisplay.innerText = "-";
        
        if (typeof checkAndApplyModulation === 'function') checkAndApplyModulation("");
        return;
    }

    if (typeof processChordsForLight === 'function') {
        const chordList = processChordsForLight(notesArr);
        
        if (chordList.length > 0) {
            const primary = chordList[0].original; 
            const primaryRootMatch = primary.match(/^[A-G][#b]?/);
            const primaryRoot = primaryRootMatch ? primaryRootMatch[0] : "";
            const baseChordForRoman = chordList[0].classified;
            
            let secondaryHtml = "";
            if (chordList.length > 1) {
                const secondary = chordList[1].original;
                const secondaryRootMatch = secondary.match(/^[A-G][#b]?/);
                const secondaryRoot = secondaryRootMatch ? secondaryRootMatch[0] : "";
                
                if (secondaryRoot !== primaryRoot) {
                    // 应用你在 style.css 中定义的新字体控制类
                    secondaryHtml = `
                        <span style="font-size: 0.6em; color: rgba(255,255,255,0.3); margin-right: 8px; font-weight: normal;">OR</span>
                        <span class="chord-secondary-text">${secondary}</span>
                    `;
                }
            }
            
            textDiv.innerHTML = `
                <div style="height: 60%; display: flex; align-items: flex-end; justify-content: center; padding-bottom: 2px;">
                    <span class="chord-primary-text">${primary}</span>
                </div>
                <div style="height: 40%; display: flex; align-items: baseline; justify-content: center; padding-top: 2px;">
                    ${secondaryHtml}
                </div>
            `;
            
            if (typeof checkAndApplyModulation === 'function') checkAndApplyModulation(baseChordForRoman);
            const roman = getRomanNumeral(baseChordForRoman);
            if (romanDisplay) romanDisplay.innerText = roman;

            // TSD display handled by C++ backend via onNativeChordInfo
            // (original setTimeout overwrote #light-function-display.innerText)

        } else {
            textDiv.innerHTML = `
                <div style="height: 60%; display: flex; align-items: flex-end; justify-content: center; padding-bottom: 2px;">
                    <span style="color:rgba(255,255,255,0.4); font-size:clamp(14px, 3vh, 18px);">等待和弦...</span>
                </div>
                <div style="height: 40%;"></div>
            `;
            if (romanDisplay) romanDisplay.innerText = "-";
            if (typeof checkAndApplyModulation === 'function') checkAndApplyModulation("");
        }
    }
}