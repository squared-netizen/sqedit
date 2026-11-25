-- build.lua: sqedit's own build, for sqmake.
--
--   sqmake          build/sqedit, its library (libsqedit.a) and the tests' programs
--   sqmake test     the engine's tests, then tests/run_check.sh with build/sqedit
--   sqmake --debug  -O0 (build/debug)
--
-- The compiler is $CXX (and $CC): clang and libc++ 19 or later (23 is what
-- sqterm and CI use), for `import std;`. Offline, and with no sed, grep or
-- awk: sqedit is built before anything uses it (its design,
-- "Bootstrapping"), by sqmake, which needs neither (tools/bootstrap.sh
-- there). sq.data (squared's JSON module) and yyjson are vendored.

local opt = mode.debug and {"-O0", "-g"} or {"-O2", "-g"}
toolchain.cxxflags = {"-std=c++23", "-stdlib=libc++", opt}
toolchain.cflags = {opt}
toolchain.ldflags = {"-stdlib=libc++"}

local warn = {"-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wno-sign-conversion",
              "-Wno-missing-field-initializers", "-Wno-missing-designated-field-initializers"}
local yyjson = {"-isystem", "third_party/yyjson-0.12.0/src"}

-- sq.data (squared's, vendored: tools/vendor_squared_data.sh) and yyjson.
local sqd = "third_party/squared-data"
local data_srcs = {"third_party/yyjson-0.12.0/src/yyjson.c"}
for _, f in ipairs(glob(sqd .. "/**/*.cppm")) do data_srcs[#data_srcs + 1] = f end
for _, f in ipairs(glob(sqd .. "/src/*.cpp")) do data_srcs[#data_srcs + 1] = f end
local data = library{name = "sqdata", srcs = data_srcs, flags = {"-w", yyjson}}

-- The library: what a tool that parses text links (sqedit.peg, and the
-- grammars sqedit ships, grammars/*.peg embedded).
local lib = library{name = "sqedit", srcs = {"src/peg.cppm", "src/grammars.cppm"}, flags = warn}

local bin = exe{name = "sqedit", srcs = {"tools/sqedit.cpp"}, deps = {lib, data}, flags = {warn, yyjson}}

local tests = {}
for _, src in ipairs(glob("tests/test_*.cpp")) do
  tests[#tests + 1] = exe{name = src:match("([^/]+)%.cpp$"), srcs = {src}, deps = {lib}, flags = warn}
end

verb("test", {needs = {bin, tests}, call = function()
  for _, t in ipairs(tests) do
    if exec{t} ~= 0 then return 1 end
  end
  return exec{"sh", "tests/run_check.sh", bin}
end})
