// A tiny 8-bit CPU built from the earlier examples' blocks.
//
// Hierarchy (6 levels): cpu -> datapath -> alu8 -> adder8 -> full_adder -> half_adder,
//   also cpu -> datapath -> regfile -> reg8 -> dffe -> mux2
//   and  cpu -> pc_unit -> adder8 -> ...
//
// Instruction (8 bits): [7:6] opcode, [5:4] rd, [3:2] rs, [1:0] funct, [3:0] imm
//   00 rd rs ff : rd = rd op rs   (ff: 00 add, 01 sub, 10 and, 11 xor)
//   01 rd iiii  : rd = iiii       (zero-extended)
//   10 rd oooo  : if rd != 0, pc = pc + oooo (sign-extended)
//   11 rd ----  : out = rd
// The control unit alternates FETCH (latch the instruction) and EXECUTE.

module alu8 (
    input      [7:0] a,
    input      [7:0] b,
    input      [1:0] op,
    output reg [7:0] y
);
  wire [7:0] sum;
  wire       cout;
  // op 01 subtracts: a + ~b + 1
  adder8 add (.a(a), .b(op[0] ? ~b : b), .cin(op[0]), .sum(sum), .cout(cout));

  always @(*) begin
    case (op)
      2'b10:   y = a & b;
      2'b11:   y = a ^ b;
      default: y = sum;
    endcase
  end
endmodule

module control (
    input        clk,
    input        rst,
    input  [7:0] ir,
    input        rd_zero,
    output reg   execute,   // 0: FETCH, 1: EXECUTE
    output       reg_we,
    output       out_we,
    output       branch
);
  always @(posedge clk)
    if (rst)
      execute <= 1'b0;
    else
      execute <= ~execute;

  wire [1:0] opcode = ir[7:6];
  assign reg_we = execute && (opcode == 2'b00 || opcode == 2'b01);
  assign out_we = execute && opcode == 2'b11;
  assign branch = execute && opcode == 2'b10 && !rd_zero;
endmodule

module pc_unit (
    input            clk,
    input            rst,
    input            advance,
    input            branch,
    input      [3:0] offset,
    output reg [7:0] pc
);
  wire [7:0] step = branch ? {{4{offset[3]}}, offset} : 8'd1;
  wire [7:0] next;
  wire       cout;
  adder8 add (.a(pc), .b(step), .cin(1'b0), .sum(next), .cout(cout));

  always @(posedge clk)
    if (rst)
      pc <= 8'd0;
    else if (advance)
      pc <= next;
endmodule

// Register file + ALU; executes the instruction held in `ir`.
module datapath (
    input        clk,
    input        reg_we,
    input  [7:0] ir,
    output [7:0] rd_val
);
  wire [1:0] opcode = ir[7:6];
  wire [1:0] rd     = ir[5:4];
  wire [1:0] rs     = ir[3:2];
  wire [7:0] rs_val, alu_y;

  regfile rf (
      .clk(clk), .we(reg_we), .waddr(rd),
      .wdata(opcode == 2'b01 ? {4'b0000, ir[3:0]} : alu_y),
      .raddr_a(rd), .raddr_b(rs), .rdata_a(rd_val), .rdata_b(rs_val));

  alu8 alu (.a(rd_val), .b(rs_val), .op(ir[1:0]), .y(alu_y));
endmodule

module cpu (
    input            clk,
    input            rst,
    input      [7:0] instr,   // instruction at address `pc`
    output     [7:0] pc,
    output reg [7:0] out
);
  reg  [7:0] ir;
  wire       execute;
  wire [7:0] rd_val;
  wire       reg_we, out_we, branch;

  always @(posedge clk)
    if (!execute)
      ir <= instr;   // FETCH

  datapath dp (.clk(clk), .reg_we(reg_we), .ir(ir), .rd_val(rd_val));

  control ctl (
      .clk(clk), .rst(rst), .ir(ir), .rd_zero(rd_val == 8'd0),
      .execute(execute), .reg_we(reg_we), .out_we(out_we), .branch(branch));

  pc_unit pcu (
      .clk(clk), .rst(rst), .advance(execute), .branch(branch),
      .offset(ir[3:0]), .pc(pc));

  always @(posedge clk)
    if (out_we)
      out <= rd_val;
endmodule
