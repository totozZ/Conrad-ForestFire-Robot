import { test } from "node:test";
import assert from "node:assert/strict";
import { WebSocket } from "ws";
import { startSimulator } from "../scripts/simulator.mjs";

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function client(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`);
  const pending = new Map();
  let sequence = 0,
    time = 0,
    received = 0,
    heartbeat;
  ws.on("message", (data) => {
    const reply = JSON.parse(data);
    time = reply.now_ms;
    received = performance.now();
    pending.get(reply.id)?.(reply);
    pending.delete(reply.id);
  });
  ws.on("error", () => {});
  await new Promise((resolve, reject) => {
    ws.once("open", resolve);
    ws.once("error", reject);
  });
  const api = {
    ws,
    command(op, args = {}, overrides = {}) {
      const payload = {
        v: 1,
        id: ++sequence,
        issued_ms: Math.floor(time + performance.now() - received),
        op,
        args,
        ...overrides,
      };
      return new Promise((resolve, reject) => {
        const timeout = setTimeout(
          () => reject(new Error(`No reply to ${op}`)),
          2000,
        );
        pending.set(payload.id, (reply) => {
          clearTimeout(timeout);
          resolve(reply);
        });
        ws.send(JSON.stringify(payload));
      });
    },
    async state() {
      return (await api.command("sync")).state;
    },
    async claim() {
      await api.state();
      assert.equal((await api.command("claim")).accepted, true);
      heartbeat = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN)
          void api.command("heartbeat").catch(() => {});
      }, 100);
    },
    stopHeartbeat() {
      clearInterval(heartbeat);
    },
    close() {
      clearInterval(heartbeat);
      ws.close();
    },
  };
  await api.state();
  return api;
}
async function rig(fn) {
  const sim = await startSimulator({ port: 0 });
  const clients = [];
  try {
    await fn(sim, async () => {
      const c = await client(sim.port);
      clients.push(c);
      return c;
    });
  } finally {
    for (const c of clients) c.close();
    await sim.close();
  }
}
async function startDrive(c) {
  await c.claim();
  assert.equal((await c.command("reset")).accepted, true);
  assert.equal(
    (await c.command("enable", { motor: "left_drive" })).accepted,
    true,
  );
  assert.equal(
    (await c.command("enable", { motor: "right_drive" })).accepted,
    true,
  );
  await wait(30);
  assert.equal(
    (await c.command("drive", { forward: 1, turn: 0, limit_rps: 0.3 }))
      .accepted,
    true,
  );
}

test("end-to-end HTTP assets, shared C++ controller, acceleration and software stop", async () =>
  rig(async (sim, connect) => {
    const page = await fetch(`http://127.0.0.1:${sim.port}/`);
    assert.equal(page.status, 200);
    assert.match(await page.text(), /林间/);
    const script = await fetch(`http://127.0.0.1:${sim.port}/app.js`);
    assert.equal(script.status, 200);
    const c = await connect();
    const initial = await c.state();
    assert.equal(initial.simulation, true);
    assert.equal(initial.locked, true);
    await startDrive(c);
    await wait(140);
    const moving = await c.state();
    assert.equal(moving.motors[0].target_rps, 0.3);
    assert.ok(moving.motors[0].actual_rps > 0);
    assert.equal((await c.command("stop_all")).accepted, true);
    await wait(50);
    const stopped = await c.state();
    assert.equal(stopped.locked, true);
    assert.equal(stopped.motors[0].requested, false);
    assert.equal(stopped.motors[0].target_rps, 0);
  }));
test("only owner moves, observer can stop, replay and expired commands rejected", async () =>
  rig(async (sim, connect) => {
    const a = await connect(),
      b = await connect();
    await startDrive(a);
    assert.equal((await b.command("claim")).code, "owner_busy");
    assert.equal(
      (await b.command("drive", { forward: 1, turn: 0, limit_rps: 1 })).code,
      "not_owner",
    );
    assert.equal(
      (await a.command("drive", {}, { issued_ms: -1000 })).code,
      "expired_command",
    );
    assert.equal(
      (await a.command("reset", {}, { id: 1 })).code,
      "duplicate_or_reordered",
    );
    assert.equal(
      (await b.command("stop_all", {}, { issued_ms: -1000 })).accepted,
      true,
    );
    assert.equal((await a.state()).locked, true);
  }));
test("500 ms heartbeat expiry locks all outputs even while observer polls", async () =>
  rig(async (sim, connect) => {
    const a = await connect(),
      b = await connect();
    await startDrive(a);
    a.stopHeartbeat();
    await wait(650);
    const s = await b.state();
    assert.equal(s.owner, 0);
    assert.equal(s.locked, true);
    assert.equal(s.reason, "heartbeat_timeout");
    for (const m of s.motors) {
      assert.equal(m.target_rps, 0);
      assert.equal(m.requested, false);
    }
    await a.claim();
    assert.equal((await a.state()).locked, true);
  }));
test("disconnect/reconnect and process reboot never restore motion", async () =>
  rig(async (sim, connect) => {
    const a = await connect();
    await startDrive(a);
    a.close();
    await wait(60);
    const b = await connect();
    let s = await b.state();
    assert.equal(s.locked, true);
    assert.equal(s.owner, 0);
    assert.equal(s.motors[0].target_rps, 0);
    await startDrive(b);
    b.stopHeartbeat();
    await sim.restart();
    const c = await connect();
    s = await c.state();
    assert.equal(s.locked, true);
    assert.equal(s.reason, "startup");
    assert.equal(s.owner, 0);
    assert.ok(s.motors.every((m) => m.target_rps === 0 && !m.requested));
  }));
test("offline cutter does not block driving; active feedback loss does lock", async () =>
  rig(async (sim, connect) => {
    const c = await connect();
    await c.claim();
    await c.command("inject", { motor: "cutter", kind: "offline" });
    await wait(340);
    assert.equal((await c.state()).motors[2].online, false);
    assert.equal((await c.command("reset")).accepted, true);
    await c.command("enable", { motor: "left_drive" });
    await c.command("enable", { motor: "right_drive" });
    await wait(30);
    assert.equal(
      (await c.command("drive", { forward: 1, turn: 0, limit_rps: 0.2 }))
        .accepted,
      true,
    );
    await c.command("inject", { motor: "left_drive", kind: "offline" });
    await wait(360);
    let s = await c.state();
    assert.equal(s.locked, true);
    assert.equal(s.reason, "feedback_timeout:left_drive");
    assert.equal(s.motors[0].actual_rps, null);
    assert.equal(s.motors[0].enabled, null);
    await c.command("inject", { motor: "left_drive", kind: "clear" });
    await wait(50);
    s = await c.state();
    assert.equal(s.locked, true);
    assert.equal(s.motors[0].requested, false);
  }));
test("driver and bus faults require explicit recovery and are logged", async () =>
  rig(async (sim, connect) => {
    const c = await connect();
    await startDrive(c);
    await c.command("inject", { motor: "right_drive", kind: "driver" });
    await wait(30);
    let s = await c.state();
    assert.equal(s.reason, "motor_fault:right_drive");
    assert.equal((await c.command("reset")).accepted, false);
    await c.command("inject", { motor: "right_drive", kind: "clear" });
    await wait(100);
    assert.equal((await c.command("reset")).accepted, true);
    await c.command("inject", { motor: "left_drive", kind: "can" });
    await wait(30);
    s = await c.state();
    assert.equal(s.reason, "bus_fault");
    assert.equal(s.locked, true);
    assert.ok(s.events.some((e) => e.code.includes("stop_delivery_failed")));
  }));
test("configuration validation and cutter interlock survive full websocket path", async () =>
  rig(async (sim, connect) => {
    const c = await connect();
    await c.claim();
    let s = await c.state();
    const config = { ...s.motors[0].config, can_id: 2 };
    assert.equal(
      (await c.command("configure", { motor: "left_drive", config })).code,
      "id_collision",
    );
    config.can_id = 1;
    config.max_rps = 0.5;
    assert.equal(
      (await c.command("configure", { motor: "left_drive", config })).accepted,
      true,
    );
    await c.command("reset");
    assert.equal(
      (await c.command("enable", { motor: "cutter" })).code,
      "cutter_locked",
    );
    await c.command("unlock_cutter");
    await c.command("enable", { motor: "cutter" });
    await wait(30);
    assert.equal(
      (await c.command("speed", { motor: "cutter", rps: 100 })).code,
      "clamped",
    );
    await c.command("stop_motor", { motor: "cutter" });
    s = await c.state();
    assert.equal(s.cutter_unlocked, false);
    assert.equal(s.motors[2].target_rps, 0);
  }));
