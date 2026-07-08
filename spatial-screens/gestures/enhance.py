"""Image brightening pre-pass for the gesture sidecar. Pure (cv2/numpy only),
unit-testable. The tracking cameras deliver very dark GRAY8 frames (mean ~12/255
measured on hardware 2026-07-08); lifting shadows makes hands detectable across
the frame, not just the lamp-lit zone.
See docs/specs/2026-07-08-centered-hand-tracking-design.md."""
import cv2
import numpy as np

MODES = ("none", "gamma", "clahe", "gamma_clahe")


def _gamma_lut(gamma):
    return np.array([((i / 255.0) ** gamma) * 255 for i in range(256)],
                    dtype=np.uint8)


def make_enhancer(mode="gamma_clahe", gamma=0.45, clahe_clip=3.0):
    """Return a callable(gray_u8)->gray_u8 applying the configured enhancement.
    The CLAHE object is built once and reused across frames."""
    if mode not in MODES:
        raise ValueError(f"unknown enhance mode {mode!r}; want one of {MODES}")
    if mode == "none":
        return lambda gray: gray
    lut = _gamma_lut(gamma) if "gamma" in mode else None
    clahe = (cv2.createCLAHE(clipLimit=clahe_clip, tileGridSize=(8, 8))
             if "clahe" in mode else None)

    def enhance(gray):
        out = gray
        if lut is not None:
            out = cv2.LUT(out, lut)
        if clahe is not None:
            out = clahe.apply(out)
        return out

    return enhance
