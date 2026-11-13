// Calor web UI. Talks to the C++ server's JSON API; every chart number
// comes from a segment-tree range query on the server.
"use strict";

// ---------- Dates (local calendar, ISO strings) ----------
const pad = (n) => String(n).padStart(2, "0");
const iso = (d) => `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
const fromIso = (s) => { const [y, m, d] = s.split("-").map(Number); return new Date(y, m - 1, d); };
const addDays = (s, n) => { const d = fromIso(s); d.setDate(d.getDate() + n); return iso(d); };
const today = () => iso(new Date());
const pretty = (s, opts = { weekday: "short", month: "short", day: "numeric" }) =>
  fromIso(s).toLocaleDateString(undefined, opts);

// ---------- State ----------
const state = {
  nutrients: [],
  day: today(),
  rangeDays: 30,
  chartNutrient: "calories",
  editingId: null,
  editingFoodId: null,
  pickedFood: null,
  charts: {},
};

const $ = (id) => document.getElementById(id);
const fmt = (v, digits = 0) =>
  v === null || v === undefined ? "–" : Number(v).toLocaleString(undefined, { maximumFractionDigits: digits });
const unitOf = (key) => (state.nutrients.find((n) => n.key === key) || {}).unit || "";
const labelOf = (key) => (state.nutrients.find((n) => n.key === key) || {}).label || key;

async function api(path, opts = {}) {
  const res = await fetch(path, opts);
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(body.error || `Request failed (${res.status})`);
  return body;
}
const post = (path, data, method = "POST") =>
  api(path, { method, body: new URLSearchParams(data) });

// ---------- Heat calendar ----------
async function loadHeat() {
  const end = today();
  // Start on the Sunday on or before the day 52 weeks ago, so columns are weeks.
  let start = addDays(end, -364);
  start = addDays(start, -fromIso(start).getDay());
  const days = await api(`/api/series?from=${start}&to=${end}&bucket=day`);

  const logged = days.filter((d) => d.stats.logged_days).map((d) => d.stats.sum.calories).sort((a, b) => a - b);
  const q = (p) => logged[Math.min(logged.length - 1, Math.floor(p * logged.length))];
  const cuts = logged.length ? [q(0.2), q(0.4), q(0.6), q(0.8)] : [];
  const level = (v) => 1 + cuts.filter((c) => v > c).length;

  const grid = $("heat-grid");
  grid.replaceChildren();
  for (const d of days) {
    const b = document.createElement("button");
    b.type = "button";
    b.dataset.date = d.start;
    const kcal = d.stats.logged_days ? d.stats.sum.calories : null;
    if (kcal !== null) b.className = `h${level(kcal)}`;
    const label = `${pretty(d.start, { weekday: "long", month: "long", day: "numeric", year: "numeric" })}: ${
      kcal === null ? "not logged" : `${fmt(kcal)} kcal`}`;
    b.title = label;
    b.setAttribute("aria-label", label);
    b.setAttribute("aria-selected", d.start === state.day ? "true" : "false");
    b.addEventListener("click", () => setDay(d.start));
    grid.append(b);
  }
  const sel = grid.querySelector('[aria-selected="true"]');
  if (sel) sel.scrollIntoView({ block: "nearest", inline: "nearest" });
}

function markHeatSelection() {
  for (const b of $("heat-grid").children) b.setAttribute("aria-selected", b.dataset.date === state.day ? "true" : "false");
}

// ---------- Day panel ----------
function setDay(d) {
  state.day = d;
  $("day-input").value = d;
  markHeatSelection();
  loadDay();
}

async function loadDay() {
  const d = state.day;
  $("day-title").textContent = d === today() ? "Today" : d === addDays(today(), -1) ? "Yesterday" : pretty(d);
  const entries = await api(`/api/entries?date=${d}`);
  const total = entries.reduce((s, e) => s + e.nutrients.calories, 0);
  const p = entries.reduce((s, e) => s + e.nutrients.protein_g, 0);
  $("day-total").innerHTML = entries.length
    ? `<strong>${fmt(total)}</strong> kcal across ${entries.length} ${entries.length === 1 ? "entry" : "entries"}, ${fmt(p)} g protein`
    : "";

  const box = $("entries");
  box.replaceChildren();
  if (!entries.length) {
    box.innerHTML = `<p class="empty">Nothing logged for this day. Add a food below.</p>`;
    return;
  }
  const order = ["breakfast", "lunch", "dinner", "snack"];
  const groups = new Map();
  for (const e of entries) {
    const m = e.meal || "other";
    if (!groups.has(m)) groups.set(m, []);
    groups.get(m).push(e);
  }
  const keys = [...groups.keys()].sort((a, b) => (order.indexOf(a) + 99) % 99 - (order.indexOf(b) + 99) % 99);
  for (const m of keys) {
    const sec = document.createElement("div");
    sec.className = "meal";
    const h = document.createElement("h3");
    h.textContent = m;
    sec.append(h);
    for (const e of groups.get(m)) sec.append(entryRow(e));
    box.append(sec);
  }
}

function entryRow(e) {
  const row = document.createElement("div");
  row.className = "entry";
  const what = document.createElement("div");
  what.className = "what";
  what.textContent = e.name;
  const small = document.createElement("small");
  const bits = [];
  if (e.food_id && e.servings !== 1) bits.push(`${fmt(e.servings, 2)} servings`);
  bits.push(`P ${fmt(e.nutrients.protein_g)} g, C ${fmt(e.nutrients.carbs_g)} g, F ${fmt(e.nutrients.fat_g)} g`);
  if (e.notes) bits.push(e.notes);
  small.textContent = bits.join(". ");
  what.append(small);

  const kcal = document.createElement("div");
  kcal.className = "kcal";
  kcal.textContent = `${fmt(e.nutrients.calories)} kcal`;

  const tools = document.createElement("div");
  tools.className = "tools";
  const edit = document.createElement("button");
  edit.type = "button";
  edit.textContent = "Edit";
  edit.setAttribute("aria-label", `Edit ${e.name}`);
  edit.addEventListener("click", () => startEdit(e));
  const del = document.createElement("button");
  del.type = "button";
  del.textContent = "Delete";
  del.setAttribute("aria-label", `Delete ${e.name}`);
  del.addEventListener("click", async () => {
    await api(`/api/entries/${e.id}`, { method: "DELETE" });
    message(`Deleted ${e.name}.`);
    refreshAfterWrite();
  });
  tools.append(edit, del);
  row.append(what, kcal, tools);
  return row;
}

// ---------- Form ----------
function buildNutrientFields() {
  const box = $("nutrient-fields");
  for (const n of state.nutrients) {
    const f = document.createElement("div");
    f.className = "field";
    f.innerHTML = `<label for="n-${n.key}">${n.label} (${n.unit})</label>
      <input id="n-${n.key}" name="${n.key}" type="number" min="0" step="any" inputmode="decimal">`;
    box.append(f);
  }
}

function message(text, isError = false) {
  const m = $("form-msg");
  m.textContent = text;
  m.className = isError ? "msg error" : "msg";
}

function resetForm() {
  state.editingId = null;
  state.editingFoodId = null;
  state.pickedFood = null;
  $("log-form").reset();
  $("servings").value = "1";
  $("form-title").textContent = "Add food";
  $("submit-btn").textContent = "Add to day";
  $("cancel-edit").hidden = true;
  $("food-search").disabled = false;
  $("save-food").parentElement.hidden = false;
  $("manual").open = false;
  $("values-hint").textContent = "Values are per serving.";
}

function startEdit(e) {
  resetForm();
  state.editingId = e.id;
  state.editingFoodId = e.food_id;
  $("form-title").textContent = `Edit ${e.name}`;
  $("submit-btn").textContent = "Save changes";
  $("cancel-edit").hidden = false;
  $("food-search").disabled = true;
  $("save-food").parentElement.hidden = true;
  $("manual").open = true;
  $("values-hint").textContent = "Values are totals for this entry; changing servings does not rescale them.";
  $("name").value = e.name;
  $("servings").value = e.servings;
  $("meal").value = e.meal || "snack";
  $("notes").value = e.notes;
  for (const n of state.nutrients) $(`n-${n.key}`).value = e.nutrients[n.key] || "";
  $("log-form").scrollIntoView({ behavior: "smooth", block: "start" });
  $("name").focus();
}

function manualValues() {
  const data = { name: $("name").value.trim() };
  for (const n of state.nutrients) {
    const v = $(`n-${n.key}`).value;
    if (v !== "") data[n.key] = v;
  }
  return data;
}

async function onSubmit(ev) {
  ev.preventDefault();
  const common = {
    date: state.day,
    servings: $("servings").value,
    meal: $("meal").value,
    notes: $("notes").value.trim(),
  };
  try {
    if (state.editingId) {
      const v = manualValues();
      if (!v.name) throw new Error("Give the entry a name.");
      const link = state.editingFoodId ? { food_id: state.editingFoodId } : {};
      await post(`/api/entries/${state.editingId}`, { ...common, ...v, ...link });
      message(`Saved changes to ${v.name}.`);
    } else if (state.pickedFood) {
      const e = await post("/api/entries", { ...common, food_id: state.pickedFood.id });
      message(`Added ${e.name}, ${fmt(e.nutrients.calories)} kcal.`);
    } else {
      const v = manualValues();
      if (!v.name) throw new Error("Pick a saved food, or open “Enter values by hand” and give it a name.");
      if ($("save-food").checked) {
        // Values typed are per serving; save the food, then log it.
        const food = await post("/api/foods", { ...v, serving: $("serving-desc").value.trim() });
        const e = await post("/api/entries", { ...common, food_id: food.id });
        message(`Saved ${food.name} to your foods and added ${fmt(e.nutrients.calories)} kcal.`);
      } else {
        // One-off entry: scale the typed per-serving values by servings.
        const s = Number(common.servings) || 1;
        const scaled = { ...v };
        for (const n of state.nutrients) if (scaled[n.key] !== undefined) scaled[n.key] = Number(scaled[n.key]) * s;
        const e = await post("/api/entries", { ...common, ...scaled });
        message(`Added ${e.name}, ${fmt(e.nutrients.calories)} kcal.`);
      }
    }
    resetForm();
    refreshAfterWrite();
  } catch (err) {
    message(err.message, true);
  }
}

// Food search: trie-backed prefix search on the server.
function setupSearch() {
  const input = $("food-search");
  const list = $("food-results");
  let timer = null, results = [], active = -1;

  const close = () => { list.hidden = true; active = -1; input.setAttribute("aria-expanded", "false"); };
  const pick = (f) => {
    state.pickedFood = f;
    input.value = f.name;
    close();
    message(`${f.name}: ${fmt(f.nutrients.calories)} kcal per ${f.serving || "serving"}.`);
  };
  const render = () => {
    list.replaceChildren();
    results.forEach((f, i) => {
      const li = document.createElement("li");
      li.setAttribute("role", "option");
      li.setAttribute("aria-selected", i === active ? "true" : "false");
      li.innerHTML = `<span></span><small></small>`;
      li.firstChild.textContent = f.name;
      li.lastChild.textContent = `${fmt(f.nutrients.calories)} kcal / ${f.serving || "serving"}`;
      li.addEventListener("mousedown", (e) => { e.preventDefault(); pick(f); });
      list.append(li);
    });
    if (!results.length) {
      const li = document.createElement("li");
      li.textContent = "No saved food matches. Enter values by hand below.";
      list.append(li);
    }
    list.hidden = false;
    input.setAttribute("aria-expanded", "true");
  };

  input.addEventListener("input", () => {
    state.pickedFood = null;
    clearTimeout(timer);
    const q = input.value.trim();
    if (!q) return close();
    timer = setTimeout(async () => {
      results = await api(`/api/foods?q=${encodeURIComponent(q)}&limit=8`);
      active = -1;
      render();
    }, 120);
  });
  input.addEventListener("keydown", (e) => {
    if (list.hidden) return;
    if (e.key === "ArrowDown") { active = Math.min(results.length - 1, active + 1); render(); e.preventDefault(); }
    else if (e.key === "ArrowUp") { active = Math.max(0, active - 1); render(); e.preventDefault(); }
    else if (e.key === "Enter" && active >= 0) { pick(results[active]); e.preventDefault(); }
    else if (e.key === "Escape") close();
  });
  input.addEventListener("blur", () => setTimeout(close, 100));
}

// ---------- Range panel ----------
function rangeBounds() {
  const to = today();
  return { from: addDays(to, -(state.rangeDays - 1)), to };
}

async function loadRange() {
  const { from, to } = rangeBounds();
  const [stats, daily, rolling, weekly] = await Promise.all([
    api(`/api/stats?from=${from}&to=${to}`),
    api(`/api/series?from=${from}&to=${to}&bucket=day`),
    api(`/api/rolling?from=${from}&to=${to}&window=7&nutrient=${state.chartNutrient}`),
    api(`/api/series?from=${from}&to=${to}&bucket=${state.rangeDays > 120 ? "month" : "week"}`),
  ]);
  renderFacts(stats);
  renderDaily(daily, rolling);
  renderMacros(weekly);
}

function renderFacts(s) {
  const st = s.stats;
  const avg = st.avg, n = st.logged_days;
  const row = (label, value, cls = "") => `<dt class="${cls}">${label}</dt><dd class="${cls}">${value}</dd>`;
  const macroKcal = avg.calories ? (g, per) => ` <small>${fmt((g * per / avg.calories) * 100)}% of kcal</small>` : () => "";
  const day = (key, which) =>
    s[which][key] ? `${fmt(st[which === "max_day" ? "max" : "min"][key])} kcal <small>${pretty(s[which][key])}</small>` : "–";
  $("facts").innerHTML = `
    <div class="big"><span>Average per logged day</span><strong>${n ? `${fmt(avg.calories)}<small>kcal</small>` : "–"}</strong></div>
    <dl>
      ${row("Days logged", `${n} <small>of ${s.days}</small>`)}
      ${row("Total calories", `${fmt(st.sum.calories)} kcal`)}
      ${row("Highest day", day("calories", "max_day"))}
      ${row("Lowest day", day("calories", "min_day"), "thick")}
      ${row("Protein", n ? `${fmt(avg.protein_g)} g${macroKcal(avg.protein_g, 4)}` : "–")}
      ${row("Carbohydrates", n ? `${fmt(avg.carbs_g)} g${macroKcal(avg.carbs_g, 4)}` : "–")}
      ${row('<span class="sub">Fiber</span>', n ? `${fmt(avg.fiber_g)} g` : "–")}
      ${row('<span class="sub">Sugar</span>', n ? `${fmt(avg.sugar_g)} g` : "–")}
      ${row("Fat", n ? `${fmt(avg.fat_g)} g${macroKcal(avg.fat_g, 9)}` : "–")}
      ${row('<span class="sub">Saturated fat</span>', n ? `${fmt(avg.sat_fat_g)} g` : "–")}
      ${row("Sodium", n ? `${fmt(avg.sodium_mg)} mg` : "–")}
      ${row("Water", n ? `${fmt(avg.water_ml)} ml` : "–")}
    </dl>
    <p class="empty" style="margin:.5rem 0 0;font-size:.85rem">Averages count only days with at least one entry.</p>`;
}

function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function baseOptions(yTitle) {
  const ink = cssVar("--muted"), grid = cssVar("--hair");
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: window.matchMedia("(prefers-reduced-motion: reduce)").matches ? false : { duration: 250 },
    interaction: { mode: "index", intersect: false },
    scales: {
      x: { ticks: { color: ink, maxRotation: 0, autoSkip: true, maxTicksLimit: 8 }, grid: { display: false } },
      y: { beginAtZero: true, ticks: { color: ink }, grid: { color: grid }, title: { display: true, text: yTitle, color: ink } },
    },
    plugins: { legend: { labels: { color: ink, boxWidth: 12 } } },
  };
}

function draw(id, config) {
  if (state.charts[id]) state.charts[id].destroy();
  state.charts[id] = new Chart($(id), config);
}

function renderDaily(daily, rolling) {
  const k = state.chartNutrient;
  const short = (s) => pretty(s, { month: "short", day: "numeric" });
  draw("daily-chart", {
    type: "bar",
    data: {
      labels: daily.map((d) => short(d.start)),
      datasets: [
        {
          type: "bar",
          label: `${labelOf(k)} per day`,
          data: daily.map((d) => (d.stats.logged_days ? d.stats.sum[k] : null)),
          backgroundColor: cssVar("--h2"),
          order: 2,
        },
        {
          type: "line",
          label: "7-day average",
          data: rolling.map((r) => r.avg),
          borderColor: cssVar("--h4"),
          backgroundColor: cssVar("--h4"),
          borderWidth: 2,
          pointRadius: 0,
          spanGaps: true,
          order: 1,
        },
      ],
    },
    options: baseOptions(unitOf(k)),
  });
}

function renderMacros(buckets) {
  const label = (b) => pretty(b.start, state.rangeDays > 120 ? { month: "short", year: "2-digit" } : { month: "short", day: "numeric" });
  const avg = (b, key, per) => (b.stats.logged_days ? b.stats.avg[key] * per : null);
  const opts = baseOptions("kcal per logged day");
  opts.scales.x.stacked = true;
  opts.scales.y.stacked = true;
  draw("macro-chart", {
    type: "bar",
    data: {
      labels: buckets.map(label),
      datasets: [
        { label: "Protein", data: buckets.map((b) => avg(b, "protein_g", 4)), backgroundColor: cssVar("--protein") },
        { label: "Carbohydrates", data: buckets.map((b) => avg(b, "carbs_g", 4)), backgroundColor: cssVar("--h2") },
        { label: "Fat", data: buckets.map((b) => avg(b, "fat_g", 9)), backgroundColor: cssVar("--h4") },
      ],
    },
    options: opts,
  });
}

async function loadEngineInfo() {
  const m = await api("/api/meta");
  $("engine-info").textContent =
    `The segment tree indexes ${fmt(m.tree.days)} days from ${pretty(m.tree.first_day, { month: "short", day: "numeric", year: "numeric" })} in ${fmt(m.tree.memory_bytes / 1024)} KB. ${m.foods} saved ${m.foods === 1 ? "food" : "foods"}.`;
  return m;
}

function refreshAfterWrite() {
  loadDay();
  loadHeat();
  loadRange();
  loadEngineInfo();
}

// ---------- Boot ----------
async function boot() {
  const meta = await loadEngineInfo();
  state.nutrients = meta.nutrients;
  buildNutrientFields();

  const pick = $("nutrient-pick");
  for (const n of state.nutrients) pick.add(new Option(`${n.label} (${n.unit})`, n.key));
  pick.addEventListener("change", () => { state.chartNutrient = pick.value; loadRange(); });

  for (const b of document.querySelectorAll(".chips button")) {
    b.addEventListener("click", () => {
      state.rangeDays = Number(b.dataset.days);
      for (const o of document.querySelectorAll(".chips button")) o.setAttribute("aria-pressed", o === b ? "true" : "false");
      loadRange();
    });
  }

  $("day-input").value = state.day;
  $("day-input").addEventListener("change", (e) => e.target.value && setDay(e.target.value));
  $("prev-day").addEventListener("click", () => setDay(addDays(state.day, -1)));
  $("next-day").addEventListener("click", () => setDay(addDays(state.day, 1)));
  $("log-form").addEventListener("submit", onSubmit);
  $("cancel-edit").addEventListener("click", () => { resetForm(); message(""); });
  setupSearch();

  await Promise.all([loadHeat(), loadDay(), loadRange()]);
}

boot().catch((err) => {
  document.querySelector("main").insertAdjacentHTML(
    "afterbegin",
    `<p class="msg error">Could not reach the Calor server: ${err.message}. Check that it is running, then reload.</p>`,
  );
});
