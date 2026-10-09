"""Offline fixture guards; real model acceptance runs separately."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import wave

spec = importlib.util.spec_from_file_location("acceptance", Path(__file__).resolve().parents[1] / "scripts/researcher_acceptance.py")
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


class AudioFixtureTests(unittest.TestCase):
    def fixture(self, frames, channels=1, rate=16000, width=2):
        temporary = tempfile.NamedTemporaryFile(suffix=".wav")
        self.addCleanup(temporary.close)
        with wave.open(temporary.name, "wb") as audio:
            audio.setnchannels(channels)
            audio.setframerate(rate)
            audio.setsampwidth(width)
            audio.writeframes(bytes(frames * channels * width))
        return Path(temporary.name)

    def test_valid_pcm(self):
        self.assertEqual(acceptance.audio_fixture(self.fixture(32000))["duration_seconds"], 2)

    def test_empty_audio(self):
        with self.assertRaises(ValueError):
            acceptance.audio_fixture(self.fixture(0))

    def test_wrong_format(self):
        for options in ({"channels": 2}, {"rate": 24000}, {"width": 1}):
            with self.subTest(options=options), self.assertRaises(ValueError):
                acceptance.audio_fixture(self.fixture(32000, **options))


if __name__ == "__main__":
    unittest.main()
