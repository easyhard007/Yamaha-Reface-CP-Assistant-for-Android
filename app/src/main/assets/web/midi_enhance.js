/**
 * 低音增强助手 (Bass Enhancer v4.0)
 */

window.bass_enhance_ratio = 0.5;
window.bass_enhance_center = 43;
window.bass_enhance_spread = 12;

window.BASS_WEIGHT_MAP = {}; // 提升为全局以便 UI 读取

function updateBassWeightMap() {
    const b = window.bass_enhance_center;
    const d = window.bass_enhance_spread;
    const r = window.bass_enhance_ratio;

    // 计算高斯方差
    const variance2 = (d * d) / Math.log(10);

    const newMap = {};
    let firstActiveNote = -1;

    for (let i = 0; i <= 127; i++) {
        const x = i - b;
        const weight = Math.exp(-(x * x) / variance2) * r; // 直接乘以增益比

        // 阈值过滤：低于 0.1 * ratio 的项设为 0
        if (weight >= 0.1 * r) {
            newMap[i] = weight;
            if (firstActiveNote === -1) firstActiveNote = i;
        } else {
            newMap[i] = 0;
        }
    }

    window.BASS_WEIGHT_MAP = newMap;

    // 触发 UI 刷新（滚动由红点驱动，不受蓝条影响）
    if (typeof refreshKeyboardUI === 'function') refreshKeyboardUI();
}

function bass_enhance(note, velocity, isOn) {
    if (!window.bassEnhanceEnabled) return;

    const weight = window.BASS_WEIGHT_MAP[note] || 0;
    if (weight === 0) return;

    const lowerNote = note - 12;
    // 现在直接乘以权重即可，权重中已包含 ratio
    let targetVelocity = Math.round(velocity * weight);

    if (isOn && targetVelocity === 0) targetVelocity = 1;
    if (!isOn) targetVelocity = 0;

    if (typeof sendMidiNote === 'function') {
        sendMidiNote(lowerNote, targetVelocity, isOn);
    }
}