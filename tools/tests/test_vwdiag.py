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
import time
import unittest
from contextlib import redirect_stdout

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

from vwdiag import cli, dbc, dtc, logfile, obd, simos18, vw   # noqa: E402
from vwdiag import binlink                      # noqa: E402
from vwdiag.binlink import BinaryLink, BleSimLink   # noqa: E402
from vwdiag.link import Link, LinkError         # noqa: E402
from vwdiag.uds import InterfaceRefused, NegativeResponse, NoResponse, UdsClient, UdsError  # noqa: E402

SIM_DIR = os.path.join(REPO, "hardware", "pico2w", "firmware", "sim")
SIM = os.path.join(SIM_DIR, "vw_diag_sim")


# Most suites exercise the interface itself (monitoring, logging, the link protocols), which needs bus
# traffic to work on, so they run the realistic simulator with the gateway open. RealCar below runs
# the default, where the gateway hides the broadcast traffic as it probably does on the car.
TEST_SIM = "realistic,gateway=open"


def setUpModule():
    subprocess.run(["make", "-C", SIM_DIR], check=True, capture_output=True)
    os.environ["VWSIM"] = TEST_SIM


def tearDownModule():
    os.environ.pop("VWSIM", None)


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
        with open(os.path.join(REPO, "hardware", "pico2w", "firmware", "src", "vw_ecus.h")) as f:
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
    LINK = Link          # the same tests run again over the binary protocol below
    READ_CHUNK = 4096

    def setUp(self):
        self.link = self.LINK.simulator(SIM, read_chunk=self.READ_CHUNK)
        self.addCleanup(self.link.close)

    def engine(self):
        return UdsClient(self.link, 0x7E0, 0x7E8)

    def test_listen_only_by_default(self):
        with self.assertRaises(InterfaceRefused):
            self.engine().tester_present()
        self.assertTrue(self.link.status()["silent"])

    def test_tester_present_and_session(self):
        self.link.set_silent(False)
        self.engine().tester_present()
        self.engine().session(0x03)
        with self.assertRaises(NegativeResponse) as cm:
            self.engine().session(0x7E)
        self.assertEqual(cm.exception.nrc, 0x12)

    def test_multi_frame_read(self):
        self.link.set_silent(False)
        self.assertEqual(self.engine().read_did(0xF190), b"WVWZZZAUZJW123456")
        self.assertEqual(vw.text(self.engine().read_did(0xF19E)), "EV_ECM20TFS0205G0906259L")

    def test_negative_and_missing(self):
        self.link.set_silent(False)
        with self.assertRaises(NegativeResponse) as cm:
            self.engine().read_did(0x1234)
        self.assertEqual(cm.exception.nrc, 0x31)
        with self.assertRaises(NoResponse):
            UdsClient(self.link, 0x757, 0x7C1, timeout_ms=200).tester_present()

    def test_response_pending(self):
        self.link.set_silent(False)
        # A timeout shorter than the simulated delay: only "pending" keeps the request alive.
        engine = UdsClient(self.link, 0x7E0, 0x7E8, timeout_ms=100)
        self.assertEqual(vw.text(engine.read_did(0xF17C)), "TST-01.01.2018")

    def test_identify(self):
        self.link.set_silent(False)
        info = dict(vw.identify(self.engine()))
        self.assertEqual(info["VIN"], "WVWZZZAUZJW123456")
        self.assertEqual(info["Part number"], "5G0906259L")
        self.assertEqual(len(info), 10)
        self.assertEqual(info["Engine code"], "DKFA")
        cluster = dict(vw.identify(UdsClient(self.link, 0x714, 0x77E)))
        self.assertEqual(sorted(cluster), ["Part number", "Software version", "System name"])

    def test_fingerprint_matches_opendbc_format(self):
        # One request for part number, software version and coding, answered run together.
        # The expected bytes are real Golf MK7 values from opendbc's fingerprint list.
        self.link.set_silent(False)
        raw = UdsClient(self.link, 0x712, 0x77C).request([0x22, 0xF1, 0x87, 0xF1, 0x89, 0xF1, 0x82])
        self.assertEqual(raw[1:], b"\xf1\x875Q0909144AB\xf1\x891082\xf1\x82\x0521A00642A1")
        engine = vw.fingerprint(self.engine())
        self.assertEqual(engine, {0xF187: b"5G0906259L ", 0xF189: b"0002"})     # no coding item: left out
        with self.assertRaises(NegativeResponse):
            self.engine().read_dids([0x1234, 0x5678])

    def test_fault_codes_read_filter_clear(self):
        self.link.set_silent(False)
        _, codes = self.engine().read_dtcs()
        self.assertEqual([dtc.sae_code(c) for c, _ in codes], ["P0299", "P130A"])
        _, only_failed = self.engine().read_dtcs(0x01)
        self.assertEqual([dtc.sae_code(c) for c, _ in only_failed], ["P0299"])
        self.engine().clear_dtcs()
        self.assertEqual(self.engine().read_dtcs()[1], [])

    def test_obd_live_data(self):
        self.link.set_silent(False)
        pids = obd.supported_pids(self.engine())
        self.assertEqual(pids, [0x01, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x1C, 0x1F,
                                0x21, 0x2F, 0x33, 0x34, 0x42, 0x44, 0x46, 0x5C])
        rpm, unit = obd.read_pid(self.engine(), "rpm")
        self.assertEqual(unit, "rpm")
        self.assertTrue(680 <= rpm < 740, rpm)   # warm idle, with a little unevenness
        self.assertEqual(obd.read_pid(self.engine(), "coolant")[0], 90)
        self.assertEqual(obd.read_vin(self.engine()), "WVWZZZAUZJW123456")
        with self.assertRaises(NegativeResponse):
            obd.read_pid(self.engine(), "rail")   # simulated engine does not implement it

    def test_monitor_and_counters(self):
        self.link.set_monitor(True)
        frames = self.link.read_frames(0.35)
        self.link.set_monitor(False)
        self.assertTrue(frames)
        self.assertTrue(all(dbc.parse_log_line(f) for f in frames), frames)
        status = self.link.status()
        self.assertGreater(status["rx"], 0)
        self.assertEqual((status["dropped"], status["tx"], status["monitor"]), (0, 0, False))

    def test_broadcast_frames_decode(self):
        # The simulator packs these frames by hand in C; the Python side decodes them
        # from the public database. Agreement checks both.
        db = dbc.Database()
        seen = {}
        self.link.set_monitor(True)
        for line in self.link.read_frames(1.3):
            frame = dbc.parse_log_line(line)
            if frame and db.lookup(frame[0], frame[1]):
                seen[db.lookup(frame[0], frame[1]).name] = db.lookup(frame[0], frame[1]).decode(frame[2])
        self.assertEqual(seen["Klemmen_Status_01"]["ZAS_Kl_15"][0], 1)
        self.assertIn(seen["ESP_21"]["ESP_v_Signal"][0], (0, 1, 2, 3))
        self.assertEqual(seen["Motor_07"]["MO_Kuehlmittel_Temp"][0], 90.0)
        self.assertEqual(seen["Motor_Code_01"]["MO_Anzahl_Zyl"][0], 4)
        self.assertEqual(seen["Motor_Code_01"]["MO_Hubraum"][0], 2.0)
        self.assertEqual(seen["Motor_Code_01"]["MO_Leistung"][0], 170)
        d = seen["Diagnose_01"]
        self.assertEqual(d["KBI_Kilometerstand"], (48213, "KiloMeter"))
        self.assertEqual([d[k][0] for k in ("UH_Jahr", "UH_Monat", "UH_Tag", "UH_Stunde", "UH_Minute", "UH_Sekunde")],
                         [2026, 11, 15, 14, 30, 5])

    def test_firmware_scan_command(self):
        if self.LINK is not Link:
            self.skipTest("text console command")
        self.link.set_silent(False)
        lines = self.link.command("scan")
        self.assertEqual(len(lines), len(vw.ECUS))
        self.assertEqual([l.split()[0] for l in lines if "no answer" in l], ["radar", "camera"])

    def test_keepalive_sends_without_reply(self):
        self.link.set_silent(False)
        self.link.set_keepalive(True)
        before = self.link.status()["tx"]
        self.link.read_frames(2.3)
        self.assertGreaterEqual(self.link.status()["tx"] - before, 1)
        self.engine().tester_present()   # the link still works afterwards


class Logging(unittest.TestCase):
    """Logging to the card. In the simulator the card is a folder named by VWDIAG_SD."""
    LINK = Link

    def setUp(self):
        self.card = tempfile.mkdtemp()
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", self.card]))
        os.environ["VWDIAG_SD"] = self.card
        self.addCleanup(os.environ.pop, "VWDIAG_SD", None)

    def open(self):
        link = self.LINK.simulator(SIM)
        self.addCleanup(link.close)
        return link

    def test_no_card(self):
        os.environ["VWDIAG_SD"] = ""
        link = self.open()
        with self.assertRaises(LinkError):
            link.log_start()
        self.assertFalse(link.log_status()["active"])

    def test_records_traffic_marks_and_time(self):
        link = self.open()
        link.set_time(1794787200)
        state = link.log_start()
        self.assertEqual((state["active"], state["name"]), (True, "LOG00001.VWL"))
        link.log_mark("engine cold")
        link.set_silent(False)
        self.assertEqual(UdsClient(link, 0x7E0, 0x7E8).read_did(0xF190), b"WVWZZZAUZJW123456")
        link.read_frames(1.2)
        state = link.log_stop()
        self.assertFalse(state["active"])
        self.assertGreater(state["frames"], 10)

        log = logfile.Log(os.path.join(self.card, "LOG00001.VWL"))
        self.assertEqual(len(log.frames), state["frames"])
        self.assertEqual(os.path.getsize(os.path.join(self.card, "LOG00001.VWL")), state["bytes"])
        self.assertEqual((log.bitrate, log.lost, log.truncated), (500000, 0, False))
        self.assertEqual(log.marks[0][1], "engine cold")
        self.assertAlmostEqual(log.epoch(log.start_ms), 1794787200, delta=2)
        sent = [f.text() for f in log.frames if f.sent]
        received = [f.text() for f in log.frames if not f.sent]
        self.assertEqual(sent[0], "7E0 [8] 03 22 F1 90 55 55 55 55")         # our request...
        self.assertTrue(any(t.startswith("7E8 [8] 10 14 62 F1 90") for t in received))  # ...its multi-frame answer
        self.assertTrue(any(t.startswith("7E0 [8] 30") for t in sent))      # and our flow control
        self.assertTrue(any(t.startswith("3C0 ") for t in received))        # broadcast traffic too
        ms = [f.ms for f in log.frames]
        self.assertEqual(ms, sorted(ms))

    def test_poll_list_puts_live_data_in_the_log(self):
        link = self.open()
        link.set_silent(False)
        for pid in (0x0C, 0x05):
            link.poll_add(0x7E0, 0x7E8, 100, [0x01, pid])
        link.log_start()
        link.read_frames(1.1)
        link.log_stop()
        link.poll_clear()
        samples = list(logfile.Log(os.path.join(self.card, "LOG00001.VWL")).obd_samples())
        rpm = [v for _, n, v, _ in samples if n == "rpm"]
        coolant = [v for _, n, v, _ in samples if n == "coolant"]
        self.assertGreaterEqual(len(rpm), 6)
        self.assertGreaterEqual(len(coolant), 6)
        self.assertEqual(set(coolant), {90})
        with self.assertRaises(LinkError):
            link.poll_add(0x7E0, 0x7E8, 5, [0x01, 0x0C])     # faster than the interface allows

    def test_files_are_numbered_and_never_overwritten(self):
        link = self.open()
        for _ in range(3):
            link.log_start()
            link.log_stop()
        self.assertEqual(sorted(os.listdir(self.card)), ["LOG00001.VWL", "LOG00002.VWL", "LOG00003.VWL"])
        link.close()
        link = self.open()   # after a restart numbering carries on
        self.assertEqual(link.log_start()["name"], "LOG00004.VWL")

    def test_autorun_logs_without_a_computer(self):
        with open(os.path.join(self.card, "AUTORUN.TXT"), "w") as f:
            f.write("# start logging at power-up\r\nsilent off\r\npoll 7E0 7E8 100 01 0D\r\nlog on\r\n")
        link = self.open()
        time.sleep(1.0)
        state = link.log_status()
        self.assertTrue(state["active"])
        self.assertGreater(state["frames"], 10)
        link.log_stop()
        log = logfile.Log(os.path.join(self.card, "LOG00001.VWL"))
        self.assertTrue(any(n == "speed" for _, n, _, _ in log.obd_samples()))

    def test_power_cut_mid_record(self):
        link = self.open()
        link.log_start()
        link.read_frames(0.6)
        state = link.log_stop()
        path = os.path.join(self.card, "LOG00001.VWL")
        with open(path, "rb") as f:
            data = f.read()
        with open(path, "wb") as f:
            f.write(data[:-3])
        log = logfile.Log(path)
        self.assertTrue(log.truncated)
        self.assertEqual(len(log.frames), state["frames"] - 1)

    def test_logread_command(self):
        link = self.open()
        link.set_silent(False)
        link.poll_add(0x7E0, 0x7E8, 100, [0x01, 0x0C])
        link.log_start()
        link.read_frames(0.8)
        link.log_stop()
        path = os.path.join(self.card, "LOG00001.VWL")
        dump, table = os.path.join(self.card, "out.log"), os.path.join(self.card, "out.csv")
        out = io.StringIO()
        with redirect_stdout(out):
            cli.main(["logread", path, "--candump", dump, "--csv", table, "--decode"])
        self.assertIn("Klemmen_Status_01", out.getvalue())
        with open(dump) as f:
            first = f.readline()
        self.assertRegex(first, r"^\(\d+\.\d{6}\) can0 [0-9A-F]{3}#[0-9A-F]*$")
        with open(table) as f:
            rows = f.read().strip().split("\n")
        self.assertEqual(rows[0], "time_s,rpm_rpm")
        self.assertGreater(len(rows), 4)


    def test_copy_logs_off_the_card(self):
        link = self.open()
        link.log_start()
        with open(os.path.join(self.card, "AUTORUN.TXT"), "w") as f:
            f.write("poll 7E0 7E8 500 01 0C\n")
        with self.assertRaises(LinkError) as cm:            # still being recorded
            link.log_read("LOG00001.VWL", 0, 100)
        self.assertIn("recorded", str(cm.exception))
        link.read_frames(1.0)
        link.log_stop()
        link.log_start()                                     # a second file, longer than one read
        link.read_frames(2.5)
        link.log_stop()
        files = dict(link.log_files())
        self.assertEqual(sorted(files), ["AUTORUN.TXT", "LOG00001.VWL", "LOG00002.VWL"])
        for name in ("LOG00001.VWL", "LOG00002.VWL", "AUTORUN.TXT"):
            with open(os.path.join(self.card, name), "rb") as f:
                on_card = f.read()
            self.assertEqual(files[name], len(on_card))
            self.assertEqual(link.fetch(name), on_card, name)
        big = bytes((i * 7919) % 251 for i in range(9001))   # several reads on either link
        with open(os.path.join(self.card, "BIG.BIN"), "wb") as f:
            f.write(big)
        self.assertEqual(link.fetch("BIG.BIN"), big)
        size, tail = link.log_read("LOG00002.VWL", files["LOG00002.VWL"] - 5, 100)
        self.assertEqual((size, len(tail)), (files["LOG00002.VWL"], 5))
        self.assertEqual(link.log_read("LOG00002.VWL", files["LOG00002.VWL"] + 10, 100)[1], b"")
        for bad in ("../x", "A/B.TXT", "NOSUCH.VWL"):
            with self.assertRaises(LinkError):
                link.log_read(bad, 0, 10)

    def test_files_command(self):
        link = self.open()
        link.log_start()
        link.read_frames(0.5)
        link.log_stop()
        link.close()
        out = tempfile.mkdtemp()
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", out]))
        buf = io.StringIO()
        mode = ["--sim", "--binary"] if self.LINK is BinaryLink else ["--sim"]
        with redirect_stdout(buf):
            cli.main(mode + ["files"])
            cli.main(mode + ["files", "all", "--out", out])
            cli.main(mode + ["files", "all", "--out", out])
        self.assertIn("LOG00001.VWL", buf.getvalue())
        self.assertIn("already here", buf.getvalue())
        with open(os.path.join(out, "LOG00001.VWL"), "rb") as a, open(os.path.join(self.card, "LOG00001.VWL"), "rb") as b:
            self.assertEqual(a.read(), b.read())


class LoggingBinary(Logging):
    LINK = BinaryLink

    def test_listing_pages(self):
        # more files than fit one listing packet: the tools ask for the rest
        for i in range(120):
            with open(os.path.join(self.card, "F%05d.TXT" % i), "w") as f:
                f.write("x" * i)
        link = self.open()
        files = dict(link.log_files())
        self.assertEqual(len(files), 120)
        self.assertEqual(files["F00099.TXT"], 99)


class BinaryAgainstSimulator(AgainstSimulator):
    """Everything above again, over the packet protocol an app will use."""
    LINK = BinaryLink

    def test_hello(self):
        self.assertEqual(self.link.info["version"], 1)
        self.assertEqual(self.link.info["max_payload"], binlink.MAX_PAYLOAD)
        self.assertTrue(self.link.info["name"].startswith("vw-diag"))

    def test_per_request_timeout(self):
        self.link.set_silent(False)
        with self.assertRaises(NoResponse):
            UdsClient(self.link, 0x757, 0x7C1, timeout_ms=50).tester_present()   # no radar fitted
        self.engine().tester_present()

    def test_bad_requests_are_answered(self):
        self.assertEqual(self.link._exchange(0x55)[0], binlink.UNKNOWN_CMD)
        self.assertEqual(self.link._exchange(binlink.CMD_SET_SILENT, [7])[0], binlink.BAD_ARGS)
        self.assertEqual(self.link._exchange(binlink.CMD_UDS, [1, 2, 3])[0], binlink.BAD_ARGS)

    def test_damaged_packets_are_dropped_and_counted(self):
        packet = bytearray(binlink.encode([9, binlink.CMD_STATUS]))
        packet[2] ^= 0x10                                  # a flipped bit
        self.link._write_bytes(bytes(packet))
        self.assertEqual(self.link.status()["bad_packets"], 1)
        self.link._write_bytes(b"\x33\x44\x00")            # a truncated packet
        self.assertEqual(self.link.status()["bad_packets"], 2)

    def test_monitoring_does_not_disturb_requests(self):
        self.link.set_silent(False)
        self.link.set_monitor(True)
        for _ in range(20):
            self.assertEqual(self.engine().read_did(0xF190), b"WVWZZZAUZJW123456")
        frames = self.link.read_frames(0.3)
        self.link.set_monitor(False)
        self.assertTrue(any(f.startswith("7E8 ") for f in frames))    # our own exchange was seen too
        self.assertTrue(any(f.startswith("3C0 ") for f in frames))

    def test_exit_returns_to_text_and_reentry_works(self):
        self.link._leave()
        self.link._write("mode m\n")
        deadline = time.monotonic() + 3
        while self.link._read_line(deadline) != "$":
            pass
        self.assertIn("bus listen-only", Link.command(self.link, "status"))
        self.link._enter()
        # A second program finds the console still in binary mode (the first one died without leaving).
        self.link._enter()
        self.assertTrue(self.link.status()["silent"])


class BinarySmallChunks(BinaryAgainstSimulator):
    """As a BLE connection delivers it: a few bytes at a time."""
    READ_CHUNK = 7


class BleAgainstSimulator(AgainstSimulator):
    """Everything in AgainstSimulator again, through the simulator's Bluetooth stand-in
    (182-byte notifications at 20 kB/s, as from an iPhone)."""
    LINK = BleSimLink

    def test_usb_console_and_phone_at_the_same_time(self):
        self.link.set_silent(False)
        self.link.console("mode m")
        self.link.console("vin")
        self.assertEqual(self.engine().read_did(0xF190), b"WVWZZZAUZJW123456")
        out = b""
        deadline = time.monotonic() + 3
        while b"WVWZZZAUZJW123456" not in out and time.monotonic() < deadline:
            out += self.link._proc.stdout.read1(4096) if hasattr(self.link._proc.stdout, "read1") else os.read(self.link._proc.stdout.fileno(), 4096)
        self.assertIn(b"F190: WVWZZZAUZJW123456", out)

    def test_monitor_belongs_to_the_link(self):
        self.link.set_monitor(True)
        self.assertTrue(self.link.status()["monitor"])
        self.link.console("status")   # the text console's own monitor stays off
        self.link.set_monitor(False)


class BleSlowLink(unittest.TestCase):
    """A link far too slow for the bus traffic: events are dropped, answers are not."""

    def test_responses_survive_an_event_flood(self):
        link = BleSimLink.simulator(SIM, mtu=23, rate=300)   # far slower than the bus traffic
        self.addCleanup(link.close)
        link.set_silent(False)
        link.set_monitor(True)
        engine = UdsClient(link, 0x7E0, 0x7E8)
        for _ in range(3):
            self.assertEqual(engine.read_did(0xF19E)[:5], b"EV_EC")   # 27-byte multi-frame answer
        link.read_frames(3.0)
        for _ in range(3):
            self.assertEqual(engine.read_did(0xF19E)[:5], b"EV_EC")     # still answered while flooded
        link.set_monitor(False)
        # The queue drains slowly at this speed, which is the point: a reply is
        # never queued behind more than BLE_TX_EVENT_BACKLOG bytes of events.
        self.assertGreater(link.status()["events_dropped"], 0)

    def test_reconnect_starts_clean(self):
        link = BleSimLink.simulator(SIM)
        self.addCleanup(link.close)
        port = link._sock.getpeername()[1]
        link.set_monitor(True)
        link._write_bytes(b"\x05\x01")       # half a packet, then the phone goes away
        link._sock.close()
        link._sock = None
        again = BleSimLink.connect("127.0.0.1", port)
        self.addCleanup(again.close)
        status = again.status()
        self.assertFalse(status["monitor"])
        self.assertEqual(status["bad_packets"], 0)


class Framing(unittest.TestCase):
    def test_crc_reference_value(self):
        self.assertEqual(binlink.crc16(b"123456789"), 0x29B1)

    def test_round_trips(self):
        for n in (1, 2, 252, 253, 254, 255, 256, 508, 509, 4160):
            for payload in (bytes(n), bytes([0xFF]) * n, bytes((i * 31 + 7) & 0xFF for i in range(n))):
                packet = binlink.encode(payload)
                self.assertEqual(packet.count(0), 1)
                self.assertEqual(packet[-1], 0)
                self.assertEqual(binlink.decode(packet[:-1]), payload)

    def test_damage_is_detected(self):
        packet = bytearray(binlink.encode(b"\x01\x02\x00\x03")[:-1])
        packet[1] ^= 1
        self.assertIsNone(binlink.decode(bytes(packet)))
        self.assertIsNone(binlink.decode(b"\x05\x11"))

    def test_wire_bytes_are_pinned(self):
        # The two examples in hardware/pico2w/docs/link_protocol.md.
        self.assertEqual(binlink.encode([1, 1]).hex(), "0501013e1f00")
        read_vin = bytes([2, 5]) + bytes.fromhex("e0070000e80700000000") + bytes.fromhex("22f190")
        self.assertEqual(binlink.encode(read_vin).hex(), "050205e0070103e8070101010622f190ad1200")


class CommandLine(unittest.TestCase):
    MODE = ["--sim"]

    def run_cli(self, *argv):
        out = io.StringIO()
        with redirect_stdout(out):
            cli.main(self.MODE + list(argv))
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
        self.assertIn("MO_Leistung=170 KiloWatt", summary)
        self.assertGreater(full.count("Klemmen_Status_01"), 5)

    def test_live_csv_log(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "log.csv")
            self.run_cli("--transmit", "live", "rpm", "coolant", "rail", "--count", "3", "--interval", "0.05", "--csv", path)
            with open(path) as f:
                rows = f.read().strip().split("\n")
        self.assertEqual(rows[0], "time_s,rpm_rpm,coolant_C,rail_kPa")
        self.assertEqual(len(rows), 4)
        self.assertTrue(rows[1].endswith(",90,"))   # unsupported value logs as an empty cell


class CommandLineBinary(CommandLine):
    MODE = ["--sim", "--binary"]


def sim_with(spec, link_class=Link):
    """Start the simulator with a particular scenario (VWSIM)."""
    saved = os.environ.get("VWSIM")
    os.environ["VWSIM"] = spec
    try:
        return link_class.simulator(SIM)
    finally:
        os.environ["VWSIM"] = saved if saved is not None else ""


class RealCar(unittest.TestCase):
    """The simulator's pessimistic side: the things the car will do that an ideal bus would not."""

    def open(self, spec):
        link = sim_with(spec)
        self.addCleanup(link.close)
        return link

    def engine(self, link, **kw):
        return UdsClient(link, 0x7E0, 0x7E8, **kw)

    def test_gateway_hides_broadcast_by_default(self):
        link = self.open("realistic")
        link.set_monitor(True)
        frames = link.read_frames(0.5)
        link.set_monitor(False)
        self.assertEqual(frames, [])
        link.set_silent(False)
        self.assertEqual(self.engine(link).read_did(0xF1AD), b"DKFA")   # diagnostics still get through

    def test_answers_take_time(self):
        link = self.open("realistic,latency=40-60")
        link.set_silent(False)
        start = time.monotonic()
        for _ in range(5):
            self.engine(link).tester_present()
        self.assertGreaterEqual((time.monotonic() - start) / 5, 0.035)

    def test_extended_session_expires(self):
        link = self.open("realistic,s3=600")
        link.set_silent(False)
        engine = self.engine(link)
        engine.session(0x03)
        self.assertEqual(engine.read_did(0xF186), b"\x03")
        time.sleep(0.3)
        engine.tester_present()                       # keeps it alive
        time.sleep(0.4)
        self.assertEqual(engine.read_did(0xF186), b"\x03")
        time.sleep(0.9)
        self.assertEqual(engine.read_did(0xF186), b"\x01")   # dropped back to the default session

    def test_functional_obd_request_gets_two_answers(self):
        # Engine and transmission both answer 7DF; listening for one of them ignores the other.
        link = self.open("realistic")
        link.set_silent(False)
        engine = UdsClient(link, 0x7DF, 0x7E8).request([0x01, 0x00])
        trans = UdsClient(link, 0x7DF, 0x7E9).request([0x01, 0x00])
        self.assertEqual(engine[:2], bytes([0x41, 0x00]))
        self.assertEqual(trans, bytes([0x41, 0x00, 0x80, 0x08, 0x00, 0x00]))   # PIDs 01 and 0D only
        with self.assertRaises(NoResponse):          # functional requests get no refusals
            UdsClient(link, 0x7DF, 0x7E9, timeout_ms=200).request([0x09, 0x02])

    def test_modules_pace_long_requests(self):
        # A request spanning several frames: the ABS module asks for blocks of 2, 5 ms apart.
        link = self.open("realistic,fc=2/5")
        link.set_silent(False)
        abs_ = UdsClient(link, 0x713, 0x77D)
        out = abs_.read_dids([0xF187, 0xF189, 0xF197, 0xF190, 0xF18C, 0xF19E])
        self.assertEqual(out[0xF187], b"5Q0614517BK")

    def test_ignition_decides_who_answers(self):
        link = self.open("realistic,ignition=off:0/on:1500/crank:2500/run:3200")
        link.set_silent(False)
        gw = UdsClient(link, 0x710, 0x77A, timeout_ms=300)
        gw.tester_present()                                   # the gateway wakes for diagnostics
        with self.assertRaises(NoResponse):
            self.engine(link, timeout_ms=300).tester_present()   # everything else is asleep
        time.sleep(1.6)
        self.assertEqual(obd.read_pid(self.engine(link), "rpm")[0], 0)   # ignition on, engine off
        self.assertAlmostEqual(obd.read_pid(self.engine(link), "voltage")[0], 12.3, delta=0.2)
        time.sleep(1.0)
        with self.assertRaises(NegativeResponse) as cm:       # cranking: busy, ask again
            obd.read_pid(self.engine(link), "rpm")
        self.assertEqual(cm.exception.nrc, 0x21)
        time.sleep(1.2)
        self.assertGreater(obd.read_pid(self.engine(link), "rpm")[0], 600)

    def test_bus_faults_are_counted_and_survived(self):
        from vwdiag.session import retry
        link = self.open("realistic,gateway=open,drop=3,errors=5,delay=5:20")
        link.set_silent(False)
        ok = 0
        for _ in range(20):
            try:
                retry(lambda: self.engine(link, timeout_ms=200).read_did(0xF190), tries=4)
                ok += 1
            except UdsError:
                pass
        status = link.status()
        self.assertGreater(status["errors"], 0)
        self.assertGreater(status["dropped"], 0)
        self.assertGreaterEqual(ok, 16)

    def test_bus_off_window(self):
        link = self.open("realistic,busoff=0:1200")
        link.set_silent(False)
        with self.assertRaises(UdsError):
            self.engine(link, timeout_ms=200).tester_present()
        time.sleep(1.3)
        self.engine(link).tester_present()
        self.assertGreater(link.status()["errors"], 0)

    def test_live_values_move_together(self):
        link = self.open("realistic,drive=1")
        link.set_silent(False)
        e = self.engine(link)
        rpm, maf, map_ = (obd.read_pid(e, n)[0] for n in ("rpm", "maf", "map"))
        self.assertTrue(650 <= rpm <= 760, rpm)
        self.assertTrue(2.0 <= maf <= 6.0, maf)        # EA888 Gen 3 at warm idle: a few g/s
        self.assertTrue(25 <= map_ <= 40, map_)
        time.sleep(17)                                   # the trace pulls away after 15 s
        rpm2, speed = obd.read_pid(e, "rpm")[0], obd.read_pid(e, "speed")[0]
        self.assertGreater(speed, 5)
        self.assertGreater(rpm2, rpm)


class Misfire(unittest.TestCase):
    """The 'gti' scenario: cylinder 3 low on compression, a stored P0303 from earlier drives."""

    def setUp(self):
        self.link = sim_with("gti")
        self.addCleanup(self.link.close)
        self.link.set_silent(False)
        self.engine = UdsClient(self.link, 0x7E0, 0x7E8)

    def test_emission_view(self):
        lamp, count = obd.monitor_status(self.engine)
        self.assertTrue(lamp)
        self.assertEqual(count, 3)
        self.assertIn("P0303", [dtc.sae_code(c) for c in obd.emission_dtcs(self.engine)])
        code, values = obd.freeze_frame(self.engine)
        self.assertEqual(dtc.sae_code(code), "P0303")
        self.assertEqual(values["rpm"], 812)
        self.assertEqual(values["coolant"], 41)
        self.assertAlmostEqual(values["stft"], 7.8, delta=0.1)

    def test_misfire_monitor_points_at_cylinder_3(self):
        counts = obd.misfire_counts(self.engine)
        self.assertEqual(counts[3][1], 47)                  # ten-cycle average from earlier drives
        self.assertEqual([counts[c][1] for c in (1, 2, 4)], [0, 0, 0])
        time.sleep(4)                                        # cold idle: the weak cylinder misfires
        counts = obd.misfire_counts(self.engine)
        self.assertGreater(counts[3][0], 0)
        self.assertEqual([counts[c][0] for c in (1, 2, 4)], [0, 0, 0])
        self.assertEqual(counts[0][0], counts[3][0])

    def test_uds_snapshot_and_counters(self):
        p0303 = bytes([0x03, 0x03, 0x00])
        snaps = self.engine.dtc_snapshots(p0303, sizes=obd.SNAPSHOT_SIZES)
        values = obd.decode_snapshot(snaps[1])
        self.assertEqual(values["rpm"], 812)
        self.assertEqual(self.engine.dtc_extended(p0303), bytes([0x01, 6, 0x02, 0]))
        self.assertEqual(self.engine.dtc_count(0x08), 3)
        with self.assertRaises(NegativeResponse):
            self.engine.dtc_snapshots(bytes([0x01, 0x23, 0x00]))

    def test_simos18_values(self):
        self.assertIn("knock_cyl_4", simos18.VALUES)
        self.assertEqual(simos18.read(self.engine, "knock_cyl_3")[0], 0)
        rpm = simos18.read(self.engine, "engine_speed")[0]
        self.assertGreater(rpm, 600)
        self.assertLess(simos18.read(self.engine, "ign_avg")[0], 0)   # catalyst heating after the cold start

    def test_clearing_resets_the_monitor(self):
        time.sleep(2)
        self.engine.request([0x04])
        counts = obd.misfire_counts(self.engine)
        self.assertEqual(counts[3], (0, 0))
        self.assertEqual(obd.emission_dtcs(self.engine), [])

    def test_formula_parser_refuses_code(self):
        with self.assertRaises(ValueError):
            simos18._compile("__import__('os').system('true')")
        self.assertEqual(simos18._compile("(x - 2731.4) / 10")(3631.4), 90)


class Session(unittest.TestCase):
    def test_session_runs_and_saves_everything(self):
        card = tempfile.mkdtemp()
        out = tempfile.mkdtemp()
        os.environ["VWDIAG_SD"] = card
        self.addCleanup(os.environ.pop, "VWDIAG_SD", None)
        buf = io.StringIO()
        with redirect_stdout(buf):
            cli.main(["--sim", "--sim-profile", "gti", "--transmit", "session", "--yes",
                      "--listen", "1", "--idle", "3", "--drive", "2", "--out", out])
        import json
        with open(os.path.join(out, "session.json")) as f:
            data = json.load(f)
        self.assertEqual(data["steps"]["listen"]["off"]["frames"], 0)
        self.assertEqual(data["steps"]["scan"]["modules"]["radar"]["state"], "no answer")
        self.assertEqual(data["steps"]["scan"]["identification"]["engine"]["Engine code"], "DKFA")
        codes = [c["code"] for c in data["steps"]["faults"]["engine"]]
        self.assertIn("P0303", codes)
        self.assertEqual(data["steps"]["faults"]["engine_obd"]["freeze_frame"]["code"], "P0303")
        self.assertGreaterEqual(data["steps"]["idle"]["rows"], 2)
        self.assertGreater(data["steps"]["drive"]["frames"], 0)
        for name in ("report.txt", "idle.csv", "listen_off.log", "listen_on.log"):
            self.assertTrue(os.path.exists(os.path.join(out, name)), name)
        self.assertIn("gateway keeps it in", buf.getvalue())
        self.assertTrue(os.listdir(card))


class Experiments(unittest.TestCase):
    """Experiments against a stand-in car whose numbers are known, then calibration back to them."""
    CAR = ("realistic,gateway=closed,latency=10-30,fc=4/2,misfire=2,idle=680,cold_idle=1180,mis_idle=1.2,"
           "trim_idle=4.5,history=0")

    def run_exp(self, out, spec, *argv):
        buf = io.StringIO()
        with redirect_stdout(buf):
            cli.main(["--sim", "--binary", "--sim-profile", spec, "--transmit", "experiment"] + list(argv) +
                     ["--yes", "--out", out])
        return buf.getvalue()

    def test_identify_listen_warm_idle_and_calibrate(self):
        out = tempfile.mkdtemp()
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", out]))
        text = self.run_exp(out, self.CAR, "identify")
        self.assertIn("functional OBD request answered by: 7E8, 7E9", text)
        self.run_exp(out, self.CAR, "listen", "--seconds", "1")
        self.run_exp(out, self.CAR + ",start=warm,speed=10", "warm-idle", "--seconds", "20", "--interval", "0.3")
        import json
        folders = sorted(os.path.join(out, d) for d in os.listdir(out))
        with open(os.path.join(folders[0], "summary.json")) as f:
            ident = json.load(f)
        self.assertEqual(ident["modules"]["abs"]["flow_control"][0], {"bs": 4, "stmin": 2})
        self.assertFalse(ident["modules"]["radar"]["present"])
        with open(os.path.join(folders[0], "meta.json")) as f:
            self.assertEqual(json.load(f)["target"], "simulator")

        from vwdiag import calibrate
        scenario, cdir = calibrate.run(folders, out=os.path.join(out, "calib"), say=lambda *_: None)
        got = dict(item.split("=", 1) for item in scenario.split(",")[1:])
        self.assertEqual((got["gateway"], got["fc"], got["misfire"], got["trans_obd"]), ("closed", "4/2", "2", "1"))
        self.assertAlmostEqual(float(got["idle"]), 680, delta=15)
        self.assertAlmostEqual(float(got["mis_idle"]), 1.2, delta=0.35)
        self.assertAlmostEqual(float(got["trim_idle"]), 4.5, delta=1.0)
        lo, hi = (int(v) for v in got["latency"].split("-"))
        self.assertTrue(8 <= lo <= 14 and 26 <= hi <= 36, got["latency"])
        self.assertTrue(os.path.exists(os.path.join(cdir, "calibration.md")))
        # the calibrated scenario is one the simulator accepts
        link = sim_with(scenario)
        link.close()

    def test_engine_settings_reach_the_simulator(self):
        link = sim_with("realistic,idle=820")
        self.addCleanup(link.close)
        link.set_silent(False)
        self.assertAlmostEqual(obd.read_pid(UdsClient(link, 0x7E0, 0x7E8), "rpm")[0], 820, delta=20)

    def test_several_pids_in_one_request(self):
        link = sim_with("realistic")
        self.addCleanup(link.close)
        link.set_silent(False)
        r = UdsClient(link, 0x7E0, 0x7E8).request([0x01, 0x0C, 0x05, 0x99])
        self.assertEqual((r[0], r[1], r[4]), (0x41, 0x0C, 0x05))   # unsupported 99 is left out
        self.assertEqual(len(r), 6)


class Power(unittest.TestCase):
    """The dongle's power management, in the simulator (built with it, like the dongle)."""

    def open(self, spec, usb="1", wake="can", link_class=BinaryLink):
        os.environ["VWDIAG_USB"], os.environ["VWDIAG_WAKE"] = usb, wake
        self.addCleanup(os.environ.pop, "VWDIAG_USB", None)
        self.addCleanup(os.environ.pop, "VWDIAG_WAKE", None)
        link = sim_with(spec, link_class)
        self.addCleanup(link.close)
        return link

    def test_status_over_both_links(self):
        for cls in (Link, BinaryLink):
            link = self.open("realistic", wake="engine", link_class=cls)
            p = link.power()
            self.assertEqual(p["wake"], "engine")
            self.assertIn("usb", p["holds"])
            self.assertIn("engine", p["holds"])          # 14.1 V: the engine is running
            self.assertAlmostEqual(p["battery_mv"], 14150, delta=150)
            link.power(2, 90)
            link.power(1, 1)
            p = link.power()
            self.assertEqual((p["idle_s"], p["stay_awake"]), (90, True))
            with self.assertRaises(LinkError):
                link.power(2, 5)                         # under the 30 s floor
            with self.assertRaises(LinkError):
                link.power(3, 9000)                      # a cutoff that would flatten the battery

    def test_asleep_when_asked_unless_on_usb(self):
        link = self.open("realistic,ignition=off:0", usb="1")
        link.power(4)
        time.sleep(1.0)
        self.assertEqual(link.power()["wake"], "can")    # still here: a computer is on USB
        link = self.open("realistic,ignition=off:0", usb="0")
        link.power(4)
        time.sleep(1.0)
        with self.assertRaises(LinkError):              # the power went off: the link is gone
            link.status(timeout=2)

    def test_sleeps_when_nothing_holds_it(self):
        link = self.open("realistic,ignition=off:0", usb="0")
        link.power(2, 30)
        p = link.power()
        self.assertEqual(p["holds"], ["activity"])       # just used: one idle period
        self.assertAlmostEqual(p["battery_mv"], 12500, delta=50)
        time.sleep(32)
        with self.assertRaises(LinkError):
            link.status(timeout=2)

    def test_boards_without_it_say_so(self):
        # the text console answers on every build; the simulator is built with power management
        link = self.open("realistic")
        self.assertIn("usb", link.power()["holds"])


if __name__ == "__main__":
    unittest.main()
