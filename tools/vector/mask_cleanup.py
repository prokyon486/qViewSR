"""Optional, bounded binary gap closing for dark centerline extraction.

This operates on the extracted mask, never on the source image. A radius of one
or two source pixels uses a 3x3 or 5x5 square respectively. Nearby intentional
strokes can merge, so the caller must expose this as an opt-in setting.
"""
import operator

import numpy as np
from PIL import Image, ImageFilter

MASK_VERSION = 'dark-gap-close-1'


def close_dark_gaps(mask, alpha, radius):
    """Return a new boolean mask, with transparent source pixels excluded.

    ``mask`` is a 2D boolean array from dark-line extraction and ``alpha`` is
    the corresponding Pillow L image. Radius zero is an exact bypass; the
    upstream extractor already excludes transparent pixels. Enabled closing
    also clears hidden mask values before filtering, so hidden RGB cannot seed
    a new visible line. The source mask and alpha image are never modified.
    """
    try:
        if isinstance(radius, (bool, np.bool_)):
            raise TypeError
        radius = operator.index(radius)
    except TypeError as exc:
        raise ValueError('Mask gap radius must be 0, 1 or 2 source pixels') from exc
    if radius not in (0, 1, 2):
        raise ValueError('Mask gap radius must be 0, 1 or 2 source pixels')
    source = np.asarray(mask)
    opacity = np.asarray(alpha)
    if source.ndim != 2 or source.dtype != np.bool_ or opacity.shape != source.shape:
        raise ValueError('Mask and alpha must have matching two-dimensional shapes')
    if radius == 0:
        return source.copy()
    visible = opacity > 0
    source = np.logical_and(source, visible)
    image = Image.fromarray(source.astype(np.uint8) * 255)
    size = radius * 2 + 1
    closed = image.filter(ImageFilter.MaxFilter(size)).filter(ImageFilter.MinFilter(size))
    result = np.asarray(closed) > 0
    # Closing should be extensive: explicitly keep every original visible mask
    # pixel, including those at the image edge where filter padding can differ.
    np.logical_or(result, source, out=result)
    np.logical_and(result, visible, out=result)
    return result
