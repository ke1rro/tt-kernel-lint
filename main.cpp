#include <iostream>
#include <clang-c/Index.h>

int main() {
    // Create an index. An index is the index of translation units
    // that go together to make up an executable or library.
    int IGNORE_LOCAL_DECLS = 0;
    int DIAGNOSTICS = 0;
    CXIndex index = clang_createIndex(IGNORE_LOCAL_DECLS, DIAGNOSTICS);

    // Parse a C++ file. This yields a translation unit if it succeeds,
    // or if it fails, it simply yields nothing. To get an error message,
    // use `clang_parseTranslationUnit2`. Here we keep it simple.
    CXTranslationUnit unit = clang_parseTranslationUnit(
        index,                  // The index to register this translation unit's AST with.
        "example.cpp",          // The file to parse and build an AST for.
        nullptr,                // We have no command line args to pass to Clang.
        0,                      // Num of command line args to pass to Clang is 0.
        nullptr,                // We have no other files to parse.
        0,                      // Num of other files to parse is 0.
        CXTranslationUnit_None  // No special options.
    );

    // If clang failed to parse `example.cpp`, show a message and exit.
    if (!unit) {
        std::cerr << "Can't parse example.cpp." << std::endl;
        clang_disposeIndex(index);  // clean up
        exit(1);
    }

    // Clean up.
    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
}
