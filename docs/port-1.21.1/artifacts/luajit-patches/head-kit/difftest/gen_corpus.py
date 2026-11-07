"""Generate the differential-parse corpus as pack files for parsediff.lua.

usage: python -I gen_corpus.py <rom-dir> <out-dir> [seed]
Writes:
  hand.pack     handwritten cases: new.* = LuaJIT 3.0 syntax backported to v2.1 (must fail on both builds),
                l52.* = Lua 5.2-isms and old syntax (goto/labels, goto/continue/const as names, long
                strings, numbers, escapes, empty statements, '#' lines, limits, assorted syntax errors)
  rom.pack      bios.lua + every rom/**/*.lua listed in <rom-dir>/MANIFEST.txt
  soup.pack     random token soup (vocabulary includes every new operator/keyword)
  gram.pack     random Lua 5.2 programs from a small grammar, each with 0-2 new-syntax mutations
  rommut.pack   CC:T ROM files with one or two random new-syntax insertions/replacements
"""
import os
import random
import sys

rom_dir, out_dir = sys.argv[1], sys.argv[2]
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 20261107
rnd = random.Random(seed)


def write_pack(name, cases):
    ids = set()
    with open(os.path.join(out_dir, name), "wb") as f:
        for cid, src in cases:
            assert " " not in cid and cid not in ids, cid
            ids.add(cid)
            b = src.encode("latin-1") if isinstance(src, str) else src
            f.write(b"@@CASE %s %d\n" % (cid.encode(), len(b)))
            f.write(b)
            f.write(b"\n")
    print("%s: %d cases" % (name, len(cases)))


# ---------------------------------------------------------------- (a) new constructs
NEW = [
    # bit operators
    ("band", "local a, b = 1, 2\nx = a & b"),
    ("bor", "x = a | b"),
    ("bxor", "x = a ~ b"),
    ("bnot", "x = ~a"),
    ("bnot_const", "x = ~0"),
    ("shl", "x = a << 2"),
    ("shr", "x = a >> 2"),
    ("sar", "x = a ~>> 2"),
    ("sar_malformed", "x = a ~> 2"),
    ("bit_const_fold", "x = 1 & 3"),
    ("bit_prec", "x = 1 + 2 << 3 & 4 | 5 ~ 6"),
    ("bit_in_call", "print(a & b, c)"),
    ("bit_in_table", "t = { a | b }"),
    ("bit_in_if", "if a & 1 == 1 then end"),
    ("bnot_in_return", "return ~x"),
    # customary operators
    ("not_bang", "x = !a"),
    ("not_bang_if", "if !a then end"),
    ("not_bang_while", "while !done do end"),
    ("andand", "x = a && b"),
    ("oror", "x = a || b"),
    ("ne_bang", "x = a != b"),
    ("ne_bang_if", "if a != b then end"),
    ("bang_bang", "x = !!a"),
    # ternary
    ("ternary", "x = a ? b : c"),
    ("ternary_nested", "x = a ? b ? 1 : 2 : 3"),
    ("ternary_call_arg", "print(a ? 1 : 2)"),
    ("ternary_method", "x = a ? (o):m() : c"),
    ("ternary_missing_colon", "x = a ? b"),
    ("ternary_return", "return a ? b : c"),
    # safe navigation
    ("nav_field", "x = a?.b"),
    ("nav_index", "x = a?.[1]"),
    ("nav_call", "x = a?.(1)"),
    ("nav_method", "x = a:m?.()"),
    ("nav_stmt_call", "a?.b()"),
    ("nav_assign", "a?.b = 1"),
    ("nav_chain", "x = a?.b?.c?.d"),
    ("nav_multi", "x, y = a?.b()"),
    ("nav_return", "return a?.b"),
    # nil-coalescing
    ("coal", "x = a ?? b"),
    ("coal_chain", "x = a ?? b ?? c"),
    ("coal_return", "return a ?? 0"),
    # compound assignment
    ("c_add", "x += 1"),
    ("c_sub", "x -= 1"),
    ("c_mul", "x *= 2"),
    ("c_div", "x /= 2"),
    ("c_mod", "x %= 2"),
    ("c_band", "x &= 1"),
    ("c_bor", "x |= 1"),
    ("c_bxor", "x ~= 1"),
    ("c_shl", "x <<= 1"),
    ("c_shr", "x >>= 1"),
    ("c_sar", "x ~>>= 1"),
    ("c_concat", 'x ..= "a"'),
    ("c_pow_unsupported", "x ^= 2"),
    ("c_field", "t.x += 1"),
    ("c_index", "t[1] += 1"),
    ("c_local", "local x = 1\nx += 1"),
    ("c_upval", "local x = 1\nfunction f() x += 1 end"),
    ("c_space", "x + = 1"),
    ("c_in_loop", "for i = 1, 3 do s += i end"),
    # continue
    ("continue_for", "for i = 1, 3 do if i == 2 then continue end end"),
    ("continue_while", "while true do continue end"),
    ("continue_repeat", "repeat continue until true"),
    ("continue_forin", "for k in pairs({}) do continue end"),
    ("continue_semicolon", "for i = 1, 2 do continue; end"),
    ("continue_outside", "continue"),
    ("continue_scope", "repeat local x continue until x"),
    ("continue_eof_in_loop", "while x do\ncontinue\nend"),
    # const
    ("const_decl", "const x = 1"),
    ("const_multi", "const a, b = 1, 2"),
    ("const_func", "const function f() end"),
    ("const_assign", "const x = 1\nx = 2"),
    ("const_nil", "const x"),
    ("const_redecl", "const x = 1\nlocal x = 2"),
    ("const_in_fn", "function f() const y = 2 return y end"),
    # short function expression
    ("sf_bar", "f = |x| -> x"),
    ("sf_empty", "f = || -> 1"),
    ("sf_name", "f = x -> x + 1"),
    ("sf_two", "f = |a, b| -> a + b"),
    ("sf_do", "f = |x| -> do return x end"),
    ("sf_name_do", "f = x -> do end"),
    ("sf_call", "print((x -> x)(1))"),
    ("sf_table", "t = { f = x -> x }"),
    ("sf_arg", "table.sort(t, |a, b| -> a > b)"),
    ("sf_vararg", "f = |...| -> ..."),
    # underscores in number literals
    ("num_us", "x = 1_000"),
    ("num_us_hex", "x = 0x_ff"),
    ("num_us_hex2", "x = 0xff_ff"),
    ("num_us_0x", "x = 0_x10"),
    ("num_us_frac", "x = 1_000.000_1"),
    ("num_us_exp", "x = 1e1_0"),
    ("num_us_double", "x = 1__0"),
    ("num_us_trailing", "x = 1_"),
    ("num_us_dot", "x = .5_5"),
    ("num_us_zero", "x = 0_0"),
    ("num_us_zero_only", "x = 0_"),
    ("num_us_hexfloat", "x = 0x1_0p1_0"),
    # empty statements anywhere (the only change under LUA52COMPAT is after a final statement)
    ("semi_after_return", "return;;"),
    ("semi_after_return_fn", "function f() return 1;; end"),
    ("semi_after_return_val", "return 1;;;"),
    ("semi_after_continue", "for i = 1, 2 do continue;; end"),
    # line number directive
    ("linedir_first", '# 10 "foo.lua"\nx = = 1'),
    ("linedir_first_num", "#5\nx = = 1"),
    ("linedir_mid", "x = 1\n#100\ny = = 2"),
    ("linedir_mid_name", 'x = 1\n# 7 "bar"\ny = = 2'),
    ("linedir_mid_eqname", 'x = 1\n#7 "=baz"\ny = = 2'),
    ("linedir_malformed", "x = 1\n#5 junk\n"),
    ("linedir_len_number", "print(\n#5)"),
    ("linedir_runtime_line", "#42\nerror('boom')"),
    ("linedir_huge", "x = 1\n#99999999999\n"),
    ("linedir_tab", "x = 1\n#\t3\ny = = 1"),
    ("linedir_ok_only", "x = 1\n#12\ny = 2"),
]

# ---------------------------------------------------------------- (c) Lua 5.2-isms and old syntax
L52 = [
    # goto / labels
    ("goto_basic", "local i = 0\n::top:: i = i + 1\nif i < 3 then goto top end\nreturn i"),
    ("goto_forward", "goto done\nx = 1\n::done::"),
    ("goto_continue_idiom", "for i = 1, 3 do\n  if i == 2 then goto continue end\n  print(i)\n  ::continue::\nend"),
    ("goto_undefined", "goto nowhere"),
    ("goto_dup_label", "::a:: ::a::"),
    ("goto_into_scope", "goto f\nlocal x\n::f::\nprint(x)"),
    ("goto_scope_end_ok", "do goto f; local x; ::f:: end"),
    ("goto_nested_fn", "::a::\nfunction f() goto a end"),
    ("goto_eof", "goto"),
    ("goto_number", "goto 1"),
    ("goto_goto", "::goto:: goto goto"),
    ("goto_label_named_goto", "::goto::"),
    ("goto_label_continue", "::continue::"),
    ("goto_label_const", "goto const\n::const::"),
    ("goto_continue_target", "goto continue\n::continue::"),
    ("goto_break_label", "while true do goto out end ::out::"),
    ("goto_semicolons", "::a:: ;; goto b; ::b:: ;"),
    ("goto_label_end_of_loop", "for i = 1, 2 do goto e; local z = 1 ::e:: end"),
    ("goto_repeat_scope", "repeat goto x; local a ::x:: until a"),
    # goto as an ordinary name (patch 06)
    ("goto_field", "local t = {}\nfunction t.goto() end\nt.goto()\nt:goto()\nlocal x = t.goto"),
    ("goto_local", "local goto = 1\ngoto = goto + 1\nreturn goto"),
    ("goto_param", "function f(goto) return goto end"),
    ("goto_tablekey", "t = { goto = 1 }"),
    ("goto_call", "goto(1)"),
    ("goto_string_call", 'goto "x"'),
    ("goto_index", "goto[1] = 2"),
    ("goto_method", "goto:m()"),
    ("goto_assign_global", "goto = 5"),
    # continue / const as ordinary names
    ("continue_local", "local continue = 1"),
    ("continue_global", "continue = 2"),
    ("continue_field", "t = {}\nt.continue = 1\nprint(t.continue)"),
    ("continue_func", "function continue() end"),
    ("continue_localfunc", "local function continue() end"),
    ("continue_call", "continue()"),
    ("continue_string_call", 'continue "x"'),
    ("continue_method", "t:continue()"),
    ("continue_param", "function f(continue) return continue end"),
    ("continue_forvar", "for continue = 1, 2 do end"),
    ("continue_forin", "for continue in pairs({}) do end"),
    ("continue_key", "t = { continue = 1, const = 2 }"),
    ("continue_expr", "x = continue + 1"),
    ("continue_end", "for i = 1, 2 do continue end"),
    ("continue_semicolon", "continue;"),
    ("continue_index", "continue[1] = 1"),
    ("continue_upvalue", "local continue = 1\nfunction f() return continue end"),
    ("continue_dotted_fn", "function a.continue.b() end"),
    ("const_local", "local const = 1"),
    ("const_global", "const = 1"),
    ("const_call", "const(1)"),
    ("const_field", "x = t.const"),
    ("const_func", "function const() end"),
    ("const_index", "const[1] = 2"),
    ("const_method", "const:m()"),
    ("const_name_stmt", "const x = 1"),
    ("const_function_stmt", "const function f() end"),
    ("const_multi_assign", "const, continue = 1, 2"),
    # long strings / comments
    ("longstr", "x = [[a\nb]]"),
    ("longstr_level", "x = [==[ ]] ]=] ]==]"),
    ("longstr_leading_nl", "x = [[\nleading newline]]"),
    ("longstr_unfinished", "x = [[abc"),
    ("longstr_bad_delim", "x = [=x"),
    ("longstr_wrong_close", "x = [==[ a ]=]"),
    ("longcomment", "--[[ comment ]] x = 1"),
    ("longcomment_level", "--[==[ long\ncomment ]] ]==] x = 1"),
    ("longcomment_unfinished", "--[[ abc"),
    ("comment_arrow", "x = 1 -- -> !a & b"),
    ("comment_dashdash_gt", "x = 1 -->"),
    # numbers
    ("num_hexfloat", "x = 0x1p4"),
    ("num_hexfrac", "x = 0x.8"),
    ("num_hexfloat2", "x = 0xA.8p1"),
    ("num_big", "x = 1e308"),
    ("num_inf", "x = 1e309"),
    ("num_hex_big", "x = 0x7fffffffffffffff"),
    ("num_hex_huge", "x = 0xffffffffffffffffffff"),
    ("num_trailing_dot", "x = 3."),
    ("num_leading_dot", "x = .5"),
    ("num_e", "x = 1e"),
    ("num_e_plus", "x = 1e+"),
    ("num_0x", "x = 0x"),
    ("num_08", "x = 08"),
    ("num_dotdot", "x = 1..2"),
    ("num_exp_neg", "x = 1.5e-3"),
    ("num_upper_hex", "x = 0X1P-2"),
    ("num_ll", "x = 1LL"),
    ("num_imag", "x = 1i"),
    ("num_bin", "x = 0b101"),
    ("num_two_dots", "x = 1.2.3"),
    ("num_ee", "x = 1e5e5"),
    ("num_hex_e_plus", "x = 0x1e+5"),
    ("num_hex_p", "x = 0x1p"),
    ("num_dot_e", "x = 5.e3"),
    ("num_neg_zero", "x = -0"),
    ("num_concat_space", "x = 1 .. 2"),
    ("num_ident_suffix", "x = 3abc"),
    # strings / escapes
    ("str_z", 'x = "\\z\n   abc"'),
    ("str_hex", 'x = "\\x41"'),
    ("str_hex_bad", 'x = "\\xZZ"'),
    ("str_utf8", 'x = "\\u{48}"'),
    ("str_utf8_big", 'x = "\\u{7FFFFFFF}"'),
    ("str_utf8_bad", 'x = "\\u{110000000}"'),
    ("str_dec_big", 'x = "\\300"'),
    ("str_bad_escape", 'x = "\\q"'),
    ("str_unfinished", "x = 'abc"),
    ("str_newline", 'x = "a\nb"'),
    ("str_backslash_nl", 'x = "a\\\nb"'),
    ("str_bang_inside", 'x = "!a && b ?? c ?. d"'),
    # empty statements under LUA52COMPAT
    ("semi_lead", ";;x = 1;;"),
    ("semi_block", "do ;; end"),
    ("semi_break", "for i = 1, 2 do break;; end"),
    ("semi_only", ";"),
    ("semi_return_one", "return;"),
    ("semi_after_if", "if x then end;;;"),
    ("break_not_last", "while true do break x = 1 end"),
    # '#' at the start of the chunk / of a line
    ("hash_comment_first", "# just a comment\nx = 1"),
    ("hash_alone_first", "#\nx = 1"),
    ("hash_digit_first", "#5\nx = 1"),
    ("shebang", "#!/usr/bin/lua\nx = 1"),
    ("hash_len_line", "local t = {}\nlocal n =\n#t"),
    ("hash_len_number_line", "local n =\n#5"),
    ("hash_len_space_name", "local t = {}\nlocal n =\n# t"),
    ("bom_hash", "\xef\xbb\xbf# comment\nx = 1"),
    ("bom_code", "\xef\xbb\xbfx = 1"),
    # high bytes in identifiers
    ("highbyte_ident", "x\xc3\xa9 = 1"),
    # assorted syntax errors
    ("err_eq_eq", "x = = 1"),
    ("err_local_function", "local function end"),
    ("err_if_eof", "if x then"),
    ("err_call_eof", "f("),
    ("err_method_noargs", "a.b:c"),
    ("err_return_return", "return return"),
    ("err_local_number", "local 1 = 2"),
    ("err_close_brace", "x = }"),
    ("err_for", "for i = 1 do end"),
    ("err_for_multi", "for a, b = 1, 2 do end"),
    ("err_param_trailing", "function f(a,) end"),
    ("err_param_dots", "function f(..., a) end"),
    ("err_dots_outside", "function f() return ... end"),
    ("err_break_outside", "break"),
    ("err_paren_stmt", "(f)"),
    ("err_assign_call", "f() = 1"),
    ("err_assign_list_call", "a, b() = 1, 2"),
    ("err_attrib_54", "local a <const> = 1"),
    ("err_floor_div", "x = a // b"),
    ("err_len_eof", "x = #"),
    ("err_after_return", "return 1 x = 2"),
    ("err_end_end", "x = function() end end"),
    ("err_stray_elseif", "elseif"),
    ("err_stray_until", "until x"),
    ("err_label_open", "::a"),
    ("err_label_number", ":: 1 ::"),
    ("err_arrow_expr", "x = a -> b"),
    ("err_question_end", "x = a ?"),
    ("err_bang_alone", "!"),
    ("err_amp_alone", "&"),
    ("err_pipe_alone", "|"),
    ("err_question_alone", "?"),
    ("err_tilde_alone", "~"),
    ("err_dollar", "x = $"),
    ("err_backtick", "x = `a`"),
    ("err_at", "x = @a"),
    ("err_ne_stmt", "x ~= 1"),
    ("err_eq_stmt", "x == 1"),
    ("err_lt_lt_stmt", "x << 1"),
    ("err_call_then_op", "a:b() + 1"),
    ("err_nested_fn_line", "local f = function()\n  local x = \nend"),
    ("err_multiline", "local t = {\n  a = 1,\n  b = 2\n  c = 3\n}"),
    ("err_match_far", "if x then\n\n\n\nx = 1\n"),
    ("err_match_fn", "function f()\n\nreturn 1\n"),
    ("err_string_after_number", "x = 1'a'"),
    # line attribution of field/method chains split across lines
    ("chain_dot_newline", "local x = a.b\n  .c"),
    ("chain_dot_newline_global", "x = undefinedthing\n  .field"),
    ("chain_dot_newline_3", "x = a\n.b\n.c\n.d"),
    ("chain_colon_newline", "x = a:b()\n  :c()"),
    ("chain_mixed_newline", "x = a\n.b:c()\n.d[1]\n:e 'x'"),
    ("fname_dot_newline", "function a\n.b\n.c() end"),
    ("fname_colon_newline", "function a.b\n:c() end"),
    ("index_newline", "x = a\n[1]"),
    ("call_newline", "a\n(1)"),
    ("string_call_newline", 'a\n"s"'),
    ("table_call_newline", "a\n{}"),
    ("assign_chain_newline", "a\n.b\n.c = 1"),
    ("local_chain_newline", "local t = {}\nlocal v = t\n.x\n.y"),
    # limits
    ("limit_params_201", "local f = function(%s) end" % ", ".join("a%d" % i for i in range(201))),
    ("limit_params_201_stmt", "\n\nfunction g(%s) end" % ", ".join("a%d" % i for i in range(201))),
    ("limit_locals_201", "function h()\n%s\nend" % "\n".join("local v%d = %d" % (i, i) for i in range(201))),
    ("limit_locals_main_201", "\n".join("local v%d = %d" % (i, i) for i in range(201))),
    ("limit_upvalues_61", "%s\nfunction u() return %s end" % (
        "\n".join("local w%d = %d" % (i, i) for i in range(61)), " + ".join("w%d" % i for i in range(61)))),
    ("limit_parens_250", "x = " + "(" * 250 + "1" + ")" * 250),
    ("limit_nested_fn_250", "x = " + "function() return " * 250 + "1" + " end" * 250),
    ("limit_nested_tables_300", "x = " + "{" * 300 + "}" * 300),
    ("limit_concat_chain", "x = " + " .. ".join("'a'" for _ in range(300))),
]

hand = [("new." + n, s) for n, s in NEW] + [("l52." + n, s) for n, s in L52]
write_pack("hand.pack", hand)
with open(os.path.join(out_dir, "hand_new_ids.txt"), "w", newline="\n") as f:
    for n, _ in NEW:
        f.write("new.%s\n" % n)

# ---------------------------------------------------------------- (b) ROM
rom = []
with open(os.path.join(rom_dir, "MANIFEST.txt")) as m:
    rom_paths = [ln.strip() for ln in m if ln.strip()]
for rel in rom_paths:
    with open(os.path.join(rom_dir, *rel.split("/")), "rb") as f:
        rom.append(("rom:" + rel, f.read()))
write_pack("rom.pack", rom)

# ---------------------------------------------------------------- fuzz: token soup
NEWTOK = ["&", "|", "~", "<<", ">>", "~>>", "~>", "!", "&&", "||", "!=", "?", ":", "??", "?.", "->",
          "+=", "-=", "*=", "/=", "%=", "&=", "|=", "~=", "<<=", ">>=", "~>>=", "..=", "^=",
          "continue", "const", "1_000", "0x_f", "0_x1", "1e1_0", ";;", "\n#5\n", '\n# 3 "f"\n', "\n#\n", "#"]
OLDTOK = ["a", "b", "x", "t", "self", "_", "goto", "f", "local", "function", "end", "if", "then", "else",
          "elseif", "for", "in", "do", "while", "repeat", "until", "return", "break", "nil", "true", "false",
          "not", "and", "or", "1", "0x10", "1e5", ".5", "3.", "0x1p4", '"s"', "'s'", "[[l]]", "+", "-", "*",
          "/", "%", "^", "#", "==", "~=", "<=", ">=", "<", ">", "=", "(", ")", "{", "}", "[", "]", ";", ":",
          ",", ".", "..", "...", "::", "\n", "--c\n"]
SEPS = [" ", " ", " ", "", "\n", "\t"]
soup = []
for i in range(60000):
    n = rnd.randint(1, 14)
    toks = []
    for _ in range(n):
        toks.append(rnd.choice(NEWTOK) if rnd.random() < 0.3 else rnd.choice(OLDTOK))
    src = "".join(t + rnd.choice(SEPS) for t in toks)
    if rnd.random() < 0.15:
        src = rnd.choice(["#", "# ", "#!", "#1", "\n#"]) + src
    soup.append(("soup.%d" % i, src))
write_pack("soup.pack", soup)

# ---------------------------------------------------------------- fuzz: small grammar + mutations
NAMES = ["a", "b", "c", "x", "t", "goto", "continue", "const", "self"]


def g_name():
    return rnd.choice(NAMES)


def g_exp(d=0):
    r = rnd.random()
    if d > 3 or r < 0.3:
        return rnd.choice([g_name(), "1", "2.5", "0x1F", '"s"', "nil", "true", "...", "{}", "#t", "-1"])
    if r < 0.55:
        return "%s %s %s" % (g_exp(d + 1), rnd.choice(["+", "-", "*", "/", "%", "^", "..", "==", "~=", "<",
                                                      "<=", ">", ">=", "and", "or"]), g_exp(d + 1))
    if r < 0.65:
        return rnd.choice(["not ", "-", "#"]) + g_exp(d + 1)
    if r < 0.75:
        return "(%s)" % g_exp(d + 1)
    if r < 0.85:
        return "%s.%s" % (g_name(), g_name())
    if r < 0.9:
        return "%s(%s)" % (g_name(), g_exp(d + 1))
    if r < 0.95:
        return "{ %s = %s, %s }" % (g_name(), g_exp(d + 1), g_exp(d + 1))
    return "function(%s) return %s end" % (g_name(), g_exp(d + 1))


def g_block(d=0, inloop=False):
    return "\n".join(g_stmt(d, inloop) for _ in range(rnd.randint(0, 4)))


def g_stmt(d=0, inloop=False):
    r = rnd.random()
    if d > 2 or r < 0.25:
        return "%s = %s" % (g_name(), g_exp())
    if r < 0.35:
        return "local %s = %s" % (g_name(), g_exp())
    if r < 0.42:
        return "%s(%s)" % (g_name(), g_exp())
    if r < 0.5:
        return "if %s then\n%s\nelse\n%s\nend" % (g_exp(), g_block(d + 1, inloop), g_block(d + 1, inloop))
    if r < 0.58:
        return "for %s = 1, 3 do\n%s\nend" % (g_name(), g_block(d + 1, True))
    if r < 0.63:
        return "while %s do\n%s\nend" % (g_exp(), g_block(d + 1, True))
    if r < 0.67:
        return "repeat\n%s\nuntil %s" % (g_block(d + 1, True), g_exp())
    if r < 0.72:
        return "function %s.%s(%s)\n%s\nreturn %s\nend" % (g_name(), g_name(), g_name(), g_block(d + 1), g_exp())
    if r < 0.77:
        lab = g_name()
        return "goto %s\n::%s::" % (lab, lab)
    if r < 0.8 and inloop:
        return "break"
    if r < 0.85:
        return "do\n%s\nend" % g_block(d + 1, inloop)
    if r < 0.9:
        return "local function %s(%s)\n%s\nend" % (g_name(), g_name(), g_block(d + 1))
    return ";"


MUTS = ["&", "|", "~", "<<", ">>", "~>>", "!", "&&", "||", "!=", "?", "??", "?.", "->", "+=", "-=", "..=",
        "~=", "continue", "const", "1_0", ";;", "\n#5\n", "\n#\n", "goto", "::", ":", "|x| -> x", "x -> x",
        "? 1 : 2", "const y = 1", "continue\n", "end", "local", "="]
gram = []
for i in range(25000):
    src = g_block()
    for _ in range(rnd.choice([0, 1, 1, 2])):
        if not src:
            break
        p = rnd.randint(0, len(src))
        m = rnd.choice(MUTS)
        if rnd.random() < 0.5:
            src = src[:p] + " " + m + " " + src[p:]
        else:
            q = min(len(src), p + rnd.randint(1, 4))
            src = src[:p] + m + src[q:]
    gram.append(("gram.%d" % i, src))
write_pack("gram.pack", gram)

# ---------------------------------------------------------------- fuzz: ROM mutants
REPL = [(b"~=", b"!="), (b" and ", b" && "), (b" or ", b" || "), (b"not ", b"!"), (b"local ", b"const "),
        (b" = ", b" += "), (b" = ", b" ..= "), (b"break", b"continue"), (b".", b"?."), (b" == ", b" ?? "),
        (b"1", b"1_0"), (b"\n", b"\n#7\n"), (b"(", b"|x| -> ("), (b" + ", b" & "), (b" - ", b" ~ "),
        (b" * ", b" << ")]
rommut = []
for rel, data in rom:
    for k in range(30):
        src = data
        for _ in range(rnd.choice([1, 1, 2])):
            if rnd.random() < 0.6:
                old, new = rnd.choice(REPL)
                hits = []
                start = 0
                while True:
                    j = src.find(old, start)
                    if j < 0:
                        break
                    hits.append(j)
                    start = j + 1
                if hits:
                    j = rnd.choice(hits)
                    src = src[:j] + new + src[j + len(old):]
            else:
                p = rnd.randint(0, len(src))
                src = src[:p] + b" " + rnd.choice(MUTS).encode() + b" " + src[p:]
        rommut.append(("rommut:%s#%d" % (rel, k), src))
write_pack("rommut.pack", rommut)

# ---------------------------------------------------------------- line splitting (line info stress)
nlsplit = []
for rel, data in rom:
    nlsplit.append(("nl:all:" + rel, data.replace(b" ", b"\n")))
    for k in range(2):
        nlsplit.append(("nl:rnd%d:%s" % (k, rel), bytes((10 if (c == 32 and rnd.random() < 0.3) else c) for c in data)))
for i, (cid, src) in enumerate(gram):
    nlsplit.append(("nl:" + cid, src.replace(" ", "\n")))
write_pack("nlsplit.pack", nlsplit)
