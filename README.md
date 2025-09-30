# C+ → C++98 Converter (scope-aware)

This repo contains a **single-pass converter** that turns a lightweight “C+” dialect into valid **C++98**. It is designed to be **predictable**, **whitespace-tolerant**, and **scope-aware**, so it rewrites member access correctly based on whether the base expression is a pointer, an array element, or a plain object.


## What is “C+”?

**C+** is “basically C with two key conveniences”:

1. **Pointers use `.` in source (never `->`).**  
   In C+, all member access uses a dot. During conversion, the tool changes `.` to `->` where the base expression is a pointer. For multi-level pointers, it rewrites `pps.a` as `(*pps)->a`.

2. **End of line acts like a semicolon `;`.**  
   You don’t have to type `;` at the end of statements. The converter inserts semicolons where needed (and avoids doing it inside enum bodies or immediately after `{`).

3. **Function overloading.**  
   Functions can share a name as long as its argumets differ.

Everything else should “feel like C”. Preprocessor lines are kept; comments are ignored.

### Quick C+ examples

```c
// pointers use '.'
struct Node* head = make_list(3)
head.next.v = 123        // → head->next->v = 123;

// arrays then fields
Vec2* buf[16] = { }
buf[8].dx = 7            // → buf[8]->dx = 7;

// double pointers
struct S** pps = &ps
pps.a = 0                // → (*pps)->a = 0
```

### Installing / Building

Single C++98 source file. No deps.
```bash
g++ -std=c++98 -O2 -o cplus2cpp cplus_to_cpp_scoped.cpp
# Windows (MSVC):
cl /O2 /EHsc cplus_to_cpp_scoped.cpp /Fe:cplus2cpp.exe
```

### Usage

```bash
# Convert one file
./cplus2cpp path/to/file.cp
# → writes path/to/file.cpp

# Convert multiple files
./cplus2cpp a.cp src/b.cp dir/nested/c.cp
# → writes a.cpp, src/b.cpp, dir/nested/c.cpp
```

### Known limitations

- **Typedef pointers:** `typedef T* P; P x;` pointer level on x is detected via its own declarator; stars attached to the typedef name itself aren’t propagated globally yet.

- **Wild macros:** macros that splice braces/semicolons across lines can defeat EOL inference (no actual CPP expansion is performed).

- **Non-C/C++98 tokens:** newer C++ features/attributes aren’t recognized.

- **Very complex declarators:** common forms work; exotic ones may require tweaks.

If you hit a case, open a minimal repro and we can adjust the passes.

### Troubleshooting

- **Windows / CRLF:** handled. Prefer passing file paths directly (this build doesn’t read stdin).

- **No output file:** ensure you passed at least one .cp path and that the file is readable/writable.

- **Expected ```->``` not produced:** verify the base is declared as a pointer in the current or a parent scope; the converter relies on scope-visible symbols.
