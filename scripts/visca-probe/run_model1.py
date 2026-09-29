from model import *
ip=sys.argv[1]; T=float(sys.argv[2]) if len(sys.argv)>2 else 5
m=Model(ip); m.protocol_reset(); m.pump(T)
print(ip); print(m.summary()); 
if '-v' in sys.argv: print('\n'.join(m.log))
