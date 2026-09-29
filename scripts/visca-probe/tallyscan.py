"""Tries the tally lamp commands and inquiries a camera might have, and shows
what it answers. Leaves every lamp it turns on turned off again."""
from vp import *
v=V(sys.argv[1]); v.reset(); v.rx(0.3)
def go(label, hexs, inq=False, w=0.4):
    (v.inq if inq else v.cmd)(bytes.fromhex(hexs)); r=v.rx(w)
    print(f"  {label:34s} {hexs:26s} ->", ' | '.join(d[8:].hex(':') for _,d in r) or 'NO REPLY')
    time.sleep(0.05)
print("inquiries:")
for n in ('81097e010aff','81097e010a00ff','81097e010a01ff','81097e010a02ff','81097e0a01ff','81090a0100ff'): go("inq",n,True)
print("commands (then inquiry):")
for label,c in [("0a 00 02 (red on?)",'81017e010a0002ff'),("0a 00 03 (red off?)",'81017e010a0003ff'),
                ("0a 01 02 (green on?)",'81017e010a0102ff'),("0a 01 03 (green off?)",'81017e010a0103ff'),
                ("0a 02 02",'81017e010a0202ff'),("0a 02 03",'81017e010a0203ff')]:
    go(label,c); go("   inquiry 7e010a",'81097e010aff',True)
