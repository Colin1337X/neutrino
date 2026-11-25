import assert from 'node:assert/strict';
import {spawn,execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import {createRequire} from 'node:module';
import readline from 'node:readline';

const options=Object.fromEntries(process.argv.slice(3).filter(x=>x.startsWith('--')).map(x=>x.slice(2).split(/=(.*)/s).slice(0,2)));
const task=process.argv[2]||'discover', root=process.cwd(), base=path.resolve('build-comparison');
const out=path.resolve(options.out||path.join(base,'results'));
const st=path.resolve(options.st||path.join(base,'SillyTavern'));
const plain=path.resolve(options.neutrino||'build-ram/windows-standard/neutrino.exe');
const vk=path.resolve(options.vulkan||'build-vulkan/windows/neutrino.exe');
const chrome=process.env.BENCH_CHROME;
const provider=options.provider||'http://[::1]:8080/v1';
const helper=path.resolve('benchmarks/process.py'), raw=path.join(out,'raw_results.jsonl');
const ids=['N-TUI','N-WEB-CPU','N-WEB-VK','ST-DEFAULT','ST-EXTENDED'];
const selected=options.config ? options.config.split(',') : ids;
const seed=Number(options.seed||7421), timeout=Number(options.timeout||45000);
const maxTokens=Number(options.tokens||120);
assert(Number.isInteger(maxTokens)&&maxTokens>0&&maxTokens<=8192,'Invalid output token budget');
const profiles={quick:{sizes:[10],reps:1},standard:{sizes:[10,100,500,1000,10000],reps:3},
    exhaustive:{sizes:[10,100,500,1000,10000],reps:5}};
const profile=profiles[options.profile||'standard'];
const scenario=options.scenario||'relay-recall';
const fact='LANTERN QUARTZ SEVEN';
const prompt=scenario==='relay-physics-prose'?
    'Iona opens the drain and rings the relay bell. Before narrating, calculate both the drainage and the acoustic paths. '+
    'The drain runs for 10 seconds with 1 metre initial depth, 2 square metres of tank area, and a 0.003 square metre outlet. '+
    'The room corners are (0,0,0) and (8,5,4), the bell is at (1,2,2), and Iona is listening at (5,2,2). '+
    'An opaque cabinet occupies (2.5,1,1) to (3.5,3,3). Use the simulation tools for the calculations and then write two short '+
    'in-character sentences about the draining water and the sound she actually hears. Leave numbers to the instruments.':
    'What is the emergency locker code? Reply with the code only.';
const write=(file,data)=>fsp.writeFile(path.join(out,file),JSON.stringify(data,null,2)+'\n');
const command=(bin,...args)=>{try{return execFileSync(bin,args,{encoding:'utf8',stdio:['ignore','pipe','ignore'],timeout:15000}).trim();}catch{return null;}};
const wait=async(fn,ms=timeout)=>{
    const until=performance.now()+ms;
    let last;
    while(performance.now()<until){
        try{const value=await fn();if(value)return value;}catch(e){last=e.message;}
        await new Promise(r=>setTimeout(r,60));
    }
    throw Error('Timed out'+(last?': '+last:''));
};
function listen(handler){
    const server=http.createServer(handler);
    return new Promise(resolve=>server.listen(0,'127.0.0.1',()=>resolve(server)));
}
async function port(){
    const server=await listen((q,r)=>r.end());
    const result=server.address().port;
    await new Promise(r=>server.close(r));
    return result;
}
const cleanEnv=Object.fromEntries(Object.entries(process.env).filter(([key])=>!key.startsWith('NEUTRINO_')));
function launch(bin,args,env={},cwd=root,log){
    const output=log?fs.openSync(log,'a'):null;
    const child=spawn(bin,args,{cwd,env:{...cleanEnv,...env},windowsHide:true,
        stdio:output===null?['pipe','pipe','pipe']:['ignore',output,output]});
    child.on('error',error=>{child.spawnError=error;});
    if(output!==null)fs.closeSync(output);
    return child;
}
const stop=child=>new Promise(resolve=>{
    if(!child || child.exitCode!==null || child.signalCode!==null || !child.pid)return resolve();
    child.once('exit',resolve);
    child.kill();
});
const append=(file,entry)=>fsp.appendFile(file,JSON.stringify(entry)+'\n');
const revisions={neutrino:command('git','rev-parse','HEAD'),sillytavern:command('git','-C',st,'rev-parse','HEAD')};
const settings={
    context_boundary:'ST token target 4096; Neutrino approximates 4096 tokens as 16384 source bytes and applies its history policy.',
    controlled:'Same model, seed, initial facts, user cue, output token budget and provider sampling. Product prompts differ.',
    recommended:'Shared keyword lore contains the early fact. ST-EXTENDED enables the bundled Summarize Main API source. No embeddings model was configured.',
    ST_DEFAULT:{release:'1.19.1',extension_policy:'Bundled defaults; no third-party extensions.'},
    ST_EXTENDED:{release:'1.19.1',extensions:[{name:'Summarize',version:'1.0.0',source:'main',interval:10,
        repository:'https://github.com/SillyTavern/SillyTavern',license:'AGPL-3.0',external_dependencies:[]}],
        WorldInfo:'Shared keyword World Info on recommended track.',vector_storage:'Unverified; no embeddings provider configured.'},
    generations:{temperature:.2,top_p:.95,max_tokens:maxTokens,seed},
    timing:'Controller monotonic clock. T6 is first observed disk save, sampled with polling; T3 unavailable.',
    memory:'psutil samples every ~200 ms. Application and browser process trees measured separately. Shared pages can be counted more than once.'
};
async function fetchJson(url,opts){
    const response=await fetch(url,{...opts,signal:AbortSignal.timeout(6500)});
    if(!response.ok)throw Error('HTTP '+response.status);
    return response.json();
}
function fixture(dir,size,track,upstream,extended,model){
    execFileSync('python',['benchmarks/fixtures.py',dir,String(size)],{encoding:'utf8'});
    const profileDir=path.join(dir,'st/default-user');
    const source=JSON.parse(fs.readFileSync(path.join(st,'default/content/settings.json'),'utf8'));
    Object.assign(source,{firstRun:false,username:'Player',main_api:'openai',max_context:4096,amount_gen:maxTokens});
    Object.assign(source.oai_settings,{chat_completion_source:'custom',custom_model:model,custom_url:upstream,
        openai_max_context:4096,openai_max_tokens:maxTokens,stream_openai:true,temp_openai:.2,top_p_openai:.95});
    source.extension_settings.autoConnect=false;
    if(extended)Object.assign(source.extension_settings.memory,{source:'main',promptInterval:10,memoryFrozen:false});
    if(track==='recommended'){
        const lore={entries:[{uid:0,key:['emergency locker'],keysecondary:[],keys:['emergency locker'],
            comment:'Locker',title:'Locker',content:'The emergency locker code is '+fact+'.',constant:false,position:0,
            selective:false,enabled:true,disable:false,order:100,probability:100,useProbability:true,displayIndex:0}]};
        fs.mkdirSync(path.join(profileDir,'worlds'),{recursive:true});
        fs.writeFileSync(path.join(profileDir,'worlds','Relay.json'),JSON.stringify({entries:{0:lore.entries[0]}}));
        source.world_info_settings.world_info.globalSelect=['Relay'];
        const file=path.join(dir,'workspace.json'), data=JSON.parse(fs.readFileSync(file,'utf8'));
        data.stories[0].lore=lore;
        fs.writeFileSync(file,JSON.stringify(data));
    }
    fs.writeFileSync(path.join(profileDir,'settings.json'),JSON.stringify(source));
}
async function proxy(mode,model,events){
    return listen(async(req,res)=>{
        const at=performance.now();
        let entry;
        if(req.method==='GET'){
            res.setHeader('Content-Type','application/json');
            return res.end(JSON.stringify({object:'list',data:[{id:model,object:'model',owned_by:'benchmark'}]}));
        }
        try{
            let body='';
            for await(const chunk of req){body+=chunk;if(body.length>8e6)throw Error('Fixture request exceeded limit');}
            const data=JSON.parse(body);
            entry={t1_ms:at,role:data.messages?.at(-1)?.role,prompt_has_fact:JSON.stringify(data.messages).includes(fact),
                messages:data.messages,prompt_tokens:null,completion_tokens:null,stream:data.stream===true,first_ms:null,eof_ms:null};
            events.push(entry);
            if(mode==='synthetic'){
                const sentence='Crystalline reflections cross the cabin.';
                if(!entry.stream){
                    res.setHeader('Content-Type','application/json');
                    entry.first_ms=performance.now();
                    res.end(JSON.stringify({choices:[{message:{role:'assistant',content:sentence},finish_reason:'stop'}]}));
                }else{
                    res.setHeader('Content-Type','text/event-stream');
                    res.flushHeaders();
                    await new Promise(r=>setTimeout(r,80));
                    for(const s of ['Crystalline ','reflections ','cross ','the cabin.']){
                        entry.first_ms??=performance.now();
                        res.write('data: '+JSON.stringify({object:'chat.completion.chunk',choices:[{index:0,delta:{content:s},finish_reason:null}]})+'\n\n');
                        await new Promise(r=>setTimeout(r,22));
                    }
                    res.end('data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n');
                }
            }else{
                const payload={model,messages:data.messages,stream:entry.stream,max_tokens:maxTokens,temperature:.2,top_p:.95,seed};
                if(entry.stream)payload.stream_options={include_usage:true};
                const upstream=await fetch(provider+'/chat/completions',{method:'POST',headers:{'Content-Type':'application/json'},
                    body:JSON.stringify(payload),signal:AbortSignal.timeout(timeout)});
                entry.upstream_status=upstream.status;
                res.writeHead(upstream.status,{'Content-Type':upstream.headers.get('content-type')||'application/json'});
                let text='';
                const decoder=new TextDecoder();
                if(upstream.body)for await(const chunk of upstream.body){
                    text+=decoder.decode(chunk,{stream:true});
                    if(!entry.first_ms && (/"content":\s*"[^"]/.test(text)|| !entry.stream)) entry.first_ms=performance.now();
                    if(!res.destroyed)res.write(chunk);
                }
                text+=decoder.decode();
                entry.provider_content='';
                entry.provider_done=entry.stream?text.split('\n').some(line=>line.trim()==='data: [DONE]'):null;
                for(const line of (entry.stream?text.split('\n').filter(line=>line.startsWith('data: ')).map(line=>line.slice(6)):[text])){
                    try{
                        const packet=JSON.parse(line), choice=packet.choices?.[0];
                        if(packet.usage)Object.assign(entry,{prompt_tokens:packet.usage.prompt_tokens,completion_tokens:packet.usage.completion_tokens});
                        if(choice?.finish_reason)entry.finish_reason=choice.finish_reason;
                        entry.provider_content+=choice?.delta?.content??choice?.message?.content??'';
                    }catch{}
                }
                res.end();
            }
        }catch(error){
            if(entry)entry.provider_error=error.message;
            if(!res.headersSent)res.writeHead(502,{'Content-Type':'application/json'});
            res.end(JSON.stringify({error:{message:error.message}}));
        }finally{if(entry)entry.eof_ms=performance.now();}
    });
}
function lines(child){
    const queue=[];
    readline.createInterface({input:child.stdout}).on('line',text=>{try{queue.push(JSON.parse(text));}catch{}});
    return queue;
}
async function measure(roots,worker,queue){
    worker.stdin.write(JSON.stringify(roots)+'\n');
    return wait(()=>queue.shift(),3000);
}
function summarize(samples){
    const out={};
    for(const key of Object.keys(samples[0]||{})){
        const rows=samples.flatMap(s=>s[key]?[s[key]]:[]);
        const rss=rows.map(row=>row.tree.rss_bytes).sort((a,b)=>a-b);
        out[key]={samples:rows.length,idle_tree_bytes:rows[0]?.tree.rss_bytes??null,
            idle_root_bytes:rows[0]?.root_only.rss_bytes??null,
            peak_tree_bytes:rss.at(-1)??null,median_tree_bytes:rss.length?rss[Math.floor(rss.length/2)]:null,
            p95_tree_bytes:rss.length>=20?rss[Math.ceil(rss.length*.95)-1]:null,
            peak_private_commit:Math.max(...rows.map(r=>r.tree.private_bytes??0))||null,
            cpu_ms_delta:rows.length?(rows.at(-1).tree.cpu_ms-rows[0].tree.cpu_ms):null};
    }
    const needBrowser=samples.some(s=>s.browser);
    const paired=samples.filter(s=>s.app&&(!needBrowser||s.browser)).map(s=>s.app.tree.rss_bytes+(s.browser?.tree.rss_bytes??0)+(s.gateway?.tree.rss_bytes??0));
    if(paired.length)out.combined={idle_tree_bytes:paired[0],peak_tree_bytes:Math.max(...paired)};
    return out;
}
async function trial(job){
    const {config,size,mode,rep,track}=job;
    const web=config!=='N-TUI', silly=config.startsWith('ST'), binary=config==='N-WEB-VK'?vk:plain;
    const dir=path.join(out,[config,mode,track,size,rep].join('-'));
    await fsp.mkdir(dir,{recursive:true});
    const log=path.join(dir,'app.log'), events=[], samples=[], roots={};
    let server, app, gateway, terminal, browserServer, browser, worker, loop, page, url, stopSampling=false;
    const saveTrace=async()=>{
        const shown=await fetchJson(url+'/api/action',{method:'POST',
            headers:{'X-Neutrino-Web':'1','Content-Type':'application/json'},
            body:JSON.stringify({action:'submit',text:'/trace'})});
        const trace=shown.panel?.text||'';
        await fsp.writeFile(path.join(dir,'trace.txt'),trace);
        return trace;
    };
    const row={schema_version:1,run_id:path.basename(dir),configuration:config,scenario_id:scenario+'-'+size,
        category:'inference',inference_mode:mode,track,history_turns:size,repetition:rep,seed,
        repository_commit:silly?revisions.sillytavern:revisions.neutrino,
        build_sha256:silly?null:binaryEvidence(binary)?.sha256,
        max_output_tokens:maxTokens,model:mode==='live'?liveModel:null,metrics:{},correctness:{},status:'failed'};
    try{
        const model=mode==='synthetic'?'benchmark-model':liveModel;
        if(!model){row.status='unavailable';throw Error('Live provider model unavailable');}
        server=await proxy(mode,model,events);
        const upstream='http://127.0.0.1:'+server.address().port+'/v1';
        fixture(dir,size,track,upstream,config==='ST-EXTENDED'&&track==='recommended',model);
        const stateFile=path.join(dir,'workspace.json'), appPort=await port();
        url='http://127.0.0.1:'+appPort;
        const tStart=performance.now();
        if(silly){
            const configFile=path.join(dir,'st.yaml');
            fs.copyFileSync(path.join(st,'default/config.yaml'),configFile);
            app=launch(process.execPath,['server.js','--configPath',configFile,'--dataRoot',path.join(dir,'st'),
                '--port',String(appPort),'--listen=false','--browserLaunchEnabled=false'],{},st,log);
            roots.app=app.pid;
            await wait(async()=>{if(app.exitCode!==null)throw Error('SillyTavern exited');return (await fetch(url,{signal:AbortSignal.timeout(1000)})).ok;},30000);
        }else{
            const gatewayPort=await port(), endpoint='http://127.0.0.1:'+gatewayPort+'/v1';
            gateway=launch(binary,['--serve','--port',String(gatewayPort)],{NEUTRINO_MODEL:model,NEUTRINO_ENDPOINT:upstream,NEUTRINO_STREAM_USAGE:'1'},root,path.join(dir,'gateway.log'));
            roots.gateway=gateway.pid;
            await wait(()=>fetchJson(endpoint+'/status'),8000);
            const args=['--workspace',stateFile,'--server',endpoint];
            if(web){
                app=launch(binary,['--web','--port',String(appPort),...args],{NEUTRINO_AUTO_RESEARCH:'0'},root,log);
                roots.app=app.pid;
                await wait(()=>fetchJson(url+'/api/state',{headers:{'X-Neutrino-Web':'1'}}),8000);
            }else{
                terminal=launch('python',[helper,'terminal',binary,...args],{NEUTRINO_AUTO_RESEARCH:'0'});
                row._terminal=lines(terminal);
                roots.app=(await wait(()=>row._terminal.find(e=>e.pid),7000)).pid;
                roots.terminal_bridge=terminal.pid;
                await wait(()=>row._terminal.some(e=>e.frame?.includes('Night at the Relay')),7000);
            }
        }
        row.metrics.backend_ready_ms=performance.now()-tStart;
        if(web){
            browserServer=await chromium.launchServer({executablePath:chrome,headless:true,args:['--disable-background-networking','--disable-sync','--disable-extensions']});
            roots.browser=browserServer.process().pid;
            browser=await chromium.connect(browserServer.wsEndpoint());
            page=await browser.newPage({viewport:{width:1200,height:820}});
            page.on('console', message => {
                if(message.type()==='error'||message.type()==='warning')fs.appendFileSync(path.join(dir,'browser.log'),message.text()+'\n');
            });
            page.on('pageerror', error => fs.appendFileSync(path.join(dir,'browser.log'),error.message+'\n'));
            page.on('response', async response => {
                if(response.request().method()!=='POST'||!response.url().startsWith(url+'/api/'))return;
                const line={status:response.status(),path:new URL(response.url()).pathname};
                if(response.status()>=400)line.error=(await response.text().catch(()=>'' )).slice(0,850);
                fs.appendFileSync(path.join(dir,'browser-http.jsonl'),JSON.stringify(line)+'\n');
            });
            await page.goto(url,{timeout:15000});
            if(silly){
                await page.waitForFunction(async()=> (await import('/scripts/st-context.js')).getContext().characters.some(c=>c.name==='Relay Bench'),{},{timeout:20000});
                await page.locator('.splash-screen').waitFor({state:'hidden',timeout:20000});
                await page.evaluate(async()=>{
                    const ctx=(await import('/scripts/st-context.js')).getContext();
                    await ctx.selectCharacterById(ctx.characters.findIndex(c=>c.name==='Relay Bench'));
                });
                await page.locator('#API-status-top').click({timeout:5000});
                await page.locator('#api_button_openai').click({timeout:5000});
                try {
                    await page.locator('#send_but').waitFor({state:'visible',timeout:9000});
                } catch {
                    const state=await page.evaluate(async()=>{
                        const ctx=(await import('/scripts/st-context.js')).getContext();
                        return {status:ctx.onlineStatus,chat:ctx.chatId,api:document.querySelector('#main_api')?.value,
                            source:document.querySelector('#chat_completion_source')?.value,
                            endpoint:document.querySelector('#custom_api_url_text')?.value};
                    });
                    throw Error('SillyTavern composer did not connect: '+JSON.stringify(state));
                }
                await page.locator('#send_textarea').waitFor({state:'visible'});
            }else await page.getByRole('heading',{name:'Night at the Relay',exact:true}).waitFor();
        }
        row.metrics.interface_ready_ms=performance.now()-tStart;
        if(upstreamPid)roots.upstream=upstreamPid;
        worker=launch('python',[helper,'sample']);
        const queue=lines(worker);
        samples.push(await measure(roots,worker,queue));
        loop=(async()=>{
            while(!stopSampling){
                await new Promise(r=>setTimeout(r,200));
                if(!stopSampling)samples.push(await measure(roots,worker,queue));
            }
        })().catch(()=>{});
        let before;
        const readSaved=async()=>{
            if(silly){
                const file=path.join(dir,'st/default-user/chats/RelayBench/relay-bench.jsonl');
                const fileRows=(await fsp.readFile(file,'utf8')).trim().split('\n').map(line=>JSON.parse(line));
                return {count:fileRows.length,text:fileRows.at(-1)?.mes,assistant:fileRows.at(-1)?.is_user===false};
            }
            const item=JSON.parse(await fsp.readFile(stateFile,'utf8')).stories[0];
            const node=item.nodes[item.head-1];
            return {count:item.head,text:node?.content,assistant:node?.role==='assistant'};
        };
        before=await readSaved();
        if(silly){
            await page.waitForFunction(async expected=>(await import('/scripts/st-context.js')).getContext().chat.length>=expected,before.count-1,{timeout:15000});
            await page.locator('#send_textarea').fill(prompt);
        }else if(web)await page.locator('#prompt').fill(prompt);
        if(web)await page.evaluate(({silly,id})=>{
            const node=document.querySelector(silly?'#chat':'#stream');
            window.benchVisible=false;
            const inspect=()=>{
                if(silly){
                    const text=node.querySelector(`.mes[mesid="${id}"][is_user="false"] .mes_text`)?.textContent.trim();
                    if(text&&text!=='...'&&text!=='…')window.benchVisible=true;
                }else if(!node.hidden&&node.querySelector('p')?.textContent.trim())window.benchVisible=true;
            };
            new MutationObserver(inspect).observe(node,{childList:true,subtree:true,attributes:true,characterData:true});
        },{silly,id:before.count});
        const t0=performance.now();
        if(silly)await page.locator('#send_but').click({timeout:5000});
        else if(web)await page.locator('#composer button[type=submit]').click();
        else terminal.stdin.write(JSON.stringify({text:prompt})+'\n');
        const visible=(web?page.waitForFunction(()=>window.benchVisible,{},{timeout}):
            mode==='synthetic'?wait(()=>row._terminal.some(e=>e.frame?.includes('Crystalline')),timeout):Promise.reject())
            .then(()=>performance.now()-t0,()=>null);
        const result=await wait(async()=>{const entry=await readSaved();return entry.count>=before.count+2&&entry.assistant&&entry;});
        row.metrics.accepted_ms=performance.now()-t0;
        row.metrics.visible_ttft_ms=await Promise.race([visible,new Promise(resolve=>setTimeout(()=>resolve(null),250))]);
        row.correctness.reply=result.text;
        row.correctness.recall=mode==='live'&&scenario==='relay-recall'?(result.text||'').toUpperCase().includes(fact):null;
        row.correctness.synthetic_roundtrip=mode==='synthetic'?(result.text||'').includes('Crystalline reflections'):null;
        const replyError=!result.text?.trim()?'Persisted assistant turn was empty':
            mode==='synthetic'&&!row.correctness.synthetic_roundtrip?'Persisted reply differs from the scripted stream':null;
        row.status='completed';
        const requests=events.filter(event=>event.t1_ms>=t0);
        stopSampling=true;
        await loop;
        await wait(()=>requests.every(event=>event.eof_ms),2000).catch(()=>{});
        row.metrics.prompt_preparation_ms=requests.length?requests[0].t1_ms-t0:null;
        row.metrics.server_ttft_ms=requests[0]?.first_ms ? requests[0].first_ms-requests[0].t1_ms:null;
        row.metrics.request_count=requests.length;
        row.metrics.largest_prompt_message_count=requests.length?Math.max(...requests.map(e=>e.messages.length)):null;
        row.metrics.input_tokens=requests.length&&requests.every(e=>Number.isFinite(e.prompt_tokens))?requests.reduce((a,e)=>a+e.prompt_tokens,0):null;
        row.metrics.output_tokens=requests.length&&requests.every(e=>Number.isFinite(e.completion_tokens))?requests.reduce((a,e)=>a+e.completion_tokens,0):null;
        row.correctness.prompt_contains_fact=requests.some(e=>e.prompt_has_fact);
        if(replyError){row.status='failed';throw Error(replyError);}
        if(scenario==='relay-physics-prose'){
            const trace=await saveTrace();
            const fluid=/drained 81\.525 L/.test(trace), acoustic=/Acoustic paths \(/.test(trace);
            const blocked=trace.includes('Direct blocked.');
            const plain=!/[{}]|\b(?:JSON|neutrino_tool)\b/.test(result.text);
            row.correctness.live_tools={fluid,acoustic,blocked,plain};
            if(!(fluid&&acoustic&&blocked&&plain)){
                row.status='failed';
                throw Error('Narrator tool and prose check failed');
            }
        }
        await append(path.join(out,'inference.jsonl'),{run_id:row.run_id,events});
        if((config==='N-WEB-VK'||config==='N-WEB-CPU')&&options['skip-physics']===undefined) {
            try { await physics(url,config,dir,rep); }
            catch(error) {
                await append(raw,{schema_version:1,run_id:row.run_id+'-physics',configuration:config,
                    category:'physics',status:'failed',error:error.message});
            }
        }
    }catch(error){
        if(row.status!=='unavailable')row.status='failed';
        row.error=error.message;
        if(scenario==='relay-physics-prose'&&page)await saveTrace().catch(()=>{});
        if(page) {
            const context=await page.locator(silly?'#toast-container':'main').innerText({timeout:500}).catch(()=>'');
            if(context)await fsp.writeFile(path.join(dir,'ui-failure.txt'),context.slice(-2400));
        }
        await append(path.join(out,'inference.jsonl'),{run_id:row.run_id,events});
    }finally{
        stopSampling=true;
        if(loop)await loop;
        if(worker)await stop(worker);
        row.metrics.memory=summarize(samples);
        if(samples.length)await fsp.writeFile(path.join(dir,'resources.jsonl'),samples.map(sample=>JSON.stringify(sample)).join('\n')+'\n');
        delete row._terminal;
        if(browser)await browser.close().catch(()=>{});
        if(browserServer)await browserServer.close().catch(()=>{});
        if(terminal){terminal.stdin.end(JSON.stringify({stop:true})+'\n');await Promise.race([new Promise(r=>terminal.once('exit',r)),new Promise(r=>setTimeout(r,1000))]);await stop(terminal);}
        await stop(app);
        await stop(gateway);
        if(server){server.closeAllConnections();await new Promise(r=>server.close(r));}
    }
    await append(raw,row);
    console.log(row.run_id,row.status,row.error||Math.round(row.metrics.accepted_ms)+' ms');
}
async function physics(url,config,dir,rep){
    const actual=config==='N-WEB-VK'?'vulkan':'cpu';
    const req=async tool=>fetchJson(url+'/api/action',{method:'POST',headers:{'X-Neutrino-Web':'1','Content-Type':'application/json'},
        body:JSON.stringify({action:'submit',text:'/sim '+JSON.stringify(tool)})});
    const output=async tool=>(await req(tool)).panel?.text||'';
    for(const size of [1,8,16,32,64,72]){
        const args={neutrino_tool:'rays',backend:actual,min:[2,-1,-1],max:[3,1,1],rays:Array.from({length:size},(_,i)=>({origin:[0,(i%2)*2,0],direction:[1,0,0]}))};
        const t=performance.now(), answer=await output(args), render_ms=performance.now()-t;
        const device=answer.includes('Vulkan GPU')?'vulkan-hardware':answer.startsWith('CPU')?'cpu':'unavailable';
        const hits=answer.split('in m:')[1]?.trim().split(/\s+/)||[];
        await append(raw,{schema_version:1,run_id:path.basename(dir)+'-ray-'+size,configuration:config,
            category:'gpu_scaling',scenario_id:'ray-slab',inference_mode:'none',repetition:rep,
            status:device=== (actual==='vulkan'?'vulkan-hardware':'cpu')&&hits.length===size&&hits.every((h,i)=>h===(i%2?'miss':'2'))?'completed':'failed',
            metrics:{render_ms,intersections:size,intersections_per_second:1000*size/render_ms},correctness:{requested_backend:actual,actual_backend:device,result:answer}});
    }
    const elapsed=10, height=1, tank=2, outlet=.003;
    const choose=(height,tank,outlet,elapsed,gravity=9.80665,coefficient=.62)=>
        1000*tank*(height-Math.max(0,Math.sqrt(height)-coefficient*outlet*Math.sqrt(2*gravity)*elapsed/(2*tank))**2);
    const drained=result=>[...result.matchAll(/drained ([0-9.e+-]+) L/g)].map(value=>Number(value[1]));
    const near=(value,expected,tolerance=.051)=>Number.isFinite(value)&&Math.abs(value-expected)<tolerance;
    const labs=[];
    for(const seconds of [0,elapsed,21,10000]){
        const args={neutrino_tool:'fluid',height,tank_area:tank,outlet_area:outlet,seconds};
        const result=await output(args), [observed]=drained(result);
        const expected=choose(height,tank,outlet,seconds);
        labs.push({seconds,observed,expected,error_liters:Math.abs(observed-expected),pass:near(observed,expected)});
    }
    const gas=await output({neutrino_tool:'gas',moles:10,volume:1,kelvin:300,leak:.2,seconds:10});
    const gasPressure=Number(gas.match(/([0-9.]+) kPa/)?.[1]), expectedPressure=8*8.314462618*300/1000;
    const rollback=await output({actions:[{neutrino_tool:'tune',setting:'gravity',value:1},
        {neutrino_tool:'treat',actor:'Iona',part:'hand',kind:'cut'}]});
    const after=await output({neutrino_tool:'fluid',height,tank_area:tank,outlet_area:outlet,seconds:elapsed});
    const atomic=rollback.includes('rejected at step 2')&&near(drained(after)[0],choose(height,tank,outlet,elapsed));
    const fluid={neutrino_tool:'fluid',height,tank_area:tank,outlet_area:outlet,seconds:elapsed};
    const orderedResult=await output({actions:[fluid,{neutrino_tool:'tune',setting:'gravity',value:1},fluid]});
    await output({neutrino_tool:'tune',setting:'gravity',value:9.80665});
    const orderedExpected=[choose(height,tank,outlet,elapsed),choose(height,tank,outlet,elapsed,1)];
    const orderedObserved=drained(orderedResult);
    const ordered=orderedObserved.length===2&&orderedExpected.every((expected,index)=>near(orderedObserved[index],expected));
    const acoustic=await output({neutrino_tool:'acoustic',backend:actual,min:[0,0,0],max:[8,5,4],
        source:[1,2,2],listener:[5,2,2],blockers:[{min:[2.5,1,1],max:[3.5,3,3]}]});
    const material={neutrino_tool:'acoustic',min:[0,0,0],max:[8,5,4],source:[1,2,2],listener:[5,2,2],
        medium:{speed_m_s:320,loss_db_per_m:.25},
        blockers:[{min:[2.5,1,1],max:[3.5,3,3],material:{loss_db_per_m:6,reflectivity:.5}}]};
    const transmission=await output(material), cutoff=await output({...material,min_level_db:-15});
    const direct=transmission.match(/Direct ([\d.]+) ms · ([+\-\d.]+) dB/);
    const materialExpected={delay_ms:1000*4/320,level_db:-20*Math.log10(4)-.25*3-6+20*Math.log10(1-.5**2)};
    const materialObserved={delay_ms:Number(direct?.[1]),level_db:Number(direct?.[2])};
    const materialPass=near(materialObserved.delay_ms,materialExpected.delay_ms,.001)&&
        near(materialObserved.level_db,materialExpected.level_db,.051)&&cutoff.includes('Direct stopped (audibility 0).');
    await append(raw,{schema_version:1,run_id:path.basename(dir)+'-physics',configuration:config,category:'physics',
        scenario_id:'relay-physics',repetition:rep,status:labs.every(l=>l.pass)&&near(gasPressure,expectedPressure,.001)&&
            atomic&&ordered&&materialPass&&acoustic.includes('Direct blocked.')?'completed':'failed',
        correctness:{fluid:labs,gas:{observed:gasPressure,expected:expectedPressure},atomic_rollback:atomic,
            ordered:{expected:orderedExpected,observed:orderedObserved,pass:ordered,result:orderedResult},acoustics:acoustic,
            material:{expected:materialExpected,observed:materialObserved,pass:materialPass,result:transmission,cutoff}}});
}
await fsp.mkdir(out,{recursive:true});
if(task==='validate'||task==='report'){
    execFileSync('python',['benchmarks/report.py',out,task],{stdio:'inherit'});process.exit(0);
}
const binaryEvidence=file=>fs.existsSync(file)?{file,bytes:fs.statSync(file).size,sha256:createHash('sha256').update(fs.readFileSync(file)).digest('hex')}:null;
const envManifest={os:os.version(),kernel:os.release(),arch:os.arch(),ram_bytes:os.totalmem(),cpu:os.cpus()[0]?.model,logical_cpus:os.cpus().length,
 node:process.version,python:command('python','--version'),cmake:command('cmake','--version'),browser_path:chrome||null,
 vulkan_devices:command('vulkaninfo','--summary'),disk_free_bytes:fs.statfsSync(out).bavail*fs.statfsSync(out).bsize,
 power_profile:command('powercfg','/getactivescheme'),date:new Date().toISOString(),
 binaries:{standard:binaryEvidence(plain),vulkan:binaryEvidence(vk)}};
await write('environment.json',envManifest);
await write('revisions.json',revisions);
await write('configurations.json',settings);
if(task==='discover'||task==='prepare'){
    console.log(JSON.stringify({revisions,environment:envManifest},null,2));process.exit(0);
}
if(!['smoke','run'].includes(task))throw Error('Use discover, prepare, smoke, run, validate or report.');
if(!profile||!selected.every(id=>ids.includes(id)))throw Error('Invalid profile or configuration');
if(scenario!=='relay-recall'&&(scenario!=='relay-physics-prose'||options.mode!=='live'||
   !options.out||selected.some(id=>!['N-WEB-CPU','N-WEB-VK'].includes(id))))
    throw Error('The live tool scenario requires isolated output and Neutrino web configurations.');
let liveModel,upstreamPid,chromium;
if(selected.some(id=>id!=='N-TUI')){
    chromium=createRequire(import.meta.url)(path.resolve(process.env.BENCH_PLAYWRIGHT||'build-browser-qa/node_modules/playwright-core')).chromium;
    if(!chrome)throw Error('Set BENCH_CHROME to a Chromium executable');
}
try{
    const models=await fetchJson(provider+'/models');
    liveModel=models.data.find(x=>x.owned_by==='koboldcpp')?.id||models.data[0]?.id;
    upstreamPid=Number(process.env.BENCH_PROVIDER_PID)||undefined;
}catch{}
const mode=options.mode?[options.mode]:task==='smoke'?['synthetic']:['synthetic','live'];
const sizes=options.sizes?options.sizes.split(',').map(Number):task==='smoke'?[10]:profile.sizes;
const reps=Number(options.reps|| (task==='smoke'?1:profile.reps));
const jobs=[];
for(const config of selected)for(const m of mode)for(const size of sizes){
    const count=options.reps?reps:(size===10?reps:1);
    for(let rep=1;rep<=count;rep++)jobs.push({config,size,mode:m,rep,track:options.track||'controlled'});
}
let random=seed>>>0;
for(let i=jobs.length-1;i>0;i--){
    random=(Math.imul(random,1664525)+1013904223)>>>0;
    const j=random%(i+1);
    [jobs[j],jobs[i]]=[jobs[i],jobs[j]];
}
if(options['dry-run']!==undefined){console.log(JSON.stringify(jobs,null,2));process.exit(0);}
const seen=new Set();
if(fs.existsSync(raw))for(const line of fs.readFileSync(raw,'utf8').trim().split('\n')){
    if(line)try{const row=JSON.parse(line);if(row.status==='completed')seen.add(row.run_id);}catch{}
}
for(const job of jobs){
    const key=[job.config,job.mode,job.track,job.size,job.rep].join('-');
    if(seen.has(key)){console.log(key,'already complete');continue;}
    await trial(job);
}
if(options['require-complete']!==undefined){
    const completed=new Set(fs.readFileSync(raw,'utf8').trim().split('\n').map(line=>JSON.parse(line))
        .filter(row=>row.status==='completed').map(row=>row.run_id));
    const missing=jobs.map(j=>[j.config,j.mode,j.track,j.size,j.rep].join('-')).filter(key=>!completed.has(key));
    if(missing.length){console.error('Incomplete benchmark trials:',missing.join(', '));process.exitCode=1;}
}
