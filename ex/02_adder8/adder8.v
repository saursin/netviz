// 8-bit ripple-carry adder from eight full adders.
module adder8 (
    input  [7:0] a,
    input  [7:0] b,
    input        cin,
    output [7:0] sum,
    output       cout
);
  wire [8:0] carry;
  assign carry[0] = cin;

  genvar i;
  generate
    for (i = 0; i < 8; i = i + 1) begin : slice
      full_adder fa (.a(a[i]), .b(b[i]), .cin(carry[i]), .sum(sum[i]), .cout(carry[i+1]));
    end
  endgenerate

  assign cout = carry[8];
endmodule
