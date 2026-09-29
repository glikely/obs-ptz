from model import *
class Fixed(Model):
    def __init__(s,ip,gap_ms=15,timeout_ms=250,**k):
        super().__init__(ip,timeout_ms=timeout_ms,**k); s.gap=gap_ms/1000; s.last_rx=-1; s.inflight={}  # seq -> slot-state
        s.tx_at=None
    def tsend(s,msg):
        seq=super().tsend(msg); s.inflight[seq]=True; 
        for k in [k for k in s.inflight if k<seq-16]: del s.inflight[k]
        return seq
    def receive_datagram(s,data):
        s.last_rx=s.now()
        if len(data)>9 and struct.unpack('>H',data[:2])[0]==0x0111:
            seq=struct.unpack('>I',data[4:8])[0]
            if seq in s.inflight:            # accept any recently-sent seq, not only the latest
                s.stats['recv']+=0; s.receive(data[8:]); return
        super().receive_datagram(data)
    def receive(s,msg):
        kind=msg[1]&0xf0 if len(msg)>1 else 0
        if kind in (0x40,0x50,0x60): s.timer_at=None      # any reply: stop the retry timer
        super().receive(msg)
    def send_pending(s):
        if s.active[0]: return
        wait=s.gap-(s.now()-s.last_rx)
        if wait>0 and s.last_rx>=0 and (s.pending or (s.connected and s.stale)):
            s.defer=s.now()+wait; return
        super().send_pending()
    defer=None
    def pump(s,dur):
        end=s.now()+dur
        while s.now()<end:
            t=end-s.now()
            for at in (s.timer_at,s.update_at,s.defer):
                if at is not None: t=min(t,max(0,at-s.now()))
            r,_,_=select.select([s.sock],[],[],max(0,t))
            if r:
                d,a=s.sock.recvfrom(2048)
                if a[0]==s.ip: s.receive_datagram(d)
            if s.defer is not None and s.now()>=s.defer: s.defer=None; s.send_pending()
            if s.timer_at is not None and s.now()>=s.timer_at: s.timer_at=None; s.timeout()
            if s.update_at is not None and s.now()>=s.update_at: s.update_at=s.now()+1.0; s.send_pending()
