const fileSelect=document.getElementById('logFile');
const limitInput=document.getElementById('limit');
const refreshButton=document.getElementById('refresh');
const entries=document.getElementById('entries');
const count=document.getElementById('count');
const state=document.getElementById('state');
const fileHint=document.getElementById('fileHint');

const params=new URLSearchParams(location.search);
const initialLimit=params.get('limit');
const initialFile=params.get('file');

let currentLogPath='';

function requestedLimit(){return Math.min(150,Math.max(1,Number(limitInput.value)||100))}

function cell(row,text){const value=document.createElement('td');value.textContent=text;row.appendChild(value)}

function baseName(path){return String(path||'').split('/').pop()||''}

function parseLogDate(path){
  const name=baseName(path);
  const match=name.match(/^system-(\d{4})-(\d{2})-(\d{2})\.csv$/);
  if(!match)return null;
  const year=Number(match[1]),month=Number(match[2]),day=Number(match[3]);
  if(!year||month<1||month>12||day<1||day>31)return null;
  return Date.UTC(year,month-1,day);
}

function isLogFile(entry){
  if(!entry||entry.directory)return false;
  const name=baseName(entry.path||entry.name);
  return name==='system.csv'||name==='system-boot.csv'||/^system-\d{4}-\d{2}-\d{2}\.csv$/.test(name);
}

function sortLogFiles(a,b){
  const da=parseLogDate(a.path||a.name);
  const db=parseLogDate(b.path||b.name);
  if(da!==null&&db!==null)return db-da;
  if(da!==null)return -1;
  if(db!==null)return 1;
  if((a.path||a.name)===(currentLogPath||''))return -1;
  if((b.path||b.name)===(currentLogPath||''))return 1;
  return String(a.name||a.path).localeCompare(String(b.name||b.path),'de');
}

function logLabel(path){
  const name=baseName(path);
  if(name==='system-boot.csv')return 'Boot-Protokoll';
  if(name==='system.csv')return 'Altes Systemprotokoll';
  const dateMatch=name.match(/^system-(\d{4})-(\d{2})-(\d{2})\.csv$/);
  if(!dateMatch)return name||path;
  const label=`${dateMatch[1]}-${dateMatch[2]}-${dateMatch[3]}`;
  return path===currentLogPath?`${label} (aktuell)`:label;
}

function selectedLogPath(){
  return fileSelect.value||currentLogPath||'/logs/system-boot.csv'
}

function renderFileOptions(files){
  fileSelect.replaceChildren();
  const list=[...files];
  const current=list.find(entry=>(entry.path||entry.name)===currentLogPath);
  if(!current&&currentLogPath){
    list.unshift({path:currentLogPath,name:baseName(currentLogPath),directory:false});
  }
  list.sort(sortLogFiles);
  for(const entry of list){
    const path=entry.path||entry.name;
    const option=document.createElement('option');
    option.value=path;
    option.textContent=logLabel(path);
    fileSelect.appendChild(option);
  }
  const preferred=[initialFile,currentLogPath,list[0]?.path||list[0]?.name].find(path=>path&&list.some(entry=>(entry.path||entry.name)===path));
  if(preferred) fileSelect.value=preferred;
  fileHint.textContent=currentLogPath?`Aktuell: ${logLabel(currentLogPath)}`:'Es ist noch keine Tageslogdatei verfügbar.';
}

async function loadFileList(){
  const response=await fetch('/api/status',{cache:'no-store'});
  if(!response.ok){let message=`HTTP ${response.status}`;try{const error=await response.json();message=error.error||message}catch{}throw new Error(message)}
  const status=await response.json();
  currentLogPath=(status.logging&&status.logging.path)||'';

  const filesResponse=await fetch('/api/files?path=/logs',{cache:'no-store'});
  if(!filesResponse.ok){let message=`HTTP ${filesResponse.status}`;try{const error=await filesResponse.json();message=error.error||message}catch{}throw new Error(message)}
  const payload=await filesResponse.json();
  const files=Array.isArray(payload.files)?payload.files.filter(isLogFile):[];
  renderFileOptions(files);
}

async function loadLogs(){
  const limit=requestedLimit();
  limitInput.value=limit;
  refreshButton.disabled=true;
  fileSelect.disabled=true;
  state.className='';
  state.textContent='Wird geladen …';
  try{
    const file=selectedLogPath();
    const response=await fetch(`/api/logs?limit=${limit}&file=${encodeURIComponent(file)}`,{cache:'no-store'});
    if(!response.ok){
      if(response.status===404){
        entries.innerHTML='<tr><td colspan="3" class="empty">Für die ausgewählte Datei sind noch keine Logs vorhanden.</td></tr>';
        count.textContent='0 Einträge';
        state.textContent=logLabel(file);
        history.replaceState(null,'',`?limit=${limit}&file=${encodeURIComponent(file)}`);
        return;
      }
      let message=`HTTP ${response.status}`;try{const error=await response.json();message=error.error||message}catch{}throw new Error(message)
    }
    const text=await response.text();
    const lines=text.split(/\r?\n/).filter(line=>line&&line!=='timestamp;event;details').slice(-limit).reverse();
    entries.replaceChildren();
    for(const line of lines){
      const parts=line.split(';');
      const row=document.createElement('tr');
      cell(row,parts[0]||'–');cell(row,parts[1]||'–');cell(row,parts.slice(2).join(';')||'–');
      entries.appendChild(row);
    }
    if(!lines.length){const row=document.createElement('tr');const empty=document.createElement('td');empty.colSpan=3;empty.className='empty';empty.textContent='Noch keine Logeinträge vorhanden.';row.appendChild(empty);entries.appendChild(row)}
    count.textContent=`${lines.length} ${lines.length===1?'Eintrag':'Einträge'}`;
    state.textContent=logLabel(file);
    history.replaceState(null,'',`?limit=${limit}&file=${encodeURIComponent(file)}`);
  }catch(error){
    entries.innerHTML='<tr><td colspan="3" class="empty">Protokoll konnte nicht geladen werden.</td></tr>';
    count.textContent='0 Einträge';state.className='error';state.textContent=error.message;
  }finally{
    refreshButton.disabled=false;
    fileSelect.disabled=false;
  }
}

async function init(){
  if(initialLimit)limitInput.value=initialLimit;
  try{
    await loadFileList();
  }catch(error){
    fileHint.textContent=error.message;
    fileSelect.replaceChildren();
    const option=document.createElement('option');
    option.value=currentLogPath||'/logs/system-boot.csv';
    option.textContent=currentLogPath?logLabel(currentLogPath):'Logdateien konnten nicht geladen werden';
    fileSelect.appendChild(option);
  }
  await loadLogs();
}

refreshButton.addEventListener('click',loadLogs);
limitInput.addEventListener('keydown',event=>{if(event.key==='Enter')loadLogs()});
fileSelect.addEventListener('change',loadLogs);
init();
