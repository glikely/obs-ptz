from model import *
ip=sys.argv[1]
m=Model(ip); m.protocol_reset(); m.pump(3.0)
print("after settle:",m.summary())
m.log.clear(); m.stats={k:0 for k in m.stats}
# zoom direct to 0 (no motion) with idle queue -- like zoom_abs / preset recall / home
print("--- idle queue, send Zoom_Direct(0) (no physical move)")
m.send(Cmd(H('8101044700000000ff'),'zoom_direct'))
m.pump(2.5)
print(m.summary()); print('\n'.join(m.log))
m.log.clear(); m.stats={k:0 for k in m.stats}
print("--- pan/tilt HOME (camera already at 0,0), idle queue")
m.send(Cmd(H('81010604ff'),'home'))
m.pump(2.5)
print(m.summary()); print('\n'.join(m.log))
