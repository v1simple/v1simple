"""Correct startup geometry only for an unresolved numeric decimal.

The five literal digits must already be established by the original bright
reader. No idle, dim numeric, noncanonical, extra-ink, or visibility refusal can
enter this route. The unchanged numeric reader must repeat those exact digits
and observe its decimal and empty-space guards after startup-only alignment.
"""
import numpy as np
import encounter_frequency_geometry as geometry

ELIGIBLE_REASON = 'frequency decimal is not clearly visible'


def original_literal(reading):
    from counter_reader import MASKS
    cells = reading.get('cells', [])
    if (reading.get('state') != 'ambiguous' or reading.get('reason') != ELIGIBLE_REASON
            or len(cells) != 5):
        return None
    digits=[]
    for cell in cells:
        segments=cell.get('segments', {})
        if set(segments) != set('abcdefg'):
            return None
        if any(s.get('state') not in ('on', 'off') for s in segments.values()):
            return None
        mask=''.join(s for s in 'abcdefg' if segments[s]['state']=='on')
        digit=MASKS.get(mask)
        if mask != cell.get('mask') or digit is None or not digit.isdigit():
            return None
        digits.append(digit)
    return ''.join(digits[:2])+'.'+''.join(digits[2:])


def refine(reading, rgb, width, height, registration):
    literal=original_literal(reading)
    if literal is None:
        return reading
    calibration=registration.get('primary_frequency_calibration')
    if not isinstance(calibration,dict) or calibration.get('qualified') is not True:
        return reading
    try:
        import cv2
        from encounter_reader import Pixels, _frequency
        if (width,height)!=(1280,720) or calibration.get('model_sha256')!=geometry.MODEL_SHA256:
            return reading
        matrix=np.asarray(calibration['matrix_reference_to_observed'],dtype=np.float64)
        if matrix.shape!=(3,3) or not np.isfinite(matrix).all() or not np.array_equal(matrix[2],[0,0,1]):
            return reading
        image=np.frombuffer(rgb,dtype=np.uint8).reshape(height,width,3)
        aligned=cv2.warpAffine(image,matrix[:2].astype(np.float32),(width,height),
                              flags=cv2.INTER_LINEAR|cv2.WARP_INVERSE_MAP)
        candidate=_frequency(Pixels(aligned.tobytes(),width,height,
                                    geometry.model_metadata()['reference_registration']))
        if candidate.get('state')!='readable' or candidate.get('value')!=literal:
            return reading
        return {**candidate,'calibrated_numeric_decimal':{
            'accepted':True, 'original_literal_without_decimal_witness':literal,
            'basis':'Five original numeric glyphs repeated unchanged with the fixed numeric decimal and empty-space witnesses after startup-only geometry correction.'},
            'prior_reading':{k:reading.get(k) for k in ('state','value','reason')}}
    except Exception:
        return reading
