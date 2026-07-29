#define PMB887X_TRACE_ID		DSP
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp"

#include "qemu/osdep.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "hw/arm/pmb887x/dsp_core.h"
#include "teakra/exceptions.h"
#include "teakra/impl/register.h"
#include "teakra/teakra.h"

extern "C" {
#include "hw/arm/pmb887x/trace.h"
}

namespace {

constexpr uint32_t DSP_ADDRESS_SPACE_WORDS = 0x10000;
constexpr uint32_t DSP_REVISION_FAMILY_MASK = 0xF0;
constexpr uint16_t DSP_PAGE_MMIO = 0x00A3;
constexpr uint16_t EXTERNAL_INTERRUPT_STATUS_MMIO = 0x0000;
constexpr uint16_t EXTERNAL_INTERRUPT_CLEAR_MMIO = 0x0002;
constexpr uint16_t COMMUNICATION_FLAG_SET_MMIO = 0x0091;
constexpr uint16_t COMMUNICATION_FLAG_CLEAR_MMIO = 0x0092;
constexpr uint16_t INTERRUPT_TO_MCU_MMIO = 0x0010;
constexpr uint16_t MCU_INTERRUPT_MASK = 0x000F;
constexpr uint16_t ROM_VERSION_FAMILY_MASK = 0xFF00;
constexpr std::array<uint8_t, 3> REQUEST_INTERRUPTS = { 0, 1, 0 };

struct DspRevisionConfig {
	uint32_t family;
	uint16_t default_rom_version;
	uint16_t page_field_mask;
	uint32_t program_page_shift;
	uint16_t program_rom_address;
	uint16_t program_bank_address;
	size_t program_bank_count;
	uint16_t data_rom_address;
	uint16_t data_bank_address;
	size_t data_bank_count;
	uint16_t shared_base;
	uint16_t shared_size;
	uint16_t mmio_base;
	uint16_t mmio_size;
};

constexpr std::array<DspRevisionConfig, 2> DSP_REVISION_CONFIGS = {{
	[]() constexpr {
		DspRevisionConfig config{};
		config.family = 0x10;
		config.default_rom_version = 0x0602;
		config.page_field_mask = 0x0001;
		config.program_page_shift = 1;
		config.program_rom_address = 0x1000;
		config.program_bank_address = 0xB000;
		config.program_bank_count = 2;
		config.data_rom_address = 0x6000;
		config.data_bank_address = 0x9000;
		config.data_bank_count = 2;
		config.shared_base = 0xD800;
		config.shared_size = 0x0600;
		config.mmio_base = 0xE600;
		config.mmio_size = 0x0100;
		return config;
	}(),
	[]() constexpr {
		DspRevisionConfig config{};
		config.family = 0x30;
		config.default_rom_version = 0x0801;
		config.page_field_mask = 0x0003;
		config.program_page_shift = 2;
		config.program_rom_address = 0x2000;
		config.program_bank_address = 0xA000;
		config.program_bank_count = 3;
		config.data_rom_address = 0x8000;
		config.data_bank_address = 0x9000;
		config.data_bank_count = 4;
		config.shared_base = 0xD000;
		config.shared_size = 0x0C00;
		config.mmio_base = 0xDE00;
		config.mmio_size = 0x0E00;
		return config;
	}(),
}};

const DspRevisionConfig *FindRevisionConfig(uint32_t revision) {
	uint32_t family = revision & DSP_REVISION_FAMILY_MASK;
	for (const DspRevisionConfig &config : DSP_REVISION_CONFIGS)
		if (config.family == family)
			return &config;
	return nullptr;
}

uint16_t ReadU16(const uint8_t *data) {
	return data[0] | (uint16_t) data[1] << 8;
}

class DspCore {
public:
	DspCore(const DspRevisionConfig *revision_config, uint16_t rom_version, const uint8_t *program_rom,
		const uint8_t *data_rom)
		: revision_config(revision_config), rom_version(rom_version), program_rom(program_rom), data_rom(data_rom)
	{
		teakra_config.teaklite = true;
		teakra = std::make_unique<Teakra::Teakra>(teakra_config);
		teakra->SetMMIOReadHandler(EXTERNAL_INTERRUPT_STATUS_MMIO, [this]() { return external_interrupt_status; });
		teakra->SetMMIOWriteHandler(EXTERNAL_INTERRUPT_CLEAR_MMIO, [this](uint16_t value) { external_interrupt_status &= ~value; });
		teakra->SetMMIOWriteHandler(COMMUNICATION_FLAG_SET_MMIO, [this](uint16_t value) { SetCommunicationFlags(value); });
		teakra->SetMMIOWriteHandler(COMMUNICATION_FLAG_CLEAR_MMIO, [this](uint16_t value) { ClearCommunicationFlagsFromDsp(value); });
	}

	void Reset() {
		std::vector<uint16_t> program_ram;
		if (initialized) {
			program_ram.reserve(revision_config->program_rom_address);
			for (uint32_t address = 0; address < revision_config->program_rom_address; address++)
				program_ram.push_back(teakra->ProgramRead(address));
		}

		teakra->Reset();
		teakra->SetMMIOConfig(revision_config->mmio_base, revision_config->mmio_size, false);
		teakra->SetSharedMemoryConfig(revision_config->shared_base, revision_config->shared_size);
		size_t program_fixed_words = revision_config->program_bank_address - revision_config->program_rom_address;
		size_t data_fixed_words = revision_config->data_bank_address - revision_config->data_rom_address;
		LoadProgramRom(program_rom, revision_config->program_rom_address, program_fixed_words);
		LoadDataRom(data_rom, revision_config->data_rom_address, data_fixed_words);
		active_program_bank = SIZE_MAX;
		active_data_bank = SIZE_MAX;
		active_page = SIZE_MAX;
		MapProgramBank(0);
		MapDataBank(0);

		for (size_t address = 0; address < program_ram.size(); address++)
			teakra->ProgramWrite(address, program_ram[address]);

		uint32_t program_rom_words = DSP_ADDRESS_SPACE_WORDS - revision_config->program_rom_address;
		uint32_t data_rom_words = revision_config->shared_base - revision_config->data_rom_address;
		teakra->SetProgramWriteProtection(revision_config->program_rom_address, program_rom_words);
		teakra->SetDataWriteProtection(revision_config->data_rom_address, data_rom_words);

		auto &registers = teakra->GetRegisterState();
		registers.pc = revision_config->program_rom_address + 2;
		registers.prpage = 0;
		teakra->DataWrite(revision_config->shared_base, rom_version, true);

		input_requests = 0;
		external_interrupt_status = 0;
		communication_status.store(0, std::memory_order_relaxed);
		communication_cleared.store(0, std::memory_order_relaxed);
		last_unknown_mcu_interrupts = 0;
		halted = false;
		initialized = true;
	}

	void Run(size_t cycles, uint16_t *communication_clear_result) {
		if (halted) {
			*communication_clear_result = communication_cleared.exchange(0, std::memory_order_acquire);
			return;
		}

		for (size_t i = 0; i < cycles; i++) {
			UpdateBanks();
			uint32_t pc = CurrentPC();
			uint16_t opcode = teakra->ProgramRead(pc);
			try {
				teakra->Run(1);
			} catch (const Teakra::UndefinedInstructionException &error) {
				DPRINTF("undefined instruction: revision=r%02X pc=%05X opcode=%04X\n",
					revision_config->family, pc, error.opcode);
				halted = true;
				break;
			} catch (const Teakra::UnimplementedException &error) {
				DPRINTF("unimplemented instruction: revision=r%02X pc=%05X opcode=%04X error=%s\n",
					revision_config->family, pc, opcode, error.what());
				halted = true;
				break;
			} catch (const std::exception &error) {
				DPRINTF("DSP core exception: revision=r%02X pc=%05X opcode=%04X error=%s\n",
					revision_config->family, pc, opcode, error.what());
				halted = true;
				break;
			}
			if (communication_cleared.load(std::memory_order_relaxed) != 0)
				break;
		}

		*communication_clear_result = communication_cleared.exchange(0, std::memory_order_acquire);
	}

	uint16_t SharedRead(uint16_t offset) {
		return teakra->SharedDataRead(revision_config->shared_base + offset);
	}

	void SharedWrite(uint16_t offset, uint16_t value) {
		teakra->SharedDataWrite(revision_config->shared_base + offset, value);
	}

	void SetRequest(size_t index, bool level) {
		if (index >= REQUEST_INTERRUPTS.size()) {
			DPRINTF("unknown DSP input interrupt: revision=r%02X index=%zu level=%u\n",
				revision_config->family, index, level);
			return;
		}

		uint16_t mask = (uint16_t) (1U << index);
		if (!level) {
			input_requests &= ~mask;
			return;
		}
		if ((input_requests & mask))
			return;

		input_requests |= mask;
		external_interrupt_status |= mask;
		teakra->SignalInterrupt(REQUEST_INTERRUPTS[index]);
	}

	void SetCommunicationFlags(uint16_t value) {
		communication_status.fetch_or(value, std::memory_order_release);
	}

	void ClearCommunicationFlags(uint16_t value) {
		communication_status.fetch_and((uint16_t) ~value, std::memory_order_release);
	}

	uint16_t GetCommunicationFlags() const {
		return communication_status.load(std::memory_order_acquire);
	}

	uint16_t GetMcuInterrupts() {
		uint16_t value = teakra->MMIOPeek(INTERRUPT_TO_MCU_MMIO);
		uint16_t unknown = value & ~MCU_INTERRUPT_MASK;
		if (unknown != last_unknown_mcu_interrupts) {
			DPRINTF("unknown DSP output interrupts: revision=r%02X value=%04X unknown=%04X\n",
				revision_config->family, value, unknown);
			last_unknown_mcu_interrupts = unknown;
		}
		return value;
	}

private:
	void ClearCommunicationFlagsFromDsp(uint16_t value) {
		uint16_t previous = communication_status.fetch_and((uint16_t) ~value, std::memory_order_acq_rel);
		uint16_t cleared = previous & value;
		if (cleared) {
			communication_cleared.fetch_or(cleared, std::memory_order_release);
			DPRINTF("DSP_CFR: value=%04X cleared=%04X pc=%05X\n", value, cleared, CurrentPC());
		}
	}

	uint32_t CurrentPC() const {
		const auto &registers = teakra->GetRegisterState();
		return registers.pc | (uint32_t) registers.prpage << 18;
	}

	void LoadProgramRom(const uint8_t *rom_data, uint32_t address, size_t words) {
		for (size_t i = 0; i < words; i++)
			teakra->ProgramWrite(address + i, ReadU16(rom_data + i * sizeof(uint16_t)));
	}

	void LoadDataRom(const uint8_t *rom_data, uint16_t address, size_t words) {
		for (size_t i = 0; i < words; i++)
			teakra->DataWrite((uint16_t) (address + i), ReadU16(rom_data + i * sizeof(uint16_t)), true);
	}

	void MapProgramBank(size_t bank) {
		if (active_program_bank == bank)
			return;
		if (bank >= revision_config->program_bank_count) {
			DPRINTF("unknown program ROM bank: revision=r%02X bank=%zu count=%zu\n",
				revision_config->family, bank, revision_config->program_bank_count);
			return;
		}

		size_t fixed_words = revision_config->program_bank_address - revision_config->program_rom_address;
		size_t bank_words = DSP_ADDRESS_SPACE_WORDS - revision_config->program_bank_address;
		const uint8_t *bank_data = program_rom + (fixed_words + bank * bank_words) * sizeof(uint16_t);
		LoadProgramRom(bank_data, revision_config->program_bank_address, bank_words);
		active_program_bank = bank;
	}

	void MapDataBank(size_t bank) {
		if (active_data_bank == bank)
			return;
		if (bank >= revision_config->data_bank_count) {
			DPRINTF("unknown data ROM bank: revision=r%02X bank=%zu count=%zu\n",
				revision_config->family, bank, revision_config->data_bank_count);
			return;
		}

		size_t fixed_words = revision_config->data_bank_address - revision_config->data_rom_address;
		size_t bank_words = revision_config->shared_base - revision_config->data_bank_address;
		const uint8_t *bank_data = data_rom + (fixed_words + bank * bank_words) * sizeof(uint16_t);
		LoadDataRom(bank_data, revision_config->data_bank_address, bank_words);
		active_data_bank = bank;
	}

	void UpdateBanks() {
		uint16_t page = teakra->MMIOPeek(DSP_PAGE_MMIO);
		if (page == active_page)
			return;

		uint16_t program_page_mask = revision_config->page_field_mask << revision_config->program_page_shift;
		uint16_t page_mask = program_page_mask | revision_config->page_field_mask;
		if ((page & ~page_mask))
			DPRINTF("unknown DSP page bits: revision=r%02X value=%04X unknown=%04X\n",
				revision_config->family, page, page & ~page_mask);
		MapProgramBank(page >> revision_config->program_page_shift & revision_config->page_field_mask);
		MapDataBank(page & revision_config->page_field_mask);
		active_page = page;
	}

	const DspRevisionConfig *revision_config;
	uint16_t rom_version;
	const uint8_t *program_rom;
	const uint8_t *data_rom;
	Teakra::UserConfig teakra_config;
	std::unique_ptr<Teakra::Teakra> teakra;
	size_t active_program_bank = SIZE_MAX;
	size_t active_data_bank = SIZE_MAX;
	size_t active_page = SIZE_MAX;
	uint16_t input_requests = 0;
	uint16_t external_interrupt_status = 0;
	uint16_t last_unknown_mcu_interrupts = 0;
	std::atomic<uint16_t> communication_status{ 0 };
	std::atomic<uint16_t> communication_cleared{ 0 };
	bool halted = false;
	bool initialized = false;
};

} // namespace

struct pmb887x_dsp_core_t {
	std::unique_ptr<DspCore> core;
	std::atomic<uint16_t> input_requests{ 0 };
	std::atomic<uint16_t> pending_requests{ 0 };
	uint16_t applied_requests = 0;
};

extern "C" {

uint32_t pmb887x_dsp_core_revision_family(uint32_t revision) {
	const DspRevisionConfig *config = FindRevisionConfig(revision);
	return config != nullptr ? config->family : 0;
}

uint16_t pmb887x_dsp_core_default_rom_version(uint32_t revision) {
	const DspRevisionConfig *config = FindRevisionConfig(revision);
	return config != nullptr ? config->default_rom_version : 0;
}

pmb887x_dsp_core_t *pmb887x_dsp_core_create(uint32_t revision, uint16_t rom_version, const uint8_t *program_rom,
	const uint8_t *data_rom)
{
	const DspRevisionConfig *config = FindRevisionConfig(revision);
	if (config == nullptr) {
		DPRINTF("unsupported DSP revision: %02X\n", revision);
		return nullptr;
	}
	if ((rom_version & ROM_VERSION_FAMILY_MASK) != (config->default_rom_version & ROM_VERSION_FAMILY_MASK)) {
		DPRINTF("ROM version does not match DSP revision: revision=%02X family=r%02X rom_version=%04X\n",
			revision, config->family, rom_version);
		return nullptr;
	}

	auto context = std::make_unique<pmb887x_dsp_core_t>();
	context->core = std::make_unique<DspCore>(config, rom_version, program_rom, data_rom);
	context->core->Reset();
	return context.release();
}

void pmb887x_dsp_core_destroy(pmb887x_dsp_core_t *core) {
	delete core;
}

void pmb887x_dsp_core_request_reset(pmb887x_dsp_core_t *core) {
	core->input_requests.store(0, std::memory_order_relaxed);
	core->pending_requests.store(0, std::memory_order_relaxed);
	core->core->ClearCommunicationFlags(UINT16_MAX);
}

void pmb887x_dsp_core_reset(pmb887x_dsp_core_t *core) {
	core->core->Reset();
	core->applied_requests = 0;
}

void pmb887x_dsp_core_run(pmb887x_dsp_core_t *core, size_t cycles, uint16_t *communication_clear,
	uint16_t *mcu_interrupts)
{
	uint16_t pending = core->pending_requests.exchange(0, std::memory_order_acquire);
	uint16_t requests = core->input_requests.load(std::memory_order_relaxed);
	uint16_t changed_requests = requests ^ core->applied_requests;
	for (size_t i = 0; i < REQUEST_INTERRUPTS.size(); i++) {
		uint16_t mask = (uint16_t) (1U << i);
		bool request_pending = pending & mask;
		bool request_applied = core->applied_requests & mask;
		bool request_changed = changed_requests & mask;
		if (request_pending) {
			if (request_applied)
				core->core->SetRequest(i, false);
			core->core->SetRequest(i, true);
			core->applied_requests |= mask;
		} else if (request_changed) {
			core->core->SetRequest(i, requests & mask);
			core->applied_requests ^= mask;
		}
	}

	core->core->Run(cycles, communication_clear);
	*mcu_interrupts = core->core->GetMcuInterrupts();
}

uint16_t pmb887x_dsp_core_get_communication_flags(pmb887x_dsp_core_t *core) {
	return core->core->GetCommunicationFlags();
}

uint16_t pmb887x_dsp_core_shared_read(pmb887x_dsp_core_t *core, uint16_t offset) {
	return core->core->SharedRead(offset);
}

void pmb887x_dsp_core_shared_write(pmb887x_dsp_core_t *core, uint16_t offset, uint16_t value) {
	core->core->SharedWrite(offset, value);
}

uint64_t pmb887x_dsp_core_shared_read_bytes(pmb887x_dsp_core_t *core, size_t offset, size_t size) {
	uint64_t value = 0;
	for (size_t i = 0; i < size;) {
		size_t byte_offset = offset + i;
		uint16_t word_offset = byte_offset / 2;
		uint16_t word = core->core->SharedRead(word_offset);
		bool full_word = byte_offset % 2 == 0 && size - i >= 2;
		if (full_word) {
			value |= (uint64_t) word << (i * 8);
			i += 2;
		} else {
			uint16_t shift = byte_offset % 2 * 8;
			uint8_t byte = word >> shift;
			value |= (uint64_t) byte << (i * 8);
			i++;
		}
	}
	return value;
}

void pmb887x_dsp_core_shared_write_bytes(pmb887x_dsp_core_t *core, size_t offset, uint64_t value, size_t size) {
	for (size_t i = 0; i < size;) {
		size_t byte_offset = offset + i;
		uint16_t word_offset = byte_offset / 2;
		bool full_word = byte_offset % 2 == 0 && size - i >= 2;
		if (full_word) {
			core->core->SharedWrite(word_offset, (uint16_t) (value >> (i * 8)));
			i += 2;
		} else {
			uint16_t shift = byte_offset % 2 * 8;
			uint16_t word = core->core->SharedRead(word_offset);
			uint8_t byte = value >> (i * 8);
			word &= ~(0xFF << shift);
			word |= (uint16_t) byte << shift;
			core->core->SharedWrite(word_offset, word);
			i++;
		}
	}
}

void pmb887x_dsp_core_set_request(pmb887x_dsp_core_t *core, size_t index, bool level) {
	if (index >= REQUEST_INTERRUPTS.size()) {
		DPRINTF("unknown DSP input interrupt: index=%zu level=%u\n", index, level);
		return;
	}

	uint16_t mask = (uint16_t) (1U << index);
	if (level) {
		core->input_requests.fetch_or(mask, std::memory_order_relaxed);
		core->pending_requests.fetch_or(mask, std::memory_order_release);
	} else {
		core->input_requests.fetch_and((uint16_t) ~mask, std::memory_order_relaxed);
	}
}

void pmb887x_dsp_core_set_communication_flags(pmb887x_dsp_core_t *core, uint16_t value) {
	core->core->SetCommunicationFlags(value);
}

void pmb887x_dsp_core_clear_communication_flags(pmb887x_dsp_core_t *core, uint16_t value) {
	core->core->ClearCommunicationFlags(value);
}

}
