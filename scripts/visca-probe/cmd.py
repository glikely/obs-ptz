from vp import *
def run(v,label,kind,hexs,wait=1.5):
    t=time.time(); seq=(v.inq if kind=='i' else v.cmd)(bytes.fromhex(hexs)); out=[]
    end=t+wait
    while True:
        r=end-time.time()
        if r<=0: break
        v.sock.settimeout(r)
        try: d,a=v.sock.recvfrom(2048)
        except socket.timeout: break
        if a[0]!=v.ip: continue
        ty,ln,sq=struct.unpack('>HHI',d[:8]); out.append(f"+{(time.time()-t)*1000:6.1f}ms t={ty:04x} seq={sq}(sent {seq}) {hx(d[8:])}")
    print(f"{label}:"); [print("   ",o) for o in out] or print("    (none)")
    return out
def pos(v):
    v.inq(bytes.fromhex('81090612ff')); r=v.rx(0.5); return r[0][1][10:18] if r else None
for ip in sys.argv[1:]:
    print("=====",ip)
    v=V(ip); v.reset(); v.rx(0.3)
    p0=pos(v); print("start pan/tilt raw", p0.hex() if p0 else None)
    run(v,"zoom stop","c",'8101040700ff')
    run(v,"zoom direct 0x0000","c",'810104470000 0000ff'.replace(' ',''),3)
    run(v,"pt drive right speed 5 (0x05,0x05, right, stop tilt)","c",'8101060105050203ff',0.4)
    run(v,"pt drive STOP","c",'8101060105050303ff',1.0)
    run(v,"pt rel tiny","c",'810106030a0a0000000a00000000ff'.replace('0a0a0000000a00000000','0a0a00000005 00000000'.replace(' ','')),2.5) if False else None
    run(v,"pt abs 0,0 speed 0x0f","c",'81010602 0f0f 00000000 00000000ff'.replace(' ',''),6)
    run(v,"pt home","c",'81010604ff',6)
    # overlap: two long commands in a row
    run(v,"pt abs +0x100 (long-ish)","c",'810106020f0f0000010000000000ff'[:0]+'8101060 20f0f 00000100 00000000ff'.replace(' ',''),0.05)
    run(v,"second pt abs while first running","c",'8101060 20f0f 0000 0200 0000 0000ff'.replace(' ',''),3)
    run(v,"bad syntax cmd","c",'81010699ff',0.5)
    run(v,"inq while unknown","i",'81090099ff',0.5)
    run(v,"home cmd","c",'81010604ff',5)
    v.sock.close()
