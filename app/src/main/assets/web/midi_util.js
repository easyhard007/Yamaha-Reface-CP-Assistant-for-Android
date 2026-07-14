/**
 * MIDI Utility for Yamaha Reface CP Assistant (Android Native Bridge)
 * 通过 Android.sendMidiNote/CC 向外部 MIDI 设备发送信号
 */

function sendTransposeSysEx(deviceId, transposeValue) {
    // SysEx 暂时跳过（Android 端不支持原生 SysEx）
    console.log(`[MIDI] Transpose SysEx skipped (native bridge): ${transposeValue}`);
}

function sendSustainOn(deviceId) {
    if (typeof Android !== 'undefined') Android.sendMidiCC(64, 127);
    console.log("sustain on");
}

function sendSustainOff(deviceId) {
    if (typeof Android !== 'undefined') Android.sendMidiCC(64, 0);
    console.log("sustain OFF");
}

function sendMidiNote(note, velocity, isOn) {
    if (note < 0 || note > 127) return;
    if (typeof Android !== 'undefined') Android.sendMidiNote(note, velocity, isOn);
}
