// SPDX-License-Identifier: MIT
//
// sqedit.grammars: the grammars sqedit ships (grammars/*.peg), compiled
// into the program, so `-g json` needs no file on the disk.

export module sqedit.grammars;

import std;

namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char json_text[] = {
#embed "../grammars/json.peg"
};
constexpr char c_text[] = {
#embed "../grammars/c.peg"
};
constexpr char ini_text[] = {
#embed "../grammars/ini.peg"
};
#pragma clang diagnostic pop

}  // namespace

export namespace sqedit {

struct ShippedGrammar {
    std::string_view name;
    std::string_view text;
};

/// The grammars sqedit ships, by the name -g takes.
[[nodiscard]] std::span<const ShippedGrammar> shipped_grammars() noexcept {
    static constexpr ShippedGrammar all[] = {
        {"c", {c_text, sizeof c_text}},
        {"ini", {ini_text, sizeof ini_text}},
        {"json", {json_text, sizeof json_text}},
    };
    return all;
}

}  // namespace sqedit
