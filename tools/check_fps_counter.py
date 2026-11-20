"""Check the UT3 counter's formatting and frame-time scale on the JVM."""

import subprocess
import tempfile
from pathlib import Path

from check_touch import JDK, ROOT


CHECK = r"""
package org.sims2.port;

public class FpsCounterCheck {
    public static void main(String[] args) {
        assert FpsCounter.POINTS == 304;
        assert FpsCounter.UPDATE_US == 500_000;
        assert FpsCounter.frameRate(30, 500_000).equals("60.0");
        assert FpsCounter.frameRate(100, 1_500_000).equals("66.6");
        assert FpsCounter.frameRate(1, 0).equals("0.0");
        assert FpsCounter.milliseconds(FpsCounter.tenths(16_666)).equals("16.6");
        assert FpsCounter.height(1_000) == 1f / 40;
        assert FpsCounter.height(100_000) == 1;
        assert FpsCounter.height(20_000) < FpsCounter.height(50_000);
        assert FpsCounter.color(16_666) == 0xff00ff00;
        assert FpsCounter.color(66_667) == 0xffff0000;
        assert FpsCounter.hudScale(2.625f) == 2.0f;
        assert FpsCounter.properties("panvk.api=1.4.363\npanvk.driverId=20\nignored")
                         .get("panvk.api").equals("1.4.363");
        assert FpsCounter.statName("77 (a (b) c) R").equals("a (b) c");
        assert FpsCounter.meminfo("MemTotal: 11821596 kB").get("MemTotal") == 11821596L * 1024;
    }
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="sims2-fps-") as directory:
        folder = Path(directory)
        check = folder / "FpsCounterCheck.java"
        check.write_text(CHECK, encoding="utf-8")
        source = ROOT / "android/app/src/org/sims2/port/FpsCounter.java"
        subprocess.run([JDK / "bin/javac.exe", "--release", "8", "-encoding", "UTF-8", "-d", folder,
                        source, check], check=True)
        subprocess.run([JDK / "bin/java.exe", "-ea", "-cp", folder, "org.sims2.port.FpsCounterCheck"], check=True)
    print("PASS UT3 FPS formatting, frame-time graph, and diagnostic parsing")


if __name__ == "__main__":
    main()
