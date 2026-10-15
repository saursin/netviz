# Examples

Five example designs with increasing hierarchy depth and features. Each folder has
the Verilog source and two netlists you can open in netviz right away:

- `<top>.json` — gate level, mapped to the five gates in [cells.lib](cells.lib)
- `<top>.rtl.json` — word level, with Yosys cells such as `$add` and `$mux`

```bash
netviz ex/05_cpu/cpu.json
```

| Example | Depth | Shows | Gates (flattened) |
|---|---|---|---|
| [01_fulladder](01_fulladder) | 2 | basic gates; `half_adder` used twice | 12 |
| [02_adder8](02_adder8) | 3 | buses and bit slices, a ripple carry chain, many instances of one module | 96 |
| [03_regfile](03_regfile) | 4 | flip-flops, enables, address decoding, wide read muxes, clock fanout | 316 |
| [04_popcount](04_popcount) | 5 | parameters and recursion: one `$paramod` module per tree level | 84 |
| [05_cpu](05_cpu) | 6 | a tiny 8-bit CPU: control FSM, register file, ALU, program counter, feedback loops; reuses the blocks above | 802 |

## Regenerating

The netlists are produced by Yosys (tested with 0.51):

```bash
module load yosys   # or have yosys on your PATH
make                # all examples
make 05_cpu         # one example
```

## Large designs

[scale/gen_netlist.py](scale/gen_netlist.py) generates large random gate
netlists in Yosys JSON format for testing how netviz scales (not committed):

```bash
make scale CELLS=1000000            # one flat module
make scale CELLS=1000000 BLOCKS=8   # split into 8 block modules
```
