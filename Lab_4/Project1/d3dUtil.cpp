#include "d3dUtil.h"
#include "ThrowIfFailed.h"
#include <filesystem>
#include <windows.h>
#include <array>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace
{
    std::filesystem::path GetExeDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    std::wstring ResolveShaderFile(const std::wstring& filename)
    {
        std::filesystem::path original(filename);
        if (std::filesystem::exists(original))
            return original.wstring();

        std::filesystem::path cwd = std::filesystem::current_path();
        std::filesystem::path exeDir = GetExeDirectory();
        std::filesystem::path name = original.filename();

        std::vector<std::filesystem::path> candidates =
        {
            cwd / name,
            cwd / "Project1" / name,
            cwd / ".." / "Project1" / name,
            exeDir / name,
            exeDir / ".." / name,
            exeDir / ".." / ".." / name,
            exeDir / ".." / "Project1" / name,
            exeDir / ".." / ".." / "Project1" / name
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
                return candidate.lexically_normal().wstring();
        }

        return filename;
    }
}

namespace d3dUtil
{
    Microsoft::WRL::ComPtr<ID3D12Resource> LoadIblDds(
        ID3D12Device* device, ID3D12GraphicsCommandList* commands, const std::wstring& filename,
        Microsoft::WRL::ComPtr<ID3D12Resource>& upload)
    {
        std::ifstream file(std::filesystem::path(filename), std::ios::binary | std::ios::ate);
        const auto length = file.tellg();
        file.seekg(0);
        std::array<UINT, 37> header = {};
        auto invalid = [&] { throw std::runtime_error("Invalid or unsupported IBL DDS: " + std::filesystem::path(filename).string()); };
        if (!file.read(reinterpret_cast<char*>(header.data()), sizeof(header))) invalid();
        // shortcut: only the bundled DX10 BC6H/RG32F maps, use a general DDS loader for other formats.
        if (header[0] != 0x20534444 || header[1] != 124 || header[19] != 32 ||
            header[21] != 0x30315844 || header[33] != 3 || header[35] != 1 || header[6] > 1)
            invalid();
        const UINT width = header[4], height = header[3], mips = header[7];
        const bool cube = (header[34] & 4) != 0;
        const auto format = static_cast<DXGI_FORMAT>(header[32]);
        if (!width || !height || width > 16384 || height > 16384 || !mips ||
            (cube && width != height) || (header[34] & ~4u) != 0 ||
            (format != DXGI_FORMAT_BC6H_UF16 && format != DXGI_FORMAT_R32G32_FLOAT)) invalid();
        UINT maxMips = 1;
        for (UINT size = (std::max)(width, height); size > 1; size >>= 1) ++maxMips;
        if (mips > maxMips) invalid();
        const UINT count = mips * (cube ? 6 : 1);
        std::vector<UINT64> sourceOffsets(count), sourcePitches(count);
        std::vector<UINT> sourceRows(count);
        UINT64 bytes = 0;
        for (UINT i = 0; i < count; ++i)
        {
            const UINT w = (std::max)(1u, width >> (i % mips));
            const UINT h = (std::max)(1u, height >> (i % mips));
            sourceOffsets[i] = bytes;
            sourcePitches[i] = format == DXGI_FORMAT_BC6H_UF16 ? UINT64((w + 3) / 4) * 16 : UINT64(w) * 8;
            sourceRows[i] = format == DXGI_FORMAT_BC6H_UF16 ? (h + 3) / 4 : h;
            bytes += sourcePitches[i] * sourceRows[i];
        }
        if (length != std::streamoff(sizeof(header) + bytes)) invalid();
        std::vector<char> pixels(static_cast<size_t>(bytes));
        if (!file.read(pixels.data(), static_cast<std::streamsize>(bytes))) invalid();
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = cube ? 6 : 1;
        desc.MipLevels = static_cast<UINT16>(mips);
        desc.Format = format; desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        UINT64 uploadBytes = 0;
        device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), nullptr, nullptr, &uploadBytes);
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = uploadBytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
        char* mapped = nullptr;
        D3D12_RANGE none = { 0, 0 };
        ThrowIfFailed(upload->Map(0, &none, reinterpret_cast<void**>(&mapped)));
        for (UINT i = 0; i < count; ++i)
        {
            for (UINT row = 0; row < sourceRows[i]; ++row)
                memcpy(mapped + footprints[i].Offset + UINT64(row) * footprints[i].Footprint.RowPitch,
                    pixels.data() + sourceOffsets[i] + row * sourcePitches[i], static_cast<size_t>(sourcePitches[i]));
            D3D12_TEXTURE_COPY_LOCATION source = {}, target = {};
            source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[i];
            target.pResource = texture.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.SubresourceIndex = i;
            commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
        upload->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        commands->ResourceBarrier(1, &barrier);
        return texture;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        const std::wstring& filename,
        const D3D_SHADER_MACRO* defines,
        const std::string& entrypoint,
        const std::string& target)
    {
        UINT compileFlags = 0;
#ifdef _DEBUG
        compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        Microsoft::WRL::ComPtr<ID3DBlob> byteCode = nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;

        std::wstring resolvedFilename = ResolveShaderFile(filename);

        HRESULT hr = D3DCompileFromFile(
            resolvedFilename.c_str(),
            defines,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entrypoint.c_str(),
            target.c_str(),
            compileFlags,
            0,
            &byteCode,
            &errors
        );

        if (errors != nullptr)
        {
            std::string errorStr = "Shader Compile Error:\n";
            errorStr += (char*)errors->GetBufferPointer();
            OutputDebugStringA(errorStr.c_str());
        }

        if (FAILED(hr))
        {
            if (errors)
            {
                MessageBoxA(0,
                    (char*)errors->GetBufferPointer(),
                    "Shader Compile Error",
                    MB_OK);
            }
            ThrowIfFailed(hr);
        }
        return byteCode;
    }

    UINT CalcConstantBufferByteSize(UINT byteSize)
    {
        // Constant buffers must be a multiple of 256 bytes.
        // Round up to nearest multiple of 256.
        return (byteSize + 255) & ~255;
    }
}
