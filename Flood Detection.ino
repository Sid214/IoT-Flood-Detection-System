/*
 ╔══════════════════════════════════════════════════════════════╗
 ║           FloodWatch Pro — ESP8266 Flood Detection           ║
 ║           Complete rewrite  |  v3.0                         ║
 ╠══════════════════════════════════════════════════════════════╣
 ║  WIRING                                                      ║
 ║  ─────────────────────────────────────────────────────────── ║
 ║  HC-SR04  VCC  → Vin  (5 V)                                  ║
 ║  HC-SR04  GND  → GND                                         ║
 ║  HC-SR04  TRIG → D5  (GPIO 14)                               ║
 ║  HC-SR04  ECHO → D6  (GPIO 12)  ← 5 V signal; works in       ║
 ║                                   practice but a 1k/2k       ║
 ║                                   voltage-divider is safer   ║
 ║  Buzzer   (+)  → D7  (GPIO 13)                               ║
 ║  Buzzer   (−)  → GND                                         ║
 ╠══════════════════════════════════════════════════════════════╣
 ║  WHAT WAS FIXED FROM THE PREVIOUS VERSION                    ║
 ║  ─────────────────────────────────────────────────────────── ║
 ║  • Spurious buzzer: EMA filter + outlier rejection +         ║
 ║    hysteresis so a single bad reading can't trigger alarm    ║
 ║  • Audio latency: Web Audio API oscillator (no file fetch)   ║
 ║    + 200 ms polling = alarm fires within ~200 ms of sensor   ║
 ║  • No sound on laptop: AudioContext unlocked on first click  ║
 ║    with a guaranteed-silent pulse; works Chrome/FF/Safari    ║
 ║  • Page refresh: setInterval fetch – DOM patched in-place    ║
 ║  • Buzzer buzz on boot: digitalWrite LOW before tone()       ║
 ║  • Dashboard rebuilt from scratch, industrial/retro-tech UI  ║
 ╚══════════════════════════════════════════════════════════════╝
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// ── User config ────────────────────────────────────────────────
const char* WIFI_SSID     = "Valar Morgulis";
const char* WIFI_PASSWORD = "Valar Dohaeris.";

// Alert thresholds (centimetres). Change freely.
const int DANGER_CM  = 15;   // ≤ this → DANGER  (flood)
const int WARNING_CM = 30;   // ≤ this → WARNING (rising)
const int MAX_CM     = 400;  // discard readings beyond this

// ── Pins ───────────────────────────────────────────────────────
#define TRIG_PIN   D5
#define ECHO_PIN   D6
#define BUZZ_PIN   D7

// ── Timing (ms) ────────────────────────────────────────────────
#define SENSOR_MS       250   // read interval
#define D_BEEP_ON       110   // DANGER  beep ON
#define D_BEEP_OFF      70    // DANGER  beep OFF
#define W_BEEP_ON       220   // WARNING beep ON
#define W_BEEP_OFF      680   // WARNING beep OFF
#define D_FREQ          4200  // DANGER  tone Hz
#define W_FREQ          2600  // WARNING tone Hz

// ── Hysteresis ─────────────────────────────────────────────────
// Require this many consecutive readings before upgrading level.
// Downgrade is always immediate (fast "all-clear").
#define HYST_COUNT  2

// ── Global state ───────────────────────────────────────────────
ESP8266WebServer server(80);

int  g_dist      = 999;   // smoothed distance cm
int  g_level     = 0;     // 0 safe | 1 warning | 2 danger
bool g_buzzerOn  = true;

// Hysteresis counters
int  hyst_consec  = 0;
int  hyst_pending = 0;

// Non-blocking buzzer
bool     buz_state    = false;
uint32_t buz_lastMs   = 0;

// Sensor timing
uint32_t sens_lastMs  = 0;

// Session stats
int      sess_min     = 9999;
int      sess_max     = 0;
int      sess_alerts  = 0;   // times level entered DANGER

// ══════════════════════════════════════════════════════════════
//  EMBEDDED DASHBOARD  (flash / PROGMEM — does not use RAM)
// ══════════════════════════════════════════════════════════════
const char PAGE[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>FloodWatch Pro</title>
<link rel="preconnect" href="https://fonts.googleapis.com"/>
<link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@500;800;900&family=Share+Tech+Mono&family=DM+Sans:wght@300;400;600&display=swap" rel="stylesheet"/>
<style>
/*── Reset ────────────────────────────────────────────────────*/
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
html{font-size:16px}

/*── Tokens ───────────────────────────────────────────────────*/
:root{
  --bg:      #060e17;
  --surf:    #0c1d2c;
  --surf2:   #102030;
  --brd:     #1a3348;
  --safe:    #00e5a0;
  --warn:    #ffba2e;
  --dng:     #ff3c3c;
  --acc:     #00b8df;
  --txt:     #cce5f5;
  --muted:   #3e5e70;
  --mono:    'Share Tech Mono',monospace;
  --head:    'Orbitron',sans-serif;
  --body:    'DM Sans',sans-serif;
}

/*── Base ─────────────────────────────────────────────────────*/
body{
  background:var(--bg);
  color:var(--txt);
  font-family:var(--body);
  min-height:100vh;
  overflow-x:hidden;
}
/* Radial atmosphere */
body::before{
  content:'';
  position:fixed;inset:0;
  background:
    radial-gradient(ellipse 55% 35% at 15% 8%,rgba(0,184,223,.09) 0%,transparent 60%),
    radial-gradient(ellipse 45% 25% at 85% 92%,rgba(0,229,160,.06) 0%,transparent 55%);
  pointer-events:none;z-index:0;
}
/* Scan-line texture */
body::after{
  content:'';
  position:fixed;inset:0;
  background:repeating-linear-gradient(0deg,transparent,transparent 3px,rgba(0,0,0,.07) 3px,rgba(0,0,0,.07) 4px);
  pointer-events:none;z-index:0;
}

/*── Gateway ─────────────────────────────────────────────────*/
#gw{
  position:fixed;inset:0;z-index:300;
  background:var(--bg);
  display:flex;flex-direction:column;
  align-items:center;justify-content:center;
  text-align:center;padding:24px;
}
.gw-logo{
  font-family:var(--head);
  font-size:clamp(2rem,7vw,3.8rem);
  font-weight:900;
  color:var(--acc);
  letter-spacing:.06em;
  text-shadow:0 0 50px rgba(0,184,223,.55);
  margin-bottom:6px;
}
.gw-logo em{color:var(--txt);font-style:normal;font-weight:500}
.gw-tag{font-size:.82rem;letter-spacing:.18em;text-transform:uppercase;color:var(--muted);margin-bottom:52px}
.gw-btn{
  padding:16px 60px;
  background:var(--acc);color:var(--bg);
  border:none;border-radius:60px;
  font-family:var(--head);font-size:.95rem;font-weight:800;
  letter-spacing:.14em;text-transform:uppercase;
  cursor:pointer;
  box-shadow:0 0 35px rgba(0,184,223,.45),0 0 70px rgba(0,184,223,.15);
  transition:transform .15s,box-shadow .2s;
}
.gw-btn:hover{transform:scale(1.04);box-shadow:0 0 50px rgba(0,184,223,.65)}
.gw-btn:active{transform:scale(.97)}
.gw-note{margin-top:22px;font-size:.75rem;color:var(--muted);max-width:300px;line-height:1.6}

/*── Dashboard ───────────────────────────────────────────────*/
#dash{
  display:none;
  position:relative;z-index:1;
  max-width:980px;margin:0 auto;padding:16px;
}

/*── Header ──────────────────────────────────────────────────*/
.hdr{
  display:flex;align-items:center;justify-content:space-between;
  padding:18px 0 20px;
  border-bottom:1px solid var(--brd);
  margin-bottom:20px;flex-wrap:wrap;gap:10px;
}
.hdr-logo{
  font-family:var(--head);font-size:1.2rem;font-weight:900;
  color:var(--acc);letter-spacing:.06em;
}
.hdr-logo em{color:var(--txt);font-weight:500;font-style:normal}
.hdr-right{display:flex;align-items:center;gap:14px;flex-wrap:wrap}
.live-pill{
  display:flex;align-items:center;gap:6px;
  padding:4px 13px;border-radius:20px;
  border:1px solid rgba(0,229,160,.2);
  background:rgba(0,229,160,.06);
  font-family:var(--mono);font-size:.68rem;color:var(--safe);
}
.live-dot{width:7px;height:7px;border-radius:50%;background:var(--safe);box-shadow:0 0 8px var(--safe);animation:blink 1.4s ease-in-out infinite}
.hdr-time{font-family:var(--mono);font-size:.75rem;color:var(--muted)}

/*── Alert Banner ────────────────────────────────────────────*/
#banner{
  display:none;margin-bottom:18px;
  padding:13px 18px;border-radius:10px;
  border-left:4px solid var(--dng);
  background:rgba(255,60,60,.07);
  font-family:var(--mono);font-size:.83rem;color:var(--dng);
  letter-spacing:.04em;
  animation:banner-pulse 1.1s ease-in-out infinite alternate;
}
#banner.warn{border-color:var(--warn);background:rgba(255,186,46,.07);color:var(--warn);animation:none}

/*── Grid ────────────────────────────────────────────────────*/
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(270px,1fr));gap:16px}

/*── Card ────────────────────────────────────────────────────*/
.card{
  background:var(--surf);border:1px solid var(--brd);
  border-radius:16px;padding:22px;
  position:relative;overflow:hidden;
  transition:border-color .4s,box-shadow .4s;
}
.card::before{
  content:'';position:absolute;top:0;left:0;right:0;height:2px;
  background:linear-gradient(90deg,transparent,var(--acc),transparent);
  opacity:.5;transition:background .4s;
}
.card.lv-warn{border-color:rgba(255,186,46,.35)}
.card.lv-warn::before{background:linear-gradient(90deg,transparent,var(--warn),transparent)}
.card.lv-dng{border-color:rgba(255,60,60,.5);animation:card-glow 1.1s ease-in-out infinite alternate}
.card.lv-dng::before{background:linear-gradient(90deg,transparent,var(--dng),transparent)}

.clabel{
  font-family:var(--mono);font-size:.62rem;
  color:var(--muted);text-transform:uppercase;letter-spacing:.13em;
  margin-bottom:16px;
}

/*── SVG Gauge ───────────────────────────────────────────────*/
.gauge-wrap{display:flex;flex-direction:column;align-items:center}
.gsv{width:190px;height:190px;overflow:visible}
.g-track{fill:none;stroke:var(--brd);stroke-width:11;stroke-linecap:round}
.g-arc{
  fill:none;stroke:var(--safe);stroke-width:11;stroke-linecap:round;
  stroke-dasharray:330 110;stroke-dashoffset:330;
  transform-origin:50% 50%;
  transition:stroke-dashoffset .55s cubic-bezier(.4,0,.2,1),stroke .4s;
}
.g-num{font-family:var(--mono);font-size:2.6rem;fill:var(--txt);text-anchor:middle;dominant-baseline:middle}
.g-unit{font-family:var(--mono);font-size:.78rem;fill:var(--muted);text-anchor:middle}

/*── Status ───────────────────────────────────────────────────*/
.status-body{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:14px;min-height:210px}
.s-ico{font-size:2.8rem;line-height:1;transition:transform .3s}
.s-badge{
  display:inline-flex;align-items:center;gap:8px;
  padding:10px 28px;border-radius:100px;
  font-family:var(--head);font-size:.95rem;font-weight:800;
  letter-spacing:.12em;transition:all .35s;
}
.s-badge.safe{background:rgba(0,229,160,.1);color:var(--safe);border:1px solid rgba(0,229,160,.3)}
.s-badge.warn{background:rgba(255,186,46,.12);color:var(--warn);border:1px solid rgba(255,186,46,.35)}
.s-badge.dng{background:rgba(255,60,60,.15);color:var(--dng);border:1px solid rgba(255,60,60,.45);animation:badge-shk .33s ease infinite alternate}
.s-desc{font-size:.83rem;color:var(--muted);text-align:center;max-width:190px;line-height:1.6}

/*── Tank ─────────────────────────────────────────────────────*/
.tank-wrap{display:flex;flex-direction:column;align-items:center;gap:12px;padding:6px 0}
.tank-outer{
  width:84px;height:152px;
  border:2.5px solid var(--brd);border-radius:10px;
  background:rgba(0,0,0,.35);overflow:hidden;position:relative;
}
.tank-fill{
  position:absolute;bottom:0;left:0;right:0;
  background:var(--acc);
  transition:height .6s cubic-bezier(.4,0,.2,1),background .4s;
}
.tank-fill::before{
  content:'';position:absolute;top:-7px;left:-20px;right:-20px;height:14px;
  background:inherit;border-radius:50%;opacity:.55;
  animation:wave 2.2s ease-in-out infinite alternate;
}
.tank-marks{position:absolute;inset:0;display:flex;flex-direction:column;justify-content:space-evenly;pointer-events:none}
.tank-mark{width:13px;height:1px;background:var(--muted);opacity:.35;margin-left:0}
.tank-pct-val{font-family:var(--mono);font-size:1.1rem}
.tank-pct-lbl{font-size:.68rem;color:var(--muted)}

/*── Canvas chart ─────────────────────────────────────────────*/
#histcanvas{display:block;width:100%;height:110px;border-radius:6px;background:rgba(0,0,0,.25)}
.chart-leg{display:flex;gap:14px;margin-top:8px;font-family:var(--mono);font-size:.6rem}
.cl{display:flex;align-items:center;gap:5px;color:var(--muted)}
.cl-dot{width:10px;height:2px;border-radius:1px}

/*── Toggle switch ────────────────────────────────────────────*/
.ctrl-row{
  display:flex;align-items:center;justify-content:space-between;
  padding:13px 0;border-bottom:1px solid var(--brd);
}
.ctrl-row:last-child{border-bottom:none;padding-bottom:0}
.ctrl-row:first-child{padding-top:0}
.ctrl-lbl{font-size:.86rem}
.ctrl-sub{font-family:var(--mono);font-size:.65rem;color:var(--muted);margin-top:3px}
.tog{appearance:none;width:42px;height:23px;border-radius:12px;
  background:var(--muted);cursor:pointer;position:relative;
  transition:background .25s;flex-shrink:0;border:none;outline:none}
.tog::after{
  content:'';position:absolute;top:2px;left:2px;
  width:19px;height:19px;border-radius:50%;background:#fff;
  box-shadow:0 1px 4px rgba(0,0,0,.4);
  transition:transform .25s;
}
.tog.on{background:var(--safe)}
.tog.on::after{transform:translateX(19px)}
.tog.warn-on{background:var(--warn)}
.tog.dng-on{background:var(--dng)}

/*── Stats ────────────────────────────────────────────────────*/
.stats-g{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:16px}
.stat{background:var(--surf2);border:1px solid var(--brd);border-radius:10px;padding:12px 14px}
.stat-n{font-family:var(--mono);font-size:1.2rem}
.stat-l{font-family:var(--mono);font-size:.58rem;color:var(--muted);text-transform:uppercase;letter-spacing:.1em;margin-top:3px}

/*── Log ──────────────────────────────────────────────────────*/
.log{display:flex;flex-direction:column;gap:7px;max-height:150px;overflow-y:auto}
.log-e{display:flex;gap:8px;align-items:flex-start;font-family:var(--mono);font-size:.69rem;padding:7px 9px;border-radius:6px;background:var(--surf2)}
.log-t{color:var(--muted);white-space:nowrap;flex-shrink:0}
.log-m.safe{color:var(--safe)}.log-m.warn{color:var(--warn)}.log-m.dng{color:var(--dng)}

/*── Flood overlay ────────────────────────────────────────────*/
#ov{
  display:none;position:fixed;inset:0;z-index:200;
  background:rgba(80,0,0,.5);backdrop-filter:blur(3px);
  pointer-events:none;
  animation:ov-pulse .9s ease-in-out infinite alternate;
}
#ov .ov-inner{
  position:absolute;top:50%;left:50%;
  transform:translate(-50%,-50%);
  font-family:var(--head);font-size:clamp(2.2rem,8vw,5rem);
  font-weight:900;color:#fff;letter-spacing:.1em;text-align:center;
  text-shadow:0 0 60px rgba(255,60,60,.9);
  animation:shk .3s ease infinite alternate;
  pointer-events:auto;cursor:pointer;
  transform:translate(-50%,-50%);
}
#ov .ov-sub{display:block;font-size:.75rem;font-weight:500;letter-spacing:.2em;color:rgba(255,255,255,.65);margin-top:8px}

/*── Keyframes ───────────────────────────────────────────────*/
@keyframes blink{0%,100%{opacity:1}50%{opacity:.25}}
@keyframes banner-pulse{from{opacity:.75}to{opacity:1}}
@keyframes card-glow{from{box-shadow:0 0 0 transparent}to{box-shadow:0 0 28px rgba(255,60,60,.18)}}
@keyframes badge-shk{0%{transform:translateX(-2px)}100%{transform:translateX(2px)}}
@keyframes shk{0%{transform:translate(calc(-50% - 2px),-50%)}100%{transform:translate(calc(-50% + 2px),-50%)}}
@keyframes wave{from{transform:translateX(0) scaleY(1)}to{transform:translateX(10px) scaleY(1.4)}}
@keyframes ov-pulse{from{background:rgba(80,0,0,.45)}to{background:rgba(130,0,0,.65)}}

/*── Responsive ──────────────────────────────────────────────*/
@media(max-width:440px){
  .gsv{width:160px;height:160px}
  .g-num{font-size:2.1rem}
}
</style>
</head>
<body>

<!-- Gateway -->
<div id="gw">
  <div class="gw-logo">FloodWatch<em> Pro</em></div>
  <div class="gw-tag">ESP8266 IoT Flood Detection System</div>
  <button class="gw-btn" onclick="launch()">LAUNCH DASHBOARD</button>
  <p class="gw-note">Tap to connect to your sensor node and unlock audio alerts on this device.</p>
</div>

<!-- Flood overlay (tap to dismiss) -->
<div id="ov" onclick="dismissOv()">
  <div class="ov-inner">⚠ FLOOD <span class="ov-sub">TAP TO DISMISS</span></div>
</div>

<!-- Dashboard -->
<div id="dash">

  <div class="hdr">
    <div class="hdr-logo">FloodWatch<em> Pro</em></div>
    <div class="hdr-right">
      <div class="live-pill"><div class="live-dot"></div>LIVE</div>
      <div class="hdr-time" id="hdr-time">--:--:--</div>
    </div>
  </div>

  <div id="banner"></div>

  <div class="grid">

    <!-- 1. Distance Gauge -->
    <div class="card" id="card-gauge">
      <div class="clabel">Distance Reading</div>
      <div class="gauge-wrap">
        <!--
          Circle r=70, cx=cy=100. Circumference≈440.
          270° arc  = 330 px  (dasharray "330 110")
          Rotated 135° so gap opens at the bottom (6-o'clock).
          Dashoffset 330=empty → 0=full (inverted: closer=more fill)
        -->
        <svg class="gsv" viewBox="0 0 200 200">
          <circle class="g-track" cx="100" cy="100" r="70"
            stroke-dasharray="330 110"
            transform="rotate(135 100 100)"/>
          <circle class="g-arc" id="garc" cx="100" cy="100" r="70"
            transform="rotate(135 100 100)"/>
          <text class="g-num" x="100" y="97" id="g-dist">---</text>
          <text class="g-unit" x="100" y="118">cm</text>
        </svg>
      </div>
    </div>

    <!-- 2. Alert Status -->
    <div class="card" id="card-status">
      <div class="clabel">Alert Status</div>
      <div class="status-body">
        <div class="s-ico" id="s-ico">✅</div>
        <div class="s-badge safe" id="s-badge">SAFE</div>
        <div class="s-desc" id="s-desc">Water level normal.<br/>No action required.</div>
      </div>
    </div>

    <!-- 3. Water Tank -->
    <div class="card">
      <div class="clabel">Water Level</div>
      <div class="tank-wrap">
        <div class="tank-outer">
          <div class="tank-marks">
            <div class="tank-mark"></div>
            <div class="tank-mark"></div>
            <div class="tank-mark"></div>
            <div class="tank-mark"></div>
          </div>
          <div id="tank-fill" class="tank-fill" style="height:2%"></div>
        </div>
        <div class="tank-pct-val"><span id="tank-pct">0</span>%</div>
        <div class="tank-pct-lbl">Estimated fill</div>
      </div>
    </div>

    <!-- 4. History Chart -->
    <div class="card">
      <div class="clabel">Reading History (60 samples)</div>
      <canvas id="histcanvas"></canvas>
      <div class="chart-leg">
        <div class="cl"><div class="cl-dot" style="background:var(--dng)"></div>Danger ≤<span id="leg-d">15</span>cm</div>
        <div class="cl"><div class="cl-dot" style="background:var(--warn)"></div>Warning ≤<span id="leg-w">30</span>cm</div>
        <div class="cl"><div class="cl-dot" style="background:var(--safe)"></div>Safe</div>
      </div>
    </div>

    <!-- 5. Controls -->
    <div class="card">
      <div class="clabel">Controls</div>
      <div class="ctrl-row">
        <div>
          <div class="ctrl-lbl">Hardware Buzzer</div>
          <div class="ctrl-sub" id="hw-sub">ESP8266 piezo alarm</div>
        </div>
        <input type="checkbox" class="tog on" id="hw-tog" checked onchange="toggleHW()"/>
      </div>
      <div class="ctrl-row">
        <div>
          <div class="ctrl-lbl">Browser Alarm</div>
          <div class="ctrl-sub">Siren on this device</div>
        </div>
        <input type="checkbox" class="tog on" id="sw-tog" checked onchange="toggleSW()"/>
      </div>
      <div class="ctrl-row">
        <div>
          <div class="ctrl-lbl">Thresholds</div>
          <div class="ctrl-sub" id="thresh-lbl">Danger: 15 cm | Warning: 30 cm</div>
        </div>
      </div>
    </div>

    <!-- 6. Stats + Log -->
    <div class="card">
      <div class="clabel">Statistics</div>
      <div class="stats-g">
        <div class="stat"><div class="stat-n" id="st-min">---</div><div class="stat-l">Min Dist</div></div>
        <div class="stat"><div class="stat-n" id="st-max">---</div><div class="stat-l">Max Dist</div></div>
        <div class="stat"><div class="stat-n" id="st-up">0s</div><div class="stat-l">Uptime</div></div>
        <div class="stat"><div class="stat-n" id="st-al">0</div><div class="stat-l">Flood Events</div></div>
      </div>
      <div class="clabel" style="margin-bottom:8px">Alert Log</div>
      <div class="log" id="log-list">
        <div class="log-e"><span class="log-t">--:--:--</span><span class="log-m safe">Connecting…</span></div>
      </div>
    </div>

  </div><!-- /.grid -->
</div><!-- /#dash -->

<script>
// ══════════════════════════════════════════════════════════════
//  WEB AUDIO ENGINE
//  Uses oscillator + LFO for an instant wailing siren.
//  No audio file download = zero latency, works on all browsers.
// ══════════════════════════════════════════════════════════════
let AC = null;          // AudioContext
let sOsc = null;        // siren oscillator
let sLfo = null;        // LFO for frequency sweep
let sGain = null;       // master gain (for fade-out)
let wTimer = null;      // warning beep interval
let playing = false;
let swOn = true;        // browser alarm enabled

function initAC() {
  AC = new (window.AudioContext || window.webkitAudioContext)();
}

// Unlock AudioContext with a completely silent pulse.
// This must happen inside a user-gesture handler (the launch button).
function unlockAC() {
  if (!AC) return;
  const o = AC.createOscillator(), g = AC.createGain();
  g.gain.value = 0;
  o.connect(g); g.connect(AC.destination);
  o.start(); o.stop(AC.currentTime + 0.001);
}

function startSiren() {
  if (!AC || !swOn || playing) return;
  stopAudio();
  AC.resume().then(() => {
    sOsc = AC.createOscillator();
    sLfo = AC.createOscillator();
    const lG = AC.createGain();
    sGain = AC.createGain();

    // Siren: sawtooth carrier swept by LFO
    sOsc.type = 'sawtooth';
    sOsc.frequency.value = 490;
    sLfo.frequency.value = 1.3;   // sweep speed (Hz)
    lG.gain.value = 260;          // sweep depth (Hz)
    sGain.gain.value = 0.38;

    sLfo.connect(lG); lG.connect(sOsc.frequency);
    sOsc.connect(sGain); sGain.connect(AC.destination);
    sLfo.start(); sOsc.start();
    playing = true;
  });
}

function startWarnBeep() {
  if (!AC || !swOn || playing) return;
  stopAudio();
  playing = true;
  function beep() {
    if (!swOn || !playing || !AC) return;
    const o = AC.createOscillator(), g = AC.createGain();
    o.connect(g); g.connect(AC.destination);
    o.type = 'triangle'; o.frequency.value = 680;
    g.gain.setValueAtTime(0.28, AC.currentTime);
    g.gain.exponentialRampToValueAtTime(0.001, AC.currentTime + 0.28);
    o.start(AC.currentTime); o.stop(AC.currentTime + 0.3);
  }
  AC.resume().then(() => { beep(); wTimer = setInterval(beep, 950); });
}

function stopAudio() {
  if (wTimer) { clearInterval(wTimer); wTimer = null; }
  if (sOsc) {
    try {
      sGain.gain.linearRampToValueAtTime(0, AC.currentTime + 0.18);
      sOsc.stop(AC.currentTime + 0.2);
      sLfo.stop(AC.currentTime + 0.2);
    } catch(e) {}
    sOsc = sLfo = sGain = null;
  }
  playing = false;
}

// ══════════════════════════════════════════════════════════════
//  CANVAS SPARKLINE
// ══════════════════════════════════════════════════════════════
const cv = document.getElementById('histcanvas');
const cx = cv.getContext('2d');
const buf = new Array(60).fill(null);
let rIdx = 0;
let thrD = 15, thrW = 30;

function resizeCV() {
  const dpr = window.devicePixelRatio || 1;
  const r = cv.getBoundingClientRect();
  cv.width  = r.width  * dpr;
  cv.height = r.height * dpr;
  cx.setTransform(dpr, 0, 0, dpr, 0, 0);
  drawChart();
}

function pushReading(v) {
  buf[rIdx] = v;
  rIdx = (rIdx + 1) % 60;
  drawChart();
}

function drawChart() {
  const W = cv.offsetWidth, H = cv.offsetHeight;
  cx.clearRect(0, 0, W, H);

  const pts = [];
  for (let i = 0; i < 60; i++) {
    const v = buf[(rIdx + i) % 60];
    if (v !== null) pts.push({ i, v });
  }
  if (pts.length < 2) return;

  const maxV = Math.max(...pts.map(p => p.v), thrW + 5);
  const minV = Math.max(Math.min(...pts.map(p => p.v)) - 5, 0);
  const rng = maxV - minV || 1;
  const toY = v => H - ((v - minV) / rng) * (H - 8) - 4;
  const toX = i => (i / 59) * W;

  // Threshold dashed lines
  function thLine(val, col) {
    const y = toY(val);
    if (y < 0 || y > H) return;
    cx.save(); cx.strokeStyle = col; cx.lineWidth = 1;
    cx.setLineDash([4,6]); cx.globalAlpha = .4;
    cx.beginPath(); cx.moveTo(0, y); cx.lineTo(W, y); cx.stroke();
    cx.restore();
  }
  thLine(thrD, '#ff3c3c');
  thLine(thrW, '#ffba2e');

  // Fill gradient below line
  const grad = cx.createLinearGradient(0, 0, 0, H);
  grad.addColorStop(0, 'rgba(0,184,223,.2)');
  grad.addColorStop(1, 'rgba(0,184,223,.0)');
  cx.beginPath();
  cx.moveTo(toX(pts[0].i), H);
  pts.forEach(p => cx.lineTo(toX(p.i), toY(p.v)));
  cx.lineTo(toX(pts[pts.length - 1].i), H);
  cx.closePath(); cx.fillStyle = grad; cx.fill();

  // Coloured polyline (segment-by-segment for threshold colouring)
  for (let k = 1; k < pts.length; k++) {
    const a = pts[k - 1], b = pts[k];
    const mid = (a.v + b.v) / 2;
    cx.beginPath();
    cx.moveTo(toX(a.i), toY(a.v));
    cx.lineTo(toX(b.i), toY(b.v));
    cx.strokeStyle = mid <= thrD ? '#ff3c3c' : mid <= thrW ? '#ffba2e' : '#00e5a0';
    cx.lineWidth = 2; cx.stroke();
  }

  // Latest dot
  const lp = pts[pts.length - 1];
  cx.beginPath(); cx.arc(toX(lp.i), toY(lp.v), 4, 0, Math.PI * 2);
  cx.fillStyle = lp.v <= thrD ? '#ff3c3c' : lp.v <= thrW ? '#ffba2e' : '#00e5a0';
  cx.fill();
}

window.addEventListener('resize', resizeCV);

// ══════════════════════════════════════════════════════════════
//  GAUGE
// ══════════════════════════════════════════════════════════════
const garc = document.getElementById('garc');
const GARC = 330;   // total arc length in px (270° of r=70 circle)

function setGauge(dist) {
  const pct = Math.min(Math.max(dist, 0), 400) / 400;
  garc.style.strokeDashoffset = (GARC * pct).toFixed(1);
}

// ══════════════════════════════════════════════════════════════
//  LEVEL UI
// ══════════════════════════════════════════════════════════════
const ICO  = ['✅','⚠️','🚨'];
const NAME = ['SAFE','WARNING','FLOOD'];
const CLS  = ['safe','warn','dng'];
const DESC = [
  'Water level normal.<br/>No action required.',
  'Water is rising.<br/>Monitor closely.',
  'FLOOD DETECTED!<br/>Take immediate action!'
];

let prevLv = -1;
let ovDismissed = false;

function applyLevel(lv) {
  document.getElementById('s-ico').textContent   = ICO[lv];
  const sb = document.getElementById('s-badge');
  sb.textContent = NAME[lv]; sb.className = 's-badge ' + CLS[lv];
  document.getElementById('s-desc').innerHTML    = DESC[lv];

  // Card accents
  const cls = lv == 2 ? 'card lv-dng' : lv == 1 ? 'card lv-warn' : 'card';
  document.getElementById('card-gauge').className  = cls;
  document.getElementById('card-status').className = cls;

  // Arc colour
  garc.style.stroke = lv == 2 ? 'var(--dng)' : lv == 1 ? 'var(--warn)' : 'var(--safe)';

  // Alert banner
  const bn = document.getElementById('banner');
  if (lv == 2) {
    bn.style.display = 'block'; bn.className = '';
    bn.textContent = '🚨  FLOOD ALERT — Water level is critical!';
  } else if (lv == 1) {
    bn.style.display = 'block'; bn.className = 'warn';
    bn.textContent = '⚠️  WARNING — Water is rising. Monitor closely.';
  } else {
    bn.style.display = 'none';
  }

  // Overlay
  const ov = document.getElementById('ov');
  if (lv == 2 && !ovDismissed) { ov.style.display = 'block'; }
  else if (lv < 2) { ov.style.display = 'none'; ovDismissed = false; }

  // Audio — start/stop only on level transitions
  if (lv == 2)      startSiren();
  else if (lv == 1) startWarnBeep();
  else              stopAudio();
}

function dismissOv() {
  ovDismissed = true;
  document.getElementById('ov').style.display = 'none';
}

// ══════════════════════════════════════════════════════════════
//  CONTROLS
// ══════════════════════════════════════════════════════════════
function toggleHW() {
  fetch('/toggle').then(r => r.json()).then(d => {
    const el = document.getElementById('hw-tog');
    el.className = 'tog' + (d.buzzer ? ' on' : '');
    document.getElementById('hw-sub').textContent = d.buzzer ? 'ESP8266 piezo alarm' : 'Muted via dashboard';
  }).catch(() => {});
}

function toggleSW() {
  swOn = document.getElementById('sw-tog').checked;
  const el = document.getElementById('sw-tog');
  el.className = 'tog' + (swOn ? ' on' : '');
  if (!swOn) stopAudio();
}

// ══════════════════════════════════════════════════════════════
//  CLOCK
// ══════════════════════════════════════════════════════════════
function tickClock() {
  document.getElementById('hdr-time').textContent =
    new Date().toLocaleTimeString('en-GB');
}
setInterval(tickClock, 1000); tickClock();

// ══════════════════════════════════════════════════════════════
//  LOG
// ══════════════════════════════════════════════════════════════
const logBuf = [];
function addLog(msg, cls) {
  const ts = new Date().toLocaleTimeString('en-GB');
  logBuf.unshift({ ts, msg, cls });
  if (logBuf.length > 12) logBuf.pop();
  document.getElementById('log-list').innerHTML =
    logBuf.map(e =>
      `<div class="log-e"><span class="log-t">${e.ts}</span><span class="log-m ${e.cls}">${e.msg}</span></div>`
    ).join('');
}

// ══════════════════════════════════════════════════════════════
//  UPTIME FORMATTER
// ══════════════════════════════════════════════════════════════
function fmtUp(s) {
  if (s < 60)   return s + 's';
  if (s < 3600) return Math.floor(s / 60) + 'm ' + (s % 60) + 's';
  return Math.floor(s / 3600) + 'h ' + Math.floor((s % 3600) / 60) + 'm';
}

// ══════════════════════════════════════════════════════════════
//  MAIN POLLING — 200 ms for near-zero latency
// ══════════════════════════════════════════════════════════════
let sessMin = Infinity, sessMax = -Infinity;
let sessAlerts = 0;

async function poll() {
  try {
    const r = await fetch('/data');
    const d = await r.json();

    const dist = d.distance;
    const lv   = d.alert;
    thrD = d.danger  || 15;
    thrW = d.warning || 30;

    // Distance
    document.getElementById('g-dist').textContent = dist;
    setGauge(dist);
    pushReading(dist);

    // Tank: maps 0–100 cm linearly to 0–100% fill
    const pct = Math.round(Math.max(0, Math.min(100, (1 - dist / 100) * 100)));
    document.getElementById('tank-pct').textContent = pct;
    const tf = document.getElementById('tank-fill');
    tf.style.height = pct + '%';
    tf.style.background = lv == 2 ? 'var(--dng)' : lv == 1 ? 'var(--warn)' : 'var(--acc)';

    // Stats
    if (dist < sessMin) sessMin = dist;
    if (dist > sessMax) sessMax = dist;
    document.getElementById('st-min').textContent = sessMin === Infinity ? '---' : sessMin + ' cm';
    document.getElementById('st-max').textContent = sessMax === -Infinity ? '---' : sessMax + ' cm';
    document.getElementById('st-up').textContent  = fmtUp(d.uptime || 0);

    // Threshold labels
    document.getElementById('thresh-lbl').textContent = `Danger: ${thrD} cm  |  Warning: ${thrW} cm`;
    document.getElementById('leg-d').textContent = thrD;
    document.getElementById('leg-w').textContent = thrW;

    // Session alert counter (use server-side count)
    sessAlerts = d.alerts || 0;
    document.getElementById('st-al').textContent = sessAlerts;

    // Level change → update UI and audio
    if (lv !== prevLv) {
      applyLevel(lv);
      if (lv == 2) {
        ovDismissed = false;
        addLog('🚨 FLOOD at ' + dist + ' cm', 'dng');
      } else if (lv == 1) {
        addLog('⚠️ Rising water — ' + dist + ' cm', 'warn');
      } else if (prevLv > 0) {
        addLog('✅ Returned to safe — ' + dist + ' cm', 'safe');
      }
      prevLv = lv;
    }

    // Sync hardware buzzer toggle
    const hwEl = document.getElementById('hw-tog');
    hwEl.checked = d.buzzer;
    hwEl.className = 'tog' + (d.buzzer ? ' on' : '');

  } catch(e) {
    // Connection lost — silently retry
  }
}

// ══════════════════════════════════════════════════════════════
//  LAUNCH
// ══════════════════════════════════════════════════════════════
function launch() {
  initAC();
  unlockAC();   // <-- must be in user-gesture handler; unlocks audio everywhere
  document.getElementById('gw').style.display   = 'none';
  document.getElementById('dash').style.display = 'block';
  setTimeout(resizeCV, 60);
  addLog('✅ Dashboard connected', 'safe');
  poll();
  setInterval(poll, 200);
}
</script>
</body>
</html>
)HTMLEOF";


// ══════════════════════════════════════════════════════════════
//  SENSOR — single reading, validated
// ══════════════════════════════════════════════════════════════
int rawReading() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (dur == 0) return -1;

  int d = (int)(dur * 0.0343f / 2.0f);
  return (d <= 0 || d > MAX_CM) ? -1 : d;
}

void updateSensor() {
  int raw = rawReading();

  if (raw == -1) {
    // Timeout / out-of-range — keep last known value to avoid
    // a spurious "0 cm = FLOOD" trigger on a missed pulse.
    Serial.println(F("[Sensor] timeout — holding last value"));
    return;
  }

  // Outlier rejection: a real flood cannot move 80+ cm in 250 ms.
  // This kills multipath ghost echoes and ceiling reflections.
  if (g_dist != 999 && abs(raw - g_dist) > 80) {
    Serial.printf("[Sensor] outlier discarded  raw=%d  held=%d\n", raw, g_dist);
    return;
  }

  // Exponential moving average  (α = 0.35 → fairly smooth but responsive)
  g_dist = (g_dist == 999)
         ? raw
         : (int)(0.35f * raw + 0.65f * g_dist);

  // Session min / max
  if (g_dist < sess_min) sess_min = g_dist;
  if (g_dist > sess_max) sess_max = g_dist;

  // Raw target level
  int target = (g_dist <= DANGER_CM) ? 2
             : (g_dist <= WARNING_CM) ? 1
             : 0;

  // ── Hysteresis ────────────────────────────────────────────
  // Only upgrade (higher severity) after HYST_COUNT consecutive
  // readings at the higher level.  Downgrade is always immediate.
  if (target > g_level) {
    if (target == hyst_pending) {
      if (++hyst_consec >= HYST_COUNT) {
        g_level     = target;
        hyst_consec = 0;
        if (g_level == 2) sess_alerts++;
        Serial.printf("[Alert] Level → %d  dist=%d\n", g_level, g_dist);
      }
    } else {
      hyst_pending = target;
      hyst_consec  = 1;
    }
  } else {
    // Drop immediately
    g_level      = target;
    hyst_pending = target;
    hyst_consec  = 0;
  }

  Serial.printf("[Sensor] raw=%d  smooth=%d  level=%d\n", raw, g_dist, g_level);
}


// ══════════════════════════════════════════════════════════════
//  BUZZER — non-blocking state machine
//  Two distinct patterns so the user can tell levels by ear.
// ══════════════════════════════════════════════════════════════
void updateBuzzer() {
  if (!g_buzzerOn || g_level == 0) {
    if (buz_state) {
      noTone(BUZZ_PIN);
      digitalWrite(BUZZ_PIN, LOW);
      buz_state = false;
    }
    return;
  }

  uint32_t now = millis(), el = now - buz_lastMs;

  if (g_level == 2) {                       // DANGER: rapid high beep
    if (buz_state  && el >= D_BEEP_ON)  { noTone(BUZZ_PIN); digitalWrite(BUZZ_PIN, LOW); buz_state = false; buz_lastMs = now; }
    if (!buz_state && el >= D_BEEP_OFF) { tone(BUZZ_PIN, D_FREQ);                        buz_state = true;  buz_lastMs = now; }
  } else {                                  // WARNING: slow low beep
    if (buz_state  && el >= W_BEEP_ON)  { noTone(BUZZ_PIN); digitalWrite(BUZZ_PIN, LOW); buz_state = false; buz_lastMs = now; }
    if (!buz_state && el >= W_BEEP_OFF) { tone(BUZZ_PIN, W_FREQ);                        buz_state = true;  buz_lastMs = now; }
  }
}


// ══════════════════════════════════════════════════════════════
//  HTTP ROUTES
// ══════════════════════════════════════════════════════════════
void handleRoot() {
  server.send_P(200, "text/html", PAGE);
}

void handleData() {
  // Build JSON manually — no ArduinoJson needed
  String j = F("{\"distance\":");  j += g_dist;
  j += F(",\"alert\":");           j += g_level;
  j += F(",\"buzzer\":");          j += g_buzzerOn ? "true" : "false";
  j += F(",\"uptime\":");          j += millis() / 1000;
  j += F(",\"danger\":");          j += DANGER_CM;
  j += F(",\"warning\":");         j += WARNING_CM;
  j += F(",\"alerts\":");          j += sess_alerts;
  j += '}';

  server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
  server.send(200, F("application/json"), j);
}

void handleToggle() {
  g_buzzerOn = !g_buzzerOn;
  if (!g_buzzerOn) { noTone(BUZZ_PIN); digitalWrite(BUZZ_PIN, LOW); buz_state = false; }
  String j = F("{\"buzzer\":");
  j += g_buzzerOn ? F("true}") : F("false}");
  server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
  server.send(200, F("application/json"), j);
}


// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n╔═══════════════════════════╗"));
  Serial.println(F("║  FloodWatch Pro  v3.0     ║"));
  Serial.println(F("╚═══════════════════════════╝"));

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZ_PIN, OUTPUT);
  // ← Write LOW first; prevents buzzer chirp on power-up
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(BUZZ_PIN, LOW);

  // Two-tone startup beep confirms the buzzer is alive
  tone(BUZZ_PIN, 1200); delay(90);
  noTone(BUZZ_PIN); digitalWrite(BUZZ_PIN, LOW); delay(70);
  tone(BUZZ_PIN, 1800); delay(90);
  noTone(BUZZ_PIN); digitalWrite(BUZZ_PIN, LOW);

  // WiFi
  Serial.printf("\nConnecting to: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print('.');
    if (++tries > 40) { Serial.println(F("\n[FAIL] WiFi timeout — restarting")); ESP.restart(); }
  }
  Serial.printf("\n[OK]  IP Address → http://%s\n", WiFi.localIP().toString().c_str());

  server.on("/",       HTTP_GET, handleRoot);
  server.on("/data",   HTTP_GET, handleData);
  server.on("/toggle", HTTP_GET, handleToggle);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println(F("[OK]  HTTP server started\n"));
}


// ══════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();   // ← first, so HTTP stays responsive

  uint32_t now = millis();
  if (now - sens_lastMs >= SENSOR_MS) {
    sens_lastMs = now;
    updateSensor();
  }

  updateBuzzer();          // non-blocking; no delay() ever used here
}
