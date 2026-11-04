import importlib.util
from pathlib import Path
import tempfile
import unittest


def load_analyzer():
    path = Path(__file__).resolve().parents[1] / "tools/analyze_vertex_preparation.py"
    spec = importlib.util.spec_from_file_location("vertex_analysis", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.analyze_capture


class VertexAnalysisTests(unittest.TestCase):
    def fixture(self, root, *, sampling=0, mismatch=False):
        path = Path(root) / "capture.csv"
        lines = ["# mode=0 duration_ns=1000000000 boundary_elapsed_ns=1001000000 initial_real=0 initial_generated=0 initial_draws=0 initial_rejected=0",
                 "elapsed_ns,interval_ns,real_frames,generated_frames,world_draws,rejected,timers,sampling"]
        elapsed = 0
        for index, interval in enumerate([10000000, 16666666, 16666667, 20000000, 30000000, 50000000, 5000000, 12000000, 40000000, 60000000], 1):
            elapsed += interval
            lines.append(f"{elapsed},{interval + int(mismatch)},{index},0,{index * 13},0,0,{sampling}")
        path.write_text(chr(10).join(lines) + chr(10))
        return path

    def test_percentiles_and_exact_budget_boundary(self):
        with tempfile.TemporaryDirectory() as root:
            result = load_analyzer()(self.fixture(root))
        self.assertEqual(result["frames"], 10)
        self.assertAlmostEqual(result["median_ms"], 18.3333335)
        self.assertEqual((result["p95_ms"], result["p99_ms"]), (60, 60))
        self.assertEqual(result["frames_over_60fps_budget"], 6)
        self.assertEqual(result["frames_over_33_333ms"], 3)
        self.assertEqual(result["world_draws_per_frame"], 13)

    def test_sampling_is_not_a_clean_pacing_measurement(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(ValueError, "CPU sampling"):
                load_analyzer()(self.fixture(root, sampling=1))

    def test_inconsistent_intervals_are_not_summarized(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(ValueError, "inconsistent raw"):
                load_analyzer()(self.fixture(root, mismatch=True))


if __name__ == "__main__":
    unittest.main()
