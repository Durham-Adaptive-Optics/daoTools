#!/usr/bin/env python3
'''
Fast & Furious focal-plane wavefront sensing on a DAO pipeline.

The F&F loop reads a non-coronagraphic PSF camera and writes its modal correction theta
as an offset on an output SHM: out = out0 + T theta, out0 the SHM's content at start, T
chosen by the SHM's size:
  nModes values      a modal command channel (T = identity)
  M2A's rows (-z)    a DM actuator channel (T = M2A)
  IM's rows (-I)     a WFS reference (T = the modal interaction matrix): F&F with the AO
                     loop closed, the loop putting the correction on the DM

Subcommands:
  basis      the pupil phase of the M2A modes (Gaussian influence functions on the DM map)
  calibrate  the PSF's centre, the camera's orientation, the basis' sign and scale (pokes)
  run        the F&F loop
  sim        a simulated camera reading the output SHM (end-to-end tests)

Example:
  daoFastAndFurious.py basis -m /tmp/dmMap.im.shm -z /tmp/dmM2A.im.shm -M 50 ffBasis.npz
  daoFastAndFurious.py calibrate -i /tmp/sciIm.im.shm -o /tmp/ctrlModes05.im.shm \\
                       -B ffBasis.npz -q 7.5 -N 63 ffCalib.npz
  daoFastAndFurious.py run -i /tmp/sciIm.im.shm -o /tmp/ctrlModes05.im.shm \\
                       -B ffBasis.npz -C ffCalib.npz -g 0.3 -l 0.97
'''

import sys
import time
import signal
import argparse
import numpy as np

import dao
import daoFastAndFuriousLib as ff

end = False


def endme(_sig, _frame):
    global end
    end = True

# ========================================================================================== #
# SHM HELPERS
# ========================================================================================== #

def readShm(name, dtype=np.float64):
    return np.asarray(dao.shm(name).get_data(), dtype=dtype)


def readScalar(shm, default):
    if shm is None:
        return default
    return float(np.asarray(shm.get_data()).ravel()[0])


class Camera:
    '''The PSF camera: frames averaged, the background (-b) subtracted.'''

    def __init__(self, name, bgName=None):
        self.shm = dao.shm(name)
        self.bg = readShm(bgName) if bgName else 0.0

    def frame(self, nFrames=1, skip=0):
        for _ in range(skip):
            self.shm.get_data(check=True)
        acc = 0.0
        for _ in range(nFrames):
            acc = acc + np.asarray(self.shm.get_data(check=True), dtype=np.float64)
        return acc / nFrames - self.bg


class Output:
    '''out = out0 + T theta, T found from the output SHM's size (see the module's doc).'''

    def __init__(self, name, nModes, m2aName=None, imName=None):
        self.shm = dao.shm(name)
        data = np.asarray(self.shm.get_data())
        self.shape, self.dtype = data.shape, data.dtype
        self.out0 = data.astype(np.float64).ravel().copy()
        size = self.out0.size
        m2a = readShm(m2aName) if m2aName else None
        im = readShm(imName) if imName else None
        if size == nModes:
            self.T, self.kind = None, 'modes'
        elif m2a is not None and m2a.shape[0] == size:
            self.T, self.kind = m2a[:, :nModes], 'actuators (M2A)'
        elif im is not None and im.shape[0] == size:
            self.T, self.kind = im[:, :nModes], 'WFS reference (IM)'
        else:
            raise SystemExit(f'{name}: {size} values, neither the {nModes} modes, M2A\'s '
                             f'nor IM\'s rows (-z / -I)')
        if self.T is not None and self.T.shape[1] < nModes:
            raise SystemExit(f'the basis has {nModes} modes, {self.T.shape[1]} in M2A / IM')

    def write(self, theta):
        out = self.out0 + (theta if self.T is None else self.T @ theta)
        self.shm.set_data(out.reshape(self.shape).astype(self.dtype))

    def restore(self):
        self.shm.set_data(self.out0.reshape(self.shape).astype(self.dtype))


def loadBasis(path):
    d = np.load(path)
    pupDiam = float(d['pupDiam']) if 'pupDiam' in d else None
    return d['pupil'], d['basis'], pupDiam


def makeFF(args, basis=None, samp=None):
    pupil, b, pupDiam = loadBasis(args.basis)
    if basis is not None:
        b = basis
    nModes = args.modes or b.shape[0]
    return ff.FastAndFurious(pupil, b[:nModes], samp or args.samp, args.size, pupDiam=pupDiam,
                             eps=args.eps, window=args.window, reg=args.reg)

# ========================================================================================== #
# SUBCOMMANDS
# ========================================================================================== #

def cmdBasis(args):
    dmMap = readShm(args.map, np.int32)
    m2a = readShm(args.m2a)
    if args.nmodes:
        m2a = m2a[:, :args.nmodes]
    pupil = np.load(args.pupil) if args.pupil else None
    res = pupil.shape[0] if pupil is not None else args.res
    pupil, basis = ff.gaussianBasis(dmMap, m2a, res=res, diameter=args.diameter,
                                    coupling=args.coupling, rotation=args.rotation,
                                    shift=(args.shiftx, args.shifty), stroke=args.stroke,
                                    obstruction=args.obstruction, pupil=pupil)
    np.savez(args.output, pupil=pupil, basis=basis.astype(np.float32), pupDiam=res)
    print(f'{args.output}: {basis.shape[0]} modes on a {res} x {res} pupil')


def cmdCalibrate(args):
    cam = Camera(args.input, args.background)
    fnf = makeFF(args)
    out = Output(args.output, fnf.nModes, args.m2a, args.im)
    print(f'output {args.output}: {out.kind}')

    def acquire(theta, center=None):
        out.write(theta)
        frame = cam.frame(args.frames, args.delay)
        return frame if center is None else ff.crop(frame, center, args.size)

    try:
        ref = acquire(np.zeros(fnf.nModes))
        center = ff.findCenter(ref)
        print(f'PSF centre (row, col): {center[0]}, {center[1]}')
        samp = args.samp
        if args.fit_samp:
            samp = ff.fitSampling(dict(pupil=fnf.pupil, samp=args.samp, nIm=args.size,
                                       pupDiam=fnf.pupDiam), ff.crop(ref, center, args.size))
            print(f'sampling: {samp:.3f} px per lambda/D (given {args.samp})')
            fnf = makeFF(args, samp=samp)
        modes = range(min(args.cal_modes, fnf.nModes))
        cal = ff.calibrate(fnf, lambda th: acquire(th, center), modes, args.amp)
    finally:
        out.restore()
    print(f'orientation {cal["orientation"]}, scale {cal["scale"]:.4g}, correlation '
          f'{cal["correlation"]:.3f}, Strehl ~ {cal["strehl"]:.3f}')
    if cal['correlation'] < 0.3:
        print('WARNING: low correlation, check the pokes (-a), the sampling (-q), the crop (-N)')
    np.savez(args.calib, center=np.array(center), orientation=cal['orientation'],
             scale=cal['scale'], samp=samp, size=args.size, correlation=cal['correlation'])
    print(f'calibration written to {args.calib}')


def cmdRun(args):
    cal = np.load(args.calib)
    if int(cal['size']) != args.size:
        raise SystemExit(f'the calibration was done with -N {int(cal["size"])}')
    pupil, basis, _ = loadBasis(args.basis)
    fnf = makeFF(args, basis=basis * float(cal['scale']), samp=float(cal['samp']))
    center, k = tuple(int(c) for c in cal['center']), int(cal['orientation'])
    cam = Camera(args.input, args.background)
    out = Output(args.output, fnf.nModes, args.m2a, args.im)
    loop = ff.FastAndFuriousLoop(fnf, args.gain, args.leak)
    gainShm = dao.shm(args.gain_shm) if args.gain_shm else None
    leakShm = dao.shm(args.leak_shm) if args.leak_shm else None
    enableShm = dao.shm(args.enable) if args.enable else None
    phaseShm = None
    if args.phase:
        phaseShm = dao.shm(args.phase, np.zeros(fnf.pupil.shape, dtype=np.float32))
    print(f'output {args.output}: {out.kind}; {fnf.nModes} modes, centre {center}, '
          f'orientation {k}')

    signal.signal(signal.SIGINT, endme)
    signal.signal(signal.SIGTERM, endme)
    it = 0
    out.write(loop.theta)
    while not end and (args.iterations == 0 or it < args.iterations):
        image = ff.crop(cam.frame(args.frames, args.delay), center, args.size, k)
        loop.gain = readScalar(gainShm, args.gain)
        loop.leak = readScalar(leakShm, args.leak)
        if readScalar(enableShm, 1) == 0:
            # held: the estimator still runs, the next image has no known diversity
            fnf.estimate(image)
            loop.hold()
            time.sleep(0.01)
            continue
        theta, strehl = loop.step(image)
        out.write(theta)
        if phaseShm is not None:
            phaseShm.set_data(np.tensordot(theta, fnf.basis, axes=1).astype(np.float32))
        it += 1
        sys.stdout.write(f'\riteration {it}: Strehl ~ {strehl:.3f}, |theta| {np.linalg.norm(theta):.4g}  ')
        sys.stdout.flush()
    print()
    if args.restore:
        out.restore()
        print('output restored')
    if args.save:
        np.save(args.save, loop.theta)
        print(f'theta saved to {args.save}')


def cmdSim(args):
    pupil, basis, pupDiam = loadBasis(args.basis)
    nModes = args.modes or basis.shape[0]
    basis = basis[:nModes]
    rng = np.random.default_rng(args.seed)
    ab = np.tensordot(rng.normal(size=nModes), basis, axes=1)
    m = pupil > 0
    ab -= ab[m].mean()
    ab *= args.rms / np.sqrt(np.mean(ab[m] ** 2))
    shape = (args.frame, args.frame)
    center = (args.frame // 2 + args.offset[0], args.frame // 2 + args.offset[1])
    sim = ff.Simulator(pupil, basis, args.samp, ab, shape=shape, center=center,
                       orientation=args.orientation, scale=args.scale, pupDiam=pupDiam,
                       flux=args.flux, readNoise=args.read_noise, nIm=args.size, seed=args.seed)
    cmdShm = dao.shm(args.output, np.zeros((nModes, 1), dtype=np.float32))
    camShm = dao.shm(args.input, np.zeros(shape, dtype=np.float32))
    print(f'simulating {args.input} from {args.output} ({nModes} modes, {args.rms} rad rms, '
          f'orientation {args.orientation}, scale {args.scale}); Ctrl-C to stop')
    signal.signal(signal.SIGINT, endme)
    signal.signal(signal.SIGTERM, endme)
    while not end:
        theta = np.asarray(cmdShm.get_data(), dtype=np.float64).ravel()
        camShm.set_data(sim.frame(theta).astype(np.float32))
        time.sleep(1.0 / args.rate)

# ========================================================================================== #
# ARGUMENTS
# ========================================================================================== #

def addFF(p, output=True):
    p.add_argument('-i', '--input', required=True, help='PSF camera SHM')
    p.add_argument('-b', '--background', help='background SHM, subtracted')
    if output:
        p.add_argument('-o', '--output', required=True,
                       help='output SHM: modal channel, actuator channel (-z) or WFS reference (-I)')
        p.add_argument('-z', '--m2a', help='M2A SHM (nAct x nModes): output on an actuator channel')
        p.add_argument('-I', '--im', help='modal interaction matrix SHM (nSignal x nModes): output on a WFS reference')
    p.add_argument('-B', '--basis', required=True, help='basis file (npz: pupil, basis[, pupDiam])')
    p.add_argument('-M', '--modes', type=int, default=0, help='modes used (default: all the basis)')
    p.add_argument('-q', '--samp', type=float, default=2.0, help='camera pixels per lambda/D')
    p.add_argument('-N', '--size', type=int, default=63, help='crop size, pixels (odd: centred on a pixel)')
    p.add_argument('-n', '--frames', type=int, default=1, help='frames averaged per image')
    p.add_argument('-d', '--delay', type=int, default=2, help='frames skipped after a command')
    p.add_argument('--eps', type=float, default=1e-3, help='odd-part regularisation, relative to the PSF peak')
    p.add_argument('--window', type=float, default=None, help='focal-plane window radius, lambda/D')
    p.add_argument('--reg', type=float, default=1e-3, help='reconstructor regularisation')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('basis', help='the pupil phase of the M2A modes')
    p.add_argument('output', help='basis file to write (npz)')
    p.add_argument('-m', '--map', required=True, help='DM map SHM (1: active actuator)')
    p.add_argument('-z', '--m2a', required=True, help='M2A SHM (nAct x nModes)')
    p.add_argument('-M', '--nmodes', type=int, default=0, help='first modes kept (default: all)')
    p.add_argument('-r', '--res', type=int, default=128, help='pupil pixels')
    p.add_argument('-p', '--pupil', help='pupil amplitude (npy, res x res; default: circular)')
    p.add_argument('-D', '--diameter', type=float, default=None,
                   help='pupil diameter in actuator pitches (default: map width - 1)')
    p.add_argument('-c', '--coupling', type=float, default=0.3, help='influence function at the next actuator')
    p.add_argument('--obstruction', type=float, default=0.0, help='central obstruction (fraction of D)')
    p.add_argument('--rotation', type=float, default=0.0, help='actuator grid rotation, degrees')
    p.add_argument('--shiftx', type=float, default=0.0, help='actuator grid shift, pitches')
    p.add_argument('--shifty', type=float, default=0.0, help='actuator grid shift, pitches')
    p.add_argument('--stroke', type=float, default=1.0, help='rad of phase per unit of command')
    p.set_defaults(func=cmdBasis)

    p = sub.add_parser('calibrate', help='PSF centre, orientation, basis sign and scale')
    p.add_argument('calib', help='calibration file to write (npz)')
    addFF(p)
    p.add_argument('-a', '--amp', type=float, default=0.1, help='poke amplitude, command units')
    p.add_argument('-k', '--cal-modes', type=int, default=20, help='first modes poked')
    p.add_argument('-f', '--fit-samp', action='store_true', help='fit the sampling on the PSF')
    p.set_defaults(func=cmdCalibrate)

    p = sub.add_parser('run', help='the F&F loop')
    addFF(p)
    p.add_argument('-C', '--calib', required=True, help='calibration file (npz)')
    p.add_argument('-g', '--gain', type=float, default=0.3, help='loop gain')
    p.add_argument('-l', '--leak', type=float, default=0.97, help='loop leak (1: pure integrator)')
    p.add_argument('-G', '--gain-shm', help='loop gain SHM (1x1), overrides -g')
    p.add_argument('-K', '--leak-shm', help='loop leak SHM (1x1), overrides -l')
    p.add_argument('-e', '--enable', help='enable SHM (1x1): 0 holds the correction')
    p.add_argument('-p', '--phase', help='SHM written with the correction\'s pupil phase')
    p.add_argument('-k', '--iterations', type=int, default=0, help='iterations (0: until Ctrl-C)')
    p.add_argument('-R', '--restore', action='store_true', help='restore the output at exit')
    p.add_argument('-s', '--save', help='save the final theta (npy)')
    p.set_defaults(func=cmdRun)

    p = sub.add_parser('sim', help='a simulated PSF camera')
    addFF(p, output=False)
    p.add_argument('-o', '--output', required=True, help='modal command SHM read (created, nModes x 1)')
    p.add_argument('--rms', type=float, default=0.5, help='static aberration, rad rms')
    p.add_argument('--orientation', type=int, default=0, help='camera orientation (0-7)')
    p.add_argument('--scale', type=float, default=1.0, help='true phase per unit / basis')
    p.add_argument('--flux', type=float, default=1e7, help='photons in the PSF (0: no noise)')
    p.add_argument('--read-noise', type=float, default=0.0, help='e- rms')
    p.add_argument('--frame', type=int, default=128, help='camera frame size')
    p.add_argument('--offset', type=int, nargs=2, default=(3, -5), help='PSF offset (row, col)')
    p.add_argument('--rate', type=float, default=100.0, help='frames per second')
    p.add_argument('--seed', type=int, default=0)
    p.set_defaults(func=cmdSim)

    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
