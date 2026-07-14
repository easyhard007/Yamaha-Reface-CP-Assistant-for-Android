// ==========================================
// 光学与物理灯光引擎 (Light & Color Engine) - DEBUG 探针版
// ==========================================


const FADE_DURATION = 500;       
const ANIMATION_TICK = 16;       
const MIDI_SEND_TICK = 33;       
const IDLE_LIGHTNESS = 0; 


window.padLightSources = Array.from({length: 16}, () => ({
    userHSL: { h: 250, s: 100, l: 100 }, 
    envelope: 0 
}));

let colorPicker = null;

let smoothedVolume = 0.0;       
const ALPHA_DECAY = 0.9;        
const BETA_ATTACK = 0.7;        

// === 探针计数器 (防止日志刷屏刷死浏览器) ===
let debugLogCounter = 0;

function hslToRgb(h, s, l) {
    h /= 360; s /= 100; l /= 100;
    let r, g, b;
    if (s === 0) { r = g = b = l; } 
    else {
        const hue2rgb = (p, q, t) => {
            if (t < 0) t += 1; if (t > 1) t -= 1;
            if (t < 1 / 6) return p + (q - p) * 6 * t;
            if (t < 1 / 2) return q;
            if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
            return p;
        };
        const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        const p = 2 * l - q;
        r = hue2rgb(p, q, h + 1 / 3); g = hue2rgb(p, q, h); b = hue2rgb(p, q, h - 1 / 3);
    }
    return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
}


function interpolateHSL(source, target, progress) {
    progress = Math.max(0, Math.min(1, progress));
    let h1 = source.h, s1 = source.s;
    let h2 = target.h, s2 = target.s;
    
    if (s1 < 1) h1 = h2;
    if (s2 < 1) h2 = h1;

    const h1Rad = h1 * (Math.PI / 180);
    const h2Rad = h2 * (Math.PI / 180);
    
    const x1 = s1 * Math.cos(h1Rad);
    const y1 = s1 * Math.sin(h1Rad);
    const x2 = s2 * Math.cos(h2Rad);
    const y2 = s2 * Math.sin(h2Rad);
    
    const currentX = x1 + (x2 - x1) * progress;
    const currentY = y1 + (y2 - y1) * progress;
    
    let currentS = Math.sqrt(currentX * currentX + currentY * currentY);
    let currentH = Math.atan2(currentY, currentX) * (180 / Math.PI);
    if (currentH < 0) currentH += 360;
    
    let currentL = source.l + (target.l - source.l) * progress;
    
    return { h: currentH, s: currentS, l: currentL };
}


// === 核心修复 4：调色盘自适应大小 ===
function initColorPicker() {
    const container = document.getElementById('color-picker-section');
    // 根据父级容器当前实际可用宽度，动态算出最合理的尺寸 (留点内边距)
    const initialWidth = Math.min(container.clientWidth * 0.8, container.clientHeight * 0.8, 180);

    colorPicker = new iro.ColorPicker("#color-picker-container", {
        width: initialWidth, 
        color: "#aa00ff", 
        layoutDirection: "horizontal", 
        layout: [ { component: iro.ui.Wheel } ]
    });

    // 监听窗口尺寸变化，动态修正 iro 色环的大小！
    window.addEventListener('resize', () => {
        if (!colorPicker || !container) return;
        // 算出新的可用尺寸
        const newWidth = Math.min(container.clientWidth * 0.8, container.clientHeight * 0.8, 180);
        colorPicker.resize(newWidth);
    });

    setTimeout(() => {
        if (typeof initTSDOverlay === 'function') initTSDOverlay();
    }, 100);
    
    // 初始化颜色数组 (代码保留不变)
    const hsl = colorPicker.color.hsl;
    window.padLightSources[0].userHSL = { h: hsl.h, s: hsl.s, l: 100 };
    
    requestAnimationFrame(engineLoop);
}

function triggerPadLights() {
    window.padLightSources[0].envelope = 1.0; 
}

function forceSendCurrentColorToMidi() {
    triggerPadLights();
}

let lastFrameTime = performance.now();
let lastEngineTick = 0; 
let lastMidiSendTick = 0; 

function engineLoop(currentTime) {
    const deltaTime = currentTime - lastFrameTime;
    lastFrameTime = currentTime;
    debugLogCounter++;

    // ================== 探针 1：钢琴引擎更新 ==================
    if (typeof updateVirtualPianoEngine === 'function') {
        updateVirtualPianoEngine(currentTime);
    } else if (debugLogCounter === 1) {
        console.error("[Error] 找不到 updateVirtualPianoEngine 函数！");
    }

    // ================== 探针 2：音量读取 ==================
    let rawVolume = window.virtualAudioVolume || 0;

    if (rawVolume > smoothedVolume) {
        smoothedVolume = smoothedVolume * BETA_ATTACK + rawVolume * (1 - BETA_ATTACK);
    } else {
        smoothedVolume = smoothedVolume * ALPHA_DECAY + rawVolume * (1 - ALPHA_DECAY);
    }
    if (smoothedVolume < 0.005) smoothedVolume = 0.0;

    // 2. 更新全局视觉状态的能量包络
    window.visualState.envelope = Math.max(0, Math.min(1.0, Math.pow(smoothedVolume, 0.4)));

    // 3. 修改调用背景引擎的部分
    if (typeof updateBackgroundState === 'function') {
        updateBackgroundState(
            smoothedVolume,
            window.visualState.h,
            window.visualState.s,
            window.visualState.l
        );
    }

    // 4. (可选) 同步 UI 罗马大字发光效果也改为读取 window.visualState
    const functionDisplay = document.getElementById('light-function-display');
    if (functionDisplay) {
        let alpha = window.visualState.envelope;
        let glowColor = `hsla(${window.visualState.h}, ${window.visualState.s}%, 50%, ${alpha})`;
        functionDisplay.style.textShadow = alpha > 0.05 ? `0 0 15px ${glowColor}, 0 0 30px ${glowColor}` : "none";
    }

    requestAnimationFrame(engineLoop);
}

