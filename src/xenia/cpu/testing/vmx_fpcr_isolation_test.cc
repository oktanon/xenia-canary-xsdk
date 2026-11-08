/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"
#include "xenia/base/byte_order.h"
#include "xenia/cpu/raw_module.h"

#include <cmath>
#include <cstring>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// =============================================================================
// FPCR isolation: VMX vector float ops must not leak their FPCR state into
// subsequent scalar FP operations.
//
// The bug: without scoped FPCR save/restore, a VMX op sets FPCR to
// round-to-nearest + flush-to-zero, and that state persists into the next
// scalar FP op — overriding whatever rounding mode the scalar path expects.
// =============================================================================

TEST_CASE("VMX_FPCR_DOES_NOT_LEAK_INTO_SCALAR", "[backend]") {
  // Strategy:
  //   1. SET_ROUNDING_MODE to toward-positive-infinity (mode 2)
  //   2. Do a VMX vector float add (internally sets FPCR to RN + FZ)
  //   3. Do a scalar float add of 1.0 + 2^-24
  //   4. If FPCR leaked, the scalar add uses round-to-nearest → result = 1.0
  //      If FPCR was properly restored, it uses toward-+inf → result > 1.0
  TestFunction test([](HIRBuilder& b) {
    // Set scalar rounding to toward +infinity.
    b.SetRoundingMode(b.LoadConstantInt32(2));

    // VMX vector float add — this touches FPCR internally.
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));

    // Now do a scalar float add. If FPCR leaked, this will round-to-nearest.
    auto a = b.Convert(LoadFPR(b, 6), FLOAT32_TYPE);
    auto c = b.Convert(LoadFPR(b, 7), FLOAT32_TYPE);
    auto sum = b.Add(a, c);
    StoreFPR(b, 3, b.Convert(sum, FLOAT64_TYPE));

    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        // Vector inputs — just normal values, we don't care about the result.
        ctx->v[4] = vec128f(1.0f, 2.0f, 3.0f, 4.0f);
        ctx->v[5] = vec128f(5.0f, 6.0f, 7.0f, 8.0f);
        // Scalar inputs: 1.0 + 2^-24 — rounds differently under different
        // modes.
        ctx->f[6] = 1.0;
        ctx->f[7] = std::ldexp(1.0, -24);
      },
      [&test](PPCContext* ctx) {
        auto result = static_cast<float>(ctx->f[3]);
        // Under toward-+infinity, 1.0f + 2^-24 rounds UP to nextafter(1.0f).
        // Under round-to-nearest, it rounds to 1.0f (ties to even).
        float expected = std::nextafterf(1.0f, 2.0f);
        REQUIRE(result == expected);
        // Reset rounding mode for subsequent tests.
        test.processors[0]->backend()->SetGuestRoundingMode(ctx, 0);
      });
}

TEST_CASE("VMX_FPCR_DOES_NOT_LEAK_INTO_SCALAR_MULTIPLE_OPS", "[backend]") {
  // Same idea but with two consecutive VMX vector float adds before the
  // scalar op, to verify FPCR is restored after each one.
  TestFunction test([](HIRBuilder& b) {
    b.SetRoundingMode(b.LoadConstantInt32(2));  // toward +inf

    // Two VMX vector float adds back to back.
    StoreVR(b, 3, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 5), FLOAT32_TYPE));
    StoreVR(b, 6, b.VectorAdd(LoadVR(b, 4), LoadVR(b, 3), FLOAT32_TYPE));

    // Scalar add — must still use toward-+inf.
    auto a = b.Convert(LoadFPR(b, 6), FLOAT32_TYPE);
    auto c = b.Convert(LoadFPR(b, 7), FLOAT32_TYPE);
    auto sum = b.Add(a, c);
    StoreFPR(b, 3, b.Convert(sum, FLOAT64_TYPE));

    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128f(1.0f, 2.0f, 3.0f, 4.0f);
        ctx->v[5] = vec128f(5.0f, 6.0f, 7.0f, 8.0f);
        ctx->f[6] = 1.0;
        ctx->f[7] = std::ldexp(1.0, -24);
      },
      [&test](PPCContext* ctx) {
        auto result = static_cast<float>(ctx->f[3]);
        float expected = std::nextafterf(1.0f, 2.0f);
        REQUIRE(result == expected);
        test.processors[0]->backend()->SetGuestRoundingMode(ctx, 0);
      });
}

TEST_CASE("CONSECUTIVE_RESERVED_LOADS_WITHOUT_STORE", "[backend]") {
  // GCC's PowerPC __atomic_compare_exchange / std::atomic pattern executes
  // lwarx, compares, and branches out without stwcx. when the expected value
  // does not match, then executes another lwarx followed by stwcx.
  TestFunction test([](HIRBuilder& b) {
    auto addr1 = LoadGPR(b, 3);
    auto addr2 = LoadGPR(b, 4);
    // First lwarx (abandoned without stwcx., e.g. failed compare branch).
    auto v1 = b.ByteSwap(b.LoadWithReserve(addr1, INT32_TYPE));
    StoreGPR(b, 5, b.ZeroExtend(v1, INT64_TYPE));
    // Second lwarx on a different or same address, followed by stwcx.
    auto v2 = b.ByteSwap(b.LoadWithReserve(addr2, INT32_TYPE));
    StoreGPR(b, 6, b.ZeroExtend(v2, INT64_TYPE));
    auto new_val = b.ByteSwap(b.Truncate(LoadGPR(b, 7), INT32_TYPE));
    auto ok = b.StoreWithReserve(addr2, new_val, INT32_TYPE);
    StoreGPR(b, 8, b.ZeroExtend(ok, INT64_TYPE));
    b.Return();
  });
  uint32_t scratch = test.memory->SystemHeapAlloc(64);
  uint8_t* host_ptr = test.memory->TranslateVirtual(scratch);
  xe::store_and_swap<uint32_t>(host_ptr + 0, 0x11223344u);
  xe::store_and_swap<uint32_t>(host_ptr + 4, 0x55667788u);
  test.Run(
      [scratch](PPCContext* ctx) {
        ctx->r[3] = scratch;
        ctx->r[4] = scratch + 4;
        ctx->r[7] = 0xAABBCCDDu;
      },
      [scratch, host_ptr](PPCContext* ctx) {
        REQUIRE(ctx->r[5] == 0x11223344u);
        REQUIRE(ctx->r[6] == 0x55667788u);
        REQUIRE(ctx->r[8] == 1u);
        REQUIRE(xe::load_and_swap<uint32_t>(host_ptr + 4) == 0xAABBCCDDu);
      });
  test.memory->SystemHeapFree(scratch);
}

TEST_CASE("XSDK_PPC_STRING_WORD_AND_XER_INSTRUCTIONS", "[ppc]") {
  auto memory = std::make_unique<Memory>();
  REQUIRE(memory->Initialize());
  auto backend = std::make_unique<xe::cpu::backend::x64::X64Backend>();
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  REQUIRE(processor->Setup(std::move(backend)));

  uint32_t code_addr = 0x82000000;
  REQUIRE(memory->LookupHeap(code_addr)->AllocFixed(
      code_addr, 64 * 1024, 64 * 1024,
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  uint8_t* code_ptr = memory->TranslateVirtual(code_addr);
  const uint32_t insns[] = {
      0x7C8103A6,  // mtxer r4
      0x7CA102A6,  // mfxer r5
      0x7CC334AA,  // lswi r6, r3, 6
      0x7CC835AA,  // stswi r6, r8, 6
      0x7D401C2A,  // lswx r10, 0, r3
      0x7D404D2A,  // stswx r10, 0, r9
      0x4E800020,  // blr
  };
  for (size_t i = 0; i < xe::countof(insns); ++i) {
    xe::store_and_swap<uint32_t>(code_ptr + i * 4, insns[i]);
  }
  auto raw_mod = std::make_unique<RawModule>(processor.get());
  raw_mod->SetAddressRange(code_addr, sizeof(insns));
  processor->AddModule(std::move(raw_mod));
  processor->backend()->CommitExecutableRange(code_addr, code_addr + 0x10000);

  uint32_t data_addr = memory->SystemHeapAlloc(256);
  uint8_t* data_ptr = memory->TranslateVirtual(data_addr);
  std::memset(data_ptr, 0, 256);
  const char src_bytes[8] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
  std::memcpy(data_ptr + 0, src_bytes, 8);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_addr = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x101,
                                                    stack_addr + stack_size);
  auto* ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;
  ctx->r[3] = data_addr;       // source buffer ("ABCDEFGH")
  ctx->r[4] = 5;               // XER byte count = 5
  ctx->r[8] = data_addr + 32;  // stswi destination (6 bytes)
  ctx->r[9] = data_addr + 64;  // stswx destination (5 bytes)

  auto* fn = processor->ResolveFunction(code_addr);
  REQUIRE(fn != nullptr);
  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  // Verify XER[57:63] byte count round-trip.
  REQUIRE((ctx->r[5] & 0x7F) == 5);
  // Verify lswi loaded 6 bytes into r6 ('ABCD') and r7 ('EF\0\0').
  REQUIRE(static_cast<uint32_t>(ctx->r[6]) == 0x41424344u);
  REQUIRE(static_cast<uint32_t>(ctx->r[7]) == 0x45460000u);
  // Verify stswi stored 6 bytes ("ABCDEF").
  REQUIRE(std::memcmp(data_ptr + 32, "ABCDEF\0\0", 8) == 0);
  // Verify lswx/stswx copied 5 bytes ("ABCDE").
  CHECK(ctx->xer_count == 5);
  CHECK(static_cast<uint32_t>(ctx->r[10]) == 0x41424344u);
  CHECK(static_cast<uint32_t>(ctx->r[11]) == 0x45000000u);
  REQUIRE(std::memcmp(data_ptr + 64, "ABCDE\0\0\0", 8) == 0);

  thread_state.reset();
  memory->SystemHeapFree(stack_addr);
  memory->SystemHeapFree(data_addr);
}
