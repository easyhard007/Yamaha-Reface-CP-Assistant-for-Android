// ===== Canvas 2D Scatter (双层 + 30fps) =====
var _scData = [];
var _scRange = 3200, _scFadeMs = 8000, _scSweepStart = performance.now();
var _scGridDirty = true, _scLastFrame = 0;

function scatterSetRange(ms) { _scRange = ms; _scGridDirty = true; scatterSync(); }
function scatterSync() { _scSweepStart = performance.now(); _scData = []; }
function scatterAddNote(p, v) {
    _scData.push([ ((performance.now() - _scSweepStart) % _scRange + _scRange) % _scRange, p, v, Date.now() ]);
}
setInterval(function(){ var n = Date.now(); _scData = _scData.filter(function(d){ return n - d[3] <= 10000; }); }, 150);

function scatterDrawGrid() {
    var c = document.getElementById('scatter-grid');
    if (!c) return; var W = c.clientWidth, H = c.clientHeight;
    if (W <= 0 || H <= 0) return; c.width = W; c.height = H;
    var ctx = c.getContext('2d'); ctx.clearRect(0, 0, W, H);
    function mx(x) { return ((x + 0.125 * _scRange) / (1.25 * _scRange)) * W; }
    for (var i = 0; i <= 4; i++) {
        ctx.beginPath(); ctx.moveTo(mx(i * 0.25 * _scRange), 0); ctx.lineTo(mx(i * 0.25 * _scRange), H);
        ctx.strokeStyle = '#333'; ctx.lineWidth = 1; ctx.stroke();
    }
    ctx.fillStyle = '#888'; ctx.font = '9px monospace'; ctx.textAlign = 'center';
    for (var i = 0; i <= 4; i++) ctx.fillText((i * 0.25 * _scRange).toFixed(0), mx(i * 0.25 * _scRange), H - 3);
    _scGridDirty = false;
}

function scatterDraw() {
    var np = performance.now();
    if (np - _scLastFrame < 33) { requestAnimationFrame(scatterDraw); return; }
    _scLastFrame = np;
    if (_scGridDirty) scatterDrawGrid();
    var c = document.getElementById('scatter-chart');
    if (!c || c.clientWidth <= 0) { requestAnimationFrame(scatterDraw); return; }
    c.width = c.clientWidth; c.height = c.clientHeight;
    var ctx = c.getContext('2d'); ctx.clearRect(0, 0, c.width, c.height);
    var r = _scRange;
    function mx(x) { return ((x + 0.125 * r) / (1.25 * r)) * c.width; }
    function my(y) { return c.height - ((y - 20) / 70) * c.height; }
    var sm = (performance.now() - _scSweepStart) % r; if (sm < 0) sm += r;
    var z = Math.floor(sm / (0.25 * r));
    if (z >= 0 && z < 4) { ctx.fillStyle = 'rgba(255,255,255,0.05)'; ctx.fillRect(mx(z * 0.25 * r), 0, mx((z + 1) * 0.25 * r) - mx(z * 0.25 * r), c.height); }
    var now = Date.now();
    for (var i = 0; i < _scData.length; i++) {
        var d = _scData[i], x = d[0], y = d[1], v = d[2], ts = d[3];
        var fo = 1 - ((now - ts) / _scFadeMs); if (fo <= 0) continue;
        ctx.globalAlpha = Math.min(1, (0.5 + v / 127 * 0.5) * fo);
        ctx.fillStyle = '#fff';
        [x, x - r, x + r].forEach(function(px) {
            var sx = mx(px); if (sx >= -20 && sx <= c.width + 20) { ctx.beginPath(); ctx.arc(sx, my(y), 4, 0, Math.PI * 2); ctx.fill(); }
        });
    }
    ctx.globalAlpha = 1.0;
    ctx.strokeStyle = 'rgba(99,102,241,0.8)'; ctx.lineWidth = 2;
    var bx = mx(sm); ctx.beginPath(); ctx.moveTo(bx, 0); ctx.lineTo(bx, c.height); ctx.stroke();
    requestAnimationFrame(scatterDraw);
}
requestAnimationFrame(scatterDraw);
