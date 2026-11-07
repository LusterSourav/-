#!/usr/bin/env python3 -u
# PowerMesh PRODUCTION v3 — persistent + cross-device sync (seq server-assigned, /api/messages, Threading)
import json, time, random, re, threading
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer as HTTPServer

stitch_path=Path(__file__).with_name("stitch.html")
if stitch_path.exists():
    INDEX=stitch_path.read_text()
else:
    sketch=Path(__file__).with_name("sketch.ino").read_text()
    m=re.search(r'const char INDEX_HTML\[\] PROGMEM = R"PROGMEMHTML\((.*?)\)PROGMEMHTML"', sketch, re.S)
    if not m: m=re.search(r'const char INDEX_HTML\[\] PROGMEM = R"HTML\((.*?)\)HTML"', sketch, re.S)
    INDEX=m.group(1) if m else "<h1>PowerMesh</h1>"

st={"seq":1,"tx":1,"rx":1,"dropped":0,"sdBuffered":1,"fwd":0,"acks":0,"rssi":-72,"battV":12.6,"battPct":100,"peers":0,"sdOK":True,"ap":"PowerMesh_Rescue","ip":"192.168.4.1","uptime":0}
start=time.time()
ql=Path(__file__).with_name("queue.log")
if not ql.exists(): ql.write_text("# queue.log boot\n")
SEEN=set()
seen_list=[]
img_asms={}
MSGS=[]  # [{seq,ts,sender,type,raw,src_ip}]
MSGS_LOCK=threading.Lock()
SEQ_LOCK=threading.Lock()
def batt_pct(v): return 100 if v>=12.6 else 0 if v<=9.0 else int((v-9.0)/3.6*100)
def is_dup(seq):
    if seq in SEEN: return True
    SEEN.add(seq); seen_list.append(seq)
    if len(seen_list)>32: SEEN.remove(seen_list.pop(0))
    return False

class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a): pass
    def do_GET(self):
        # parse path + query
        path = self.path.split("?")[0]
        qs = {}
        if "?" in self.path:
            for kv in self.path.split("?",1)[1].split("&"):
                if "=" in kv: k,v=kv.split("=",1); qs[k]=v
        if path in ("/","/index.html"):
            self.send_response(200); self.send_header("Content-Type","text/html"); self.send_header("Cache-Control","no-store, no-cache, must-revalidate"); self.end_headers(); self.wfile.write(INDEX.encode())
        elif path=="/status":
            st["uptime"]=int((time.time()-start)*1000)
            st["battV"]=max(9.0,min(12.6, st["battV"]+random.uniform(-0.03,0.03)))
            st["battPct"]=batt_pct(st["battV"])
            if int(time.time())%2==0: st["rssi"]=-72+random.randint(-6,6)
            self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.send_header("Access-Control-Allow-Methods","GET,POST,OPTIONS"); self.send_header("Access-Control-Allow-Headers","Content-Type"); self.end_headers()
            self.wfile.write(json.dumps(st).encode())
        elif path=="/api/messages":
            try: since=int(qs.get("since","0"))
            except: since=0
            with MSGS_LOCK:
                out=[m for m in MSGS if m["seq"] > since]
            self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
            self.wfile.write(json.dumps(out).encode())
        elif path in ("/generate_204","/hotspot-detect.html","/canonical.html","/success.txt","/ncsi.txt","/connecttest.txt","/redirect"):
            self.send_response(302); self.send_header("Location","/"); self.end_headers()
        else: self.send_response(302); self.send_header("Location","/"); self.end_headers()
    def do_POST(self):
        if self.path=="/api/msg":
            ln=int(self.headers.get("Content-Length",0))
            body=self.rfile.read(ln).decode() if ln else ""
            try: j=json.loads(body)
            except: j={}
            ttl=int(j.get("ttl",3))
            # server assigns seq
            with SEQ_LOCK:
                st["seq"]+=1
                seq=st["seq"]
            client_id=j.get("clientId","anon")
            # dedup on client+clientSeq if provided, else server seq
            cseq=j.get("cseq",None)
            dedup_key=f"{client_id}:{cseq}" if cseq is not None else str(seq)
            if cseq is not None and dedup_key in SEEN:
                print(f"[DEDUP] drop {dedup_key}")
                st["dropped"]+=1
                self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
                self.wfile.write(json.dumps({"ok":True,"seq":seq,"dup":True}).encode())
                return
            if cseq is not None:
                SEEN.add(dedup_key); seen_list.append(dedup_key)
                if len(seen_list)>200: SEEN.remove(seen_list.pop(0))
            if len(body)>2048:
                print(f"[DROP] len {len(body)} >2048")
                st["dropped"]+=1
                self.send_response(413); self.send_header("Content-Type","application/json"); self.end_headers()
                self.wfile.write(json.dumps({"ok":False,"error":"too big"}).encode())
                return
            print(f"[HTTP RX] seq={seq} ttl={ttl} {body[:120]}")
            # image reassemble before storing
            msg_to_store=None
            if j.get("type")=="img_chunk":
                iid=j.get("imgId", j.get("total",0))
                s=j.get("seq", j.get("cseq",0))
                # use imgId separate
                k=f"img-{j.get('total',0)}-{client_id}"
                if k not in img_asms: img_asms[k]={"total":j.get("total",0),"chunks":{},"sender":client_id}
                img_asms[k]["chunks"][s]=j.get("data","")
                if len(img_asms[k]["chunks"])==img_asms[k]["total"] and img_asms[k]["total"]>0:
                    full="".join(img_asms[k]["chunks"][i] for i in range(img_asms[k]["total"]))
                    print(f"[IMG] reassembled {len(full)} bytes")
                    # store as single img message
                    msg_to_store={"seq":seq,"ts":int(time.time()*1000),"sender":client_id,"type":"img","data":full[:70000],"total":j.get("total",0)}
                    del img_asms[k]
                else:
                    # store chunk progress but ack
                    self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
                    self.wfile.write(json.dumps({"ok":True,"seq":seq,"rssi":st["rssi"],"acks":st["acks"],"fwd":st["fwd"]}).encode())
                    return
            else:
                msg_to_store={"seq":seq,"ts":int(time.time()*1000),"sender":client_id,"type":j.get("type","chat"),"msg":j.get("msg",""),"count":j.get("count",""),"lat":j.get("lat"),"lon":j.get("lon"),"raw":body[:500]}
            if msg_to_store:
                with MSGS_LOCK:
                    MSGS.append(msg_to_store)
                    if len(MSGS)>200: MSGS.pop(0)
                with open(ql,"a") as f: f.write(f"{int((time.time()-start)*1000)} STORE {seq} {json.dumps(msg_to_store)[:120]}\n")
                st["tx"]+=1; st["rx"]+=1; st["sdBuffered"]+=1
                print(f"[RF TX] seq={seq} ttl={ttl} len={len(body)}")
                print(f"[ACK TX] ack={seq}"); st["acks"]+=1
                if ttl>1: print(f"[FWD] seq={seq} ttl {ttl}->{ttl-1}"); st["fwd"]+=1
                print(f"[RF RX] crc OK seq={seq}")
            self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
            self.wfile.write(json.dumps({"ok":True,"seq":seq,"rssi":st["rssi"],"acks":st["acks"],"fwd":st["fwd"]}).encode())
        else: self.send_response(404); self.end_headers()
    def do_OPTIONS(self):
        self.send_response(200); self.send_header("Access-Control-Allow-Origin","*"); self.send_header("Access-Control-Allow-Methods","GET,POST,OPTIONS"); self.send_header("Access-Control-Allow-Headers","Content-Type"); self.end_headers()

print("=== PowerMesh PRODUCTION v3 — persistent /api/messages — http://localhost:9080 ===")
print("[SD] mounted queue.log")
print("[WiFi] softAP PowerMesh_Rescue OK IP=192.168.4.1")
print("[HTTP] server on http://localhost:9080/ (Threading, 200-msg store)")
print(f"[OLED] PM-01 100% 12.60V RSSI -72 TX:1 RX:1 F:0 A:0")
print("\nOpen http://localhost:9080  (phone: http://192.168.1.10:9080)\n")
HTTPServer(("0.0.0.0",9080), H).serve_forever()
