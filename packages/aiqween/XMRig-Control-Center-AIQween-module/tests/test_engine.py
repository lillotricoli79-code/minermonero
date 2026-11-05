import unittest

from ai_qween.engine import AIQween, QweenConfig


class AIQweenSafetyTests(unittest.TestCase):
    def test_missing_sensor_is_not_fabricated(self):
        sample = AIQween.from_xmrig_summary({
            "hashrate": {"total": [100.0, 95.0, 90.0]},
            "results": {"shares_good": 12, "shares_total": 13},
            "connection": {"uptime": 600},
        })
        self.assertIsNone(sample.cpu_temperature_c)
        self.assertIsNone(sample.gpu_temperature_c)
        self.assertIsNone(sample.cpu_load_percent)
        self.assertEqual(sample.accepted, 12)
        self.assertEqual(sample.rejected, 1)

    def test_missing_temperature_blocks_tuning_by_default(self):
        qween = AIQween()
        sample = AIQween.from_xmrig_summary({
            "hashrate": {"total": [100.0, 100.0, 100.0]},
            "results": {"shares_good": 10, "shares_total": 10},
            "connection": {"uptime": 100},
        })
        qween.ingest(sample)
        decision = qween.decide(current_threads=4, logical_cpus=8)
        self.assertEqual(decision.mode, "OBSERVE")
        self.assertEqual(decision.safety_state, "SENSOR_MISSING")
        self.assertIsNone(decision.suggested_threads)

    def test_thermal_latch_survives_sensor_disappearance(self):
        qween = AIQween()
        hot = AIQween.from_xmrig_summary({}, cpu_temperature_c=86.0)
        qween.ingest(hot)
        first = qween.decide(current_threads=6, logical_cpus=8)
        self.assertEqual(first.safety_state, "HOT")

        missing = AIQween.from_xmrig_summary({})
        qween.ingest(missing)
        second = qween.decide(current_threads=6, logical_cpus=8)
        self.assertEqual(second.mode, "PROTECT")
        self.assertEqual(second.safety_state, "HOT")
        self.assertIn("sensore CPU non è disponibile", second.reason)

    def test_stable_samples_only_suggest_one_thread_step(self):
        qween = AIQween(QweenConfig(minimum_samples=4))
        for _ in range(4):
            qween.ingest(AIQween.from_xmrig_summary(
                {"hashrate": {"total": [1000.0, 1000.0, 1000.0]},
                 "results": {"shares_good": 20, "shares_total": 20},
                 "connection": {"uptime": 300}},
                cpu_temperature_c=55.0,
                cpu_load_percent=60.0,
            ))
        decision = qween.decide(current_threads=4, logical_cpus=8)
        self.assertEqual(decision.mode, "TEST_ONE_STEP")
        self.assertEqual(decision.suggested_threads, 5)
        self.assertEqual(decision.action, "MANUAL_REVIEW")

    def test_invalid_thread_range_is_rejected(self):
        with self.assertRaises(ValueError):
            AIQween(QweenConfig(min_threads=8, max_threads=4))


if __name__ == "__main__":
    unittest.main()
