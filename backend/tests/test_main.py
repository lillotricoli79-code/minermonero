import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

from pydantic import ValidationError

from backend.src.shell_log import ShellLog

from backend.src.main import (
    MinerConfigUpdate,
    MinerController,
    apply_config_update,
    is_local_api_connection,
    msr_override_enabled,
    normalize_summary,
    public_config,
)


class ShellLogTests(unittest.TestCase):
    def test_writes_utf8_shell_output_to_persistent_file(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "logs" / "shell.log"
            log = ShellLog(path)
            try:
                log.write("Avvio miner — RandomX")
            finally:
                log.close()

            content = path.read_text(encoding="utf-8")
            self.assertIn("Avvio miner — RandomX", content)
            self.assertIn("INFO", content)

    def test_rotates_logs_at_configured_size(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "logs" / "shell.log"
            log = ShellLog(path, max_bytes=40, backup_count=2)
            try:
                for index in range(5):
                    log.write(f"shell-output-line-{index}-xxxxxxxx")
            finally:
                log.close()

            self.assertTrue(path.is_file())
            self.assertTrue(Path(f"{path}.1").is_file())
            history = "\n".join(log.recent_lines(20))
            self.assertIn("shell-output-line-4", history)
            self.assertNotIn("shell-output-line-0", history)


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

    def test_dashboard_update_preserves_backup_pools_and_edits_first_enabled(self):
        current = {
            "http": {"port": 18088},
            "cpu": {"enabled": True},
            "pools": [
                {"url": "disabled.example:3333", "user": "disabled-wallet", "enabled": False},
                {"url": "primary.example:4444", "user": "primary-wallet", "enabled": True},
                {"url": "backup.example:5555", "user": "backup-wallet", "enabled": True},
            ],
        }
        saved = apply_config_update(
            current,
            MinerConfigUpdate(pool="new-primary.example:6666", wallet="new-wallet"),
        )

        self.assertEqual(len(saved["pools"]), 3)
        self.assertEqual(saved["pools"][0]["url"], "disabled.example:3333")
        self.assertEqual(saved["pools"][1]["url"], "new-primary.example:6666")
        self.assertEqual(saved["pools"][1]["user"], "new-wallet")
        self.assertEqual(saved["pools"][2]["url"], "backup.example:5555")
        self.assertEqual(public_config(saved)["pool"], "new-primary.example:6666")

    def test_public_config_uses_first_enabled_pool(self):
        public = public_config({
            "http": {"port": 18088},
            "pools": [
                {"url": "disabled.example:3333", "user": "disabled-wallet", "enabled": False},
                {"url": "backup.example:4444", "user": "backup-wallet", "enabled": True},
            ],
        })

        self.assertEqual(public["pool"], "backup.example:4444")
        self.assertEqual(public["wallet"], "backup-wallet")


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



class MinerShutdownTests(unittest.TestCase):
    def test_windows_stop_requests_graceful_ctrl_break(self):
        class FakeProcess:
            def __init__(self):
                self.signals = []
                self.terminated = False
                self.killed = False

            def poll(self):
                return None

            def send_signal(self, value):
                self.signals.append(value)

            def terminate(self):
                self.terminated = True

            def wait(self, timeout):
                return 0

            def kill(self):
                self.killed = True

        controller = MinerController()
        process = FakeProcess()
        controller.process = process
        with patch("backend.src.main.os.name", "nt"), patch(
            "backend.src.main.signal.CTRL_BREAK_EVENT", 123, create=True
        ):
            controller.stop()

        self.assertEqual(process.signals, [123])
        self.assertFalse(process.terminated)
        self.assertFalse(process.killed)
        self.assertIn("Arresto miner completato", controller.recent_logs()[-1]["message"])

class LocalApiTests(unittest.TestCase):
    def test_rejects_forwarded_non_loopback_peer_even_with_loopback_host(self):
        self.assertFalse(is_local_api_connection("95.235.94.91", "127.0.0.1"))
        self.assertTrue(is_local_api_connection("127.0.0.1", "127.0.0.1"))

    def test_native_automatic_and_fast_msr_use_cpu_specific_profiles(self):
        self.assertFalse(msr_override_enabled({}))
        self.assertFalse(msr_override_enabled({"wrmsr": None}))
        self.assertFalse(msr_override_enabled({"wrmsr": True}))
        self.assertFalse(msr_override_enabled({"wrmsr": False}))
        self.assertFalse(msr_override_enabled({"wrmsr": -1}))

    def test_custom_msr_payload_uses_compatible_external_backend(self):
        self.assertTrue(msr_override_enabled({"wrmsr": ["0x1a4:0xf"]}))
        self.assertTrue(msr_override_enabled({"wrmsr": []}))
        self.assertTrue(msr_override_enabled({"wrmsr": 0}))
        self.assertTrue(msr_override_enabled({"wrmsr": 6}))

    def test_fast_msr_config_uses_native_automatic_profile(self):
        config = apply_config_update(
            {"http": {"port": 18088}, "pools": []},
            MinerConfigUpdate(msr_mode="fast"),
        )

        self.assertIs(config["randomx"]["wrmsr"], True)
        self.assertIs(config["randomx"]["rdmsr"], True)
        self.assertFalse(msr_override_enabled(config["randomx"]))


if __name__ == "__main__":
    unittest.main()