// Run: node tests/test_console.cjs (no dependencies).
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../web/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script); // Check the complete inline script, not just tested functions.
const section = (start, end) => script.slice(script.indexOf(start), script.indexOf(end));
const fields = {manageKind:{value:'all'},versionSearch:{value:''},versionAvailability:{value:'all'}};
const state = {managePlatform:'Windows',enabledMap:{Windows:['installer:1.0']},versions:[
  {packageKind:'installer',versionId:'1.0'},
  {packageKind:'base',versionId:'1.0',requiredGameVersion:'2.0'},
  {packageKind:'patch',versionId:'broken',valid:false},
]};
let requests = 0, release;
const ctx = vm.createContext({state, $:id=>fields[id], URL, AbortController, setTimeout, clearTimeout,
  window:{}, location:{origin:'http://localhost'}, headers:()=>({}), renderVersions:()=>{}, toast:()=>{}});
vm.runInContext(section('function apiUrl(', 'function switchProject(') +
  section('function versionKind(', 'function renderVersions(') +
  section('async function toggleVersion(', 'async function showVersion('), ctx);
const keys = () => Array.from(ctx.managedVersions(), v=>v.packageKind);
(async () => {
  assert.deepEqual(keys(), ['installer','base','patch']);
  fields.versionSearch.value='2.0'; assert.deepEqual(keys(), ['base']);
  fields.versionSearch.value=''; fields.versionAvailability.value='open'; assert.deepEqual(keys(), ['installer']);
  fields.versionAvailability.value='closed'; assert.deepEqual(keys(), ['base']);
  fields.versionAvailability.value='invalid'; assert.deepEqual(keys(), ['patch']);
  fields.versionAvailability.value='all'; fields.manageKind.value='resources'; assert.deepEqual(keys(), ['base','patch']);
  ctx.fetch = async () => ({ok:true,status:200,json:async()=>null});
  await assert.rejects(ctx.api('/api/status'), /无效数据/);
  ctx.fetch = async () => ({ok:false,status:403,json:async()=>({})});
  await assert.rejects(ctx.api('/api/status'), /令牌/);
  ctx.fetch = async () => {throw new TypeError('Failed to fetch');};
  await assert.rejects(ctx.api('/api/status'), /无法连接/);
  await assert.rejects(ctx.api('/api/enabled_versions',{method:'POST'}), /结果未知/);
  ctx.fetch = async url => {
    requests++; assert.match(url, /platform=Windows/);
    await new Promise(resolve => release=resolve);
    return {ok:true,status:200,json:async()=>({message:'saved',clientVersions:[]})};
  };
  const pending=ctx.setAllVersions(true);
  assert.equal(state.enabledSaveBusy,true);
  await ctx.toggleVersion('installer:1.0',false);
  assert.equal(requests,1);
  release(); await pending;
  assert.deepEqual(Array.from(state.enabledMap.Windows), ['installer:1.0','base:1.0']);
  assert.equal(state.enabledSaveBusy,false);
  ctx.fetch=async()=>{throw new Error('offline');};
  await ctx.toggleVersion('installer:1.0',false);
  assert.deepEqual(Array.from(state.enabledMap.Windows), ['installer:1.0','base:1.0']);
  assert.equal(state.enabledSaveBusy,false);
  console.log('Console tests passed: filtering, API errors, scoped batch changes, duplicate prevention and failure recovery.');
})().catch(error => {console.error(error);process.exitCode=1;});
