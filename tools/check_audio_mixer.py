"""Run the actual-C contracts of the game's mixer driver (EA SND without DirectSound, sims2_audio.c)."""
import unittest
from check_audio_streaming import SoundStreamingTest

if __name__ == "__main__":
    names = [name for name in unittest.defaultTestLoader.getTestCaseNames(SoundStreamingTest)
             if name.startswith("test_snd")]
    assert names, "no SND driver tests found"
    result = unittest.TextTestRunner().run(unittest.TestSuite(SoundStreamingTest(name) for name in names))
    raise SystemExit(0 if result.wasSuccessful() else 1)
