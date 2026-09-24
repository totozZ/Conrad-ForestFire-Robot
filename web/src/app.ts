export {};
type Name = "left_drive" | "right_drive" | "cutter";
type Config = {
  verified: boolean;
  model: "H6215" | "H3510";
  mode: string;
  can_id: number;
  feedback_id: number;
  direction: number;
  max_rps: number;
  accel_rps2: number;
  v_max_rad_s: number;
  feedback_timeout_ms: number;
};
type Motor = {
  name: Name;
  config: Config;
  online: boolean;
  requested: boolean;
  enabled: boolean | null;
  target_rps: number;
  output_rps: number;
  actual_rps: number | null;
  age_ms: number | null;
  fault: number | null;
  mos_c: number | null;
  rotor_c: number | null;
  current_a: number | null;
};
type EventRecord = { seq: number; ms: number; code: string };
type State = {
  now_ms: number;
  session: number;
  owner: number;
  simulation: boolean;
  board_ready: boolean;
  bus_healthy: boolean;
  locked: boolean;
  cutter_unlocked: boolean;
  reason: string;
  mode: string;
  selected: Name;
  motors: Motor[];
  events: EventRecord[];
};
type Reply = {
  v: 1;
  type: string;
  id: number | null;
  accepted: boolean;
  code: string;
  now_ms: number;
  state?: State;
};
const $ = <T extends HTMLElement = HTMLElement>(id: string) =>
  document.getElementById(id) as T;
const names: Record<Name, string> = {
  left_drive: "左行走",
  right_drive: "右行走",
  cutter: "刀片机构",
};
const errors: Record<string, string> = {
  not_owner: "请先取得控制权",
  owner_busy: "已有其他操作端控制机器人",
  locked: "系统已锁定，请检查后复位",
  board_unverified: "板级接线与参数尚未确认",
  unverified_config: "硬件或电机配置尚未确认",
  motor_offline: "电机离线，禁止使能",
  enable_unconfirmed: "等待电机有效使能反馈",
  cutter_locked: "请先独立解锁刀片",
  wrong_mode: "当前模式不允许此操作",
  not_stopped: "请先停止所有电机并等待停转",
  must_lock_and_stop: "请先全部停止并等待停转后保存",
  id_collision: "CAN ID 或反馈 ID 存在冲突",
  invalid_config: "配置不合法，请核对字段、单位与范围",
  expired_command: "命令已过期，请检查连接",
  simulation_only: "此操作仅用于模拟模式",
  bus_fault: "CAN 总线故障尚未排除",
  motor_fault: "电机故障尚未排除",
  already_enabled: "电机已请求使能",
  config_persist_failed: "配置保存失败，原配置已保留",
};
let socket: WebSocket | null = null,
  state: State | undefined,
  seq = 0,
  clockOffset = 0,
  lastStateAt = 0;
let currentTab = "drive",
  engaged = true,
  configLoadedFor = "",
  lastSession = 0;
let connectionEpoch = 0;
const pending = new Map<
  number,
  {
    resolve: (reply: Reply) => void;
    reject: (e: Error) => void;
    timer: number;
    op: string;
  }
>();
const rows: string[][] = [];
const history: { ms: number; values: (number | null)[] }[] = [];
const seenEvents = new Set<string>();
const csvHeaders = [
  "connection",
  "source",
  "robot_ms",
  "kind",
  "motor",
  "target_rps",
  "output_rps",
  "actual_rps",
  "online",
  "enabled",
  "feedback_age_ms",
  "fault",
  "mos_c",
  "rotor_c",
  "event_or_ack",
];
function notify(text: string, error = false) {
  $("notice").textContent = text;
  $("notice").classList.toggle("error", error);
}
function record(row: string[]) {
  rows.push(row);
  if (rows.length > 30000) rows.shift();
}
function isOwner() {
  return (
    !!state &&
    state.owner === state.session &&
    state.owner !== 0 &&
    socket?.readyState === WebSocket.OPEN
  );
}
function send(op: string, args: object = {}): Promise<Reply> {
  return new Promise((resolve, reject) => {
    if (socket?.readyState !== WebSocket.OPEN) {
      reject(new Error("连接已断开"));
      return;
    }
    if (socket.bufferedAmount > 4096) {
      socket.close();
      reject(new Error("发送积压，已断开连接"));
      return;
    }
    const id = ++seq;
    const timer = window.setTimeout(() => {
      pending.delete(id);
      reject(new Error("命令应答超时"));
    }, 1200);
    pending.set(id, { resolve, reject, timer, op });
    socket.send(
      JSON.stringify({
        v: 1,
        id,
        issued_ms: Math.floor(performance.now() + clockOffset),
        op,
        args,
      }),
    );
  });
}
async function action(op: string, args: object = {}) {
  try {
    const reply = await send(op, args);
    if (!reply.accepted) {
      notify(errors[reply.code] || reply.code, true);
      return false;
    }
    notify(
      reply.code === "clamped"
        ? "已按配置限幅；请查看目标与实际反馈。"
        : "命令已接受；执行结果以电机反馈为准。",
    );
    return true;
  } catch (e) {
    notify((e as Error).message, true);
    return false;
  }
}
function discardPending() {
  for (const p of pending.values()) {
    clearTimeout(p.timer);
    p.reject(new Error("连接已断开"));
  }
  pending.clear();
}
function disconnected() {
  state = undefined;
  lastStateAt = 0;
  held = null;
  $("connection").textContent = "未连接";
  $("connection-dot").classList.add("muted");
  $("environment").textContent =
    "连接中断 · 无有效反馈，旧读数已隐藏。重新连接后不会自动恢复动作。";
  $("control-state").textContent = "连接中断";
  $("lock-reason").textContent = "需要重新取得控制权";
  $("motor-cards").replaceChildren();
  refreshControls();
}
function connect() {
  const old = socket;
  socket = null;
  old?.close();
  discardPending();
  disconnected();
  seq = 0;
  configLoadedFor = "";
  connectionEpoch++;
  lastSession = 0;
  seenEvents.clear();
  history.length = 0;
  const ws = new WebSocket(
    `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`,
  );
  socket = ws;
  $("connection").textContent = "连接中";
  ws.onopen = () => {
    if (socket === ws) void send("sync").catch(() => {});
  };
  ws.onmessage = (event) => {
    if (socket !== ws) return;
    const reply = JSON.parse(event.data) as Reply;
    clockOffset = reply.now_ms - performance.now();
    if (reply.id !== null) {
      const p = pending.get(reply.id);
      if (p) {
        clearTimeout(p.timer);
        pending.delete(reply.id);
        p.resolve(reply);
        if (p.op !== "sync" && p.op !== "heartbeat")
          record([
            String(connectionEpoch),
            state?.simulation ? "simulation" : "real_or_unknown",
            String(reply.now_ms),
            "ack",
            "",
            "",
            "",
            "",
            "",
            "",
            "",
            "",
            "",
            "",
            `${p.op}:${reply.accepted}:${reply.code}`,
          ]);
      }
    }
    if (reply.state) {
      state = reply.state;
      lastStateAt = performance.now();
      render();
    }
  };
  ws.onclose = () => {
    if (socket === ws) {
      socket = null;
      discardPending();
      disconnected();
    }
  };
  ws.onerror = () => notify("无法连接机器人，请检查热点或模拟服务。", true);
}
function refreshControls() {
  const connected = !!state && socket?.readyState === WebSocket.OPEN;
  const owner = isOwner();
  $("claim").toggleAttribute("disabled", !connected || owner || !!state?.owner);
  $("release").toggleAttribute("disabled", !owner);
  $("reset").toggleAttribute("disabled", !owner || !state?.locked);
  $("stop-all").toggleAttribute("disabled", !connected);
  for (const id of [
    "drive-enable",
    "drive-disable",
    "cutter-unlock",
    "cutter-enable",
    "cutter-start",
    "cutter-stop",
    "enter-test",
    "test-unlock",
    "test-enable",
    "test-stop",
    "enter-drive",
  ])
    $(id).toggleAttribute("disabled", !owner);
  const canDrive =
    owner &&
    !state?.locked &&
    state?.mode === "drive" &&
    state.motors.slice(0, 2).every((m) => m.online && m.enabled && m.requested);
  document
    .querySelectorAll<HTMLButtonElement>("[data-drive]")
    .forEach((b) => (b.disabled = !canDrive));
  const test = state?.motors.find(
    (m) => m.name === $<HTMLSelectElement>("test-motor").value,
  );
  $("test-run").toggleAttribute(
    "disabled",
    !(
      owner &&
      !state?.locked &&
      state?.mode === "test" &&
      state.selected === test?.name &&
      test?.online &&
      test?.enabled &&
      test?.requested
    ),
  );
  document.querySelector<HTMLButtonElement>(
    "#config-form button[type=submit]",
  )!.disabled = !owner || !state?.locked;
  document
    .querySelectorAll<HTMLButtonElement>("[data-fault]")
    .forEach((b) => (b.disabled = !owner || !state?.simulation));
}
function render() {
  if (!state) return;
  const s = state;
  if (lastSession !== s.session) {
    lastSession = s.session;
    history.length = 0;
  }
  $("connection").textContent = "已连接";
  $("connection-dot").classList.remove("muted");
  $("environment").textContent = s.simulation
    ? "模拟模式 · 反馈由软件生成，控制逻辑与固件共用，不连接真实 CAN。模拟参数不可直接用于实机。"
    : `真机模式 · ${s.board_ready ? "板级配置已确认，电机仍需独立验证。" : "板级配置未就绪，CAN 输出保持关闭。"}`;
  $("environment").classList.toggle("real", !s.simulation);
  $("control-state").textContent = isOwner()
    ? s.locked
      ? "已控制 · 停止锁定"
      : "已控制 · 准备就绪"
    : s.owner
      ? "只读观察"
      : "等待取得控制权";
  $("lock-reason").textContent =
    `${s.reason} · ${s.bus_healthy ? "总线正常" : "总线故障"}`;
  $("mode-status").textContent =
    `当前：${s.mode === "drive" ? "整车控制" : `单机测试 / ${names[s.selected]}`}。切换前需停用所有电机。`;
  $("cutter-tag").textContent = s.cutter_unlocked ? "已独立解锁" : "独立解锁";
  const limit = $<HTMLInputElement>("drive-limit");
  limit.max = String(
    Math.max(
      0.05,
      Math.min(...s.motors.slice(0, 2).map((m) => m.config.max_rps || 0.05)),
    ),
  );
  $("drive-limit-label").textContent = Number(limit.value).toFixed(2);
  const cards = s.motors.map((m) => {
    const node = document.createElement("article");
    node.className = "motor-card";
    // Only fixed markup enters innerHTML; data and event strings use textContent.
    node.innerHTML =
      '<div class="card-head"><span></span><span class="badge"></span></div><div class="speed-reading"></div><div class="card-details"><span></span><span></span><span></span></div>';
    node.querySelector(".card-head span")!.textContent =
      `${names[m.name]} · ${m.config.model}`;
    const badge = node.querySelector(".badge")!;
    badge.textContent = !m.config.verified
      ? "未配置"
      : !m.online
        ? "离线"
        : m.fault
          ? `故障 ${m.fault}`
          : m.enabled
            ? "已使能"
            : m.requested
              ? "等待使能"
              : "已停用";
    badge.classList.toggle("off", !m.online || !!m.fault);
    const value = node.querySelector(".speed-reading")!;
    value.textContent = m.actual_rps === null ? "—" : m.actual_rps.toFixed(2);
    const unit = document.createElement("small");
    unit.textContent = "rps";
    value.append(unit);
    const fields = node.querySelectorAll(".card-details span");
    fields[0].textContent = `目标 ${m.target_rps.toFixed(2)}`;
    fields[1].textContent = `反馈 ${m.age_ms === null ? "—" : m.age_ms + " ms"}`;
    fields[2].textContent = `线圈 ${m.rotor_c === null ? "—" : m.rotor_c + " °C"}`;
    return node;
  });
  $("motor-cards").replaceChildren(...cards);
  $("fault-panel").hidden = !s.simulation;
  $("footer-status").textContent =
    `会话 ${s.session} / ${s.mode === "drive" ? "整车" : "单机"} / 心跳 100 ms`;
  const source = s.simulation ? "simulation" : "real";
  for (const m of s.motors)
    record([
      String(connectionEpoch),
      source,
      String(s.now_ms),
      "sample",
      m.name,
      String(m.target_rps),
      String(m.output_rps),
      m.actual_rps === null ? "" : String(m.actual_rps),
      String(m.online),
      m.enabled === null ? "" : String(m.enabled),
      m.age_ms === null ? "" : String(m.age_ms),
      m.fault === null ? "" : String(m.fault),
      m.mos_c === null ? "" : String(m.mos_c),
      m.rotor_c === null ? "" : String(m.rotor_c),
      "",
    ]);
  for (const e of s.events) {
    const key = `${connectionEpoch}:${e.seq}`;
    if (!seenEvents.has(key)) {
      seenEvents.add(key);
      record([
        String(connectionEpoch),
        source,
        String(e.ms),
        "event",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        e.code,
      ]);
    }
  }
  if (seenEvents.size > 2048) seenEvents.clear();
  $("sample-count").textContent =
    `${rows.length} 条记录（最多保留 30,000 条，包含采样、应答和事件）`;
  $("event-list").replaceChildren(
    ...s.events
      .slice(-30)
      .reverse()
      .map((e) => {
        const li = document.createElement("li");
        const t = document.createElement("time");
        t.textContent = (e.ms / 1000).toFixed(1) + "s";
        li.append(t, document.createTextNode(e.code));
        return li;
      }),
  );
  history.push({ ms: s.now_ms, values: s.motors.map((m) => m.actual_rps) });
  while (
    history.length &&
    (history[0].ms < s.now_ms - 30000 || history.length > 310)
  )
    history.shift();
  if (currentTab === "logs") drawChart();
  if (!configLoadedFor) loadConfig();
  refreshControls();
}
function loadConfig() {
  const chosen = $<HTMLSelectElement>("test-motor").value;
  const motor = state?.motors.find((m) => m.name === chosen);
  if (!motor) return;
  const form = $<HTMLFormElement>("config-form");
  for (const [key, value] of Object.entries(motor.config)) {
    const field = form.elements.namedItem(key) as
      | HTMLInputElement
      | HTMLSelectElement
      | null;
    if (!field) continue;
    if (field instanceof HTMLInputElement && field.type === "checkbox")
      field.checked = value as boolean;
    else field.value = String(value);
  }
  configLoadedFor = chosen;
}
function drawChart() {
  const canvas = $<HTMLCanvasElement>("chart"),
    ctx = canvas.getContext("2d")!;
  ctx.clearRect(0, 0, 800, 260);
  const max = Math.max(
    0.5,
    ...history.flatMap((h) => h.values.map((v) => Math.abs(v ?? 0))),
  );
  ctx.font = "12px sans-serif";
  ctx.strokeStyle = "#e3e9df";
  ctx.fillStyle = "#80917c";
  for (let i = 0; i < 5; i++) {
    const y = 20 + i * 55;
    ctx.beginPath();
    ctx.moveTo(50, y);
    ctx.lineTo(790, y);
    ctx.stroke();
    ctx.fillText((max * (1 - i / 2)).toFixed(1), 3, y + 4);
  }
  const end = history.at(-1)?.ms ?? 0;
  ["#276146", "#c78a32", "#698fb2"].forEach((color, index) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    let previous = false;
    for (const sample of history) {
      const v = sample.values[index];
      if (v === null) {
        previous = false;
        continue;
      }
      const x = 790 - ((end - sample.ms) / 30000) * 740,
        y = 130 - (v / max) * 110;
      if (previous) ctx.lineTo(x, y);
      else ctx.moveTo(x, y);
      previous = true;
    }
    ctx.stroke();
  });
}
let held: { release: () => void } | null = null;
function bindHold(button: HTMLElement, press: () => void, release: () => void) {
  button.addEventListener("pointerdown", (e) => {
    if ((button as HTMLButtonElement).disabled || held) return;
    e.preventDefault();
    button.setPointerCapture(e.pointerId);
    held = { release };
    press();
  });
  const end = () => {
    if (held) {
      const h = held;
      held = null;
      h.release();
    }
  };
  for (const event of [
    "pointerup",
    "pointercancel",
    "lostpointercapture",
    "blur",
  ])
    button.addEventListener(event, end);
  button.addEventListener("keydown", (e) => {
    if (
      (e.key === " " || e.key === "Enter") &&
      !e.repeat &&
      !held &&
      !(button as HTMLButtonElement).disabled
    ) {
      e.preventDefault();
      held = { release };
      press();
    }
  });
  button.addEventListener("keyup", (e) => {
    if (e.key === " " || e.key === "Enter") {
      e.preventDefault();
      end();
    }
  });
}
function leave() {
  engaged = false;
  if (held) {
    held = null;
  }
  if (isOwner()) {
    void action("stop_all");
    void action("release");
  }
}
window.addEventListener("blur", leave);
window.addEventListener("pagehide", leave);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) leave();
  else engaged = true;
});
window.addEventListener("focus", () => {
  engaged = true;
});
$("reconnect").onclick = connect;
$("claim").onclick = () => {
  engaged = true;
  void action("claim");
};
$("reset").onclick = () => void action("reset");
$("release").onclick = () => void action("release");
$("stop-all").onclick = () => {
  held = null;
  void action("stop_all");
};
$<HTMLInputElement>("drive-limit").oninput = () => {
  $("drive-limit-label").textContent = Number(
    $<HTMLInputElement>("drive-limit").value,
  ).toFixed(2);
};
$("drive-enable").onclick = async () => {
  if (!(await action("enable", { motor: "left_drive" }))) return;
  if (!(await action("enable", { motor: "right_drive" })))
    await action("stop_all");
};
$("drive-disable").onclick = async () => {
  await action("stop_motor", { motor: "left_drive" });
  await action("stop_motor", { motor: "right_drive" });
};
document.querySelectorAll<HTMLElement>("[data-drive]").forEach((button) => {
  const [forward, turn] = button.dataset.drive!.split(",").map(Number);
  bindHold(
    button,
    () =>
      void action("drive", {
        forward,
        turn,
        limit_rps: Number($<HTMLInputElement>("drive-limit").value),
      }),
    () => void action("drive", { forward: 0, turn: 0, limit_rps: 0 }),
  );
});
$("cutter-unlock").onclick = () => void action("unlock_cutter");
$("cutter-enable").onclick = () => void action("enable", { motor: "cutter" });
$("cutter-start").onclick = () =>
  void action("speed", {
    motor: "cutter",
    rps: Number($<HTMLInputElement>("cutter-rps").value),
  });
$("cutter-stop").onclick = () => void action("stop_motor", { motor: "cutter" });
const testMotor = () => $<HTMLSelectElement>("test-motor").value;
$<HTMLSelectElement>("test-motor").onchange = () => {
  loadConfig();
  refreshControls();
};
$("enter-test").onclick = () =>
  void action("select", { mode: "test", motor: testMotor() });
$("enter-drive").onclick = () =>
  void action("select", { mode: "drive", motor: "left_drive" });
$("test-unlock").onclick = () => void action("unlock_cutter");
$("test-enable").onclick = () => void action("enable", { motor: testMotor() });
$("test-stop").onclick = () =>
  void action("stop_motor", { motor: testMotor() });
let heldMotor = "";
bindHold(
  $("test-run"),
  () => {
    heldMotor = testMotor();
    void action("speed", {
      motor: heldMotor,
      rps: Number($<HTMLInputElement>("test-rps").value),
    });
  },
  () => void action("speed", { motor: heldMotor, rps: 0 }),
);
$("load-config").onclick = loadConfig;
$<HTMLFormElement>("config-form").onsubmit = (event) => {
  event.preventDefault();
  const data = new FormData(event.currentTarget as HTMLFormElement);
  const config: Record<string, unknown> = {
    model: testMotor() === "cutter" ? "H3510" : "H6215",
    mode: data.get("mode"),
    verified: data.get("verified") === "on",
  };
  for (const key of [
    "can_id",
    "feedback_id",
    "direction",
    "max_rps",
    "accel_rps2",
    "v_max_rad_s",
    "feedback_timeout_ms",
  ])
    config[key] = Number(data.get(key));
  void action("configure", { motor: testMotor(), config });
};
document.querySelectorAll<HTMLButtonElement>("[data-tab]").forEach(
  (button) =>
    (button.onclick = () => {
      if (held) {
        const h = held;
        held = null;
        h.release();
      }
      currentTab = button.dataset.tab!;
      document
        .querySelectorAll<HTMLElement>(".tab")
        .forEach((t) => (t.hidden = t.id !== `tab-${currentTab}`));
      document
        .querySelectorAll(".nav")
        .forEach((b) => b.classList.toggle("active", b === button));
      $("page-title").textContent =
        { drive: "整车控制", test: "单电机调试", logs: "状态与记录" }[
          currentTab
        ] || "";
      if (currentTab === "logs") drawChart();
    }),
);
document
  .querySelectorAll<HTMLButtonElement>("[data-fault]")
  .forEach(
    (button) =>
      (button.onclick = () =>
        void action("inject", {
          motor: $<HTMLSelectElement>("fault-motor").value,
          kind: button.dataset.fault,
        })),
  );
$("export-csv").onclick = () => {
  const escape = (s: string) => '"' + s.replace(/"/g, '""') + '"';
  const csv =
    "\uFEFF" +
    [csvHeaders, ...rows].map((row) => row.map(escape).join(",")).join("\r\n");
  const url = URL.createObjectURL(
    new Blob([csv], { type: "text/csv;charset=utf-8" }),
  );
  const a = document.createElement("a");
  a.href = url;
  a.download = `conrad-${state?.simulation ? "simulation" : "session"}-${new Date().toISOString().replace(/[:.]/g, "-")}.csv`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
};
setInterval(() => {
  if (socket?.readyState !== WebSocket.OPEN) return;
  if (lastStateAt && performance.now() - lastStateAt > 600) {
    socket.close();
    return;
  }
  if (engaged && !document.hidden && isOwner())
    void send("heartbeat")
      .then((r) => {
        if (!r.accepted) notify(errors[r.code] || r.code, true);
      })
      .catch(() => {});
  void send("sync").catch(() => {});
}, 100);
connect();
