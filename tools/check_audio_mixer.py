"""Run actual-C audio contracts (shared with the streaming regressions)."""
import unittest
from check_audio_streaming import SoundStreamingTest

if __name__ == "__main__":
    names = [name for name in unittest.defaultTestLoader.getTestCaseNames(SoundStreamingTest)
             if name.startswith("test_directsound")]
    result = unittest.TextTestRunner().run(unittest.TestSuite(SoundStreamingTest(name) for name in names))
    raise SystemExit(0 if result.wasSuccessful() else 1)
