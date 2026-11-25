"""Dev-only process accounting and native terminal bridge."""
import json, os, re, sys, threading
import psutil

def metrics(roots):
    result={}
    for name,pid in roots.items():
        try:
            root=psutil.Process(pid)
            processes=[root]+root.children(recursive=True)
            rows=[]
            for p in processes:
                try:
                    with p.oneshot():
                        m=p.memory_info()
                        cpu=p.cpu_times()
                        io=p.io_counters()
                        rows.append({"rss_bytes":m.rss,"private_bytes":getattr(m,"private",None),
                                     "cpu_ms":(cpu.user+cpu.system)*1000,"handles":p.num_handles() if os.name=="nt" else p.num_fds(),
                                     "threads":p.num_threads(),"read_bytes":io.read_bytes,"write_bytes":io.write_bytes})
                except psutil.Error: continue
            if rows:
                result[name]={"root_only":rows[0],"process_count":len(rows),"tree":{
                    key:sum(r[key] for r in rows) if all(r[key] is not None for r in rows) else None for key in rows[0]}}
        except psutil.Error: pass
    return result

if sys.argv[1]=="terminal":
    from winpty import PtyProcess
    terminal=PtyProcess.spawn(sys.argv[2:],env=dict(os.environ),dimensions=(30,112))
    lock=threading.Lock()
    def emit(obj):
        with lock: print(json.dumps(obj),flush=True)
    emit({"pid":terminal.pid})
    ansi=re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\a]*(?:\a|\x1b\\)")
    wanted=re.compile(r"Night at the Relay|Reply saved|Crystalline|LANTERN QUARTZ SEVEN")
    def drain():
        try:
            while chunk:=terminal.read(8192):
                clean=ansi.sub("",chunk)
                if match:=wanted.search(clean): emit({"frame":clean[-16000:]})
        except (EOFError,OSError): pass
    threading.Thread(target=drain,daemon=True).start()
    try:
        for line in sys.stdin:
            value=json.loads(line)
            if value.get("stop"):
                terminal.write("/quit\r")
                break
            terminal.write(value["text"]+"\r")
    finally: terminal.close()
else:
    for line in sys.stdin:
        try: print(json.dumps(metrics(json.loads(line))),flush=True)
        except (OSError,ValueError): print("{}",flush=True)
