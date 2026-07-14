/**
 * 自动踏板引擎 (Auto-Sustain Engine)
 */


window.isBreakingSustain = false;//自动踏板状态锁：标识当前是否处于“等待换气”状态
let pedalReleaseStartTime = 0; // 【新增】记录踏板松开的瞬间，用于计算止音时长
/**
 * 自动踏板引擎 (Auto-Sustain Engine - Adaptive Version)
 * @param {string} phase - 当前阶段："check_collision" 或 "try_repress"
 * @param {number} newNote - 当前新弹奏的音符 (仅在 check_collision 阶段有效)
 */
/**
 * 自动踏板引擎 (Auto-Sustain Engine)
 */
function sustainer(phase, newNote = null) {
    // 开启按钮时，立即激活自动踏板逻辑状态
    window.autoPedalDown = window.autoSustainEnabled;

    if (window.autoSustainEnabled) {
        // 只有在没被“换气锁”锁定时，才发送硬件 ON 信号
        if (!window.isBreakingSustain) {
            if (typeof sendSustainOn === 'function') sendSustainOn(window.currentDeviceID);
        }

        // --- 冲突检测逻辑 ---
        if (phase === "check_collision") {
            if (window.lowNotes.has(newNote)) return;
            let hasCollision = false;
            if (window.lowNotes.size > 0) {
                const minExistingNote = Math.min(...window.lowNotes);
                if (newNote < minExistingNote) hasCollision = true;
            }
            if (!hasCollision) {
                const notesArr = Array.from(window.lowNotes);
                notesArr.push(newNote);
                notesArr.sort((a, b) => a - b);
                for (let i = 0; i < notesArr.length - 1; i++) {
                    if (notesArr[i + 1] - notesArr[i] < 4) { hasCollision = true; break; }
                }
            }

            if (hasCollision) {
                break_sustain();
                window.lowNotes.forEach(note => {
                    if (!window.activeNotes.has(note)) window.lowNotes.delete(note);
                });
            }
        }
        // --- 重踩逻辑 ---
        else if (phase === "try_repress") {
            if (window.isBreakingSustain) {
                if (typeof sendSustainOn === 'function') sendSustainOn(window.currentDeviceID);
                window.isBreakingSustain = false;
                // 注意：这里不需要手动改 isPedalDown，因为 autoPedalDown 一直是 true
                window.activeNotes.forEach(note => window.pedalHeldNotes.add(note));
            }
        }
    } else {
        // --- 关键：关闭自动踏板 ---
        window.autoPedalDown = false;
        if (typeof sendSustainOff === 'function') sendSustainOff(window.currentDeviceID);
        window.lowNotes.clear();
        // 这里不需要手动 clear pedalHeldNotes，syncPedalState 会处理
    }

    // 每一阶段结束都同步逻辑状态
    if (typeof syncPedalState === 'function') syncPedalState();
    if (typeof updateCombinedNotes === 'function') updateCombinedNotes();
    if (typeof refreshKeyboardUI === 'function') refreshKeyboardUI();
}


/**
 * 【仅用作测试】瞬间切踏板 (Break & Re-sustain)
 */
function break_sustain() {
    if (!window.autoSustainEnabled || window.isBreakingSustain) return;

    window.isBreakingSustain = true;

    // 瞬间松开硬件
    if (typeof sendSustainOff === 'function') sendSustainOff(window.currentDeviceID);

    // 强制清理 UI 上的延音点，因为物理上确实断开了
    window.pedalHeldNotes.clear();
    updateCombinedNotes();
    refreshKeyboardUI();
}
