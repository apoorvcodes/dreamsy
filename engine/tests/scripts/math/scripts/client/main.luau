local fns = { "sin", "cos", "tan", "asin", "acos", "atan", "exp", "log", "log10", "sinh", "cosh", "tanh" }
local xs = { 0.1, 0.5, 1, 1.5, 2.2, 3.14159, 10.7, 100.3, 1e5 + 0.37, -0.73, 0.999, 1e-7 }
local function format_number(x)
  -- NaN's printed sign depends on the platform's formatter.
  if x ~= x then return "nan" end
  return string.format("%.17g", x)
end
for _, f in fns do
  local parts = {}
  for _, x in xs do parts[#parts + 1] = format_number(math[f](x)) end
  print(f, table.concat(parts, " "))
end
local p = {}
for _, a in { {2, 0.5}, {1.1, 7.3}, {10, -2.2}, {0.3, 1e3}, {7, 1/3} } do p[#p + 1] = format_number(a[1] ^ a[2]) end
print("pow", table.concat(p, " "))
local q = {}
for _, a in { {1, 2}, {-1, -2}, {0.3, -0.7}, {5, 0.01} } do q[#q + 1] = format_number(math.atan2(a[1], a[2])) end
print("atan2", table.concat(q, " "))
