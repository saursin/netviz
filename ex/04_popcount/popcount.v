// Population count (number of 1 bits) of an N-bit word, built as a
// recursive tree: popcount #(N) uses two popcount #(N/2) and adds their
// results. Yosys creates one specialized module per level
// ($paramod\popcount\N=...), giving a deep hierarchy from one source module.

module popcount #(
    parameter N = 8,
    parameter W = $clog2(N) + 1  // width of the count
) (
    input  [N-1:0] in,
    output [W-1:0] count
);
  generate
    if (N == 1) begin : leaf
      assign count = in;
    end else begin : split
      wire [W-2:0] lo, hi;
      popcount #(.N(N/2)) lower (.in(in[N/2-1:0]), .count(lo));
      popcount #(.N(N/2)) upper (.in(in[N-1:N/2]), .count(hi));
      assign count = lo + hi;
    end
  endgenerate
endmodule

module popcount8 (
    input  [7:0] in,
    output [3:0] count
);
  popcount #(.N(8)) tree (.in(in), .count(count));
endmodule
