const limitInput=document.getElementById('limit');
const refreshButton=document.getElementById('refresh');
const entries=document.getElementById('entries');
const count=document.getElementById('count');
const state=document.getElementById('state');

function requestedLimit(){return Math.min(500,Math.max(1,Number(limitInput.value)||250))}
function cell(row,text){const value=document.createElement('td');value.textContent=text;row.appendChild(value)}

async function loadLogs(){
  const limit=requestedLimit();
  limitInput.value=limit;
  refreshButton.disabled=true;
  state.className='';
  state.textContent='Wird geladen …';
  try{
    const response=await fetch(`/api/logs?limit=${limit}`,{cache:'no-store'});
    if(!response.ok){let message=`HTTP ${response.status}`;try{const error=await response.json();message=error.error||message}catch{}throw new Error(message)}
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
    state.textContent='Neueste Einträge zuerst';
    history.replaceState(null,'',`?limit=${limit}`);
  }catch(error){
    entries.innerHTML='<tr><td colspan="3" class="empty">Protokoll konnte nicht geladen werden.</td></tr>';
    count.textContent='0 Einträge';state.className='error';state.textContent=error.message;
  }finally{refreshButton.disabled=false}
}

const initial=new URLSearchParams(location.search).get('limit');
if(initial)limitInput.value=initial;
refreshButton.addEventListener('click',loadLogs);
limitInput.addEventListener('keydown',event=>{if(event.key==='Enter')loadLogs()});
loadLogs();
