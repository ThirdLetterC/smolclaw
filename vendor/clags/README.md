# clags
A simple declarative command line argument parser, written in C.

## Features
- Positional, option and flag arguments
- Typed arguments: `bool`, `int8`, `uint8`, `int32`, `uint32`, `int64`, `uint64`, `double`, `path`, `size`, `time_s`, `time_ns`
- Choice arguments: restrict values to a fixed set (like an enum)
- Custom parsing functions for user-defined types
- Native recursive subcommands

## How to use
`clags` ships as a regular C library with:
- public header: `include/clags/clags.h`
- implementation unit: `src/clags.c`

Compile it with any compiler supporting C23 (`-std=c23`, or `-std=c2x` on older toolchains). No further dependencies are required.

### Example
```c
#include <stdio.h>

#include "clags/clags.h"

// Declare argument variables with optional default values
const char *input_file = nullptr;
const char *output_file = "a.out";
bool warnings = false;
bool help = false;

// Declare all expected arguments
clags_arg_t args[] = {
    // Positional arguments are parsed in the order they are defined here
    clags_positional(&input_file, "input_file", "the input file"),
    
    // Option arguments support both short and long flags
    // For long flags, both the `--output <file>` and `--output=<file>` syntaxes are supported
    clags_option('o', "output", &output_file, "FILE", "the output file"),

    // Use flags to set boolean values on occurrence
    // Short flags can be standalone, or combined into multi-flags, e.g.: -abc
    clags_flag('w', "warnings", &warnings, "print warnings"),
    // This is a neat short-hand since the `--help` flags are so common
    clags_flag_help(&help),
};

clags_config_t config = clags_config(args);

int main(int argc, char **argv)
{
    // Parse the arguments using the previously defined rules, returns the config on error
    if (clags_parse(argc, argv, &config) != nullptr) {
        // Print an automatic usage, based on the defined config
        clags_usage(argv[0], &config);
        return 1;
    }

    // You can now use the set argument variables
    if (help) {
        clags_usage(argv[0], &config);
        return 0;
    }

    printf("input: %s, output: %s, warnings: %s\n",
           input_file,
           output_file,
           warnings ? "true" : "false");
    return 0;
}
```
When run with this input:
```console
./example --help
```
**clags** will print the following usage:
```console
Usage: ./example [OPTIONS] [FLAGS] <input_file>
  Arguments:
    input_file                       : the input file
  Options:
    -o, --output(=)FILE              : the output file
  Flags:
    -w, --warnings                   : print warnings
    -h, --help                       : print this help dialog and exit
```
For more advanced examples see the [examples](/examples) directory.
