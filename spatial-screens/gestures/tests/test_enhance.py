import numpy as np
import pytest
from enhance import make_enhancer, MODES


def _dark():  # 64x64 image resembling the hardware frames (mean ~12/255)
    rng = np.random.default_rng(0)
    return (rng.random((64, 64)) * 24).astype(np.uint8)


def test_none_is_identity():
    img = _dark()
    out = make_enhancer("none")(img)
    assert np.array_equal(out, img)


def test_gamma_brightens():
    img = _dark()
    out = make_enhancer("gamma", gamma=0.45)(img)
    assert out.dtype == np.uint8 and out.shape == img.shape
    assert out.mean() > img.mean() + 10


def test_clahe_adds_contrast():
    img = _dark()
    out = make_enhancer("clahe", clahe_clip=3.0)(img)
    assert out.dtype == np.uint8 and out.shape == img.shape
    assert out.std() > img.std()  # local contrast raised


def test_gamma_clahe_brightens_most():
    img = _dark()
    g = make_enhancer("gamma")(img)
    gc = make_enhancer("gamma_clahe")(img)
    assert gc.mean() > img.mean() and gc.std() >= g.std() - 1


def test_unknown_mode_raises():
    with pytest.raises(ValueError):
        make_enhancer("sharpen")


def test_modes_constant():
    assert set(MODES) == {"none", "gamma", "clahe", "gamma_clahe"}
