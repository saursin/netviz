module half_adder(
    input logic a,
    input logic b,
    output logic sum,
    output logic carry
);
  assign sum = a ^ b;
  assign carry = a & b;
endmodule

module full_adder(
    input logic a,
    input logic b,
    input logic cin,
    output logic sum,
    output logic cout
);
  wire s1, c1, c2;
  half_adder ha1(.a(a), .b(b), .sum(s1), .carry(c1));
  half_adder ha2(.a(s1), .b(cin), .sum(sum), .carry(c2));
  assign cout = c1 | c2;
endmodule