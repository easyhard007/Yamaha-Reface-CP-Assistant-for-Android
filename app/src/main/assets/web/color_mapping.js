// ==========================================
// 和弦颜色映射引擎 (Color Mapping Engine v6.0)
// 七大顺阶色彩体系 (Diatonic Seven-Color System)
// ==========================================

// 7 大级数色彩库
let diatonicColors = {
    I:   { h: 260, s: 100 }, // 蓝紫
    IV:  { h: 20,  s: 100 }, // 橙红
    V:   { h: 170, s: 100 }, // 青绿
    ii:  { h: 43,  s: 40 }, // 金黄
    iii: { h: 228, s: 70 }, // 深海蓝
    vi:  { h: 290, s: 60 }, // 粉紫
    vii: { h: 130, s: 30 }  // 绿色 (减和弦)
};

let svgOverlay = null;
let currentChordDot = null; 

function initTSDOverlay() {
    const pickerContainer = document.querySelector('.IroWheel');
    if (!pickerContainer) return;

    const width = pickerContainer.clientWidth;
    const height = pickerContainer.clientHeight;

    svgOverlay = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svgOverlay.setAttribute("width", width);
    svgOverlay.setAttribute("height", height);
    svgOverlay.style.position = "absolute";
    svgOverlay.style.top = "0";
    svgOverlay.style.left = "0";
    svgOverlay.style.pointerEvents = "none"; 
    svgOverlay.style.zIndex = "10";
    svgOverlay.style.overflow = "visible";
    
    pickerContainer.appendChild(svgOverlay);
    drawTSDTriangle(width);
    updateChordDotPosition(width);

    window.addEventListener('resize', () => {
        if(pickerContainer.clientWidth > 0) {
            svgOverlay.setAttribute("width", pickerContainer.clientWidth);
            svgOverlay.setAttribute("height", pickerContainer.clientHeight);
            drawTSDTriangle(pickerContainer.clientWidth);
            updateChordDotPosition(pickerContainer.clientWidth);
        }
    });
}

function getPosFromHue(hue, size, saturation = 100) {
    const radius = size / 2;
    const visualAngle = (90 - hue + 360) % 360; 
    const angleRad = (visualAngle - 90) * (Math.PI / 180);
    const maxR = radius - 15; 
    const r = maxR * (saturation / 100);
    return { x: radius + r * Math.cos(angleRad), y: radius + r * Math.sin(angleRad) };
}

function drawTSDTriangle(size) {
    if (!svgOverlay) return;
    svgOverlay.innerHTML = ''; 

    const pT = getPosFromHue(diatonicColors.I.h, size, diatonicColors.I.s);
    const pS = getPosFromHue(diatonicColors.IV.h, size, diatonicColors.IV.s);
    const pD = getPosFromHue(diatonicColors.V.h, size, diatonicColors.V.s);

    const polygon = document.createElementNS("http://www.w3.org/2000/svg", "polygon");
    polygon.setAttribute("points", `${pT.x},${pT.y} ${pS.x},${pS.y} ${pD.x},${pD.y}`);
    polygon.setAttribute("fill", "none");
    polygon.setAttribute("stroke", "#ffffff");
    polygon.setAttribute("stroke-width", "1.5");
    svgOverlay.appendChild(polygon);

    const pointsData = [
        { id: "T", hue: diatonicColors.I.h, s: diatonicColors.I.s, p: pT },
        { id: "S", hue: diatonicColors.IV.h, s: diatonicColors.IV.s, p: pS },
        { id: "D", hue: diatonicColors.V.h, s: diatonicColors.V.s, p: pD }
    ];

    pointsData.forEach(pt => {
        const circle = document.createElementNS("http://www.w3.org/2000/svg", "circle");
        circle.setAttribute("cx", pt.p.x);
        circle.setAttribute("cy", pt.p.y);
        circle.setAttribute("r", "5");
        circle.setAttribute("fill", "#ffffff");
        svgOverlay.appendChild(circle);

        const textPos = getPosFromHue(pt.hue, size, 145); 
        const text = document.createElementNS("http://www.w3.org/2000/svg", "text");
        text.setAttribute("x", textPos.x);
        text.setAttribute("y", textPos.y);
        text.setAttribute("fill", "#ffffff");
        
        // text.setAttribute("font-size", "20px");
        // 【新增】：根据宽高比动态决定 SVG 字体大小
        const w = window.innerWidth;
        const h = window.innerHeight;
        // 如果宽高比小于 3:4，字号调小 2 号 (比如从 20px 降到 16px)
        const fontSize = (w / h < 0.75) ? "16px" : "20px";

        text.setAttribute("font-size", fontSize);
        text.setAttribute("font-weight", "900");
        text.setAttribute("font-family", "sans-serif");
        text.setAttribute("text-anchor", "middle");
        text.setAttribute("dominant-baseline", "central");
        text.textContent = pt.id;
        svgOverlay.appendChild(text);
    });

    currentChordDot = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    currentChordDot.setAttribute("r", "9");
    currentChordDot.setAttribute("fill", "none");
    currentChordDot.setAttribute("stroke", "#ffffff");
    currentChordDot.setAttribute("stroke-width", "3");
    currentChordDot.style.transition = "cx 0.35s cubic-bezier(0.1, 0.9, 0.2, 1), cy 0.35s cubic-bezier(0.1, 0.9, 0.2, 1)";
    currentChordDot.style.filter = "drop-shadow(0px 3px 6px rgba(0, 0, 0, 0.8))";
    svgOverlay.appendChild(currentChordDot);
}

let currentTargetHSL = { h: 0, s: 0, l: 100 }; // 默认白色白光占位

function getTrueRGB(h, s, l) {
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
    return `rgb(${Math.round(r * 255)}, ${Math.round(g * 255)}, ${Math.round(b * 255)})`;
}

//使用级数和弦判断功能级数并赋予颜色。
function applyChordColorByNumeral(romanNumeral) {

    if (!romanNumeral || romanNumeral === "-") return "-"; // 返回未知状态给 keyboard.js
    
    let targetH = 0; 
    let targetS = 100; 
    let targetL = 100; 
    let functionGroup = "-"; // 用于返回给 UI 第二行大字显示

    let targetRomanForColor = "";

    // 【核心重构】：优先使用斜杠后的低音（Bass）来决定灯光色彩！
    if (romanNumeral.includes('/')) {
        // 如果存在转位，比如 "Imaj7/iii"
        // 提取斜杠后面的部分："iii"
        const bassRoman = romanNumeral.split('/')[1];
        // 清洗掉所有无用的符号，只留下纯罗马数字用于 switch 匹配
        const cleanBass = bassRoman.replace(/b|#|m|maj|sus|dim|aug|[0-9]/g, "").toUpperCase();
        const match = cleanBass.match(/^[b#]*([IVXivx]+)/i);
        if (match) {
            targetRomanForColor = match[1].toUpperCase();
        }
    } else {
        // 如果没有转位，使用主和弦
        const baseRoman = romanNumeral.split('/')[0];
        const cleanNumeral = baseRoman.replace(/b|#|m|maj|sus|dim|aug|[0-9]/g, "").toUpperCase();
        const match = cleanNumeral.match(/^[b#]*([IVXivx]+)/i);
        if (match) {
            targetRomanForColor = match[1].toUpperCase();
        }
    }

    if (targetRomanForColor!== "") {
        switch (targetRomanForColor) {
            case "I": 
                targetH = diatonicColors.I.h; targetS = diatonicColors.I.s; 
                functionGroup = "I";
                break;
            case "II": 
                targetH = diatonicColors.ii.h; targetS = diatonicColors.ii.s; 
                functionGroup = "ii"; // 小写
                break;
            case "III": 
                targetH = diatonicColors.iii.h; targetS = diatonicColors.iii.s; 
                functionGroup = "iii"; // 小写
                break;
            case "IV": 
                targetH = diatonicColors.IV.h; targetS = diatonicColors.IV.s; 
                functionGroup = "IV";
                break;
            case "V": 
                targetH = diatonicColors.V.h; targetS = diatonicColors.V.s; 
                functionGroup = "V";
                break;
            case "VI": 
                targetH = diatonicColors.vi.h; targetS = diatonicColors.vi.s; 
                functionGroup = "vi"; // 小写
                break;
            case "VII": 
                targetH = diatonicColors.vii.h; targetS = diatonicColors.vii.s; 
                functionGroup = "vii°"; // 附加减度符号
                break;
            default: 
                targetH = 0; targetS = 0; 
                functionGroup = "-";
                break; 
        }
    } else {
        // 兜底：如果正则连罗马数字都没找到
        targetH = 0; targetS = 0; 
        functionGroup = "-";
    }

    currentTargetHSL = { h: targetH, s: targetS, l: targetL };

    // 1. 注入光能引擎

    window.visualState.h = targetH;
    window.visualState.s = targetS;
    window.visualState.l = targetL;

    // 2. 移动色环准星
    updateChordDotPosition();

    // 3. 返回处理好的功能级数名（如 "IV" 或 "vii°"），交给 keyboard.js 渲染到屏幕！
    return functionGroup;
}

function updateChordDotPosition(forceSize) {
    if (!currentChordDot || !svgOverlay) return;
    const size = forceSize || svgOverlay.clientWidth;
    if (size <= 0) return;
    const pos = getPosFromHue(currentTargetHSL.h, size, currentTargetHSL.s);
    currentChordDot.setAttribute("cx", pos.x);
    currentChordDot.setAttribute("cy", pos.y);
    currentChordDot.style.display = "block";
}