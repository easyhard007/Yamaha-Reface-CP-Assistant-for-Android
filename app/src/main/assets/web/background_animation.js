// ==========================================
// 动态背景渲染引擎 (Color Bends Engine v1.0)
// 原生复刻 React Bits - ColorBends
// ==========================================

let cbScene, cbCamera, cbRenderer, cbMaterial;
let cbUniforms = {};
let cbClock;
let cbRafId = null;

// 动态追踪的物理参数
let cbTargetEnergy = 0.0;
let cbCurrentEnergy = 0.0;

// 用于颜色平滑过渡的向量
let cbTargetColor = new THREE.Vector3(168/255, 85/255, 247/255); // 初始紫色 #A855F7
let cbCurrentColor = new THREE.Vector3(168/255, 85/255, 247/255);
const tempThreeColor = new THREE.Color();

// 根据当前窗口的长宽比动态计算最佳的 uScale
function calculateOptimalScale() {
    const w = window.innerWidth;
    const h = window.innerHeight;
    const ratio = w / h;

    // 设定锚点：
    // 横屏锚点: 16/9 (约 1.777) -> scale = 3.1
    // 竖屏锚点: 1/2  (0.5) -> scale = 1.8
    const maxRatio = 16 / 9;
    const minRatio = 1 / 2;
    const maxScale = 2.5;
    const minScale = 0.8;

    // 夹紧当前的 ratio 在锚点范围内
    const clampedRatio = Math.max(minRatio, Math.min(maxRatio, ratio));

    // 线性插值公式 (Lerp) 映射
    const scale = minScale + ((clampedRatio - minRatio) / (maxRatio - minRatio)) * (maxScale - minScale);
    
    return scale;
}

function initBackground() {

    const container = document.getElementById('glcanvas');

    if (!container) return;


    // 清理可能遗留的旧上下文
    container.innerHTML = ''; 

    cbScene = new THREE.Scene();
    cbCamera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
    
    cbRenderer = new THREE.WebGLRenderer({
        antialias: false,
        powerPreference: 'high-performance',
        alpha: true
    });


    
    cbRenderer.outputColorSpace = THREE.SRGBColorSpace;
    // 限制像素比以节省性能 (尤其是手机端)
    cbRenderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    cbRenderer.setClearColor(0x000000, 0); // Transparent 模式
    


    const w = window.innerWidth;
    const h = window.innerHeight;
    const initialScale = calculateOptimalScale();
    cbRenderer.setSize(w, h, false);
    container.appendChild(cbRenderer.domElement);

    cbClock = new THREE.Clock();

    // ==========================================
    // ColorBends 着色器源码
    // ==========================================
    const MAX_COLORS = 8;
    
    const vertexShader = `
        varying vec2 vUv;
        void main() {
            vUv = uv;
            gl_Position = vec4(position, 1.0);
        }
    `;

    const fragmentShader = `
        #define MAX_COLORS ${MAX_COLORS}
        uniform vec2 uCanvas;
        uniform float uTime;
        uniform float uSpeed;
        uniform vec2 uRot;
        uniform int uColorCount;
        uniform vec3 uColors[MAX_COLORS];
        uniform int uTransparent;
        uniform float uScale;
        uniform float uFrequency;
        uniform float uWarpStrength;
        uniform vec2 uPointer; // in NDC [-1,1]
        uniform float uMouseInfluence;
        uniform float uParallax;
        uniform float uNoise;
        uniform int uIterations;
        uniform float uIntensity;
        uniform float uBandWidth;
        varying vec2 vUv;

        void main() {
            float t = uTime * uSpeed;
            vec2 p = vUv * 2.0 - 1.0;
            p += uPointer * uParallax * 0.1;
            
            // 旋转坐标系
            vec2 rp = vec2(p.x * uRot.x - p.y * uRot.y, p.x * uRot.y + p.y * uRot.x);
            vec2 q = vec2(rp.x * (uCanvas.x / uCanvas.y), rp.y);
            
            q /= max(uScale, 0.0001);
            q /= 0.5 + 0.2 * dot(q, q);
            q += 0.2 * cos(t) - 7.56;
            
            vec2 toward = (uPointer - rp);
            q += toward * uMouseInfluence * 0.2;

            for (int j = 0; j < 5; j++) {
                if (j >= uIterations - 1) break;
                vec2 rr = sin(1.5 * (q.yx * uFrequency) + 2.0 * cos(q * uFrequency));
                q += (rr - q) * 0.15;
            }

            vec3 col = vec3(0.0);
            float a = 1.0;

            if (uColorCount > 0) {
                vec2 s = q;
                vec3 sumCol = vec3(0.0);
                float cover = 0.0;
                for (int i = 0; i < MAX_COLORS; ++i) {
                    if (i >= uColorCount) break;
                    s -= 0.01;
                    vec2 r = sin(1.5 * (s.yx * uFrequency) + 2.0 * cos(s * uFrequency));
                    float m0 = length(r + sin(5.0 * r.y * uFrequency - 3.0 * t + float(i)) / 4.0);
                    float kBelow = clamp(uWarpStrength, 0.0, 1.0);
                    float kMix = pow(kBelow, 0.3);
                    float gain = 1.0 + max(uWarpStrength - 1.0, 0.0); 
                    vec2 disp = (r - s) * kBelow;
                    vec2 warped = s + disp * gain;
                    float m1 = length(warped + sin(5.0 * warped.y * uFrequency - 3.0 * t + float(i)) / 4.0);
                    float m = mix(m0, m1, kMix);
                    float w = 1.0 - exp(-uBandWidth / exp(uBandWidth * m));
                    sumCol += uColors[i] * w;
                    cover = max(cover, w);
                }
                col = clamp(sumCol, 0.0, 1.0);
                a = uTransparent > 0 ? cover : 1.0;
            } else {
                vec2 s = q;
                for (int k = 0; k < 3; ++k) {
                    s -= 0.01;
                    vec2 r = sin(1.5 * (s.yx * uFrequency) + 2.0 * cos(s * uFrequency));
                    float m0 = length(r + sin(5.0 * r.y * uFrequency - 3.0 * t + float(k)) / 4.0);
                    float kBelow = clamp(uWarpStrength, 0.0, 1.0);
                    float kMix = pow(kBelow, 0.3);
                    float gain = 1.0 + max(uWarpStrength - 1.0, 0.0);
                    vec2 disp = (r - s) * kBelow;
                    vec2 warped = s + disp * gain;
                    float m1 = length(warped + sin(5.0 * warped.y * uFrequency - 3.0 * t + float(k)) / 4.0);
                    float m = mix(m0, m1, kMix);
                    col[k] = 1.0 - exp(-uBandWidth / exp(uBandWidth * m));
                }
                a = uTransparent > 0 ? max(max(col.r, col.g), col.b) : 1.0;
            }

            col *= uIntensity;

            if (uNoise > 0.0001) {
                float n = fract(sin(dot(gl_FragCoord.xy + vec2(uTime), vec2(12.9898, 78.233))) * 43758.5453123);
                col += (n - 0.5) * uNoise;
                col = clamp(col, 0.0, 1.0);
            }

            vec3 rgb = (uTransparent > 0) ? col * a : col;
            gl_FragColor = vec4(rgb, a);
        }
    `;

    // 注入单色参数 (恢复纯粹的单色质感)
    // ==========================================
    const uColorsArray = Array.from({ length: MAX_COLORS }, () => new THREE.Vector3(0, 0, 0));
    
    // 初始化时注入 1 个颜色：#A855F7 (紫色)
    const initColor = new THREE.Color("#A855F7");
    uColorsArray[0].set(initColor.r, initColor.g, initColor.b);

    cbUniforms = {
        uCanvas: { value: new THREE.Vector2(w, h) },
        uTime: { value: 0 },
        uSpeed: { value: 1.0 },              
        uRot: { value: new THREE.Vector2(1, 0) },
        
        // 【恢复为 1】：避免颜色混合导致的脏色，只用一层极其纯粹的流体光
        uColorCount: { value: 1 },           
        
        uColors: { value: uColorsArray },
        uTransparent: { value: 1 },          
        uScale: { value:  initialScale },              
        uFrequency: { value: 1.0 },          
        uWarpStrength: { value: 1.0 },       
        uPointer: { value: new THREE.Vector2(0, 0) },
        uMouseInfluence: { value: 0.0 },     
        uParallax: { value: 0.5 },           
        uNoise: { value: 0.0 },              
        uIterations: { value: 3 },           
        uIntensity: { value: 1.5 },          
        uBandWidth: { value: 1.0 }           
    };

    cbMaterial = new THREE.ShaderMaterial({
        vertexShader,
        fragmentShader,
        uniforms: cbUniforms,
        premultipliedAlpha: true,
        transparent: true
    });

    const geometry = new THREE.PlaneGeometry(2, 2);
    const mesh = new THREE.Mesh(geometry, cbMaterial);
    cbScene.add(mesh);

    // 窗口自适应
window.addEventListener('resize', () => {
        if (!cbRenderer || !container) return;
        const nw = window.innerWidth;
        const nh = window.innerHeight;
        cbRenderer.setSize(nw, nh, false);
        // 动态更新 Shader 变量
        cbUniforms.uCanvas.value.set(nw, nh);
        cbUniforms.uScale.value = calculateOptimalScale();
    });

    // 启动动画循环
    renderBackground();
}

// ==========================================
// 动态联动与渲染循环
// ==========================================

// ==========================================
// 动态联动与渲染循环
// ==========================================




// 供 light_control.js 调用的外部接口
function updateBackgroundState(volume, h, s, l) {
    if (!cbMaterial) return;
    // 1. 接收物理音量 (由虚拟钢琴引擎计算出的 0.0 ~ 1.0 包络)
    cbTargetEnergy = volume*1.2;
    
    // 2. 接收和弦颜色
    // 为了防止背景太刺眼抢了前景 UI 的风头，我们把传入的明度(l)压低固定在 40% 左右
    // Three.js 的 setHSL 接收 0.0 ~ 1.0 的小数
    tempThreeColor.setHSL(h / 360, s / 100, 0.40);
    cbTargetColor.set(tempThreeColor.r, tempThreeColor.g, tempThreeColor.b);
}

// 主渲染循环
function renderBackground() {


    if (!cbMaterial || !cbRenderer) return;

    const dt = cbClock.getDelta();
    const elapsed = cbClock.elapsedTime;
    
    // ==========================================
    // 物理能量与色彩平滑系统 (Smoothing)
    // ==========================================
    // 能量平滑 (带来阻尼感)
    cbCurrentEnergy += (cbTargetEnergy - cbCurrentEnergy) * 0.1;
    
    // 色彩平滑 (和弦切换时色彩溶解过渡)
    cbCurrentColor.lerp(cbTargetColor, 0.05);

    // ==========================================
    // 映射 Shader 参数 (Mapping)
    // ==========================================
    cbUniforms.uTime.value = elapsed;

    // 1. 颜色写入
    cbUniforms.uColors.value[0].copy(cbCurrentColor);

    // // 2. 能量爆发：音量越大，亮度(Intensity)、流速(Speed)、扭曲度(Warp) 越狂暴！
    cbUniforms.uIntensity.value = 0.5 + (cbCurrentEnergy * 3.0);    // 从 0.5 飙升到 4.5

    // Auto Rotate 逻辑 (缓慢自转)
    const baseRotation = 7;
    const autoRotateSpeed = 5;
    const deg = (baseRotation % 360) + autoRotateSpeed * elapsed;
    const rad = (deg * Math.PI) / 180;
    cbUniforms.uRot.value.set(Math.cos(rad), Math.sin(rad));

    // 渲染输出
    cbRenderer.render(cbScene, cbCamera);
    cbRafId = requestAnimationFrame(renderBackground);
}