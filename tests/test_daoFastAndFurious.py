'''
 # @ Description: Unit tests for daoFastAndFuriousLib (Fast & Furious focal-plane wavefront
 #   sensing): the estimator, the calibration and the loop on a simulated camera.
 '''

# ========================================================================================== #
# IMPORTS
# ========================================================================================== #

import os
import sys
import numpy as np
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src', 'python'))
import daoFastAndFuriousLib as ff

# ========================================================================================== #
# DEFINES
# ========================================================================================== #

N_ACT = 10          # DM map width
RES = 40            # pupil pixels
SAMP = 3.0          # pixels per lambda/D
N_IM = 41           # crop

# ========================================================================================== #
# FIXTURES
# ========================================================================================== #

@pytest.fixture(scope='module')
def system():
    x = np.arange(N_ACT) - (N_ACT - 1) / 2
    dmMap = (np.hypot(*np.meshgrid(x, x)) <= N_ACT / 2 + 0.2).astype(int)
    nAct = int(dmMap.sum())
    pupil, basis = ff.gaussianBasis(dmMap, np.eye(nAct), res=RES)
    rng = np.random.default_rng(3)
    ab = np.tensordot(rng.normal(size=nAct), basis, axes=1)
    m = pupil > 0
    ab -= ab[m].mean()
    ab *= 0.5 / np.sqrt(np.mean(ab[m] ** 2))
    return pupil, basis, ab


def residual(sim, pupil, theta):
    ph = sim.phase(theta)[pupil > 0]
    return np.sqrt(np.mean((ph - ph.mean()) ** 2))

# ========================================================================================== #
# TESTS
# ========================================================================================== #

def test_orientations_invert():
    img = np.arange(25.0).reshape(5, 5)
    for k in range(8):
        assert np.array_equal(ff.orient(ff.orient(img, k), ff.orientInverse(k)), img)


def test_crop_centre():
    frame = np.zeros((32, 32))
    frame[10, 20] = 1
    c = ff.crop(frame, (10, 20), 7)
    assert c[3, 3] == 1 and c.sum() == 1
    assert ff.crop(frame, (0, 0), 7).sum() == 0      # outside: zeros


def test_model_is_real_and_even(system):
    pupil, basis, _ = system
    f = ff.FastAndFurious(pupil, basis, SAMP, N_IM)
    assert f.asymmetry < 1e-12
    assert np.allclose(f.a, f.a[::-1, ::-1])


def test_odd_mode_estimate(system):
    '''A small odd phase is measured from a single image.'''
    pupil, basis, _ = system
    f = ff.FastAndFurious(pupil, basis, SAMP, N_IM)
    theta = np.zeros(f.nModes)
    theta[0] = 0.05
    # its odd part, projected on the basis, is what one image measures
    _, y = f.field(theta)
    image = f.model(theta)
    coefs, strehl = f.estimate(image)
    _, ye = f.field(coefs)
    assert strehl > 0.95
    assert np.corrcoef(y.ravel(), ye.ravel())[0, 1] > 0.95


def test_calibrate_finds_orientation_and_scale(system):
    pupil, basis, ab = system
    sim = ff.Simulator(pupil, basis, SAMP, ab, shape=(96, 96), center=(50, 44),
                       orientation=6, scale=1.3, flux=1e8, nIm=N_IM)
    f = ff.FastAndFurious(pupil, basis, SAMP, N_IM)
    center = ff.findCenter(sim.frame(np.zeros(f.nModes)))
    assert tuple(int(c) for c in center) == (50, 44)
    cal = ff.calibrate(f, lambda th: ff.crop(sim.frame(th), center, N_IM),
                       range(f.nModes), 0.2)
    # up to the twin ambiguity: rot180 (k ^ 3) with the opposite sign
    assert (cal['orientation'], np.sign(cal['scale'])) in [(6, 1), (6 ^ 3, -1)]
    assert abs(abs(cal['scale']) - 1.3) < 0.15


@pytest.mark.parametrize('orientation', [0, 5])
def test_loop_converges(system, orientation):
    pupil, basis, ab = system
    sim = ff.Simulator(pupil, basis, SAMP, ab, shape=(96, 96), center=(48, 48),
                       orientation=orientation, scale=1.0, flux=1e7, nIm=N_IM, seed=1)
    f = ff.FastAndFurious(pupil, basis, SAMP, N_IM)
    center = ff.findCenter(sim.frame(np.zeros(f.nModes)))
    cal = ff.calibrate(f, lambda th: ff.crop(sim.frame(th), center, N_IM),
                       range(f.nModes), 0.2)
    loop = ff.FastAndFuriousLoop(ff.FastAndFurious(pupil, basis * cal['scale'], SAMP, N_IM),
                                 gain=0.3, leak=0.99)
    theta = loop.theta
    start = residual(sim, pupil, theta)
    for _ in range(40):
        theta, strehl = loop.step(ff.crop(sim.frame(theta), center, N_IM, cal['orientation']))
    assert start > 0.45
    assert residual(sim, pupil, theta) < 0.1
    assert strehl > 0.95


def test_hold_drops_diversity(system):
    pupil, basis, _ = system
    loop = ff.FastAndFuriousLoop(ff.FastAndFurious(pupil, basis, SAMP, N_IM))
    image = loop.ff.model(np.zeros(loop.ff.nModes))
    loop.step(image)
    assert loop.delta is not None
    loop.hold()
    assert loop.delta is None


def test_gaussian_basis_shapes():
    dmMap = np.ones((4, 4), dtype=int)
    dmMap[0, 0] = 0
    m2a = np.eye(15)[:, :5]
    pupil, basis = ff.gaussianBasis(dmMap, m2a, res=16)
    assert pupil.shape == (16, 16) and basis.shape == (5, 16, 16)
    with pytest.raises(ValueError):
        ff.gaussianBasis(dmMap, np.eye(16), res=16)
