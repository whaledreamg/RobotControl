/* 机器人摩擦辨识面板；所有运动只由显式的「启动已预览方案」按钮发起。 */
(function () {
  "use strict";
  var api = window.__frictionApi;
  if (!api) return;
  var $ = function (id) { return document.getElementById(id); };
  var state = { parameters: null, robots: [], dirtyRobotPlans: new Set(), plan: null, previewRevision: 0, sessionId: "", result: null, startKey: "", timer: null, polling: false };
  var legacy = {
    collision: { speeds: "0.07854,0.15708,0.23562,0.31416,0.3927,0.47124,0.54978,0.62832,0.70686,0.7854", acceleration: 6.28, halfTravel: 0.7854 },
    drag: { speeds: "0.03,0.06,0.09,0.12,0.15,0.18,0.21,0.24,0.27,0.3", acceleration: 3, halfTravel: 0.4 }
  };
  function show(id, value) { $(id).textContent = typeof value === "string" ? value : JSON.stringify(value, null, 2); }
  function error(e) { show("frictionMessage", e.message || String(e)); $("frictionMessage").scrollIntoView({ block: "nearest" }); }
  function run(fn) { show("frictionMessage", ""); return Promise.resolve().then(fn).catch(error); }
  function json(id) { return JSON.parse($(id).value); }
  function numericInput(element, label) {
    var text = element.value.trim(), value = Number(text);
    if (!text || !Number.isFinite(value)) throw Error("请完整填写有限数值：" + label);
    return value;
  }
  function number(id) { return numericInput($(id), id); }
  function chosenKinds() { return ["collision", "drag"].filter(function (k) { return $("frictionUse" + k).checked; }); }
  function invalidatePlan() { ++state.previewRevision; state.plan = null; state.startKey = ""; $("frictionStart").disabled = true; $("frictionReviewed").checked = false; }
  function setSession(id) { state.sessionId = id; $("frictionSession").value = id; }
  function showQueued() {
    if (state.dirtyRobotPlans.size) show("frictionQueued", { robots: state.robots, needsUpdate: Array.from(state.dirtyRobotPlans), message: "这些机器人已修改，请点击「将当前臂加入 / 更新方案」后再预览。" });
    else show("frictionQueued", state.robots.length ? state.robots : "尚未加入机器人；可依次读取并加入左右臂。直接预览时使用当前臂。");
  }
  function input(value, cls) {
    var e = document.createElement("input"); e.type = "number"; e.step = "any"; e.value = value; e.className = cls; e.style.width = "90px"; return e;
  }
  function cell(row, value) { var td = document.createElement("td"); if (value instanceof Node) td.appendChild(value); else td.textContent = value; row.appendChild(td); return td; }
  function speedsFor(kind) {
    var texts = $("friction" + kind + "Speeds").value.split(","), speeds = texts.map(Number), count = new Set(speeds).size;
    if (texts.some(function (v) { return !v.trim(); }) || count < 3 || count > 10 || speeds.some(function (v) { return !Number.isFinite(v) || v <= 0; })) throw Error("每套方案须填写 3～10 个不同的正速度档，以英文逗号分隔。");
    return speeds;
  }
  function referenceRows() {
    var p = state.parameters; if (!p) return;
    var quality = json("frictionQuality"), travel = {}, profiles = {}, kinds = chosenKinds();
    if (!kinds.length) kinds = ["collision", "drag"];
    ["collision", "drag"].forEach(function (kind) {
      var speed = Math.max.apply(null, speedsFor(kind)), acceleration = number("friction" + kind + "Acceleration");
      var width = quality.positionBinWidth === undefined ? 0.005 : quality.positionBinWidth;
      var windowMs = quality.steadyWindowMs === undefined ? 50 : quality.steadyWindowMs;
      var settleMs = quality.settleMs === undefined ? 250 : quality.settleMs;
      if (!(acceleration > 0) || !Number.isFinite(width) || width <= 0 || !Number.isFinite(windowMs) || windowMs <= 0 || !Number.isFinite(settleMs) || settleMs < 0) throw Error("请填写有效的加速度与质量阈值后再生成参考行程。");
      var required = (speed * speed / acceleration + Math.max(speed * (2 * settleMs + 30 * windowMs) / 1000, 30 * width)) / 2;
      travel[kind] = Math.max(legacy[kind].halfTravel, Math.ceil((required + 0.01) * 100) / 100);
      profiles[kind] = { speed: speed, acceleration: acceleration };
    });
    var half = Math.max.apply(null, kinds.map(function (kind) { return travel[kind]; }));
    var limits = p.limits, selected = false, unavailable = false, excluded = [];
    Array.from($("frictionAxes").rows).forEach(function (tr, i) {
      var center = 0, reason = "", values = limits && [limits.lower, limits.upper, limits.velocity, limits.acceleration].map(function (a) { return Array.isArray(a) ? a[i] : undefined; });
      if (!values || !values.every(Number.isFinite)) unavailable = true;
      else if (values[0] + half + 0.005 > values[1] - half - 0.005) reason = "行程不足";
      else {
        center = Math.max(values[0] + half + 0.005, Math.min(0, values[1] - half - 0.005));
        if (kinds.some(function (kind) { return profiles[kind].speed > values[2] || profiles[kind].acceleration > values[3]; })) reason = "速度或加速度超限";
      }
      tr.querySelector(".friction-center").value = Number(center.toFixed(6));
      tr.querySelector(".friction-collision-travel").value = travel.collision;
      tr.querySelector(".friction-drag-travel").value = travel.drag;
      tr.querySelector(".friction-axis-use").checked = !selected && !reason;
      if (!reason && !selected) selected = true;
      if (reason) excluded.push(tr.dataset.joint + "：" + reason);
    });
    show("frictionReferenceState", "已按当前速度和质量阈值生成参考行程，默认仅勾选首个可用轴。完整方案以预览空间预算为准，上限 8 GiB；超限时可按关节或用途分批。" +
      (unavailable ? " 控制器未提供完整限位，请手动核对中心、行程及速度限制。" : " 中心已按关节限位调整，完整路径仍须预览。") +
      (excluded.length ? " 未默认勾选：" + excluded.join("；") + "。" : ""));
  }
  function renderParameters(p) {
    state.parameters = p; invalidatePlan();
    if (state.robots.some(function (robot) { return robot.name === p.name; })) state.dirtyRobotPlans.add(p.name);
    var cfg = p.configured || {}, col = cfg.collision || {}, drag = cfg.drag || {};
    var body = $("frictionAxes"); body.replaceChildren();
    (p.axisNames || []).forEach(function (name, i) {
      var tr = document.createElement("tr"); tr.dataset.joint = name;
      var use = document.createElement("input"); use.type = "checkbox"; use.checked = false; use.className = "friction-axis-use";
      cell(tr, use); cell(tr, name);
      cell(tr, input(0, "friction-center"));
      cell(tr, input(0.85, "friction-collision-travel")); cell(tr, input(0.4, "friction-drag-travel"));
      cell(tr, input((col.kc || [])[i], "friction-collision-kc")); cell(tr, input((col.kv || [])[i], "friction-collision-kv"));
      cell(tr, input((drag.kc || [])[i], "friction-drag-kc")); cell(tr, input((drag.kv || [])[i], "friction-drag-kv"));
      body.appendChild(tr);
    });
    referenceRows(); show("frictionParameterState", p);
    showQueued();
  }
  function readParameters() {
    var name = $("frictionRobot").value.trim(); if (!name) throw Error("请填写机器人名。");
    return api.call("friction/getParameters", { name: name }).then(renderParameters);
  }
  function robotPlan() {
    var p = state.parameters; if (!p || p.name !== $("frictionRobot").value.trim()) throw Error("请先读取当前机器人的参数与轴表。");
    var kinds = chosenKinds(); if (!kinds.length) throw Error("至少选择碰撞或拖动一套方案。");
    var r = { name: p.name, joints: [], kinds: kinds, plans: {}, support: [] };
    var rows = Array.from($("frictionAxes").rows).filter(function (tr) { return tr.querySelector(".friction-axis-use").checked; });
    if (!rows.length) throw Error("至少选择一个关节。");
    kinds.forEach(function (kind) {
      var plan = { speeds: speedsFor(kind), acceleration: number("friction" + kind + "Acceleration"), rounds: number("frictionRounds"), center: {}, halfTravel: {} };
      if (!Number.isInteger(plan.rounds) || plan.rounds < 2 || plan.rounds > 5 || plan.acceleration <= 0) throw Error("轮数须为 2～5 的整数，加速度须大于 0。");
      rows.forEach(function (tr) {
        var name = tr.dataset.joint, q = numericInput(tr.querySelector(".friction-center"), name + " 中心"), d = numericInput(tr.querySelector(".friction-" + kind + "-travel"), name + " 半行程");
        if (d <= 0) throw Error(name + " 的" + (kind === "drag" ? "拖动" : "碰撞 / 常规") + "半行程须大于 0；请填写正值，或取消勾选该关节。");
        plan.center[name] = q; plan.halfTravel[name] = d;
      });
      r.plans[kind] = plan;
    });
    r.joints = rows.map(function (tr) { return tr.dataset.joint; });
    if ($("frictionSupport").checked) r.support.push({ targetJoint: $("frictionSupportTarget").value.trim(), joint: $("frictionSupportJoint").value.trim(), position: number("frictionSupportPosition"), velocity: number("frictionSupportVelocity"), acceleration: number("frictionSupportAcceleration") });
    return r;
  }
  function queueRobot() {
    var r = robotPlan(); state.robots = state.robots.filter(function (x) { return x.name !== r.name; }); state.robots.push(r); state.dirtyRobotPlans.delete(r.name); invalidatePlan(); showQueued();
    show("frictionMessage", "已加入 / 更新 " + r.name + "（" + r.joints.length + " 个关节；" + r.kinds.map(function (kind) { return kind === "drag" ? "拖动" : "碰撞 / 常规"; }).join("、") + "）。请预览完整方案。");
  }
  function status() {
    var id = $("frictionSession").value.trim(); if (!id) throw Error("尚无会话标识。");
    return api.call("friction/status", { sessionId: id }).then(function (s) {
      setSession(id); show("frictionStatus", s);
      $("frictionProgress").value = Number(s.progress) || 0;
      if (["candidateReady", "cancelled", "failed"].indexOf(s.phase) >= 0) { clearInterval(state.timer); state.timer = null; }
      return s;
    });
  }
  function startPolling() {
    clearInterval(state.timer);
    state.timer = setInterval(function () {
      if (state.polling) return; state.polling = true;
      status().catch(error).finally(function () { state.polling = false; });
    }, 1000);
  }
  function result() {
    var id = $("frictionSession").value.trim(); if (!id) throw Error("尚无会话标识。");
    return api.call("friction/result", { sessionId: id }).then(function (r) {
      state.result = r; show("frictionResult", r);
      var sel = $("frictionCandidateRobot"); sel.replaceChildren();
      (r.robots || []).forEach(function (robot) { var opt = document.createElement("option"); opt.value = robot.name; opt.textContent = robot.name; sel.appendChild(opt); });
    });
  }
  $("frictionRead").onclick = function () { run(readParameters); };
  $("frictionLegacyPreset").onclick = function () { run(function () {
    ["collision", "drag"].forEach(function (kind) { $("friction" + kind + "Speeds").value = legacy[kind].speeds; $("friction" + kind + "Acceleration").value = legacy[kind].acceleration; });
    $("frictionRounds").value = 2; invalidatePlan();
    if (state.parameters && state.robots.some(function (r) { return r.name === state.parameters.name; })) state.dirtyRobotPlans.add(state.parameters.name);
    referenceRows(); showQueued(); show("frictionMessage", "已填入旧版完整十档速度与加速度，并按当前质量阈值重设参考行程；请核对关节、中心与支撑姿态，再更新并预览方案。");
  }); };
  $("frictionQueue").onclick = function () { run(queueRobot); };
  $("frictionClearPlan").onclick = function () { state.robots = []; state.dirtyRobotPlans.clear(); invalidatePlan(); showQueued(); };
  $("frictionPreview").onclick = function () { run(function () {
    if (state.dirtyRobotPlans.size) throw Error("已加入方案的 " + Array.from(state.dirtyRobotPlans).join("、") + " 有未更新的修改，请先重新加入方案。");
    invalidatePlan(); var revision = state.previewRevision, robots = state.robots.length ? state.robots : [robotPlan()];
    return api.call("friction/preview", { robots: robots, qualityPolicy: json("frictionQuality") }).then(function (p) {
      if (state.previewRevision !== revision) return;
      state.plan = p; show("frictionPlan", p); $("frictionStart").disabled = false;
    });
  }); };
  $("frictionStart").onclick = function () { run(function () {
    if (!state.plan) throw Error("请先预览方案。");
    if (!$("frictionReviewed").checked) throw Error("请核对完整路径并勾选现场确认。");
    state.startKey = state.startKey || (window.crypto && crypto.randomUUID ? crypto.randomUUID() : Date.now() + "-" + Math.random().toString(36).slice(2));
    return api.call("friction/start", { planId: state.plan.planId, planRevision: state.plan.planRevision, idempotencyKey: state.startKey }).then(function (s) {
      if (state.sessionId !== s.sessionId) {
        state.result = null; $("frictionCandidateRobot").replaceChildren(); show("frictionResult", "新会话尚未生成候选，请完成后查看结果与质量。");
      }
      setSession(s.sessionId); show("frictionStatus", s); startPolling();
    });
  }); };
  $("frictionPoll").onclick = function () { run(status); };
  $("frictionResultRead").onclick = function () { run(result); };
  $("frictionCancel").onclick = function () { run(function () {
    var id = $("frictionSession").value.trim(); if (!id) throw Error("尚无会话标识。");
    return api.call("friction/cancel", { sessionId: id }).then(function (s) { show("frictionStatus", s); startPolling(); });
  }); };
  $("frictionApply").onclick = function () { run(function () {
    var r = state.result; if (!r) throw Error("请先查看辨识结果。");
    var name = $("frictionCandidateRobot").value, robot = (r.robots || []).find(function (x) { return x.name === name; });
    var kinds = chosenKinds(), ids = {}; if (!robot || !kinds.length) throw Error("请选择机器人和用途。");
    kinds.forEach(function (kind) { var c = (robot.kinds || {})[kind]; if (!c || !c.applicable || !c.complete) throw Error(kind + " 候选未通过质量检查或不完整。"); ids[kind] = c.candidateId; });
    return api.call("friction/getParameters", { name: name }).then(function (p) {
      return api.call("friction/apply", { name: name, candidateGroupId: r.candidateGroupId, candidateIds: ids, kinds: kinds, expectedConfiguredVersion: p.configuredVersion });
    }).then(function (s) { show("frictionMessage", s); });
  }); };
  $("frictionSave").onclick = function () { run(function () {
    var p = state.parameters, kinds = chosenKinds(); if (!p || p.name !== $("frictionRobot").value.trim()) throw Error("请先读取参数。");
    if (!kinds.length) throw Error("至少选择一套参数。");
    var request = { name: p.name, expectedConfiguredVersion: p.configuredVersion };
    kinds.forEach(function (kind) {
      request[kind] = { kc: [], kv: [] };
      Array.from($("frictionAxes").rows).forEach(function (tr) { ["kc", "kv"].forEach(function (key) {
        var value = numericInput(tr.querySelector(".friction-" + kind + "-" + key), tr.dataset.joint + " " + kind + " " + key);
        if (value < 0) throw Error("Kc/Kv 须为非负有限数。"); request[kind][key].push(value);
      }); });
    });
    return api.call("friction/setParameters", request).then(function (s) { show("frictionMessage", s); return readParameters(); });
  }); };
  $("deviceFriction").addEventListener("input", function (e) {
    var id = e.target.id || "", element = e.target;
    var robotPlanChanged = /^(frictionUse|frictionSupport|frictioncollision|frictiondrag)/.test(id) || id === "frictionRounds" ||
      element.matches(".friction-axis-use,.friction-center,.friction-collision-travel,.friction-drag-travel");
    if (robotPlanChanged && state.parameters && state.robots.some(function (r) { return r.name === state.parameters.name; })) {
      state.dirtyRobotPlans.add(state.parameters.name); showQueued();
    }
    if (robotPlanChanged || id === "frictionRobot" || id === "frictionQuality") invalidatePlan();
  });
})();
