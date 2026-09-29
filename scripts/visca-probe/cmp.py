import model, fixed
from model import *
ip=sys.argv[1]
def scen(M,name):
    m=M(ip); m.protocol_reset(); m.pump(2.5)
    t_settle=None
    s1=m.summary()
    m.log.clear(); m.stats={k:0 for k in m.stats}
    m.send(Cmd(H('81010604ff'),'home')); m.pump(1.5)
    home=(m.stats['disconnects'],m.stats['retries'],m.connected)
    m.stats={k:0 for k in m.stats}
    end=m.now()+4; nxt=m.now()
    while m.now()<end:
        if m.now()>=nxt:
            m.pending=[c for c in m.pending if c.name!='drive']
            m.send(Cmd(H('8101060105050303ff'),'drive')); nxt+=0.05
        m.pump(0.005)
    m.pump(1)
    print(f"{name:8s} startup: {s1}\n{'':8s} idle-home: disconnects={home[0]} retries={home[1]} connected={home[2]}\n{'':8s} joystick 4s: sent={m.stats['sent']} retries={m.stats['retries']} disconnects={m.stats['disconnects']} outofseq_drop={m.stats['outofseq_drop']}")
    m.sock.close()
scen(Model,"current"); scen(fixed.Fixed,"fixed")
