import { build } from "esbuild";
import { mkdir, copyFile, writeFile, readFile } from "node:fs/promises";
import { createHash } from "node:crypto";
await mkdir("web/dist", { recursive: true });
await build({
  entryPoints: ["web/src/app.ts"],
  outfile: "web/dist/app.js",
  bundle: true,
  minify: true,
  target: "es2022",
});
for (const file of ["index.html", "style.css"])
  await copyFile(`web/${file}`, `web/dist/${file}`);
const hashes = {};
for (const file of ["index.html", "app.js", "style.css"])
  hashes[file] = createHash("sha256")
    .update(await readFile(`web/dist/${file}`))
    .digest("hex");
await writeFile(
  "web/dist/manifest.json",
  JSON.stringify({ version: "0.1.0", sha256: hashes }, null, 2),
);
console.log("Web assets built; ready for ESP-IDF embedding.");
