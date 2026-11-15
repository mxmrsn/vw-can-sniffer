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

from vwdiag import cli, dbc, dtc, obd, vw       # noqa: E402
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


class BroadcastDatabase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = dbc.Database()

    def test_database_loads(self):
        self.assertEqual(len(self.db.messages), 113)
        self.assertEqual(self.db.by_name("ESP_21").id, 0x0FD)
        self.assertEqual(self.db.by_name("Klemmen_Status_01").size, 4)
        self.assertTrue(self.db.by_name("NMH_Gateway").extended)

    def test_scaling_and_offset(self):
        speed = self.db.by_name("ESP_21").decode(bytes([0, 0, 0, 0, 0x10, 0x27, 0, 0]))
        self.assertEqual(speed["ESP_v_Signal"], (100.0, "KiloMeterPerHour"))    # 10000 x 0.01
        temps = self.db.by_name("Motor_07").decode(bytes([0, 100, 156, 184, 0, 0, 0, 0]))
        self.assertEqual(temps["MO_Ansaugluft_Temp"][0], 27.0)
        self.assertEqual(temps["MO_Oel_Temp"][0], 96.0)
        self.assertEqual(temps["MO_Kuehlmittel_Temp"][0], 90.0)

    def test_housekeeping_hidden_and_multiplexing(self):
        self.assertNotIn("CHECKSUM", self.db.by_name("ESP_21").decode(bytes(8)))
        self.assertIn("CHECKSUM", self.db.by_name("ESP_21").decode(bytes(8), housekeeping=True))
        vin = self.db.by_name("VIN_01")
        first, second = vin.decode(bytes([0] + [0x41] * 7)), vin.decode(bytes([1] + [0x41] * 7))
        self.assertNotEqual(set(first), set(second))     # different signals per multiplexer value
        self.assertEqual(first["VIN_01_MUX"][0], 0)

    def test_big_endian_signal(self):
        sig = dbc.Signal("x", None, 7, 16, False, False, 1, 0, "")
        self.assertEqual(sig.raw(bytes([0x12, 0x34])), 0x1234)
        signed = dbc.Signal("y", None, 0, 8, True, True, 1, 0, "")
        self.assertEqual(signed.raw(bytes([0xFF])), -1)

    def test_log_line_parsing(self):
        self.assertEqual(dbc.parse_log_line("3C0 [4] 00 47 03 00"), (0x3C0, False, bytes([0, 0x47, 3, 0])))
        self.assertEqual(dbc.parse_log_line("1B000010 [2] AA BB"), (0x1B000010, True, bytes([0xAA, 0xBB])))
        self.assertIsNone(dbc.parse_log_line("monitor on"))
        self.assertIsNone(dbc.describe(self.db, 0x7FF, False, bytes(8)))


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
            UdsClient(self.link, 0x757, 0x7C1).tester_present()

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

    def test_fingerprint_matches_opendbc_format(self):
        # One request for part number, software version and coding, answered run together.
        # The expected bytes are real Golf MK7 values from opendbc's fingerprint list.
        self.link.command("silent off")
        raw = UdsClient(self.link, 0x712, 0x77C).request([0x22, 0xF1, 0x87, 0xF1, 0x89, 0xF1, 0x82])
        self.assertEqual(raw[1:], b"\xf1\x875Q0909144AB\xf1\x891082\xf1\x82\x0521A00642A1")
        engine = vw.fingerprint(self.engine())
        self.assertEqual(engine, {0xF187: b"5G0906259L ", 0xF189: b"0002"})     # no coding item: left out
        with self.assertRaises(NegativeResponse):
            self.engine().read_dids([0x1234, 0x5678])

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
        self.assertTrue(all(dbc.parse_log_line(f) for f in frames), frames)
        status = " ".join(self.link.command("status"))
        self.assertRegex(status, r"rx [1-9]\d* dropped 0 tx 0")

    def test_broadcast_frames_decode(self):
        # The simulator packs these frames by hand in C; the Python side decodes them
        # from the public database. Agreement checks both.
        db = dbc.Database()
        seen = {}
        self.link.command("mon on")
        for line in self.link.read_lines(1.3):
            frame = dbc.parse_log_line(line)
            if frame and db.lookup(frame[0], frame[1]):
                seen[db.lookup(frame[0], frame[1]).name] = db.lookup(frame[0], frame[1]).decode(frame[2])
        self.assertEqual(seen["Klemmen_Status_01"]["ZAS_Kl_15"][0], 1)
        self.assertIn(seen["ESP_21"]["ESP_v_Signal"][0], (0, 1, 2, 3))
        self.assertEqual(seen["Motor_07"]["MO_Kuehlmittel_Temp"][0], 90.0)
        self.assertEqual(seen["Motor_Code_01"]["MO_Anzahl_Zyl"][0], 4)
        self.assertEqual(seen["Motor_Code_01"]["MO_Hubraum"][0], 2.0)
        self.assertEqual(seen["Motor_Code_01"]["MO_Leistung"][0], 169)
        d = seen["Diagnose_01"]
        self.assertEqual(d["KBI_Kilometerstand"], (48213, "KiloMeter"))
        self.assertEqual([d[k][0] for k in ("UH_Jahr", "UH_Monat", "UH_Tag", "UH_Stunde", "UH_Minute", "UH_Sekunde")],
                         [2026, 11, 15, 14, 30, 5])

    def test_firmware_scan_command(self):
        self.link.command("silent off")
        lines = self.link.command("scan")
        self.assertEqual(len(lines), len(vw.ECUS))
        self.assertEqual([l.split()[0] for l in lines if "no answer" in l], ["radar", "camera"])

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
        self.assertEqual(scan.count("present"), 7)
        self.assertIn("no answer", scan)
        self.assertIn("EV_ECM20TFS0205G0906259L", self.run_cli("--transmit", "info", "engine"))
        self.assertIn("U0121-00", self.run_cli("--transmit", "dtc", "abs"))

    def test_decode_saved_log(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "cap.log")
            self.run_cli("monitor", "--seconds", "1.2", "--out", path)
            summary = self.run_cli("decode", path, "--summary")
            full = self.run_cli("decode", path)
        self.assertIn("6B2 Diagnose_01:", summary)
        self.assertIn("KBI_Kilometerstand=48213 KiloMeter", summary)
        self.assertIn("MO_Leistung=169 KiloWatt", summary)
        self.assertGreater(full.count("Klemmen_Status_01"), 5)

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
