from vp import *
for ip in sys.argv[1:]:
    print(ip)
    v=V(ip); v.reset(); v.show(v.rx(0.5))
    v.inq(bytes.fromhex('81090002ff')); v.show(v.rx(1))
    v.inq(bytes.fromhex('81090400ff')); v.show(v.rx(1))
    v.inq(bytes.fromhex('81090612ff')); v.show(v.rx(1))
    v.sock.close()
