// node tests/ui.test.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
function element() {
  const classes = new Set();
  return {textContent:'',value:'',disabled:false,children:[],events:{},
    classList:{add:x=>classes.add(x),remove:x=>classes.delete(x),contains:x=>classes.has(x),toggle(x,on){if(on===undefined)on=!classes.has(x);on?classes.add(x):classes.delete(x);}},
    style:{setProperty(){},removeProperty(){}},setAttribute(){},focus(){},
    addEventListener(type,fn){this.events[type]=fn;},querySelectorAll(){return [];},
    append(...items){this.children.push(...items);},appendChild(item){this.children.push(item);},replaceChildren(){this.children=[];},
    getContext(){return {};}};
}
const nodes = new Map();
const get = id => {if(!nodes.has(id))nodes.set(id,element());return nodes.get(id);};
const messages = [], resizeListeners = new Set();
let now=1000;
const context = vm.createContext({URL,console,location:{search:''},innerWidth:1280,innerHeight:760,
  performance:{now:()=>now},setTimeout:()=>1,clearInterval(){},cancelAnimationFrame(){},requestAnimationFrame:()=>1,
  window:{chrome:{webview:{postMessage:m=>messages.push(m),addEventListener(){}}},
    addEventListener:(type,fn)=>resizeListeners.add(fn),removeEventListener:(type,fn)=>resizeListeners.delete(fn)},
  document:{getElementById:get,createElement:element,addEventListener(){},querySelectorAll:()=>['btnRefresh','btnRepairMenu','btnSettingsSave','btnBrowse'].map(get)}});
vm.runInContext(fs.readFileSync(path.join(__dirname,'../ui/app.js'),'utf8'),context);
context.applyInit({frames:[],busy:false,installed:true,serverUrl:'http://localhost',lastResult:'失败',lastResultOk:false});
assert.equal(get('operationStatus').textContent,'失败');
assert.equal(get('operationStatus').classList.contains('error'),true);
context.setupFrames([],12); context.setupFrames([],12);
assert.equal(resizeListeners.size,1,'frame refresh must not accumulate resize handlers');
context.renderPending([{type:'resource',versionId:'<script>',totalSize:1024}], '');
assert.equal(get('updateList').children[0].textContent,'资源基础包 <script> · 1.0 KB');
context.renderBusy(true,2);
assert.equal(get('btnRefresh').disabled,true);
assert.equal(get('btnSettingsSave').disabled,true);
context.renderProgress(true,0.2,'game.pak',10240,true,2);
now+=1000;context.renderProgress(true,0.4,'game.pak',10240,true,2);
assert.match(get('transferInfo').textContent,/4.0 KB \/ 10.0 KB.*预计剩余/);
get('btnCancel').events.click();
assert.equal(get('btnCancel').disabled,true);
assert.equal(messages.at(-1).type,'cancel');
context.renderBusy(false,0);
assert.equal(get('btnSettingsSave').disabled,false);
context.openSettings();
get('setGamePath').value='draft';
context.applyInit({frames:[],gamePath:'saved',serverUrl:'http://localhost'});
assert.equal(get('setGamePath').value,'draft','background snapshots preserve settings draft');
const count=messages.length;
context.handleHost({type:'folder_selected',path:'new-folder'});
assert.equal(messages.length,count,'browse in settings must not save immediately');
assert.equal(get('setGamePath').value,'new-folder');
get('setServerUrl').value='file:///invalid';
get('settingsForm').events.submit({preventDefault(){}});
assert.equal(messages.length,count,'invalid server URL must not be sent');
context.handleHost({type:'settings_saved',ok:false,message:'busy'});
assert.equal(get('settingsPanel').classList.contains('open'),true);
context.handleHost({type:'settings_saved'});
assert.equal(get('settingsPanel').classList.contains('open'),false);
console.log('Launcher UI tests passed: progress, update details, errors, busy guards, settings drafts and background lifecycle.');
