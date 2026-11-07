-- fib: naive recursion (call-heavy)
local NAME = "fib"
local N = FIB_N or 30
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
local function run() return string.format("%.0f", fib(N)) end
