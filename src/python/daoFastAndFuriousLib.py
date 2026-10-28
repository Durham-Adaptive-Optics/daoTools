'''
Fast & Furious (F&F) focal-plane wavefront sensing.

Keller et al. 2012 (SPIE 8447), Korkiakoski et al. 2014 (arXiv:1406.1006), Bos et al. 2020
(A&A 639, A52). Sequential phase diversity on a non-coronagraphic PSF: the odd part of the
pupil phase comes from the odd part of the image, the even part's magnitude from the even
part of the image and its sign from the previous image, the diversity being the correction
applied on the DM between the two.

Modal: the phase is solved on the DM's modes (the basis: the pupil phase of each mode, rad
per unit of command), through a reconstructor computed once, from the focal-plane fields
of the modes to the modal coefficients. The focal plane is computed with a matrix Fourier
transform, so the camera's sampling needs not be an integer padding of the pupil.

Conventions: pupil field A exp(i phi), focal field E = F{A exp(i phi)}, image p = |E|^2.
With A even and the grids centrosymmetric:
    a = F{A}            real, even
    v = Re F{A phi}     F{A phi_e}, real, even
    y = Im F{A phi}     F{A phi_o} / i, real, odd
    p ~ a^2 - 2 a y + y^2 + v^2:  p_e = a^2 + v^2 + y^2,  p_o = -2 a y
The image used is the crop of the camera frame centred on the optical axis (nIm x nIm,
the centre at (nIm - 1) / 2), turned by one of the 8 orientations (transpose and flips)
that map the camera onto the model: found by calibrate().
'''

import numpy as np

# ========================================================================================== #
# ORIENTATIONS: bit 2 transpose, bit 1 flip rows, bit 0 flip columns (in that order)
# ========================================================================================== #

def orient(img, k):
    '''The image (..., n, n) in orientation k (0..7).'''
    if k & 4:
        img = np.swapaxes(img, -1, -2)
    if k & 2:
        img = img[..., ::-1, :]
    if k & 1:
        img = img[..., ::-1]
    return img


def orientInverse(k):
    '''The orientation that undoes k.'''
    x = np.arange(9).reshape(3, 3)
    for j in range(8):
        if np.array_equal(orient(orient(x, k), j), x):
            return j
    raise ValueError(k)

# ========================================================================================== #
# PUPIL, MATRIX FOURIER TRANSFORM
# ========================================================================================== #

def circularPupil(res, obstruction=0.0):
    '''A res x res pupil, the diameter res pixels, centred at (res - 1) / 2.'''
    x = (np.arange(res) - (res - 1) / 2) / (res / 2)
    r = np.hypot(*np.meshgrid(x, x))
    return ((r <= 1) & (r >= obstruction)).astype(np.float64)


def mftMatrix(nPup, pupDiam, nIm, samp):
    '''nIm x nPup: the 1D transform from a pupil of nPup pixels (pupDiam across the
    diameter) to nIm focal-plane pixels sampled samp pixels per lambda/D, both grids
    centred at (n - 1) / 2.'''
    x = (np.arange(nPup) - (nPup - 1) / 2) / pupDiam
    u = (np.arange(nIm) - (nIm - 1) / 2) / samp
    return np.exp(-2j * np.pi * np.outer(u, x))


def mft(W, field):
    '''The focal field of field (..., nPup, nPup).'''
    return np.matmul(np.matmul(W, field), W.T)

# ========================================================================================== #
# F&F
# ========================================================================================== #

class FastAndFurious:
    '''
    The F&F estimator.

    pupil:   nPup x nPup amplitude; its centrosymmetric part is used (the method assumes it)
    basis:   nModes x nPup x nPup, the pupil phase of each mode, rad per unit of command
    samp:    camera pixels per lambda/D
    nIm:     the crop's size, pixels
    pupDiam: pixels across the pupil's diameter (default nPup)
    eps:     the odd part's regularisation, relative to max(a^2) (Korkiakoski: 50-500 x
             the image's noise)
    window:  the focal-plane weighting's radius, lambda/D: a concave parabola, 0 beyond
             (default: the crop's half width)
    reg:     the reconstructor's Tikhonov regularisation, relative to its mean eigenvalue
    '''

    def __init__(self, pupil, basis, samp, nIm, pupDiam=None, eps=1e-3, window=None, reg=1e-3):
        pupil = np.asarray(pupil, dtype=np.float64)
        basis = np.asarray(basis, dtype=np.float64)
        nPup = pupil.shape[0]
        self.nModes = basis.shape[0]
        self.nIm = nIm
        self.samp = samp
        self.pupDiam = nPup if pupDiam is None else pupDiam
        self.asymmetry = np.abs(pupil - pupil[::-1, ::-1]).sum() / max(pupil.sum(), 1e-12)
        A = 0.5 * (pupil + pupil[::-1, ::-1])
        self.pupil = A
        # piston is invisible, the linear model would see it: removed
        w = A / A.sum()
        self.basis = basis - np.tensordot(basis, w, axes=([1, 2], [0, 1]))[:, None, None]

        self.W = mftMatrix(nPup, self.pupDiam, nIm, samp)
        self.a = mft(self.W, A).real
        self.a2 = self.a ** 2
        self.eps = eps * self.a2.max()

        G = mft(self.W, A * self.basis).reshape(self.nModes, -1)    # F{A B_i}
        self.G = G

        u = (np.arange(nIm) - (nIm - 1) / 2) / samp
        r = np.hypot(*np.meshgrid(u, u))
        if window is None:
            window = (nIm - 1) / 2 / samp
        self.window = np.clip(1 - (r / window) ** 2, 0, None)
        M = np.concatenate([G.real, G.imag], axis=1).T                  # 2 nIm^2 x nModes
        wt = np.concatenate([self.window.ravel()] * 2)
        MtW = M.T * wt
        H = MtW @ M
        H += reg * np.trace(H) / self.nModes * np.eye(self.nModes)
        self.R = np.linalg.solve(H, MtW)                                # nModes x 2 nIm^2
        self.reset()

    def reset(self):
        self.prevPe = None

    def normalize(self, image):
        '''Korkiakoski eqs. 12-13: the image scaled to a's flux, the peak's loss added back.'''
        p = np.asarray(image, dtype=np.float64)
        p = p * (self.a2.sum() / max(p.sum(), 1e-30))
        strehl = p.max() / self.a2.max()
        return p + (1 - strehl) * self.a2, strehl

    def field(self, coefs):
        '''F{A phi} of the modal phase coefs: (v, y).'''
        f = (np.asarray(coefs, dtype=np.float64) @ self.G).reshape(self.nIm, self.nIm)
        return f.real, f.imag

    def estimate(self, image, diversity=None):
        '''
        The modal phase of image (nIm x nIm, oriented). diversity: the modal change applied
        between the previous image given and this one, None if unknown (only the odd part
        is then measured). Returns (coefs, strehl): strehl, the normalised peak, a proxy.
        '''
        p, strehl = self.normalize(image)
        pr = p[::-1, ::-1]
        pe = 0.5 * (p + pr)
        po = 0.5 * (p - pr)
        y = -self.a * po / (2 * self.a2 + self.eps)
        if diversity is not None and self.prevPe is not None:
            vd, yd = self.field(diversity)
            # previous image: a^2 + (v - vd)^2 + (y - yd)^2 on its even part
            num = pe - self.prevPe + vd ** 2 + yd ** 2 - 2 * y * yd
            sign = np.where(num * vd < 0, -1.0, 1.0)
            v = sign * np.sqrt(np.abs(pe - self.a2 - y ** 2))
        else:
            v = np.zeros_like(y)
        self.prevPe = pe
        coefs = self.R @ np.concatenate([v.ravel(), y.ravel()])
        return coefs, strehl

    def model(self, coefs):
        '''The normalised model image of the modal phase coefs (the full exponential).'''
        phi = np.tensordot(np.asarray(coefs, dtype=np.float64), self.basis, axes=1)
        p = np.abs(mft(self.W, self.pupil * np.exp(1j * phi))) ** 2
        return p * self.a2.sum() / p.sum()


class FastAndFuriousLoop:
    '''F&F closed on the DM: theta(k) = leak theta(k-1) - gain coefs(k) (Korkiakoski eq. 22),
    the diversity of the next image being theta(k) - theta(k-1).'''

    def __init__(self, ff, gain=0.3, leak=0.97):
        self.ff = ff
        self.gain = gain
        self.leak = leak
        self.reset()

    def reset(self, theta=None):
        self.theta = np.zeros(self.ff.nModes) if theta is None else np.array(theta, dtype=np.float64)
        self.delta = None
        self.ff.reset()

    def step(self, image):
        '''image taken with the current theta; returns the next theta, to apply.'''
        coefs, strehl = self.ff.estimate(image, self.delta)
        new = self.leak * self.theta - self.gain * coefs
        self.delta = new - self.theta
        self.theta = new
        return self.theta, strehl

    def hold(self):
        '''theta not changed (loop disabled): the next image has no known diversity.'''
        self.delta = None

# ========================================================================================== #
# CAMERA FRAME -> IMAGE
# ========================================================================================== #

def findCenter(frame, box=3):
    '''(row, col) of the PSF's peak, the frame smoothed over box x box pixels.'''
    frame = np.asarray(frame, dtype=np.float64)
    pad = (box // 2, box - 1 - box // 2)                  # as scipy's uniform_filter, mode reflect
    p = np.pad(frame, (pad, pad), mode='symmetric')
    h, w = frame.shape
    f = sum(p[i:i + h, j:j + w] for i in range(box) for j in range(box))
    return np.unravel_index(np.argmax(f), f.shape)


def crop(frame, center, nIm, k=0):
    '''The nIm x nIm crop of frame centred on center (its pixel at (nIm - 1) / 2), in
    orientation k; zero outside the frame.'''
    frame = np.asarray(frame, dtype=np.float64)
    out = np.zeros((nIm, nIm))
    r0 = int(center[0]) - (nIm - 1) // 2
    c0 = int(center[1]) - (nIm - 1) // 2
    rs, cs = max(r0, 0), max(c0, 0)
    re, ce = min(r0 + nIm, frame.shape[0]), min(c0 + nIm, frame.shape[1])
    if re > rs and ce > cs:
        out[rs - r0:re - r0, cs - c0:ce - c0] = frame[rs:re, cs:ce]
    return orient(out, k)

# ========================================================================================== #
# CALIBRATION
# ========================================================================================== #

def fitSampling(ff_args, image, span=0.2, n=41):
    '''The sampling that best matches image (centred crop) to a^2: scanned over
    samp (1 +- span). ff_args: dict of FastAndFurious arguments (pupil, samp, nIm, pupDiam).'''
    pupil = np.asarray(ff_args['pupil'], dtype=np.float64)
    A = 0.5 * (pupil + pupil[::-1, ::-1])
    nPup = A.shape[0]
    pupDiam = ff_args.get('pupDiam') or nPup
    img = np.asarray(image, dtype=np.float64)
    img = img / np.linalg.norm(img)
    best, bestScore = ff_args['samp'], -np.inf
    for s in ff_args['samp'] * np.linspace(1 - span, 1 + span, n):
        a2 = mft(mftMatrix(nPup, pupDiam, ff_args['nIm'], s), A).real ** 2
        score = np.sum(img * a2) / np.linalg.norm(a2)
        if score > bestScore:
            best, bestScore = s, score
    return best


def calibrate(ff, acquire, modes, amp, nIm=None):
    '''
    The camera's orientation and the basis' sign and scale, from pokes.

    acquire(theta) -> camera crop (nIm x nIm, centred, not oriented) with the modal command
    theta applied. Each mode of modes is poked +-amp: (p+ - p-) / 2, normalised, is the
    first-order (odd) response -2 a Im(G_i) amp, compared in the 8 orientations.
    Returns dict(orientation, scale, correlation, strehl): scale (signed) multiplies the
    basis. Even modes have no first-order response and do not weigh. The orientation is
    known up to the PSF's twin ambiguity (rot180 with the opposite sign), which is harmless.
    The aberration present damps the response by about the Strehl (strehl, of the
    unpoked image): the scale is divided by it.
    '''
    theta = np.zeros(ff.nModes)
    _, strehl = ff.normalize(acquire(theta))
    meas, pred = [], []
    for i in modes:
        theta[:] = 0
        theta[i] = amp
        pp = acquire(theta)
        theta[i] = -amp
        pm = acquire(theta)
        pp = pp * ff.a2.sum() / pp.sum()
        pm = pm * ff.a2.sum() / pm.sum()
        meas.append(0.5 * (pp - pm))
        _, y = ff.field(amp * (np.arange(ff.nModes) == i))
        pred.append(-2 * ff.a * y)
    meas, pred = np.array(meas), np.array(pred)
    pp2 = np.sum(pred * pred)
    scores = [np.sum(orient(meas, k) * pred) for k in range(8)]
    k = int(np.argmax(np.abs(scores)))
    corr = scores[k] / np.sqrt(pp2 * np.sum(meas * meas))
    return dict(orientation=k, scale=float(scores[k] / pp2 / max(strehl, 1e-3)),
                correlation=float(abs(corr)), strehl=float(strehl))

# ========================================================================================== #
# A SIMULATED CAMERA (tests, end-to-end checks)
# ========================================================================================== #

class Simulator:
    '''
    A camera imaging the pupil with a static aberration and the DM's modal command:
    frame (shape) with the PSF centred at center, in orientation `orientation` relative to
    the model (the camera = orient(model, orientInverse(orientation)) ), the DM making
    scale x basis per unit command. flux: photons in the PSF, readNoise: e- rms.
    '''

    def __init__(self, pupil, basis, samp, aberration, shape=(128, 128), center=(64, 64),
                 orientation=0, scale=1.0, pupDiam=None, flux=1e6, readNoise=0.0, nIm=None,
                 seed=0):
        self.pupil = np.asarray(pupil, dtype=np.float64)
        self.basis = np.asarray(basis, dtype=np.float64)
        nPup = self.pupil.shape[0]
        self.nIm = nIm or min(shape) | 1
        self.W = mftMatrix(nPup, pupDiam or nPup, self.nIm, samp)
        self.aberration = aberration
        self.shape = shape
        self.center = center
        self.k = orientInverse(orientation)
        self.scale = scale
        self.flux = flux
        self.readNoise = readNoise
        self.rng = np.random.default_rng(seed)

    def phase(self, theta):
        return self.aberration + self.scale * np.tensordot(np.asarray(theta, dtype=np.float64),
                                                           self.basis, axes=1)

    def frame(self, theta):
        p = np.abs(mft(self.W, self.pupil * np.exp(1j * self.phase(theta)))) ** 2
        p = orient(p * self.flux / p.sum(), self.k)
        if self.flux > 0:
            p = self.rng.poisson(p).astype(np.float64)
        if self.readNoise > 0:
            p += self.rng.normal(0, self.readNoise, p.shape)
        out = np.zeros(self.shape)
        h = (self.nIm - 1) // 2
        r0, c0 = self.center[0] - h, self.center[1] - h
        out[r0:r0 + self.nIm, c0:c0 + self.nIm] = p
        return out

# ========================================================================================== #
# BASIS FROM THE DM MAP
# ========================================================================================== #

def gaussianBasis(dmMap, m2a, res=128, diameter=None, coupling=0.3, rotation=0.0,
                  shift=(0.0, 0.0), stroke=1.0, obstruction=0.0, pupil=None):
    '''
    The pupil phase of the M2A modes, Gaussian influence functions on the DM map's active
    actuators (dmMap == 1, row-major: the order of the DM command).

    diameter:  the pupil's diameter in actuator pitches (default: the map's width - 1)
    coupling:  an influence function's value at the next actuator
    rotation:  degrees, the actuator grid rotated over the pupil; shift (x, y) in pitches
    stroke:    rad of phase per unit of actuator command (calibrate() corrects the scale)
    pupil:     res x res amplitude (default: circular, with the obstruction)
    Returns (pupil res x res, basis nModes x res x res).
    '''
    dmMap = np.asarray(dmMap)
    rows, cols = np.nonzero(dmMap == 1)
    ny, nx = dmMap.shape
    if diameter is None:
        diameter = nx - 1
    xa = cols - (nx - 1) / 2
    ya = rows - (ny - 1) / 2
    t = np.deg2rad(rotation)
    xa, ya = (np.cos(t) * xa - np.sin(t) * ya + shift[0],
              np.sin(t) * xa + np.cos(t) * ya + shift[1])
    g = (np.arange(res) - (res - 1) / 2) / res * diameter
    gx, gy = np.meshgrid(g, g)
    # nAct x res^2, built row by row to bound the memory
    ifs = np.empty((xa.size, res * res), dtype=np.float32)
    lnc = np.log(coupling)
    for n in range(xa.size):
        ifs[n] = np.exp(lnc * ((gx - xa[n]) ** 2 + (gy - ya[n]) ** 2)).ravel()
    m2a = np.asarray(m2a, dtype=np.float32)
    if m2a.shape[0] != xa.size:
        raise ValueError(f'M2A has {m2a.shape[0]} rows, the map {xa.size} active actuators')
    basis = (m2a.T @ ifs).reshape(-1, res, res) * stroke
    if pupil is None:
        pupil = circularPupil(res, obstruction)
    return pupil, basis
