from model import *
ip=sys.argv[1]
m=Model(ip); m.protocol_reset(); m.pump(3.0)
def reset_stats(): m.log.clear(); m.stats={k:0 for k in m.stats}
reset_stats()
print("--- 6 queued zoom-direct(0) commands at once (e.g. burst of preset/zoom actions)")
for i in range(6): m.send(Cmd(H('8101044700000000ff'),'zoom_direct'))
m.pump(3)
print(m.summary()); 
print('\n'.join(m.log[:40]))
reset_stats()
print("--- joystick emulation: PT-drive STOP (no motion) 20/s for 4s, polling active")
end=m.now()+4; nxt=m.now()
while m.now()<end:
    if m.now()>=nxt:
        m.pending=[c for c in m.pending if c.name!='drive']  # driver coalesces: only latest drive
        m.send(Cmd(H('8101060105050303ff'),'drive')); nxt+=0.05
    m.pump(0.005)
m.pump(1)
print(m.summary())
print('\n'.join(m.log[:30]))
