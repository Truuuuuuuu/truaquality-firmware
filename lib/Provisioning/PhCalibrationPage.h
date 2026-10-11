#pragma once

// The Calibrate pH screen (D-16) served by the setup portal at /ph. Kept out of Provisioning.cpp so that file
// stays readable, and separate from CalibrationPage.h so the turbidity page stays byte-for-byte untouched. The
// string is static storage, like the turbidity page, so nothing has to outlive a stack frame when it is served.
//
// The page is self-contained because the phone on the hotspot has no internet. It writes every value with
// textContent so nothing from the unit is ever parsed as markup, and it polls /ph/status with a timeout chain
// that only schedules the next request after the previous one settles: the portal's web server serves one client
// at a time, and a fixed-rate timer would stack requests behind a slow reply. The page never sends a measured
// value. It posts only the slot and the two picked buffer nominals; the unit measures, decides and refuses, and
// the page only displays. The numbers in the copy (20-35 °C, 2 pH apart, 30 days) are display text for the
// unit's rules, not the rules themselves.
namespace provisioning
{
  namespace phpage
  {
    const char PH_PAGE_HTML[] = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Calibrate pH</title>
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
select{width:100%;font-size:1.1rem;padding:10px;margin:6px 0;box-sizing:border-box;border-radius:4px;border:1px solid #999;background:#fff}
.w{line-height:1.5}
.d{font-size:28px;font-weight:bold;line-height:1.2}
.g{color:#2e7d32}.a{color:#8a5300;font-weight:bold}.r{color:#dc3630}
.x{background:#dc3630}.n{background:#999}
.row{display:flex;gap:8px}.row button{flex:1}
.hide{display:none}
#rv,#cl{margin-top:16px}
</style></head><body><div class="w">
<h2>Calibrate pH</h2>
<p>Calibrate with 6.86 then 9.18 (brackets the 9.5 alkaline alert line). 4.01 is the spare verification buffer. Put the pH probe and the temperature probe together in the buffer cup, rinse both with clean water between buffers, and hold still while it reads.</p>
<div class="b">
<div id="mv">Sensor: — mV</div>
<div id="ph" class="d">pH: —</div>
<div id="tc">Temperature: — °C</div>
<div id="st" class="m"></div>
</div>
<div class="b" id="sb">
<div id="se">Not calibrated. pH is not reported until you calibrate. Capture buffer A and buffer B below, then tap Save calibration.</div>
<div id="sk" class="hide">
<div id="sd"></div><div id="sbuf"></div><div id="sl"></div><div id="so"></div><div id="sv"></div><div id="sag" class="a"></div>
<div id="cl"><button id="cx" class="x">Clear pH calibration</button>
<div id="cf" class="hide"><div>Really clear? pH stops reporting until recalibrated</div>
<div class="row"><button id="cy" class="x">Confirm clear</button><button id="cn" class="n">Cancel</button></div></div></div>
</div>
<div id="clr" class="m"></div>
</div>
<div class="b"><b>Buffer A</b>
<select id="sa"><option value="400">pH 4.00</option><option value="401">pH 4.01</option><option value="686" selected>pH 6.86</option><option value="700">pH 7.00</option><option value="918">pH 9.18</option><option value="1001">pH 10.01</option></select>
<button id="ca">Capture A</button><div id="ra" class="m"></div></div>
<div class="b"><b>Buffer B</b>
<select id="sbsel"></select>
<button id="cb">Capture B</button><div id="rb" class="m"></div></div>
<div class="b hide" id="rv"><b>New calibration — not saved yet</b>
<div id="ql"></div><div id="qo"></div><div id="qv"></div>
<button id="sv2">Save calibration</button>
<div>Not saved yet. Leaving this page keeps the current calibration.</div>
<div id="rs" class="m"></div></div>
<p style="margin-top:24px"><a href="/">Back to menu</a></p>
<p>When you are done, tap Exit on the menu or leave the hotspot.</p>
</div>
<script>
function E(i){return document.getElementById(i)}
function T(i,s){E(i).textContent=s}
var D="—",C=" °C",NF="Could not reach the unit. Stay on the hotspot and try again.";
var NVS="Could not save to the unit's memory. Try again; if it repeats, the unit needs service.";
var G=", then capture again.",G2=", then capture both again.",CB=" Capture both buffers again.",PH="Probe health: ";
var BUSY="A capture is already running; wait for it to finish.";
var ST={steady:"Steady: Capture will judge the next 20 s",
signal_lost:"Signal lost: check the pH probe plug and the board's cable and 5 V supply"};
var RF={no_temperature:"No temperature reading. Put the temperature probe in the buffer cup next to the pH probe"+G,
temp_out_of_range:"Buffer at {X} °C is outside 20–35 °C. Let it reach room temperature"+G,
settle_timeout:"Still settling after {X} min. Rinse the probe, re-soak it in the buffer"+G,
signal_lost:"No signal from the pH board. Check the probe's BNC plug and the board's cable and 5 V supply"+G,
unstable:"The reading jumped while capturing. Hold the probe and cup still"+G,
buffers_too_close:"Buffers A and B ({a} and {b}) are less than 2 pH apart. Pick two buffers at least 2 pH apart, such as 6.86 and 9.18.",
wrong_sign:"The two readings slope the wrong way for this board. Check that each slot's buffer matches the bottle that was in the cup"+G2,
slope_too_small:"The probe barely responds ({X} mV per pH). Check that it is soaked and plugged in; if it repeats, replace the probe.",
offset_out_of_window:"Offset at pH 7 is {X} mV, outside the expected range. Check that the board's ADJ knob has not been turned and that the buffers are fresh"+G2,
invalid:"The unit refused this calibration."+CB,
cancelled:"Capture cancelled because the setup hotspot closed.",
nothing_to_save:"Capture both buffers first, then tap Save calibration.",
changed:"The buffers on this page changed since the capture."+CB,
busy:BUSY,nvs:NVS};
var VD={good:["g",PH+"Good"],aging:["a",PH+"Aging — clean and re-soak the probe; plan a replacement"],
replace:["r",PH+"Replace — slope or offset is far from the board's nominal; pH still reports but is less trustworthy"]};
var confirmOpen=false,dirty={},od=30,A=E("sa"),B=E("sbsel");
for(var i=0;i<A.options.length;i++)B.appendChild(A.options[i].cloneNode(true));B.value="918";
function n(v){return typeof v=="number"&&isFinite(v)}
function f(v,d){return n(v)?v.toFixed(d):D}
function sg(v){return n(v)?(v<0?"":"+")+v.toFixed(2):D}
function nm(h){return (h/100).toFixed(2)}
function mmss(s){s=Math.round(s);var m=s%60;return Math.floor(s/60)+":"+(m<10?"0":"")+m}
function H(i,h){E(i).classList.toggle("hide",!!h)}
function rf(t,v){var x=n(v)?(t=="temp_out_of_range"||t=="slope_too_small"?v.toFixed(1):Math.round(v)):"?";
return (RF[t]||RF.invalid).replace("{X}",x).replace("{a}",nm(A.value)).replace("{b}",nm(B.value))}
function so(p,k){var q=VD[k.verdict]||["",""];T(p+"l","Slope: "+(n(k.slopePct)?Math.round(k.slopePct):D)+" % of the board's nominal slope");
T(p+"o","Offset: "+sg(k.offsetPh)+" pH");E(p+"v").className=q[0];T(p+"v",q[1])}
function dt(at){return at>0?new Date(at*1000).toLocaleString():"date unknown"}
function sto(k){H("se",k);H("sk",!k);if(!k)return;
T("sd","Last calibrated: "+dt(k.calAt));T("sbuf","Buffers (pH used): "+f(k.bufA,2)+" and "+f(k.bufB,2)+" at "+f(k.tcalC,1)+C);so("s",k);
T("sag",k.age=="overdue"?"Overdue: calibrated "+k.ageDays+" days ago. Recalibrate every "+od+" days.":k.age=="date_unknown"?"Date unknown – recalibrate.":
k.age=="no_clock"?"Last calibrated: "+dt(k.calAt)+". Age not checked: the unit does not know today's date yet.":"")}
function K(o){confirmOpen=o;H("cx",o);H("cf",!o)}
function render(s){
if(n(s.overdueDays))od=s.overdueDays;
T("mv","Sensor: "+(n(s.mv)?Math.round(s.mv):D)+" mV");
T("ph",n(s.ph)?"pH: "+s.ph.toFixed(2):"pH: uncalibrated");
T("tc","Temperature: "+f(s.tempC,1)+C);
T("st",s.state=="settling"?"Settling…"+(n(s.driftMv)?" (drift "+s.driftMv.toFixed(1)+" mV)":""):ST[s.state]||"");
sto(s.stored);K(confirmOpen);
["ca","cb","sa","sbsel","sv2","cx","cy"].forEach(function(i){E(i).disabled=!!s.busy});
var sl=s.slot,c=s.capture,p=s[sl],L,r="";
if((sl=="a"||sl=="b")&&!dirty[sl]){L=sl.toUpperCase();
if(c=="capturing")r="Capturing "+L+": "+s.samples+"/"+s.needed+" s - hold still";
else if(c=="settling")r="Settling… (drift "+f(s.lastDriftMv,1)+" mV) - "+mmss(s.elapsedS)+" of "+mmss(s.timeoutS);
else if(c=="refused")r=rf(s.reason,s.reasonValue);
else if(p)r=L+": "+nm(p.nominal)+" at "+f(p.tempC,1)+C+" → "+f(p.bufPh,2)+" ("+p.mv+" mV)";
T("r"+sl,r)}
var ok=s.review&&s.a&&s.b&&+A.value==s.a.nominal&&+B.value==s.b.nominal;
H("rv",!ok);if(ok)so("q",s.review)}
function poll(){
fetch("/ph/status",{cache:"no-store"}).then(RJ).then(function(s){
render(s);setTimeout(poll,1000)}).catch(function(){setTimeout(poll,2000)});}
function RJ(r){return r.json()}
function P(u,b){return fetch(u,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b})}
function J(u,b,o,ok){P(u,b).then(RJ).then(function(j){if(!ok(j.result))T(o,rf(j.result))}).catch(function(){T(o,NF)})}
function cap(sl,S){var o="r"+sl,btn=E("c"+sl);btn.onclick=function(){
btn.disabled=true;dirty[sl]=false;T(o,"Starting capture…");
P("/ph/capture","slot="+sl+"&nominal="+encodeURIComponent(S.value)).then(function(r){
if(r.status==409)T(o,BUSY);else if(r.status==400)T(o,RF.invalid)}).catch(function(){T(o,NF)})};
S.onchange=function(){dirty[sl]=true;T(o,"Buffer changed: capture "+sl.toUpperCase()+" again.");H("rv",1)}}
cap("a",A);cap("b",B);
E("sv2").onclick=function(){J("/ph/save","a="+A.value+"&b="+B.value,"rs",function(x){
if(x!="saved")return;H("rv",1);T("ra","");T("rb","");T("rs","Saved. pH now reports with this calibration.");return 1})};
E("cx").onclick=function(){K(true)};E("cn").onclick=function(){K(false)};
E("cy").onclick=function(){J("/ph/clear","","clr",function(x){
if(x!="cleared")return;K(false);T("clr","pH calibration cleared. pH stops reporting until you calibrate again.");return 1})};
poll();
</script></body></html>
)rawliteral";
  }
}
