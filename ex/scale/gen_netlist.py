#!/usr/bin/env python3
"""Generate a large synthetic netlist in Yosys JSON format, for testing how
netviz scales.

The netlist is a random network of the gates in ../cells.lib (with the same
structure Yosys writes: port_directions, src attributes, blackbox gate modules).
With --blocks K > 1 the gates are split into K different block modules that
the top module chains together.

    python3 gen_netlist.py --cells 1000000 -o scale_1000000.json
"""

import argparse
import random

GATES = {  # name -> (inputs, output)
    "NAND": (["A", "B"], "Y"),
    "NOR": (["A", "B"], "Y"),
    "NOT": (["A"], "Y"),
    "BUF": (["A"], "Y"),
    "DFF": (["C", "D"], "Q"),
}
GATE_MIX = ["NAND"] * 4 + ["NOR"] * 3 + ["NOT"] * 2 + ["DFF"]
WIDTH = 32  # input and output bus width of each block


def bits(values):
    return "[ " + ", ".join(str(v) for v in values) + " ]"


def write_gate_modules(out):
    for i, (name, (inputs, output)) in enumerate(GATES.items()):
        ports = [f'"{p}": {{ "direction": "input", "bits": [ {2 + k} ] }}' for k, p in enumerate(inputs)]
        ports.append(f'"{output}": {{ "direction": "output", "bits": [ {2 + len(inputs)} ] }}')
        out.write(f'    "{name}": {{\n'
                  '      "attributes": { "blackbox": "00000000000000000000000000000001" },\n'
                  f'      "ports": {{ {", ".join(ports)} }},\n'
                  '      "cells": { }, "netnames": { }\n'
                  '    },\n')


def write_block(out, name, cells, rng, is_top):
    """A module with input bus `i`, output bus `o`, and `cells` random gates."""
    next_bit = 2
    in_bits = list(range(next_bit, next_bit + WIDTH))
    next_bit += WIDTH
    clk = next_bit
    next_bit += 1
    signals = list(in_bits)  # signals gates may read
    attrs = '"top": "00000000000000000000000000000001", ' if is_top else ""

    out.write(f'    "{name}": {{\n'
              f'      "attributes": {{ {attrs}"src": "{name}.v:1.1-9.10" }},\n'
              '      "ports": {\n'
              f'        "clk": {{ "direction": "input", "bits": [ {clk} ] }},\n'
              f'        "i": {{ "direction": "input", "bits": {bits(in_bits)} }},\n')
    out_bits_pos = out.tell()
    out.write(" " * (40 + WIDTH * 12) + "\n")  # placeholder for the output port
    out.write('      },\n      "cells": {\n')

    gate_outputs = []
    for c in range(cells):
        gate = rng.choice(GATE_MIX)
        inputs, output = GATES[gate]
        y = next_bit
        next_bit += 1
        conns = []
        for p in inputs:
            src = clk if (gate == "DFF" and p == "C") else rng.choice(signals[-2000:])
            conns.append(f'"{p}": [ {src} ]')
        conns.append(f'"{output}": [ {y} ]')
        dirs = ", ".join([f'"{p}": "input"' for p in inputs] + [f'"{output}": "output"'])
        sep = "," if c + 1 < cells else ""
        out.write(f'        "g{c}": {{ "hide_name": 0, "type": "{gate}", "parameters": {{ }}, '
                  f'"attributes": {{ "src": "{name}.v:{c + 2}.3-{c + 2}.40" }}, '
                  f'"port_directions": {{ {dirs} }}, "connections": {{ {", ".join(conns)} }} }}{sep}\n')
        signals.append(y)
        gate_outputs.append(y)

    out_bits = (gate_outputs + in_bits)[-WIDTH:]
    out.write('      },\n      "netnames": {\n')
    out.write(f'        "clk": {{ "hide_name": 0, "bits": [ {clk} ], "attributes": {{ }} }},\n')
    out.write(f'        "i": {{ "hide_name": 0, "bits": {bits(in_bits)}, "attributes": {{ }} }},\n')
    for c, y in enumerate(gate_outputs):
        out.write(f'        "n{c}": {{ "hide_name": 0, "bits": [ {y} ], "attributes": {{ }} }},\n')
    out.write(f'        "o": {{ "hide_name": 0, "bits": {bits(out_bits)}, "attributes": {{ }} }}\n')
    out.write('      }\n    }')

    # Fill in the output port now that its bits are known.
    end = out.tell()
    out.seek(out_bits_pos)
    out.write(f'        "o": {{ "direction": "output", "bits": {bits(out_bits)} }}')
    out.seek(end)


def write_top_of_blocks(out, blocks):
    """top: chains block_0 .. block_{n-1}, each output feeding the next input."""
    next_bit = 2
    clk = next_bit
    next_bit += 1
    first = list(range(next_bit, next_bit + WIDTH))
    next_bit += WIDTH
    out.write('    "top": {\n'
              '      "attributes": { "top": "00000000000000000000000000000001" },\n'
              '      "ports": {\n'
              f'        "clk": {{ "direction": "input", "bits": [ {clk} ] }},\n'
              f'        "i": {{ "direction": "input", "bits": {bits(first)} }},\n')
    chain = [first]
    for _ in range(blocks):
        chain.append(list(range(next_bit, next_bit + WIDTH)))
        next_bit += WIDTH
    out.write(f'        "o": {{ "direction": "output", "bits": {bits(chain[-1])} }}\n'
              '      },\n      "cells": {\n')
    for b in range(blocks):
        sep = "," if b + 1 < blocks else ""
        out.write(f'        "u{b}": {{ "hide_name": 0, "type": "block_{b}", "parameters": {{ }}, '
                  '"attributes": { }, '
                  '"port_directions": { "clk": "input", "i": "input", "o": "output" }, '
                  f'"connections": {{ "clk": [ {clk} ], "i": {bits(chain[b])}, '
                  f'"o": {bits(chain[b + 1])} }} }}{sep}\n')
    out.write('      },\n      "netnames": {\n'
              f'        "clk": {{ "hide_name": 0, "bits": [ {clk} ], "attributes": {{ }} }},\n'
              f'        "i": {{ "hide_name": 0, "bits": {bits(first)}, "attributes": {{ }} }},\n'
              f'        "o": {{ "hide_name": 0, "bits": {bits(chain[-1])}, "attributes": {{ }} }}\n'
              '      }\n    }')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cells", type=int, default=100000, help="total number of gates")
    parser.add_argument("--blocks", type=int, default=1, help="number of block modules")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    with open(args.output, "w") as out:
        out.write('{\n  "creator": "netviz ex/scale/gen_netlist.py",\n  "modules": {\n')
        write_gate_modules(out)
        if args.blocks <= 1:
            write_block(out, "top", args.cells, rng, is_top=True)
        else:
            per_block = max(1, args.cells // args.blocks)
            for b in range(args.blocks):
                write_block(out, f"block_{b}", per_block, rng, is_top=False)
                out.write(",\n")
            write_top_of_blocks(out, args.blocks)
        out.write("\n  }\n}\n")


if __name__ == "__main__":
    main()
