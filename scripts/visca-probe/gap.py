from vp import *
import statistics as st
ip=sys.argv[1]; N=int(sys.argv[2]) if len(sys.argv)>2 else 40
INQS=[bytes.fromhex(x) for x in ('81090612ff','81090447ff','81090400ff','81097e7e00ff')]
v=V(ip); v.reset(); v.rx(0.3)
for gap_ms in (0,1,3,10,30,100):
    lost=0; lat=[]
    for i in range(N):
        p=INQS[i%len(INQS)]
        t=time.time(); seq=v.inq(p)
        got=None; end=t+0.25
        while time.time()<end:
            v.sock.settimeout(max(0.001,end-time.time()))
            try: d,a=v.sock.recvfrom(2048)
            except socket.timeout: break
            if a[0]!=ip: continue
            sq=struct.unpack('>I',d[4:8])[0]
            if sq==seq and d[9]&0x70==0x50: got=(time.time()-t)*1000; break
        if got is None: lost+=1
        else:
            lat.append(got); 
            if gap_ms: time.sleep(gap_ms/1000)
        # drain late stuff
        v.rx(0.0005)
    print(f"gap-after-reply={gap_ms:3d}ms  lost={lost}/{N}  lat med={st.median(lat) if lat else 0:.1f} max={max(lat) if lat else 0:.1f}")
