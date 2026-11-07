import unittest

from pydantic import ValidationError

from backend.main import (
    MinerConfigUpdate,
    apply_config_update,
    is_local_api_connection,
    msr_override_enabled,
    normalize_summary,
    public_config,
)


class SummaryTests(unittest.TestCase):
    def test_normalizes_real_hashrate_and_share_counts(self):
        result = normalize_summary({
            "hashrate": {"total": [1200, 900, 850]},
            "results": {"shares_good": 12, "shares_total": 15},
            "connection": {"uptime": 61, "pool": "pool.example:3333", "connected": True},
        })

        self.assertEqual(result["hashrate"]["10s"], 1200)
        self.assertEqual(result["hashrate"]["60s"], 900)
        self.assertEqual(result["shares"], {"accepted": 12, "rejected": 3, "total": 15})
        self.assertEqual(result["uptime_seconds"], 61)

    def test_missing_telemetry_stays_unknown(self):
        result = normalize_summary({})

        self.assertEqual(result["hashrate"], {"10s": None, "60s": None, "15m": None})
        self.assertEqual(result["shares"], {"accepted": None, "rejected": None, "total": None})
        self.assertIsNone(result["uptime_seconds"])


class ConfigValidationTests(unittest.TestCase):
    def test_rejects_malformed_pool_address(self):
        with self.assertRaises(ValidationError):
            MinerConfigUpdate(pool="https://pool.example:3333/path")

    def test_accepts_host_and_port(self):
        config = MinerConfigUpdate(pool="pool.example:3333")

        self.assertEqual(config.pool, "pool.example:3333")

    def test_normalizes_pool_tls_fingerprint(self):
        config = MinerConfigUpdate(
            pool="pool.example:3333",
            tls=True,
            tls_fingerprint="AA:" * 31 + "AA",
        )

        self.assertEqual(config.tls_fingerprint, "aa" * 32)

    def test_rejects_bad_fingerprint_and_fingerprint_without_tls(self):
        with self.assertRaises(ValidationError):
            MinerConfigUpdate(tls=True, tls_fingerprint="not-a-fingerprint")
        with self.assertRaises(ValidationError):
            MinerConfigUpdate(tls_fingerprint="aa" * 32)

    def test_updates_only_the_native_xmrig_config(self):
        current = {
            "http": {"access-token": "secret-token", "port": 18088},
            "cpu": {"enabled": True},
            "pools": [],
        }
        update = MinerConfigUpdate(
            pool="pool.example:3333",
            wallet="wallet-address",
            worker="rig01",
            password="pool-pass",
            threads=2,
            tls=True,
            api_port=18089,
        )

        saved = apply_config_update(current, update)
        public = public_config(saved)

        self.assertEqual(saved["pools"][0]["url"], "pool.example:3333")
        self.assertEqual(saved["pools"][0]["pass"], "pool-pass")
        self.assertIsNone(saved["pools"][0]["tls-fingerprint"])
        self.assertEqual(saved["http"]["access-token"], "secret-token")
        self.assertEqual(saved["http"]["port"], 18089)
        self.assertTrue(saved["http"]["restricted"])
        self.assertNotIn("pass", public)
        self.assertNotIn("access-token", public)

    def test_tls_fingerprint_round_trips_in_pool_config(self):
        current = {"http": {"access-token": "secret-token", "port": 18088}, "pools": []}
        update = MinerConfigUpdate(
            pool="pool.example:3333",
            wallet="wallet-address",
            tls=True,
            tls_fingerprint="ab" * 32,
        )

        saved = apply_config_update(current, update)

        self.assertEqual(saved["pools"][0]["tls-fingerprint"], "ab" * 32)
        self.assertEqual(public_config(saved)["tls_fingerprint"], "ab" * 32)

    def test_msr_fast_enables_xmrig_preset_and_restores_original_values(self):
        saved = apply_config_update(
            {"http": {"port": 18088}, "pools": []},
            MinerConfigUpdate(msr_mode="fast"),
        )

        self.assertEqual(saved["randomx"], {"wrmsr": True, "rdmsr": True})
        self.assertEqual(public_config(saved)["msr_mode"], "fast")

    def test_msr_off_and_auto_are_distinct(self):
        base = {"http": {"port": 18088}, "pools": [], "randomx": {"wrmsr": True}}
        disabled = apply_config_update(base, MinerConfigUpdate(msr_mode="off"))
        automatic = apply_config_update(base, MinerConfigUpdate(msr_mode="auto"))

        self.assertEqual(disabled["randomx"], {"wrmsr": False, "rdmsr": True})
        self.assertEqual(automatic["randomx"], {"rdmsr": True})

    def test_msr_custom_preset_is_preserved(self):
        custom = ["0x1a4:0x6"]
        saved = apply_config_update(
            {"http": {"port": 18088}, "pools": [], "randomx": {"wrmsr": custom}},
            MinerConfigUpdate(msr_mode="custom"),
        )

        self.assertEqual(saved["randomx"]["wrmsr"], custom)
        with self.assertRaises(ValueError):
            apply_config_update(
                {"http": {"port": 18088}, "pools": []},
                MinerConfigUpdate(msr_mode="custom"),
            )


class LocalApiTests(unittest.TestCase):
    def test_rejects_forwarded_non_loopback_peer_even_with_loopback_host(self):
        self.assertFalse(is_local_api_connection("95.235.94.91", "127.0.0.1"))
        self.assertTrue(is_local_api_connection("127.0.0.1", "127.0.0.1"))

    def test_msr_override_routes_to_xmrig_for_boolean_and_custom_presets(self):
        self.assertTrue(msr_override_enabled({"wrmsr": True}))
        self.assertTrue(msr_override_enabled({"wrmsr": ["0x1a4:0xf"]}))
        self.assertTrue(msr_override_enabled({"wrmsr": 6}))
        self.assertFalse(msr_override_enabled({"wrmsr": False}))
        self.assertFalse(msr_override_enabled({"wrmsr": -1}))
        self.assertFalse(msr_override_enabled({}))

    def test_native_msr_fast_keeps_xmrig_as_explicit_backend(self):
        config = apply_config_update(
            {"http": {"port": 18088}, "pools": []},
            MinerConfigUpdate(msr_mode="fast"),
        )

        self.assertIs(config["randomx"]["wrmsr"], True)
        self.assertIs(config["randomx"]["rdmsr"], True)


if __name__ == "__main__":
    unittest.main()