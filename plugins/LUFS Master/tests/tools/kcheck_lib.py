import sys, math, numpy as np
from scipy.signal import lfilter, freqz
sys.path.insert(0, sys.argv[1]); from ref import read
def libebur128(sr):
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = math.tan(math.pi*f0/sr); Vh = 10**(G/20); Vb = Vh**0.4996667741545416; a0 = 1+K/Q+K*K
    b1 = [(Vh+Vb*K/Q+K*K)/a0, 2*(K*K-Vh)/a0, (Vh-Vb*K/Q+K*K)/a0]; a1 = [1, 2*(K*K-1)/a0, (1-K/Q+K*K)/a0]
    f0, Q = 38.13547087602444, 0.5003270373238773; K = math.tan(math.pi*f0/sr); a0 = 1+K/Q+K*K
    return b1, a1, [1,-2,1], [1, 2*(K*K-1)/a0, (1-K/Q+K*K)/a0]
official = ([1.53512485958697, -2.69169618940638, 1.19839281085285], [1, -1.69065929318241, 0.73248077421585],
            [1.0, -2.0, 1.0], [1, -1.99004745483398, 0.99007225036621])
def integ(x, sr, f):
    y = lfilter(f[2], f[3], lfilter(f[0], f[1], x, axis=0), axis=0)
    e = np.sum(y**2, axis=1); B = sr//10; nb = len(e)//B
    be = e[:nb*B].reshape(nb, B).mean(axis=1); w = (be[3:]+be[2:-1]+be[1:-2]+be[:-3])/4
    w = w[w > 10**((-70+0.691)/10)]; w = w[w > w.mean()*0.1]
    return -0.691 + 10*np.log10(w.mean())

