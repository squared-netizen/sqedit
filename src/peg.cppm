// SPDX-License-Identifier: MIT
//
// sqedit.peg: parsing expression grammars, as LPeg's `re` module writes
// them, compiled to a short list of instructions and run by a small
// backtracking machine (Medeiros and Ierusalimschy, "A Parsing Machine for
// PEGs", 2008). A rewrite in C++23, not a port.
//
//   "text" 'text'   exactly that text (no escapes: a literal is a literal)
//   .               one character: a UTF-8 code point (an invalid byte is one)
//   [a-z_] [^"]     one character in, or not in, a set; %classes inside too
//   p q             p, then q
//   p / q           p; if p fails, q (ordered: the first that matches wins)
//   p* p+ p?        repetition, greedy and possessive (never given back)
//   &p !p           p must, or must not, follow; nothing is consumed
//   (p)             grouping
//   {p}             capture what p matched
//   {:name: p :}    a named capture (`{:name: p}` too)
//   {~ p ~}         a substitution capture: the match, with every capture
//                   inside replaced by its value
//   p -> 'text'     a capture whose value is `text` (%0 in it: p's match)
//   name <- p       a rule; rules make a grammar, the first is where it starts
//   name, <name>    a call of a rule (the pattern's own, or a grammar's: -g)
//   %nl %s %d %w %a %l %u %x %p %c %g   newline, space, digit, alphanumeric,
//                   letter, lower, upper, hex digit, punctuation, control,
//                   graphic (ASCII, as LPeg's); %S %D ... their complements
//   -- comment      to the end of the line
//
// Matching allocates nothing past its two stacks, which grow only when a
// match needs them. A pattern that backtracks without end hits a step limit
// and fails with a message, as does recursion past a depth limit (a left-
// recursive rule), rather than hanging a build.

export module sqedit.peg;

import std;

export namespace sqedit::peg {

/// A pattern or grammar that does not compile, or a match that ran past a
/// limit. `source` is "pattern" or the grammar's name.
struct Error {
    std::string source = "pattern";
    std::size_t line = 0, column = 0;  ///< 0 when it is not about a place
    std::string message;
};

[[nodiscard]] inline std::string describe(const Error& e) {
    if (e.line == 0) return std::format("{}: {}", e.source, e.message);
    return std::format("{}:{}:{}: {}", e.source, e.line, e.column, e.message);
}

enum class CaptureKind : std::uint8_t { simple, named, substitution, replacement };

/// One capture of a match: where it matched, and its value (the text, the
/// substituted text, or the replacement).
struct Capture {
    CaptureKind kind = CaptureKind::simple;
    std::string name;
    std::size_t begin = 0, end = 0;
    std::string value;
};

/// A match: [begin, end) of the subject, and its captures in the order they
/// open (a capture inside a substitution or replacement is its, not here).
struct Match {
    std::size_t begin = 0, end = 0;
    std::vector<Capture> captures;

    [[nodiscard]] const Capture* named(std::string_view name) const {
        for (const Capture& c : captures) {
            if (c.kind == CaptureKind::named && c.name == name) return &c;
        }
        return nullptr;
    }
};

struct Limits {
    std::uint64_t steps = 50'000'000;  ///< instructions, for one match or search
    std::size_t depth = 100'000;       ///< backtrack entries and calls at once
};

/// Named rules, from a .peg file, that a pattern may call by name.
struct Grammar {
    std::string name;
    std::string text;
};

struct CompileOptions {
    /// The match must reach the end of the subject (grep -x): the pattern
    /// followed by !. (a choice inside it that already matched is not
    /// tried again: write the longer alternative first, as in any PEG).
    bool to_end = false;
};

}  // namespace sqedit::peg

namespace sqedit::peg {

export constexpr char32_t invalid_cp = 0x110000;

/// The code point at `at` and its length in bytes. A byte that does not
/// begin valid UTF-8 is one character, `invalid_cp`.
export [[nodiscard]] inline std::pair<char32_t, std::size_t> decode(std::string_view s, std::size_t at) noexcept {
    const auto b0 = static_cast<unsigned char>(s[at]);
    if (b0 < 0x80) return {b0, 1};
    std::size_t n = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        n = 2;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 3;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 4;
        cp = b0 & 0x07;
    } else {
        return {invalid_cp, 1};
    }
    if (at + n > s.size()) return {invalid_cp, 1};
    for (std::size_t i = 1; i < n; ++i) {
        const auto b = static_cast<unsigned char>(s[at + i]);
        if ((b & 0xC0) != 0x80) return {invalid_cp, 1};
        cp = (cp << 6) | (b & 0x3F);
    }
    constexpr char32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < least[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return {invalid_cp, 1};
    return {cp, n};
}

/// Whether `cp` is in an ASCII class named by its lower-case letter.
[[nodiscard]] inline bool in_class(char cls, char32_t cp) noexcept {
    if (cp >= 128) return false;
    const auto c = static_cast<char>(cp);
    const bool lower = c >= 'a' && c <= 'z', upper = c >= 'A' && c <= 'Z', digit = c >= '0' && c <= '9';
    switch (cls) {
        case 's': return c == ' ' || (c >= '\t' && c <= '\r');
        case 'd': return digit;
        case 'a': return lower || upper;
        case 'w': return lower || upper || digit;
        case 'l': return lower;
        case 'u': return upper;
        case 'x': return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        case 'c': return c < 0x20 || c == 0x7F;
        case 'g': return c > 0x20 && c < 0x7F;
        case 'p': return c > 0x20 && c < 0x7F && !lower && !upper && !digit;
        default: return false;
    }
}

/// A set of characters: ASCII as a bitmap, ranges above it, and classes.
struct CharSet {
    std::array<std::uint64_t, 2> ascii{};
    std::vector<std::pair<char32_t, char32_t>> ranges;
    std::vector<std::pair<char, bool>> classes;  ///< class letter, complemented
    bool negated = false;

    void add(char32_t lo, char32_t hi) {
        for (char32_t c = lo; c <= hi && c < 128; ++c) ascii[c >> 6] |= std::uint64_t{1} << (c & 63);
        if (hi >= 128) ranges.emplace_back(std::max<char32_t>(lo, 128), hi);
    }
    [[nodiscard]] bool has(char32_t cp) const noexcept {
        bool in = false;
        if (cp < 128) {
            in = (ascii[cp >> 6] >> (cp & 63)) & 1;
        } else if (cp != invalid_cp) {
            for (const auto& [lo, hi] : ranges) {
                if (cp >= lo && cp <= hi) {
                    in = true;
                    break;
                }
            }
        }
        if (!in) {
            for (const auto& [cls, complement] : classes) {
                if (in_class(cls, cp) != complement) {
                    in = true;
                    break;
                }
            }
        }
        return in != negated;
    }
};

/// Adds the class %name to `set`; false when there is no such class.
[[nodiscard]] inline bool add_class(CharSet& set, std::string_view name) {
    if (name == "nl") {
        set.add('\n', '\n');
        return true;
    }
    if (name.size() != 1) return false;
    const char c = name[0];
    const char lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (std::string_view("sdawluxcgp").find(lower) == std::string_view::npos) return false;
    set.classes.emplace_back(lower, c != lower);
    return true;
}

// --- the syntax tree ----------------------------------------------------------

enum class K : std::uint8_t {
    empty, literal, any, set, seq, choice, star, plus, opt, and_, not_,
    capture, named, subst, replace, call,
};

struct Node {
    K kind = K::empty;
    std::vector<std::uint32_t> kids;
    std::string text;         ///< literal, name, replacement, called rule
    std::uint32_t index = 0;  ///< set, or the called rule once resolved
    std::uint16_t source = 0;
    std::uint32_t line = 0, column = 0;
};

struct Rule {
    std::string name;
    std::uint32_t body = 0;
    std::uint16_t source = 0;
    bool from_pattern = false;
};

struct Tree {
    std::vector<Node> nodes;
    std::vector<CharSet> sets;
    std::vector<Rule> rules;
    std::vector<std::string> sources;  ///< "pattern", or a grammar's name
};

struct Thrown {
    Error error;
};

class Parser {
public:
    Parser(Tree& tree, std::string_view text, std::uint16_t source, bool from_pattern)
        : tree_(tree), s_(text), source_(source), from_pattern_(from_pattern) {}

    /// A pattern: an expression (or a grammar), and nothing after it.
    std::uint32_t pattern() {
        const std::uint32_t e = expression();
        skip();
        if (p_ < s_.size()) fail(p_, std::format("unexpected '{}'", shown(p_)));
        return e;
    }

    /// A grammar file: definitions only.
    void definitions() {
        skip();
        if (p_ == s_.size()) fail(p_, "no rules");
        while (true) {
            skip();
            if (p_ == s_.size()) return;
            if (!at_definition()) fail(p_, "expected a rule: name <- pattern");
            definition();
        }
    }

private:
    [[noreturn]] void fail(std::size_t at, std::string message) const {
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < at && i < s_.size(); ++i) {
            if (s_[i] == '\n') {
                ++line;
                column = 1;
            } else if ((static_cast<unsigned char>(s_[i]) & 0xC0) != 0x80) {
                ++column;
            }
        }
        throw Thrown{Error{tree_.sources[source_], line, column, std::move(message)}};
    }

    [[nodiscard]] std::string shown(std::size_t at) const {
        return std::string(s_.substr(at, decode(s_, at).second));
    }

    std::uint32_t add(Node n, std::size_t at) {
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < at && i < s_.size(); ++i) {
            if (s_[i] == '\n') {
                ++line;
                column = 1;
            } else if ((static_cast<unsigned char>(s_[i]) & 0xC0) != 0x80) {
                ++column;
            }
        }
        n.source = source_;
        n.line = static_cast<std::uint32_t>(line);
        n.column = static_cast<std::uint32_t>(column);
        tree_.nodes.push_back(std::move(n));
        return static_cast<std::uint32_t>(tree_.nodes.size() - 1);
    }

    void skip() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p_;
            } else if (s_.substr(p_, 2) == "--") {
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] static bool name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    [[nodiscard]] static bool name_char(char c) { return name_start(c) || (c >= '0' && c <= '9'); }

    std::string name() {
        if (p_ >= s_.size() || !name_start(s_[p_])) fail(p_, "expected a name");
        const std::size_t from = p_;
        while (p_ < s_.size() && name_char(s_[p_])) ++p_;
        return std::string(s_.substr(from, p_ - from));
    }

    /// A class's name, after its %: nl, or one letter.
    std::string class_name() {
        if (s_.substr(p_, 2) == "nl") {
            p_ += 2;
            return "nl";
        }
        if (p_ >= s_.size() || !name_start(s_[p_]) || s_[p_] == '_') fail(p_, "expected a class name after %");
        return std::string(1, s_[p_++]);
    }

    [[nodiscard]] bool at_definition() {
        const std::size_t keep = p_;
        bool yes = false;
        if (p_ < s_.size() && name_start(s_[p_])) {
            while (p_ < s_.size() && name_char(s_[p_])) ++p_;
            skip();
            yes = s_.substr(p_, 2) == "<-";
        }
        p_ = keep;
        return yes;
    }

    void expect(std::string_view what) {
        skip();
        if (s_.substr(p_, what.size()) != what) {
            fail(p_, p_ < s_.size() ? std::format("expected '{}', not '{}'", what, shown(p_)) : std::format("expected '{}'", what));
        }
        p_ += what.size();
    }

    std::string definition() {
        const std::size_t at = p_;
        std::string n = name();
        expect("<-");
        const std::uint32_t body = alternative();
        for (Rule& r : tree_.rules) {
            if (r.name != n) continue;
            if (r.source == source_) fail(at, std::format("the rule '{}' is defined twice", n));
            if (from_pattern_ && !r.from_pattern) {  // the pattern's own rule wins over a grammar's
                r = Rule{n, body, source_, true};
                return n;
            }
            fail(at, std::format("the rule '{}' is also defined by {}", n, tree_.sources[r.source]));
        }
        tree_.rules.push_back(Rule{n, body, source_, from_pattern_});
        return n;
    }

    std::uint32_t expression() {
        skip();
        if (!at_definition()) return alternative();
        const std::size_t at = p_;
        std::string start;  // the first rule this grammar defines is where it starts
        while (true) {
            skip();
            if (!at_definition()) break;
            std::string n = definition();
            if (start.empty()) start = std::move(n);
        }
        return add(Node{.kind = K::call, .text = std::move(start)}, at);
    }

    std::uint32_t alternative() {
        const std::size_t at = p_;
        std::vector<std::uint32_t> kids{sequence()};
        while (true) {
            skip();
            if (p_ < s_.size() && s_[p_] == '/') {
                ++p_;
                kids.push_back(sequence());
            } else {
                break;
            }
        }
        if (kids.size() == 1) return kids[0];
        return add(Node{.kind = K::choice, .kids = std::move(kids)}, at);
    }

    std::uint32_t sequence() {
        skip();
        const std::size_t at = p_;
        std::vector<std::uint32_t> kids;
        while (true) {
            skip();
            if (p_ >= s_.size()) break;
            const char c = s_[p_];
            if (c == ')' || c == '/' || c == '}' || s_.substr(p_, 2) == ":}" || s_.substr(p_, 2) == "~}") break;
            if (at_definition()) break;
            kids.push_back(prefix());
        }
        if (kids.empty()) return add(Node{.kind = K::empty}, at);
        if (kids.size() == 1) return kids[0];
        return add(Node{.kind = K::seq, .kids = std::move(kids)}, at);
    }

    std::uint32_t prefix() {
        skip();
        const std::size_t at = p_;
        if (p_ < s_.size() && (s_[p_] == '&' || s_[p_] == '!')) {
            const K k = s_[p_] == '&' ? K::and_ : K::not_;
            ++p_;
            const std::uint32_t kid = prefix();
            return add(Node{.kind = k, .kids = {kid}}, at);
        }
        return suffix();
    }

    std::uint32_t suffix() {
        const std::size_t at = p_;
        std::uint32_t e = primary();
        while (true) {
            skip();
            if (p_ >= s_.size()) break;
            const char c = s_[p_];
            if (c == '*' || c == '+' || c == '?') {
                ++p_;
                e = add(Node{.kind = c == '*' ? K::star : c == '+' ? K::plus : K::opt, .kids = {e}}, at);
            } else if (s_.substr(p_, 2) == "->") {
                p_ += 2;
                skip();
                if (p_ >= s_.size() || (s_[p_] != '\'' && s_[p_] != '"')) fail(p_, "expected a quoted replacement after '->'");
                std::string text = quoted();
                e = add(Node{.kind = K::replace, .kids = {e}, .text = std::move(text)}, at);
            } else {
                break;
            }
        }
        return e;
    }

    std::string quoted() {
        const char q = s_[p_];
        const std::size_t open = p_;
        const std::size_t close = s_.find(q, p_ + 1);
        if (close == std::string_view::npos) fail(open, "expected a closing quote");
        p_ = close + 1;
        return std::string(s_.substr(open + 1, close - open - 1));
    }

    std::uint32_t primary() {
        skip();
        const std::size_t at = p_;
        if (p_ >= s_.size()) fail(p_, "expected a pattern");
        const char c = s_[p_];
        if (c == '(') {
            ++p_;
            const std::uint32_t e = expression();
            expect(")");
            return e;
        }
        if (c == '"' || c == '\'') {
            std::string text = quoted();
            return add(Node{.kind = K::literal, .text = std::move(text)}, at);
        }
        if (c == '[') return set_class();
        if (c == '%') {
            ++p_;
            const std::string n = class_name();
            CharSet set;
            if (!add_class(set, n)) fail(at, std::format("no class %{}", n));
            tree_.sets.push_back(std::move(set));
            return add(Node{.kind = K::set, .index = static_cast<std::uint32_t>(tree_.sets.size() - 1)}, at);
        }
        if (c == '{') {
            if (s_.substr(p_, 2) == "{:") {
                p_ += 2;
                skip();
                std::string n = name();
                expect(":");
                const std::uint32_t e = expression();
                skip();
                if (s_.substr(p_, 2) == ":}") {
                    p_ += 2;
                } else {
                    expect("}");
                }
                return add(Node{.kind = K::named, .kids = {e}, .text = std::move(n)}, at);
            }
            if (s_.substr(p_, 2) == "{~") {
                p_ += 2;
                const std::uint32_t e = expression();
                expect("~}");
                return add(Node{.kind = K::subst, .kids = {e}}, at);
            }
            ++p_;
            skip();
            if (p_ < s_.size() && s_[p_] == '}') fail(at, "an empty capture {}: put a pattern in it");
            const std::uint32_t e = expression();
            expect("}");
            return add(Node{.kind = K::capture, .kids = {e}}, at);
        }
        if (c == '.') {
            ++p_;
            return add(Node{.kind = K::any}, at);
        }
        if (c == '<') {
            ++p_;
            std::string n = name();
            expect(">");
            return add(Node{.kind = K::call, .text = std::move(n)}, at);
        }
        if (name_start(c)) {
            std::string n = name();
            return add(Node{.kind = K::call, .text = std::move(n)}, at);
        }
        fail(at, std::format("unexpected '{}'", shown(at)));
    }

    std::uint32_t set_class() {
        const std::size_t at = p_;
        ++p_;  // [
        CharSet set;
        if (p_ < s_.size() && s_[p_] == '^') {
            set.negated = true;
            ++p_;
        }
        bool first = true;
        while (true) {
            if (p_ >= s_.size()) fail(at, "expected ']' to close the set");
            if (s_[p_] == ']' && !first) {
                ++p_;
                break;
            }
            first = false;
            if (s_[p_] == '%' && p_ + 1 < s_.size() && name_start(s_[p_ + 1])) {
                const std::size_t cls = p_;
                ++p_;
                const std::string n = class_name();
                if (!add_class(set, n)) fail(cls, std::format("no class %{}", n));
                continue;
            }
            const auto [lo, n] = decode(s_, p_);
            p_ += n;
            if (p_ + 1 < s_.size() && s_[p_] == '-' && s_[p_ + 1] != ']') {
                const std::size_t range = p_;
                ++p_;
                const auto [hi, m] = decode(s_, p_);
                p_ += m;
                if (hi < lo || lo == invalid_cp || hi == invalid_cp) fail(range, "a range whose end comes before its start");
                set.add(lo, hi);
            } else if (lo != invalid_cp) {
                set.add(lo, lo);
            } else {
                fail(p_ - n, "a byte in the set that is not UTF-8");
            }
        }
        tree_.sets.push_back(std::move(set));
        return add(Node{.kind = K::set, .index = static_cast<std::uint32_t>(tree_.sets.size() - 1)}, at);
    }

    Tree& tree_;
    std::string_view s_;
    std::size_t p_ = 0;
    std::uint16_t source_;
    bool from_pattern_;
};

// --- the machine ----------------------------------------------------------------

enum class Op : std::uint8_t {
    byte, any, set, choice, commit, partial_commit, back_commit, fail_twice, fail, call, ret, jump, open, close, end,
};

struct Inst {
    Op op = Op::end;
    std::uint8_t kind = 0;  ///< open: CaptureKind
    std::uint32_t a = 0;    ///< byte, set, target, or string
};

struct Program {
    std::vector<Inst> code;
    std::vector<CharSet> sets;
    std::vector<std::string> strings;
    std::optional<unsigned char> first_byte;  ///< every match starts with it
    bool captures = false;
};

class Compiler {
public:
    explicit Compiler(Tree& tree) : t_(tree) {}

    Program run(std::uint32_t root) {
        resolve();
        nullable_.assign(t_.rules.size(), false);
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t i = 0; i < t_.rules.size(); ++i) {
                if (!nullable_[i] && nullable(t_.rules[i].body)) {
                    nullable_[i] = true;
                    changed = true;
                }
            }
        }
        checked_.assign(t_.nodes.size(), false);
        check(root);
        p_.sets = t_.sets;
        emit(root);
        put({Op::end});
        for (std::size_t i = 0; i < queue_.size(); ++i) {
            const std::uint32_t r = queue_[i];
            address_[r] = here();
            emit(t_.rules[r].body);
            put({Op::ret});
        }
        for (const auto& [at, rule] : calls_) p_.code[at].a = address_[rule];
        std::vector<bool> seen(t_.rules.size(), false);
        p_.first_byte = first_byte(root, seen);
        return std::move(p_);
    }

private:
    [[noreturn]] void fail(const Node& n, std::string message) const {
        throw Thrown{Error{t_.sources[n.source], n.line, n.column, std::move(message)}};
    }

    void resolve() {
        for (Node& n : t_.nodes) {
            if (n.kind != K::call) continue;
            const auto it = std::ranges::find(t_.rules, n.text, &Rule::name);
            if (it == t_.rules.end()) fail(n, std::format("no rule named '{}' (a grammar defines it: -g)", n.text));
            n.index = static_cast<std::uint32_t>(it - t_.rules.begin());
        }
    }

    bool nullable(std::uint32_t id) const {
        const Node& n = t_.nodes[id];
        switch (n.kind) {
            case K::empty: return true;
            case K::literal: return n.text.empty();
            case K::any:
            case K::set: return false;
            case K::seq: return std::ranges::all_of(n.kids, [&](std::uint32_t k) { return nullable(k); });
            case K::choice: return std::ranges::any_of(n.kids, [&](std::uint32_t k) { return nullable(k); });
            case K::star:
            case K::opt:
            case K::and_:
            case K::not_: return true;
            case K::plus:
            case K::capture:
            case K::named:
            case K::subst:
            case K::replace: return nullable(n.kids[0]);
            case K::call: return nullable_[n.index];
        }
        return true;
    }

    void check(std::uint32_t id) {
        if (checked_[id]) return;
        checked_[id] = true;
        const Node& n = t_.nodes[id];
        if ((n.kind == K::star || n.kind == K::plus) && nullable(n.kids[0])) {
            fail(n, "this repetition repeats a pattern that can match nothing: it would never end");
        }
        for (std::uint32_t k : n.kids) check(k);
        if (n.kind == K::call) check(t_.rules[n.index].body);
    }

    [[nodiscard]] std::uint32_t here() const { return static_cast<std::uint32_t>(p_.code.size()); }
    std::uint32_t put(Inst i) {
        p_.code.push_back(i);
        return here() - 1;
    }
    void patch(std::uint32_t at) { p_.code[at].a = here(); }

    std::uint32_t string(std::string s) {
        p_.strings.push_back(std::move(s));
        return static_cast<std::uint32_t>(p_.strings.size() - 1);
    }

    void capture(std::uint32_t id, CaptureKind kind, std::uint32_t str) {
        p_.captures = true;
        put({Op::open, static_cast<std::uint8_t>(kind), str});
        emit(id);
        put({Op::close});
    }

    void emit(std::uint32_t id) {
        const Node& n = t_.nodes[id];
        switch (n.kind) {
            case K::empty: break;
            case K::literal:
                for (const char c : n.text) put({Op::byte, 0, static_cast<unsigned char>(c)});
                break;
            case K::any: put({Op::any}); break;
            case K::set: put({Op::set, 0, n.index}); break;
            case K::seq:
                for (std::uint32_t k : n.kids) emit(k);
                break;
            case K::choice: {
                std::vector<std::uint32_t> commits;
                for (std::size_t i = 0; i + 1 < n.kids.size(); ++i) {
                    const std::uint32_t choice = put({Op::choice});
                    emit(n.kids[i]);
                    commits.push_back(put({Op::commit}));
                    patch(choice);
                }
                emit(n.kids.back());
                for (std::uint32_t c : commits) patch(c);
                break;
            }
            case K::plus:
                emit(n.kids[0]);
                [[fallthrough]];
            case K::star: {
                const std::uint32_t choice = put({Op::choice});
                const std::uint32_t body = here();
                emit(n.kids[0]);
                put({Op::partial_commit, 0, body});
                patch(choice);
                break;
            }
            case K::opt: {
                const std::uint32_t choice = put({Op::choice});
                emit(n.kids[0]);
                const std::uint32_t commit = put({Op::commit});
                patch(choice);
                patch(commit);
                break;
            }
            case K::and_: {
                const std::uint32_t choice = put({Op::choice});
                emit(n.kids[0]);
                const std::uint32_t back = put({Op::back_commit});
                patch(choice);
                put({Op::fail});
                patch(back);
                break;
            }
            case K::not_: {
                const std::uint32_t choice = put({Op::choice});
                emit(n.kids[0]);
                put({Op::fail_twice});
                patch(choice);
                break;
            }
            case K::capture: capture(n.kids[0], CaptureKind::simple, 0); break;
            case K::named: capture(n.kids[0], CaptureKind::named, string(n.text)); break;
            case K::subst: capture(n.kids[0], CaptureKind::substitution, 0); break;
            case K::replace: capture(n.kids[0], CaptureKind::replacement, string(n.text)); break;
            case K::call: {
                if (!address_.contains(n.index)) {
                    address_[n.index] = 0;
                    queue_.push_back(n.index);
                }
                calls_.emplace_back(put({Op::call}), n.index);
                break;
            }
        }
    }

    /// The byte every match of `id` starts with, when there is one.
    std::optional<unsigned char> first_byte(std::uint32_t id, std::vector<bool>& seen) const {
        const Node& n = t_.nodes[id];
        switch (n.kind) {
            case K::literal:
                if (!n.text.empty()) return static_cast<unsigned char>(n.text[0]);
                return std::nullopt;
            case K::seq:
                for (std::uint32_t k : n.kids) {
                    const Node& kid = t_.nodes[k];
                    if (kid.kind == K::and_ || kid.kind == K::not_) return std::nullopt;
                    if (!nullable(k)) return first_byte(k, seen);
                    if (kid.kind != K::empty) return std::nullopt;  // may match something, or nothing
                }
                return std::nullopt;
            case K::plus:
            case K::capture:
            case K::named:
            case K::subst:
            case K::replace: return nullable(n.kids[0]) ? std::nullopt : first_byte(n.kids[0], seen);
            case K::choice: {
                std::optional<unsigned char> b;
                for (std::uint32_t k : n.kids) {
                    auto kb = nullable(k) ? std::nullopt : first_byte(k, seen);
                    if (!kb || (b && *b != *kb)) return std::nullopt;
                    b = kb;
                }
                return b;
            }
            case K::call:
                if (seen[n.index] || nullable_[n.index]) return std::nullopt;
                seen[n.index] = true;
                return first_byte(t_.rules[n.index].body, seen);
            default: return std::nullopt;
        }
    }

    Tree& t_;
    Program p_;
    std::vector<bool> nullable_, checked_;
    std::map<std::uint32_t, std::uint32_t> address_;
    std::vector<std::uint32_t> queue_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> calls_;
};

}  // namespace sqedit::peg

export namespace sqedit::peg {

/// The rules of a .peg file, checked: every rule parses. Calls between
/// them are resolved when a pattern that uses them compiles.
[[nodiscard]] std::expected<Grammar, Error> load_grammar(std::string name, std::string text) {
    Tree tree;
    tree.sources = {name};
    try {
        Parser(tree, text, 0, false).definitions();
    } catch (const Thrown& t) {
        return std::unexpected(t.error);
    }
    return Grammar{std::move(name), std::move(text)};
}

/// A compiled pattern. Immutable: one may be used from many threads.
class Pattern {
public:
    [[nodiscard]] static std::expected<Pattern, Error> compile(std::string_view source, std::span<const Grammar> grammars = {},
                                                               CompileOptions options = {}) {
        Tree tree;
        tree.sources.push_back("pattern");
        try {
            for (const Grammar& g : grammars) {
                tree.sources.push_back(g.name);
                Parser(tree, g.text, static_cast<std::uint16_t>(tree.sources.size() - 1), false).definitions();
            }
            std::uint32_t root = Parser(tree, source, 0, true).pattern();
            if (options.to_end) root = to_end(tree, root);
            Pattern p;
            p.program_ = Compiler(tree).run(root);
            return p;
        } catch (const Thrown& t) {
            return std::unexpected(t.error);
        }
    }

    /// A pattern that is `text` and nothing else (sqedit -F).
    [[nodiscard]] static Pattern literal(std::string_view text, CompileOptions options = {}) {
        Tree tree;
        tree.sources.push_back("pattern");
        tree.nodes.push_back(Node{.kind = K::literal, .text = std::string(text)});
        std::uint32_t root = 0;
        if (options.to_end) root = to_end(tree, root);
        Pattern p;
        p.program_ = Compiler(tree).run(root);
        return p;
    }

    /// Whether the pattern has a capture of its own.
    [[nodiscard]] bool has_captures() const noexcept { return program_.captures; }

    /// A match starting at `at` exactly (PEGs are anchored where they
    /// start); nothing when the pattern fails there.
    [[nodiscard]] std::expected<std::optional<Match>, Error> match(std::string_view subject, std::size_t at = 0,
                                                                   Limits limits = {}) const {
        std::uint64_t steps = limits.steps;
        return run(subject, at, steps, limits.depth);
    }

    /// The first match at or after `from`, trying each character in turn.
    [[nodiscard]] std::expected<std::optional<Match>, Error> find(std::string_view subject, std::size_t from = 0,
                                                                  Limits limits = {}) const {
        std::uint64_t steps = limits.steps;
        for (std::size_t at = from; at <= subject.size();) {
            if (program_.first_byte) {
                const std::size_t next = subject.find(static_cast<char>(*program_.first_byte), at);
                if (next == std::string_view::npos) return std::optional<Match>{};
                at = next;
            }
            auto m = run(subject, at, steps, limits.depth);
            if (!m || *m) return m;
            if (at == subject.size()) break;
            at += decode(subject, at).second;
        }
        return std::optional<Match>{};
    }

private:
    static std::uint32_t to_end(Tree& tree, std::uint32_t root) {
        tree.nodes.push_back(Node{.kind = K::any});
        tree.nodes.push_back(Node{.kind = K::not_, .kids = {static_cast<std::uint32_t>(tree.nodes.size() - 1)}});
        tree.nodes.push_back(Node{.kind = K::seq, .kids = {root, static_cast<std::uint32_t>(tree.nodes.size() - 1)}});
        return static_cast<std::uint32_t>(tree.nodes.size() - 1);
    }

    struct Entry {
        std::uint32_t next;  ///< where to go: the alternative, or the return address
        bool call;
        std::size_t pos, captures;
    };
    struct Mark {
        bool open;
        CaptureKind kind;
        std::uint32_t str;
        std::size_t pos;
    };

    std::expected<std::optional<Match>, Error> run(std::string_view s, std::size_t at, std::uint64_t& steps,
                                                   std::size_t depth) const {
        std::vector<Entry> stack;
        std::vector<Mark> marks;
        std::uint32_t pc = 0;
        std::size_t pos = at;
        const auto& code = program_.code;
        while (true) {
            if (steps-- == 0) {
                return std::unexpected(Error{"pattern", 0, 0, "the match took too many steps: the pattern backtracks too much on this text"});
            }
            const Inst& in = code[pc];
            bool failed = false;
            switch (in.op) {
                case Op::end: return build(s, at, pos, marks);
                case Op::byte:
                    if (pos < s.size() && static_cast<unsigned char>(s[pos]) == in.a) {
                        ++pos;
                        ++pc;
                    } else {
                        failed = true;
                    }
                    break;
                case Op::any:
                    if (pos < s.size()) {
                        pos += decode(s, pos).second;
                        ++pc;
                    } else {
                        failed = true;
                    }
                    break;
                case Op::set:
                    if (pos < s.size()) {
                        const auto [cp, n] = decode(s, pos);
                        if (program_.sets[in.a].has(cp)) {
                            pos += n;
                            ++pc;
                        } else {
                            failed = true;
                        }
                    } else {
                        failed = true;
                    }
                    break;
                case Op::choice:
                    if (stack.size() >= depth) return too_deep();
                    stack.push_back({in.a, false, pos, marks.size()});
                    ++pc;
                    break;
                case Op::commit:
                    stack.pop_back();
                    pc = in.a;
                    break;
                case Op::partial_commit:
                    stack.back().pos = pos;
                    stack.back().captures = marks.size();
                    pc = in.a;
                    break;
                case Op::back_commit:
                    pos = stack.back().pos;
                    marks.resize(stack.back().captures);
                    stack.pop_back();
                    pc = in.a;
                    break;
                case Op::fail_twice:
                    stack.pop_back();
                    failed = true;
                    break;
                case Op::fail: failed = true; break;
                case Op::call:
                    if (stack.size() >= depth) return too_deep();
                    stack.push_back({pc + 1, true, 0, 0});
                    pc = in.a;
                    break;
                case Op::ret:
                    pc = stack.back().next;
                    stack.pop_back();
                    break;
                case Op::jump: pc = in.a; break;
                case Op::open:
                    marks.push_back({true, static_cast<CaptureKind>(in.kind), in.a, pos});
                    ++pc;
                    break;
                case Op::close:
                    marks.push_back({false, CaptureKind::simple, 0, pos});
                    ++pc;
                    break;
            }
            if (!failed) continue;
            while (!stack.empty() && stack.back().call) stack.pop_back();
            if (stack.empty()) return std::optional<Match>{};
            pc = stack.back().next;
            pos = stack.back().pos;
            marks.resize(stack.back().captures);
            stack.pop_back();
        }
    }

    static std::unexpected<Error> too_deep() {
        return std::unexpected(Error{"pattern", 0, 0, "the match went too deep: a rule that calls itself before it matches anything?"});
    }

    struct Built {
        CaptureKind kind;
        std::uint32_t str;
        std::size_t begin, end;
        std::vector<std::size_t> kids;
    };

    std::string value(std::string_view s, const std::vector<Built>& all, std::size_t i) const {
        const Built& b = all[i];
        switch (b.kind) {
            case CaptureKind::simple:
            case CaptureKind::named: return std::string(s.substr(b.begin, b.end - b.begin));
            case CaptureKind::replacement: {
                const std::string& t = program_.strings[b.str];
                std::string out;
                for (std::size_t k = 0; k < t.size(); ++k) {
                    if (t[k] == '%' && k + 1 < t.size() && (t[k + 1] == '0' || t[k + 1] == '%')) {
                        if (t[k + 1] == '0') out += s.substr(b.begin, b.end - b.begin);
                        else out += '%';
                        ++k;
                    } else {
                        out += t[k];
                    }
                }
                return out;
            }
            case CaptureKind::substitution: {
                std::string out;
                std::size_t at = b.begin;
                for (std::size_t k : b.kids) {
                    out += s.substr(at, all[k].begin - at);
                    out += value(s, all, k);
                    at = all[k].end;
                }
                out += s.substr(at, b.end - at);
                return out;
            }
        }
        return {};
    }

    void flatten(std::string_view s, const std::vector<Built>& all, std::size_t i, std::vector<Capture>& out) const {
        const Built& b = all[i];
        out.push_back(Capture{b.kind, b.kind == CaptureKind::named ? program_.strings[b.str] : std::string(), b.begin, b.end,
                              value(s, all, i)});
        if (b.kind == CaptureKind::simple || b.kind == CaptureKind::named) {
            for (std::size_t k : b.kids) flatten(s, all, k, out);
        }
    }

    std::optional<Match> build(std::string_view s, std::size_t begin, std::size_t end, const std::vector<Mark>& marks) const {
        Match m{begin, end, {}};
        if (marks.empty()) return m;
        std::vector<Built> all;
        std::vector<std::size_t> open, top;
        for (const Mark& k : marks) {
            if (k.open) {
                all.push_back(Built{k.kind, k.str, k.pos, k.pos, {}});
                (open.empty() ? top : all[open.back()].kids).push_back(all.size() - 1);
                open.push_back(all.size() - 1);
            } else {
                all[open.back()].end = k.pos;
                open.pop_back();
            }
        }
        for (std::size_t i : top) flatten(s, all, i, m.captures);
        return m;
    }

    Program program_;
};

}  // namespace sqedit::peg
