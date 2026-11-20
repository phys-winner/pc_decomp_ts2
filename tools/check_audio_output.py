"""Run the actual-C contracts of the native AAudio output (audio.c and the activity's pause state)."""
import unittest
from check_audio_streaming import SoundStreamingTest

if __name__ == "__main__":
    names = [name for name in unittest.defaultTestLoader.getTestCaseNames(SoundStreamingTest)
             if name.startswith(("test_aaudio", "test_pause", "test_audio"))]
    assert names, "no audio output tests found"
    result = unittest.TextTestRunner().run(unittest.TestSuite(SoundStreamingTest(name) for name in names))
    raise SystemExit(0 if result.wasSuccessful() else 1)
