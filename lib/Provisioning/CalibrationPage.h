#pragma once

// The Calibrate screen (D-05) served by the setup portal. Kept out of Provisioning.cpp so that file stays
// readable, and included only by it. Both strings are static storage on purpose: WiFiManager keeps the menu
// pointer it is handed and dereferences it on every menu render, so it must outlive begin().
//
// The page is fully self-contained because the phone on the hotspot has no internet. It writes every value
// with textContent so nothing from the unit or the input box is ever parsed as markup, and it polls with a
// timeout chain that only schedules the next request after the previous one settles: the portal's web server
// serves one client at a time, and a fixed-rate timer would stack requests behind a slow reply. The numbers in
// the message text (150 mV, 2870-3580 mV) are display copy for D-03/D-04; the unit, not the page, decides.
namespace provisioning
{
  namespace calpage
  {
    const char CAL_MENU_HTML[] = "<form action='/cal' method='get'><button>Calibrate</button></form><br/>\n";

    const char CAL_PAGE_HTML[] = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Calibrate turbidity</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:12px;color:#222}
.w{max-width:420px;margin:0 auto}
button,input{width:100%;font-size:1.1rem;padding:10px;margin:6px 0;box-sizing:border-box;border-radius:4px}
button{border:0;background:#1fa3ec;color:#fff}
button:disabled{background:#999}
input{border:1px solid #999}
.b{border:1px solid #ddd;border-radius:4px;padding:8px 10px;margin:10px 0}
.m{font-weight:bold;min-height:1.2em}
a{color:#1fa3ec}
</style></head><body><div class="w">
<h2>Calibrate turbidity</h2>
<p>Put the probe in clean water, in its final position and container. Clean the probe windows. Hold still while it reads.</p>
<div class="b">
<div>Sensor: <span id="mv">—</span> mV</div>
<div>Turbidity: <span id="ntu">—</span> NTU</div>
<div id="st" class="m"></div>
</div>
<div class="b">
<div id="sv">Not calibrated</div>
<div id="dt"></div>
</div>
<button id="cap">Capture</button>
<div id="cr" class="m"></div>
<div class="b">
<label for="mvin">Clear-water value (mV)</label>
<input id="mvin" name="mv" inputmode="numeric" maxlength="5" autocomplete="off" placeholder="leave blank to keep current">
<button id="save">Save</button>
<div id="sr" class="m"></div>
</div>
<p><a href="/">Back to menu</a></p>
<p>When you are done, tap Exit on the menu or leave the hotspot.</p>
</div>
<script>
var E=function(i){return document.getElementById(i)};
var T=function(i,s){E(i).textContent=s};
var NVS="Could not save to the unit's memory. Try again; if it repeats, the unit needs service.";
var ST={settling:"Settling: collecting readings…",steady:"Steady: Capture will judge the next 20 s",
unstable:"Unstable: reading moving by more than 150 mV, wait for it to settle",
signal_lost:"Signal lost: check the sensor cable and its 5 V supply"};
var RF={signal_lost:"No signal from the turbidity sensor. Check the sensor cable and its 5 V supply, then try again.",
unstable:"Reading moved by more than 150 mV. Hold the probe still in clean water, wait for 'Steady', then capture again.",
nvs:NVS,cancelled:"Capture cancelled because the setup hotspot closed."};
var MR={keep:"Nothing changed: the stored value was kept.",
not_a_number:"Enter a whole number of millivolts, or leave blank to keep the current value.",
implausible:"That value is not a clear-water reading (expected 2870-3580).",
nvs:NVS,busy:"A capture is running; wait for it to finish."};
function n(v){return typeof v==="number"&&isFinite(v)}
function stored(mv,at){
T("sv",mv>0?"Stored clear-water value: "+mv+" mV":"Not calibrated");
T("dt",mv>0?"Last calibrated: "+(at>0?new Date(at*1000).toLocaleString():"date unknown"):"");}
function render(s){
T("mv",n(s.mv)?String(Math.round(s.mv)):"—");
T("ntu",n(s.ntu)?s.ntu.toFixed(1):"—");
T("st",ST[s.state]||"");
stored(s.storedMv,s.calibratedAt);
var c=s.capture,r="";
E("cap").disabled=c==="capturing";
if(c==="capturing")r="Capturing "+s.samples+"/"+s.needed+" - hold still";
else if(c==="accepted")r="Saved: "+s.medianMv+" mV (spread "+(n(s.spreadMv)?s.spreadMv.toFixed(0):"?")+" mV)";
else if(c==="refused")r=s.reason==="implausible"?s.medianMv+" mV is not a clear-water reading (expected 2870-3580). Clean the probe windows, use clean water, check the wiring.":(RF[s.reason]||"Capture refused.");
T("cr",r);}
function poll(){
fetch("/cal/status",{cache:"no-store"}).then(function(r){return r.json()}).then(function(s){
render(s);setTimeout(poll,1000)}).catch(function(){setTimeout(poll,2000)});}
E("cap").onclick=function(){
E("cap").disabled=true;T("cr","Starting capture…");
fetch("/cal/capture",{method:"POST"}).then(function(r){
if(r.status===409)T("cr",MR.busy)}).catch(function(){T("cr","Could not reach the unit.")});};
E("save").onclick=function(){
var v=E("mvin").value;
fetch("/cal/set",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"mv="+encodeURIComponent(v)}).then(function(r){return r.json()}).then(function(j){
if(j.result==="set"){T("sr","Saved: "+j.storedMv+" mV");stored(j.storedMv,j.calibratedAt)}
else T("sr",MR[j.result]||"Not saved.");
if(j.result==="set"||j.result==="keep")E("mvin").value="";
}).catch(function(){T("sr","Could not reach the unit.")});};
poll();
</script></body></html>
)rawliteral";
  }
}
