const $ = (selector) => document.querySelector(selector);

const elements = {
  deviceStatus: $("#deviceStatus"),
  serverStatus: $("#serverStatus"),
  statStudents: $("#statStudents"),
  statPoints: $("#statPoints"),
  statBottles: $("#statBottles"),
  statWeight: $("#statWeight"),
  studentMetricCards: document.querySelectorAll(".query-stat"),
  lookupForm: $("#lookupForm"),
  lookupUid: $("#lookupUid"),
  lookupResult: $("#lookupResult"),
  autoReadStatus: $("#autoReadStatus"),
  studentSearch: $("#studentSearch"),
  studentTableBody: $("#studentTableBody"),
  studentEmpty: $("#studentEmpty"),
  cardReadList: $("#cardReadList"),
  transactionList: $("#transactionList"),
  addStudentButton: $("#addStudentButton"),
  studentDialog: $("#studentDialog"),
  studentForm: $("#studentForm"),
  studentDialogTitle: $("#studentDialogTitle"),
  studentMode: $("#studentMode"),
  studentCardUid: $("#studentCardUid"),
  studentName: $("#studentName"),
  studentFormMessage: $("#studentFormMessage"),
  saveStudentButton: $("#saveStudentButton"),
  deleteStudentButton: $("#deleteStudentButton"),
  closeStudentDialog: $("#closeStudentDialog"),
  cancelStudentButton: $("#cancelStudentButton"),
  settlementDialog: $("#settlementDialog"),
  settlementForm: $("#settlementForm"),
  settlementCardUid: $("#settlementCardUid"),
  settlementAvatar: $("#settlementAvatar"),
  settlementName: $("#settlementName"),
  settlementCurrentPoints: $("#settlementCurrentPoints"),
  settlementPoints: $("#settlementPoints"),
  settlementFormMessage: $("#settlementFormMessage"),
  saveSettlementButton: $("#saveSettlementButton"),
  closeSettlementDialog: $("#closeSettlementDialog"),
  cancelSettlementButton: $("#cancelSettlementButton"),
  toastRegion: $("#toastRegion"),
};

const numberFormatter = new Intl.NumberFormat("zh-CN");
let searchTimer = null;
let editingStudent = null;
let settlementEventId = "";
let lastCardReadId = 0;
let cardPollInitialized = false;
let pollingCardReads = false;
let pollingDeviceStatus = false;

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function formatNumber(value) {
  return numberFormatter.format(Number(value || 0));
}

function formatDate(value) {
  if (!value) return "--";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleString("zh-CN", {
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  });
}

function initial(name) {
  const text = String(name || "?").trim();
  return text ? Array.from(text)[0].toUpperCase() : "?";
}

function normalizeUid(value) {
  return String(value || "").trim().toUpperCase();
}

function newEventId() {
  if (crypto.randomUUID) return `WEB-${crypto.randomUUID()}`;
  return `WEB-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: {
      "Content-Type": "application/json",
      ...(options.headers || {}),
    },
  });
  let payload = {};
  try {
    payload = await response.json();
  } catch {
    payload = {};
  }
  if (!response.ok) {
    const error = new Error(payload.error || `请求失败（HTTP ${response.status}）`);
    error.status = response.status;
    throw error;
  }
  return payload;
}

function setServerState(mode) {
  elements.serverStatus.classList.remove("is-online", "is-offline");
  if (mode === "online") {
    elements.serverStatus.classList.add("is-online");
    elements.serverStatus.lastElementChild.textContent = "服务器在线";
  } else {
    elements.serverStatus.classList.add("is-offline");
    elements.serverStatus.lastElementChild.textContent = "服务器未连接";
  }
}

function setDeviceState(payload) {
  elements.deviceStatus.classList.remove("is-online", "is-offline");
  elements.deviceStatus.classList.add(payload.online ? "is-online" : "is-offline");
  elements.deviceStatus.lastElementChild.textContent = payload.online
    ? "设备在线"
    : "设备离线";
  const lastSeen = payload.last_message_at || payload.last_connected_at;
  elements.deviceStatus.title = lastSeen
    ? `最后通信：${formatDate(lastSeen)}`
    : "设备尚未连接服务器";
}

async function refreshDeviceStatus() {
  if (pollingDeviceStatus) return;
  pollingDeviceStatus = true;
  try {
    const payload = await api("/api/device-status");
    setDeviceState(payload);
  } catch {
    setDeviceState({ online: false });
  } finally {
    pollingDeviceStatus = false;
  }
}

function setAutoReadStatus(mode, text) {
  elements.autoReadStatus.classList.remove("is-listening", "is-read", "is-error");
  if (mode) elements.autoReadStatus.classList.add(`is-${mode}`);
  elements.autoReadStatus.lastElementChild.textContent = text;
}

function anyDialogOpen() {
  return elements.studentDialog.open || elements.settlementDialog.open;
}

function toast(message, kind = "success") {
  const node = document.createElement("div");
  node.className = `toast${kind === "error" ? " is-error" : kind === "warning" ? " is-warning" : ""}`;
  node.textContent = message;
  elements.toastRegion.append(node);
  window.setTimeout(() => node.remove(), 3600);
}

function renderSummary(summary) {
  elements.statStudents.textContent = formatNumber(summary.student_count);
}

function setStudentMetricsVisible(visible) {
  for (const card of elements.studentMetricCards) {
    card.hidden = !visible;
  }
}

function renderStudentMetrics(student) {
  elements.statPoints.textContent = formatNumber(student.total_points);
  elements.statBottles.textContent = formatNumber(student.total_bottles);
  elements.statWeight.textContent = Number(student.total_weight_kg || 0).toFixed(2);
  setStudentMetricsVisible(true);
}

function createCell(className = "") {
  const cell = document.createElement("td");
  if (className) cell.className = className;
  return cell;
}

function createRowButton(label, className, onClick) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `row-button ${className}`;
  button.textContent = label;
  button.addEventListener("click", onClick);
  return button;
}

function renderStudents(students) {
  elements.studentTableBody.replaceChildren();
  elements.studentEmpty.hidden = students.length > 0;

  for (const student of students) {
    const row = document.createElement("tr");

    const studentCell = createCell();
    const studentWrap = document.createElement("div");
    studentWrap.className = "student-cell";
    const avatar = document.createElement("span");
    avatar.className = "student-avatar";
    avatar.textContent = initial(student.name);
    const nameWrap = document.createElement("div");
    const name = document.createElement("strong");
    name.textContent = student.name;
    const subtitle = document.createElement("small");
    subtitle.textContent = `${formatNumber(student.transaction_count)} 次结算`;
    nameWrap.append(name, subtitle);
    studentWrap.append(avatar, nameWrap);
    studentCell.append(studentWrap);

    const cardCell = createCell();
    const card = document.createElement("span");
    card.className = "card-code";
    card.textContent = student.card_uid;
    card.title = student.card_uid;
    cardCell.append(card);

    const pointsCell = createCell("align-right");
    const points = document.createElement("span");
    points.className = "points-value";
    points.textContent = formatNumber(student.total_points);
    pointsCell.append(points);

    const actionCell = createCell("align-right");
    const actions = document.createElement("div");
    actions.className = "row-actions";
    actions.append(
      createRowButton("编辑", "edit-button", () => openEditStudent(student)),
      createRowButton("结算积分", "row-button-primary", () => openSettlement(student)),
    );
    actionCell.append(actions);

    row.append(studentCell, cardCell, pointsCell, actionCell);
    elements.studentTableBody.append(row);
  }
}

function renderCardReads(reads) {
  elements.cardReadList.replaceChildren();
  if (!reads.length) {
    const empty = document.createElement("p");
    empty.className = "muted-message";
    empty.textContent = "等待设备上传刷卡记录…";
    elements.cardReadList.append(empty);
    return;
  }
  for (const read of reads) {
    const item = document.createElement("div");
    item.className = "activity-item";
    const avatar = document.createElement("span");
    avatar.className = `activity-avatar${read.registered ? "" : " is-unknown"}`;
    avatar.textContent = initial(read.registered ? read.student_name : "?");
    const body = document.createElement("div");
    body.className = "activity-body";
    const name = document.createElement("strong");
    name.textContent = read.registered ? read.student_name : "未登记学生";
    const uid = document.createElement("small");
    uid.textContent = read.card_uid;
    uid.title = read.card_uid;
    body.append(name, uid);
    const meta = document.createElement("div");
    meta.className = "activity-meta";
    const points = document.createElement("strong");
    points.textContent = formatNumber(read.total_points);
    const time = document.createElement("small");
    time.textContent = formatDate(read.seen_at);
    meta.append(points, time);
    item.append(avatar, body, meta);
    elements.cardReadList.append(item);
  }
}

function renderTransactions(transactions) {
  elements.transactionList.replaceChildren();
  if (!transactions.length) {
    const empty = document.createElement("p");
    empty.className = "muted-message";
    empty.textContent = "还没有积分流水。";
    elements.transactionList.append(empty);
    return;
  }
  for (const transaction of transactions) {
    const item = document.createElement("div");
    item.className = "activity-item";
    const avatar = document.createElement("span");
    avatar.className = "activity-avatar";
    avatar.textContent = initial(transaction.student_name);
    const body = document.createElement("div");
    body.className = "activity-body";
    const name = document.createElement("strong");
    name.textContent = transaction.student_name;
    const detail = document.createElement("small");
    detail.textContent = `${transaction.bottles} 个瓶子 · ${Number(transaction.weight_kg || 0).toFixed(2)} kg`;
    body.append(name, detail);
    const meta = document.createElement("div");
    meta.className = "activity-meta";
    const points = document.createElement("strong");
    points.textContent = `+${formatNumber(transaction.points)}`;
    const time = document.createElement("small");
    time.textContent = formatDate(transaction.received_at);
    meta.append(points, time);
    item.append(avatar, body, meta);
    elements.transactionList.append(item);
  }
}

function renderLookupReady() {
  elements.lookupResult.className = "lookup-result is-empty";
  elements.lookupResult.innerHTML = `
    <span class="result-icon">⌁</span>
    <div>
      <strong>等待刷卡或手动查询</strong>
      <p>把学生卡放到 NFC 读卡器上，姓名和积分会自动显示。</p>
    </div>
  `;
}

function renderLookupStudent(student) {
  elements.lookupResult.className = "lookup-result is-found";
  elements.lookupResult.innerHTML = "";

  const icon = document.createElement("span");
  icon.className = "result-icon";
  icon.textContent = initial(student.name);

  const main = document.createElement("div");
  main.className = "result-main";
  const title = document.createElement("strong");
  title.textContent = student.name;
  const cardLine = document.createElement("p");
  cardLine.className = "result-card-line";
  cardLine.textContent = student.card_uid;
  main.append(title, cardLine);

  const points = document.createElement("div");
  points.className = "result-points";
  const value = document.createElement("b");
  value.textContent = formatNumber(student.total_points);
  const label = document.createElement("span");
  label.textContent = "当前积分";
  points.append(value, label);

  const actions = document.createElement("div");
  actions.className = "result-actions";
  const edit = document.createElement("button");
  edit.type = "button";
  edit.className = "button button-secondary";
  edit.textContent = "编辑";
  edit.addEventListener("click", () => openEditStudent(student));
  const settle = document.createElement("button");
  settle.type = "button";
  settle.className = "button button-primary";
  settle.textContent = "结算积分";
  settle.addEventListener("click", () => openSettlement(student));
  actions.append(edit, settle);

  elements.lookupResult.append(icon, main, points, actions);
}

function renderLookupMissing(cardUid) {
  elements.lookupResult.className = "lookup-result is-missing";
  elements.lookupResult.innerHTML = "";

  const icon = document.createElement("span");
  icon.className = "result-icon";
  icon.textContent = "!";
  const main = document.createElement("div");
  main.className = "result-main";
  const title = document.createElement("strong");
  title.textContent = "未找到这张学生卡";
  const detail = document.createElement("p");
  detail.className = "result-card-line";
  detail.textContent = cardUid;
  main.append(title, detail);
  const register = document.createElement("button");
  register.type = "button";
  register.className = "button button-primary";
  register.textContent = "登记这张卡";
  register.addEventListener("click", () => openCreateStudent(cardUid));
  elements.lookupResult.append(icon, main, register);
}

async function lookupCard(uid) {
  const cardUid = normalizeUid(uid);
  if (!cardUid) {
    toast("请先输入学生卡号", "warning");
    elements.lookupUid.focus();
    return;
  }
  elements.lookupUid.value = cardUid;
  try {
    const student = await api(`/api/students/card?card_uid=${encodeURIComponent(cardUid)}`);
    renderLookupStudent(student);
    renderStudentMetrics(student);
    setServerState("online");
    return student;
  } catch (error) {
    if (error.status === 404) {
      setStudentMetricsVisible(false);
      renderLookupMissing(cardUid);
      return null;
    }
    setServerState("offline");
    toast(error.message, "error");
    return undefined;
  }
}

async function loadStudents() {
  const query = elements.studentSearch.value.trim();
  const payload = await api(`/api/students?limit=200&q=${encodeURIComponent(query)}`);
  renderStudents(payload.students || []);
}

async function refreshDashboard({ silent = false } = {}) {
  try {
    const [summary, students, transactions] = await Promise.all([
      api("/api/summary"),
      api(`/api/students?limit=200&q=${encodeURIComponent(elements.studentSearch.value.trim())}`),
      api("/api/transactions?limit=6"),
    ]);
    renderSummary(summary);
    renderStudents(students.students || []);
    renderTransactions(transactions.transactions || []);
    setServerState("online");
  } catch (error) {
    setServerState("offline");
    if (!silent) toast(error.message, "error");
  }
}

async function handleAutoCardRead(read) {
  const cardUid = normalizeUid(read.card_uid);
  const readTime = new Date(read.seen_at).toLocaleTimeString("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
  setAutoReadStatus("read", `已读取 ${readTime}`);

  const student = await lookupCard(cardUid);
  if (student === undefined) return;

  if (student?.registered) {
    toast(`已识别 ${student.name}，当前积分 ${formatNumber(student.total_points)}`);
    return;
  }

  if (anyDialogOpen()) {
    toast("读取到未登记卡；请先完成或关闭当前窗口，再补录姓名", "warning");
    return;
  }

  if (student) {
    openEditStudent({ ...student, name: "" });
  } else {
    openCreateStudent(cardUid);
  }
  toast("读取到未登记卡，请填写学生姓名", "warning");
  await refreshDashboard({ silent: true });
}

async function refreshCardReads({ initial = false, silent = true } = {}) {
  if (pollingCardReads) return;
  pollingCardReads = true;
  try {
    const payload = await api("/api/card-reads?limit=8");
    const reads = payload.reads || [];
    renderCardReads(reads);
    const latest = reads[0];
    const latestId = Number(latest?.id || 0);

    if (!cardPollInitialized) {
      cardPollInitialized = true;
      lastCardReadId = latestId;
      if (latest) {
        const previousReadTime = new Date(latest.seen_at).toLocaleTimeString("zh-CN", {
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit",
        });
        setAutoReadStatus("read", `最近读取 ${previousReadTime}`);
        await lookupCard(latest.card_uid);
      } else {
        setAutoReadStatus("listening", "正在监听刷卡");
      }
      return;
    }

    if (latest && latestId > lastCardReadId) {
      lastCardReadId = latestId;
      await handleAutoCardRead(latest);
    }
  } catch (error) {
    setAutoReadStatus("error", "读卡服务未连接");
    setServerState("offline");
    if (!silent || initial) toast(error.message, "error");
  } finally {
    pollingCardReads = false;
  }
}

function resetStudentForm() {
  elements.studentForm.reset();
  elements.studentMode.value = "create";
  elements.studentCardUid.disabled = false;
  elements.studentFormMessage.textContent = "";
  elements.deleteStudentButton.hidden = true;
  elements.saveStudentButton.disabled = false;
  editingStudent = null;
}

function openCreateStudent(prefillUid = "") {
  resetStudentForm();
  elements.studentDialogTitle.textContent = "登记学生卡";
  elements.studentMode.value = "create";
  elements.studentCardUid.value = normalizeUid(prefillUid);
  elements.studentDialog.showModal();
  window.setTimeout(() => {
    (prefillUid ? elements.studentName : elements.studentCardUid).focus();
  }, 20);
}

function openEditStudent(student) {
  resetStudentForm();
  editingStudent = student;
  elements.studentDialogTitle.textContent = "编辑学生档案";
  elements.studentMode.value = "edit";
  elements.studentCardUid.value = student.card_uid;
  elements.studentCardUid.disabled = true;
  elements.studentName.value = student.name === "未登记学生" ? "" : student.name;
  elements.deleteStudentButton.hidden = Number(student.transaction_count) > 0;
  elements.studentDialog.showModal();
  window.setTimeout(() => elements.studentName.focus(), 20);
}

function closeStudentDialog() {
  elements.studentDialog.close();
  resetStudentForm();
}

async function saveStudent(event) {
  event.preventDefault();
  elements.studentFormMessage.textContent = "";
  const cardUid = normalizeUid(elements.studentCardUid.value);
  const name = elements.studentName.value.trim();
  if (!cardUid || !name) {
    elements.studentFormMessage.textContent = "卡号和姓名都必须填写。";
    return;
  }

  const mode = elements.studentMode.value;
  elements.saveStudentButton.disabled = true;
  try {
    await api("/api/students", {
      method: mode === "edit" ? "PUT" : "POST",
      body: JSON.stringify({ card_uid: cardUid, name }),
    });
    closeStudentDialog();
    toast(mode === "edit" ? "学生姓名已更新" : "学生卡登记成功");
    await refreshDashboard({ silent: true });
    await lookupCard(cardUid);
  } catch (error) {
    elements.studentFormMessage.textContent = error.message;
  } finally {
    elements.saveStudentButton.disabled = false;
  }
}

async function deleteStudent() {
  if (!editingStudent) return;
  const confirmed = window.confirm(
    `确定删除 ${editingStudent.name}（${editingStudent.card_uid}）吗？此操作不能撤销。`,
  );
  if (!confirmed) return;
  elements.deleteStudentButton.disabled = true;
  try {
    await api(`/api/students?card_uid=${encodeURIComponent(editingStudent.card_uid)}`, {
      method: "DELETE",
    });
    closeStudentDialog();
    renderLookupReady();
    toast("学生档案已删除");
    await refreshDashboard({ silent: true });
  } catch (error) {
    elements.studentFormMessage.textContent = error.message;
  } finally {
    elements.deleteStudentButton.disabled = false;
  }
}

function openSettlement(student) {
  settlementEventId = newEventId();
  elements.settlementCardUid.value = student.card_uid;
  elements.settlementAvatar.textContent = initial(student.name);
  elements.settlementName.textContent = student.name;
  elements.settlementCurrentPoints.textContent = formatNumber(student.total_points);
  elements.settlementPoints.value = "";
  elements.settlementFormMessage.textContent = "";
  elements.saveSettlementButton.disabled = false;
  elements.settlementDialog.showModal();
  window.setTimeout(() => elements.settlementPoints.focus(), 20);
}

function closeSettlementDialog() {
  elements.settlementDialog.close();
  elements.settlementPoints.value = "";
  elements.settlementFormMessage.textContent = "";
}

async function saveSettlement(event) {
  event.preventDefault();
  elements.settlementFormMessage.textContent = "";
  const points = Number(elements.settlementPoints.value);
  if (!Number.isInteger(points) || points <= 0) {
    elements.settlementFormMessage.textContent = "本次积分必须是大于 0 的整数。";
    return;
  }
  elements.saveSettlementButton.disabled = true;
  try {
    const result = await api("/api/settlements", {
      method: "POST",
      body: JSON.stringify({
        card_uid: elements.settlementCardUid.value,
        event_id: settlementEventId,
        device_id: "WEB_ADMIN",
        points,
        bottles: 0,
        weight_kg: 0,
      }),
    });
    closeSettlementDialog();
    toast(`已入账 ${formatNumber(points)} 积分，当前 ${formatNumber(result.student.total_points)} 分`);
    await refreshDashboard({ silent: true });
    await lookupCard(result.student.card_uid);
  } catch (error) {
    elements.settlementFormMessage.textContent = error.message;
  } finally {
    elements.saveSettlementButton.disabled = false;
  }
}

elements.lookupForm.addEventListener("submit", (event) => {
  event.preventDefault();
  lookupCard(elements.lookupUid.value);
});

elements.studentSearch.addEventListener("input", () => {
  window.clearTimeout(searchTimer);
  searchTimer = window.setTimeout(async () => {
    try {
      await loadStudents();
    } catch (error) {
      toast(error.message, "error");
    }
  }, 220);
});

elements.addStudentButton.addEventListener("click", () => openCreateStudent());
elements.studentForm.addEventListener("submit", saveStudent);
elements.closeStudentDialog.addEventListener("click", closeStudentDialog);
elements.cancelStudentButton.addEventListener("click", closeStudentDialog);
elements.deleteStudentButton.addEventListener("click", deleteStudent);
elements.settlementForm.addEventListener("submit", saveSettlement);
elements.closeSettlementDialog.addEventListener("click", closeSettlementDialog);
elements.cancelSettlementButton.addEventListener("click", closeSettlementDialog);

for (const dialog of [elements.studentDialog, elements.settlementDialog]) {
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) dialog.close();
  });
}

elements.studentDialog.addEventListener("close", resetStudentForm);
elements.settlementDialog.addEventListener("close", () => {
  elements.settlementPoints.value = "";
  elements.settlementFormMessage.textContent = "";
});

document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible") {
    refreshDashboard({ silent: true });
    refreshCardReads();
    refreshDeviceStatus();
  }
});

refreshDashboard();
refreshCardReads({ initial: true });
refreshDeviceStatus();
window.setInterval(() => {
  if (document.visibilityState === "visible") refreshDashboard({ silent: true });
}, 10000);
window.setInterval(() => {
  if (document.visibilityState === "visible") refreshCardReads();
}, 1000);
window.setInterval(() => {
  if (document.visibilityState === "visible") refreshDeviceStatus();
}, 5000);
