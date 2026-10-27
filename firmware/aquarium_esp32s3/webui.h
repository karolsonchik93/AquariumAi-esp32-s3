/* =====================================================================
   webui.h — живой веб-интерфейс аквариумного контроллера на ESP32-S3.
   Страница целиком лежит во флеш-памяти платы: ни CDN, ни интернета
   браузеру не нужно. Телеметрия приходит по Server-Sent Events —
   интерфейс обновляется мгновенно, без опроса.
   ===================================================================== */
#pragma once

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#04070d">
<title>Аквариум · ESP32-S3</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#04070d;--panel:rgba(255,255,255,.045);--panel2:rgba(255,255,255,.075);
  --line:rgba(255,255,255,.09);--fg:#e8eef6;--dim:#8b9bad;--mute:#4d5b6b;
  --accent:#00e5ff;--accent-rgb:0,229,255;--ok:#22d37f;--warn:#ffb020;--danger:#ff4d5e;
  --water:#0a2a4a;--radius:18px;--glass:blur(14px) saturate(140%);
}
body[data-phase="dawn"]{--accent:#ffa63d;--accent-rgb:255,166,61}
body[data-phase="day"]{--accent:#00e5ff;--accent-rgb:0,229,255}
body[data-phase="dusk"]{--accent:#b06bff;--accent-rgb:176,107,255}
body[data-phase="night"]{--accent:#5b7cff;--accent-rgb:91,124,255}
body[data-phase="manual"]{--accent:#4be3a1;--accent-rgb:75,227,161}
body.alert{--accent:#ff4d5e;--accent-rgb:255,77,94}
html{-webkit-text-size-adjust:100%;scroll-behavior:smooth}
body{background:var(--bg);color:var(--fg);min-height:100vh;overflow-x:hidden;
  font:15px/1.5 "Segoe UI",system-ui,-apple-system,Roboto,sans-serif;
  padding:16px 14px 48px;transition:background 1.2s ease}
/* аврора на фоне: медленно плывущие цветные пятна */
.aurora{position:fixed;inset:-20% -20%;z-index:0;pointer-events:none;filter:blur(70px);opacity:.55;
  background:
    radial-gradient(38% 45% at 22% 18%,rgba(var(--accent-rgb),.30),transparent 70%),
    radial-gradient(32% 40% at 78% 26%,rgba(91,124,255,.24),transparent 70%),
    radial-gradient(45% 50% at 55% 88%,rgba(0,120,180,.26),transparent 72%);
  animation:drift 26s ease-in-out infinite alternate;transition:background 1.5s ease}
@keyframes drift{0%{transform:translate3d(-2%,-1%,0) scale(1)}50%{transform:translate3d(2%,2%,0) scale(1.06)}100%{transform:translate3d(-1%,3%,0) scale(1.02)}}
.wrap{position:relative;z-index:1;max-width:1120px;margin:0 auto}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;padding:4px 2px 16px}
.brand{display:flex;align-items:center;gap:10px;font-weight:700;letter-spacing:.1em;text-transform:uppercase;font-size:13px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--accent);box-shadow:0 0 16px var(--accent);
  animation:breathe 2.6s ease-in-out infinite;transition:background .8s,box-shadow .8s}
@keyframes breathe{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.45;transform:scale(.86)}}
.hud{display:flex;align-items:center;gap:14px;font-size:13px;color:var(--dim);flex-wrap:wrap}
.chip{display:inline-flex;align-items:center;gap:6px;padding:5px 11px;border-radius:999px;
  background:var(--panel);border:1px solid var(--line);font-variant-numeric:tabular-nums}
.clock{color:var(--fg);font-weight:600}
.card{background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);
  padding:18px;margin-bottom:16px;backdrop-filter:var(--glass);-webkit-backdrop-filter:var(--glass);
  box-shadow:0 18px 40px -28px rgba(0,0,0,.9);transition:transform .25s ease,border-color .25s ease}
.card:hover{border-color:rgba(var(--accent-rgb),.28)}
.card h2{font-size:13px;font-weight:600;letter-spacing:.12em;text-transform:uppercase;color:#c9d5e3}
.head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:14px;flex-wrap:wrap}
.note{color:var(--dim);font-size:12.5px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:16px}
.grid .card{margin-bottom:0}
.wide{grid-column:1/-1}

/* ══════════ АКВАРИУМ ══════════ */
.hero{display:grid;grid-template-columns:1fr;gap:18px}
@media(min-width:900px){.hero{grid-template-columns:1.45fr .9fr}}
.tank{position:relative;height:300px;border-radius:22px;overflow:hidden;
  background:linear-gradient(180deg,#061423 0%,#04101c 55%,#020a12 100%);
  border:1px solid rgba(var(--accent-rgb),.22);box-shadow:inset 0 0 60px rgba(0,0,0,.7),0 0 40px -18px rgba(var(--accent-rgb),.5)}
.lamp{position:absolute;top:0;left:8%;right:8%;height:8px;border-radius:0 0 8px 8px;background:rgba(255,255,255,.08)}
.lamp i{display:block;height:100%;width:var(--led,0%);max-width:100%;border-radius:0 0 8px 8px;
  background:linear-gradient(90deg,rgba(var(--accent-rgb),.5),var(--accent));transition:width .8s ease}
.bloom{position:absolute;top:0;left:0;right:0;height:150px;pointer-events:none;opacity:var(--led-o,0);
  background:radial-gradient(60% 100% at 50% 0,rgba(var(--accent-rgb),.5),transparent 72%);transition:opacity .8s ease}
.water{position:absolute;left:0;right:0;bottom:0;height:78%;
  background:linear-gradient(180deg,rgba(0,180,255,.16),rgba(0,60,120,.42) 60%,rgba(0,30,70,.6));
  transition:height .6s ease}
.waves{position:absolute;left:-50%;right:-50%;top:20.5%;height:26px;overflow:visible}
.waves path{fill:rgba(120,220,255,.16)}
.waves .w2{fill:rgba(120,220,255,.09)}
.waves .w1{animation:waveMove 9s linear infinite}
.waves .w2{animation:waveMove 14s linear infinite reverse}
@keyframes waveMove{from{transform:translateX(0)}to{transform:translateX(25%)}}
.bubbles{position:absolute;inset:0;pointer-events:none;opacity:0;transition:opacity .6s ease}
body.air-on .bubbles{opacity:1}
.bubbles i{position:absolute;bottom:6%;width:7px;height:7px;border-radius:50%;
  background:radial-gradient(circle at 30% 30%,#fff,rgba(160,230,255,.35));opacity:.75;
  animation:rise linear infinite}
@keyframes rise{0%{transform:translateY(0) scale(.6);opacity:0}
  12%{opacity:.85}100%{transform:translateY(-215px) scale(1.15);opacity:0}}
.fish{position:absolute;top:46%;left:6%;width:64px;height:auto;opacity:.5;
  animation:swim 24s ease-in-out infinite;filter:drop-shadow(0 6px 12px rgba(0,0,0,.6));
  transform:scaleX(-1)}
/* рыбка нарисована головой влево, поэтому в начале она отражена: плывёт головой вперёд */
@keyframes swim{0%{transform:translate(0,0) scaleX(-1)}45%{transform:translate(360px,-18px) scaleX(-1)}
  50%{transform:translate(380px,-18px) scaleX(1)}95%{transform:translate(10px,12px) scaleX(1)}
  100%{transform:translate(0,0) scaleX(-1)}}
.impeller{position:absolute;left:9%;bottom:12%;width:44px;height:44px}
.impeller .blades{transform-origin:50% 50%;animation:spin 2.6s linear infinite;animation-play-state:paused}
body.pump-on .impeller .blades{animation-play-state:running}
@keyframes spin{to{transform:rotate(360deg)}}
.fan{position:absolute;right:8%;top:8%;width:40px;height:40px;opacity:.45}
.fan .blades{transform-origin:50% 50%;animation:spin 1s linear infinite;animation-play-state:paused}
body.fan-on .fan{opacity:1}
body.fan-on .fan .blades{animation-play-state:running}
.heater{position:absolute;right:12%;bottom:10%;width:14px;height:64px;border-radius:7px;
  background:rgba(255,255,255,.09);overflow:hidden}
.heater i{display:block;position:absolute;inset:0;background:linear-gradient(180deg,#ff7a3c,#ff3d00);
  opacity:0;transition:opacity .5s ease}
body.heat-on .heater i{opacity:1;animation:heatPulse 1.6s ease-in-out infinite}
@keyframes heatPulse{0%,100%{filter:brightness(1)}50%{filter:brightness(1.6)}}
.heatwaves{position:absolute;right:9%;bottom:26%;width:20px;height:70px;pointer-events:none;opacity:0}
body.heat-on .heatwaves{opacity:1}
.heatwaves i{position:absolute;left:50%;width:3px;height:22px;border-radius:2px;margin-left:-1.5px;
  background:linear-gradient(180deg,rgba(255,140,60,0),rgba(255,140,60,.85),rgba(255,140,60,0));
  animation:heatUp 2.4s linear infinite}
@keyframes heatUp{0%{transform:translateY(18px) scaleY(.6);opacity:0}30%{opacity:.9}100%{transform:translateY(-46px) scaleY(1.1);opacity:0}}
.readout{position:absolute;left:50%;top:46%;transform:translate(-50%,-50%);text-align:center;pointer-events:none;
  padding:22px 30px;border-radius:50%}
.readout::before{content:'';position:absolute;inset:-10%;border-radius:50%;z-index:-1;
  background:radial-gradient(closest-side,rgba(2,10,20,.72),transparent 72%)}
.readout .big{font-size:60px;font-weight:200;letter-spacing:-.04em;font-variant-numeric:tabular-nums;
  text-shadow:0 0 40px rgba(var(--accent-rgb),.45),0 2px 18px rgba(0,0,0,.85)}
.readout .big b{font-weight:200}
.readout .big span{font-size:22px;color:var(--dim);margin-left:4px}
.readout .sub{margin-top:2px;font-size:11.5px;letter-spacing:.28em;text-transform:uppercase;color:var(--accent)}
.readout .tiny{margin-top:6px;font-size:12px;color:var(--mute)}
.ringwrap{position:absolute;left:50%;top:46%;transform:translate(-50%,-50%);width:210px;height:210px;pointer-events:none}
.ringwrap svg{width:100%;height:100%;transform:rotate(-90deg)}
.ring-bg{fill:none;stroke:rgba(255,255,255,.07);stroke-width:3}
.ring-fg{fill:none;stroke:var(--accent);stroke-width:3.5;stroke-linecap:round;
  stroke-dasharray:596.9;stroke-dashoffset:596.9;transition:stroke-dashoffset .8s cubic-bezier(.3,0,.2,1),stroke .8s;
  filter:drop-shadow(0 0 10px rgba(var(--accent-rgb),.6))}
.ring-band{fill:none;stroke:rgba(34,211,127,.55);stroke-width:9;stroke-linecap:butt}
body.heating .ring-fg{animation:ringPulse 1.8s ease-in-out infinite}
@keyframes ringPulse{0%,100%{opacity:1}50%{opacity:.45}}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px;align-content:start}
.tile{background:var(--panel2);border-radius:14px;padding:11px 13px;border:1px solid var(--line)}
.tile .k{display:block;color:var(--dim);font-size:11.5px;letter-spacing:.02em}
.tile .v{font-size:19px;font-variant-numeric:tabular-nums;white-space:nowrap}
.stats .tile .v{font-size:17px}
.tile.small .v{font-size:14px;white-space:normal;word-break:break-word}
.flash{animation:flash .9s ease-out}
@keyframes flash{0%{background:rgba(var(--accent-rgb),.4)}100%{background:transparent}}
/* ══════════ СУТОЧНАЯ ПОЛОСА ══════════ */
.daybar{position:relative;height:52px;border-radius:14px;background:rgba(255,255,255,.045);
  border:1px solid var(--line);overflow:hidden}
.daybar .seg{position:absolute;top:0;bottom:0;display:flex;align-items:center;justify-content:center;
  font-size:10.5px;letter-spacing:.06em;color:#dbe6f2;white-space:nowrap;overflow:hidden}
.seg.dawn{background:linear-gradient(90deg,rgba(255,166,61,.45),rgba(255,214,120,.35))}
.seg.day{background:linear-gradient(90deg,rgba(0,229,255,.28),rgba(0,229,255,.34))}
.seg.dusk{background:linear-gradient(90deg,rgba(176,107,255,.38),rgba(120,80,255,.42))}
.seg.night{background:linear-gradient(90deg,rgba(91,124,255,.30),rgba(60,90,220,.30))}
.seg.dark{background:rgba(255,255,255,.03)}
.now{position:absolute;top:-1px;bottom:-1px;width:2px;background:var(--accent);
  box-shadow:0 0 14px 2px rgba(var(--accent-rgb),.8);transition:left 1s linear}
.now::after{content:'';position:absolute;top:-3px;left:-3px;width:8px;height:8px;border-radius:50%;
  background:var(--accent);box-shadow:0 0 12px var(--accent)}
/* ══════════ ГЕРОЙ: сцена, лучи, индикатор ══════════ */
.heroCard .head{align-items:flex-start}
.hbadge{display:inline-flex;align-items:center;gap:7px;padding:5px 11px;border-radius:999px;font-size:10.5px;
  font-weight:700;letter-spacing:.09em;background:rgba(34,211,127,.13);color:#67eaa5;
  border:1px solid rgba(34,211,127,.38);transition:.35s;white-space:nowrap}
.hbadge i{width:7px;height:7px;border-radius:50%;background:currentColor;box-shadow:0 0 10px currentColor;
  animation:breathe 2.2s ease-in-out infinite}
.hbadge.err{background:rgba(255,77,94,.16);color:#ff9099;border-color:rgba(255,77,94,.45)}
.hero .scene{min-width:0}
.tank{height:340px}
.rays{position:absolute;inset:0;overflow:hidden;pointer-events:none;opacity:.55;mix-blend-mode:screen}
.rays i{position:absolute;top:-12%;width:16%;height:86%;filter:blur(7px);
  background:linear-gradient(180deg,rgba(180,240,255,.32),rgba(180,240,255,0) 78%);
  animation:raySway 12s ease-in-out infinite}
.rays i:nth-child(1){left:13%}
.rays i:nth-child(2){left:45%;width:10%;animation-delay:-4s;opacity:.75}
.rays i:nth-child(3){left:71%;width:20%;animation-delay:-8s;opacity:.55}
@keyframes raySway{0%,100%{transform:skewX(-15deg) translateX(0);opacity:.34}
  50%{transform:skewX(-7deg) translateX(12px);opacity:.72}}
.caustics{position:absolute;inset:0;pointer-events:none;mix-blend-mode:screen;opacity:.5;overflow:hidden}
.caustics i{position:absolute;border-radius:50%;filter:blur(20px);
  background:radial-gradient(circle,rgba(140,235,255,.34),transparent 72%);animation:drift2 19s ease-in-out infinite}
.caustics i:nth-child(1){left:8%;top:42%;width:150px;height:66px}
.caustics i:nth-child(2){left:50%;top:58%;width:190px;height:58px;animation-delay:-7s}
.caustics i:nth-child(3){left:72%;top:34%;width:120px;height:48px;animation-delay:-13s}
@keyframes drift2{0%,100%{transform:translate(0,0) scale(1)}50%{transform:translate(26px,-12px) scale(1.18)}}
.sand{position:absolute;left:0;right:0;bottom:0;height:36px;pointer-events:none;
  background:radial-gradient(130% 100% at 50% 100%,rgba(198,170,122,.34),rgba(120,100,70,.10) 72%,transparent)}
.plants{position:absolute;left:0;right:0;bottom:26px;height:84px;pointer-events:none}
.plants i{position:absolute;bottom:0;width:7px;border-radius:7px 7px 0 0;transform-origin:bottom center;
  background:linear-gradient(180deg,rgba(64,205,145,.55),rgba(18,80,58,.22));animation:sway 7s ease-in-out infinite}
.plants i:nth-child(1){left:11%;height:60px}
.plants i:nth-child(2){left:14.5%;height:40px;animation-delay:-2.5s}
.plants i:nth-child(3){left:77%;height:68px;animation-delay:-1.5s}
.plants i:nth-child(4){left:80.5%;height:42px;animation-delay:-4s}
@keyframes sway{0%,100%{transform:rotate(-3.5deg)}50%{transform:rotate(3.5deg)}}
@keyframes swimBack{0%{transform:translate(0,0) scaleX(1)}45%{transform:translate(-300px,10px) scaleX(1)}
  50%{transform:translate(-320px,10px) scaleX(-1)}95%{transform:translate(-10px,-8px) scaleX(-1)}
  100%{transform:translate(0,0) scaleX(1)}}
.fish.f1{animation-duration:27s}
.fish.f2{top:64%;left:20%;width:46px;opacity:.42;animation-name:swimBack;animation-duration:36s}
/* индикатор: дуга 270° */
.gauge{position:absolute;left:50%;top:46%;transform:translate(-50%,-50%);width:min(64%,272px);aspect-ratio:1;
  display:grid;place-items:center;pointer-events:none}
.gauge svg{position:absolute;inset:0;width:100%;height:100%;overflow:visible}
.gTrack{fill:none;stroke:rgba(255,255,255,.09);stroke-width:9;stroke-linecap:round;
  stroke-dasharray:377 502.7}
.gBand{fill:none;stroke:rgba(34,211,127,.55);stroke-width:9;stroke-linecap:butt;
  transition:stroke-dasharray .7s ease,stroke-dashoffset .7s ease}
.gVal{fill:none;stroke:url(#gArc);stroke-width:9;stroke-linecap:round;stroke-dasharray:0 502.7;
  transition:stroke-dasharray .9s cubic-bezier(.3,0,.2,1);filter:drop-shadow(0 0 7px rgba(34,211,238,.8))}
.gDot{fill:#eafcff;transition:cx .9s cubic-bezier(.3,0,.2,1),cy .9s cubic-bezier(.3,0,.2,1)}
.gDotGlow{fill:rgba(34,211,238,.30);transform-box:fill-box;transform-origin:center;
  animation:dotPulse 2.6s ease-in-out infinite;transition:cx .9s cubic-bezier(.3,0,.2,1),cy .9s cubic-bezier(.3,0,.2,1)}
@keyframes dotPulse{0%,100%{transform:scale(.65);opacity:.45}50%{transform:scale(1.4);opacity:.10}}
.gRead{position:relative;z-index:2;text-align:center;padding:8px 14px;border-radius:50%}
.gRead::before{content:'';position:absolute;inset:-8%;border-radius:50%;z-index:-1;
  background:radial-gradient(closest-side,rgba(2,10,20,.80),transparent 76%)}
.gTemp{font-size:clamp(30px,6.5vw,50px);font-weight:200;letter-spacing:-.03em;line-height:1.05;
  font-variant-numeric:tabular-nums;text-shadow:0 0 34px rgba(var(--accent-rgb),.42),0 2px 16px rgba(0,0,0,.85)}
.gTemp b{font-weight:200}
.gTemp span{font-size:.42em;color:var(--dim);margin-left:2px}
.gPhase{margin-top:2px;font-size:10.5px;letter-spacing:.26em;text-transform:uppercase;color:var(--accent)}
.gTrend{margin-top:6px;font-size:11.5px;color:var(--dim);font-variant-numeric:tabular-nums}
.gGoal{margin-top:2px;font-size:10.5px;color:var(--mute)}
/* правая колонка */
.hside{display:flex;flex-direction:column;gap:9px;min-width:0}
.hrow{display:grid;grid-template-columns:24px 1fr;grid-template-areas:"ico body" "bar bar" "sub sub";
  gap:1px 9px;background:var(--panel2);border:1px solid var(--line);border-radius:14px;
  padding:10px 12px 9px;transition:border-color .35s,background .35s}
.hico{grid-area:ico;font-size:14px;line-height:1.35;opacity:.85}
.hbody{grid-area:body;display:flex;align-items:baseline;justify-content:space-between;gap:8px;min-width:0}
.hk{font-size:10px;letter-spacing:.07em;text-transform:uppercase;color:var(--dim)}
.hv{font-size:14.5px;font-variant-numeric:tabular-nums;white-space:nowrap;font-weight:600}
.hbar{grid-area:bar;height:4px;border-radius:3px;background:rgba(255,255,255,.09);overflow:hidden;margin-top:7px}
.hbar>i{display:block;height:100%;width:0;border-radius:3px;
  background:linear-gradient(90deg,rgba(var(--accent-rgb),.5),var(--accent));
  transition:width .85s cubic-bezier(.3,0,.2,1)}
.hsub{grid-area:sub;font-size:10.5px;color:var(--mute);margin-top:6px;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis}
.hrow.hot{border-color:rgba(255,120,60,.42);background:rgba(255,120,60,.07)}
.hrow.hot .hbar>i{background:linear-gradient(90deg,#ffab5e,#ff3d00);box-shadow:0 0 12px rgba(255,90,40,.75)}
.spark{margin-top:2px;background:var(--panel2);border:1px solid var(--line);border-radius:14px;padding:8px 10px 2px}
.sparkTop{display:flex;justify-content:space-between;font-size:10px;color:var(--dim);margin-bottom:3px}
.spark canvas{width:100%;display:block;border-radius:6px}
@media(max-width:560px){.tank{height:290px}.gauge{width:min(74%,230px)}}
/* ══════════ РЕЖИМЫ И КОРМЛЕНИЕ ══════════ */
.modes{display:inline-flex;flex-wrap:wrap;gap:4px;background:rgba(255,255,255,.05);
  border:1px solid var(--line);border-radius:12px;padding:4px}
.modes button{appearance:none;border:0;background:transparent;color:var(--dim);font:inherit;
  font-size:12.5px;padding:8px 13px;border-radius:9px;cursor:pointer;transition:.2s}
.modes button:hover{color:var(--fg)}
.modes button.on{background:#E9F94B;color:#15200A;font-weight:700;box-shadow:0 0 18px -4px rgba(233,249,75,.95)}
.feedBtn{width:100%;appearance:none;border:1px solid rgba(233,249,75,.45);border-radius:14px;
  background:linear-gradient(160deg,rgba(233,249,75,.15),rgba(233,249,75,.04));color:#E9F94B;
  font:inherit;font-size:12.5px;font-weight:700;letter-spacing:.08em;padding:14px 12px;cursor:pointer;
  transition:.2s}
.feedBtn:hover{background:rgba(233,249,75,.26);transform:translateY(-1px)}
.feedBtn.active{background:#E9F94B;color:#15200A;box-shadow:0 0 22px -4px rgba(233,249,75,.9)}
.feedLeft{font-size:11px;color:var(--dim);text-align:center;margin-top:6px;font-variant-numeric:tabular-nums}
.feedCard{background:var(--panel2);border:1px solid var(--line);border-radius:14px;padding:11px}
/* ══════════ ПЕРЕКЛЮЧАТЕЛИ ══════════ */
.relay{display:flex;flex-direction:column;gap:12px}
.relay .head{margin-bottom:0}
.relay .icon{width:52px;height:52px;flex:0 0 auto}
.pill{font-size:11px;font-weight:700;letter-spacing:.09em;padding:5px 11px;border-radius:999px;
  background:rgba(255,255,255,.07);color:var(--dim);border:1px solid var(--line);white-space:nowrap;transition:.3s}
.pill.on{background:rgba(34,211,127,.16);color:#67eaa5;border-color:rgba(34,211,127,.4);
  animation:pillGlow 2.6s ease-in-out infinite}
.pill.hot{background:rgba(255,120,60,.18);color:#ffa878;border-color:rgba(255,120,60,.45);
  animation:hotGlow 1.9s ease-in-out infinite}
.pill.err{background:rgba(255,77,94,.18);color:#ff9099;border-color:rgba(255,77,94,.45)}
@keyframes pillGlow{0%,100%{box-shadow:0 0 0 rgba(34,211,127,0)}50%{box-shadow:0 0 16px rgba(34,211,127,.45)}}
@keyframes hotGlow{0%,100%{box-shadow:0 0 0 rgba(255,120,60,0)}50%{box-shadow:0 0 18px rgba(255,120,60,.5)}}
.switch{position:relative;width:70px;height:38px;border-radius:999px;border:1px solid var(--line);
  background:rgba(255,255,255,.06);cursor:pointer;transition:.3s;flex:0 0 auto}
.switch::after{content:'';position:absolute;top:3px;left:3px;width:30px;height:30px;border-radius:50%;
  background:#8b9bad;transition:transform .32s cubic-bezier(.3,1.4,.4,1),background .3s}
.switch.on{background:rgba(34,211,127,.22);border-color:rgba(34,211,127,.5)}
.switch.on::after{transform:translateX(32px);background:#22d37f;box-shadow:0 0 14px #22d37f}
.switch.hot{background:rgba(255,120,60,.22);border-color:rgba(255,120,60,.55)}
.switch.hot::after{transform:translateX(32px);background:#ff7a3c;box-shadow:0 0 14px #ff7a3c}
.switch:active::after{width:34px}
/* ══════════ КНОПКИ И ПОЛЯ ══════════ */
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:flex-end}
.row+.row{margin-top:12px}
.btn{position:relative;overflow:hidden;appearance:none;border:1px solid var(--line);background:var(--panel2);
  color:var(--fg);font:inherit;font-size:13.5px;padding:10px 15px;border-radius:12px;cursor:pointer;
  transition:transform .12s ease,background .2s,border-color .2s}
.btn:hover{background:rgba(255,255,255,.13)}
.btn:active{transform:scale(.97)}
.btn.primary{background:rgba(var(--accent-rgb),.18);border-color:rgba(var(--accent-rgb),.45);color:var(--accent)}
.btn.ghost{background:transparent;color:var(--dim)}
.btn.danger{border-color:rgba(255,77,94,.4);color:#ff9099}
.btn .rip{position:absolute;border-radius:50%;transform:scale(0);background:rgba(255,255,255,.35);
  animation:ripple .6s ease-out;pointer-events:none}
@keyframes ripple{to{transform:scale(4);opacity:0}}
.seg-ctl{display:inline-flex;background:rgba(255,255,255,.05);border:1px solid var(--line);
  border-radius:12px;padding:4px;gap:4px}
.seg-ctl button{appearance:none;border:0;background:transparent;color:var(--dim);font:inherit;font-size:13px;
  padding:8px 14px;border-radius:9px;cursor:pointer;transition:.2s}
.seg-ctl button.on{background:rgba(var(--accent-rgb),.2);color:var(--accent);box-shadow:0 0 16px -6px rgba(var(--accent-rgb),.8)}
label.f{display:flex;flex-direction:column;gap:5px;font-size:11.5px;color:var(--dim)}
input[type=number],input[type=time]{background:#080f18;border:1px solid var(--line);color:var(--fg);
  border-radius:10px;padding:8px 10px;font:inherit;font-size:13.5px;width:112px;font-variant-numeric:tabular-nums}
input[type=time]{width:118px}
input:focus{outline:1px solid rgba(var(--accent-rgb),.6);border-color:transparent}
.days{display:flex;gap:7px;flex-wrap:wrap}
.day{border:1px solid var(--line);background:transparent;color:var(--dim);border-radius:10px;
  padding:8px 12px;font:inherit;font-size:12.5px;cursor:pointer;transition:.2s}
.day.on{background:var(--accent);border-color:transparent;color:#03121b;font-weight:700;
  box-shadow:0 0 18px -4px rgba(var(--accent-rgb),.85)}
.sl{display:flex;align-items:center;gap:12px;margin-top:10px;font-size:12.5px;color:var(--dim)}
.sl>span{width:86px;flex:0 0 auto}
.sl>b{width:52px;text-align:right;color:var(--fg);font-variant-numeric:tabular-nums;font-weight:600}
input[type=range]{-webkit-appearance:none;appearance:none;flex:1;height:6px;border-radius:4px;outline:none;
  background:linear-gradient(90deg,rgba(var(--accent-rgb),.85) var(--p,0%),rgba(255,255,255,.12) var(--p,0%))}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;
  background:#fff;box-shadow:0 0 12px rgba(var(--accent-rgb),.9);cursor:pointer;transition:transform .15s}
input[type=range]::-webkit-slider-thumb:active{transform:scale(1.15)}
input[type=range]::-moz-range-thumb{width:20px;height:20px;border:0;border-radius:50%;background:#fff;cursor:pointer}
.bar{height:8px;border-radius:5px;background:rgba(255,255,255,.09);overflow:hidden;margin-top:12px}
.bar>i{display:block;height:100%;width:0;border-radius:5px;background:linear-gradient(90deg,rgba(var(--accent-rgb),.5),var(--accent));
  transition:width .7s cubic-bezier(.3,0,.2,1);box-shadow:0 0 14px rgba(var(--accent-rgb),.55)}
canvas{width:100%;display:block;border-radius:14px;cursor:crosshair}
.legend{display:flex;gap:16px;flex-wrap:wrap;margin-top:10px;font-size:11.5px;color:var(--dim)}
.legend i{display:inline-block;width:16px;height:3px;border-radius:2px;margin-right:6px;vertical-align:middle}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(178px,1fr));gap:10px}
.bars{display:inline-flex;gap:3px;align-items:flex-end;height:14px;vertical-align:middle}
.bars i{width:4px;background:var(--ok);border-radius:2px;transition:height .4s ease}
.table{font-size:12.5px}
.tr{display:grid;grid-template-columns:34px 58px 1fr 1fr 1fr 1fr;gap:6px;padding:8px 6px;border-bottom:1px solid var(--line);
  font-variant-numeric:tabular-nums}
.tr.head{color:var(--mute);font-size:11px;letter-spacing:.05em;text-transform:uppercase}
.tr:last-child{border-bottom:0}
.empty{color:var(--mute);font-size:12.5px;padding:10px 4px}
/* ══════════ WI-FI ══════════ */
.nets{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}
.nets:empty{display:none}
.net{border:1px solid var(--line);background:var(--panel2);border-radius:11px;padding:9px 13px;
  font:inherit;font-size:12.5px;cursor:pointer;color:var(--fg);transition:.2s;text-align:left}
.net:hover{border-color:rgba(var(--accent-rgb),.55);background:rgba(var(--accent-rgb),.13);transform:translateY(-1px)}
.net b{font-weight:600}
.net i{color:var(--dim);font-style:normal;margin-left:7px;font-variant-numeric:tabular-nums}
/* ══════════ СВОРАЧИВАЕМЫЕ БЛОКИ ══════════ */
details.acc{padding:0;overflow:hidden}
details.acc>summary{list-style:none;cursor:pointer;display:flex;align-items:center;justify-content:space-between;
  gap:12px;padding:18px;font-size:13px;font-weight:600;letter-spacing:.12em;text-transform:uppercase;color:#c9d5e3;
  transition:background .2s}
details.acc>summary::-webkit-details-marker{display:none}
details.acc>summary:hover{background:rgba(255,255,255,.03)}
.accRight{display:inline-flex;align-items:center;gap:10px;letter-spacing:0;text-transform:none}
.chev{color:var(--dim);transition:transform .3s ease;font-size:14px}
details.acc[open] .chev{transform:rotate(180deg)}
details.acc[open]>summary{border-bottom:1px solid var(--line)}
.accBody{padding:18px}
details.acc[open]{border-color:rgba(var(--accent-rgb),.3)}
/* ══════════ ОБНОВЛЕНИЕ С GITHUB ══════════ */
.gh{border:1px solid rgba(var(--accent-rgb),.28);background:rgba(var(--accent-rgb),.055);
  border-radius:14px;padding:14px;margin-bottom:14px}
.ghHead{display:flex;flex-direction:column;gap:4px;margin-bottom:10px}
.ghHead b{font-size:13.5px;color:var(--fg)}
/* ══════════ РАСПИСАНИЕ СВЕТА ══════════ */
.srow{display:flex;flex-wrap:wrap;gap:10px;align-items:flex-end;padding:10px 0;border-bottom:1px solid var(--line)}
.srow:last-child{border-bottom:0}
.srow.off{opacity:.45}
.sday{display:flex;align-items:center;gap:7px;min-width:78px;font-size:13px;padding-bottom:8px;cursor:pointer}
.sday input{width:17px;height:17px;accent-color:var(--accent);cursor:pointer}
.sday b{font-weight:600;letter-spacing:.04em}
.sf{display:flex;flex-direction:column;gap:4px;font-size:10.5px;letter-spacing:.03em;color:var(--dim);text-transform:uppercase}
.sf input{width:112px}
.sf input[type=number]{width:96px}
/* ══════════ OTA ══════════ */
.drop{border:1.5px dashed rgba(var(--accent-rgb),.4);border-radius:14px;padding:18px;text-align:center;
  color:var(--dim);font-size:13px;cursor:pointer;transition:.25s;background:rgba(var(--accent-rgb),.04)}
.drop:hover,.drop.over{background:rgba(var(--accent-rgb),.12);border-color:var(--accent);color:var(--fg)}
.prog{height:8px;border-radius:5px;background:rgba(255,255,255,.1);margin-top:12px;overflow:hidden;display:none}
.prog.show{display:block}
.prog i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--accent),#fff);
  transition:width .3s ease;animation:shimmer 1.4s linear infinite}
@keyframes shimmer{0%{filter:brightness(.9)}50%{filter:brightness(1.35)}100%{filter:brightness(.9)}}
/* ══════════ УВЕДОМЛЕНИЯ И СВЯЗЬ ══════════ */
.toasts{position:fixed;top:14px;right:14px;z-index:60;display:flex;flex-direction:column;gap:8px;max-width:320px}
.toast{background:rgba(10,16,24,.94);border:1px solid var(--line);border-left:3px solid var(--accent);
  border-radius:12px;padding:11px 14px;font-size:13px;box-shadow:0 16px 40px -20px #000;
  animation:slideIn .35s cubic-bezier(.2,1,.3,1);backdrop-filter:var(--glass)}
.toast.ok{border-left-color:var(--ok)}
.toast.err{border-left-color:var(--danger)}
@keyframes slideIn{from{transform:translateX(120%);opacity:0}to{transform:translateX(0);opacity:1}}
.offline{position:fixed;inset:0;z-index:50;display:none;align-items:center;justify-content:center;
  background:rgba(2,5,10,.86);backdrop-filter:blur(6px);text-align:center;padding:24px}
.offline.show{display:flex}
.offline .box{max-width:380px}
.pulse{width:56px;height:56px;margin:0 auto 18px;border-radius:50%;border:2px solid var(--accent);
  animation:offPulse 1.6s ease-out infinite}
@keyframes offPulse{0%{transform:scale(.7);opacity:1}100%{transform:scale(1.35);opacity:0}}
footer{color:var(--mute);font-size:12px;text-align:center;margin-top:22px;line-height:1.8}
body.night .aurora{opacity:.32}
body.night .card{background:rgba(255,255,255,.03)}
@media(max-width:560px){.readout .big{font-size:48px}.ringwrap{width:180px;height:180px}.tank{height:250px}}
</style>
</head>
<body data-phase="day">
<div class="aurora"></div>

<div class="toasts" id="toasts"></div>
<div class="offline" id="offline"><div class="box">
  <div class="pulse"></div>
  <h2 style="font-size:16px;letter-spacing:.1em">НЕТ СВЯЗИ С ПЛАТОЙ</h2>
  <p class="note" style="margin-top:10px">Плата перезагружается или пропала сеть. Переподключаюсь автоматически…</p>
  <p class="note" id="offTimer" style="margin-top:8px">—</p>
</div></div>

<div class="wrap">
<header>
  <div class="brand"><span class="dot"></span><span id="devname">Аквариум</span></div>
  <div class="hud">
    <span class="chip" id="netChip"><span id="netText">подключение…</span></span>
    <span class="chip"><span class="clock" id="clock">--:--:--</span></span>
  </div>
</header>

<!-- ═══════════ АКВАРИУМ ═══════════ -->
<section class="card heroCard" id="heroCard">
  <div class="head">
    <h2>Аквариум</h2>
    <div class="row" style="align-items:center;gap:10px">
      <span class="hbadge" id="hbadge"><i></i><span id="hbadgeText">НОРМА</span></span>
      <span class="note" id="phaseNote">—</span>
    </div>
  </div>
  <div class="hero">
    <div class="scene">
      <div class="tank">
        <div class="lamp"><i id="lampFill"></i></div>
        <div class="bloom" id="bloom"></div>
        <div class="rays"><i></i><i></i><i></i></div>
        <div class="water" id="waterCol"></div>
        <div class="caustics"><i></i><i></i><i></i></div>
        <svg class="waves" viewBox="0 0 1200 26" preserveAspectRatio="none">
          <path class="w1" d="M0,14 Q75,0 150,14 T300,14 T450,14 T600,14 T750,14 T900,14 T1050,14 T1200,14 L1200,26 L0,26 Z"/>
          <path class="w2" d="M0,16 Q75,4 150,16 T300,16 T450,16 T600,16 T750,16 T900,16 T1050,16 T1200,16 L1200,26 L0,26 Z"/>
        </svg>
        <div class="sand"></div>
        <div class="plants"><i></i><i></i><i></i><i></i></div>
        <div class="bubbles" id="bubbles"></div>
        <svg class="fish f1" viewBox="0 0 100 46">
          <path d="M6 23c14-14 42-16 60-6 6-6 16-9 26-8-6 5-9 10-9 14s3 9 9 14c-10 1-20-2-26-8-18 10-46 8-60-6z" fill="#8fd4ff" opacity=".9"/>
          <circle cx="26" cy="20" r="2.6" fill="#0a2030"/>
        </svg>
        <svg class="fish f2" viewBox="0 0 100 46">
          <path d="M6 23c14-14 42-16 60-6 6-6 16-9 26-8-6 5-9 10-9 14s3 9 9 14c-10 1-20-2-26-8-18 10-46 8-60-6z" fill="#5fb8e8" opacity=".7"/>
          <circle cx="26" cy="20" r="2.6" fill="#0a2030"/>
        </svg>
        <div class="impeller" title="Помпа">
          <svg viewBox="0 0 48 48">
            <circle cx="24" cy="24" r="21" fill="none" stroke="rgba(255,255,255,.22)" stroke-width="2"/>
            <g class="blades">
              <path d="M24 24 L24 6 A18 18 0 0 1 40 18 Z" fill="rgba(120,220,255,.75)"/>
              <path d="M24 24 L42 24 A18 18 0 0 1 30 40 Z" fill="rgba(120,220,255,.55)"/>
              <path d="M24 24 L24 42 A18 18 0 0 1 8 30 Z" fill="rgba(120,220,255,.45)"/>
              <path d="M24 24 L6 24 A18 18 0 0 1 18 8 Z" fill="rgba(120,220,255,.6)"/>
            </g>
            <circle cx="24" cy="24" r="4.5" fill="#0b2233"/>
          </svg>
        </div>
        <div class="fan" title="Вентилятор">
          <svg viewBox="0 0 48 48"><g class="blades">
            <path d="M24 24 C14 14 8 16 8 22 C8 28 16 28 24 24Z" fill="rgba(160,210,255,.85)"/>
            <path d="M24 24 C34 14 40 16 40 22 C40 28 32 28 24 24Z" fill="rgba(160,210,255,.7)"/>
            <path d="M24 24 C14 34 16 40 22 40 C28 40 28 32 24 24Z" fill="rgba(160,210,255,.6)"/>
            <path d="M24 24 C34 34 32 40 26 40 C20 40 20 32 24 24Z" fill="rgba(160,210,255,.5)"/>
          </g><circle cx="24" cy="24" r="3.4" fill="#0b2233"/></svg>
        </div>
        <div class="heatwaves" id="heatwaves"></div>
        <div class="heater" title="Подогреватель"><i></i></div>

        <!-- центральный индикатор температуры: дуга 270° -->
        <div class="gauge">
          <svg viewBox="0 0 200 200">
            <defs>
              <linearGradient id="gArc" x1="0" y1="1" x2="1" y2="0">
                <stop offset="0" stop-color="#38bdf8"/><stop offset=".55" stop-color="#22d3ee"/><stop offset="1" stop-color="#a3e635"/>
              </linearGradient>
            </defs>
            <g transform="rotate(135 100 100)">
              <circle class="gTrack" cx="100" cy="100" r="80"/>
              <circle class="gBand"  id="gBand" cx="100" cy="100" r="80"/>
              <circle class="gVal"   id="gVal"  cx="100" cy="100" r="80"/>
            </g>
            <g id="gTicks"></g>
            <circle class="gDotGlow" id="gDotGlow" cx="100" cy="165" r="10"/>
            <circle class="gDot" id="gDot" cx="100" cy="165" r="4.5"/>
          </svg>
          <div class="gRead">
            <div class="gTemp"><b id="tempVal">--</b><span>°C</span></div>
            <div class="gPhase" id="tempSub">—</div>
            <div class="gTrend" id="tempTrend">—</div>
            <div class="gGoal" id="tempTiny">—</div>
          </div>
        </div>
      </div>
      <div class="daybar" id="daybar" style="margin-top:14px"><div class="now" id="nowMark" style="left:0%"></div></div>
    </div>

    <div class="hside">
      <div class="hrow">
        <span class="hico">🌡</span>
        <div class="hbody"><span class="hk">Воздух</span><span class="hv" id="h-air">—</span></div>
        <div class="hbar"><i id="airBar"></i></div>
      </div>
      <div class="hrow" id="rowHeat">
        <span class="hico">🔥</span>
        <div class="hbody"><span class="hk">Нагрев</span><span class="hv" id="h-heat">—</span></div>
        <div class="hbar"><i id="heatBar2"></i></div>
        <span class="hsub" id="h-heatSub">—</span>
      </div>
      <div class="hrow">
        <span class="hico">💡</span>
        <div class="hbody"><span class="hk">Свет</span><span class="hv" id="h-light">—</span></div>
        <div class="hbar"><i id="lightBar"></i></div>
        <span class="hsub" id="h-lightSub">—</span>
      </div>
      <div class="feedCard">
        <button class="feedBtn" id="heroFeed" onclick="startFeed()">КОРМЛЕНИЕ</button>
        <div class="feedLeft" id="feedLeft">—</div>
      </div>
      <div class="spark">
        <canvas id="spark" height="56"></canvas>
        <div class="sparkTop"><span>вода, последний час</span><span id="sparkInfo">—</span></div>
      </div>
    </div>
  </div>
</section>

<!-- ═══════════ УПРАВЛЕНИЕ ═══════════ -->
<section class="grid">
  <div class="card relay">
    <div class="head"><h2>Помпа</h2><span class="pill" id="p-pump">—</span></div>
    <p class="note">Реле 1 · фильтрация и перемешивание воды</p>
    <div class="row" style="justify-content:space-between;align-items:center">
      <div class="switch" id="sw-pump" onclick="tgl('pump')"></div>
      <span class="note">GPIO4</span>
    </div>
    <div class="row" style="margin-top:12px;align-items:flex-end">
      <label class="f">Остановка на кормление, мин<input type="number" id="feedmin" min="1" max="120" step="1" style="width:96px"></label>
      <button class="btn" onclick="applyFeedMin()">Применить</button>
    </div>
    <div style="margin-top:12px">
      <button class="feedBtn" id="feedBtn" onclick="startFeed()">КОРМЛЕНИЕ</button>
      <div class="feedLeft" id="feedLeftCard">—</div>
    </div>
  </div>

  <div class="card relay">
    <div class="head"><h2>Компрессор</h2><span class="pill" id="p-air">—</span></div>
    <p class="note">Реле 2 · аэрация аквариума</p>
    <div class="row" style="justify-content:space-between;align-items:center">
      <div class="switch" id="sw-air" onclick="tgl('air')"></div>
      <span class="note">GPIO5</span>
    </div>
  </div>

  <div class="card relay">
    <div class="head"><h2>Вентилятор</h2><span class="pill" id="p-fan">—</span></div>
    <p class="note">Реле 3 · охлаждение при перегреве</p>
    <div class="row" style="margin-top:4px">
      <div class="modes">
        <button id="fan-auto" onclick="cmd('fanmode=auto','вентилятор: авто')">Авто</button>
        <button id="fan-on"   onclick="cmd('fanmode=on','вентилятор принудительно включён')">Вкл</button>
        <button id="fan-off"  onclick="cmd('fanmode=off','вентилятор принудительно выключен')">Выкл</button>
      </div>
    </div>
    <div class="row">
      <label class="f">Включать выше, °C<input type="number" id="fanon" step="0.1" min="15" max="40"></label>
      <label class="f">Гистерезис<input type="number" id="fanhy" step="0.1" min="0.1" max="2"></label>
      <button class="btn primary" onclick="applyFan()">Применить</button>
    </div>
  </div>

  <div class="card relay">
    <div class="head"><h2>Подогреватель</h2><span class="pill" id="p-heat">—</span></div>
    <p class="note">Реле 4 · термостат держит температуру сам</p>
    <div class="row" style="margin-top:4px">
      <div class="modes">
        <button id="heat-auto" onclick="cmd('heatmode=auto','нагрев: авто')">Авто</button>
        <button id="heat-on"   onclick="cmd('heatmode=on','нагрев вручную на 15 минут')">15 мин</button>
        <button id="heat-off"  onclick="cmd('heatmode=off','нагрев принудительно выключен')">Выкл</button>
      </div>
    </div>
    <p class="note" id="heatModeNote" style="margin-top:10px">—</p>
  </div>

  <!-- термостат -->
  <div class="card wide">
    <div class="head"><h2>Термостат</h2><span class="note" id="thermoNote">—</span></div>
    <div class="row" style="align-items:center;gap:26px">
      <svg width="220" height="130" viewBox="0 0 220 130" id="gauge" style="flex:0 0 auto">
        <defs>
          <linearGradient id="gg" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0" stop-color="#3d8bff"/><stop offset=".5" stop-color="var(--accent)"/><stop offset="1" stop-color="#ff7a3c"/>
          </linearGradient>
        </defs>
        <path d="M20 116 A90 90 0 0 1 200 116" fill="none" stroke="rgba(255,255,255,.10)" stroke-width="12" stroke-linecap="round"/>
        <path id="gaugeBand" d="M20 116 A90 90 0 0 1 200 116" fill="none" stroke="rgba(34,211,127,.35)" stroke-width="12" stroke-linecap="butt"/>
        <path id="gaugeVal" d="M20 116 A90 90 0 0 1 200 116" fill="none" stroke="url(#gg)" stroke-width="12" stroke-linecap="round"/>
        <line id="needle" x1="110" y1="116" x2="110" y2="40" stroke="#fff" stroke-width="2.6" stroke-linecap="round" style="transform-origin:110px 116px;transition:transform .6s cubic-bezier(.3,0,.2,1)"/>
        <circle cx="110" cy="116" r="6" fill="#fff"/>
        <text id="gaugeText" x="110" y="92" text-anchor="middle" fill="#e8eef6" font-size="26" font-weight="300">--</text>
        <text x="20" y="128" fill="#4d5b6b" font-size="10">15°</text>
        <text x="196" y="128" fill="#4d5b6b" font-size="10">35°</text>
      </svg>
      <div style="flex:1 1 300px">
        <div class="row">
          <label class="f">Уставка, °C<input type="number" id="sp" step="0.1" min="15" max="35"></label>
          <label class="f">Гистерезис, °C<input type="number" id="hy" step="0.1" min="0.1" max="2"></label>
          <label class="f">Аварийный порог<input type="number" id="maxt" step="0.5" min="20" max="40"></label>
          <label class="f">Мощность, Вт<input type="number" id="pw" step="5" min="5" max="500"></label>
          <button class="btn primary" onclick="applyHeat()">Применить</button>
        </div>
        <div class="row" style="margin-top:14px">
          <button class="btn" onclick="bumpSet(-0.1)">−0.1</button>
          <button class="btn" onclick="bumpSet(0.1)">+0.1</button>
          <button class="btn ghost" onclick="bumpSet(-0.5)">−0.5</button>
          <button class="btn ghost" onclick="bumpSet(0.5)">+0.5</button>
          <span class="note" id="heatState" style="align-self:center"></span>
        </div>
        <div class="bar"><i id="heatBar"></i></div>
      </div>
    </div>
  </div>

  <!-- светильник -->
  <div class="card wide">
    <div class="head">
      <h2>Светильник</h2>
      <div class="row" style="align-items:center">
        <div class="seg-ctl">
          <button id="lm-auto" onclick="cmd('lmode=auto','свет: авто')">Авто</button>
          <button id="lm-night" onclick="cmd('lmode=night','свет: ночник')">Ночник</button>
          <button id="lm-man" onclick="cmd('lmode=manual','свет: ручной')">Ручной</button>
        </div>
        <span class="pill" id="p-light">—</span>
      </div>
    </div>
    <p class="note" id="todaySched">—</p>
    <div class="row" style="align-items:center;margin-top:12px;gap:14px">
      <button class="btn primary" onclick="openSched()">Настроить расписание</button>
      <span class="note" id="daysNote">—</span>
    </div>
    <div id="chans"></div>
    <p class="note" id="nlen" style="margin-top:12px">—</p>
  </div>

  <!-- расписание света по дням недели (сворачиваемое) -->
  <details class="card wide acc" id="accSched">
    <summary>
      <span class="accTitle">Расписание света</span>
      <span class="accRight"><span class="note" id="schedNote">по дням</span><span class="chev">▾</span></span>
    </summary>
    <div class="accBody">
      <div id="schedRows"></div>
      <div class="row" style="margin-top:16px">
        <button class="btn primary" onclick="saveSched()">Сохранить расписание</button>
        <button class="btn ghost" onclick="syncSched(true)">Вернуть как было</button>
        <button class="btn ghost" onclick="copyDay(1)">Скопировать Пн во все дни</button>
        <button class="btn ghost" onclick="copyDay(6,true)">Будни одним днём</button>
        <span class="note" id="schedState">—</span>
      </div>
      <p class="note" style="margin-top:12px">
        У каждого дня свои интервалы: <b>рассвет</b> — когда свет включается, <b>закат</b> — когда
        начинается плавное выключение, <b>ночник до</b> — время, когда ночник гаснет полностью
        (если поставить время заката, ночник не включится). <b>Переход</b> — сколько минут занимает
        рассвет и закат. Снимите галочку — в этот день свет не включится вовсе.
        Изменения вступают в силу после кнопки «Сохранить расписание».
      </p>
    </div>
  </details>

  <!-- график -->
  <div class="card wide">
    <div class="head">
      <h2>История температур</h2>
      <div class="seg-ctl">
        <button class="on" id="rg0" onclick="setRange(0)">1 ч</button>
        <button id="rg1" onclick="setRange(1)">6 ч</button>
        <button id="rg2" onclick="setRange(2)">24 ч</button>
      </div>
    </div>
    <canvas id="chart" height="190"></canvas>
    <div class="legend">
      <span><i style="background:var(--accent)"></i>вода</span>
      <span><i style="background:#5b7cff"></i>воздух</span>
      <span><i style="background:rgba(255,120,60,.85)"></i>нагрев</span>
      <span><i style="background:rgba(255,214,120,.8)"></i>свет</span>
      <span id="chartInfo"></span>
    </div>
  </div>

  <!-- циклы -->
  <div class="card wide">
    <div class="head"><h2>Журнал циклов нагрева</h2><span class="note" id="cycNote">—</span></div>
    <div class="table" id="cyc"></div>
  </div>

  <!-- Wi-Fi (свёрнуто, в самом низу) -->
  <details class="card wide acc" id="accWifi">
    <summary>
      <span class="accTitle">Wi-Fi</span>
      <span class="accRight"><span class="pill" id="w-pill">—</span><span class="chev">▾</span></span>
    </summary>
    <div class="accBody">
      <p class="note" id="w-now">—</p>
      <div class="row">
        <label class="f" style="flex:1 1 190px">Сеть (SSID)
          <input type="text" id="w-ssid" placeholder="имя сети" autocomplete="off" spellcheck="false" style="width:100%">
        </label>
        <label class="f" style="flex:1 1 190px">Пароль
          <input type="password" id="w-pass" placeholder="пароль сети" autocomplete="new-password" style="width:100%">
        </label>
        <button class="btn primary" onclick="wifiConnect()">Подключить</button>
        <button class="btn" onclick="wifiScan()">Найти сети</button>
        <button class="btn ghost danger" onclick="wifiForget()">Забыть сеть</button>
      </div>
      <div id="w-list" class="nets"></div>
      <p class="note" style="margin-top:12px">
        В списке только сети <b>2,4 ГГц</b> — те, которые плата видит сама. Сеть и пароль сохраняются
        в энергонезависимой памяти: перепрошивать плату при смене роутера больше не нужно.
        После успешного подключения точка доступа «Aquarium» погаснет через 30 секунд — плата
        продолжит работу в вашей сети, новый адрес появится в этом же блоке.
      </p>
    </div>
  </details>

  <!-- диагностика и обновление (свёрнуто, в самом низу) -->
  <details class="card wide acc" id="accDiag">
    <summary>
      <span class="accTitle">Диагностика и обновление</span>
      <span class="accRight"><span class="note" id="diagNote">—</span><span class="chev">▾</span></span>
    </summary>
    <div class="accBody">
      <div class="tiles" id="diag"></div>

      <div class="gh" id="ghBox">
        <div class="ghHead">
          <b>Обновление прошивки с GitHub</b>
          <span class="note" id="ghInfo">нажмите «Проверить обновление» — плата сама скачает свежий файл</span>
        </div>
        <div class="row">
          <button class="btn primary" onclick="ghCheck()">Проверить обновление</button>
          <button class="btn" id="ghRun" onclick="ghRun()" disabled>Обновить с GitHub</button>
          <span class="note" id="ghUrl"></span>
        </div>
        <div class="prog" id="ghProg"><i id="ghProgFill"></i></div>
      </div>

      <div class="gh" id="widgetBox">
        <div class="ghHead">
          <b>Виджет для Android</b>
          <span class="note" id="widgetInfo">квадрат 2×3 на главном экране: температура, уставка, нагрев и свет</span>
        </div>
        <div class="row">
          <a class="btn primary" href="/api/widget.apk" download="aquarium-widget.apk">Скачать виджет (APK)</a>
          <a class="btn" id="bindLink" href="#" onclick="return bindWidget()">Привязать к этой плате</a>
          <span class="note" id="widgetNote">—</span>
        </div>
        <ol class="note" style="margin-top:10px;padding-left:20px;line-height:1.7">
          <li>Скачайте APK и установите — Android спросит разрешение на установку из браузера.</li>
          <li>Вернитесь на эту страницу и нажмите <b>«Привязать к этой плате»</b> — приложение получит адрес платы автоматически.</li>
          <li>В приложении нажмите <b>«Добавить виджет»</b> и подтвердите — виджет 2×3 появится на главном экране.</li>
          <li>Нажатие на виджет открывает эту страницу, кнопка ⟳ на виджете обновляет данные.</li>
        </ol>
      </div>

      <div class="row" style="margin-top:14px">
        <button class="btn ghost" onclick="cmd('timesync=1','запрос времени у NTP')">Синхронизировать время</button>
        <button class="btn ghost" onclick="cmd('scan=1','поиск датчиков')">Найти датчики</button>
        <button class="btn ghost" onclick="showSensors()">Адреса датчиков</button>
        <button class="btn" onclick="setTimeFromBrowser()">Взять время с этого устройства</button>
        <button class="btn ghost" onclick="cmd('conf=1','настройки сохранены')">Сохранить настройки</button>
        <button class="btn ghost danger" onclick="if(confirm('Сбросить настройки к заводским?'))cmd('reset=1','сброс настроек')">Сброс</button>
        <button class="btn ghost danger" onclick="if(confirm('Перезагрузить плату?'))cmd('reboot=1','перезагрузка')">Перезагрузка</button>
      </div>
    </div>
  </details>
</section>

<footer>
  <div id="footInfo">—</div>
  <div class="note" style="margin-top:6px">ESP32-S3 · живая телеметрия по SSE · страница хранится в памяти платы, интернет не нужен</div>
</footer>
</div>

<script>
var $=function(id){return document.getElementById(id)};
var S=null,H=null,evt=null,lastOk=0,rangeIdx=0,ranges=[3600,21600,86400],firstRender=true;
var WD=['Вс','Пн','Вт','Ср','Чт','Пт','Сб'];
var PH={night:'Ночь',dawn:'Рассвет',day:'День',dusk:'Закат',manual:'Ручной свет'};
var builtChans=false,builtDiag=false,bubbleEls=false;
var schedBuilt=false,schedDirty=false,schedSv=-1;
var schedPending=false,schedSvAtSave=-1;

/* ---------- утилиты ---------- */
function nf(v,d){ if(v===null||v===undefined||isNaN(v))return '—'; return Number(v).toFixed(d===undefined?1:d); }
function setv(el,v){ if(el&&document.activeElement!==el) el.value=v; }
function cls(id,base,on){ var e=$(id); if(e) e.className=base+(on?' on':''); }

function toast(text,kind){
  if(!text) return;
  var t=document.createElement('div');
  t.className='toast '+(kind||'');
  t.textContent=text;
  $('toasts').appendChild(t);
  setTimeout(function(){ t.style.transition='opacity .4s,transform .4s'; t.style.opacity='0'; t.style.transform='translateX(120%)';
    setTimeout(function(){ t.remove(); },420); },2600);
}

function cmd(qs,msg){
  fetch('/api/cmd?'+qs,{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
    if(msg) toast(msg, j&&j.ok?'ok':'err');
  }).catch(function(){ toast('нет связи с платой','err'); });
}
function tgl(k){ cmd(k+'=toggle'); }

/* ---------- приём данных ---------- */
function connect(){
  if(evt) try{ evt.close(); }catch(e){}
  evt=new EventSource('/events');
  evt.onmessage=function(e){
    try{ S=JSON.parse(e.data); }catch(err){ return; }
    lastOk=Date.now();
    $('offline').classList.remove('show');
    render();
  };
  evt.onerror=function(){ /* EventSource переподключится сам */ };
}
function poll(){
  fetch('/api/state',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
    S=j; lastOk=Date.now(); $('offline').classList.remove('show'); render();
  }).catch(function(){});
}
function fetchHistory(){
  fetch('/api/history?win='+ranges[rangeIdx],{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(h){ H=h; drawChart(); drawSpark(); }).catch(function(){});
}
function setRange(i){
  rangeIdx=i;
  for(var k=0;k<3;k++) $('rg'+k).className=(k===i?'on':'');
  fetchHistory();
}

/* ---------- Wi-Fi: настройка со страницы ---------- */
function wifiConnect(){
  var s=$('w-ssid').value.trim(), p=$('w-pass').value;
  if(!s){ toast('укажите имя сети','err'); return; }
  toast('сохраняю сеть «'+s+'»');
  fetch('/api/wifi?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p),{cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(j){
      if(j&&j.ok){ toast('плата переключается на «'+s+'» — смотрите статус выше','ok'); $('w-pass').value=''; }
      else toast('не удалось: '+((j&&j.err)||'ошибка'),'err');
    }).catch(function(){ toast('нет связи с платой','err'); });
}
function wifiForget(){
  if(!confirm('Удалить сохранённую сеть? Плата вернётся к настройкам из config.h.')) return;
  fetch('/api/wifi?forget=1',{cache:'no-store'}).then(function(r){return r.json()}).then(function(){
    toast('сохранённая сеть удалена','ok');
  }).catch(function(){ toast('нет связи с платой','err'); });
}
function wifiScan(){
  $('w-list').innerHTML='<span class="note">сканирую 2,4 ГГц…</span>';
  toast('сканирую эфир…');
  fetch('/api/scanwifi',{cache:'no-store'}).then(function(r){return r.text()}).then(function(txt){
    var lines=txt.split('\n'), h='';
    for(var i=0;i<lines.length;i++){
      if(!lines[i]) continue;
      var p=lines[i].split('|'); if(p.length<3) continue;
      var rssi=p[0], enc=(p[1]==='1'), ssid=p.slice(2).join('|');
      var lvl=Math.max(1,Math.min(4,Math.round((Number(rssi)+95)/12)));
      h+='<button class="net" data-ssid="'+ssid.replace(/"/g,'&quot;')+'" onclick="pickNet(this)">'+
         '<b>'+ssid+'</b><i>'+rssi+' дБм'+(enc?' · пароль':' · открытая')+'</i></button>';
    }
    $('w-list').innerHTML=h||'<span class="note">сети 2,4 ГГц не найдены</span>';
    if(!h) toast('сети 2,4 ГГц не найдены','err');
  }).catch(function(){ $('w-list').innerHTML=''; toast('не удалось просканировать','err'); });
}
function pickNet(btn){
  $('w-ssid').value=btn.getAttribute('data-ssid');
  $('w-pass').focus();
}

/* ---------- обновление прошивки с GitHub ---------- */
var ghSize=0, ghVer='', ghNewer=false;
function ghStream(url,onLine){
  fetch(url,{cache:'no-store'}).then(function(r){
    if(!r.body||!r.body.getReader){ return r.text().then(function(t){ t.split('\n').forEach(onLine); }); }
    var rd=r.body.getReader(), dec=new TextDecoder(), buf='';
    function pump(){
      return rd.read().then(function(res){
        if(res.done) return;
        buf+=dec.decode(res.value,{stream:true});
        var i;
        while((i=buf.indexOf('\n'))>=0){ var line=buf.slice(0,i); buf=buf.slice(i+1); if(line) onLine(line); }
        return pump();
      });
    }
    return pump();
  }).catch(function(){ toast('нет связи с платой','err'); });
}
function ghSummary(){
  var found = (ghVer && ghVer!=='?') ? ('v'+ghVer) : 'версия не указана';
  var rel = ghNewer ? ' — новее вашей v'+(S?S.fw:'') : ' — у вас та же версия v'+(S?S.fw:'');
  return 'на GitHub: '+found+rel;
}
function ghCheck(){
  $('ghInfo').textContent='проверяю GitHub…';
  $('ghRun').disabled=true; ghSize=0; ghVer=''; ghNewer=false;
  var sizeTxt='';
  ghStream('/api/updcheck',function(line){
    var c=line.charAt(0), v=line.slice(2);
    if(c==='S') $('ghInfo').textContent=v;
    else if(c==='V'){ ghVer=v; }
    else if(c==='N'){ ghNewer=(v==='1'); }
    else if(c==='I'){ sizeTxt=' · '+v.replace('размер прошивки ',''); $('ghInfo').textContent=ghSummary()+sizeTxt; }
    else if(c==='D'){
      $('ghInfo').textContent=ghSummary()+sizeTxt+' — можно обновляться';
      $('ghRun').disabled=false;
      toast(ghVer==='?'?'файл прошивки доступен':('на GitHub версия v'+ghVer+(ghNewer?' — новее установленной':' — у вас актуальная')),'ok');
    }
    else if(c==='E'){ $('ghInfo').textContent=v; toast(v,'err'); }
  });
}
function ghRun(){
  if(!confirm('Скачать прошивку с GitHub и прошить плату? Плата перезагрузится.')) return;
  $('ghProg').classList.add('show'); $('ghProgFill').style.width='0%';
  $('ghRun').disabled=true;
  $('ghInfo').textContent='скачиваю прошивку…';
  ghStream('/api/updrun',function(line){
    var c=line.charAt(0), v=line.slice(2);
    if(c==='S') $('ghInfo').textContent=v;
    else if(c==='I') $('ghInfo').textContent='скачиваю '+v;
    else if(c==='P'){ var p=parseInt(v,10)||0; $('ghProgFill').style.width=p+'%'; $('ghInfo').textContent='загрузка '+p+'%'; }
    else if(c==='D'){ $('ghProgFill').style.width='100%'; $('ghInfo').textContent=v; toast('прошивка обновлена — плата перезагружается','ok'); }
    else if(c==='E'){ $('ghProg').classList.remove('show'); $('ghInfo').textContent=v; $('ghRun').disabled=false; toast(v,'err'); }
  });
}
function showSensors(){
  fetch('/api/scan',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
    var t='Датчиков 1-Wire: '+j.n+'\n';
    for(var i=0;i<j.dev.length;i++){
      t+=j.dev[i]+(i===j.water?'   ← вода':(i===j.air?'   ← воздух':''))+'\n';
    }
    toast('Датчиков: '+j.n,'ok');
    alert(t+'\nСменить датчик воды: /api/cmd?water=0 (индекс из списка)');
  }).catch(function(){ toast('не удалось получить список','err'); });
}
function applyHeat(){ cmd('set='+$('sp').value+'&hyst='+$('hy').value+'&maxt='+$('maxt').value+'&pw='+$('pw').value,'термостат обновлён'); }
function applyFan(){ cmd('fanon='+$('fanon').value+'&fanhy='+$('fanhy').value,'вентилятор обновлён'); }

/* ---------- расписание света по дням ---------- */
/* порядок показа — с понедельника; 0 в данных платы это воскресенье (tm_wday) */
var DAY_ORDER=[1,2,3,4,5,6,0];
function openSched(){
  var d=$('accSched');
  if(d){ d.open=true; d.scrollIntoView({behavior:'smooth',block:'center'}); }
}
function buildSched(){
  var h='';
  for(var i=0;i<7;i++){
    var d=DAY_ORDER[i];
    h+='<div class="srow" id="srow'+d+'">'
      +'<label class="sday"><input type="checkbox" id="sen'+d+'" onchange="schedTouched()"><b>'+WD[d]+'</b></label>'
      +'<label class="sf">Рассвет<input type="time" id="son'+d+'" onchange="schedTouched()"></label>'
      +'<label class="sf">Закат<input type="time" id="soff'+d+'" onchange="schedTouched()"></label>'
      +'<label class="sf">Ночник до<input type="time" id="snoff'+d+'" onchange="schedTouched()"></label>'
      +'<label class="sf">Переход, мин<input type="number" min="1" max="180" id="sramp'+d+'" onchange="schedTouched()"></label>'
      +'</div>';
  }
  $('schedRows').innerHTML=h;
  schedBuilt=true;
  schedSv=-1;
}
function setTime(id,v){ var el=$(id); if(el&&document.activeElement!==el) el.value=v||'00:00'; }
function syncSched(force){
  if(!S||!S.sched) return;
  if(!schedBuilt) buildSched();
  if(!force){
    /* пока плата не подтвердила сохранение, поля не трогаем — иначе они «откатятся» */
    if(schedPending && S.sv===schedSvAtSave) return;
    if(!schedDirty && schedSv===S.sv) return;
  }
  for(var d=0;d<7;d++){
    var e=S.sched[d]; if(!e) continue;
    var cb=$('sen'+d); if(cb) cb.checked=!!e[0];
    setTime('son'+d,e[1]); setTime('soff'+d,e[2]); setTime('snoff'+d,e[3]);
    var r=$('sramp'+d); if(r&&document.activeElement!==r) r.value=e[4];
    var row=$('srow'+d); if(row) row.className='srow'+(e[0]?'':' off');
  }
  schedSv=S.sv;
  if(schedPending){ schedPending=false; $('schedState').textContent='сохранено на плате'; }
  if(force){ schedDirty=false; $('schedState').textContent=''; }
}
function schedTouched(){
  schedDirty=true;
  $('schedState').textContent='есть несохранённые изменения';
  for(var d=0;d<7;d++){
    var cb=$('sen'+d), row=$('srow'+d);
    if(cb&&row) row.className='srow'+(cb.checked?'':' off');
  }
}
function hm(v){ return String(v||'00:00').replace(':',''); }
/* Проверка: плата должна вернуть новый sv. Если не вернула — честно говорим об этом. */
function verifySched(){
  if(!S) return;
  if(S.sv!==schedSvAtSave){ schedPending=false; $('schedState').textContent='сохранено на плате'; return; }
  schedPending=false;
  $('schedState').textContent='плата не подтвердила — перечитываю';
  toast('плата не подтвердила расписание, перечитываю','err');
  poll();
}
function saveSched(){
  if(!S||!S.sched){ toast('нет данных с платы','err'); return; }
  var parts=[];
  for(var d=0;d<7;d++){
    var en=($('sen'+d)&&$('sen'+d).checked)?1:0;
    var rp=parseInt($('sramp'+d).value,10); if(!rp||rp<1) rp=40; if(rp>180) rp=180;
    parts.push(en+','+hm($('son'+d).value)+','+hm($('soff'+d).value)+','+hm($('snoff'+d).value)+','+rp);
  }
  $('schedState').textContent='сохраняю…';
  fetch('/api/cmd?sched='+encodeURIComponent(parts.join(';')),{cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(j){
      if(j&&j.sched===1){
        schedDirty=false; schedPending=true; schedSvAtSave=(S?S.sv:-1);
        $('schedState').textContent='принято платой, проверяю…';
        toast('расписание сохранено','ok');
        setTimeout(verifySched,3000);
      } else if(j&&j.sched===0){
        schedDirty=true;
        $('schedState').textContent='плата отклонила формат';
        toast('плата отклонила расписание — проверьте поля','err');
      } else {
        $('schedState').textContent='плата не ответила';
        toast('плата не ответила','err');
      }
    }).catch(function(){ $('schedState').textContent='нет связи'; toast('нет связи с платой','err'); });
}
function copyDay(from,weekdaysOnly){
  if(!$('son'+from)) return;
  var on=$('son'+from).value, off=$('soff'+from).value, nf=$('snoff'+from).value,
      rp=$('sramp'+from).value, en=$('sen'+from).checked;
  for(var d=0;d<7;d++){
    if(d===from) continue;
    if(weekdaysOnly && (d===0||d===6)) continue;   /* 0 — Вс, 6 — Сб */
    $('son'+d).value=on; $('soff'+d).value=off; $('snoff'+d).value=nf;
    $('sramp'+d).value=rp; $('sen'+d).checked=en;
  }
  schedTouched();
  toast(weekdaysOnly?'будни заполнены по '+WD[from]:('день '+WD[from]+' скопирован на все дни'));
}
function bumpSet(d){
  if(!S) return;
  var v=(Number(S.sp)+d).toFixed(1);
  $('sp').value=v;
  cmd('set='+v,'уставка '+v+' °C');
}

/* ---------- скелет интерфейса ---------- */
/* деления на дуге индикатора + буфер температур для тренда и мини-графика */
function buildTicks(){
  var g=$('gTicks'); if(!g) return;
  var h='';
  for(var i=0;i<=12;i++){
    var th=(135+i*270/12)*Math.PI/180;
    var x1=(100+73*Math.cos(th)).toFixed(1), y1=(100+73*Math.sin(th)).toFixed(1);
    var x2=(100+66*Math.cos(th)).toFixed(1), y2=(100+66*Math.sin(th)).toFixed(1);
    h+='<line x1="'+x1+'" y1="'+y1+'" x2="'+x2+'" y2="'+y2+'" stroke="rgba(255,255,255,.17)" stroke-width="1.6" stroke-linecap="round"/>';
  }
  g.innerHTML=h;
}
var tempHist=[], lastSpark=0;
function pushTemp(v){
  if(v===null||v===undefined||isNaN(v)||v<=0) return;
  tempHist.push(Number(v));
  if(tempHist.length>900) tempHist.shift();
}
function tempTrend(){
  if(tempHist.length<40) return 'накапливаю данные…';
  var now=tempHist[tempHist.length-1], old=tempHist[Math.max(0,tempHist.length-300)];
  var d=now-old;
  if(Math.abs(d)<0.03) return '→ держится ровно';
  return (d>0?'▲ +':'▼ ')+d.toFixed(2)+' °C за 5 мин';
}
function drawSpark(){
  var c=$('spark'); if(!c||!H||!H.n||!H.t) return;
  var r=c.getBoundingClientRect();
  if(!r.width) return;
  var dpr=Math.min(2,window.devicePixelRatio||1);
  c.width=Math.round(r.width*dpr); c.height=Math.round(56*dpr);
  var ctx=c.getContext('2d'); ctx.setTransform(dpr,0,0,dpr,0,0);
  var W=r.width, Hh=56;
  ctx.clearRect(0,0,W,Hh);
  var take=Math.min(H.n,240), t=[];
  for(var i=H.n-take;i<H.n;i++) t.push(H.t[i]/100);
  var mn=Math.min.apply(null,t), mx=Math.max.apply(null,t);
  if(!isFinite(mn)||!isFinite(mx)) return;
  var pad=(mx-mn)*0.28+0.06; var lo=mn-pad, hi=mx+pad;
  function X(i){ return 2+(W-4)*i/Math.max(1,t.length-1); }
  function Y(v){ return 5+(Hh-10)*(1-(v-lo)/(hi-lo)); }
  var acc=(getComputedStyle(document.body).getPropertyValue('--accent').trim()||'#00e5ff');
  var grad=ctx.createLinearGradient(0,0,0,Hh);
  grad.addColorStop(0,acc+'66'); grad.addColorStop(1,acc+'00');
  ctx.beginPath(); ctx.moveTo(X(0),Hh);
  for(var k=0;k<t.length;k++) ctx.lineTo(X(k),Y(t[k]));
  ctx.lineTo(X(t.length-1),Hh); ctx.closePath(); ctx.fillStyle=grad; ctx.fill();
  ctx.beginPath();
  for(var k2=0;k2<t.length;k2++){ if(k2===0) ctx.moveTo(X(k2),Y(t[k2])); else ctx.lineTo(X(k2),Y(t[k2])); }
  ctx.strokeStyle=acc; ctx.lineWidth=1.8; ctx.lineJoin='round'; ctx.stroke();
  if(S&&S.sp){ ctx.setLineDash([3,3]); ctx.strokeStyle='rgba(255,255,255,.28)'; ctx.lineWidth=1;
    ctx.beginPath(); ctx.moveTo(0,Y(S.sp)); ctx.lineTo(W,Y(S.sp)); ctx.stroke(); ctx.setLineDash([]); }
  $('sparkInfo').textContent=mn.toFixed(1)+'…'+mx.toFixed(1)+' °C';
}

/* ---------- кормление ---------- */
function startFeed(){
  if(!S) return;
  if((S.feed||0)>0){ cmd('fstop=1','пауза кормления отменена'); return; }
  var m=S.fmin||10;
  cmd('feed='+m,'кормление: помпа остановлена на '+m+' мин');
}
function applyFeedMin(){
  var m=parseInt($('feedmin').value,10);
  if(!m||m<1||m>120){ toast('время от 1 до 120 минут','err'); return; }
  cmd('fset='+m,'пауза кормления: '+m+' мин');
}
function buildChans(){
  var h='';
  for(var c=0;c<(S?S.nch:1);c++){
    var name=(S&&S.labels&&S.labels[c])?S.labels[c]:('Канал '+(c+1));
    h+='<div class="bar" style="margin-top:16px"><i id="bar'+c+'"></i></div>'+
       '<div class="sl"><span>День, %</span><b id="vday'+c+'">0</b>'+
       '<input type="range" min="0" max="100" id="day'+c+'" oninput="sliderUi(\'day'+c+'\',\'vday'+c+'\')" '+
       'onchange="cmd(\'lday='+c+',\'+this.value,\''+name+': день \'+this.value+\'%\')"></div>'+
       '<div class="sl"><span>Ночник, %</span><b id="vnight'+c+'">0</b>'+
       '<input type="range" min="0" max="100" id="night'+c+'" oninput="sliderUi(\'night'+c+'\',\'vnight'+c+'\')" '+
       'onchange="cmd(\'lnight='+c+',\'+this.value,\''+name+': ночник \'+this.value+\'%\')"></div>'+
       '<div class="sl"><span>Сейчас, %</span><b id="vman'+c+'">0</b>'+
       '<input type="range" min="0" max="100" id="man'+c+'" oninput="sliderUi(\'man'+c+'\',\'vman'+c+'\')" '+
       'onchange="cmd(\'lch='+c+'&lval=\'+this.value,\'яркость \'+this.value+\'%\')"></div>';
  }
  $('chans').innerHTML=h;
}
function sliderUi(id,vid){
  var e=$(id); if(!e) return;
  $(vid).textContent=e.value;
  e.style.setProperty('--p', e.value+'%');
}
function buildDiag(){
  var t=[['Адрес интерфейса','ip'],['Сеть','ssid'],['Шлюз','gw'],['Сигнал','rssi'],
         ['Имя в сети','host'],['Время работы','up'],['Часы','time'],['Датчики','sen'],
         ['Память настроек','nvm'],['Прошивка','fw'],['Свободно RAM / PSRAM','mem'],
         ['Запросов с сайта','req'],['Последняя перезагрузка','rst']];
  var h='';
  for(var i=0;i<t.length;i++) h+='<div class="tile small"><span class="k">'+t[i][0]+'</span><span class="v" id="d-'+t[i][1]+'">—</span></div>';
  $('diag').innerHTML=h;
}
function buildBubbles(){
  var h='';
  for(var i=0;i<18;i++){
    var left=8+Math.random()*84, dur=5+Math.random()*5, delay=Math.random()*6, size=4+Math.random()*6;
    h+='<i style="left:'+left.toFixed(1)+'%;width:'+size.toFixed(1)+'px;height:'+size.toFixed(1)+'px;'+
       'animation-duration:'+dur.toFixed(1)+'s;animation-delay:'+delay.toFixed(1)+'s"></i>';
  }
  $('bubbles').innerHTML=h;
  bubbleEls=true;
}
function buildHeatwaves(){
  var h='';
  for(var i=0;i<3;i++) h+='<i style="margin-left:'+(i*6-6)+'px;animation-delay:'+(i*0.8).toFixed(1)+'s"></i>';
  $('heatwaves').innerHTML=h;
}
function buildDaySegments(){
  if(!S||!S.sched) return;
  var e=S.sched[S.wd||0]||S.sched[0];
  if(!e) return;
  var enabled=!!e[0];
  var on=toMin(e[1]), off=toMin(e[2]), noff=toMin(e[3]), ramp=Number(e[4])||40;
  var segs=[], dayLen=off-on; if(dayLen<=0) dayLen+=1440;
  var nightLen=(noff-off+1440)%1440;
  function push(a,b,k,txt){ if(b<=a) return; segs.push({a:a,b:b,k:k,t:txt}); }
  if(!enabled){
    push(0,1440,'dark','свет выключен');
  } else {
    push(0,on,'dark','');
    push(on,on+ramp,'dawn','рассвет');
    push(on+ramp,on+dayLen-ramp,'day','день');
    push(on+dayLen-ramp,on+dayLen,'dusk','закат');
    var noStart=on+dayLen;
    if(nightLen>0) push(noStart,noStart+nightLen,'night','ночник');
    push(noStart+Math.max(nightLen,0),1440,'dark','');
  }
  var h='';
  for(var i=0;i<segs.length;i++){
    var s=segs[i], w=(s.b-s.a)/1440*100;
    var txt=(w>9)?s.t:'';                       // подпись только если сегмент достаточно широкий
    h+='<div class="seg '+s.k+'" style="left:'+(s.a/1440*100).toFixed(2)+'%;width:'+w.toFixed(2)+'%">'+txt+'</div>';
  }
  $('daybar').innerHTML=h+'<div class="now" id="nowMark" style="left:'+(nowMin()/1440*100).toFixed(2)+'%"></div>';
}
function toMin(v){ if(!v) return 0; var p=String(v).split(':'); return (parseInt(p[0],10)||0)*60+(parseInt(p[1],10)||0); }
function nowMin(){ if(!S||!S.time) return 0; var p=S.time.split(':'); return (parseInt(p[0],10)||0)*60+(parseInt(p[1],10)||0)+ (parseInt(p[2],10)||0)/60; }

/* ---------- отрисовка ---------- */
function tween(id,to,dec,suffix){
  var el=$(id); if(!el) return;
  var from=parseFloat(el.dataset.v);
  if(isNaN(from)||el.dataset.v===undefined) from=to;
  if(Math.abs(from-to)<0.005){ el.dataset.v=to; el.textContent=to.toFixed(dec)+(suffix||''); return; }
  el.dataset.v=to;
  var t0=performance.now(), dur=450;
  function step(t){
    var k=Math.min(1,(t-t0)/dur), e=1-Math.pow(1-k,3);
    el.textContent=(from+(to-from)*e).toFixed(dec)+(suffix||'');
    if(k<1) requestAnimationFrame(step);
    else el.textContent=to.toFixed(dec)+(suffix||'');
  }
  requestAnimationFrame(step);
  if(!firstRender){
    el.classList.remove('flash'); void el.offsetWidth; el.classList.add('flash');
  }
}

function render(){
  if(!S) return;
  var nightLight=(S.phase==='night' && S.lmod!==1 && S.lp && S.lp[0]>0);
  var critical=(S.al&0x0D)!==0;
  var body=[];
  if(nightLight) body.push('night');
  if(S.pump) body.push('pump-on');
  if(S.air) body.push('air-on');
  if(S.fan) body.push('fan-on');
  if(S.heat) body.push('heat-on');
  if(S.heat) body.push('heating');
  if(critical) body.push('alert');
  document.body.className=body.join(' ');
  document.body.dataset.phase=nightLight?'night':(S.phase||'day');

  $('devname').textContent=S.name||'Аквариум';
  $('clock').textContent=S.time||'--:--:--';
  var netTxt=(S.ap?'точка доступа ':'')+(S.ip||'')+(S.rssi?(' · '+S.rssi+' дБм'):'');
  $('netText').textContent=netTxt;
  var wdNow=(S.wd===undefined?0:S.wd), schedNow=(S.sched&&S.sched[wdNow])?S.sched[wdNow]:null;
  $('phaseNote').textContent=(S.tok?('вода '+nf(S.t,2)+' °C · воздух '+nf(S.ta,1)+' °C'):'НЕТ ДАТЧИКА ВОДЫ')+
    ' · день '+nf(S.day[0],0)+'% · ночник '+nf(S.night[0],0)+'%'+
    (schedNow&&schedNow[0]?(', ночник до '+schedNow[3]):' (свет сегодня выключен)');

  /* лампа и вода */
  $('lampFill').style.width=(S.lp[0]||0)+'%';
  $('bloom').style.setProperty('--led-o',Math.min(1,(S.lp[0]||0)/70));
  var wh=(72+Math.min(6,(S.lp[0]||0)/20))+'%';
  $('waterCol').style.height=wh;

  /* температура и дуга индикатора (270°, r=80 -> 502.7; видимая часть 377) */
  if(S.tok){
    tween('tempVal',Number(S.t)||0,2,'');
    pushTemp(Number(S.t));
  } else {
    $('tempVal').textContent='—';
  }
  $('tempSub').textContent=nightLight?'Ночник':(PH[S.phase]||'—');
  $('tempTiny').textContent=S.tok?('цель '+nf(S.sp,1)+' ±'+nf(S.hy,1)+' °C'):'датчик воды не найден';
  $('tempTrend').textContent=tempTrend();
  var CIRC=502.65, SPAN=377.0, GLO=18, GHI=32;
  var gp=Math.max(0,Math.min(1,((Number(S.t)||0)-GLO)/(GHI-GLO)));
  if(S.tok) $('gVal').style.strokeDasharray=(gp*SPAN).toFixed(1)+' '+CIRC;
  else $('gVal').style.strokeDasharray='0 '+CIRC;
  var hy=Number(S.hy)||0.4;
  var gb1=Math.max(0,Math.min(1,((S.sp-hy)-GLO)/(GHI-GLO)));
  var gb2=Math.max(0,Math.min(1,((S.sp+hy)-GLO)/(GHI-GLO)));
  $('gBand').style.strokeDasharray=((gb2-gb1)*SPAN).toFixed(1)+' '+CIRC;
  $('gBand').style.strokeDashoffset=(-gb1*SPAN).toFixed(1);
  var gth=(135+gp*270)*Math.PI/180;
  var gx=(100+80*Math.cos(gth)).toFixed(1), gy=(100+80*Math.sin(gth)).toFixed(1);
  $('gDot').setAttribute('cx',gx); $('gDot').setAttribute('cy',gy);
  $('gDotGlow').setAttribute('cx',gx); $('gDotGlow').setAttribute('cy',gy);

  /* бейдж состояния и правая колонка */
  var crit=(S.al&0x0D)!==0;
  $('hbadge').className='hbadge'+(crit?' err':'');
  $('hbadgeText').textContent=crit?'тревога':'норма';
  tween('h-air',Number(S.ta)||0,1,' °C');
  $('airBar').style.width=Math.max(0,Math.min(100,((Number(S.ta)||0)-10)/25*100)).toFixed(0)+'%';
  $('h-heat').textContent=S.heat?('греет '+fmtDur(S.hl)):(S.tok?'выключен':'заблокирован');
  $('rowHeat').className='hrow'+(S.heat?' hot':'');
  $('heatBar2').style.width=(S.heat?Math.max(6,Math.min(100,(S.hl%(45*60))/(45*60)*100)):0)+'%';
  $('h-heatSub').textContent=S.heat
    ? ('до уставки ≈ '+Math.max(1,Math.round((S.est||0)/60))+' мин, мощность '+nf(S.pw,0)+' Вт')
    : (S.tok?('вода '+nf(S.t,2)+' °C, недобор '+nf(Math.max(0,S.sp-S.t),2)+' °C'):'нагрев заблокирован без датчика воды');
  $('h-light').textContent=(S.lp[0]||0)+' %';
  $('lightBar').style.width=(S.lp[0]||0)+'%';
  var todayS=(S.sched&&S.sched[S.wd||0])?S.sched[S.wd||0]:null;
  $('h-lightSub').textContent=(PH[S.phase]||'—')+(todayS&&todayS[0]?(', ночник до '+todayS[3]):', сегодня выключен');

  /* кормление: одна кнопка в герое и одна в карточке помпы */
  var feeding=(S.feed||0)>0;
  var feedTxt=feeding?('идёт кормление, помпа вернётся через '+fmtDur(S.feed)):('пауза при кормлении: '+(S.fmin||10)+' мин');
  ['feedLeft','feedLeftCard'].forEach(function(id){ var el=$(id); if(el) el.textContent=feedTxt; });
  ['heroFeed','feedBtn'].forEach(function(id){
    var el=$(id); if(!el) return;
    el.className='feedBtn'+(feeding?' active':'');
    el.textContent=feeding?'ОТМЕНИТЬ ПАУЗУ':'КОРМЛЕНИЕ';
  });
  setv($('feedmin'),S.fmin||10);

  /* режимы вентилятора и нагрева: подсвечиваем выбранный */
  var fm=(S.fanmode|0), hm=(S.heatmode|0);
  $('fan-auto').className=(fm===0?'on':''); $('fan-on').className=(fm===1?'on':''); $('fan-off').className=(fm===2?'on':'');
  $('heat-auto').className=(hm===0?'on':''); $('heat-on').className=(hm===1?'on':''); $('heat-off').className=(hm===2?'on':'');
  $('heatModeNote').textContent=hm===0?'автоматически по уставке'
    :(hm===1?'включён вручную, осталось '+fmtDur(Math.max(0,900-(S.hl||0))):'принудительно выключен, автоматика не включит');

  /* мини-график обновляем не чаще раза в 15 секунд */
  if(Date.now()-lastSpark>15000){ lastSpark=Date.now(); drawSpark(); }

  /* реле */
  cls('p-pump','pill',S.pump); cls('p-air','pill',S.air);
  cls('p-fan','pill',S.fan);   cls('p-heat','pill',S.heat);
  if(S.heat) $('p-heat').className='pill hot';
  if(feeding) $('p-pump').className='pill err';
  $('p-pump').textContent=feeding?'ПАУЗА':(S.pump?'ВКЛ':'ВЫКЛ');
  $('p-air').textContent=S.air?'ВКЛ':'ВЫКЛ';
  $('p-fan').textContent=S.fan?'ВКЛ':(fm===2?'ВЫКЛ (принуд.)':'ВЫКЛ');
  $('p-heat').textContent=S.heat?(hm===1?'ВКЛ вручную':'НАГРЕВ'):(hm===2?'ВЫКЛ (принуд.)':'ВЫКЛ');
  cls('sw-pump','switch',S.pump); cls('sw-air','switch',S.air);
  /* переключателя нагрева больше нет — им управляют режимные кнопки, поэтому проверяем наличие */
  var swh=$('sw-heat');
  if(swh) swh.className='switch'+(S.heat?' hot':'');
  var swf=$('sw-fan');
  if(swf) swf.className='switch'+(S.fan?' on':'');

  /* термостат */
  $('thermoNote').textContent='полоса '+nf(S.sp-S.hy,1)+'…'+nf(S.sp+S.hy,1)+' °C · защита при '+
    nf(S.maxt,1)+' °C · '+nf(S.pw,0)+' Вт'+(S.hlock?' · ЗАБЛОКИРОВАН':'');
  $('gaugeText').textContent=nf(S.t,1)+'°';
  var gp=(Number(S.t)-15)/20; gp=Math.max(0,Math.min(1,gp));
  $('needle').style.transform='rotate('+(-90+gp*180).toFixed(1)+'deg)';
  var L=Math.PI*90;
  $('gaugeVal').style.strokeDasharray=(gp*L).toFixed(1)+' '+(L*2);
  var b1=((Number(S.sp)-hy)-15)/20, b2=((Number(S.sp)+hy)-15)/20;
  $('gaugeBand').style.strokeDasharray=((b2-b1)*L).toFixed(1)+' '+(L*2);
  $('gaugeBand').style.strokeDashoffset=(-b1*L).toFixed(1);
  $('heatState').textContent=S.heat?('греет уже '+fmtDur(S.hl)):'нагрев выключен';
  $('heatBar').style.width=(S.heat?Math.min(100,(S.hl%(45*60))/(45*60)*100):0)+'%';
  setv($('sp'),S.sp); setv($('hy'),S.hy); setv($('maxt'),S.maxt); setv($('pw'),S.pw);
  setv($('fanon'),S.fanon); setv($('fanhy'),S.fanhy);

  /* светильник */
  $('lm-auto').className=(S.lmod===0?'on':''); $('lm-night').className=(S.lmod===2?'on':'');
  $('lm-man').className=(S.lmod===1?'on':'');
  $('p-light').textContent=['АВТО','РУЧНОЙ','НОЧНИК'][S.lmod]||'АВТО';
  $('p-light').className='pill '+((!S.lmod||S.lp[0]>0)?'on':'');
  /* сегодняшнее расписание одной строкой + счётчик дней */
  var wd=(S.wd===undefined?0:S.wd), today=(S.sched&&S.sched[wd])?S.sched[wd]:null;
  $('todaySched').textContent=today
    ? (today[0] ? ('сегодня ('+WD[wd]+'): свет '+today[1]+'–'+today[2]+
                   ', ночник до '+today[3]+' ('+Math.round((today[5]||0)/6)/10+' ч), переход '+today[4]+' мин')
                : ('сегодня ('+WD[wd]+') выходной — свет не включается'))
    : '—';
  var dayCnt=0;
  if(S.sched) for(var di=0;di<7;di++) if(S.sched[di]&&S.sched[di][0]) dayCnt++;
  $('daysNote').textContent=(dayCnt===7)?'расписание на все 7 дней'
    :(dayCnt===0?'ни один день не включён, свет не работает':'включено дней: '+dayCnt+' из 7');
  $('schedNote').textContent=dayCnt+' из 7 дней';
  syncSched();
  if(!builtChans){ buildChans(); builtChans=true; }
  for(var c=0;c<S.nch;c++){
    var vd=$('vday'+c), vn=$('vnight'+c), vm=$('vman'+c);
    if(vd) vd.textContent=S.day[c];
    if(vn) vn.textContent=S.night[c];
    if(vm) vm.textContent=S.man[c];
    var sd=$('day'+c), sn=$('night'+c), sm=$('man'+c);
    if(sd){ sd.style.setProperty('--p',S.day[c]+'%'); if(document.activeElement!==sd) sd.value=S.day[c]; }
    if(sn){ sn.style.setProperty('--p',S.night[c]+'%'); if(document.activeElement!==sn) sn.value=S.night[c]; }
    if(sm){ sm.style.setProperty('--p',S.man[c]+'%'); if(document.activeElement!==sm) sm.value=S.man[c]; }
    var b=$('bar'+c); if(b) b.style.width=(S.lp[c]||0)+'%';
  }
  var nl=Number(S.nlen||0);
  $('nlen').textContent=!S.night[0]?'ночник выключен — ночью светильник не горит':
    (nl<=0?'ночник гаснет сразу после заката':'ночник горит '+fmtDur(nl*60)+' после заката, затем светильник выключается до утра');
  buildDaySegments();

  /* Wi-Fi */
  var wst=S.wst||'wait';
  var wmap={ok:['ok','ПОДКЛЮЧЕНО'],wait:['','ПОДКЛЮЧАЮСЬ'],nosid:['err','СЕТЬ НЕ НАЙДЕНА'],
            badpass:['err','ПРОВЕРЬТЕ ПАРОЛЬ'],lost:['err','СВЯЗЬ ПОТЕРЯНА']};
  var wm=wmap[wst]||wmap.wait;
  $('w-pill').className='pill '+wm[0];
  $('w-pill').textContent=wm[1];
  $('w-now').textContent=(wst==='ok')
      ? ('подключено к «'+(S.ssid||S.wssid)+'» · адрес '+S.ip+' · сигнал '+(S.rssi||0)+' дБм')
      : (S.ap ? ('режим точки доступа · http://192.168.4.1 · пытаюсь выйти в «'+(S.wssid||'—')+'»')
              : ('подключаюсь к «'+(S.wssid||'—')+'»…'));
  if(!$('w-ssid').value && S.wssid && wst!=='ok') $('w-ssid').value=S.wssid;
  $('w-pass').placeholder=S.wcustom?'пароль сохранён — введите, только чтобы заменить':'пароль сети';

  /* журнал циклов */
  var h='<div class="tr head"><span>#</span><span>Время</span><span>Старт</span><span>Конец</span><span>Нагрев</span><span>Ошибка</span></div>';
  if(!S.cyc.length) h+='<div class="empty">Циклов ещё не было</div>';
  for(var k=0;k<S.cyc.length;k++){
    var c2=S.cyc[k], err=(c2.te-c2.sp), m=Math.floor(c2.d/60), s2=c2.d%60;
    h+='<div class="tr"><span>'+(S.cyc.length-k)+'</span><span>'+c2.t+'</span><span>'+nf(c2.ts,2)+'°</span>'+
       '<span>'+nf(c2.te,2)+'°</span><span>'+m+':'+(s2<10?'0':'')+s2+'</span>'+
       '<span style="color:'+(Math.abs(err)<0.1?'#67eaa5':(err<0?'#7fb2ff':'#ffa878'))+'">'+
       (err>=0?'+':'')+err.toFixed(2)+'°</span></div>';
  }
  $('cyc').innerHTML=h;
  $('cycNote').textContent=S.cyc.length?('последние '+S.cyc.length):'—';

  /* диагностика */
  if(!builtDiag){ buildDiag(); builtDiag=true; }
  var base='http://'+(S.ip||'');
  $('d-ip').innerHTML='<a href="'+base+'" target="_blank" style="color:var(--accent);text-decoration:none">'+base+'</a>';
  $('d-ssid').textContent=(S.ssid||'—')+(S.ap?' · точка доступа':'');
  $('d-gw').textContent=S.gw||'—';
  $('d-rssi').innerHTML=(S.ap?'—':bars(S.rssi)+(S.rssi||0)+' дБм');
  $('d-host').innerHTML=(S.host||'—')+(S.ap?'':'<span style="color:var(--mute)">.local</span>');
  $('d-up').textContent=fmtDur(S.up);
  var tsrcMap={ntp:'NTP',browser:'с устройства',approx:'из памяти платы',build:'время сборки'};
  $('d-time').textContent=(S.time||'')+' '+(S.date||'')+' · '+(tsrcMap[S.tsrc]||S.tsrc||'—')+
    (S.wd!==undefined?(' · '+WD[S.wd]):'');
  $('d-sen').textContent=S.nsen+' шт · вода '+(S.tok?'есть':'НЕТ')+' · воздух '+(S.aok?'есть':'НЕТ');
  $('d-nvm').textContent=S.nvm?'сохранены':'по умолчанию';
  $('d-fw').textContent='v'+(S.fw||'?')+(S.wdt?' · WDT':'');
  $('d-mem').textContent=Math.round((S.heap||0)/1024)+' КБ / '+((S.psram||0)/1048576).toFixed(0)+' МБ';
  $('d-req').textContent=(S.reqs||0)+' (команд '+(S.cmds||0)+')';
  $('d-rst').textContent=S.rstrt||'—';
  $('diagNote').textContent=S.al?(S.altext||'есть тревоги'):'все системы в норме';
  if($('ghBox')) $('ghBox').style.display=S.ogithub?'':'none';
  if($('ghUrl')) $('ghUrl').textContent=S.ogithub?'источник: build/prebuilt в вашем репозитории':'';
  updateWidgetLinks();

  $('footInfo').textContent=(S.name||'')+' v'+(S.fw||'')+' · http://'+(S.ip||'')+
    ' · работает '+fmtDur(S.up)+' · heap '+Math.round((S.hp||S.heap||0)/1024)+' КБ';
  firstRender=false;
}
function bars(rssi){
  var lvl=Math.max(0,Math.min(4,Math.round(((rssi||-90)+90)/12)));
  var h='<span class="bars">';
  for(var i=0;i<4;i++) h+='<i style="height:'+((i+1)*3+2)+'px;opacity:'+(i<lvl?1:.25)+'"></i>';
  return h+'</span> ';
}
function fmtDur(sec){
  sec=Number(sec)||0;
  var h=Math.floor(sec/3600), m=Math.floor((sec%3600)/60), s=Math.floor(sec%60);
  if(h) return h+' ч '+m+' мин';
  if(m) return m+':'+(s<10?'0':'')+s;
  return s+' с';
}

/* ---------- график ---------- */
function fitCanvas(c){
  var r=c.getBoundingClientRect(), dpr=Math.min(2,window.devicePixelRatio||1);
  c.width=Math.max(320,Math.round(r.width*dpr));
  c.height=Math.round(212*dpr);
  var ctx=c.getContext('2d');
  ctx.setTransform(dpr,0,0,dpr,0,0);
  return {ctx:ctx,W:c.width/dpr,H:c.height/dpr};
}
/* Живой график: между полными обновлениями дописываем точку сами,
   чтобы линия двигалась, а не «замирала до перезагрузки». */
function appendLivePoint(){
  if(!S||!H||!H.n||S.t===null||S.t===undefined) return;
  var value=Math.round(Number(S.t)*100);
  if(!isFinite(value)||value<=0) return;
  H.t.push(value);
  H.a.push((S.tok&&S.ta)?Math.round(Number(S.ta)*100):-3000);
  var fl=0;
  if(S.heat) fl|=0x01;
  if(S.fan) fl|=0x02;
  if(S.pump) fl|=0x04;
  if(S.air) fl|=0x08;
  if(S.lp&&S.lp[0]>2) fl|=0x10;
  H.f.push(fl);
  while(H.t.length>420){ H.t.shift(); H.a.shift(); H.f.shift(); }
  H.n=H.t.length;
  drawChart();
}
function drawChart(){
  var c=$('chart'); if(!c) return;
  var o=fitCanvas(c), ctx=o.ctx, W=o.W, Hh=o.H, pad=8;
  ctx.clearRect(0,0,W,Hh);
  if(!H||!H.n){ ctx.fillStyle='rgba(255,255,255,.25)'; ctx.font='12px system-ui';
    ctx.fillText('нет данных — накапливаю историю…',10,Hh/2); return; }
  var n=H.n, t=H.t.map(function(v){return v/100}), a=H.a.map(function(v){return v/100}), f=H.f;
  var all=t.concat(a.filter(function(v){return v>-20}));
  var mn=Math.min.apply(null,all), mx=Math.max.apply(null,all);
  if(!isFinite(mn)||!isFinite(mx)) return;
  var m=(mx-mn)*0.18+0.25; mn-=m; mx+=m;
  var accent=getComputedStyle(document.body).getPropertyValue('--accent').trim()||'#00e5ff';
  var plotH=Hh-62;
  function X(i){ return pad+(W-2*pad)*i/Math.max(1,n-1); }
  function Y(v){ return pad+(plotH-pad)*(1-(v-mn)/(mx-mn)); }

  /* сетка */
  ctx.strokeStyle='rgba(255,255,255,.06)'; ctx.lineWidth=1; ctx.font='10px system-ui';
  for(var g=0;g<=3;g++){
    var vv=mn+(mx-mn)*g/3, y=Y(vv);
    ctx.beginPath(); ctx.moveTo(pad,y); ctx.lineTo(W-pad,y); ctx.stroke();
    ctx.fillStyle='rgba(255,255,255,.35)'; ctx.fillText(vv.toFixed(1)+'°',4,y-3);
  }
  /* вода: заливка */
  var grad=ctx.createLinearGradient(0,pad,0,plotH);
  grad.addColorStop(0,'rgba('+getComputedStyle(document.body).getPropertyValue('--accent-rgb').trim()+',.34)');
  grad.addColorStop(1,'rgba('+getComputedStyle(document.body).getPropertyValue('--accent-rgb').trim()+',0)');
  ctx.beginPath(); ctx.moveTo(X(0),plotH);
  for(var i2=0;i2<n;i2++) ctx.lineTo(X(i2),Y(t[i2]));
  ctx.lineTo(X(n-1),plotH); ctx.closePath(); ctx.fillStyle=grad; ctx.fill();
  /* воздух */
  ctx.beginPath(); var started=false;
  for(var i3=0;i3<n;i3++){ var v=a[i3]; if(!(v>-20)){started=false;continue;}
    if(!started){ctx.moveTo(X(i3),Y(v));started=true;} else ctx.lineTo(X(i3),Y(v)); }
  ctx.strokeStyle='#5b7cff'; ctx.lineWidth=1.6; ctx.stroke();
  /* вода: линия */
  ctx.beginPath(); started=false;
  for(var i4=0;i4<n;i4++){ var v2=t[i4]; if(!(v2>-20)){started=false;continue;}
    if(!started){ctx.moveTo(X(i4),Y(v2));started=true;} else ctx.lineTo(X(i4),Y(v2)); }
  ctx.strokeStyle=accent; ctx.lineWidth=2.2; ctx.shadowColor=accent; ctx.shadowBlur=8; ctx.stroke(); ctx.shadowBlur=0;
  /* уставка */
  if(S){ ctx.setLineDash([5,5]); ctx.strokeStyle='rgba(255,255,255,.32)'; ctx.lineWidth=1;
    ctx.beginPath(); ctx.moveTo(pad,Y(S.sp)); ctx.lineTo(W-pad,Y(S.sp)); ctx.stroke(); ctx.setLineDash([]); }
  /* полосы состояния */
  var base=plotH+14;
  for(var i5=0;i5<n;i5++){
    var fl=f[i5]||0, x0=X(i5), x1=X(i5+1);
    if(fl&0x01){ ctx.fillStyle='rgba(255,120,60,.85)'; ctx.fillRect(x0,base,Math.max(1,x1-x0),8); }
    if(fl&0x10){ ctx.fillStyle='rgba(255,214,120,.6)'; ctx.fillRect(x0,base+10,Math.max(1,x1-x0),8); }
  }
  /* временная шкала: 5 отметок от самого старого значения до «сейчас» */
  var stepSec=(H.step&&H.step>0)?H.step:15;
  var windowSec=(n-1)*stepSec;
  var endT=Date.now();
  ctx.font='10px system-ui'; ctx.fillStyle='rgba(255,255,255,.45)'; ctx.textBaseline='top';
  for(var k=0;k<5;k++){
    var frac=k/4;
    var x=pad+(W-2*pad)*frac;
    var when=new Date(endT-(1-frac)*windowSec*1000);
    var lbl=String(when.getHours()); if(lbl.length<2) lbl='0'+lbl;
    var mi=String(when.getMinutes()); if(mi.length<2) mi='0'+mi;
    ctx.strokeStyle='rgba(255,255,255,.07)';
    ctx.beginPath(); ctx.moveTo(x,pad); ctx.lineTo(x,plotH); ctx.stroke();
    ctx.fillText(lbl+':'+mi, Math.min(W-pad-26, Math.max(pad, x-13)), plotH+26);
  }
  ctx.textBaseline='alphabetic';
  $('chartInfo').textContent=n+' точек · шаг '+stepSec+' с · окно '+Math.round(windowSec/60)+' мин';
}
window.addEventListener('resize',function(){ drawChart(); });

/* ---------- время с устройства ---------- */
function setTimeFromBrowser(){
  var ep=Math.floor(Date.now()/1000);
  cmd('tset='+ep,'время взято с этого устройства: '+new Date().toLocaleString());
}

/* ---------- привязка Android-виджета к этой плате ---------- */
function widgetHost(){
  var h=(S&&S.ip&&S.ip!=='0.0.0.0')?S.ip:location.hostname;
  return h||'';
}
function bindWidget(){
  var h=widgetHost();
  if(!h){ toast('адрес платы пока неизвестен','err'); return false; }
  $('widgetNote').textContent='передаю адрес '+h+' в приложение…';
  try{ window.location.href='aqua://bind?host='+encodeURIComponent(h); }catch(e){}
  setTimeout(function(){
    $('widgetNote').textContent='если приложение не открылось — сначала установите APK кнопкой слева';
    toast('если приложение не открылось — сначала установите APK','err');
  },2600);
  return false;
}
function updateWidgetLinks(){
  var h=widgetHost();
  if($('bindLink')&&h) $('bindLink').href='aqua://bind?host='+encodeURIComponent(h);
  if($('widgetNote')&&h&&!$('widgetNote').textContent.replace('—','').trim())
    $('widgetNote').textContent='адрес для привязки: '+h;
  if($('widgetInfo')&&S&&S.wapk) $('widgetInfo').textContent=
    'квадрат 2×3 на главном экране · APK '+(S.wapk)+' КБ вшит в плату';
}
/* рябь на кнопках */
document.addEventListener('click',function(e){
  var b=e.target.closest('.btn'); if(!b) return;
  var r=b.getBoundingClientRect(), s=document.createElement('span');
  s.className='rip'; s.style.width=s.style.height='10px';
  s.style.left=(e.clientX-r.left-5)+'px'; s.style.top=(e.clientY-r.top-5)+'px';
  b.appendChild(s); setTimeout(function(){ s.remove(); },620);
});

/* ---------- связь и запуск ---------- */
setInterval(function(){
  var age=lastOk?Math.round((Date.now()-lastOk)/1000):999;
  if(age>6){
    $('offline').classList.add('show');
    var msg='последние данные: '+age+' с назад';
    /* плата ушла в домашнюю сеть из точки доступа — подсказываем новый адрес */
    if(S&&S.ip&&S.ip!=='0.0.0.0'&&S.ip.indexOf('192.168.4.')!==0){
      msg='плата уже в сети — откройте <a href="http://'+S.ip+'" style="color:var(--accent)">http://'+S.ip+'</a>';
    }
    $('offTimer').innerHTML=msg;
  }
},1000);
setInterval(function(){ if(!S||Date.now()-lastOk>6000) poll(); },5000);
setInterval(function(){ if(Date.now()-lastOk<6000) fetchHistory(); },60000);
/* точка на графике каждые 15 секунд — так же, как плата пишет историю */
setInterval(function(){ if(Date.now()-lastOk<6000) appendLivePoint(); },15000);

window.addEventListener('load',function(){
  buildTicks(); buildBubbles(); buildHeatwaves();
  connect(); poll(); fetchHistory();
  setTimeout(function(){ if(!S) fetchHistory(); drawSpark(); },1500);
  setTimeout(drawSpark,4000);
});
window.addEventListener('visibilitychange',function(){ if(!document.hidden){ poll(); fetchHistory(); } });
</script>
</body>
</html>
)HTML";
