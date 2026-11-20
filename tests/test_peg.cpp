// SPDX-License-Identifier: MIT
//
// sqedit.peg against LPeg's documented behaviour, and the design's gate:
// every pattern in the design parses, and a pattern that backtracks without
// end fails cleanly instead of hanging.

import std;
import sqedit.peg;
import sqedit.grammars;

namespace {

using namespace sqedit::peg;

int failures = 0;

void check(bool ok, std::string_view what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL " << what << "\n";
    }
}

Pattern pat(std::string_view source, std::span<const Grammar> g = {}, CompileOptions o = {}) {
    auto p = Pattern::compile(source, g, o);
    if (!p) {
        std::cerr << "does not compile: " << source << ": " << describe(p.error()) << "\n";
        std::exit(1);
    }
    return std::move(*p);
}

/// The text of the first match of `source` in `subject`, or "<none>".
std::string first(std::string_view source, std::string_view subject) {
    auto m = pat(source).find(subject);
    if (!m) return "<error: " + describe(m.error()) + ">";
    if (!*m) return "<none>";
    return std::string(subject.substr((*m)->begin, (*m)->end - (*m)->begin));
}

/// Whether `source` matches at the start of `subject`, and how far.
std::optional<std::size_t> at_start(std::string_view source, std::string_view subject) {
    auto m = pat(source).match(subject);
    if (!m || !*m) return std::nullopt;
    return (*m)->end;
}

std::vector<std::string> values(std::string_view source, std::string_view subject) {
    std::vector<std::string> out;
    auto m = pat(source).find(subject);
    if (m && *m) {
        for (const Capture& c : (*m)->captures) out.push_back(c.value);
    }
    return out;
}

std::string error_of(std::string_view source, std::span<const Grammar> g = {}) {
    auto p = Pattern::compile(source, g);
    return p ? std::string("<compiled>") : describe(p.error());
}

}  // namespace

int main() {
    // Literals: quoted text means only itself.
    check(first(R"("id = gethostid ();")", "x = 1; id = gethostid (); y") == "id = gethostid ();", "a literal with ( ) ; and spaces");
    check(first(R"('a.b')", "axb a.b") == "a.b", "a dot in a literal is a dot");
    check(first(R"("libc++: ")", "libc++: 23") == "libc++: ", "+ in a literal is a plus");

    // Sets, any, classes.
    check(first("[a-z_]+", "123 abc_d 9") == "abc_d", "a set with a range");
    check(first(R"([^"]+)", R"("abc")") == "abc", "a negated set");
    check(first("%d+", "v 42 x") == "42", "%d");
    check(first("%w+", "  hello_x") == "hello", "%w is alphanumeric (LPeg's)");
    check(first("%S+", "  ab  ") == "ab", "%S, the complement of %s");
    check(first("[%d_]+", "x 1_2 y") == "1_2", "a class inside a set");
    check(first(".", "é") == "é", ". is one code point");
    check(at_start("..", "é") == std::nullopt, "é is one character, not two");
    check(first("[à-ÿ]+", "voilà ça") == "à", "a set over code points");
    check(at_start(".", "\xff") == 1, "an invalid byte is one character");

    // Sequence, ordered choice, possessive repetition.
    check(at_start(R"("a" / "ab")", "ab") == 1, "the first choice that matches wins");
    check(at_start(R"("a"* "a")", "aaa") == std::nullopt, "repetition never gives back");
    check(at_start(R"(("ab" / "a")* "c")", "abac") == 4, "repetition of a choice");
    check(at_start(R"("x"?)", "y") == 0, "? matches nothing when it must");
    check(at_start(R"("x"+)", "y") == std::nullopt, "+ needs one");

    // Lookahead.
    check(at_start(R"(&"a" .)", "ab") == 1, "&p consumes nothing");
    check(at_start(R"(!"a" .)", "ab") == std::nullopt, "!p fails when p matches");
    check(first(R"((!"*/" .)* "*/")", "a /* b */ c") == "a /* b */", "!p in a loop");

    // Captures.
    check(values(R"("libc++: " {.*})", "libc++: 23.1.0") == std::vector<std::string>{"23.1.0"}, "pick a field");
    check(values(R"({%d+} "." {%d+})", "v1.25") == std::vector<std::string>{"1", "25"}, "two captures");
    {
        auto m = pat(R"({:major: %d+ :} "." {:minor: %d+})").find("v3.7");
        check(m && *m && (*m)->named("major") && (*m)->named("major")->value == "3" && (*m)->named("minor")->value == "7",
              "named captures, both closing forms");
    }
    check(values(R"({~ ("a" -> "b" / .)* ~})", "banana") == std::vector<std::string>{"bbnbnb"}, "a substitution capture");
    check(values(R"(%d+ -> "<%0>")", "x 12") == std::vector<std::string>{"<12>"}, "a replacement with %0");
    check(values(R"({ {"a"} "b" })", "ab") == std::vector<std::string>{"ab", "a"}, "nested captures, outer first");

    // Grammars.
    const auto balanced = pat(R"peg(
        -- a balanced parenthesised group
        group <- "(" ( [^()] / group )* ")"
    )peg");
    {
        auto m = balanced.find("f(a(b)c) d");
        check(m && *m && (*m)->begin == 1 && (*m)->end == 8, "a recursive rule matches balanced text");
    }
    check(error_of("x <- y") == "pattern:1:6: no rule named 'y' (a grammar defines it: -g)", "an undefined rule, with its place");
    check(error_of("a <- 'x' a <- 'y'").find("defined twice") != std::string::npos, "a rule defined twice");

    // Grammars from files, and the ones sqedit ships.
    for (const sqedit::ShippedGrammar& g : sqedit::shipped_grammars()) {
        auto loaded = load_grammar(std::string(g.name) + ".peg", std::string(g.text));
        check(loaded.has_value(), std::format("the shipped grammar {} parses", g.name));
    }
    {
        auto json = load_grammar("json.peg", std::string(sqedit::shipped_grammars()[2].text));
        const std::vector<Grammar> g{*json};
        auto p = pat(R"('"version":' %s* {string})", g);
        auto m = p.find(R"({"name": "x", "version": "1.2.3"})");
        check(m && *m && (*m)->captures.size() == 1 && (*m)->captures[0].value == "\"1.2.3\"", "a rule from json.peg");
        check(error_of(R"(nothing)", g).find("no rule named 'nothing'") != std::string::npos, "a rule a grammar lacks");
        auto own = pat(R"(string <- "s" )", g);  // the pattern's own rule wins
        auto mo = own.match("s");
        check(mo && *mo, "a pattern's rule replaces a grammar's of the same name");
    }
    {
        auto bad = load_grammar("bad.peg", "x <- 'a\n");
        check(!bad && describe(bad.error()) == "bad.peg:1:6: expected a closing quote", "a grammar's error names it");
    }

    // Errors, with where they are.
    check(error_of(R"("libc++: " {.*)") == "pattern:1:15: expected '}'", "an unclosed capture");
    check(error_of(R"(sub "id = gethostid ();)") == "pattern:1:5: expected a closing quote", "an unclosed quote");
    check(error_of("[a-") .find("expected ']'") != std::string::npos, "an unclosed set");
    check(error_of("%q") == "pattern:1:1: no class %q", "an unknown class");
    check(error_of(R"(("a"?)*)").find("can match nothing") != std::string::npos, "a loop that could match nothing");
    check(error_of(R"("a" ) )") == "pattern:1:5: unexpected ')'", "a stray parenthesis");

    // Bounded: a pattern that backtracks exponentially fails, it does not hang.
    {
        const auto p = pat(R"(s <- "a" s "b" / "a" s "c" / "a")");
        const std::string text(40, 'a');
        auto m = p.match(text + "d", 0, {.steps = 1'000'000});
        check(!m && m.error().message.find("too many steps") != std::string::npos, "exponential backtracking hits the step limit");
        auto left = pat("r <- r 'x' / 'x'").match("xxx");
        check(!left && left.error().message.find("too deep") != std::string::npos, "left recursion hits the depth limit");
    }

    // The whole line (-x), and searching past a pattern that may match nothing.
    check(!pat(R"("a" / "ab")", {}, {.to_end = true}).match("ab").value().has_value(),
          "to_end does not retry a committed choice: (\"a\" / \"ab\") !. fails on ab, as in LPeg");
    check(pat(R"("ab" / "a")", {}, {.to_end = true}).match("ab").value().has_value(), "to_end: the whole line");
    check(!pat(R"("a")", {}, {.to_end = true}).match("ab").value().has_value(), "to_end: not a prefix");
    check(first(R"(%s* "x")", "  x") == "  x", "a search does not skip a leading %s*");
    check(first(R"("x")", "abcx") == "x", "a search skips to the first byte");

    // Literal (-F).
    {
        auto m = Pattern::literal(R"(execve("/bin/sh", )").find(R"(r = execve("/bin/sh", argv);)");
        check(m && *m && (*m)->begin == 4, "a literal pattern");
    }

    // Every pattern in the design doc parses.
    for (const std::string_view source : {
             R"("libc++: " {.*})", R"("set -euo")", R"("id = gethostid ();")", R"x("    if (setgid(getgid()) != 0)")x",
             R"("class " %w+)", R"("text")", R"('text')", ".", "[a-z_]", R"([^"])", "%nl", "%s", "%d", "%w",
             R"({~ "a" -> 'new' ~})", "name <- 'x' name?", R"(&"a")", R"(!"a")"}) {
        check(Pattern::compile(source).has_value(), std::format("the design's pattern {} parses", source));
    }

    if (failures == 0) std::cout << "ok   peg\n";
    return failures == 0 ? 0 : 1;
}
