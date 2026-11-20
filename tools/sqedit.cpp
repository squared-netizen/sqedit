// SPDX-License-Identifier: MIT
//
// sqedit: a text tool built on parsing expression grammars, to replace sed
// and grep in scripts. One command a job:
//
//   sqedit match PATTERN [FILE...]            lines that match (grep)
//   sqedit pick PATTERN [FILE...]             the captures of every match
//   sqedit lines RANGE [FILE...]              a range of lines (2..29, 2.."set -euo")
//   sqedit sub PATTERN REPLACEMENT [FILE...]  every match replaced
//   sqedit wrap PATTERN BEFORE AFTER [FILE...] a line (or --lines N) wrapped
//   sqedit json PATH [FILE]                   a value from JSON, by a dotted path
//
// A command that changes a file takes --expect N, the number of matches the
// script assumes: a different number fails it, with nothing written. Exit
// status as grep's: 0 matched (or changed as expected), 1 no match or the
// wrong count, 2 a usage, pattern or file error.

import std;
import sq.data;
import sqedit.peg;
import sqedit.grammars;

namespace {

using sqedit::peg::Match;
using sqedit::peg::Pattern;

constexpr std::string_view version = "0.1.0";

constexpr std::string_view usage_text = R"(usage: sqedit COMMAND ARGUMENTS [FILE...]

  match PATTERN [FILE...]              print the lines that match
      -o  only the matches   -c  count them   -q  print nothing
      -v  lines that do not  -n  line numbers
  pick PATTERN [FILE...]               print the captures of every match, one
                                       match a line, tab-separated (the match
                                       itself when the pattern captures nothing)
  lines RANGE [FILE...]                print lines: 2..29, 5, 3.., ..$,
                                       2.."set -euo" (to the first line after 2
                                       that starts with the pattern)
  sub PATTERN REPLACEMENT [FILE...]    replace every match; in REPLACEMENT,
                                       {name} is a named capture, {1}.. the
                                       captures in order, {0} the match, {{ }}
                                       braces, \n a newline, \t a tab, \\ a
                                       backslash
  wrap PATTERN BEFORE AFTER [FILE...]  put BEFORE above each matching line and
                                       AFTER below it (or below --lines N lines)
  json PATH [FILE]                     the value at a dotted path
                                       (consumers.sqterm.key, items.0): a string
                                       as itself, anything else as JSON

  -x           match, pick, sub: the pattern must match the whole line
               (or, with --whole, the whole file), as grep -x
  -F           the pattern is literal text
  -g GRAMMAR   rules a pattern may call: a .peg file, or one sqedit ships
               (c, ini, json)
  --whole      match across the whole file, not line by line
  --in-place   sub, wrap: change the files (each written beside, then renamed)
  --expect N   sub, wrap: fail, writing nothing, unless there are N matches
               in each file
  --lines N    wrap: wrap N lines from the matching one (default 1)
  --strict     a file that is not valid UTF-8 is an error

Patterns are parsing expression grammars, as LPeg's re module writes them:
"text" 'text' . [a-z] [^"] p q  p / q  p* p+ p?  &p !p  (p)  {p}  {:name: p :}
{~ p ~}  p -> 'text'  name <- p  %nl %s %d %w %a %l %u %x %p %c %g (%S ... not).
Exit status: 0 matched, 1 no match or not --expect, 2 an error.
)";

struct Failure {
    int status;
    std::string message;
};

[[noreturn]] void fail(std::string message, int status = 2) { throw Failure{status, std::move(message)}; }

struct Options {
    bool literal = false, whole = false, in_place = false, strict = false;
    bool only = false, count = false, quiet = false, invert = false, full_line = false, numbers = false;
    std::optional<std::size_t> expect;
    std::size_t wrap_lines = 1;
    std::vector<sqedit::peg::Grammar> grammars;
    std::vector<std::string> args;  ///< positional, after the command
};

std::size_t number(std::string_view option, std::string_view text) {
    std::size_t n = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (ec != std::errc() || end != text.data() + text.size()) fail(std::format("{} takes a number, not '{}'", option, text));
    return n;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail(std::format("{}: cannot read it", path));
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string read_stdin() { return std::string(std::istreambuf_iterator<char>(std::cin), {}); }

sqedit::peg::Grammar grammar(const std::string& name) {
    std::string text;
    std::string shown = name;
    const auto shipped = std::ranges::find(sqedit::shipped_grammars(), std::string_view(name), &sqedit::ShippedGrammar::name);
    if (shipped != sqedit::shipped_grammars().end() && name.find('/') == std::string::npos && !name.ends_with(".peg")) {
        text = std::string(shipped->text);
        shown = name + ".peg";
    } else {
        text = read_file(name);
    }
    auto g = sqedit::peg::load_grammar(shown, std::move(text));
    if (!g) fail(describe(g.error()));
    return std::move(*g);
}

Options parse(std::span<const std::string_view> args) {
    Options o;
    bool options = true;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        const auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) fail(std::format("{} needs a value", a));
            return args[++i];
        };
        if (!options || a.size() < 2 || a[0] != '-') {
            o.args.emplace_back(a);
        } else if (a == "--") {
            options = false;
        } else if (a == "-F") {
            o.literal = true;
        } else if (a == "--whole") {
            o.whole = true;
        } else if (a == "--in-place") {
            o.in_place = true;
        } else if (a == "--strict") {
            o.strict = true;
        } else if (a == "--expect") {
            o.expect = number(a, value());
        } else if (a == "--lines") {
            o.wrap_lines = number(a, value());
            if (o.wrap_lines == 0) fail("--lines takes a number of 1 or more");
        } else if (a == "-g") {
            o.grammars.push_back(grammar(std::string(value())));
        } else if (a.size() >= 2 && a[0] == '-' && a[1] != '-') {
            for (const char c : a.substr(1)) {
                switch (c) {
                    case 'o': o.only = true; break;
                    case 'c': o.count = true; break;
                    case 'q': o.quiet = true; break;
                    case 'v': o.invert = true; break;
                    case 'x': o.full_line = true; break;
                    case 'n': o.numbers = true; break;
                    case 'F': o.literal = true; break;
                    default: fail(std::format("no option -{}", c));
                }
            }
        } else {
            fail(std::format("no option {}", a));
        }
    }
    return o;
}

Pattern compile(const Options& o, std::string_view source, bool to_end = false) {
    if (o.literal) return Pattern::literal(source, {.to_end = to_end});
    auto p = Pattern::compile(source, o.grammars, {.to_end = to_end});
    if (!p) fail(describe(p.error()));
    return std::move(*p);
}

std::optional<Match> find(const Pattern& p, std::string_view s, std::size_t from) {
    auto m = p.find(s, from);
    if (!m) fail(describe(m.error()));
    return std::move(*m);
}

std::optional<Match> match_at(const Pattern& p, std::string_view s, std::size_t at) {
    auto m = p.match(s, at);
    if (!m) fail(describe(m.error()));
    return std::move(*m);
}

/// Every match in `s`, one after another; an empty match moves on a
/// character, so none is found twice.
std::vector<Match> all_matches(const Pattern& p, std::string_view s) {
    std::vector<Match> out;
    std::size_t at = 0;
    while (at <= s.size()) {
        auto m = find(p, s, at);
        if (!m) break;
        at = m->end > m->begin ? m->end : m->end + (m->end < s.size() ? sqedit::peg::decode(s, m->end).second : 1);
        out.push_back(std::move(*m));
    }
    return out;
}

/// The matches a command acts on: every one, or with -x the one that is the
/// whole subject (the pattern compiled to reach its end).
std::vector<Match> matches(const Options& o, const Pattern& p, std::string_view s) {
    if (!o.full_line) return all_matches(p, s);
    std::vector<Match> out;
    if (auto m = match_at(p, s, 0)) out.push_back(std::move(*m));
    return out;
}

/// A file split into lines, without their newlines; whether the last had one.
struct Lines {
    std::vector<std::string_view> lines;
    bool final_newline = false;
};

Lines split(std::string_view text) {
    Lines l;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) {
            l.lines.push_back(text.substr(at));
            return l;
        }
        l.lines.push_back(text.substr(at, nl - at));
        at = nl + 1;
    }
    l.final_newline = !text.empty();
    return l;
}

std::string join(const std::vector<std::string>& lines, bool final_newline) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || final_newline) out += '\n';
    }
    return out;
}

bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto [cp, n] = sqedit::peg::decode(s, i);
        if (cp == sqedit::peg::invalid_cp) return false;
        i += n;
    }
    return true;
}

/// The inputs: each file named, or standard input.
struct Input {
    std::string name;
    std::string text;
};

std::vector<Input> inputs(const Options& o, std::span<const std::string> files) {
    std::vector<Input> in;
    if (files.empty()) {
        if (o.in_place) fail("--in-place needs a file to change");
        in.push_back({"(standard input)", read_stdin()});
    }
    for (const std::string& f : files) in.push_back({f, read_file(f)});
    if (o.strict) {
        for (const Input& i : in) {
            if (!valid_utf8(i.text)) fail(std::format("{}: not valid UTF-8 (--strict)", i.name));
        }
    }
    return in;
}

void out(std::string_view s) { std::cout.write(s.data(), static_cast<std::streamsize>(s.size())); }

/// A REPLACEMENT with its captures filled in.
std::string replaced(std::string_view text, std::string_view subject, const Match& m) {
    std::string r;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {
            r += c;
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'n' || text[i + 1] == 't' || text[i + 1] == '\\')) {
            r += text[i + 1] == 'n' ? '\n' : text[i + 1] == 't' ? '\t' : '\\';
            ++i;
            continue;
        }
        if (c == '}') fail("a '}' in the replacement that closes nothing (write }} for one)");
        if (c != '{') {
            r += c;
            continue;
        }
        const std::size_t close = text.find('}', i);
        if (close == std::string_view::npos) fail("a '{' in the replacement that is not closed (write {{ for one)");
        const std::string_view name = text.substr(i + 1, close - i - 1);
        i = close;
        if (!name.empty() && std::ranges::all_of(name, [](char d) { return d >= '0' && d <= '9'; })) {
            const std::size_t n = number("the replacement", name);
            if (n == 0) {
                r += subject.substr(m.begin, m.end - m.begin);
            } else if (n <= m.captures.size()) {
                r += m.captures[n - 1].value;
            } else {
                fail(std::format("the replacement asks for capture {{{}}}; the pattern has {}", n, m.captures.size()));
            }
            continue;
        }
        const auto* cap = m.named(name);
        if (!cap) fail(std::format("the replacement asks for {{{}}}; the pattern has no capture of that name", name));
        r += cap->value;
    }
    return r;
}

/// Writes `text` over `path`: beside it first, then renamed over it.
void write_over(const std::string& path, const std::string& text) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto perms = fs::status(path, ec).permissions();
    const std::string part = path + ".sqedit-part";
    {
        std::ofstream o(part, std::ios::binary | std::ios::trunc);
        o.write(text.data(), static_cast<std::streamsize>(text.size()));
        o.flush();
        if (!o) {
            fs::remove(part, ec);
            fail(std::format("{}: cannot write it", path));
        }
    }
    if (!ec) fs::permissions(part, perms, ec);
    fs::rename(part, path, ec);
    if (ec) {
        fs::remove(part, ec);
        fail(std::format("{}: cannot replace it", path));
    }
}

/// A changed text for each input, its count checked against --expect;
/// then written (--in-place) or printed. Nothing is written when any
/// file's count is not what was expected.
int finish(const Options& o, const std::vector<Input>& in, const std::vector<std::string>& changed,
           const std::vector<std::size_t>& counts) {
    bool short_of = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (o.expect && counts[i] != *o.expect) {
            std::cerr << std::format("sqedit: {}: {} match{}, expected {}\n", in[i].name, counts[i], counts[i] == 1 ? "" : "es", *o.expect);
            short_of = true;
        }
    }
    if (short_of) return 1;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (o.in_place) {
            if (changed[i] != in[i].text) write_over(in[i].name, changed[i]);
        } else {
            out(changed[i]);
        }
    }
    return std::ranges::any_of(counts, [](std::size_t c) { return c > 0; }) ? 0 : 1;
}

// --- the commands -----------------------------------------------------------------

int cmd_match(const Options& o) {
    if (o.args.empty()) fail("match needs a PATTERN");
    const Pattern p = compile(o, o.args[0], o.full_line);
    const auto in = inputs(o, std::span(o.args).subspan(1));
    const bool names = in.size() > 1;
    std::size_t total = 0;
    for (const Input& f : in) {
        std::size_t count = 0;
        const auto prefix = [&](std::size_t line) {
            std::string s;
            if (names) s += f.name + ":";
            if (o.numbers) s += std::format("{}:", line);
            return s;
        };
        if (o.whole) {
            for (const Match& m : o.full_line ? std::vector<Match>{} : all_matches(p, f.text)) {
                ++count;
                if (!o.quiet && !o.count) out(std::format("{}{}\n", names ? f.name + ":" : "", std::string_view(f.text).substr(m.begin, m.end - m.begin)));
            }
            if (o.full_line) {
                if (auto m = match_at(p, f.text, 0)) {
                    ++count;
                    if (!o.quiet && !o.count) out(f.text.ends_with('\n') ? f.text : f.text + "\n");
                }
            }
        } else {
            const Lines lines = split(f.text);
            for (std::size_t n = 0; n < lines.lines.size(); ++n) {
                const std::string_view line = lines.lines[n];
                if (o.only && !o.invert) {
                    for (const Match& m : o.full_line ? std::vector<Match>{} : all_matches(p, line)) {
                        ++count;
                        if (!o.quiet && !o.count) out(std::format("{}{}\n", prefix(n + 1), line.substr(m.begin, m.end - m.begin)));
                    }
                    if (o.full_line && match_at(p, line, 0)) {
                        ++count;
                        if (!o.quiet && !o.count) out(std::format("{}{}\n", prefix(n + 1), line));
                    }
                    continue;
                }
                const bool hit = (o.full_line ? match_at(p, line, 0) : find(p, line, 0)).has_value();
                if (hit == o.invert) continue;
                ++count;
                if (!o.quiet && !o.count) out(std::format("{}{}\n", prefix(n + 1), line));
            }
        }
        if (o.count && !o.quiet) out(names ? std::format("{}:{}\n", f.name, count) : std::format("{}\n", count));
        total += count;
    }
    return total > 0 ? 0 : 1;
}

int cmd_pick(const Options& o) {
    if (o.args.empty()) fail("pick needs a PATTERN");
    const Pattern p = compile(o, o.args[0], o.full_line);
    const auto in = inputs(o, std::span(o.args).subspan(1));
    std::size_t total = 0;
    const auto print = [&](std::string_view s, const Match& m) {
        ++total;
        if (m.captures.empty()) {
            out(s.substr(m.begin, m.end - m.begin));
        } else {
            for (std::size_t i = 0; i < m.captures.size(); ++i) {
                if (i) out("\t");
                out(m.captures[i].value);
            }
        }
        out("\n");
    };
    for (const Input& f : in) {
        if (o.whole) {
            for (const Match& m : matches(o, p, f.text)) print(f.text, m);
        } else {
            for (const std::string_view line : split(f.text).lines) {
                for (const Match& m : matches(o, p, line)) print(line, m);
            }
        }
    }
    return total > 0 ? 0 : 1;
}

/// One end of a RANGE: a line number, $ (the last), or a pattern the line
/// starts with.
struct Bound {
    std::optional<std::size_t> number;
    bool last = false;
    std::optional<Pattern> pattern;
};

Bound bound(const Options& o, std::string_view text) {
    Bound b;
    if (text == "$") {
        b.last = true;
    } else if (!text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        b.number = number("a range", text);
        if (*b.number == 0) fail("lines are numbered from 1");
    } else {
        b.pattern = compile(o, text);
    }
    return b;
}

int cmd_lines(const Options& o) {
    if (o.args.empty()) fail("lines needs a RANGE: 2..29, 5, 3.., ..$, 2..\"set -euo\"");
    const std::string_view range = o.args[0];
    const std::size_t dots = range.find("..");
    const bool span_given = dots != std::string_view::npos;
    const std::string_view from_text = span_given ? range.substr(0, dots) : range;
    const std::string_view to_text = span_given ? range.substr(dots + 2) : range;
    if (!span_given && from_text.empty()) fail("lines needs a RANGE");
    const std::optional<Bound> from = from_text.empty() ? std::nullopt : std::optional(bound(o, from_text));
    const std::optional<Bound> to = to_text.empty() ? std::nullopt : std::optional(bound(o, to_text));
    std::size_t printed = 0;
    for (const Input& f : inputs(o, std::span(o.args).subspan(1))) {
        const Lines lines = split(f.text);
        const std::size_t n = lines.lines.size();
        const auto starts = [&](const Pattern& p, std::size_t i) { return match_at(p, lines.lines[i], 0).has_value(); };
        // The first line: its number, or the first that starts with the pattern.
        std::size_t first = 0;
        if (from) {
            if (from->number) first = *from->number - 1;
            else if (from->last) first = n == 0 ? 0 : n - 1;
            else {
                first = n;
                for (std::size_t i = 0; i < n; ++i) {
                    if (starts(*from->pattern, i)) {
                        first = i;
                        break;
                    }
                }
            }
        }
        if (first >= n) continue;
        // The last: its number, $, or the first line after the first that starts with the pattern.
        std::size_t last = n - 1;
        if (to) {
            if (to->number) last = std::min(*to->number - 1, n - 1);
            else if (to->pattern) {
                const std::size_t search = span_given ? first + 1 : first;
                last = n - 1;
                for (std::size_t i = search; i < n; ++i) {
                    if (starts(*to->pattern, i)) {
                        last = i;
                        break;
                    }
                }
            }
        }
        if (!span_given) last = first;
        for (std::size_t i = first; i <= last && i < n; ++i) {
            out(lines.lines[i]);
            out("\n");
            ++printed;
        }
    }
    return printed > 0 ? 0 : 1;
}

int cmd_sub(const Options& o) {
    if (o.args.size() < 2) fail("sub needs a PATTERN and a REPLACEMENT");
    const Pattern p = compile(o, o.args[0], o.full_line);
    const std::string_view replacement = o.args[1];
    const auto in = inputs(o, std::span(o.args).subspan(2));
    std::vector<std::string> changed;
    std::vector<std::size_t> counts;
    const auto substitute = [&](std::string_view s, std::size_t& count) {
        std::string r;
        std::size_t at = 0;
        for (const Match& m : matches(o, p, s)) {
            r += s.substr(at, m.begin - at);
            r += replaced(replacement, s, m);
            at = m.end;
            ++count;
        }
        r += s.substr(at);
        return r;
    };
    for (const Input& f : in) {
        std::size_t count = 0;
        if (o.whole) {
            changed.push_back(substitute(f.text, count));
        } else {
            const Lines lines = split(f.text);
            std::vector<std::string> result;
            for (const std::string_view line : lines.lines) result.push_back(substitute(line, count));
            changed.push_back(join(result, lines.final_newline));
        }
        counts.push_back(count);
    }
    return finish(o, in, changed, counts);
}

int cmd_wrap(const Options& o) {
    if (o.args.size() < 3) fail("wrap needs a PATTERN, the line to put BEFORE and the line to put AFTER");
    if (o.whole) fail("wrap works on lines: --whole does not apply");
    const Pattern p = compile(o, o.args[0]);
    const std::string& before = o.args[1];
    const std::string& after = o.args[2];
    const auto in = inputs(o, std::span(o.args).subspan(3));
    std::vector<std::string> changed;
    std::vector<std::size_t> counts;
    for (const Input& f : in) {
        const Lines lines = split(f.text);
        std::vector<std::string> result;
        std::size_t count = 0;
        for (std::size_t i = 0; i < lines.lines.size();) {
            if (!find(p, lines.lines[i], 0)) {
                result.emplace_back(lines.lines[i++]);
                continue;
            }
            ++count;
            result.push_back(before);
            const std::size_t end = std::min(i + o.wrap_lines, lines.lines.size());
            for (; i < end; ++i) result.emplace_back(lines.lines[i]);
            result.push_back(after);
        }
        changed.push_back(join(result, lines.final_newline || (count > 0 && lines.lines.empty())));
        counts.push_back(count);
    }
    return finish(o, in, changed, counts);
}

int cmd_json(const Options& o) {
    if (o.args.empty()) fail("json needs a PATH: name, or name.name, or name.0");
    if (o.args.size() > 2) fail("json reads one file");
    const auto in = inputs(o, std::span(o.args).subspan(1));
    auto parsed = sq::data::parse_json(in[0].text);
    if (!parsed) fail(std::format("{}: not JSON: {}", in[0].name, parsed.error.message));
    const sq::data::JsonValue* v = &parsed.value;
    const std::string_view path = o.args[0];
    for (std::size_t at = 0; at <= path.size();) {
        std::size_t dot = path.find('.', at);
        if (dot == std::string_view::npos) dot = path.size();
        const std::string_view key = path.substr(at, dot - at);
        if (key.empty()) fail(std::format("'{}' is not a path: an empty name", path));
        if (v->object_if()) {
            v = v->find(key);
        } else if (const auto* list = v->array_if()) {
            std::size_t i = 0;
            const auto [end, ec] = std::from_chars(key.data(), key.data() + key.size(), i);
            v = ec == std::errc() && end == key.data() + key.size() && i < list->size() ? &(*list)[i] : nullptr;
        } else {
            v = nullptr;
        }
        if (!v) return 1;
        at = dot + 1;
    }
    if (const std::string* s = v->string_if()) {
        out(*s);
    } else {
        auto text = sq::data::write_json(*v);
        if (!text) fail(text.error.message);
        out(text.text);
    }
    out("\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        out(usage_text);
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "--version") {
        out(std::format("sqedit {}\n", version));
        return 0;
    }
    try {
        const std::string_view command = args[0];
        const Options o = parse(std::span(args).subspan(1));
        int status = 0;
        if (command == "match") status = cmd_match(o);
        else if (command == "pick") status = cmd_pick(o);
        else if (command == "lines") status = cmd_lines(o);
        else if (command == "sub") status = cmd_sub(o);
        else if (command == "wrap") status = cmd_wrap(o);
        else if (command == "json") status = cmd_json(o);
        else fail(std::format("no command '{}' (match, pick, lines, sub, wrap, json; sqedit --help)", command));
        std::cout.flush();
        return status;
    } catch (const Failure& f) {
        std::cout.flush();
        std::cerr << "sqedit: " << f.message << "\n";
        return f.status;
    }
}
