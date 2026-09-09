import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parent / 'bench'))
from encounter_card_text import split_card_text, complete_card_band
import numpy as np
from PIL import Image


def row(text, box, confidence=1.):
    return {'box': box, 'candidates': [{'text': text, 'confidence': confidence}]}


def original():
    return {'rows': [
        row('K', [0.0276243095, 0.2162162139, 0.1408839780, 0.8513513491]),
        row('24.150', [0.1215469592, 0.0270270267, 0.8093922630, 0.8513513511])
    ]}


class SplitCardTextTest(unittest.TestCase):
    def test_original_split_tokens_with_camera_fringe(self):
        self.assertEqual(split_card_text(original())[0], [('K', '24.150')])

    def test_spatial_order_is_independent_of_ocr_return_order(self):
        value = original(); value['rows'].reverse()
        self.assertEqual(split_card_text(value)[0], [('K', '24.150')])

    def test_existing_glyph_normalization(self):
        value = original(); value['rows'][0]['candidates'][0]['text'] = 'К'
        self.assertEqual(split_card_text(value)[0], [('K', '24.150')])

    def test_missing_band_or_digit_or_decimal_is_not_supplied(self):
        for value in ({'rows': [original()['rows'][1]]},
                      {'rows': [original()['rows'][0]]}):
            self.assertEqual(split_card_text(value)[0], [])
        for text in ('24.15', '24150', '24,150', ':24.150', '.24.150'):
            value = original(); value['rows'][1]['candidates'][0]['text'] = text
            self.assertEqual(split_card_text(value)[0], [], text)

    def test_extra_punctuation_in_band_is_not_erased(self):
        value = original(); value['rows'][0]['candidates'][0]['text'] = 'K.'
        self.assertEqual(split_card_text(value)[0], [])

    def test_alternative_band_or_frequency_or_unparsed_literal_refuses(self):
        for slot, text in ((0, 'X'), (1, '24.151'), (1, '24,150')):
            value = original(); value['rows'][slot]['candidates'].append({'text':text, 'confidence':.75})
            self.assertEqual(split_card_text(value)[0], [])

    def test_low_confidence_component_refuses(self):
        for slot in (0, 1):
            value = original(); value['rows'][slot]['candidates'][0]['confidence'] = .49
            self.assertEqual(split_card_text(value)[0], [])

    def test_third_observation_refuses_even_when_it_is_not_a_band(self):
        value = original(); value['rows'].append(row('?', [.85,.1,.95,.8]))
        self.assertEqual(split_card_text(value)[0], [])

    def test_separate_lines_reverse_order_and_overlapping_tokens_refuse(self):
        for box in ([.1,.90,.22,.99], [.85,.2,.96,.85], [.1,.2,.4,.85]):
            value = original(); value['rows'][0]['box'] = box
            self.assertEqual(split_card_text(value)[0], [], box)

    def test_missing_nonfinite_degenerate_out_of_crop_geometry_refuses(self):
        for box in ([], [0,0,1], [0,0,float('nan'),1], [0,0,0,1], [-.1,0,.1,1], [0,0,1.1,1]):
            value = original(); value['rows'][0]['box'] = box
            self.assertEqual(split_card_text(value)[0], [], box)

    def test_no_mutation_of_original_ocr_evidence(self):
        value = original(); before = copy.deepcopy(value); split_card_text(value)
        self.assertEqual(value, before)


class PixelCrops:
    """Source GFX glyph pixels on a fixed background; no saved-run dependency."""
    scale = (1., 1.0126582278481013)
    def __init__(self, band='K'):
        self.left = 393
        self.glyph = np.empty((37,28,3),dtype='uint8');self.glyph[:]=[0,10,132]
        self.gap = np.empty((25,3,3),dtype='uint8');self.gap[:]=[0,10,132]
        font={'K':(127,8,20,34,65),'X':(99,20,8,20,99),'L':(127,64,64,64,64),
              'a':(32,84,84,120,64),'u':(60,64,64,32,124)}
        for index, letter in enumerate(band):
            bits=np.array([[(value>>row)&1 for value in font[letter]]
                           for row in range(7)],dtype='uint8')*255
            mask=np.asarray(Image.fromarray(bits).resize((17,23),Image.Resampling.NEAREST))>0
            start=3+20*index;end=min(28,start+17)
            if start<28:self.glyph[5:28,start:end][mask[:,:end-start]]=[50,150,255]
            if index:self.gap[5:20]=[50,150,255]
    def crop(self, box):
        if box==(self.left+51,377,self.left+79,413):return self.glyph
        if box==(self.left+74,382,self.left+77,407):return self.gap
        raise AssertionError('Unexpected image search')

def observation(text='24.150'):
    return {'rows':[{'box':[.1243093901,.0270270267,.8121546940,.8513513511],
                     'candidates':[{'text':text,'confidence':1.}]}]}

class CompleteCardBandTest(unittest.TestCase):
    def run_pixel(self, pixels, obs=None):
        return complete_card_band(pixels,pixels.left,observation() if obs is None else obs)
    def erase(self, p, box):
        x0,y0,x1,y1=box;p.glyph[y0:y1,x0:x1]=np.median(p.glyph[:3],axis=(0,1)).astype('uint8')
    def test_complete_K_frequency_is_preserved(self):
        self.assertEqual(self.run_pixel(PixelCrops())[0],[('K','24.150')])
    def test_Ka_Ku_X_L_cannot_become_K(self):
        for band in ('Ka','Ku','X','L'):
            self.assertEqual(self.run_pixel(PixelCrops(band))[0],[],band)
    def test_removed_Ka_suffix_cannot_change_two_letter_layout(self):
        p=PixelCrops(); obs=observation('35.500');obs['rows'][0]['box']=[.2787,.027,.912,.85]
        # Deliberately give complete K and an empty suffix gutter, the strongest
        # possible erased-suffix appearance. The original frequency stays far.
        self.assertEqual(self.run_pixel(p,obs)[0],[])
    def test_truncated_K_cut_stem_and_missing_each_diagonal_refuse(self):
        for box in ((0,0,28,15),(0,20,28,37),(2,14,8,20),(9,8,21,14),(9,20,21,27)):
            p=PixelCrops();self.erase(p,box);self.assertEqual(self.run_pixel(p)[0],[],box)
    def test_low_contrast_complete_K_refuses(self):
        p=PixelCrops();bg=np.median(p.glyph[:3],axis=(0,1));p.glyph=np.clip(bg+.05*(p.glyph.astype(float)-bg),0,255).astype('uint8');self.assertEqual(self.run_pixel(p)[0],[])
    def test_extra_competing_stroke_refuses(self):
        p=PixelCrops();p.glyph[8:26,17:20]=p.glyph[8:26,4:7];self.assertEqual(self.run_pixel(p)[0],[])
    def test_suffix_ink_in_blank_gutter_refuses(self):
        p=PixelCrops();p.gap[5:17,:]=[50,160,255];self.assertEqual(self.run_pixel(p)[0],[])
    def test_other_channel_competing_ink_inside_K_is_not_ignored(self):
        for strength in (255,40):
            p=PixelCrops();p.glyph[9:25,16:20,0]=strength;self.assertEqual(self.run_pixel(p)[0],[],strength)
    def test_other_channel_suffix_ink_is_not_ignored(self):
        for color in ([255,10,132],[40,10,132],[0,150,132],[0,10,220]):
            p=PixelCrops();p.gap[5:17,:]=color;self.assertEqual(self.run_pixel(p)[0],[],color)
    def test_bad_numeric_token_or_other_row_never_repaired(self):
        for token in ('24.15','24150','24,150',':24.150','K24.150','２４.１５０',''):
            self.assertEqual(self.run_pixel(PixelCrops(),observation(token))[0],[],token)
        obs=observation();obs['rows'].append(copy.deepcopy(obs['rows'][0]));self.assertEqual(self.run_pixel(PixelCrops(),obs)[0],[])
    def test_conflicting_ocr_candidate_or_low_confidence_refuses(self):
        obs=observation();obs['rows'][0]['candidates'].append({'text':'24.151','confidence':.8});self.assertEqual(self.run_pixel(PixelCrops(),obs)[0],[])
        obs=observation();obs['rows'][0]['candidates'][0]['confidence']=.49;self.assertEqual(self.run_pixel(PixelCrops(),obs)[0],[])
    def test_bad_numeric_geometry_refuses(self):
        for box in ([],[0,0,1],[float('nan'),0,1,1],[.124,.027,.124,.85],[-1,0,.81,.85],[.3,.027,.98,.85],[.12,.7,.81,.95]):
            obs=observation();obs['rows'][0]['box']=box;self.assertEqual(self.run_pixel(PixelCrops(),obs)[0],[],box)
    def test_original_pixels_and_ocr_are_not_mutated(self):
        p=PixelCrops();o=observation();before=copy.deepcopy(o);g=p.glyph.copy();self.run_pixel(p,o);self.assertEqual(o,before);np.testing.assert_array_equal(p.glyph,g)


if __name__ == '__main__': unittest.main()
