import { defineConfig } from "vite";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { deviceExplorerMock } from "./mock/server.js";
import { DEVICEEXPLORER_PROTOCOL_VERSION } from "./protocol-version.js";

const webUiRoot = path.dirname(fileURLToPath(import.meta.url));
const pluginRoot = path.resolve(webUiRoot, "..");
const outputDirectory = path.join(pluginRoot, "Resources", "Web");
const protocolHeader = path.join(pluginRoot, "Source", "DeviceExplorerCore", "Public", "DeviceExplorerTypes.h");

function verifyProtocolVersion() {
  if (!existsSync(protocolHeader)) return;
  const match = readFileSync(protocolHeader, "utf8").match(/ProtocolVersion\s*=\s*(\d+)/);
  if (match && Number(match[1]) !== DEVICEEXPLORER_PROTOCOL_VERSION) {
    throw new Error(`WebUI protocol ${DEVICEEXPLORER_PROTOCOL_VERSION} does not match C++ protocol ${match[1]}`);
  }
}

// The manifest is committed with the bundle, so it must depend only on the
// sources. Build timestamps and commit hashes would make every rebuild dirty.
function writeUiManifest() {
  return {
    name: "deviceexplorer-ui-manifest",
    closeBundle() {
      mkdirSync(outputDirectory, { recursive: true });
      writeFileSync(
        path.join(outputDirectory, "ui-manifest.json"),
        `${JSON.stringify({
          uiVersion: process.env.npm_package_version ?? "0.0.0",
          protocolVersion: DEVICEEXPLORER_PROTOCOL_VERSION,
        }, null, 2)}\n`,
        "utf8",
      );
    },
  };
}

export default defineConfig(({ mode }) => {
  verifyProtocolVersion();
  const mock = mode === "mock" || mode.startsWith("mock-");
  const scenario = mode === "mock" ? "default" : mode.slice("mock-".length);
  const host = process.env.DEVICEEXPLORER_HOST_URL ?? "http://127.0.0.1:18080";
  return {
    base: "./",
    build: {
      outDir: outputDirectory,
      emptyOutDir: true,
      sourcemap: false,
      assetsDir: "assets",
    },
    plugins: [mock && deviceExplorerMock({ scenario }), writeUiManifest()].filter(Boolean),
    server: {
      host: "127.0.0.1",
      port: 5173,
      strictPort: true,
      proxy: mock ? undefined : {
        "/api": { target: host, changeOrigin: false },
        "/health": { target: host, changeOrigin: false },
      },
    },
  };
});
