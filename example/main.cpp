#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "keyvalue.h"

static const char* kSample = R"(
// A comment
"Game"
{
    "Name"      "My game"
    "Version"   3                   /* block comment */
    "Scale"     1.5
    "Windowed"  true
    "Quoted"    "42"                // the type comes from the text: this is an Int

    "Window"
    {
        "Width"   1280
        "Height"  720
    }

    "SearchPath"  "base"
    "SearchPath"  "mods"            // the same name may repeat
    "SearchPath"  "custom"

    "Backend"     "dx11"            [$WINDOWS]
    "Backend"     "vulkan"          [$LINUX]
}
)";

static const char* TypeName(CKeyValue::Type type)
{
    switch (type)
    {
        case CKeyValue::String:
            return "string";
        case CKeyValue::Int:
            return "int";
        case CKeyValue::Float:
            return "float";
        default:
            return "object";
    }
}

int main()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string input = (dir / "kv_example_in.txt").string();
    const std::string output = (dir / "kv_example_out.txt").string();

    std::ofstream(input) << kSample;

    // Reading. Failures are printed to stderr as "file:line:col: message".
    CKeyValue kv;

    if (!kv.LoadFromFile(input.c_str()))
        return 1;

    // An unnamed object takes the name of the first root in the file; names are always lower case.
    std::printf("root: %s\n", kv.GetName());

    // Typed getters with defaults. Names are case-insensitive.
    std::printf("name: %s\n", kv.GetString("name", "?").c_str());
    std::printf("version: %d\n", kv.GetInt("VERSION"));
    std::printf("scale: %g\n", static_cast<double>(kv.GetFloat("scale")));
    std::printf("windowed: %d\n", kv.GetBool("windowed"));
    std::printf("quoted number: %d\n", kv.GetInt("quoted"));
    std::printf("missing: %d\n", kv.GetInt("missing", -1));

    // A nested object: the pointer belongs to the tree, do not delete it.
    if (const CKeyValue* window = kv.GetKeyValue("window"))
        std::printf("window: %dx%d\n", window->GetInt("width"), window->GetInt("height"));

    // The default of GetKeyValue is copied, and the caller owns the copy.
    CKeyValue fallback("fallback");
    fallback.SetInt("x", 1);
    const CKeyValue* sound = kv.GetKeyValue("sound", &fallback);
    std::printf("sound.x: %d\n", sound->GetInt("x"));
    delete sound;

    // Repeated names: Get* sees the first, Children(name) walks all of them.
    std::printf("search path (first): %s\n", kv.GetString("searchpath").c_str());

    for (const CKeyValue::Child c : kv.Children("searchpath"))
        std::printf("  %s = %s\n", c.Name(), c.GetString().c_str());

    // Conditions: only the entry whose condition is true exists (here the platform one).
    std::printf("backend: %s\n", kv.GetString("backend").c_str());

    // Walking everything in file order.
    for (const CKeyValue::Child c : kv.Children())
    {
        std::printf("  %-10s %s\n", c.Name(), TypeName(c.GetType()));
    }

    // Editing. Set* replaces the first entry of the name (or appends), Add* always appends.
    kv.SetString("name", "Renamed");
    kv.SetFloat("scale", 2.0f);
    kv.AddString("searchpath", "extra");

    CKeyValue audio("audio");
    audio.SetInt("volume", 80);
    audio.SetString("device", "default");
    kv.SetKeyValue("audio", &audio);

    kv.RemoveKey("quoted");
    kv.RemoveKeys("searchpath");

    std::printf("entries after editing: %zu\n", kv.Count());

    // Writing.
    if (!kv.WriteToFile(output.c_str()))
        return 1;

    std::ifstream written(output);
    std::printf("written file:\n%s", std::string(std::istreambuf_iterator<char>(written), {}).c_str());
    written.close();

    std::filesystem::remove(input);
    std::filesystem::remove(output);

    return 0;
}
