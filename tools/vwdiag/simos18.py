"""Engine measuring values for the Simos 18 engine module (EA888 Gen 3, as in the MK7 / MK7.5 GTI).

data/simos18_parameters_22.csv is the service-0x22 logging list from Brian Ledbetter's VW_Flash
project (BSD 2-clause licence, see data/LICENSE.VW_Flash): identifier, length, signedness and
the formula from raw value to engineering unit, as its logger uses them on Simos 18. Not yet
confirmed on this car. Rows computed from other rows (identifier 0xFFFF) are left out.
"""
import ast
import csv
import operator
import os
import re

from .uds import UdsError

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "simos18_parameters_22.csv")

_OPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv,
        ast.USub: operator.neg}


def _compile(equation):
    """Turn a formula like "(x - 2731.4) / 10" into a function of x; only arithmetic is allowed."""
    tree = ast.parse(equation, mode="eval").body

    def ev(node, x):
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
            return node.value
        if isinstance(node, ast.Name) and node.id == "x":
            return x
        if isinstance(node, ast.BinOp) and type(node.op) in _OPS:
            return _OPS[type(node.op)](ev(node.left, x), ev(node.right, x))
        if isinstance(node, ast.UnaryOp) and type(node.op) in _OPS:
            return _OPS[type(node.op)](ev(node.operand, x))
        raise ValueError("unsupported formula: %s" % equation)
    ev(tree, 1)   # reject anything else up front
    return lambda x: ev(tree, x)


def key(name):
    """'Knock Cyl 1' -> 'knock_cyl_1'."""
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def load():
    """{key: (did, length, signed, unit, decode, name)} for every directly readable value."""
    out = {}
    with open(PATH, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            did = int(row["Address"], 16)
            if did == 0xFFFF:
                continue
            try:
                decode = _compile(row["Equation"])
            except (ValueError, SyntaxError):
                continue
            out[key(row["Name"])] = (did, int(row["Length"]), row["Signed"].upper() == "TRUE",
                                     row["Unit"], decode, row["Name"])
    return out


VALUES = load()


def read(client, name):
    """Read one value from the engine module. Returns (value, unit)."""
    did, length, signed, unit, decode, _ = VALUES[name]
    data = client.read_did(did)
    if len(data) < length:
        raise UdsError("short answer for %04X" % did)
    raw = int.from_bytes(data[:length], "big", signed=signed)
    return decode(raw), unit
