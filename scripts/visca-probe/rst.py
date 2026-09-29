from vp import *
ip=sys.argv[1]; v=V(ip); N=30
for gap in (0,0.002,0.02,0.1):
    stats={'req_reply':0,'req_lost':0,'seqerr':0,'rst_ack':0,'rst_lost':0}
    for i in range(N):
        v.raw(0x0200,b'\x01',0); 
        if gap: time.sleep(gap)
        v.inq(bytes.fromhex('81090400ff'),1)
        got=v.rx(0.25)
        types=[(struct.unpack('>H',d[:2])[0],d[8:]) for _,d in got]
        stats['rst_ack']+=any(t==0x0201 for t,_ in types)
        stats['seqerr']+=any(p[:2]==b'\x0f\x01' for t,p in types)
        ok=any(t==0x0111 and p[1]==0x50 for t,p in types)
        stats['req_reply']+=ok; stats['req_lost']+= (not ok)
    print(f"reset->seq1 gap={gap*1000:5.0f}ms:",stats)
