"""Sends each inquiry the driver uses, plus the standard single-value ones,
and shows which the camera answers or rejects (60 02 = not supported)."""
from vp import *
INQS = {
 'version 0002': '81090002ff', 'power 0400': '81090400ff', 'pantilt pos 0612': '81090612ff',
 'zoom pos 0447': '81090447ff', 'focus pos 0448': '81090448ff', 'focus mode 0438': '81090438ff',
 'focus near limit 0428': '81090428ff', 'dzoom 0406': '81090406ff', 'af sens 0458': '81090458ff',
 'af mode 0457': '81090457ff', 'wb mode 0435': '81090435ff', 'rgain 0443': '81090443ff',
 'bgain 0444': '81090444ff', 'ae mode 0439': '81090439ff', 'shutter 044a': '8109044aff',
 'iris 044b': '8109044bff', 'gain 044c': '8109044cff', 'bright 044d': '8109044dff',
 'expcomp mode 043e': '8109043eff', 'expcomp pos 044e': '8109044eff', 'backlight 0433': '81090433ff',
 'aperture 0442': '81090442ff', 'high res 0452': '81090452ff', 'nr 0453': '81090453ff',
 'gamma 045b': '8109045bff', 'picture effect 0463': '81090463ff', 'camera id 0422': '81090422ff',
 'block lens 7e7e00': '81097e7e00ff', 'block camctl 7e7e01': '81097e7e01ff', 'block other 7e7e02': '81097e7e02ff',
 'block enl1 7e7e03': '81097e7e03ff', 'block enl2 7e7e04': '81097e7e04ff', 'block enl3 7e7e05': '81097e7e05ff',
 'pantilt maxspeed 0611': '81090611ff', 'pantilt mode 0610': '81090610ff',
}
v=V(sys.argv[1]); v.reset(); v.rx(0.3)
for name,p in INQS.items():
    v.inq(bytes.fromhex(p)); r=v.rx(0.35)
    rep=r[0][1][8:].hex(':') if r else 'NO REPLY'
    tag='ERR' if r and r[0][1][9]&0xf0==0x60 else 'ok '
    print(f"{tag} {name:24s} {rep}")
    time.sleep(0.03)
