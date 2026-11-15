"""Tests for the vwdiag tools. Decoders are tested directly; everything else runs
end to end against the host simulator (the real firmware core plus simulated ECUs).

    python3 -m unittest discover -s tools/tests -v
"""
import io
import os
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

from vwdiag import cli, dtc, obd, vw            # noqa: E402
from vwdiag.link import Link                    # noqa: E402
from vwdiag.uds import InterfaceRefused, NegativeResponse, NoResponse, UdsClient  # noqa: E402

SIM_DIR = os.path.join(REPO, "firmware", "pico2w", "sim")
SIM = os.path.join(SIM_DIR, "vw_diag_sim")


def setUpModule():
    subprocess.run(["make", "-C", SIM_DIR], check=True, capture_output=True)


class Decoders(unittest.TestCase):
    def test_sae_codes(self):
        self.assertEqual(dtc.sae_code(bytes([0x02, 0x99, 0x00])), "P0299")
        self.assertEqual(dtc.sae_code(bytes([0x13, 0x0A, 0x00])), "P130A")
        self.assertEqual(dtc.sae_code(bytes([0x41, 0x23, 0x00])), "C0123")
        self.assertEqual(dtc.sae_code(bytes([0x9A, 0xBC, 0x00])), "B1ABC")
        self.assertEqual(dtc.sae_code(bytes([0xC1, 0x21, 0x00])), "U0121")

    def test_status_bits(self):
        self.assertEqual(dtc.describe_status(0x09), ["test failed", "confirmed"])
        self.assertEqual(dtc.describe_status(0x00), [])
        self.assertEqual(len(dtc.describe_status(0xFF)), 8)

    def test_obd_formulas(self):
        d = {name: spec[3] for name, spec in obd.PIDS.items()}
        self.assertEqual(d["rpm"]([0x0C, 0x88]), 802.0)
        self.assertEqual(d["coolant"]([130]), 90)
        self.assertEqual(d["voltage"]([0x37, 0x14]), 14.1)
        self.assertEqual(d["maf"]([0x01, 0x18]), 2.8)
        self.assertAlmostEqual(d["throttle"]([255]), 100.0)
        self.assertEqual(d["timing"]([128]), 0.0)
        self.assertEqual(d["oil"]([0]), -40)

    def test_ecu_table_matches_firmware(self):
        with open(os.path.join(REPO, "firmware", "pico2w", "src", "vw_ecus.h")) as f:
            header = f.read()
        rows = re.findall(r'\{\s*"(\w+)",\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),', header)
        firmware = {name: (int(a, 16), int(b, 16)) for name, a, b in rows}
        self.assertEqual(firmware, {name: (v[0], v[1]) for name, v in vw.ECUS.items()})


class AgainstSimulator(unittest.TestCase):
    def setUp(self):
        self.link = Link.simulator(SIM)
        self.addCleanup(self.link.close)

    def engine(self):
        return UdsClient(self.link, 0x7E0, 0x7E8)

    def test_listen_only_by_default(self):
        with self.assertRaises(InterfaceRefused):
            self.engine().tester_present()
        self.assertIn("bus listen-only", self.link.command("status"))

    def test_tester_present_and_session(self):
        self.link.command("silent off")
        self.engine().tester_present()
        self.engine().session(0x03)
        with self.assertRaises(NegativeResponse) as cm:
            self.engine().session(0x7E)
        self.assertEqual(cm.exception.nrc, 0x12)

    def test_multi_frame_read(self):
        self.link.command("silent off")
        self.assertEqual(self.engine().read_did(0xF190), b"WVWZZZAUZJW123456")
        self.assertEqual(vw.text(self.engine().read_did(0xF19E)), "EV_ECM20TFS0205G0906259L")

    def test_negative_and_missing(self):
        self.link.command("silent off")
        with self.assertRaises(NegativeResponse) as cm:
            self.engine().read_did(0x1234)
        self.assertEqual(cm.exception.nrc, 0x31)
        self.link.command("timeout 200")
        with self.assertRaises(NoResponse):
            UdsClient(self.link, 0x715, 0x77F).tester_present()

    def test_response_pending(self):
        self.link.command("silent off")
        self.link.command("timeout 100")   # shorter than the simulated delay: only "pending" keeps it alive
        self.assertEqual(vw.text(self.engine().read_did(0xF17C)), "TST-01.01.2018")

    def test_identify(self):
        self.link.command("silent off")
        info = dict(vw.identify(self.engine()))
        self.assertEqual(info["VIN"], "WVWZZZAUZJW123456")
        self.assertEqual(info["Part number"], "5G0906259L")
        self.assertEqual(len(info), 9)
        cluster = dict(vw.identify(UdsClient(self.link, 0x714, 0x77E)))
        self.assertEqual(sorted(cluster), ["Part number", "Software version", "System name"])

    def test_fault_codes_read_filter_clear(self):
        self.link.command("silent off")
        _, codes = self.engine().read_dtcs()
        self.assertEqual([dtc.sae_code(c) for c, _ in codes], ["P0299", "P130A"])
        _, only_failed = self.engine().read_dtcs(0x01)
        self.assertEqual([dtc.sae_code(c) for c, _ in only_failed], ["P0299"])
        self.engine().clear_dtcs()
        self.assertEqual(self.engine().read_dtcs()[1], [])

    def test_obd_live_data(self):
        self.link.command("silent off")
        pids = obd.supported_pids(self.engine())
        self.assertEqual(pids, [0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1F, 0x2F, 0x42, 0x46, 0x5C])
        rpm, unit = obd.read_pid(self.engine(), "rpm")
        self.assertEqual(unit, "rpm")
        self.assertTrue(780 <= rpm < 840)
        self.assertEqual(obd.read_pid(self.engine(), "coolant")[0], 90)
        self.assertEqual(obd.read_vin(self.engine()), "WVWZZZAUZJW123456")
        with self.assertRaises(NegativeResponse):
            obd.read_pid(self.engine(), "baro")   # simulated engine does not implement it

    def test_monitor_and_counters(self):
        self.link.command("mon on")
        frames = self.link.read_lines(0.35)
        self.link.command("mon off")
        self.assertTrue(frames)
        self.assertTrue(all(re.match(r"^3C0 \[4\]( [0-9A-F]{2}){4}$", f) for f in frames), frames)
        status = " ".join(self.link.command("status"))
        self.assertRegex(status, r"rx [1-9]\d* dropped 0 tx 0")

    def test_firmware_scan_command(self):
        self.link.command("silent off")
        lines = self.link.command("scan")
        self.assertEqual(len(lines), len(vw.ECUS))
        self.assertEqual([l.split()[0] for l in lines if "no answer" in l], ["airbag"])

    def test_keepalive_sends_without_reply(self):
        self.link.command("silent off")
        self.link.command("keepalive on")
        before = int(re.search(r"tx (\d+)", " ".join(self.link.command("status"))).group(1))
        self.link.read_lines(2.3)
        after = int(re.search(r"tx (\d+)", " ".join(self.link.command("status"))).group(1))
        self.assertGreaterEqual(after - before, 1)
        self.engine().tester_present()   # the link still works afterwards


class CommandLine(unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        with redirect_stdout(out):
            cli.main(["--sim"] + list(argv))
        return out.getvalue()

    def test_transmit_guard(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_cli("scan")
        self.assertIn("--transmit", str(cm.exception))

    def test_clear_needs_confirmation(self):
        with self.assertRaises(SystemExit) as cm:
            self.run_cli("--transmit", "clear", "engine")
        self.assertIn("--yes", str(cm.exception))

    def test_scan_info_dtc(self):
        scan = self.run_cli("--transmit", "scan")
        self.assertEqual(scan.count("present"), 6)
        self.assertIn("no answer", scan)
        self.assertIn("EV_ECM20TFS0205G0906259L", self.run_cli("--transmit", "info", "engine"))
        self.assertIn("U0121-00", self.run_cli("--transmit", "dtc", "abs"))

    def test_live_csv_log(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "log.csv")
            self.run_cli("--transmit", "live", "rpm", "coolant", "baro", "--count", "3", "--interval", "0.05", "--csv", path)
            with open(path) as f:
                rows = f.read().strip().split("\n")
        self.assertEqual(rows[0], "time_s,rpm_rpm,coolant_C,baro_kPa")
        self.assertEqual(len(rows), 4)
        self.assertTrue(rows[1].endswith(",90,"))   # unsupported value logs as an empty cell


if __name__ == "__main__":
    unittest.main()
