#pragma once

static const char kIndexHtml[] = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover" />
<title>Newo GOOUUU Vision Bench</title>
<style>
  :root { color-scheme: dark; font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
  body { margin:0; background:#090909; color:#f1f1f1; }
  main { max-width:980px; margin:auto; padding:16px; }
  h1 { font-size:18px; margin:0 0 12px; font-weight:650; }
  .sub { opacity:.68; font-size:12px; margin-bottom:14px; line-height:1.45; }
  .viewer { position:relative; width:100%; background:#000; border:1px solid #2a2a2a; border-radius:12px; overflow:hidden; min-height:180px; }
  #stream { width:100%; display:block; }
  #overlay { position:absolute; inset:0; width:100%; height:100%; pointer-events:none; }
  .toolbar { display:flex; flex-wrap:wrap; gap:8px; margin:12px 0; align-items:center; }
  .label { font-size:11px; opacity:.65; margin-right:2px; }
  button { background:#151515; color:#eee; border:1px solid #333; padding:9px 12px; border-radius:8px; font:inherit; cursor:pointer; }
  button.active { border-color:#f1f1f1; background:#242424; }
  .grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:8px; }
  .card { border:1px solid #2a2a2a; border-radius:10px; padding:10px; background:#111; }
  .k { font-size:10px; opacity:.58; text-transform:uppercase; letter-spacing:.08em; }
  .v { font-size:17px; margin-top:3px; }
  .small { font-size:12px; }
  #benchNotice { display:none; padding:44px 12px; text-align:center; opacity:.7; }
  #modeHint { font-size:11px; opacity:.62; margin-top:-4px; margin-bottom:8px; }
</style>
</head>
<body>
<main>
  <h1>Newo / GOOUUU vision bench v3</h1>
  <div class="sub">Direct RGB565 detector path. FAST uses one QVGA pass. RANGE captures VGA and scans four overlapping crops so distant faces occupy more detector pixels.</div>

  <div class="viewer" id="viewer">
    <img id="stream" alt="camera stream" />
    <canvas id="overlay"></canvas>
    <div id="benchNotice">BENCH mode: video is stopped. QVGA RGB565 goes straight to the detector with no JPEG streaming cost.</div>
  </div>

  <div class="toolbar">
    <button data-mode="view" onclick="setMode('view')">VIEW</button>
    <button data-mode="fast" onclick="setMode('fast')">FAST</button>
    <button data-mode="range" onclick="setMode('range')">RANGE</button>
    <button data-mode="bench" onclick="setMode('bench')">BENCH</button>
  </div>
  <div id="modeHint">FAST = 320×240 / 1 detector pass · RANGE = 640×480 / 4 overlapping crop passes · VIEW = 640×480 / no AI</div>

  <div class="toolbar">
    <span class="label">Sensitivity</span>
    <button data-thr="0.50" onclick="setSensitivity(0.50)">0.50 strict</button>
    <button data-thr="0.40" onclick="setSensitivity(0.40)">0.40</button>
    <button data-thr="0.30" onclick="setSensitivity(0.30)">0.30</button>
    <button data-thr="0.20" onclick="setSensitivity(0.20)">0.20 balanced</button>
    <button data-thr="0.10" onclick="setSensitivity(0.10)">0.10 aggressive</button>
  </div>

  <div class="toolbar">
    <button onclick="sensorToggle('hmirror')">Mirror</button>
    <button onclick="sensorToggle('vflip')">Flip</button>
    <button onclick="resetStats()">Reset stats</button>
  </div>

  <div class="grid">
    <div class="card"><div class="k">Mode</div><div class="v" id="mode">-</div></div>
    <div class="card"><div class="k">Threshold</div><div class="v" id="threshold">-</div></div>
    <div class="card"><div class="k">Input</div><div class="v small" id="input">-</div></div>
    <div class="card"><div class="k">Resolution</div><div class="v" id="resolution">-</div></div>
    <div class="card"><div class="k">Detector passes</div><div class="v" id="passes">-</div></div>
    <div class="card"><div class="k">Faces</div><div class="v" id="faces">-</div></div>
    <div class="card"><div class="k">Detection hit rate</div><div class="v" id="hitRate">-</div></div>
    <div class="card"><div class="k">Face streak</div><div class="v" id="streak">-</div></div>
    <div class="card"><div class="k">Detect now</div><div class="v" id="detectMs">-</div></div>
    <div class="card"><div class="k">Detect average</div><div class="v" id="detectAvg">-</div></div>
    <div class="card"><div class="k">Detect p95</div><div class="v" id="detectP95">-</div></div>
    <div class="card"><div class="k">Crop / prep</div><div class="v" id="prepMs">-</div></div>
    <div class="card"><div class="k">Capture</div><div class="v" id="captureMs">-</div></div>
    <div class="card"><div class="k">Vision pipeline</div><div class="v" id="visionMs">-</div></div>
    <div class="card"><div class="k">Vision rate</div><div class="v" id="visionFps">-</div></div>
    <div class="card"><div class="k">JPEG encode</div><div class="v" id="encodeMs">-</div></div>
    <div class="card"><div class="k">Compute+encode rate</div><div class="v" id="computeFps">-</div></div>
    <div class="card"><div class="k">Largest face</div><div class="v" id="faceSize">-</div></div>
    <div class="card"><div class="k">Raw frame</div><div class="v" id="frameBytes">-</div></div>
    <div class="card"><div class="k">Free PSRAM</div><div class="v" id="psram">-</div></div>
    <div class="card"><div class="k">Largest PSRAM block</div><div class="v" id="psramLargest">-</div></div>
    <div class="card"><div class="k">Free internal RAM</div><div class="v" id="internal">-</div></div>
    <div class="card"><div class="k">Sensor PID</div><div class="v small" id="sensor">-</div></div>
  </div>
</main>
<script>
const stream = document.getElementById('stream');
const canvas = document.getElementById('overlay');
const ctx = canvas.getContext('2d');
const benchNotice = document.getElementById('benchNotice');
let currentMode = 'fast';
let mirror = 0, flip = 0;

function streamUrl(){ return `http://${location.hostname}:81/stream?t=${Date.now()}`; }
function startStream(){ stream.style.display='block'; benchNotice.style.display='none'; stream.src=streamUrl(); }
function stopStream(){ stream.removeAttribute('src'); stream.style.display='none'; benchNotice.style.display='block'; ctx.clearRect(0,0,canvas.width,canvas.height); }

async function setMode(name){
  await fetch(`/mode?name=${name}`);
  currentMode=name;
  document.querySelectorAll('[data-mode]').forEach(b=>b.classList.toggle('active', b.dataset.mode===name));
  if(name==='bench') stopStream(); else startStream();
}
async function setSensitivity(value){
  await fetch(`/sensitivity?value=${Number(value).toFixed(2)}`);
  document.querySelectorAll('[data-thr]').forEach(b=>b.classList.toggle('active', Number(b.dataset.thr)===Number(value)));
}
async function sensorToggle(which){
  if(which==='hmirror'){ mirror = mirror ? 0 : 1; await fetch(`/sensor?var=hmirror&val=${mirror}`); }
  if(which==='vflip'){ flip = flip ? 0 : 1; await fetch(`/sensor?var=vflip&val=${flip}`); }
}
async function resetStats(){ await fetch('/reset'); }
function fmtBytes(n){ if(n>1048576) return (n/1048576).toFixed(2)+' MB'; if(n>1024) return (n/1024).toFixed(0)+' KB'; return n+' B'; }
function put(id,val){ document.getElementById(id).textContent=val; }

function drawBoxes(m){
  const r = stream.getBoundingClientRect();
  const dpr = devicePixelRatio || 1;
  canvas.width = Math.max(1, Math.round(r.width*dpr));
  canvas.height = Math.max(1, Math.round(r.height*dpr));
  ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,r.width,r.height);
  if(!m.width || !m.height || currentMode==='bench') return;
  const sx = r.width/m.width, sy = r.height/m.height;
  ctx.lineWidth=2;
  ctx.font='12px ui-monospace, monospace';
  for(const b of (m.boxes||[])){
    const x=b.x1*sx, y=b.y1*sy, w=(b.x2-b.x1)*sx, h=(b.y2-b.y1)*sy;
    ctx.strokeStyle='#00ff8a'; ctx.strokeRect(x,y,w,h);
    const label=`${Math.round(w/sx)}×${Math.round(h/sy)} px  ${(b.score*100).toFixed(0)}%`;
    const tw=ctx.measureText(label).width+8;
    ctx.fillStyle='rgba(0,0,0,.72)'; ctx.fillRect(x,Math.max(0,y-18),tw,18);
    ctx.fillStyle='#00ff8a'; ctx.fillText(label,x+4,Math.max(12,y-5));
  }
}

async function poll(){
  try{
    const m=await (await fetch('/metrics',{cache:'no-store'})).json();
    currentMode=m.mode;
    put('mode',m.mode.toUpperCase());
    put('threshold',Number(m.threshold).toFixed(2));
    put('input',m.pixel_format+' direct');
    put('resolution',`${m.width}×${m.height}`);
    put('passes',m.detector_passes || '-');
    put('faces',m.faces);
    put('hitRate',m.samples ? `${m.hit_rate.toFixed(1)}% (${m.hits}/${m.samples})` : '-');
    put('streak',m.face_streak);
    put('detectMs',m.detect_ms+' ms');
    put('detectAvg',m.detect_avg_ms.toFixed(1)+' ms');
    put('detectP95',m.detect_p95_ms+' ms');
    put('prepMs',m.prep_ms+' ms');
    put('captureMs',m.capture_ms+' ms');
    put('visionMs',m.vision_ms+' ms');
    put('visionFps',m.vision_ms ? (1000/m.vision_ms).toFixed(1)+' fps' : '-');
    put('encodeMs',m.encode_ms+' ms');
    const computeMs=(m.vision_ms||0)+(m.encode_ms||0);
    put('computeFps',computeMs ? (1000/computeMs).toFixed(1)+' fps' : '-');
    put('faceSize',m.largest_face_w ? `${m.largest_face_w}×${m.largest_face_h} px` : '-');
    put('frameBytes',fmtBytes(m.frame_bytes));
    put('psram',fmtBytes(m.psram_free));
    put('psramLargest',fmtBytes(m.psram_largest));
    put('internal',fmtBytes(m.internal_free));
    put('sensor','0x'+Number(m.sensor_pid).toString(16).padStart(4,'0'));
    document.querySelectorAll('[data-thr]').forEach(b=>b.classList.toggle('active', Math.abs(Number(b.dataset.thr)-Number(m.threshold))<0.001));
    document.querySelectorAll('[data-mode]').forEach(b=>b.classList.toggle('active', b.dataset.mode===m.mode));
    drawBoxes(m);
  } catch(e) {}
  setTimeout(poll,250);
}

stream.onload=()=>drawBoxes({});
setMode('fast');
poll();
</script>
</body>
</html>
)HTML";
