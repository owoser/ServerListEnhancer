// Run only against the explicitly authorized development server.
const fs=require('node:fs');
const assert=require('node:assert/strict');
const {setTimeout:delay}=require('node:timers/promises');
const configPath='D:/MCserver/SER-DEV/mods/ServerListEnhancer/config/config.json';
const inbox='build/server-command.txt';
const original=fs.readFileSync(configPath,'utf8');
const config=JSON.parse(original);
let nonce=Date.now();
function reload(value){fs.writeFileSync(configPath,typeof value==='string'?value:JSON.stringify(value,null,2));fs.writeFileSync(inbox,String(++nonce)+'\n'+'sle reload\n');}
async function get(){const response=await fetch('http://127.0.0.1:19132/v1/join',{signal:AbortSignal.timeout(3000)});assert.equal(response.status,200);const bytes=Buffer.from(await response.arrayBuffer());assert.equal(Number(response.headers.get('content-length')),bytes.length);const json=JSON.parse(bytes.toString());for(const key of ['name','protocol','version','level','players','maxPlayers','gameType'])assert.ok(key in json,key);return json;}
async function until(predicate){const end=Date.now()+5000;while(Date.now()<end){const json=await get();if(predicate(json))return json;await delay(50);}throw Error('reload response did not converge');}
(async()=>{try{
 const before=await get();assert.equal(before.server_list_enhancer.schemaVersion,1);
 reload({...config,enabled:false});await until(j=>j.server_list_enhancer.enabled===false);
 const logoData='data:image/png;base64,'+fs.readFileSync('resources/logo/default.png').toString('base64');
 reload({...config,enabled:true,logoData,introText:'SLE live reload acceptance'});
 const changed=await until(j=>j.server_list_enhancer.introText==='SLE live reload acceptance');
 assert.equal(changed.server_list_enhancer.logo.type,'data');assert.equal(changed.server_list_enhancer.logo.value,logoData);
 assert.notEqual(changed.server_list_enhancer.revision,before.server_list_enhancer.revision);
 await Promise.all(Array.from({length:20},()=>get()));
 console.log('GET /v1/join: native fields, Content-Length, disabled clear, inline PNG, reload, 20 concurrent requests: PASS');
 }finally{reload(original);await until(j=>j.server_list_enhancer.introText===config.introText&&j.server_list_enhancer.logo.type==='url');assert.equal(fs.readFileSync(configPath,'utf8'),original);console.log('Development-server config restored byte-for-byte: PASS');}
})().catch(e=>{console.error(e);process.exitCode=1;});
