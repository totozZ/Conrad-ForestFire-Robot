import { test, expect } from "@playwright/test";
import { readFile } from "node:fs/promises";
test("dashboard, hold-to-run, cutter interlock, logs and export", async ({
  page,
}, testInfo) => {
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto("/");
  await expect(page.locator("#environment")).toContainText("模拟模式");
  await expect(page.locator(".motor-card")).toHaveCount(3);
  await page.getByRole("button", { name: "取得控制权", exact: true }).click();
  await expect(page.locator("#control-state")).toContainText("已控制");
  await page.locator("[data-tab=test]").click();
  await page.getByRole("button", { name: "返回整车控制模式" }).click();
  await page.locator("[data-tab=drive]").click();
  await page.getByRole("button", { name: "复位 / 准备" }).click();
  await page.getByRole("button", { name: "使能左右轮", exact: true }).click();
  const forward = page.getByRole("button", { name: "前进", exact: true });
  await expect(forward).toBeEnabled();
  await forward.scrollIntoViewIfNeeded();
  const box = await forward.boundingBox();
  const point = {
    x: Math.round(box.x + box.width / 2),
    y: Math.round(box.y + box.height / 2),
  };
  let touch;
  if (testInfo.project.name === "mobile") {
    touch = await page.context().newCDPSession(page);
    await touch.send("Input.dispatchTouchEvent", {
      type: "touchStart",
      touchPoints: [point],
    });
  } else {
    await page.mouse.move(point.x, point.y);
    await page.mouse.down();
  }
  await expect(page.locator(".motor-card").first()).toContainText("目标 0.20");
  if (touch)
    await touch.send("Input.dispatchTouchEvent", {
      type: "touchEnd",
      touchPoints: [],
    });
  else {
    await page.mouse.move(1300, 100);
    await page.mouse.up();
  }
  await expect(page.locator(".motor-card").first()).toContainText("目标 0.00");
  await page.getByRole("button", { name: "2. 使能刀片" }).click();
  await expect(page.locator("#notice")).toContainText("解锁刀片");
  await page.getByRole("button", { name: "1. 解锁刀片" }).click();
  await page.getByRole("button", { name: "2. 使能刀片" }).click();
  await expect(page.locator(".motor-card").nth(2)).toContainText("已使能");
  await page.getByRole("button", { name: "3. 启动刀片" }).click();
  await expect(page.locator(".motor-card").nth(2)).toContainText("目标 0.20");
  await page.getByRole("button", { name: /全部停止/ }).click();
  await expect(page.locator("#control-state")).toContainText("停止锁定");
  await page.locator("[data-tab=logs]").click();
  await expect(page.locator("#event-list")).toContainText("software_stop");
  const download = page.waitForEvent("download");
  await page.getByRole("button", { name: "导出 CSV" }).click();
  const file = await download;
  expect(file.suggestedFilename()).toContain("simulation");
  const csv = await readFile(await file.path(), "utf8");
  expect(csv).toContain("actual_rps");
  expect(csv).toContain("simulation");
  expect(csv).toContain("software_stop");
  await page.screenshot({
    path: testInfo.outputPath("logs.png"),
    fullPage: true,
  });
  await page.locator("[data-tab=drive]").click();
  await page.screenshot({
    path: testInfo.outputPath("dashboard.png"),
    fullPage: true,
  });
  expect(errors).toEqual([]);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    ),
  ).toBe(true);
});
test("single-motor mode gates driving and enforces configured speed limit", async ({
  page,
}) => {
  await page.goto("/");
  await expect(page.locator("#environment")).toContainText("模拟模式");
  await page.getByRole("button", { name: "取得控制权", exact: true }).click();
  await page.locator("[data-tab=test]").click();
  await page.locator("input[name=max_rps]").fill("0.5");
  await page.getByRole("button", { name: "保存所选电机配置" }).click();
  await expect(page.locator("#notice")).toContainText("命令已接受");
  await page.getByRole("button", { name: "进入所选电机测试模式" }).click();
  await expect(page.locator("#mode-status")).toContainText("单机测试 / 左行走");
  await page.getByRole("button", { name: "复位 / 准备" }).click();
  await page.getByRole("button", { name: "使能所选电机" }).click();
  await expect(page.locator("#test-run")).toBeEnabled();
  await page.locator("#test-rps").fill("100");
  await page.locator("#test-run").focus();
  await page.keyboard.down("Space");
  await expect(page.locator(".motor-card").first()).toContainText("目标 0.50");
  await page.keyboard.up("Space");
  await expect(page.locator(".motor-card").first()).toContainText("目标 0.00");
  await page.getByRole("button", { name: /全部停止/ }).click();
  await expect(page.locator("#test-run")).toBeDisabled();
  await page.locator("[data-tab=drive]").click();
  await expect(
    page.getByRole("button", { name: "前进", exact: true }),
  ).toBeDisabled();
});
test("blur stops control, reconnect requires manual claim; configuration remains editable", async ({
  page,
}, testInfo) => {
  await page.goto("/");
  await expect(page.locator("#environment")).toContainText("模拟模式");
  await page.getByRole("button", { name: "取得控制权", exact: true }).click();
  await expect(page.locator("#control-state")).toContainText("已控制");
  await page.evaluate(() => window.dispatchEvent(new Event("blur")));
  await expect(page.locator("#control-state")).toContainText("等待取得控制权");
  await page.getByRole("button", { name: "重新连接" }).click();
  await expect(page.locator("#control-state")).toContainText("等待取得控制权");
  await page.getByRole("button", { name: "取得控制权", exact: true }).click();
  await page.locator("[data-tab=test]").click();
  await expect(page.locator("input[name=can_id]")).toHaveValue("1");
  await page.locator("input[name=max_rps]").fill("0.8");
  await page.getByRole("button", { name: "保存所选电机配置" }).click();
  await expect(page.locator("#notice")).toContainText("命令已接受");
  await page.screenshot({
    path: testInfo.outputPath("configuration.png"),
    fullPage: true,
  });
});
