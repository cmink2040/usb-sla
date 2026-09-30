#pragma once
#include <Arduino.h>

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SLA-USB</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1d2127;--mute:#6b7280;--line:#e3e5e8;--acc:#2563eb;--acc2:#93c5fd;--ok:#15803d;--warn:#b45309;--bad:#b91c1c;--mono:ui-monospace,SFMono-Regular,Consolas,monospace}
@media (prefers-color-scheme:dark){:root{--bg:#121417;--card:#1b1e22;--fg:#e6e8eb;--mute:#9aa1ab;--line:#2c3036;--acc:#60a5fa;--acc2:#1e3a5f;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:1080px;margin:0 auto;padding:18px 16px 40px;display:grid;gap:14px;grid-template-columns:1fr 1fr}
@media (max-width:820px){main{grid-template-columns:1fr}}
header{grid-column:1/-1;display:flex;align-items:baseline;gap:10px;flex-wrap:wrap}
h1{font-size:20px;margin:0}h2{font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--mute);margin:0 0 10px;font-weight:600}
.sub{color:var(--mute);font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;min-width:0}
.wide{grid-column:1/-1}
.row{display:flex;justify-content:space-between;gap:12px;align-items:center}
.pill{font-size:12px;font-weight:600;padding:2px 10px;border-radius:99px;border:1px solid currentColor;white-space:nowrap}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.mute{color:var(--mute)}
.file{display:flex;gap:14px;align-items:flex-start}
#pv{width:116px;height:116px;border-radius:8px;background:#000;flex:none;image-rendering:auto}
.fname{font-weight:600;font-size:15px;word-break:break-all}
dl{display:grid;grid-template-columns:auto 1fr;gap:3px 12px;margin:10px 0 0;font-size:13px}dt{color:var(--mute)}dd{margin:0;font-variant-numeric:tabular-nums}
.big{font-size:30px;font-weight:700;font-variant-numeric:tabular-nums;line-height:1.1}
.bar{height:8px;background:var(--line);border-radius:4px;overflow:hidden;margin:8px 0 4px}.bar i{display:block;height:100%;background:var(--acc);width:0}
canvas#map{width:100%;height:28px;display:block;border-radius:4px;margin-top:8px}
.legend{display:flex;gap:14px;font-size:12px;color:var(--mute);margin-top:6px}.legend b{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:4px;vertical-align:-1px}
#log{font:12px/1.5 var(--mono);height:300px;overflow:auto;margin:0;padding:0;list-style:none}
#log li{display:grid;grid-template-columns:56px 58px 1fr;gap:8px;padding:1px 0;border-bottom:1px solid var(--line)}
#log .t{color:var(--mute);text-align:right}#log .k{font-weight:600}
.k.read{color:var(--acc)}.k.write{color:var(--warn)}.k.error{color:var(--bad)}.k.usb{color:var(--ok)}.k.file{color:#a855f7}.k.info{color:var(--mute)}
.tabs{display:flex;gap:6px;margin-bottom:12px}.tabs button.on{background:var(--acc);border-color:var(--acc);color:#fff}
#drop{border:2px dashed var(--line);border-radius:10px;padding:24px 12px;text-align:center;cursor:pointer;color:var(--mute)}
#drop.over{border-color:var(--acc);color:var(--acc)}#drop b{color:var(--fg)}
#msg{margin-top:10px;min-height:1.4em}
button{font:inherit;padding:6px 14px;border-radius:7px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}
button:hover{border-color:var(--acc)}button.primary{background:var(--acc);border-color:var(--acc);color:#fff}
.btns{display:flex;gap:8px;flex-wrap:wrap}
input[type=text],input[type=password],input[type=url]{width:100%;font:inherit;padding:7px 9px;margin:4px 0 10px;border-radius:7px;border:1px solid var(--line);background:var(--bg);color:var(--fg)}
code{font:12px var(--mono);background:var(--bg);padding:1px 5px;border-radius:4px}
details summary{cursor:pointer;color:var(--mute);font-weight:600}
</style></head><body><main>
<header><h1>SLA-USB</h1><span class="sub" id="hdr"></span></header>

<section class="card">
  <div class="row"><h2>On the drive</h2><span id="src" class="pill mute">…</span></div>
  <div class="file">
    <canvas id="pv" width="116" height="116"></canvas>
    <div style="min-width:0;flex:1">
      <div class="fname" id="fname">…</div>
      <div class="sub" id="fsize"></div>
      <dl id="meta"></dl>
    </div>
  </div>
</section>

<section class="card">
  <div class="row"><h2>Printer</h2><span id="usb" class="pill">…</span></div>
  <div class="big" id="layer">–</div>
  <div class="sub" id="layersub"></div>
  <div class="bar"><i id="lbar"></i></div>
  <canvas id="map" width="800" height="28"></canvas>
  <div class="legend"><span><b style="background:var(--line)"></b>not here</span><span><b style="background:var(--acc2)"></b>on device</span><span><b style="background:var(--acc)"></b>read by printer</span></div>
  <dl id="stats"></dl>
</section>

<section class="card wide">
  <div class="row"><h2>Live activity</h2><label class="sub"><input type="checkbox" id="showReads" checked> show reads</label></div>
  <ul id="log"></ul>
</section>

<section class="card">
  <h2>Put a file on the drive</h2>
  <div class="tabs"><button id="tUp" class="on">Upload to flash</button><button id="tSt">Stream from URL</button></div>
  <div id="pUp">
    <div id="drop"><b>Drop a print file here</b><br>or click to choose. Replaces what is on the drive. <span id="cap"></span></div>
    <input id="pick" type="file" hidden>
  </div>
  <div id="pSt" hidden>
    <form id="sform">
      <label class="sub">File URL (server must support HTTP Range)<input type="url" name="url" placeholder="http://192.168.1.10:8765/f/model.goo" required></label>
      <button class="primary">Stream it</button>
    </form>
    <p class="sub">From a PC: <code>python tools/slausb.py stream model.goo</code> serves the file and points the drive at it. Keep that PC awake while printing.</p>
  </div>
  <div class="bar" id="pbar" hidden><i id="prog"></i></div>
  <div id="msg"></div>
</section>

<section class="card">
  <h2>Device</h2>
  <div class="btns" style="margin-bottom:14px"><button id="replug">Re-plug USB</button><button id="clear">Clear drive</button></div>
  <dl id="dev"></dl>
  <details style="margin-top:14px"><summary>Wi-Fi settings</summary>
    <form id="wifi" style="margin-top:10px">
      <label class="sub">Network name (SSID)<input type="text" name="ssid" required></label>
      <label class="sub">Password<input type="password" name="pass"></label>
      <button class="primary">Save and restart</button>
    </form>
  </details>
  <details style="margin-top:10px"><summary>Firmware update</summary>
    <p class="sub">Upload <code>firmware.bin</code> from <code>.pio/build/sla-usb/</code>. The device restarts afterwards.</p>
    <input type="file" id="fw" accept=".bin">
  </details>
</section>
</main>
<script>
const $=id=>document.getElementById(id);
let st=null,busyUp=false,lastEv=0,fileKey='',evs=new Map();
const fmt=b=>b==null?'–':b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':b<1073741824?(b/1048576).toFixed(1)+' MB':(b/1073741824).toFixed(2)+' GB';
const dur=s=>{s=Math.round(s);const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h?h+' h '+m+' min':m?m+' min '+(s%60)+' s':s+' s'};
const ago=ms=>ms==null?'never':ms<1500?'just now':dur(ms/1000)+' ago';
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const dl=(el,rows)=>{el.innerHTML=rows.filter(r=>r).map(([k,v])=>'<dt>'+esc(k)+'</dt><dd>'+esc(v)+'</dd>').join('')};
function msg(t,cls){$('msg').textContent=t;$('msg').className=cls||''}

async function preview(){
  const c=$('pv').getContext('2d');c.fillStyle='#000';c.fillRect(0,0,116,116);
  if(!st.goo)return;
  try{const r=await fetch('/api/preview');if(!r.ok)return;
    const d=new Uint8Array(await r.arrayBuffer()),img=c.createImageData(116,116);
    for(let i=0;i<116*116;i++){const p=d[2*i]<<8|d[2*i+1];
      img.data[4*i]=(p>>11&31)*255/31;img.data[4*i+1]=(p>>5&63)*255/63;img.data[4*i+2]=(p&31)*255/31;img.data[4*i+3]=255}
    c.putImageData(img,0,0)}catch(e){}
}

async function refresh(){
  try{st=await (await fetch('/api/status')).json()}catch(e){$('hdr').textContent='offline';return}
  const f=st.file,g=st.goo,p=st.progress,u=st.usb;
  $('hdr').textContent=(st.wifi.ip||(st.wifi.setupAp?'setup mode':''))+' · up '+dur(st.uptime);
  // file card
  const src=$('src');
  if(!f){src.textContent='empty';src.className='pill mute';$('fname').textContent='Drive is empty';$('fsize').textContent='';$('meta').innerHTML=''}
  else{
    src.textContent=f.kind=='stream'?'streamed':'in flash';src.className='pill '+(f.kind=='stream'?'warn':'ok');
    $('fname').textContent=f.name;$('fsize').textContent=fmt(f.size)+(f.kind=='stream'?' · '+f.url:'');
    dl($('meta'),g?[['Printer',g.machine],['Layers',g.layers+(g.known<g.layers?' ('+g.known+' indexed)':'')],['Layer height',(g.layerHeight*1000).toFixed(0)+' µm'],
      ['Exposure',g.exposure+' s'],['Resolution',g.resX+' × '+g.resY],['Print time',dur(g.printTime)],['Resin',g.volumeMl.toFixed(1)+' ml · '+g.grams.toFixed(1)+' g'],
      ['Sliced',g.created+' · '+g.software]]:[['Format','not a .goo file (no metadata)']]);
  }
  const key=f?f.name+f.size+f.mtime+(g?1:0):'';if(key!=fileKey){fileKey=key;preview()}
  // printer card
  const U=$('usb');
  if(u.restarting){U.textContent='Re-plugging…';U.className='pill warn'}
  else if(st.upload.active){U.textContent='Writing file…';U.className='pill warn'}
  else if(!u.attached){U.textContent='Detached';U.className='pill bad'}
  else if(!u.hostConnected){U.textContent='No USB host';U.className='pill bad'}
  else if(u.busy){U.textContent='Printer reading';U.className='pill warn'}
  else{U.textContent='Connected';U.className='pill ok'}
  if(g&&p.layer>=0){
    const pct=100*(p.layer+1)/g.layers;
    $('layer').textContent='Layer '+(p.layer+1)+' / '+g.layers;
    $('layersub').textContent='Z '+((p.layer+1)*g.layerHeight).toFixed(2)+' mm · '+pct.toFixed(1)+'% · about '+dur(g.printTime*(1-pct/100))+' left';
    $('lbar').style.width=pct+'%';
  }else if(f&&p.msSinceFileRead!=null){
    const pct=100*p.fileOffset/f.size;$('layer').textContent=pct.toFixed(1)+'%';$('layersub').textContent='file position '+fmt(p.fileOffset);$('lbar').style.width=pct+'%';
  }else{$('layer').textContent='–';$('layersub').textContent=f?'printer has not read the file yet':'';$('lbar').style.width='0'}
  const s=st.stream;
  dl($('stats'),[['Last file read',ago(p.msSinceFileRead)],['File data read',fmt(p.fileBytesRead)],['All disk reads',fmt(p.diskBytesRead)],
    s&&['Stream cache',s.cached+' / '+s.slots+' × '+fmt(s.chunk)+' · '+s.kbps+' KB/s'],
    s&&['Network',s.fetched+' chunks, '+s.errors+' errors, printer waited '+s.hostWaits+'×'],
    s&&s.lastError&&['Last error',s.lastError+' ('+ago(s.lastErrorAgo)+')']]);
  // device card
  $('cap').textContent='Up to '+fmt(st.volume.flashCapacity)+'.';
  dl($('dev'),[['Wi-Fi',st.wifi.ssid?st.wifi.ssid+' ('+st.wifi.rssi+' dBm)':'not connected'],['USB',u.hostConnected?'host connected, '+fmt(u.sectorsRead*512)+' read':'no host'],
    ['Volume','FAT32 '+fmt(st.volume.bytes)+', '+fmt(st.volume.cluster)+' clusters'],['Host writes',st.volume.overlaySectors+' sectors (kept in RAM)'],
    ['Memory',fmt(st.heap)+' heap, '+fmt(st.psram)+' PSRAM free']]);
}

async function refreshMap(){
  const c=$('map'),x=c.getContext('2d'),cs=getComputedStyle(document.documentElement);
  const col=[cs.getPropertyValue('--line'),cs.getPropertyValue('--acc2'),cs.getPropertyValue('--acc'),cs.getPropertyValue('--acc')];
  let m;try{m=await (await fetch('/api/map?n=400')).json()}catch(e){return}
  x.clearRect(0,0,c.width,c.height);if(!m.size)return;
  const w=c.width/m.map.length;
  for(let i=0;i<m.map.length;i++){x.fillStyle=col[+m.map[i]];x.fillRect(i*w,0,Math.ceil(w),c.height)}
  if(st&&st.file&&st.progress.msSinceFileRead!=null){x.fillStyle=cs.getPropertyValue('--bad');x.fillRect(c.width*st.progress.fileOffset/st.file.size-1,0,2,c.height)}
}

function evText(e){
  if(e.kind=='read'||e.kind=='write'){
    let t;
    if(e.region=='file'){t='file '+fmt(e.a)+' – '+fmt(e.b);if(e.la>=0)t+=e.la==e.lb?'  · layer '+(e.la+1):'  · layers '+(e.la+1)+'–'+(e.lb+1)}
    else t=e.region+' @ sector '+(e.a/512)+' ('+fmt(e.b-e.a)+')';
    return t+(e.n>1?'  ×'+e.n:'');
  }
  return e.text;
}
async function refreshEvents(){
  let r;try{r=await (await fetch('/api/events?since='+lastEv)).json()}catch(e){return}
  const now=Date.now();
  for(const e of r.events){e.at=now-e.ago;evs.set(e.id,e)}
  lastEv=Math.max(1,r.next-1);
  while(evs.size>300)evs.delete(evs.keys().next().value);
  const show=$('showReads').checked,log=$('log'),atTop=log.scrollTop<10;
  log.innerHTML=[...evs.values()].reverse().filter(e=>show||e.kind!='read').map(e=>{
    const d=new Date(e.at);
    return '<li><span class="t">'+d.toTimeString().slice(0,8)+'</span><span class="k '+e.kind+'">'+e.kind+'</span><span>'+esc(evText(e))+'</span></li>'}).join('');
  if(atTop)log.scrollTop=0;
}

function busyOk(what){return !st||!st.usb.busy||confirm('The printer is reading the drive and may be printing. '+what+' will interrupt it. Continue?')}
function force(){return st&&st.usb.busy?'?force=1':''}

function sendFile(url,f,label,done){
  const fd=new FormData();fd.append('file',f,f.name);
  const x=new XMLHttpRequest(),t0=Date.now();
  busyUp=true;$('pbar').hidden=false;$('prog').style.width='0';
  x.upload.onprogress=e=>{if(!e.lengthComputable)return;const r=e.loaded/Math.max((Date.now()-t0)/1000,.1);
    $('prog').style.width=(100*e.loaded/e.total)+'%';msg(label+': '+fmt(e.loaded)+' / '+fmt(e.total)+'  ('+fmt(r)+'/s)')};
  x.onload=()=>{busyUp=false;$('pbar').hidden=true;let r={};try{r=JSON.parse(x.responseText)}catch(e){}
    if(x.status==200)done(r,(Date.now()-t0)/1000);else msg(r.error||('Failed ('+x.status+')'),'bad');refresh()};
  x.onerror=()=>{busyUp=false;$('pbar').hidden=true;msg('Connection lost.','bad');refresh()};
  x.open('POST',url+force());x.send(fd);
}
function upload(f){
  if(busyUp||!f)return;
  if(st&&f.size>st.volume.flashCapacity){msg(f.name+' is '+fmt(f.size)+'; flash holds '+fmt(st.volume.flashCapacity)+'. Stream it instead.','bad');return}
  if(!busyOk('Replacing the file'))return;
  sendFile('/upload',f,'Uploading '+f.name,(r,s)=>msg('Done: '+r.name+' ('+fmt(r.size)+') in '+s.toFixed(1)+' s. The printer sees it now.','ok'));
}
const d=$('drop');
d.onclick=()=>$('pick').click();
$('pick').onchange=e=>{upload(e.target.files[0]);e.target.value=''};
d.ondragover=e=>{e.preventDefault();d.classList.add('over')};
d.ondragleave=()=>d.classList.remove('over');
d.ondrop=e=>{e.preventDefault();d.classList.remove('over');upload(e.dataTransfer.files[0])};
$('tUp').onclick=()=>{$('tUp').className='on';$('tSt').className='';$('pUp').hidden=false;$('pSt').hidden=true};
$('tSt').onclick=()=>{$('tSt').className='on';$('tUp').className='';$('pSt').hidden=false;$('pUp').hidden=true};
$('sform').onsubmit=async e=>{e.preventDefault();if(!busyOk('Switching files'))return;
  msg('Connecting to the file server…');
  const r=await fetch('/api/stream'+force(),{method:'POST',body:new URLSearchParams(new FormData(e.target))});
  const j=await r.json().catch(()=>({}));
  msg(r.ok?'Streaming '+j.name+' ('+fmt(j.size)+').':(j.error||'Failed.'),r.ok?'ok':'bad');refresh()};
async function post(url,what){
  if(!busyOk(what))return;
  const r=await fetch(url+force(),{method:'POST'});const j=await r.json().catch(()=>({}));
  msg(r.ok?what+' done.':(j.error||what+' failed.'),r.ok?'ok':'bad');refresh();
}
$('replug').onclick=()=>post('/api/replug','Re-plugging USB');
$('clear').onclick=()=>{if(confirm('Remove the file from the drive?'))post('/api/clear','Clearing the drive')};
$('wifi').onsubmit=async e=>{e.preventDefault();
  await fetch('/api/wifi',{method:'POST',body:new URLSearchParams(new FormData(e.target))}).catch(()=>{});
  msg('Saved. The device is restarting and will join the new network.','ok')};
$('fw').onchange=e=>{const f=e.target.files[0];e.target.value='';if(!f||busyUp)return;
  if(!busyOk('Updating firmware'))return;
  sendFile('/api/firmware',f,'Updating firmware',()=>msg('Firmware installed. Restarting… (page reconnects automatically)','ok'))};
$('showReads').onchange=()=>{lastEv=0;evs.clear();refreshEvents()};

refresh().then(refreshMap);refreshEvents();
setInterval(()=>{if(!busyUp){refresh();refreshEvents()}},1000);
setInterval(()=>{if(!busyUp)refreshMap()},2500);
</script></body></html>
)HTML";
