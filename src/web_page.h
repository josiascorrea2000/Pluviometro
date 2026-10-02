#pragma once
#include <Arduino.h>

// Página web servida por el ESP32 (se actualiza sola cada 2 s)
const char PAGINA_WEB[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pluviómetro</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#0f172a;color:#e2e8f0}
h1{font-size:1.3rem;margin:0 0 12px}
.card{background:#1e293b;border-radius:10px;padding:14px;margin-bottom:12px}
.big{font-size:2.6rem;font-weight:700;color:#38bdf8}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.k{color:#94a3b8;font-size:.85rem}.v{font-size:1.1rem;font-weight:600}
label{display:block;margin:8px 0 2px;font-size:.9rem;color:#94a3b8}
input,select{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #334155;background:#0f172a;color:#e2e8f0;font-size:1rem}
button{margin-top:12px;padding:10px 14px;border:0;border-radius:6px;font-size:1rem;cursor:pointer}
.ok{background:#0ea5e9;color:#fff}.warn{background:#ef4444;color:#fff}
.hide{display:none}
</style></head><body>
<h1>🌧️ Pluviómetro</h1>
<div class="card">
  <div class="k">Lluvia acumulada (h = V / A)</div>
  <div class="big"><span id="mm">-</span> mm</div>
  <div class="grid">
    <div><div class="k">Vuelcos</div><div class="v" id="vuelcos">-</div></div>
    <div><div class="k">Volumen V</div><div class="v"><span id="vol">-</span> ml</div></div>
    <div><div class="k">Área A</div><div class="v"><span id="area">-</span> cm²</div></div>
    <div><div class="k">Temperatura</div><div class="v"><span id="temp">-</span> °C</div></div>
    <div><div class="k">Humedad</div><div class="v"><span id="hum">-</span> %</div></div>
    <div id="pbox"><div class="k">Proyección (V = A·h)</div><div class="v"><span id="proy">-</span> L</div></div>
  </div>
</div>
<div class="card">
  <form id="f">
    <label>Forma de la boca</label>
    <select name="forma" id="forma">
      <option value="rectangular">Rectangular</option>
      <option value="circular">Circular</option>
    </select>
    <div id="rect"><div class="grid">
      <div><label>Largo (cm)</label><input name="largo" id="largo" inputmode="decimal"></div>
      <div><label>Ancho (cm)</label><input name="ancho" id="ancho" inputmode="decimal"></div>
    </div></div>
    <div id="circ" class="hide"><label>Diámetro (cm)</label><input name="diametro" id="diametro" inputmode="decimal"></div>
    <label>ml por vuelco</label><input name="ml" id="ml" inputmode="decimal">
    <label>Superficie para proyección (m², 0 = off)</label><input name="superficie" id="superficie" inputmode="decimal">
    <button class="ok" type="submit">Guardar</button>
    <button class="warn" type="button" id="reset">Reiniciar conteo</button>
  </form>
</div>
<script>
const $=id=>document.getElementById(id);
let cargado=false;
function formaUI(){const c=$('forma').value==='circular';$('rect').classList.toggle('hide',c);$('circ').classList.toggle('hide',!c);}
function pintar(d){
  $('mm').textContent=d.lluvia_mm.toFixed(2);$('vuelcos').textContent=d.vuelcos;
  $('vol').textContent=d.volumen_ml.toFixed(1);$('area').textContent=d.area_cm2.toFixed(1);
  $('temp').textContent=d.temperatura===null?'--':d.temperatura.toFixed(1);
  $('hum').textContent=d.humedad===null?'--':d.humedad.toFixed(0);
  $('proy').textContent=d.proyeccion_l.toFixed(2);$('pbox').classList.toggle('hide',d.superficie<=0);
  if(!cargado){for(const k of['largo','ancho','diametro','superficie'])$(k).value=d[k];
    $('ml').value=d.ml_vuelco;$('forma').value=d.forma;formaUI();cargado=true;}
}
async function act(){try{pintar(await (await fetch('/api/estado')).json());}catch(e){}}
$('forma').onchange=formaUI;
$('f').onsubmit=async e=>{e.preventDefault();
  const r=await fetch('/api/config',{method:'POST',body:new URLSearchParams(new FormData($('f')))});
  cargado=false;pintar(await r.json());};
$('reset').onclick=async()=>{if(confirm('¿Reiniciar el conteo de vuelcos?'))
  pintar(await (await fetch('/api/reset',{method:'POST'})).json());};
act();setInterval(act,2000);
</script></body></html>)HTML";
