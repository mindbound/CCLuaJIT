-- Syntax probe: LuaJIT v3.0 syntax extensions (backported upstream in a2ce8114, 0218af7e, c41d727d) must be
-- REJECTED by load(); Lua 5.2 / Cobalt constructs must be ACCEPTED.
-- usage: luajit syntax_probe.lua   -> one line per case; final line "SUMMARY ok=<n> bad=<n>"
local reject = {
  {"bit and &",            "local a = 1 & 2"},
  {"bit or |",             "local a = 1 | 2"},
  {"bit xor ~",            "local a = 1 ~ 2"},
  {"bit not ~x",           "local a = ~1"},
  {"shift <<",             "local a = 1 << 2"},
  {"shift >>",             "local a = 8 >> 1"},
  {"arith shift ~>>",      "local a = 8 ~>> 1"},
  {"!= operator",          "local a = 1 != 2"},
  {"! operator",           "local a = !true"},
  {"&& operator",          "local a = true && false"},
  {"|| operator",          "local a = true || false"},
  {"ternary ?:",           "local a = true ? 1 : 2"},
  {"safe navigation ?.",   "local t; local a = t?.x"},
  {"nil-coalescing ??",    "local a = nil ?? 1"},
  {"+=",                   "local a = 1; a += 1"},
  {"-=",                   "local a = 1; a -= 1"},
  {"*=",                   "local a = 1; a *= 2"},
  {"/=",                   "local a = 1; a /= 2"},
  {"%=",                   "local a = 1; a %= 2"},
  {"&=",                   "local a = 1; a &= 2"},
  {"|=",                   "local a = 1; a |= 2"},
  {"~= as statement (bxor-assign)", "local a = 1; a ~= 2"},
  {"<<=",                  "local a = 1; a <<= 2"},
  {">>=",                  "local a = 1; a >>= 2"},
  {"~>>=",                 "local a = 1; a ~>>= 2"},
  {"..=",                  "local a = 'x'; a ..= 'y'"},
  {"continue statement",   "for i = 1, 2 do continue end"},
  {"const declaration",    "const x = 1"},
  {"short function x->",   "local f = x -> x + 1"},
  {"short function |a,b|->", "local f = |a, b| -> a + b"},
  {"short function ||->",  "local f = || -> 1"},
  {"underscore number",    "local a = 1_000"},
  {"semicolon after return", "do return;; end"},
  {"line directive #10 (line 2)",     "local a = 1\n#10\nreturn a"},
  {"line directive #10 \"f\" (line 2)", "local a = 1\n#10 \"file.lua\"\nreturn a"},
}
-- Informational only (not counted): LuaJIT's lexer skips a first line that starts with '#' in every chunk,
-- including load() of a string; PUC Lua 5.2 skips it only in luaL_loadfile. Pre-existing, unrelated to v3.0.
local info = {
  {"first-line '#' skipped by load()", "#10\nreturn 1"},
}
local accept = {
  {"~= comparison",          "local a = 1 ~= 2"},
  {"goto as field name (patch 06)", "local t = {}; t.goto = 1; return t.goto"},
  {"goto statement",         "do goto l; ::l:: end"},
  {"continue as variable",   "local continue = 1; continue = continue + 1"},
  {"continue as function",   "local function continue() end; continue()"},
  {"const as variable",      "local const = 1; const = 2"},
  {"empty statements (5.2)", "local a = 1;; ; local b = 2"},
  {"return with one ;",      "do return; end"},
  {"long string / comments", "--[[ x ]] local s = [==[ a ]==]"},
  {"nested function",        "local f = function(x) return x end"},
}
local ok, bad = 0, 0
local function run(list, want_ok)
  for _, c in ipairs(list) do
    local f, err = load(c[2], "=probe")
    local got_ok = f ~= nil
    local pass = (got_ok == want_ok)
    if pass then ok = ok + 1 else bad = bad + 1 end
    print(string.format("%-4s %-8s %-32s %s", pass and "ok" or "BAD", got_ok and "ACCEPTED" or "REJECTED",
      c[1], got_ok and "" or tostring(err):gsub("\n", " ")))
  end
end
print("-- must be REJECTED (v3.0 syntax):"); run(reject, false)
print("-- must be ACCEPTED (Lua 5.2 / Cobalt):"); run(accept, true)
for _, c in ipairs(info) do
  local f = load(c[2], "=probe")
  print(string.format("info %-8s %s", f and "ACCEPTED" or "REJECTED", c[1]))
end
print(string.format("SUMMARY ok=%d bad=%d (%s)", ok, bad, jit and jit.version or _VERSION))
