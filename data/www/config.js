const $=id=>document.getElementById(id);
let loadedConfig=null;
let toastTimer;
let currentDirectory='/www';
let detectedImei='';

function notify(message,error=false){const toast=$('toast');toast.textContent=message;toast.className=`toast show${error?' error':''}`;clearTimeout(toastTimer);toastTimer=setTimeout(()=>toast.className='toast',4500)}
function value(id){return $(id).value.trim()}
function number(id){return Number($(id).value)||0}
function checked(id){return $(id).checked}
function setValue(id,v=''){$(id).value=v??''}
function setChecked(id,v){$(id).checked=Boolean(v)}
function radio(name,dhcp){document.querySelector(`input[name="${name}"][value="${dhcp?'dhcp':'static'}"]`).checked=true}
function isDhcp(name){return document.querySelector(`input[name="${name}"]:checked`).value==='dhcp'}
function secret(id,stored){const entered=value(id);return entered||(stored==='***'?'***':'')}
function updateMqttTopic(){const base=value('mqttBaseTopic')||'mione';$('mqttDeviceTopic').value=detectedImei?`${base}/modems/${detectedImei}`:'IMEI nicht verfügbar'}

function refreshVisibility(){
  $('wifiStatic').classList.toggle('hidden',isDhcp('wifiIpMode'));
  $('ethernetStatic').classList.toggle('hidden',isDhcp('ethernetIpMode'));
  document.querySelector('[data-feature="wifi"]').dataset.disabled=!checked('wifiEnabled');
  document.querySelector('[data-feature="ethernet"]').dataset.disabled=!checked('ethernetEnabled');
  document.querySelector('[data-feature="cellular"]').dataset.disabled=!checked('cellularEnabled');
  document.querySelector('[data-feature="mqtt"]').dataset.disabled=!checked('mqttEnabled');
  document.querySelector('[data-feature="offlineTcp"]').dataset.disabled=!checked('offlineTcpEnabled');
}

function fillForm(c){
  loadedConfig=c;
  setValue('deviceId',c.deviceId);setChecked('wifiEnabled',c.wifi.enabled);setValue('wifiSsid',c.wifi.ssid);
  radio('wifiIpMode',c.wifi.ip.dhcp);setValue('wifiAddress',c.wifi.ip.address);setValue('wifiGateway',c.wifi.ip.gateway);setValue('wifiSubnet',c.wifi.ip.subnet);setValue('wifiDns',c.wifi.ip.dns);
  setChecked('ethernetEnabled',c.ethernet.enabled);radio('ethernetIpMode',c.ethernet.ip.dhcp);setValue('ethernetAddress',c.ethernet.ip.address);setValue('ethernetGateway',c.ethernet.ip.gateway);setValue('ethernetSubnet',c.ethernet.ip.subnet);setValue('ethernetDns',c.ethernet.ip.dns);
  setChecked('sdEnabled',c.hardware.sd);setChecked('rtcEnabled',c.hardware.rtc);setChecked('displayEnabled',c.hardware.display);setValue('logIntervalSeconds',c.logging?.intervalSeconds||10);
  setChecked('cellularEnabled',c.cellular.enabled);setValue('apn',c.cellular.apn);setValue('apnUser',c.cellular.user);
  setChecked('mqttEnabled',c.mqtt.enabled);setValue('mqttHost',c.mqtt.host);setValue('mqttPort',c.mqtt.port);setValue('mqttUser',c.mqtt.user);setValue('mqttBaseTopic',c.mqtt.baseTopic);
  setChecked('offlineTcpEnabled',c.offlineTcp.enabled);setValue('offlineTcpHost',c.offlineTcp.host);setValue('offlineTcpPort',c.offlineTcp.port);
  setChecked('alarmProgressEnabled',c.notifications?.alarmProgress!==false);
  setValue('webUser',c.web.user);setChecked('updateCheckEnabled',c.update.checkEnabled);setValue('updateManifestUrl',c.update.manifestUrl);setValue('updateCheckMinutes',c.update.checkMinutes);
  $('deviceTitle').textContent=c.deviceId;refreshVisibility();updateMqttTopic();
}

function ipConfig(prefix,name){return{dhcp:isDhcp(name),address:value(`${prefix}Address`),gateway:value(`${prefix}Gateway`),subnet:value(`${prefix}Subnet`)||'255.255.255.0',dns:value(`${prefix}Dns`)||'1.1.1.1'}}
function buildConfig(){const c=loadedConfig;return{
  schema:c.schema,deviceId:value('deviceId'),provisioned:true,
  wifi:{enabled:checked('wifiEnabled'),ssid:value('wifiSsid'),password:secret('wifiPassword',c.wifi.password),ip:ipConfig('wifi','wifiIpMode')},
  ethernet:{enabled:checked('ethernetEnabled'),ip:ipConfig('ethernet','ethernetIpMode')},
  hardware:{sd:checked('sdEnabled'),rtc:checked('rtcEnabled'),display:checked('displayEnabled')},
  logging:{intervalSeconds:number('logIntervalSeconds')},
  cellular:{enabled:checked('cellularEnabled'),simPin:secret('simPin',c.cellular.simPin),apn:value('apn'),user:value('apnUser'),password:secret('apnPassword',c.cellular.password)},
  mqtt:{enabled:checked('mqttEnabled'),host:value('mqttHost'),port:number('mqttPort'),user:value('mqttUser'),password:secret('mqttPassword',c.mqtt.password),baseTopic:value('mqttBaseTopic')},
  offlineTcp:{enabled:checked('offlineTcpEnabled'),host:value('offlineTcpHost'),port:number('offlineTcpPort'),secret:secret('commandSecret',c.offlineTcp.secret)},
  notifications:{alarmProgress:checked('alarmProgressEnabled')},
  web:{user:value('webUser'),password:secret('webPassword',c.web.password)},
  update:{checkEnabled:checked('updateCheckEnabled'),manifestUrl:value('updateManifestUrl'),checkMinutes:number('updateCheckMinutes')}
}}

async function api(url,options){const response=await fetch(url,options);let body={};try{body=await response.json()}catch{body={error:`HTTP ${response.status}`}}if(!response.ok)throw new Error(body.error||body.message||`HTTP ${response.status}`);return body}
function renderMqttConnection(mqtt){const panel=$('mqttConnection');const disabled=mqtt.code===-10;const state=mqtt.connected?'connected':disabled?'disabled':mqtt.code===-1?'waiting':'error';panel.className=`mqtt-connection ${state}`;$('mqttConnectionTitle').textContent=mqtt.connected?'MQTT verbunden':disabled?'MQTT deaktiviert':state==='waiting'?'Verbindung wird vorbereitet':'MQTT nicht verbunden';$('mqttConnectionMessage').textContent=`${mqtt.message||'Kein Status verfügbar'}${mqtt.transport?` über ${mqtt.transport}`:''}`;$('mqttConnectionCode').textContent=mqtt.code>=-4&&mqtt.code<=5?`Code ${mqtt.code}`:'Lokal'}
async function loadStatus(){try{const s=await api('/api/status');const paths={wifi:'WLAN',ethernet:'Ethernet',cellular:'Mobilfunk',offline:'Offline'};detectedImei=s.modemImei||'';updateMqttTopic();$('connectionState').classList.add('online');$('connectionState').querySelector('span').textContent='Gerät verbunden';$('serialNumber').textContent=s.serialNumber||'–';$('modemImei').textContent=detectedImei||'Nicht verfügbar';$('modemType').value=s.modemModel||'Nicht erkannt';$('wifiStatus').textContent=s.wifiIp||'Nicht verbunden';$('ethernetStatus').textContent=s.ethernetIp||'Nicht verbunden';$('cellularStatus').textContent=s.cellular?'Verbunden':'Nicht verbunden';$('mqttStatus').textContent=s.mqtt?'Verbunden':'Getrennt';renderMqttConnection(s.mqttConnection||{connected:s.mqtt,code:s.mqtt?0:-1,message:s.mqtt?'Verbunden':'Getrennt',transport:''});$('dateTimeStatus').textContent=s.dateTime||'–';$('internetStatus').textContent=paths[s.internet]||s.internet||'–';$('currentFirmware').textContent=s.update.currentVersion;$('updateMessage').textContent=s.update.message||'Noch nicht geprüft';$('approveUpdate').hidden=!s.update.available||s.update.installing}catch(error){$('connectionState').querySelector('span').textContent='Gerät nicht erreichbar';notify(error.message,true)}}
function secondsAsTime(seconds){seconds=Number(seconds);if(seconds===86400)seconds=0;const h=Math.floor(seconds/3600)%24,m=Math.floor(seconds%3600/60),s=seconds%60;return[h,m,s].map(v=>String(v).padStart(2,'0')).join(':')}
function renderMioneLive(routing){const heartbeat=routing.heartbeat||{},heartbeatPanel=$('heartbeatStatus');heartbeatPanel.className=`mione-live ${heartbeat.online?'ok':heartbeat.received?'error':'waiting'}`;$('heartbeatTitle').textContent=heartbeat.online?`Aktiv · ${heartbeat.ageSeconds}s`:heartbeat.received?`Gestört · ${heartbeat.ageSeconds}s`:'Kein Signal';$('heartbeatMessage').textContent=`${heartbeat.message||'Noch nicht empfangen'} · ${heartbeat.topic||''}`;const imei=routing.imeiCheck||{},imeiPanel=$('imeiMatchStatus');imeiPanel.className=`mione-live ${imei.matches?'ok':imei.received?'error':'waiting'}`;$('imeiMatchTitle').textContent=imei.matches?'IMEI stimmt überein':imei.received?'IMEI stimmt nicht überein':'Noch keine IMEI empfangen';$('imeiMatchMessage').textContent=`Lokal: ${imei.local||'–'} · MiOne: ${imei.configured||'–'} · ${imei.topic||''}`}
async function loadAlarmRouting(){try{const routing=await api('/api/alarm-routing');renderMioneLive(routing);$('technicalWindow').textContent=`${secondsAsTime(routing.technicalFrom)} – ${secondsAsTime(routing.technicalUntil)}`;const sync=$('routingSync');sync.className=`routing-sync ${routing.lastReceivedMs?'received':routing.subscriptionReady?'waiting':'error'}`;sync.querySelector('b').textContent=routing.lastReceivedMs?'Mobile-Konfiguration empfangen':routing.subscriptionReady?'Warte auf MiOne-Konfiguration':'Topic nicht abonniert';$('routingSyncMessage').textContent=`${routing.syncMessage||'–'} · ${routing.sourceTopic||''}`;$('mobileSlots').innerHTML=routing.mobileSlots.map(slot=>`<div class="mobile-slot${slot.active?' active':''}"><span class="slot-number">Slot ${slot.slot}<i></i></span><b>${escapeHtml(slot.number||'Nicht belegt')}</b><small>${slot.active?escapeHtml(slot.delivery):'Deaktiviert'}</small></div>`).join('')}catch(error){$('mobileSlots').innerHTML=`<p class="empty">${escapeHtml(error.message)}</p>`}}

async function loadFiles(){const list=$('fileList');$('currentPath').textContent=currentDirectory;$('parentDirectory').hidden=currentDirectory==='/www'||currentDirectory==='/firmware';list.innerHTML='<p class="empty">Dateiliste wird geladen …</p>';try{const data=await api(`/api/files?path=${encodeURIComponent(currentDirectory)}`);const files=(data.files||[]).sort((a,b)=>Number(b.directory)-Number(a.directory)||a.name.localeCompare(b.name,'de',{sensitivity:'base'}));if(!files.length){list.innerHTML='<p class="empty">Dieser Ordner ist leer.</p>';return}list.innerHTML='<div class="file-head"><span>Name</span><span>Größe</span><span>Aktionen</span></div>'+files.map(file=>{const path=file.path||(file.name.startsWith('/')?file.name:`${currentDirectory}/${file.name}`);const encoded=encodeURIComponent(path);const icon=file.directory?'DIR':'FILE';const detail=file.directory?'Ordner':fileType(file.name);const actions=file.directory?`<button type="button" data-open="${encoded}">Öffnen</button>`:`<a href="/api/file?path=${encoded}">Herunterladen</a><button type="button" data-delete="${encoded}">Löschen</button>`;return `<div class="file-row"><div class="file-name"><i class="${file.directory?'folder':'document'}">${icon}</i><span><b>${escapeHtml(file.name)}</b><small>${detail}</small></span></div><span class="file-size">${file.directory?'–':formatBytes(file.size)}</span><div class="file-actions">${actions}</div></div>`}).join('')}catch(error){list.innerHTML=`<p class="empty">${escapeHtml(error.message)}</p>`}}
function escapeHtml(text){const div=document.createElement('div');div.textContent=text;return div.innerHTML}
function formatBytes(bytes){if(bytes<1024)return`${bytes} B`;if(bytes<1048576)return`${(bytes/1024).toFixed(1)} KB`;return`${(bytes/1048576).toFixed(1)} MB`}
function fileType(name){const extension=name.includes('.')?name.split('.').pop().toUpperCase():'DATEI';return `${extension}-Datei`}

document.querySelectorAll('input[type="radio"],.switch input').forEach(input=>input.addEventListener('change',refreshVisibility));
$('mqttBaseTopic').addEventListener('input',updateMqttTopic);
document.querySelectorAll('.reveal').forEach(button=>button.addEventListener('click',()=>{const input=button.previousElementSibling;const visible=input.type==='text';input.type=visible?'password':'text';button.textContent=visible?'Anzeigen':'Verbergen'}));
document.querySelectorAll('.sidebar a').forEach(link=>link.addEventListener('click',()=>{document.querySelectorAll('.sidebar a').forEach(a=>a.classList.remove('active'));link.classList.add('active')}));
$('configForm').addEventListener('submit',async event=>{event.preventDefault();if(!event.currentTarget.reportValidity())return;const button=$('saveButton');button.disabled=true;button.textContent='Wird gespeichert …';try{await api('/api/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(buildConfig())});notify('Konfiguration gespeichert. Das Gerät startet neu.')}catch(error){notify(error.message,true);button.disabled=false;button.textContent='Konfiguration speichern'}});
$('checkUpdate').addEventListener('click',async()=>{try{await api('/api/firmware/check',{method:'POST'});notify('Firmwareprüfung wurde gestartet.');setTimeout(loadStatus,1800)}catch(error){notify(error.message,true)}});
$('approveUpdate').addEventListener('click',async()=>{if(!confirm('Firmwareupdate jetzt laden und installieren?'))return;try{await api('/api/firmware/approve',{method:'POST'});notify('Update bestätigt. Download wird gestartet.');setTimeout(loadStatus,1000)}catch(error){notify(error.message,true)}});
$('fileDirectory').addEventListener('change',()=>{currentDirectory=value('fileDirectory');loadFiles()});$('refreshFiles').addEventListener('click',loadFiles);
$('parentDirectory').addEventListener('click',()=>{currentDirectory=currentDirectory.substring(0,currentDirectory.lastIndexOf('/'))||value('fileDirectory');loadFiles()});
$('fileUpload').addEventListener('change',async event=>{const file=event.target.files[0];if(!file)return;const path=`${currentDirectory}/${file.name}`;const data=new FormData();data.append('file',file);try{await api(`/api/file?path=${encodeURIComponent(path)}`,{method:'POST',body:data});notify(`${file.name} wurde hochgeladen.`);loadFiles()}catch(error){notify(error.message,true)}event.target.value=''});
$('fileList').addEventListener('click',async event=>{const open=event.target.dataset.open;if(open){currentDirectory=decodeURIComponent(open);loadFiles();return}const path=event.target.dataset.delete;if(!path||!confirm('Datei wirklich löschen?'))return;try{await api(`/api/file?path=${path}`,{method:'DELETE'});notify('Datei gelöscht.');loadFiles()}catch(error){notify(error.message,true)}});

Promise.all([api('/api/config').then(fillForm),loadStatus(),loadAlarmRouting(),loadFiles()]).catch(error=>notify(error.message,true));
setInterval(()=>{loadStatus();loadAlarmRouting()},5000);
