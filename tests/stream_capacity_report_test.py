"""Loss in a converged report must fail the modest-rate integration fixture."""
import unittest

from stream_capacity_e2e import check_fixture_report


class FixtureLossTests(unittest.TestCase):
    def report(self, clients=1):
        return dict(samples=64, clients=clients, received=64 * clients, missed_updates=0,
                    duplicate_updates=0, final_values_converged=True,
                    latency_p50_ms=1, latency_p99_ms=2)

    def test_exact_scalar_and_fanout_reports_pass(self):
        for clients in (1, 16):
            check_fixture_report(self.report(clients), 64, clients)

    def test_converged_report_with_one_or_many_missed_updates_fails(self):
        for clients, missed in ((1, 1), (1, 63), (16, 1), (16, 1000)):
            with self.subTest(clients=clients, missed=missed):
                report = dict(self.report(clients), received=64 * clients - missed, missed_updates=missed)
                # Every mutant passes the old received + missed == produced identity.
                self.assertEqual(report["received"] + report["missed_updates"], 64 * clients)
                with self.assertRaisesRegex(ValueError, "without loss"):
                    check_fixture_report(report, 64, clients)

    def test_workload_mismatch_and_duplicates_fail(self):
        with self.assertRaises(ValueError):
            check_fixture_report(self.report(1), 64, 16)
        with self.assertRaises(ValueError):
            check_fixture_report(dict(self.report(), duplicate_updates=1), 64, 1)


if __name__ == "__main__":
    unittest.main()
