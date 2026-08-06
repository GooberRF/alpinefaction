#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string_view>
#include <fstream>

// Needed by MinGW
#ifndef D3D_COMPILE_STANDARD_FILE_INCLUDE
#define D3D_COMPILE_STANDARD_FILE_INCLUDE ((ID3DInclude*)(UINT_PTR)1)
#endif

// Returns the compiler identification string stored in the RDEF chunk of a DXBC blob,
// or an empty view if it cannot be located.
static std::string_view get_dxbc_creator(const char* data, std::size_t size)
{
    auto read_u32 = [data](std::size_t offset) {
        std::uint32_t value;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    };

    // DXBC header: magic, 16 byte digest, version, total size, chunk count, chunk offsets
    if (size < 32 || std::memcmp(data, "DXBC", 4) != 0) {
        return {};
    }
    std::uint32_t num_chunks = read_u32(28);
    if (num_chunks > (size - 32) / 4) {
        return {};
    }

    for (std::uint32_t i = 0; i < num_chunks; ++i) {
        std::size_t chunk_offset = read_u32(32 + i * 4);
        if (chunk_offset > size - 8 || std::memcmp(data + chunk_offset, "RDEF", 4) != 0) {
            continue;
        }
        // RDEF body: buffer/binding counts and offsets, version, flags, then the creator offset
        std::size_t body_offset = chunk_offset + 8;
        if (body_offset + 28 > size) {
            return {};
        }
        std::size_t creator_offset = body_offset + read_u32(body_offset + 24);
        if (creator_offset >= size) {
            return {};
        }
        const char* creator = data + creator_offset;
        std::size_t max_len = size - creator_offset;
        const void* nul = std::memchr(creator, '\0', max_len);
        return {creator, nul ? static_cast<const char*>(nul) - creator : max_len};
    }
    return {};
}

int main(int argc, char* argv[])
{
    if (argc <= 1) {
        printf(
            "Usage: shader_compiler [options...] hlsl_file\n\n"
            "Available options:\n"
            "-o output_file output file name\n"
            "-e entrypoint  sets shader main function\n"
            "-t target      sets shader target (e.g. vs_2_0)\n"
            "-Dname=value   adds macro-definition\n"
            "-O[123]        sets optimization level\n"
        );
        return 1;
    }

    std::string hlsl_filename;
    std::string output_filename;
    std::string entrypoint{"main"};
    std::string target;
    DWORD flags1 = 0;
    DWORD flags2 = 0;
    std::vector<D3D_SHADER_MACRO> macros;

    for (int i = 1; i < argc; ++i) {
        char* arg = argv[i];
        std::string_view arg_sv{arg};
        if (arg_sv.starts_with("-D")) {
            auto eq_pos = arg_sv.find("=", 2);
            if (eq_pos == std::string_view::npos) {
                D3D_SHADER_MACRO macro = {arg + 2, nullptr};
                macros.push_back(macro);
            }
            else {
                arg[eq_pos] = '\0';
                D3D_SHADER_MACRO macro = {arg + 2, arg + eq_pos + 1};
                macros.push_back(macro);
            }
        }
        else if (arg_sv == "-e" && i + 1 < argc) {
            entrypoint = argv[++i];
        }
        else if (arg_sv == "-t" && i + 1 < argc) {
            target = argv[++i];
        }
        else if (arg_sv == "-o" && i + 1 < argc) {
            output_filename = argv[++i];
        }
        else if (arg_sv == "-O1") {
            flags1 |= D3DCOMPILE_OPTIMIZATION_LEVEL1;
        }
        else if (arg_sv == "-O2") {
            flags1 |= D3DCOMPILE_OPTIMIZATION_LEVEL2;
        }
        else if (arg_sv == "-O3") {
            flags1 |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
        }
        else if (arg_sv[0] == '-') {
            printf("Unrecognized option: %s\n", arg);
        }
        else if (hlsl_filename.empty()) {
            hlsl_filename = arg_sv;
        }
        else if (output_filename.empty()) {
            output_filename = arg_sv;
        }
        else {
            printf("Unexpected argument: %s\n", arg);
        }
    }

    if (output_filename.empty()) {
        output_filename = hlsl_filename + ".bin";
    }

    D3D_SHADER_MACRO null_macro = {nullptr, nullptr};
    macros.push_back(null_macro);

    std::wstring hlsl_filename_w;
    hlsl_filename_w.resize(hlsl_filename.size() + 1);
    int num = mbstowcs(hlsl_filename_w.data(), hlsl_filename.c_str(), hlsl_filename.size() + 1);
    hlsl_filename_w.resize(num);
    ID3DBlob* shader_bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    // D3DCompileFromFile is not available in d3dcompiler_43
    // HRESULT hr = D3DCompileFromFile(
    //     hlsl_filename_w.c_str(),
    //     macros.data(),
    //     nullptr,
    //     entrypoint.c_str(),
    //     target.c_str(),
    //     flags1,
    //     flags2,
    //     &shader_bytecode,
    //     &errors
    // );
    std::fstream input_file{hlsl_filename.c_str(), std::ios_base::in | std::ios_base::binary};
    std::string hlsl_code((std::istreambuf_iterator<char>(input_file)), std::istreambuf_iterator<char>());

    HRESULT hr = D3DCompile(
        hlsl_code.data(),
        hlsl_code.size(),
        hlsl_filename.c_str(),
        macros.data(),
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entrypoint.c_str(),
        target.c_str(),
        flags1,
        flags2,
        &shader_bytecode,
        &errors
    );
    if (errors) {
        if (errors->GetBufferSize() > 0) {
            auto errors_size = errors->GetBufferSize();
            auto errors_ptr = errors->GetBufferPointer();
            printf("Error messages:\n%*s\n", static_cast<int>(errors_size), static_cast<char*>(errors_ptr));
        }
        errors->Release();
    }

    if (FAILED(hr)) {
        printf("D3DCompileFromFile failed: %lx\n", hr);
        return 1;
    }

    auto shader_size = shader_bytecode->GetBufferSize();
    auto shader_data = shader_bytecode->GetBufferPointer();

    // When running under Wine without a native d3dcompiler DLL, D3DCompile is served by Wine's
    // builtin implementation, which forwards to vkd3d-shader. That compiler ignores the
    // optimization flags and flattens all control flow, so the bytecode ends up an order of
    // magnitude larger and needs far more temporary registers than drivers accept - the shaders
    // then fail to create at runtime. Reject such blobs instead of packing them.
    auto creator = get_dxbc_creator(static_cast<const char*>(shader_data), shader_size);
    if (creator.find("Microsoft") == std::string_view::npos) {
        if (creator.empty()) {
            creator = "<unknown>";
        }
        printf(
            "Refusing to write %s: compiled by \"%.*s\" instead of the Microsoft HLSL compiler.\n"
            "Install a native d3dcompiler_43.dll into the Wine prefix, e.g. "
            "`winetricks -q d3dcompiler_43`, and set WINEDLLOVERRIDES=d3dcompiler_43=n.\n",
            output_filename.c_str(),
            static_cast<int>(creator.size()),
            creator.data()
        );
        shader_bytecode->Release();
        return 1;
    }

    std::fstream output_file{output_filename.c_str(), std::ios_base::out | std::ios_base::binary};
    if (!output_file) {
        printf("Failed to open output file: %s\n", output_filename.c_str());
        return 1;
    }
    output_file.write(static_cast<char*>(shader_data), shader_size);
    shader_bytecode->Release();
    printf("Shader byte code size: %ld\n", shader_size);
    return 0;
}
