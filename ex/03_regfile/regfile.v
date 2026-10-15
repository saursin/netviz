// 2-to-1 multiplexer.
module mux2 (
    input  a,
    input  b,
    input  sel,
    output y
);
  assign y = sel ? b : a;
endmodule

// Flip-flop with enable: a flop that reloads its own value unless enabled.
module dffe (
    input      clk,
    input      en,
    input      d,
    output reg q
);
  wire next;
  mux2 hold (.a(q), .b(d), .sel(en), .y(next));

  always @(posedge clk)
    q <= next;
endmodule

// 8-bit register: eight enabled flip-flops.
module reg8 (
    input        clk,
    input        en,
    input  [7:0] d,
    output [7:0] q
);
  genvar i;
  generate
    for (i = 0; i < 8; i = i + 1) begin : slice
      dffe ff (.clk(clk), .en(en), .d(d[i]), .q(q[i]));
    end
  endgenerate
endmodule

// 4 x 8-bit register file: one write port, two read ports.
module regfile (
    input        clk,
    input        we,
    input  [1:0] waddr,
    input  [7:0] wdata,
    input  [1:0] raddr_a,
    input  [1:0] raddr_b,
    output [7:0] rdata_a,
    output [7:0] rdata_b
);
  wire [7:0] q0, q1, q2, q3;
  reg8 r0 (.clk(clk), .en(we && waddr == 2'd0), .d(wdata), .q(q0));
  reg8 r1 (.clk(clk), .en(we && waddr == 2'd1), .d(wdata), .q(q1));
  reg8 r2 (.clk(clk), .en(we && waddr == 2'd2), .d(wdata), .q(q2));
  reg8 r3 (.clk(clk), .en(we && waddr == 2'd3), .d(wdata), .q(q3));

  assign rdata_a = raddr_a[1] ? (raddr_a[0] ? q3 : q2) : (raddr_a[0] ? q1 : q0);
  assign rdata_b = raddr_b[1] ? (raddr_b[0] ? q3 : q2) : (raddr_b[0] ? q1 : q0);
endmodule
