// SPDX-License-Identifier: GPL-2.0-or-later
// Executes Dolphin's portable and generated ARM64 vertex loaders under QEMU.
// This is a focused throughput/correctness test, not a Switch performance claim.
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#include <sys/mman.h>

#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderARM64.h"
#include "VideoCommon/VertexLoaderManager.h"

CPState g_main_cp_state;

namespace VertexLoaderManager
{
Common::EnumMap<u8*, CPArray::TexCoord7> cached_arraybases;
std::array<std::array<float, 4>, 3> position_cache;
std::array<u32, 3> position_matrix_index_cache;
std::array<float, 4> normal_cache;
std::array<float, 4> tangent_cache;
std::array<float, 4> binormal_cache;
}

namespace Common
{
void* AllocateExecutableMemory(std::size_t size)
{
  void* address = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return address == MAP_FAILED ? nullptr : address;
}
bool FreeMemoryPages(void* address, std::size_t size)
{
  return address == nullptr || munmap(address, size) == 0;
}
bool WriteProtectMemory(void* address, std::size_t size, bool executable)
{
  return mprotect(address, size, PROT_READ | (executable ? PROT_EXEC : 0)) == 0;
}
void JITPageWriteEnableExecuteDisable() {}
void JITPageWriteDisableExecuteEnable() {}
bool MsgAlertFmtImpl(bool, MsgType, Log::LogType, const char*, int,
                     fmt::string_view, const fmt::format_args&)
{
  std::abort();
}
namespace Log
{
void GenericLogFmtImpl(LogLevel, LogType, const char*, int, fmt::string_view,
                       const fmt::format_args&)
{
}
}
}

namespace
{
using Clock = std::chrono::steady_clock;

struct Case
{
  const char* name;
  TVtxDesc desc{};
  VAT attr{};
  int vertices = 0;
  int iterations = 0;
  std::vector<u8> input;
  std::array<std::vector<u8>, NUM_VERTEX_COMPONENT_ARRAYS> arrays;
};

void SetArrayPointers(Case& test)
{
  for (std::size_t i = 0; i < test.arrays.size(); ++i)
  {
    auto& data = test.arrays[i];
    if (data.empty())
      data.resize(512, 0);
    VertexLoaderManager::cached_arraybases[static_cast<CPArray>(i)] = data.data();
    g_main_cp_state.array_strides[static_cast<CPArray>(i)] = 32;
  }
}

Case MakeDirectCase()
{
  Case test;
  test.name = "direct_position_normal_color_texcoord";
  test.vertices = 16380;
  test.iterations = 120;
  test.desc.low.Position = VertexComponentFormat::Direct;
  test.desc.low.Normal = VertexComponentFormat::Direct;
  test.desc.low.Color0 = VertexComponentFormat::Direct;
  test.desc.high.Tex0Coord = VertexComponentFormat::Direct;
  test.attr.g0.PosElements = CoordComponentCount::XYZ;
  test.attr.g0.PosFormat = ComponentFormat::Float;
  test.attr.g0.NormalElements = NormalComponentCount::N;
  test.attr.g0.NormalFormat = ComponentFormat::Short;
  test.attr.g0.Color0Elements = ColorComponentCount::RGBA;
  test.attr.g0.Color0Comp = ColorFormat::RGBA8888;
  test.attr.g0.Tex0CoordElements = TexComponentCount::ST;
  test.attr.g0.Tex0CoordFormat = ComponentFormat::Short;
  const auto bytes = VertexLoaderBase::GetVertexSize(test.desc, test.attr);
  test.input.resize(static_cast<std::size_t>(bytes) * test.vertices, 0);
  return test;
}

Case MakeIndexedCase()
{
  Case test;
  test.name = "indexed_large_float_vertex";
  test.vertices = 12000;
  test.iterations = 40;
  test.desc.low.PosMatIdx = true;
  for (std::size_t i = 0; i < test.desc.low.TexMatIdx.Size(); ++i)
    test.desc.low.TexMatIdx[i] = true;
  test.desc.low.Position = VertexComponentFormat::Index16;
  test.desc.low.Normal = VertexComponentFormat::Index16;
  test.desc.low.Color0 = VertexComponentFormat::Index16;
  test.desc.low.Color1 = VertexComponentFormat::Index16;
  for (std::size_t i = 0; i < test.desc.high.TexCoord.Size(); ++i)
    test.desc.high.TexCoord[i] = VertexComponentFormat::Index16;
  test.attr.g0.PosElements = CoordComponentCount::XYZ;
  test.attr.g0.PosFormat = ComponentFormat::Float;
  test.attr.g0.NormalElements = NormalComponentCount::NTB;
  test.attr.g0.NormalFormat = ComponentFormat::Float;
  test.attr.g0.Color0Elements = ColorComponentCount::RGBA;
  test.attr.g0.Color0Comp = ColorFormat::RGBA8888;
  test.attr.g0.Color1Elements = ColorComponentCount::RGBA;
  test.attr.g0.Color1Comp = ColorFormat::RGBA8888;
  test.attr.g0.Tex0CoordElements = TexComponentCount::ST;
  test.attr.g0.Tex0CoordFormat = ComponentFormat::Float;
  test.attr.g1.Tex1CoordElements = TexComponentCount::ST;
  test.attr.g1.Tex1CoordFormat = ComponentFormat::Float;
  test.attr.g1.Tex2CoordElements = TexComponentCount::ST;
  test.attr.g1.Tex2CoordFormat = ComponentFormat::Float;
  test.attr.g1.Tex3CoordElements = TexComponentCount::ST;
  test.attr.g1.Tex3CoordFormat = ComponentFormat::Float;
  test.attr.g1.Tex4CoordElements = TexComponentCount::ST;
  test.attr.g1.Tex4CoordFormat = ComponentFormat::Float;
  test.attr.g2.Tex5CoordElements = TexComponentCount::ST;
  test.attr.g2.Tex5CoordFormat = ComponentFormat::Float;
  test.attr.g2.Tex6CoordElements = TexComponentCount::ST;
  test.attr.g2.Tex6CoordFormat = ComponentFormat::Float;
  test.attr.g2.Tex7CoordElements = TexComponentCount::ST;
  test.attr.g2.Tex7CoordFormat = ComponentFormat::Float;
  const auto bytes = VertexLoaderBase::GetVertexSize(test.desc, test.attr);
  assert(bytes == 33);
  test.input.resize(static_cast<std::size_t>(bytes) * test.vertices, 0);
  for (std::size_t i = 0; i < test.arrays.size(); ++i)
  {
    test.arrays[i].resize(512, 0);
    g_main_cp_state.array_strides[static_cast<CPArray>(i)] = 32;
  }
  return test;
}

std::uint64_t RunTimed(VertexLoaderBase& loader, const std::vector<u8>& input,
                       std::vector<u8>& output, int vertices, int iterations)
{
  for (int i = 0; i < 3; ++i)
    assert(loader.RunVertices(input.data(), output.data(), vertices) == vertices);
  const auto start = Clock::now();
  for (int i = 0; i < iterations; ++i)
    assert(loader.RunVertices(input.data(), output.data(), vertices) == vertices);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

void RunCase(Case test)
{
  SetArrayPointers(test);
  auto software = std::make_unique<VertexLoader>(test.desc, test.attr);
  auto native = std::make_unique<VertexLoaderARM64>(test.desc, test.attr);
  assert(software->m_vertex_size == native->m_vertex_size);
  assert(software->m_native_vtx_decl.stride == native->m_native_vtx_decl.stride);
  const std::size_t output_bytes =
      static_cast<std::size_t>(software->m_native_vtx_decl.stride) * test.vertices;
  std::vector<u8> software_output(output_bytes, 0xcd);
  std::vector<u8> native_output(output_bytes, 0xcd);
  VertexLoaderBase& software_base = *software;
  VertexLoaderBase& native_base = *native;
  assert(software_base.RunVertices(test.input.data(), software_output.data(), test.vertices) ==
         test.vertices);
  assert(native_base.RunVertices(test.input.data(), native_output.data(), test.vertices) ==
         test.vertices);
  assert(software_output == native_output);
  const auto software_ns = RunTimed(software_base, test.input, software_output,
                                    test.vertices, test.iterations);
  const auto native_ns = RunTimed(native_base, test.input, native_output,
                                  test.vertices, test.iterations);
  const double ratio = static_cast<double>(software_ns) / native_ns;
  std::cout << "case=" << test.name << " vertices=" << test.vertices * test.iterations
            << " software_ms=" << software_ns / 1e6 << " arm64_jit_ms=" << native_ns / 1e6
            << " speedup=" << ratio << "x\n";
  assert(ratio > 1.1);
}

void RunPositionCorrectnessMatrix()
{
  constexpr std::array formats = {
      ComponentFormat::UByte, ComponentFormat::Byte, ComponentFormat::UShort,
      ComponentFormat::Short, ComponentFormat::Float, ComponentFormat::InvalidFloat5,
      ComponentFormat::InvalidFloat6, ComponentFormat::InvalidFloat7};
  constexpr std::array addresses = {VertexComponentFormat::Direct,
                                    VertexComponentFormat::Index8,
                                    VertexComponentFormat::Index16};
  constexpr std::array elements = {CoordComponentCount::XY, CoordComponentCount::XYZ};
  constexpr std::array fractions = {0, 1, 31};
  std::size_t cases = 0;
  for (const auto address : addresses)
  {
    for (const auto format : formats)
    {
      for (const auto element_count : elements)
      {
        for (const int fraction : fractions)
        {
          TVtxDesc desc{};
          VAT attr{};
          desc.low.Position = address;
          attr.g0.PosFormat = format;
          attr.g0.PosElements = element_count;
          attr.g0.PosFrac = fraction;
          attr.g0.ByteDequant = true;
          VertexLoader software(desc, attr);
          VertexLoaderARM64 native(desc, attr);
          assert(software.m_vertex_size == native.m_vertex_size);
          assert(software.m_native_vtx_decl.stride == native.m_native_vtx_decl.stride);
          constexpr int count = 12;
          std::vector<u8> source(static_cast<std::size_t>(software.m_vertex_size) * count);
          std::vector<u8> array(4096);
          for (std::size_t i = 0; i < array.size(); ++i)
            array[i] = static_cast<u8>(i * 37 + 11);
          if (address == VertexComponentFormat::Direct)
            std::copy_n(array.begin(), source.size(), source.begin());
          else
            std::fill(source.begin(), source.end(), 0);
          VertexLoaderManager::cached_arraybases[CPArray::Position] = array.data();
          g_main_cp_state.array_strides[CPArray::Position] =
              GetElementSize(format) * (element_count == CoordComponentCount::XY ? 2 : 3);
          const auto output_size =
              static_cast<std::size_t>(software.m_native_vtx_decl.stride) * count;
          std::vector<u8> software_output(output_size, 0xcd);
          std::vector<u8> native_output(output_size, 0xcd);
          VertexLoaderBase& software_base = software;
          VertexLoaderBase& native_base = native;
          assert(software_base.RunVertices(source.data(), software_output.data(), count) == count);
          assert(native_base.RunVertices(source.data(), native_output.data(), count) == count);
          if (software_output != native_output)
          {
            const auto mismatch = std::mismatch(software_output.begin(), software_output.end(),
                                                native_output.begin());
            std::cerr << "MISMATCH address=" << static_cast<int>(address)
                      << " format=" << static_cast<int>(format)
                      << " elements=" << static_cast<int>(element_count)
                      << " fraction=" << fraction
                      << " offset=" << std::distance(software_output.begin(), mismatch.first)
                      << " software=" << static_cast<int>(*mismatch.first)
                      << " native=" << static_cast<int>(*mismatch.second) << '\n';
            std::abort();
          }
          ++cases;
        }
      }
    }
  }

  for (const auto address : {VertexComponentFormat::Index8, VertexComponentFormat::Index16})
  {
    TVtxDesc desc{};
    VAT attr{};
    desc.low.Position = address;
    attr.g0.PosFormat = ComponentFormat::Float;
    attr.g0.PosElements = CoordComponentCount::XYZ;
    VertexLoader software(desc, attr);
    VertexLoaderARM64 native(desc, attr);
    std::vector<u8> source(software.m_vertex_size * 4, 0xff);
    std::vector<u8> array(64, 0);
    std::vector<u8> software_output(software.m_native_vtx_decl.stride * 4, 0xcd);
    std::vector<u8> native_output(native.m_native_vtx_decl.stride * 4, 0xcd);
    VertexLoaderManager::cached_arraybases[CPArray::Position] = array.data();
    g_main_cp_state.array_strides[CPArray::Position] = 12;
    VertexLoaderBase& software_base = software;
    VertexLoaderBase& native_base = native;
    assert(software_base.RunVertices(source.data(), software_output.data(), 4) == 0);
    assert(native_base.RunVertices(source.data(), native_output.data(), 4) == 0);
    // Skipped output is outside the returned range and is intentionally
    // unspecified; the compare loader also compares zero output bytes here.
    ++cases;
  }
  std::cout << "correctness_cases=" << cases << " outputs_match=yes\n";
}
}

int main()
{
  RunPositionCorrectnessMatrix();
  RunCase(MakeDirectCase());
  RunCase(MakeIndexedCase());
  std::cout << "PASS: Dolphin software and ARM64 JIT vertex loaders match and ARM64 is faster under QEMU\n";
}
