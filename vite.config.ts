import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";

const { version } = JSON.parse(readFileSync(resolve(__dirname, "package.json"), "utf8")) as { version: string };

export default defineConfig({
  plugins: [react()],
  root: "src/renderer",
  base: "./",
  // The interface ships inside the same executable as the host, so the
  // version it was built with is the version that is running.
  define: {
    __APP_VERSION__: JSON.stringify(version)
  },
  build: {
    outDir: "../../dist/renderer",
    emptyOutDir: true
  },
  server: {
    port: 5173,
    strictPort: true
  },
  resolve: {
    alias: {
      "@shared": resolve(__dirname, "src/shared")
    }
  }
});
