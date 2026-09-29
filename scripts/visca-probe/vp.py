import socket, struct, time, sys
def hx(b): return b.hex(':')
class V:
    def __init__(s, ip, port=52381, lport=52381):
        s.ip=ip; s.port=port
        s.sock=socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR,1)
        try: s.sock.bind(('',lport))
        except Exception as e: print("bind fail",e); s.sock.bind(('',0))
        s.seq=0; s.t0=time.time()
    def raw(s, t, payload, seq=None):
        if seq is None:
            s.seq+=1; seq=s.seq
        p=struct.pack('>HHI',t,len(payload),seq)+payload
        s.sock.sendto(p,(s.ip,s.port)); return seq
    def cmd(s,payload,seq=None): return s.raw(0x0100,payload,seq)
    def inq(s,payload,seq=None): return s.raw(0x0110,payload,seq)
    def reset(s):
        s.raw(0x0200,b'\x01',0); s.seq=0
    def rx(s, timeout=1.0, until=None):
        out=[]; end=time.time()+timeout
        while True:
            r=end-time.time()
            if r<=0: break
            s.sock.settimeout(r)
            try: d,a=s.sock.recvfrom(2048)
            except socket.timeout: break
            if a[0]!=s.ip: continue
            out.append((time.time()-s.t0,d))
        return out
    def show(s, items, base=None):
        for t,d in items:
            ty,ln,sq=struct.unpack('>HHI',d[:8]) if len(d)>=8 else (0,0,0)
            print(f"  +{t:8.3f} type={ty:04x} seq={sq} pl={hx(d[8:])}")
