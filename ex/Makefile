# Regenerates the example netlists. Requires Yosys (e.g. `module load yosys`).
#
#   make                      rebuild every example's .json and .rtl.json
#   make 05_cpu               rebuild one example
#   make scale CELLS=100000   generate a large synthetic netlist (not committed)
#
# Each example folder holds Verilog sources and two netlists:
#   <top>.json      gate level: mapped to the gates in cells.lib
#   <top>.rtl.json  word level: Yosys `prep`, cells like $add and $mux
# Yosys runs inside the folder so `src` attributes are relative to it.

YOSYS  ?= yosys
PYTHON ?= python3
LIB    := $(CURDIR)/cells.lib

# example folder -> top module
EXAMPLES := 01_fulladder 02_adder8 03_regfile 04_popcount 05_cpu
TOP_01_fulladder := full_adder
TOP_02_adder8    := adder8
TOP_03_regfile   := regfile
TOP_04_popcount  := popcount8
TOP_05_cpu       := cpu

GATES = read_liberty -lib $(LIB); read_verilog -sv $(notdir $(filter %.v,$^)); \
        synth -top $(TOP_$(1)); dfflibmap -liberty $(LIB); abc -liberty $(LIB); \
        opt_clean; write_json $(notdir $@)
RTL   = read_verilog -sv $(notdir $(filter %.v,$^)); prep -top $(TOP_$(1)); write_json $(notdir $@)

.PHONY: all clean scale $(EXAMPLES)
all: $(EXAMPLES)

define example
$(1): $(1)/$(TOP_$(1)).json $(1)/$(TOP_$(1)).rtl.json

$(1)/$(TOP_$(1)).json: $(wildcard $(1)/*.v) $(LIB)
	cd $(1) && $(YOSYS) -q -p "$$(call GATES,$(1))"

$(1)/$(TOP_$(1)).rtl.json: $(wildcard $(1)/*.v)
	cd $(1) && $(YOSYS) -q -p "$$(call RTL,$(1))"
endef
$(foreach e,$(EXAMPLES),$(eval $(call example,$(e))))

CELLS  ?= 100000
BLOCKS ?= 1
scale:
	$(PYTHON) scale/gen_netlist.py --cells $(CELLS) --blocks $(BLOCKS) \
		-o scale/scale_$(CELLS).json

clean:
	rm -f scale/*.json
