// Qulf Nazorati — Service Worker
// Maqsad: sayt "qobig'i"ni (HTML/CSS/JS) offline holatda ham ochilishi uchun keshlash.
// Google login, EmailJS va Firebase real vaqt ma'lumotlari internet talab qiladi —
// bular offline ishlamaydi, lekin oxirgi ko'rilgan qulf kodi/holati localStorage'dan ko'rsatiladi.

const CACHE_NAME = 'qulf-nazorati-v1';
const APP_SHELL = [
  './',
  './index.html',
  './manifest.json'
];

self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE_NAME).then(cache => cache.addAll(APP_SHELL))
  );
  self.skipWaiting();
});

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)))
    )
  );
  self.clients.claim();
});

self.addEventListener('fetch', event => {
  const url = event.request.url;

  // Firebase, EmailJS va Google so'rovlariga tegmaymiz — ular internetni talab qiladi
  if (url.includes('firebaseio.com') || url.includes('googleapis.com') ||
      url.includes('emailjs.com') || url.includes('google.com')) {
    return;
  }

  event.respondWith(
    caches.match(event.request).then(cached => {
      if (cached) return cached;
      return fetch(event.request).then(response => {
        if (response.ok && event.request.method === 'GET') {
          const clone = response.clone();
          caches.open(CACHE_NAME).then(cache => cache.put(event.request, clone));
        }
        return response;
      }).catch(() => cached);
    })
  );
});
