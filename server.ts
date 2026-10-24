import { resolve, sep } from "node:path";

const PORT = Number(process.env.PORT ?? 2555);
const ROOT = resolve(import.meta.dir, "web");
const ALLOWED_HOSTS = (process.env.ALLOWED_HOSTS ?? "seb777.com,localhost,127.0.0.1")
  .split(",")
  .map((h) => h.trim().toLowerCase());

const hostOf = (req: Request) => (req.headers.get("host") ?? "").toLowerCase().replace(/:\d+$/, "");

Bun.serve({
  port: PORT,
  hostname: "0.0.0.0",
  async fetch(req) {
    if (!ALLOWED_HOSTS.includes(hostOf(req))) return new Response("Forbidden", { status: 403 });
    if (req.method !== "GET" && req.method !== "HEAD") return new Response("Method Not Allowed", { status: 405 });

    let path: string;
    try {
      path = decodeURIComponent(new URL(req.url).pathname);
    } catch {
      return new Response("Bad Request", { status: 400 });
    }
    if (path.endsWith("/")) path += "index.html";

    const file = resolve(ROOT, "." + path);
    if (file !== ROOT && !file.startsWith(ROOT + sep)) return new Response("Forbidden", { status: 403 });

    const f = Bun.file(file);
    if (!(await f.exists())) return new Response("Not Found", { status: 404 });
    return new Response(req.method === "HEAD" ? null : f, { headers: { "Cache-Control": "no-cache" } });
  },
});

console.log(`Death Golf serving ${ROOT} on :${PORT} for hosts: ${ALLOWED_HOSTS.join(", ")}`);
