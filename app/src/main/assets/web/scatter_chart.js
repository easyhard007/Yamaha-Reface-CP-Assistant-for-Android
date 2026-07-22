// ===== Scatter Chart (双层 Canvas + 30fps 节流) =====
var _scData = [];           // [xMs, pitch, velocity, timestamp]
var _scRange = 3200;
var _scFadeMs = 3000;
var _scSweepStart = performance.now();
var _scGridDirty = true;    // range 变化时重绘网格
var _scLastFrame = 0;

function scatterSetRange(ms) { _scRange = ms; _scGridDirty = true; scatterSync(); }
function scatterSync() { _scSweepStart = performance.now(); _scData = []; }

function scatterAddNote(pitch, velocity) {
    _scData.push([ ((performance.now() - _scSweepStart) % _scRange + _scRange) % _scRange, pitch, velocity, Date.now() ]);
}

// Auto-cleanup 10s
setInterval(function() {
    var now = Date.now();
    _scData = _scData.filter(function(p) { return now - p[3] <= 10000; });
}, 150);

// 静态网格层 (仅 range 变化时重绘)
function scatterDrawGrid() {
    var c = document.getElementById('scatter-grid');
    if (!c) return;
    var W = c.clientWidth, H = c.clientHeight;
    if (W <= 0 || H <= 0) return;
    c.width = W; c.height = H;
    var ctx = c.getContext('2d');
    ctx.clearRect(0, 0, W, H);
    var r = _scRange;
    function mapX(x) { return ((x + 0.125*r) / (1.25*r)) * W; }
    // Grid lines
    for (var i = 0; i <= 4; i++) {
        var cx = mapX(i * 0.25 * r);
        ctx.beginPath(); ctx.moveTo(cx, 0); ctx.lineTo(cx, H);
        ctx.strokeStyle = '#333'; ctx.lineWidth = 1; ctx.stroke();
    }
    // Labels
    ctx.fillStyle = '#888'; ctx.font = '9px monospace'; ctx.textAlign = 'center';
    for (var i = 0; i <= 4; i++) {
        ctx.fillText((i * 0.25 * r).toFixed(0), mapX(i * 0.25 * r), H - 3);
    }
    _scGridDirty = false;
}

// 动态层: 散点 + 扫线 + 高亮 (30fps)
function scatterDraw() {
    var nowP = performance.now();
    if (nowP - _scLastFrame < 33) { requestAnimationFrame(scatterDraw); return; } // ~30fps
    _scLastFrame = nowP;

    if (_scGridDirty) scatterDrawGrid();

    var c = document.getElementById('scatter-chart');
    if (!c) { requestAnimationFrame(scatterDraw); return; }
    var W = c.clientWidth, H = c.clientHeight;
    if (W <= 0 || H <= 0) { requestAnimationFrame(scatterDraw); return; }
    c.width = W; c.height = H;
    var ctx = c.getContext('2d');
    ctx.clearRect(0, 0, W, H);

    var r = _scRange;
    var Y_MIN = 20, Y_MAX = 90;
    function mapX(x) { return ((x + 0.125*r) / (1.25*r)) * W; }
    function mapY(y) { return H - ((y - Y_MIN) / (Y_MAX - Y_MIN)) * H; }

    var sweepMs = (performance.now() - _scSweepStart) % r;
    if (sweepMs < 0) sweepMs += r;

    // Zone highlight
    var actZone = Math.floor(sweepMs / (0.25 * r));
    if (actZone >= 0 && actZone < 4) {
        ctx.fillStyle = 'rgba(255,255,255,0.05)';
        ctx.fillRect(mapX(actZone * 0.25 * r), 0, mapX((actZone + 1) * 0.25 * r) - mapX(actZone * 0.25 * r), H);
    }

    // Points
    var now = Date.now();
    for (var i = 0; i < _scData.length; i++) {
        var d = _scData[i];
        var x = d[0], y = d[1], vel = d[2], ts = d[3];
        var fadeOpac = 1 - ((now - ts) / _scFadeMs);
        if (fadeOpac <= 0) continue;
        ctx.globalAlpha = Math.min(1, (0.5 + vel/127*0.5) * fadeOpac);
        ctx.fillStyle = '#fff';
        [x, x - r, x + r].forEach(function(px) {
            var mx = mapX(px);
            if (mx >= -20 && mx <= W + 20) {
                ctx.beginPath(); ctx.arc(mx, mapY(y), 4, 0, Math.PI*2); ctx.fill();
            }
        });
    }
    ctx.globalAlpha = 1.0;

    // Sweep line
    ctx.strokeStyle = 'rgba(99,102,241,0.8)'; ctx.lineWidth = 2;
    var blx = mapX(sweepMs);
    ctx.beginPath(); ctx.moveTo(blx, 0); ctx.lineTo(blx, H); ctx.stroke();

    requestAnimationFrame(scatterDraw);
}
requestAnimationFrame(scatterDraw);
