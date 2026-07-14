// ==========================================
// 虚拟钢琴物理音量引擎 (Virtual Piano Engine v2.0)
// 新增: 低频长衰减、低频大能量权重
// ==========================================

const KEY_MIN = 24; // C0
const KEY_MAX = 108; // C7

// 1. 当前实时音量数组 (0.0 ~ 无上限，因低频加权可能突破 1.0)
let keyVolumes = new Array(128).fill(0.0);

// 2. 最大基础音量数组 (包含低频权重)
// C0 权重 3.0，C7 权重 1.0。用线性插值计算。
let keyMaxVolumes = new Array(128).fill(1.0);
for (let i = 0; i < 128; i++) {
    if (i <= KEY_MIN) {
        keyMaxVolumes[i] = 3.0;
    } else if (i >= KEY_MAX) {
        keyMaxVolumes[i] = 1.0;
    } else {
        // 从 3.0 线性降到 1.0
        let ratio = (i - KEY_MIN) / (KEY_MAX - KEY_MIN);
        keyMaxVolumes[i] = 3.0 - (2.0 * ratio);
    }
}

// 3. 延音情况下的衰减系数数组
// 代表每 1 毫秒保留的能量百分比 (指数衰减)。
// 假设高音 C7 延音需要 5000ms 衰减到 1% -> 系数约 0.999079
// 假设低音 C0 延音需要 10000ms (高音的 2 倍长) 衰减到 1% -> 系数约 0.999539
let keyDecayRates = new Array(128).fill(0.999079);
const rateC7 = Math.pow(0.01, 1/5000); 
const rateC0 = Math.pow(0.01, 1/10000); 

for (let i = 0; i < 128; i++) {
    if (i <= KEY_MIN) {
        keyDecayRates[i] = rateC0;
    } else if (i >= KEY_MAX) {
        keyDecayRates[i] = rateC7;
    } else {
        let ratio = (i - KEY_MIN) / (KEY_MAX - KEY_MIN);
        keyDecayRates[i] = rateC0 + (rateC7 - rateC0) * ratio;
    }
}

// 无踏板时的阻尼(止音呢)系数倍率
const DAMPING_MULTIPLIER = 25; 

// 暴露给灯光引擎的全局虚拟音量
window.virtualAudioVolume = 0.0;

function triggerVirtualNoteOn(note, velocity) {
    if (note < 0 || note > 127) return;
    
    // 力度归一化并平方 (模拟真实钢琴手感曲线)
    let force = velocity / 127.0;
    force = force * force; 
    
    // 应用低频加权音量
    let targetVolume = keyMaxVolumes[note] * force;
    
    // 余音不突降逻辑
    if (targetVolume > keyVolumes[note]) {
        keyVolumes[note] = targetVolume;
    }
}

let lastVirtualTime = performance.now();

function updateVirtualPianoEngine(currentTime) {
    const deltaTime = currentTime - lastVirtualTime;
    lastVirtualTime = currentTime;
    
    let sumSquared = 0;
    
    for (let i = KEY_MIN; i <= KEY_MAX; i++) {
        if (keyVolumes[i] > 0) {
            
            let isSustained = window.activeNotes.has(i) || window.pedalHeldNotes.has(i);
            
            // 每个琴键拥有自己独立的衰减速度
            let currentRate = Math.pow(keyDecayRates[i], deltaTime);
            
            if (!isSustained) {
                currentRate = Math.pow(currentRate, DAMPING_MULTIPLIER);
            }
            
            keyVolumes[i] *= currentRate;
            
            if (keyVolumes[i] < 0.001) keyVolumes[i] = 0;
            
            sumSquared += keyVolumes[i] * keyVolumes[i];
        }
    }
    
    // RMS 能量聚合 (因为低音加权了最高可能到3.0，这里的除数也稍微调大一点防爆音)
    let rms = Math.sqrt(sumSquared) / 2.5; 
    
    window.virtualAudioVolume = rms; // 注意：这里不再强制截断到 1.0，允许轻微溢出交给后处理压缩
}