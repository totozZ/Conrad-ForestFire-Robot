import { defineConfig } from "@playwright/test";
const port = Number(process.env.UI_TEST_PORT || 18080);
const url = `http://127.0.0.1:${port}`;
export default defineConfig({
  testDir: "./tests/ui",
  fullyParallel: false,
  workers: 1,
  timeout: 20000,
  use: {
    baseURL: url,
    headless: true,
    screenshot: "only-on-failure",
  },
  webServer: {
    command: "node scripts/simulator.mjs",
    env: { SIM_PERSIST: "0", PORT: String(port) },
    url,
    reuseExistingServer: false,
    timeout: 15000,
  },
  projects: [
    { name: "desktop", use: { viewport: { width: 1440, height: 1050 } } },
    {
      name: "mobile",
      use: {
        viewport: { width: 390, height: 844 },
        isMobile: true,
        hasTouch: true,
      },
    },
  ],
});
