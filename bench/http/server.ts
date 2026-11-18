// HTTP/1.1 server for bench/http (see client.ts). PORT, and with STOP=1 it
// stops itself (SIGSTOP) after WARM requests and again after WARM+N, so that
// count-helpers.py (gdb) counts helper calls for exactly N requests.
const n = Number(process.env.N ?? 200), warm = Number(process.env.WARM ?? 50), stopper = !!process.env.STOP;
let count = 0;
const server = Bun.serve({
  port: Number(process.env.PORT ?? 18080),
  fetch() {
    count++;
    if (stopper && (count === warm || count === warm + n)) setTimeout(() => process.kill(process.pid, "SIGSTOP"), 0);
    return new Response("ok", { headers: { "content-type": "text/plain", "x-served-by": "bench" } });
  },
});
console.log("ready", server.port);
