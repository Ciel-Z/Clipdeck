import { build } from "esbuild";
import { copyFile, mkdir, readFile, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";

await mkdir("dist", { recursive: true });
await build({
  entryPoints: ["src/editor.ts"],
  outfile: "dist/editor.js",
  bundle: true,
  minify: true,
  sourcemap: false,
  target: ["safari14", "chrome65"],
  format: "iife",
  legalComments: "none"
});
await Promise.all([
  copyFile("src/index.html", "dist/index.html"),
  copyFile("src/editor.css", "dist/editor.css")
]);

const files = ["index.html", "editor.js", "editor.css"];
const hashes = Object.fromEntries(await Promise.all(files.map(async file => {
  const contents = await readFile(`dist/${file}`);
  return [file, createHash("sha256").update(contents).digest("hex")];
})));
await writeFile("dist/manifest.json", `${JSON.stringify({ version: "0.1.0", hashes }, null, 2)}\n`);
