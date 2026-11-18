// bun bench/http/client.ts [N]: N sequential keep-alive fetch() requests with
// browser-like headers to 127.0.0.1:PORT; prints {n, seconds, rps, usPerReq}.
// Bun's fetch parses the responses with picohttpparser, which uses SSE4.2
// pcmpestri (translated by fakeavx on a Core 2). With STOP=1 it stops itself
// (SIGSTOP) before and after the measured requests, for count-helpers.py.
const port = Number(process.env.PORT ?? 18080), n = Number(process.argv[2] ?? 2000);
const headers = {
  "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36",
  "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
  "Accept-Language": "en-US,en;q=0.9,id;q=0.8",
  "Accept-Encoding": "gzip, deflate, br",
  "Cookie": "session=4f1c2a9be0d34c7a8f1e2d3c4b5a6978; theme=dark; lang=en",
  "Referer": "http://localhost/index.html",
  "Cache-Control": "no-cache",
};
const url = `http://127.0.0.1:${port}/api/items?page=2&sort=name`;
for (let i = 0; i < 50; i++) await (await fetch(url, { headers })).text(); // warm up
if (process.env.STOP) process.kill(process.pid, "SIGSTOP"); // count-helpers.py: start
const t0 = Bun.nanoseconds();
for (let i = 0; i < n; i++) await (await fetch(url, { headers })).text();
const s = (Bun.nanoseconds() - t0) / 1e9;
if (process.env.STOP) process.kill(process.pid, "SIGSTOP"); // count-helpers.py: stop
console.log(JSON.stringify({ n, seconds: s, rps: n / s, usPerReq: (s / n) * 1e6 }));
