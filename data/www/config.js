const $=id=>document.getElementById(id);
let loadedConfig=null;
let loadedSnapshot='';
let toastTimer;
let currentDirectory='/www';
let otaInProgress=false;
let updatePollTimer;
let reloadAfterReconnect=false;
const stableManifestUrl='https://github.com/mrcrash112/ESP32-GSM-AE08-R-L/releases/latest/download/firmware.json';
const betaManifestUrl='https://github.com/mrcrash112/ESP32-GSM-AE08-R-L/releases/download/beta/firmware.json?v=0.1.67';

function notify(message,error=false){const toast=$('toast');toast.textContent=message;toast.className=`toast show${error?' error':''}`;clearTimeout(toastTimer);toastTimer=setTimeout(()=>toast.className='toast',4500)}
function value(id){return $(id).value.trim()}
function number(id){return Number($(id).value)||0}
function checked(id){return $(id).checked}
function setValue(id,v=''){$(id).value=v??''}
function setChecked(id,v){$(id).checked=Boolean(v)}
function radio(name,dhcp){document.querySelector(`input[name="${name}"][value="${dhcp?'dhcp':'static'}"]`).checked=true}
function isDhcp(name){return document.querySelector(`input[name="${name}"]:checked`).value==='dhcp'}
function secret(id,stored){const entered=value(id);return entered||(stored==='***'?'***':'')}
function updateManifestForChannel(){const current=value('updateManifestUrl'),channel=value('updateChannel');if(current&&current!==stableManifestUrl&&current!==betaManifestUrl)return;setValue('updateManifestUrl',channel==='beta'?betaManifestUrl:stableManifestUrl);updateSaveBar()}

function setSectionOpen(section,open){
  section.classList.toggle('collapsed',!open);
  const button=section.querySelector(':scope>.section-heading .section-toggle');
  if(button){button.setAttribute('aria-expanded',String(open));button.setAttribute('aria-label',open?'Sektion einklappen':'Sektion ausklappen')}
}

function openSection(id){
  const section=$(id);
  if(section?.classList.contains('collapsible'))setSectionOpen(section,true);
}

function setupCollapsibleSections(){
  document.querySelectorAll('#configForm>.card').forEach((section,index)=>{
    const heading=section.querySelector(':scope>.section-heading');
    if(!heading)return;
    const body=document.createElement('div');
    body.className='section-body';
    while(heading.nextSibling)body.appendChild(heading.nextSibling);
    section.appendChild(body);
    section.classList.add('collapsible');
    const button=document.createElement('button');
    button.type='button';
    button.className='section-toggle';
    button.innerHTML='<span></span>';
    button.addEventListener('click',()=>setSectionOpen(section,section.classList.contains('collapsed')));
    heading.appendChild(button);
    setSectionOpen(section,index===0);
  });
}

function buildAlarmInputUi(){
  const container=$('alarmInputs');
  const delayOptions=[[0,'Sofort'],[300,'5 Min'],[600,'10 Min'],[900,'15 Min']];
  const deliveryOptions=[[0,'Aus'],[1,'SMS'],[2,'Anruf'],[3,'SMS u. Anruf']];
  container.innerHTML=Array.from({length:4},(_,i)=>{
    const input=i+1;
    const recipients=Array.from({length:5},(_,slot)=>`<div class="recipient-row"><label><span>Rufnummer ${slot+1}</span><input id="alarmInput${input}Number${slot+1}" type="tel" maxlength="32" placeholder="+491…"></label><label><span>Weg</span><select id="alarmInput${input}Delivery${slot+1}">${deliveryOptions.map(([v,t])=>`<option value="${v}">${t}</option>`).join('')}</select></label></div>`).join('');
    return `<article class="input-alarm"><div class="input-alarm-head"><div><h3>Digitaleingang ${input}</h3><p>Pin ${input}</p></div><label class="switch"><input id="alarmInput${input}Enabled" type="checkbox"><span></span><b>Aktiv</b></label></div><div class="fields two"><label><span>Alarmierung bei</span><select id="alarmInput${input}Trigger"><option value="open">Öffnen</option><option value="close">Schließen</option></select></label><label><span>Verzögerungszeit</span><select id="alarmInput${input}Delay">${delayOptions.map(([v,t])=>`<option value="${v}">${t}</option>`).join('')}</select></label><label class="wide"><span>Alarmtext</span><input id="alarmInput${input}Text" type="text" maxlength="160" placeholder="Alarm Digitaleingang ${input}"><small><span id="alarmInput${input}TextCount">0</span>/160 Zeichen</small></label></div><div class="recipient-grid">${recipients}</div></article>`;
  }).join('');
  for(let input=1;input<=4;input++){$(`alarmInput${input}Text`).addEventListener('input',()=>{$(`alarmInput${input}TextCount`).textContent=value(`alarmInput${input}Text`).length})}
}

function fillAlarmInputs(inputs=[]){
  for(let input=1;input<=4;input++){
    const cfg=inputs.find(item=>Number(item.index)===input)||{};
    setChecked(`alarmInput${input}Enabled`,cfg.enabled);
    setValue(`alarmInput${input}Trigger`,cfg.trigger||'close');
    setValue(`alarmInput${input}Delay`,String(cfg.delaySeconds??0));
    setValue(`alarmInput${input}Text`,cfg.text||'');
    $(`alarmInput${input}TextCount`).textContent=value(`alarmInput${input}Text`).length;
    const recipients=cfg.recipients||[];
    for(let slot=1;slot<=5;slot++){
      const recipient=recipients[slot-1]||{};
      setValue(`alarmInput${input}Number${slot}`,recipient.number||'');
      setValue(`alarmInput${input}Delivery${slot}`,String(recipient.delivery??0));
    }
  }
}

function buildAlarmInputs(){
  return Array.from({length:4},(_,i)=>{
    const input=i+1;
    return{index:input,enabled:checked(`alarmInput${input}Enabled`),trigger:value(`alarmInput${input}Trigger`),text:value(`alarmInput${input}Text`).slice(0,160),delaySeconds:number(`alarmInput${input}Delay`),recipients:Array.from({length:5},(_,slot)=>({number:value(`alarmInput${input}Number${slot+1}`),delivery:number(`alarmInput${input}Delivery${slot+1}`)}))};
  });
}

function refreshVisibility(){
  $('wifiStatic').classList.toggle('hidden',isDhcp('wifiIpMode'));
  $('ethernetStatic').classList.toggle('hidden',isDhcp('ethernetIpMode'));
  document.querySelector('[data-feature="wifi"]').dataset.disabled=!checked('wifiEnabled');
  document.querySelector('[data-feature="ethernet"]').dataset.disabled=!checked('ethernetEnabled');
  document.querySelector('[data-feature="mqtt"]').dataset.disabled=!checked('mqttEnabled');
  const mqttServiceEnabled=checked('mqttServiceEnabled');
  const mqttBrokerSection=$('mqttBrokerSection');
  mqttBrokerSection.dataset.disabled=!mqttServiceEnabled;
  mqttBrokerSection.querySelectorAll('input,select,button,textarea').forEach(element=>{
    if(element.id==='mqttServiceEnabled') return;
    element.disabled=!mqttServiceEnabled;
  });
  document.querySelector('[data-feature="offlineTcp"]').dataset.disabled=!checked('offlineTcpEnabled');
  $('mqttBaseTopic').required=checked('mqttEnabled');
  $('mqttTopTopic').required=mqttServiceEnabled;
}

function fillForm(c){
  loadedConfig=c;
  setValue('deviceId',c.deviceId);setChecked('wifiEnabled',c.wifi.enabled);setValue('wifiSsid',c.wifi.ssid);
  radio('wifiIpMode',c.wifi.ip.dhcp);setValue('wifiAddress',c.wifi.ip.address);setValue('wifiGateway',c.wifi.ip.gateway);setValue('wifiSubnet',c.wifi.ip.subnet);setValue('wifiDns',c.wifi.ip.dns);
  setChecked('ethernetEnabled',c.ethernet.enabled);radio('ethernetIpMode',c.ethernet.ip.dhcp);setValue('ethernetAddress',c.ethernet.ip.address);setValue('ethernetGateway',c.ethernet.ip.gateway);setValue('ethernetSubnet',c.ethernet.ip.subnet);setValue('ethernetDns',c.ethernet.ip.dns);
  setValue('mdnsName',c.mdnsName||c.deviceId);
  setChecked('sdEnabled',c.hardware.sd);setChecked('rtcEnabled',c.hardware.rtc);setChecked('displayEnabled',c.hardware.display);setValue('logIntervalSeconds',c.logging?.intervalSeconds||10);
  setValue('apn',c.cellular.apn);setValue('apnUser',c.cellular.user);
  setChecked('mqttEnabled',c.mqtt.enabled);setValue('mqttBaseTopic',c.mqtt.baseTopic);setChecked('mqttServiceEnabled',c.mqtt.serviceEnabled??Boolean(c.mqtt.topTopic));setValue('mqttTopTopic',c.mqtt.topTopic||'');setValue('mqttHost',c.mqtt.host);setValue('mqttPort',c.mqtt.port);setValue('mqttUser',c.mqtt.user);
  setChecked('offlineTcpEnabled',c.offlineTcp.enabled);setValue('offlineTcpHost',c.offlineTcp.host);setValue('offlineTcpPort',c.offlineTcp.port);
  setChecked('alarmProgressEnabled',c.notifications?.alarmProgress!==false);
  fillAlarmInputs(c.alarmInputs||[]);
  setValue('webUser',c.web.user);setChecked('updateCheckEnabled',c.update.checkEnabled);setChecked('updateCellularDownloads',c.update.cellularDownloads);setChecked('updateAutoInstall',c.update.autoInstall);setValue('updateInstallTime',c.update.installTime||'03:00');setValue('updateChannel',c.update.channel||'stable');setValue('updateManifestUrl',c.update.manifestUrl);setValue('updateCheckMinutes',c.update.checkMinutes);
  $('deviceTitle').textContent=c.deviceId;refreshVisibility();
  loadedSnapshot=configSnapshot();
  updateSaveBar();
}

function ipConfig(prefix,name){return{dhcp:isDhcp(name),address:value(`${prefix}Address`),gateway:value(`${prefix}Gateway`),subnet:value(`${prefix}Subnet`)||'255.255.255.0',dns:value(`${prefix}Dns`)||'1.1.1.1'}}
function buildConfig(){const c=loadedConfig;return{
  schema:c.schema,deviceId:value('deviceId'),provisioned:true,
  wifi:{enabled:checked('wifiEnabled'),ssid:value('wifiSsid'),password:secret('wifiPassword',c.wifi.password),ip:ipConfig('wifi','wifiIpMode')},
  mdnsName:value('mdnsName'),
  ethernet:{enabled:checked('ethernetEnabled'),ip:ipConfig('ethernet','ethernetIpMode')},
  hardware:{sd:checked('sdEnabled'),rtc:checked('rtcEnabled'),display:checked('displayEnabled')},
  logging:{intervalSeconds:number('logIntervalSeconds')},
  cellular:{enabled:true,simPin:secret('simPin',c.cellular.simPin),apn:value('apn'),user:value('apnUser'),password:secret('apnPassword',c.cellular.password)},
  mqtt:{enabled:checked('mqttEnabled'),host:value('mqttHost'),port:number('mqttPort'),user:value('mqttUser'),password:secret('mqttPassword',c.mqtt.password),baseTopic:value('mqttBaseTopic'),serviceEnabled:checked('mqttServiceEnabled'),topTopic:value('mqttTopTopic')},
  offlineTcp:{enabled:checked('offlineTcpEnabled'),host:value('offlineTcpHost'),port:number('offlineTcpPort'),secret:secret('commandSecret',c.offlineTcp.secret)},
  notifications:{alarmProgress:checked('alarmProgressEnabled')},
  alarmInputs:buildAlarmInputs(),
  web:{user:value('webUser'),password:secret('webPassword',c.web.password)},
  update:{checkEnabled:checked('updateCheckEnabled'),cellularDownloads:checked('updateCellularDownloads'),autoInstall:checked('updateAutoInstall'),installTime:value('updateInstallTime')||'03:00',channel:value('updateChannel'),manifestUrl:value('updateManifestUrl'),checkMinutes:number('updateCheckMinutes')}
}}

function configSnapshot(){return loadedConfig?JSON.stringify(buildConfig()):''}
function updateSaveBar(){if(!loadedConfig)return;$('saveBar').hidden=configSnapshot()===loadedSnapshot}
function networkError(error){return error instanceof TypeError||/load failed|failed to fetch|network/i.test(error.message||'')}
function scheduleUpdatePoll(delay=800){clearTimeout(updatePollTimer);updatePollTimer=setTimeout(async()=>{await loadStatus();if(otaInProgress)scheduleUpdatePoll()},delay)}
function updateNeedsPolling(update){return Boolean(update.installing||update.approved||update.downloading||update.downloadQueued||update.checking||(update.available&&!update.downloaded&&!update.failed&&!update.manualCheckRequired))}
function renderUpdateButton(update){const button=$('checkUpdate'),checking=Boolean(update.checking),downloading=Boolean(update.downloadQueued||update.downloading||(update.available&&!update.downloaded&&!update.failed&&!update.manualCheckRequired)),installing=Boolean(update.approved||update.installing);button.disabled=checking||downloading||installing;button.textContent=checking?'Prüfung in Arbeit …':downloading?'Download in Arbeit …':installing?'Installation in Arbeit …':'Jetzt prüfen'}

async function api(url,options){const response=await fetch(url,options);let body={};try{body=await response.json()}catch{body={error:`HTTP ${response.status}`}}if(!response.ok)throw new Error(body.error||body.message||`HTTP ${response.status}`);return body}
function renderMqttConnection(mqtt){const panel=$('mqttConnection');const disabled=mqtt.code===-10;const state=mqtt.connected?'connected':disabled?'disabled':mqtt.code===-1?'waiting':'error';panel.className=`mqtt-connection ${state}`;$('mqttConnectionTitle').textContent=mqtt.connected?'MQTT verbunden':disabled?'MQTT deaktiviert':state==='waiting'?'Verbindung wird vorbereitet':'MQTT nicht verbunden';$('mqttConnectionMessage').textContent=`${mqtt.message||'Kein Status verfügbar'}${mqtt.transport?` über ${mqtt.transport}`:''}`;$('mqttConnectionCode').textContent=mqtt.code>=-4&&mqtt.code<=5?`Code ${mqtt.code}`:'Lokal'}
function formatVersion(version){const value=String(version||'').trim();if(!value||value==='0.0.0')return'';return value.replace(/_Beta$/i,' Beta').replace(/-beta$/i,' Beta')}
async function loadStatus() {
  try {
    const s = await api('/api/status');
    if (reloadAfterReconnect) {
      reloadAfterReconnect = false;
      setTimeout(() => location.reload(), 800);
      return;
    }
    otaInProgress = updateNeedsPolling(s.update);
    renderUpdateButton(s.update);
    const paths = {
      wifi: 'WLAN',
      ethernet: 'Ethernet',
      cellular: 'Mobilfunk',
      offline: 'Offline',
    };
    const currentFirmware = formatVersion(s.update.currentVersion) || '–';
    const currentRecovery = formatVersion(s.update.recoveryVersion);
    const currentWeb = formatVersion(s.update.currentWebVersion);
    const bridge = s.appBridge || {};
    const mione = s.mioneSystem || {};
    const theApp = s.theApp || {};
    const appClients = Array.isArray(theApp.clients) ? theApp.clients : [];
    const activeClients = appClients.filter((client) => client.online);
    const activeNames = activeClients
      .map((client) => client.displayName || client.name || client.email || client.uid)
      .filter(Boolean);
    const activeNameText = activeNames.length
      ? activeNames.slice(0, 3).join(', ') + (activeNames.length > 3 ? ' …' : '')
      : '';
    const heartbeatAge = heartbeatAgeText(mione.heartbeatAgeSeconds);
    const mioneState =
      mione.enabled === false ? 'deaktiviert' : s.mqtt ? 'online' : 'offline';
    const heartbeatState =
      mione.enabled === false ? 'aus' : mione.heartbeatState || 'wartet';
    const appState = activeClients.length
      ? `${activeClients.length} online`
      : appClients.length
      ? 'offline'
      : 'wartet';
    $('connectionState').classList.add('online');
    $('connectionState').querySelector('span').textContent = 'Gerät verbunden';
    $('connectionDetail').textContent =
      `MIOne-System: ${mioneState} · Heartbeat: ${heartbeatState}${heartbeatAge ? ` (${heartbeatAge})` : ''} · ` +
      `The_App: ${appState}${activeNameText ? ` (${activeNameText})` : ''} · ` +
      `Bridge: ${bridge.online ? 'online' : 'offline'}`;
    $('serialNumber').textContent = s.serialNumber || '–';
    $('modemImei').textContent = s.modemImei || 'Nicht verfügbar';
    $('modemType').value = s.modemModel || 'Nicht erkannt';
    $('wifiStatus').textContent = s.wifiIp || 'Nicht verbunden';
    $('ethernetStatus').textContent = s.ethernetIp || 'Nicht verbunden';
    $('cellularStatus').textContent = s.cellular ? 'Verbunden' : 'Nicht verbunden';
    $('mqttStatus').textContent =
      mione.enabled === false ? 'Deaktiviert' : s.mqtt ? 'Verbunden' : 'Getrennt';
    $('appBridgeStatus').textContent = activeClients.length
      ? `${activeClients.length} online`
      : bridge.online
      ? `${bridge.source || 'The_App'} online`
      : `${bridge.source || 'The_App'} offline`;
    renderMqttConnection(
      s.mqttConnection || {
        connected: s.mqtt,
        code: s.mqtt ? 0 : -1,
        message: s.mqtt ? 'Verbunden' : 'Getrennt',
        transport: '',
      },
    );
    $('firmwareVersionStatus').textContent = currentFirmware;
    $('dateTimeStatus').textContent = s.dateTime || '–';
    $('internetStatus').textContent = paths[s.internet] || s.internet || '–';
    $('currentFirmware').textContent = currentFirmware;
    $('installedDetails').textContent =
      `Recovery: ${currentRecovery || 'Noch nicht gestartet'} · Web: ${currentWeb || 'Nicht versioniert'}`;
    const firmwareState = $('firmwareUpdateState');
    const recoveryState = $('recoveryUpdateState');
    const webState = $('webUpdateState');
    firmwareState.textContent = s.update.firmwareAvailable
      ? `Update ${formatVersion(s.update.version)} verfügbar`
      : 'Aktuell';
    recoveryState.textContent = s.update.recoveryAvailable
      ? `Update ${formatVersion(s.update.recoveryTargetVersion)} verfügbar`
      : 'Aktuell';
    webState.textContent = s.update.webAvailable
      ? `Update ${formatVersion(s.update.webTargetVersion)} verfügbar`
      : 'Aktuell';
    firmwareState.className = s.update.firmwareAvailable
      ? 'update-available'
      : 'update-current';
    recoveryState.className = s.update.recoveryAvailable
      ? 'update-available'
      : 'update-current';
    webState.className = s.update.webAvailable ? 'update-available' : 'update-current';
    const progress =
      (s.update.installing || s.update.downloading || s.update.checking || s.update.downloadQueued) &&
      s.update.progress
        ? ` (${s.update.progress}%)`
        : '';
    const ready =
      s.update.available &&
      s.update.downloaded &&
      !s.update.autoInstall &&
      !s.update.installing &&
      !s.update.approved
        ? ' · Dateien geladen'
        : '';
    $('updateMessage').textContent = s.update.failed
      ? `Fehler: ${s.update.message || s.update.detail || 'Update fehlgeschlagen'}`
      : `${s.update.message || 'Noch nicht geprüft'}${progress}${ready}${s.update.detail ? ` · ${s.update.detail}` : ''}`;
    $('approveUpdate').hidden =
      !s.update.available ||
      !s.update.downloaded ||
      s.update.failed ||
      s.update.manualCheckRequired ||
      s.update.checking ||
      s.update.downloadQueued ||
      s.update.installing ||
      s.update.approved ||
      s.update.downloading ||
      s.update.autoInstall;
    renderAppPresence(theApp);
  } catch (error) {
    if (otaInProgress && networkError(error)) {
      reloadAfterReconnect = true;
      $('connectionState').querySelector('span').textContent = 'OTA läuft / Gerät startet neu';
      $('connectionDetail').textContent = 'Bitte warten …';
      $('updateMessage').textContent = 'Verbindung während des Updates unterbrochen. Bitte warten …';
      $('approveUpdate').hidden = true;
      return;
    }
    $('connectionState').querySelector('span').textContent = 'Gerät nicht erreichbar';
    $('connectionDetail').textContent = '–';
    notify(error.message, true);
  }
}
function secondsAsTime(seconds){seconds=Number(seconds);if(seconds===86400)seconds=0;const h=Math.floor(seconds/3600)%24,m=Math.floor(seconds%3600/60),s=seconds%60;return[h,m,s].map(v=>String(v).padStart(2,'0')).join(':')}
function heartbeatAgeText(ageSeconds){const seconds=Number(ageSeconds);if(!Number.isFinite(seconds)||seconds<0)return'';if(seconds<2)return'gerade eben';if(seconds<60)return`vor ${Math.round(seconds)}s`;const minutes=Math.floor(seconds/60);return`vor ${minutes}m ${Math.round(seconds%60)}s`}
function renderMioneLive(routing){const systemEnabled=routing.systemEnabled!==false,heartbeat=routing.heartbeat||{},heartbeatPanel=$('heartbeatStatus'),heartbeatAge=heartbeatAgeText(heartbeat.ageSeconds),heartbeatAgeSeconds=Number(heartbeat.ageSeconds),heartbeatWaiting=Number.isFinite(heartbeatAgeSeconds)&&heartbeatAgeSeconds>=0&&heartbeatAgeSeconds<=60;heartbeatPanel.className=`mione-live ${systemEnabled?(heartbeat.online?'ok':heartbeat.received&&(heartbeatWaiting||heartbeat.value)?'waiting':'error'):'disabled'}`;$('heartbeatTitle').textContent=systemEnabled?(heartbeat.online?`Aktiv${heartbeatAge?` · ${heartbeatAge}`:''}`:heartbeat.received?`${heartbeatWaiting?'Ausstehend':'Gestört'}${heartbeatAge?` · ${heartbeatAge}`:''}`:'Kein Signal'):'MIOne-System deaktiviert';$('heartbeatMessage').textContent=systemEnabled?`${heartbeat.message||'Noch nicht empfangen'} · ${heartbeat.topic||''}`:'System-ID ist nicht aktiv';const imei=routing.imeiCheck||{},imeiPanel=$('imeiMatchStatus');imeiPanel.className=`mione-live ${systemEnabled?(imei.matches?'ok':imei.received?'error':'waiting'):'disabled'}`;$('imeiMatchTitle').textContent=systemEnabled?(imei.matches?'IMEI stimmt überein':imei.received?'IMEI stimmt nicht überein':'Noch keine IMEI empfangen'):'MIOne-System deaktiviert';$('imeiMatchMessage').textContent=systemEnabled?`Lokal: ${imei.local||'–'} · MiOne: ${imei.configured||'–'} · ${imei.topic||''}`:'Alarmconfig aus System ist ausgeblendet'}
function renderAppPresence(theApp){const container=$('appPresenceList'),count=$('appPresenceCount'),clients=Array.isArray(theApp?.clients)?theApp.clients:[],active=clients.filter(client=>client.online),activeCount=active.length,ordered=[...clients].sort((a,b)=>Number(Boolean(b.online))-Number(Boolean(a.online))||(()=>{const ageA=Number(a.ageSeconds);const ageB=Number(b.ageSeconds);const normA=Number.isFinite(ageA)&&ageA>=0?ageA:Number.MAX_SAFE_INTEGER;const normB=Number.isFinite(ageB)&&ageB>=0?ageB:Number.MAX_SAFE_INTEGER;return normA-normB;})());count.textContent=clients.length?`${activeCount}/${clients.length}`:'–';if(!container)return;if(!clients.length){container.innerHTML='<p class="empty">Noch keine App-Anmeldungen gefunden.</p>';return}container.innerHTML=ordered.map(client=>{const online=Boolean(client.online);const name=client.displayName||client.name||client.email||client.uid||client.installationId||'Unbekannt';const subtitle=[client.role==='owner'?'Hauptbenutzer':'Benutzer',client.email||'',client.uid||''].filter(Boolean).join(' · ');const status=online?'Online':'Offline';const age=heartbeatAgeText(client.ageSeconds);return `<div class="app-presence-item ${online?'online':'offline'}"><div class="app-presence-item-head"><div><b>${escapeHtml(name)}</b><div class="app-presence-tag"><span class="app-presence-dot"></span><span>${status}${age?` · ${escapeHtml(age)}`:''}</span></div></div><strong>${escapeHtml(client.installationId||'–')}</strong></div><small>${escapeHtml(subtitle||'Keine weiteren Angaben')}</small><small>Topic: ${escapeHtml(client.topic||'–')}</small></div>`}).join('')}
async function loadAlarmRouting(){try{const routing=await api('/api/alarm-routing');renderMioneLive(routing);$('technicalWindow').textContent=`${secondsAsTime(routing.technicalFrom)} – ${secondsAsTime(routing.technicalUntil)}`;const sync=$('routingSync'),systemEnabled=routing.systemEnabled!==false;sync.className=`routing-sync ${systemEnabled?(routing.lastReceivedMs?'received':routing.subscriptionReady?'waiting':'error'):'disabled'}`;sync.querySelector('b').textContent=systemEnabled?(routing.lastReceivedMs?'Alarmconfig aus System empfangen':routing.subscriptionReady?'Warte auf Alarmconfig aus System':'System-ID fehlt / Topic nicht abonniert'):'MIOne-System deaktiviert';$('routingSyncMessage').textContent=systemEnabled?`${routing.syncMessage||'–'} · ${routing.sourceTopic||''}`:'Keine MQTT-Synchronisierung aktiv';$('mobileSlots').innerHTML=routing.mobileSlots.map(slot=>`<div class="mobile-slot${slot.active?' active':''}"><span class="slot-number">Slot ${slot.slot}<i></i></span><b>${escapeHtml(slot.number||'Nicht belegt')}</b><small>${slot.active?escapeHtml(slot.delivery):'Deaktiviert'}</small></div>`).join('')}catch(error){if(otaInProgress&&networkError(error))return;$('mobileSlots').innerHTML=`<p class="empty">${escapeHtml(error.message)}</p>`}}

async function loadFiles(){const list=$('fileList');$('currentPath').textContent=currentDirectory;$('parentDirectory').hidden=currentDirectory==='/www'||currentDirectory==='/firmware';list.innerHTML='<p class="empty">Dateiliste wird geladen …</p>';try{const data=await api(`/api/files?path=${encodeURIComponent(currentDirectory)}`);const storage=data.storage||{},storageItems=$('storageStatus').querySelectorAll('strong');storageItems[0].textContent=formatBytes(storage.total||0);storageItems[1].textContent=formatBytes(storage.used||0);storageItems[2].textContent=formatBytes(storage.free||0);const files=(data.files||[]).filter(file=>!file.name.startsWith('.')).sort((a,b)=>Number(b.directory)-Number(a.directory)||a.name.localeCompare(b.name,'de',{sensitivity:'base'}));if(!files.length){list.innerHTML='<p class="empty">Dieser Ordner ist leer.</p>';return}list.innerHTML='<div class="file-head"><span>Name</span><span>Größe</span><span>Aktionen</span></div>'+files.map(file=>{const path=file.path||(file.name.startsWith('/')?file.name:`${currentDirectory}/${file.name}`);const encoded=encodeURIComponent(path);const icon=file.directory?'DIR':'FILE';const detail=file.directory?'Ordner':fileType(file.name);const actions=file.directory?`<button type="button" data-open="${encoded}">Öffnen</button>`:`<a href="/api/file?path=${encoded}">Herunterladen</a><button type="button" data-delete="${encoded}">Löschen</button>`;return `<div class="file-row"><div class="file-name"><i class="${file.directory?'folder':'document'}">${icon}</i><span><b>${escapeHtml(file.name)}</b><small>${detail}</small></span></div><span class="file-size">${file.directory?'–':formatBytes(file.size)}</span><div class="file-actions">${actions}</div></div>`}).join('')}catch(error){list.innerHTML=`<p class="empty">${escapeHtml(error.message)}</p>`}}
function escapeHtml(text){const div=document.createElement('div');div.textContent=text;return div.innerHTML}
function formatBytes(bytes){bytes=Number(bytes)||0;if(bytes<1024)return`${bytes} B`;if(bytes<1048576)return`${(bytes/1024).toFixed(1)} KB`;if(bytes<1073741824)return`${(bytes/1048576).toFixed(1)} MB`;return`${(bytes/1073741824).toFixed(2)} GB`}
function fileType(name){const extension=name.includes('.')?name.split('.').pop().toUpperCase():'DATEI';return `${extension}-Datei`}

setupCollapsibleSections();
buildAlarmInputUi();
document.querySelectorAll('input[type="radio"],.switch input').forEach(input=>input.addEventListener('change',()=>{refreshVisibility();updateSaveBar()}));
document.querySelectorAll('#configForm input,#configForm select').forEach(input=>{input.addEventListener('input',updateSaveBar);input.addEventListener('change',updateSaveBar)});
$('updateChannel').addEventListener('change',updateManifestForChannel);
document.querySelectorAll('.reveal').forEach(button=>button.addEventListener('click',()=>{const input=button.previousElementSibling;const visible=input.type==='text';input.type=visible?'password':'text';button.textContent=visible?'Anzeigen':'Verbergen'}));
document.querySelectorAll('.sidebar a').forEach(link=>link.addEventListener('click',()=>{document.querySelectorAll('.sidebar a').forEach(a=>a.classList.remove('active'));link.classList.add('active');const id=link.getAttribute('href')?.slice(1);if(id)openSection(id)}));
$('configForm').addEventListener('submit',async event=>{event.preventDefault();if(configSnapshot()===loadedSnapshot){updateSaveBar();return}if(!event.currentTarget.reportValidity())return;const button=$('saveButton');button.disabled=true;button.textContent='Wird gespeichert …';try{await api('/api/config',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(buildConfig())});loadedSnapshot=configSnapshot();updateSaveBar();button.disabled=false;button.textContent='Konfiguration speichern';notify('Konfiguration gespeichert. Das Gerät startet neu.')}catch(error){notify(error.message,true);button.disabled=false;button.textContent='Konfiguration speichern'}});
$('pushTestButton').addEventListener('click',async()=>{const button=$('pushTestButton');button.disabled=true;button.textContent='Push Service Test wird gesendet …';try{await api('/api/push/test',{method:'POST'});notify('Push Service Test wurde an den MQTT-Server gesendet.');}catch(error){notify(error.message,true)}finally{button.disabled=false;button.textContent='Push Service Test senden'}});
$('checkUpdate').addEventListener('click',async()=>{const button=$('checkUpdate');button.disabled=true;button.textContent='Prüfung in Arbeit …';$('updateMessage').textContent='Firmwareprüfung wurde gestartet · Manifest wird angefordert';otaInProgress=true;try{await api('/api/firmware/check',{method:'POST'});notify('Firmwareprüfung wurde gestartet.');scheduleUpdatePoll(300)}catch(error){otaInProgress=false;button.disabled=false;button.textContent='Jetzt prüfen';notify(error.message,true)}});
$('approveUpdate').addEventListener('click',async()=>{if(!confirm('Firmwareupdate jetzt laden und installieren?'))return;otaInProgress=true;$('approveUpdate').hidden=true;$('updateMessage').textContent='Update wurde freigegeben. Gerät lädt und installiert …';try{await api('/api/firmware/approve',{method:'POST'});notify('Update bestätigt. Download wird gestartet.');scheduleUpdatePoll(500)}catch(error){if(networkError(error)){reloadAfterReconnect=true;notify('Freigabe gesendet. Das Gerät ist vermutlich bereits im Update.');scheduleUpdatePoll(1000);return}otaInProgress=false;notify(error.message,true)}});
$('modemFactoryReset').addEventListener('click',async()=>{if(!confirm('Modem wirklich in den Auslieferungszustand versetzen? APN- und Modemeinstellungen im Modul werden zurückgesetzt.'))return;const button=$('modemFactoryReset');button.disabled=true;button.textContent='Modem wird zurückgesetzt …';try{await api('/api/modem/factory-reset',{method:'POST'});notify('Modem wurde in den Auslieferungszustand versetzt.');setTimeout(loadStatus,1500)}catch(error){notify(error.message,true)}finally{button.disabled=false;button.textContent='Modem-Auslieferungszustand'}});
$('systemReboot').addEventListener('click',async()=>{if(!confirm('Gerät jetzt neu starten?'))return;const button=$('systemReboot');button.disabled=true;button.textContent='Neustart wird ausgelöst …';try{await api('/api/system/reboot',{method:'POST'});notify('Neustart ausgelöst. Das Gerät startet neu.');$('connectionState').querySelector('span').textContent='Gerät startet neu';setTimeout(()=>location.reload(),8000)}catch(error){notify(error.message,true);button.disabled=false;button.textContent='Gerät neu starten'}});
$('nvsFactoryReset').addEventListener('click',async()=>{const confirmation=prompt('NVS wirklich löschen? Dadurch werden alle gespeicherten Parameter inklusive WLAN-Daten entfernt. Zum Bestätigen RESET eingeben.');if(confirmation!=='RESET')return;const button=$('nvsFactoryReset');button.disabled=true;button.textContent='NVS wird gelöscht …';try{await api('/api/system/nvs-reset',{method:'POST'});notify('NVS wurde gelöscht. Das Gerät startet neu.');$('connectionState').querySelector('span').textContent='Gerät startet neu';setTimeout(()=>location.reload(),8000)}catch(error){notify(error.message,true);button.disabled=false;button.textContent='NVS löschen'}});
$('fileDirectory').addEventListener('change',()=>{currentDirectory=value('fileDirectory');loadFiles()});$('refreshFiles').addEventListener('click',loadFiles);
$('parentDirectory').addEventListener('click',()=>{currentDirectory=currentDirectory.substring(0,currentDirectory.lastIndexOf('/'))||value('fileDirectory');loadFiles()});
$('fileUpload').addEventListener('change',async event=>{const file=event.target.files[0];if(!file)return;const path=`${currentDirectory}/${file.name}`;const data=new FormData();data.append('file',file);try{await api(`/api/file?path=${encodeURIComponent(path)}`,{method:'POST',body:data});notify(`${file.name} wurde hochgeladen.`);loadFiles()}catch(error){notify(error.message,true)}event.target.value=''});
$('fileList').addEventListener('click',async event=>{const open=event.target.dataset.open;if(open){currentDirectory=decodeURIComponent(open);loadFiles();return}const path=event.target.dataset.delete;if(!path||!confirm('Datei wirklich löschen?'))return;try{await api(`/api/file?path=${path}`,{method:'DELETE'});notify('Datei gelöscht.');loadFiles()}catch(error){notify(error.message,true)}});

Promise.all([api('/api/config').then(fillForm),loadStatus(),loadAlarmRouting(),loadFiles()]).catch(error=>notify(error.message,true));
setInterval(()=>{loadStatus();loadAlarmRouting()},5000);
