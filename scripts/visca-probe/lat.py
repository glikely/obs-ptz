from vp import *
import statistics as st
def timed(v, kind, payload, wait=0.6):
    t=time.time(); seq=(v.inq if kind=='i' else v.cmd)(payload)
    res=[]; end=t+wait
    while True:
        r=end-time.time()
        if r<=0: break
        v.sock.settimeout(r)
        try: d,a=v.sock.recvfrom(2048)
        except socket.timeout: break
        if a[0]!=v.ip: continue
        ty,ln,sq=struct.unpack('>HHI',d[:8]); res.append(((time.time()-t)*1000,ty,sq,d[8:]))
    return seq,res
for ip in sys.argv[1:]:
    print("=====",ip)
    v=V(ip); v.reset(); v.rx(0.3)
    for name,p in [("pantilt_pos",'81090612ff'),("zoom_pos",'81090447ff'),("lens_ctl",'81097e7e00ff'),("cam_ctl",'81097e7e01ff'),("power",'81090400ff'),("version",'81090002ff')]:
        lats=[];bad=0
        for i in range(30):
            seq,res=timed(v,'i',bytes.fromhex(p),0.3)
            if not res: bad+=1; continue
            lats.append(res[0][0])
            if len(res)>1 or res[0][2]!=seq: print("  odd",name,seq,res)
        if lats: print(f"{name:12s} n={len(lats)} none={bad} min={min(lats):.1f} med={st.median(lats):.1f} p90={sorted(lats)[int(len(lats)*.9)-1]:.1f} max={max(lats):.1f} ms")
        else: print(name,"no replies", res)
