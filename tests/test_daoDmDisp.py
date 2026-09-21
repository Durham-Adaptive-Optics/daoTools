"""Offscreen GUI checks with in-memory streams; no live DM commands are touched."""
import importlib.util
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
from types import SimpleNamespace

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import numpy as np
from PyQt5.QtWidgets import QApplication

spec = importlib.util.spec_from_file_location(
    'daoDmDisp', Path(__file__).resolve().parents[1] / 'gui' / 'daoDmDisp.py')
display = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {'dao': SimpleNamespace()}):
    spec.loader.exec_module(display)


class MemoryStream:
    def __init__(self, data):
        self.data = data.copy()

    def get_data(self):
        return self.data.copy()

    def set_data(self, data):
        self.data = data.copy()

    def get_counter(self):
        return 0


class DisplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def test_channel_counts_update_colormap_and_reset(self):
        for channels in (1, 4, 6, 8, 16):
            with self.subTest(channels=channels):
                mask = np.array([[1, 0], [1, 1]], dtype=np.float32)
                streams = {'/tmp/dmMap.im.shm': MemoryStream(mask)}
                names = ['dmCmd'] + [f'dmCmd{i:02d}' for i in range(channels)]
                for i, name in enumerate(names):
                    streams[f'/tmp/{name}.im.shm'] = MemoryStream(
                        np.full((3, 1), i + 1, dtype=np.float32))
                with patch.object(display.dao, 'shm', streams.__getitem__, create=True):
                    window = display.Main('dmCmd', 'dmMap', channels=channels)
                try:
                    window.show()
                    self.app.processEvents()
                    self.assertEqual(window.grid.count(), channels + 1)
                    self.assertEqual(window.grid.columnCount(), 3)
                    for position in range(channels + 1):
                        self.assertEqual(window.grid.getItemPosition(position),
                                         (*divmod(position, 3), 1, 1))
                    for i, image in enumerate(window.images):
                        np.testing.assert_array_equal(image.image[mask == 1], i + 1)
                    window.channel_shms[-1].set_data(
                        np.array([[-2], [1], [3]], dtype=np.float32))
                    window.cmapCombo.setCurrentText('bwr')
                    np.testing.assert_array_equal(
                        window.images[-1].image[mask == 1], [-2, 1, 3])
                    np.testing.assert_array_equal(window.images[-1].getLevels(), [-3, 3])
                    window.pushButton.click()
                    window.Update()
                    for stream in window.streams:
                        np.testing.assert_array_equal(stream.get_data(), 0)
                    self.assertEqual(window.minLabel.text(), '0.0')
                    self.assertEqual(window.maxLabel.text(), '0.0')
                finally:
                    window.close()

    def test_arguments(self):
        self.assertEqual(display.parse_args([]).channels, 4)
        args = display.parse_args(['-n', '6', '-l', 'a,b,c,d,e,f', '--light'])
        self.assertEqual(args.labels, list('abcdef'))
        self.assertTrue(args.light)
        for arguments in (['-n', '0'], ['-n', '-1'], ['-n', 'six'],
                          ['-n', '6', '-l', 'a,b,c,d']):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit) as error:
                display.parse_args(arguments)
            self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
