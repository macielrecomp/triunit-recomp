#pragma once
#include <cstdint>
#include <deque>
#include <filesystem>
#include <vector>

namespace mk2 {
// DCS1 communication latches only; DSP execution and clocks belong to the scheduler.
class DcsLink {
    enum class Kind { Reset, Input, Output, ClearIrq2 };
    struct Event { Kind kind; uint16_t value; template<class A> friend void Fields(A& a, Event& e) { a(e.kind, e.value); } };
    std::deque<Event> pending_;
    uint16_t control_ = 0xc00, input_ = 0, output_ = 0, pending_output_ = 0;
    bool reset_ = false, irq2_ = false;
    template<class A> friend void Fields(A& a, DcsLink& d) {
        a(d.pending_, d.control_, d.input_, d.output_, d.pending_output_, d.reset_, d.irq2_);
    }
    enum class Boundary { Host, Dsp, IsolatedDsp };
    bool SynchronizeEvents(Boundary boundary);
public:
    struct HostRead { uint8_t value; uint32_t polling_cycles; };
    void HostWrite(uint32_t address_bit, uint16_t value, uint16_t mask = 0xffff);
    void Synchronize();
    bool RetireDspInstruction(bool deliver_output = true) {
        return !pending_.empty() && SynchronizeEvents(deliver_output ? Boundary::Dsp : Boundary::IsolatedDsp);
    }
    bool Quiet() const { return pending_.empty(); }   // nothing waits for a DSP instruction boundary
    uint16_t Control() const { return control_; }
    uint16_t MainStatus() const { return control_ >> 4; }
    bool ResetAsserted() const { return reset_; }
    bool Irq2Asserted() const { return irq2_; }
    std::vector<uint64_t> ExecutionState() const {
        std::vector<uint64_t> values={control_,input_,output_,pending_output_,uint64_t(reset_),uint64_t(irq2_),pending_.size()};
        for(const auto& event:pending_){values.push_back(uint64_t(event.kind));values.push_back(event.value);}
        return values;
    }
    uint16_t DspReadInput(bool side_effects = true);
    void DspWriteOutput(uint16_t value);
    HostRead HostReadOutput(bool side_effects = true);
};
std::vector<uint32_t> DecodeDcsBoot(const std::vector<uint8_t>& page);
std::vector<uint8_t> LoadDcsRom(const std::filesystem::path& u2);
inline constexpr const char* kDcsDataSha256 = "002237fe2e9bb44466e617af56695fd2f5ba2cbaf32764b92a85ae9ac75d3648";
std::vector<uint8_t> LoadDcsDataRoms(const std::filesystem::path& directory);
std::vector<uint32_t> LoadDcsBoot(const std::filesystem::path& u2);
}
