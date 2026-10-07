// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-windows: shows Windows exception dispatch overwriting a System V red zone, and the
// red-zone patcher (windows/redzone.cpp) preventing it.
//
// A generated leaf function fills its 128-byte red zone, stores to a write-protected page (as
// GPU write tracking protects guest pages; the fault handler unprotects it) and counts the
// red-zone values that changed. Each store form is tried: a long one (rerouted with a near jump)
// and a 2-byte one (relocated with its neighbours).
#include "../redzone.cpp"

#include <signal.h>
#include <ucontext.h>

extern "C" void* runtime_low_map(size_t size, int prot) {
    return mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

namespace {

u8* tracked_page;
int faults;
uintptr_t fault_rsp;

void OnFault(int, siginfo_t* info, void* context) {
    ++faults;
    fault_rsp = static_cast<uintptr_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RSP]);
    if (static_cast<u8*>(info->si_addr) >= tracked_page && static_cast<u8*>(info->si_addr) < tracked_page + 4096) {
        mprotect(tracked_page, 4096, PROT_READ | PROT_WRITE);
        return;
    }
    std::fprintf(stderr, "unexpected fault at %p\n", info->si_addr);
    std::_Exit(2);
}

/// rdi: the page. Returns the number of red-zone qwords that changed across the store.
struct LeafFunction : Xbyak::CodeGenerator {
    LeafFunction(bool short_store, u8* buffer) : Xbyak::CodeGenerator(4096, buffer) {
        for (int i = 1; i <= 16; ++i) {
            mov(rax, 0xA5A5A5A500000000ull | i);
            mov(ptr[rsp - 8 * i], rax);
        }
        mov(eax, 1);
        if (short_store)
            mov(dword[rdi], eax); // 2 bytes
        else
            mov(dword[rdi + 0x40], 1); // 7 bytes
        xor_(ecx, ecx);
        for (int i = 1; i <= 16; ++i) {
            mov(rdx, 0xA5A5A5A500000000ull | i);
            cmp(ptr[rsp - 8 * i], rdx);
            setne(al);
            movzx(eax, al);
            add(ecx, eax);
        }
        mov(eax, ecx);
        ret();
    }
};

using Leaf = int(__attribute__((sysv_abi)) *)(u8*);

int Run(Leaf leaf) {
    mprotect(tracked_page, 4096, PROT_READ);
    return leaf(tracked_page);
}

} // namespace

int main() {
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    struct sigaction action {};
    action.sa_sigaction = OnFault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    tracked_page = static_cast<u8*>(mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

    int failures = 0;
    for (const bool short_store : {false, true}) {
        // Code and trampolines in one buffer: the patched site jumps there with a rel32.
        auto* buffer = static_cast<u8*>(mmap(nullptr, 1 << 17, PROT_READ | PROT_WRITE | PROT_EXEC,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        LeafFunction code(short_store, buffer);
        auto* start = const_cast<u8*>(code.getCode());
        faults = 0;
        const int before = Run(reinterpret_cast<Leaf>(start));
        const int faults_before = faults;
        const uintptr_t rsp_before = fault_rsp;

        PatchModule module(start, code.getSize(), buffer + (1 << 16), 1 << 16);
        const uintptr_t function = reinterpret_cast<uintptr_t>(start);
        const auto result = PatchRedZoneMemoryInstructions(&module, function, code.getSize(),
                                                           std::span<const uintptr_t>(&function, 1));
        faults = 0;
        const int after = Run(reinterpret_cast<Leaf>(start));
        std::printf("  faults: %d unpatched (rsp %p), %d patched (rsp %p); handler frame at %p\n", faults_before,
                    (void*)rsp_before, faults, (void*)fault_rsp, (void*)&before);

        std::printf("%s store: %d of 16 red-zone values overwritten unpatched, %d patched "
                    "(%llu/%llu accesses rerouted)\n",
                    short_store ? "2-byte" : "7-byte", before, after,
                    (unsigned long long)result.patched_memory_instruction_count,
                    (unsigned long long)result.memory_instruction_count);
        failures += after != 0 || result.patched_memory_instruction_count != 1;
    }
    const int clobbered = ProbeRedZoneClobbering();
    if (clobbered)
        std::printf("This Windows: exception records reach %d bytes below the stack pointer (red zone at risk)\n",
                    clobbered);
    else
        std::puts("This Windows: exception records stay out of the red zone");
    std::puts(failures ? "FAIL" : "PASS: red zones survive faults in patched leaf functions");
    return failures ? 1 : 0;
}
