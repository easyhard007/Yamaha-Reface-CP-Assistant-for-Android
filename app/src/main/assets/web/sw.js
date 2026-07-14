// ==========================================
// 强制刷新版 Service Worker
// ==========================================
const CACHE_NAME = 'yamaha-reface-cp-assistant-v0.25';

const urlsToCache = [
  './',
  './index.html',
  './style.css',
  './keyboard.js',
  './background_animation.js',
  './scale_chord_engine.js',
  './modulation_engine.js',
  './color_mapping.js',
  './virtual_piano_engine.js',
  './light_control.js',
  './midi_util.js',
  './auto_sustain.js',
  './midi_enhance.js',

  './manifest.json',
  './icon.png',

  './vendor/three.min.js',
  './vendor/tonal.min.js',
  './vendor/iro.min.js',
  './vendor/NoSleep.min.js'

];

// 安装阶段：缓存所有文件，并强制跳过等待期
self.addEventListener('install', event => {
    // 【核心指令 1】：skipWaiting() 强迫 ServiceWorker 一旦下载完毕，立刻进入激活状态，不排队！
    self.skipWaiting();
    
    event.waitUntil(
        caches.open(CACHE_NAME)
            .then(cache => cache.addAll(urlsToCache))
    );
});

// 激活阶段：清理旧版本缓存，并立刻接管所有页面
self.addEventListener('activate', event => {
    // 【核心指令 2】：clients.claim() 强迫新的 ServiceWorker 立刻接管所有打开的页面，不用等用户重启！
    event.waitUntil(self.clients.claim());
    
    event.waitUntil(
        caches.keys().then(cacheNames => {
            return Promise.all(
                cacheNames.map(cacheName => {
                    // 如果发现旧版本名字，无情删除
                    if (cacheName !== CACHE_NAME) {
                        console.log('删除旧缓存:', cacheName);
                        return caches.delete(cacheName);
                    }
                })
            );
        })
    );
});

// 拦截请求阶段：为了开发调试，我们采用 "Network First, falling back to cache" (网络优先，断网才用缓存)
// 这样每次刷新都会尝试拉取最新代码！
self.addEventListener('fetch', event => {
    event.respondWith(
        fetch(event.request).catch(() => {
            // 只有在断网时，才会退回去读缓存
            return caches.match(event.request);
        })
    );
});