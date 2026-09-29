"""Line-for-line model of PTZVisca + ViscaUDPTransport (current main)."""
import socket, struct, time, select, sys
H=bytes.fromhex
INQ={ # name -> (payload, result names)
 'version':(H('81090002ff'),{'vendor_id','model_id','vendor_name','model_name','rom_version','socket_number'}),
 'power':(H('81090400ff'),{'power_on'}),
 'pantilt':(H('81090612ff'),{'pan_pos','tilt_pos'}),
 'lens':(H('81097e7e00ff'),{'zoom_pos','focus_pos','focus_near_limit','focus_af_mode','focus_af_sensitivity','dzoom','focus_af_enabled','low_contrast_mode'}),
 'camctl':(H('81097e7e01ff'),{'r_gain','b_gain','wb_mode','aperature_gain','exposure_mode','high_resolution','wide_d','back_light','exposure_comp','slow_shutter','shutter_pos','iris_pos','gain_pos','bright_pos','exposure_comp_pos'}),
 'other':(H('81097e7e02ff'),{'picture_effect_mode','camera_id','framerate'}),
 'enl1':(H('81097e7e03ff'),{'dzoom_pos','focus_af_move_time','focus_af_interval_time','color_gain','gamma','high_sensitivity','nr_level','chroma_suppress','gain_limit'}),
 'enl2':(H('81097e7e04ff'),{'defog_mode'}),
 'enl3':(H('81097e7e05ff'),{'color_hue'}),
}
# property -> inquiry, as PTZVisca::inquires
PROP2INQ={'vendor_id':'version','power_on':'power','pan_pos':'pantilt','tilt_pos':'pantilt','focus_pos':'lens','zoom_pos':'lens','wb_mode':'camctl','iris_pos':'camctl','gain_pos':'camctl','camera_id':'other','dzoom_pos':'enl1','defog_mode':'enl2','color_hue':'enl3'}
class Cmd:
    def __init__(s,payload,name='',results=(),affects=''):
        s.cmd=payload; s.name=name; s.results=set(results); s.affects=affects
    @property
    def is_inq(s): return s.cmd[1]==0x09
class Model:
    def __init__(s,ip,timeout_ms=50,max_retry=3,lport=52381):
        s.ip=ip; s.sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        s.sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        s.sock.bind(('',lport)); s.timeout_ms=timeout_ms; s.max_retry=max_retry
        s.seq_state=[0]*8; s.pending=[]; s.active=[None]*8; s.stale=set(); s.connected=False
        s.timer_at=None; s.retry=0; s.update_at=None; s.t0=time.time()
        s.stats={k:0 for k in ('sent','recv','retries','disconnects','outofseq_drop','cam_seq_err','spurious','timeouts','errors')}
        s.log=[]
        s.pan=0
    def now(s): return time.time()-s.t0
    def L(s,m): s.log.append(f"{s.now()*1000:8.1f} {m}")
    # ---- transport
    def protocol_reset(s):
        s.seq_state=[0]*8
        s.sock.sendto(H('020000010000000001'),(s.ip,52381)); s.L("TX reset")
        s.reset()
    def tsend(s,msg):
        s.seq_state[0]+=1; seq=s.seq_state[0]
        p=bytearray(H('0100000000000000')+msg)
        p[1]=0x10 if msg[1]==9 else 0x00; p[3]=len(msg)
        p[4:8]=struct.pack('>I',seq); p[8]=0x81
        s.sock.sendto(bytes(p),(s.ip,52381)); return seq
    def receive_datagram(s,data):
        if len(data)<9: return
        ty,_,seq=struct.unpack('>HHI',data[:8]); rc=(data[9]&0x70) if len(data)>9 else 0; slot=(data[9]&0xf) if len(data)>9 else 0
        if ty==0x0111:
            if seq!=s.seq_state[0] and seq!=s.seq_state[slot]:
                s.stats['outofseq_drop']+=1; s.L(f"RX DROP out-of-seq seq={seq} state0={s.seq_state[0]} slot{slot}={s.seq_state[slot]} {data[8:].hex(':')}"); return
            if slot: s.seq_state[slot]=s.seq_state[0] if rc==0x40 else 0
            s.receive(data[8:])
        elif ty in (0x0200,0x0201):
            if data[8]==0x0f and data[9]==1:
                s.stats['cam_seq_err']+=1; s.L(f"RX camera SEQ ERROR (seq={seq}) -> protocol_reset"); s.protocol_reset()
            elif data[8]==1: s.L("RX reset-ack -> refresh"); s.cmd_get_camera_info()
    # ---- PTZVisca
    def reset(s): s.cmd_get_camera_info()
    def set_connected(s,c):
        if c!=s.connected:
            s.connected=c; s.L(f"CONNECTED={c}")
            if not c: s.stats['disconnects']+=1
    def cmd_get_camera_info(s):
        s.set_connected(True)
        for k in PROP2INQ: s.stale.add(k)
        s.update_at=s.now()+1.0
        s.send_pending()
    def send(s,cmd): s.pending.append(cmd); s.send_pending()
    def send_packet(s,payload):
        s.stats['sent']+=1; seq=s.tsend(payload); s.L(f"TX seq={seq} {payload.hex(':')}")
        s.timer_at=s.now()+s.timeout_ms/1000
    def timeout(s):
        s.stats['timeouts']+=1
        if s.connected and s.active[0] and s.retry<s.max_retry:
            s.L(f"TIMEOUT retry {s.retry+1}"); s.stats['retries']+=1
            s.send_packet(s.active[0].cmd); s.retry+=1
        else:
            s.L("TIMEOUT -> DISCONNECT"); s.set_connected(False); s.active[0]=None; s.send_pending()
    def receive(s,msg):
        if len(msg)<3 or ((msg[0]&0x70)>>4)!=1: return
        s.stats['recv']+=1; slot=msg[1]&7; kind=msg[1]&0xf0
        s.L(f"RX slot{slot} {msg.hex(':')}")
        if kind==0x40:
            s.set_connected(True)
            if slot: s.active[slot]=s.active[0]; s.active[0]=None
        elif kind==0x50:
            s.set_connected(True)
            if slot==0: s.timer_at=None
            if s.active[slot] is None:
                if s.active[0]: s.active[slot]=s.active[0]; s.active[0]=None
                else: s.stats['spurious']+=1; s.L("  spurious reply"); s.send_pending(); return
            c=s.active[slot]
            if slot==0 and len(msg)>3:
                s.stale-= c.results
            s.active[slot]=None
        elif kind==0x60:
            s.timer_at=None; s.stats['errors']+=1
            if s.active[0]: s.stale-=s.active[0].results
            s.active[0]=None; s.active[slot]=None
        s.send_pending()
    def send_pending(s):
        if s.active[0]: return
        if not s.pending and s.connected:
            for p in list(s.stale):
                if p in PROP2INQ:
                    n=PROP2INQ[p]; pl,res=INQ[n]; s.pending.append(Cmd(pl,n,res)); break
        if not s.pending: return
        s.active[0]=s.pending.pop(0); s.send_packet(s.active[0].cmd); s.retry=0
    def pump(s,dur):
        end=s.now()+dur
        while s.now()<end:
            t=end-s.now()
            for at in (s.timer_at,s.update_at):
                if at is not None: t=min(t,max(0,at-s.now()))
            r,_,_=select.select([s.sock],[],[],max(0,t))
            if r:
                d,a=s.sock.recvfrom(2048)
                if a[0]==s.ip: s.receive_datagram(d)
            if s.timer_at is not None and s.now()>=s.timer_at: s.timer_at=None; s.timeout()
            if s.update_at is not None and s.now()>=s.update_at:
                s.update_at=s.now()+1.0; s.send_pending()
    def summary(s):
        return ' '.join(f"{k}={v}" for k,v in s.stats.items())+f" connected={s.connected} stale={sorted(s.stale)}"
