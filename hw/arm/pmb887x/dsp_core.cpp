#define PMB887X_TRACE_ID		DSP
#define PMB887X_TRACE_PREFIX	"pmb887x-dsp"

#include "qemu/osdep.h"

#include <array>
#include <memory>
#include <vector>

#include "hw/arm/pmb887x/dsp_core.h"
#include "qemu/bswap.h"
#include "teakra/exceptions.h"
#include "teakra/impl/register.h"
#include "teakra/teakra.h"

extern "C" {
#include "hw/arm/pmb887x/trace.h"
}

namespace {

constexpr size_t DSP1_MAX_SEGMENTS = 10;
constexpr uint32_t DSP_REVISION_FAMILY_MASK = 0xF0;
constexpr uint16_t DSP_PAGE_MMIO = 0x00A3;
constexpr uint16_t DSP_ROM_PAGE_MASK = 0x0003;
constexpr unsigned DSP_PROGRAM_PAGE_SHIFT = 2;
constexpr uint16_t DSP_PAGE_MASK = DSP_ROM_PAGE_MASK << DSP_PROGRAM_PAGE_SHIFT | DSP_ROM_PAGE_MASK;
constexpr uint16_t COMMUNICATION_STATUS_MMIO = 0x0000;
constexpr uint16_t COMMUNICATION_REQUEST_MMIO = 0x0001;
constexpr uint16_t COMMUNICATION_CLEAR_MMIO = 0x0092;
constexpr uint16_t INTERRUPT_TO_MCU_MMIO = 0x0010;
constexpr uint16_t RUNTIME_PIPE_OFFSET = 0x0005;
constexpr uint16_t RUNTIME_PIPE_STRIDE = 0x001C;
constexpr uint16_t MCU_INTERRUPT_MASK = 0x000F;
constexpr uint16_t ROM_VERSION_FAMILY_MASK = 0xFF00;
constexpr std::array<uint8_t, 3> REQUEST_INTERRUPTS = { 0, 1, 0 };

struct QEMU_PACKED Dsp1SegmentEntry {
	uint32_t offset;
	uint32_t address;
	uint32_t size;
	uint8_t pa;
	uint8_t pb;
	uint8_t pc;
	uint8_t type;
	uint8_t sha256[32];
};

struct QEMU_PACKED Dsp1Header {
	uint8_t signature[0x100];
	uint8_t magic[4];
	uint32_t file_size;
	uint16_t memory_layout;
	uint16_t padding;
	uint8_t unknown;
	uint8_t filter_type;
	uint8_t segment_count;
	uint8_t flags;
	uint32_t filter_address;
	uint32_t filter_size;
	uint8_t reserved[8];
	Dsp1SegmentEntry segments[DSP1_MAX_SEGMENTS];
};

static_assert(sizeof(Dsp1SegmentEntry) == 0x30);
static_assert(sizeof(Dsp1Header) == 0x300);

struct DspRevisionConfig {
	uint32_t family;
	uint16_t default_rom_version;
	uint16_t program_rom_address;
	uint16_t program_bank_address;
	size_t program_bank_count;
	uint16_t data_rom_address;
	uint16_t data_bank_address;
	size_t data_bank_count;
	uint16_t shared_base;
	uint16_t mmio_base;
	uint16_t mmio_size;
};

constexpr std::array<DspRevisionConfig, 2> DSP_REVISION_CONFIGS = {{
	[]() constexpr {
		DspRevisionConfig config{};
		config.family = 0x10;
		config.default_rom_version = 0x0602;
		config.program_rom_address = 0x1000;
		config.program_bank_address = 0xB000;
		config.program_bank_count = 2;
		config.data_rom_address = 0x6000;
		config.data_bank_address = 0x9000;
		config.data_bank_count = 2;
		config.shared_base = 0xD800;
		config.mmio_base = 0xE600;
		config.mmio_size = 0x0100;
		return config;
	}(),
	[]() constexpr {
		DspRevisionConfig config{};
		config.family = 0x30;
		config.default_rom_version = 0x0801;
		config.program_rom_address = 0x2000;
		config.program_bank_address = 0xA000;
		config.program_bank_count = 3;
		config.data_rom_address = 0x8000;
		config.data_bank_address = 0x9000;
		config.data_bank_count = 4;
		config.shared_base = 0xD000;
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

struct Segment {
	uint32_t address;
	uint32_t words;
	uint8_t type;
	const uint8_t *data;
};

class DspCore {
public:
	DspCore(const DspRevisionConfig *revision_config, uint16_t rom_version, const uint8_t *image, size_t size) :
		revision_config(revision_config), rom_version(rom_version), image(image), image_size(size) {
		teakra_config.teaklite = true;
		teakra = std::make_unique<Teakra::Teakra>(teakra_config);
	}

	bool Parse() {
		if (image_size < sizeof(Dsp1Header))
			return false;
		const Dsp1Header *header = (const Dsp1Header *) image;
		if (memcmp(header->magic, "DSP1", sizeof(header->magic)) != 0 || le32_to_cpu(header->file_size) != image_size)
			return false;

		size_t count = header->segment_count;
		if (count == 0 || count > DSP1_MAX_SEGMENTS)
			return false;

		segments.reserve(count);
		program_banks.reserve(revision_config->program_bank_count);
		data_banks.reserve(revision_config->data_bank_count);
		bool has_program_rom = false;
		bool has_data_rom = false;
		for (size_t i = 0; i < count; i++) {
			const Dsp1SegmentEntry &entry = header->segments[i];
			uint32_t offset = le32_to_cpu(entry.offset);
			uint32_t address = le32_to_cpu(entry.address);
			uint32_t bytes = le32_to_cpu(entry.size);
			uint8_t type = entry.type;
			bool invalid = bytes == 0 || (bytes & 1) || address > UINT16_MAX || offset > image_size ||
				bytes > image_size - offset || type > 2;
			if (invalid)
				return false;

			segments.push_back({ address, bytes / 2, type, image + offset });

			const Segment &segment = segments.back();
			if (segment.type < 2 && segment.address == revision_config->program_bank_address)
				program_banks.push_back(&segment);
			else if (segment.type == 2 && segment.address == revision_config->data_bank_address)
				data_banks.push_back(&segment);
			else if (segment.type < 2 && segment.address == revision_config->program_rom_address)
				has_program_rom = true;
			else if (segment.type == 2 && segment.address == revision_config->data_rom_address)
				has_data_rom = true;
			else {
				DPRINTF("unknown DSP1 segment: type=%u address=%04X words=%u\n", segment.type,
					segment.address, segment.words);
				return false;
			}
		}

		bool valid = has_program_rom && has_data_rom &&
			program_banks.size() == revision_config->program_bank_count &&
			data_banks.size() == revision_config->data_bank_count;
		if (!valid)
			DPRINTF("DSP1 layout does not match r%02X: program_rom=%u data_rom=%u program_banks=%zu/%zu "
				"data_banks=%zu/%zu\n", revision_config->family, has_program_rom, has_data_rom,
				program_banks.size(), revision_config->program_bank_count, data_banks.size(),
				revision_config->data_bank_count);
		return valid;
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
		for (const Segment &segment : segments) {
			bool banked = segment.type < 2 ? segment.address == revision_config->program_bank_address :
				segment.address == revision_config->data_bank_address;
			if (!banked)
				LoadSegment(segment);
		}
		active_program_bank = UINT_MAX;
		active_data_bank = UINT_MAX;
		active_page = UINT_MAX;
		MapProgramBank(0);
		MapDataBank(0);
		for (size_t address = 0; address < program_ram.size(); address++)
			teakra->ProgramWrite(address, program_ram[address]);
		teakra->SetProgramWriteProtection(revision_config->program_rom_address,
			0x10000 - revision_config->program_rom_address);
		teakra->SetDataWriteProtection(revision_config->data_rom_address,
			revision_config->shared_base - revision_config->data_rom_address);
		auto &registers = teakra->GetRegisterState();
		registers.pc = revision_config->program_rom_address + 2;
		registers.prpage = 0;
		teakra->DataWrite(revision_config->shared_base, rom_version, true);
		input_requests = 0;
		latched_requests = 0;
		runtime_pending = 0;
		last_unknown_mcu_interrupts = 0;
		boot_mode = true;
		branch_pending = false;
		halted = false;
		initialized = true;
	}

	void Run(size_t cycles) {
		if (halted)
			return;
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
				return;
			} catch (const Teakra::UnimplementedException &error) {
				DPRINTF("unimplemented instruction: revision=r%02X pc=%05X opcode=%04X error=%s\n",
					revision_config->family, pc, opcode, error.what());
				halted = true;
				return;
			} catch (const std::exception &error) {
				DPRINTF("DSP core exception: revision=r%02X pc=%05X opcode=%04X error=%s\n",
					revision_config->family, pc, opcode, error.what());
				halted = true;
				return;
			}
			if (boot_mode && teakra->GetRegisterState().pc == (uint32_t) revision_config->program_rom_address + 0x51) {
				latched_requests = input_requests;
				teakra->MMIOWrite(COMMUNICATION_STATUS_MMIO, input_requests);
			}
			UpdateRuntimeRequests();
		}
	}

	uint16_t SharedRead(uint16_t offset) {
		return teakra->DataRead(revision_config->shared_base + offset, true);
	}

	void SharedWrite(uint16_t offset, uint16_t value) {
		teakra->DataWrite(revision_config->shared_base + offset, value, true);
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
		latched_requests |= mask;
		teakra->MMIOWrite(COMMUNICATION_STATUS_MMIO, latched_requests);
		if (!boot_mode) {
			teakra->MMIOWrite(COMMUNICATION_REQUEST_MMIO, latched_requests);
			runtime_commands[index] = SharedRead(RUNTIME_PIPE_OFFSET + index * RUNTIME_PIPE_STRIDE);
			runtime_stack[index] = teakra->GetRegisterState().sp;
			runtime_entered[index] = false;
			runtime_pending |= mask;
			teakra->SignalInterrupt(REQUEST_INTERRUPTS[index]);
		}
	}

	void SetBootMode(bool value) {
		boot_mode = value;
		branch_pending = !value;
	}

	uint16_t TakeCommunicationClear() {
		uint16_t value = teakra->MMIOPeek(COMMUNICATION_CLEAR_MMIO);
		if (value == 0)
			return 0;
		teakra->MMIOWrite(COMMUNICATION_CLEAR_MMIO, 0);
		if (branch_pending) {
			latched_requests &= ~value;
			teakra->MMIOWrite(COMMUNICATION_STATUS_MMIO, latched_requests);
			branch_pending = false;
		}
		return value;
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

	uint32_t GetPC() const {
		return CurrentPC();
	}

private:
	uint32_t CurrentPC() const {
		const auto &registers = teakra->GetRegisterState();
		return registers.pc | (uint32_t) registers.prpage << 18;
	}

	void LoadSegment(const Segment &segment) {
		for (uint32_t i = 0; i < segment.words; i++) {
			uint16_t value = ReadU16(segment.data + i * 2);
			if (segment.type < 2)
				teakra->ProgramWrite(segment.address + i, value);
			else
				teakra->DataWrite((uint16_t) (segment.address + i), value, true);
		}
	}

	void MapProgramBank(unsigned bank) {
		if (active_program_bank == bank)
			return;
		if (bank >= program_banks.size()) {
			DPRINTF("unknown program ROM bank: revision=r%02X bank=%u count=%zu\n",
				revision_config->family, bank, program_banks.size());
			return;
		}
		LoadSegment(*program_banks[bank]);
		active_program_bank = bank;
	}

	void MapDataBank(unsigned bank) {
		if (active_data_bank == bank)
			return;
		if (bank >= data_banks.size()) {
			DPRINTF("unknown data ROM bank: revision=r%02X bank=%u count=%zu\n",
				revision_config->family, bank, data_banks.size());
			return;
		}
		LoadSegment(*data_banks[bank]);
		active_data_bank = bank;
	}

	void UpdateBanks() {
		uint16_t page = teakra->MMIOPeek(DSP_PAGE_MMIO);
		if (page == active_page)
			return;
		if ((page & ~DSP_PAGE_MASK))
			DPRINTF("unknown DSP page bits: revision=r%02X value=%04X unknown=%04X\n",
				revision_config->family, page, page & ~DSP_PAGE_MASK);
		MapProgramBank(page >> DSP_PROGRAM_PAGE_SHIFT & DSP_ROM_PAGE_MASK);
		MapDataBank(page & DSP_ROM_PAGE_MASK);
		active_page = page;
	}

	void UpdateRuntimeRequests() {
		if (boot_mode || runtime_pending == 0)
			return;
		uint16_t completed = 0;
		for (size_t i = 0; i < runtime_commands.size(); i++) {
			uint16_t mask = (uint16_t) 1U << i;
			if (!(runtime_pending & mask))
				continue;
			runtime_entered[i] |= teakra->GetRegisterState().sp != runtime_stack[i];
			uint16_t response = SharedRead(RUNTIME_PIPE_OFFSET + i * RUNTIME_PIPE_STRIDE);
			if (response != runtime_commands[i] || (runtime_entered[i] && teakra->GetRegisterState().sp == runtime_stack[i]))
				completed |= mask;
		}
		if (completed == 0)
			return;
		runtime_pending &= ~completed;
		latched_requests &= ~completed;
		teakra->MMIOWrite(COMMUNICATION_STATUS_MMIO, latched_requests);
		teakra->MMIOWrite(COMMUNICATION_REQUEST_MMIO, latched_requests);
	}

	const DspRevisionConfig *revision_config;
	uint16_t rom_version;
	const uint8_t *image;
	size_t image_size;
	Teakra::UserConfig teakra_config;
	std::unique_ptr<Teakra::Teakra> teakra;
	std::vector<Segment> segments;
	std::vector<const Segment *> program_banks;
	std::vector<const Segment *> data_banks;
	unsigned active_program_bank = UINT_MAX;
	unsigned active_data_bank = UINT_MAX;
	unsigned active_page = UINT_MAX;
	uint16_t input_requests = 0;
	uint16_t latched_requests = 0;
	std::array<uint16_t, 3> runtime_commands{};
	std::array<uint16_t, 3> runtime_stack{};
	std::array<bool, 3> runtime_entered{};
	uint16_t runtime_pending = 0;
	uint16_t last_unknown_mcu_interrupts = 0;
	bool boot_mode = true;
	bool branch_pending = false;
	bool halted = false;
	bool initialized = false;
};

} // namespace

struct pmb887x_dsp_core_t {
	std::unique_ptr<DspCore> core;
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

pmb887x_dsp_core_t *pmb887x_dsp_core_create(uint32_t revision, uint16_t rom_version, const uint8_t *image,
	size_t size) {
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
	context->core = std::make_unique<DspCore>(config, rom_version, image, size);
	if (!context->core->Parse())
		return nullptr;
	context->core->Reset();
	return context.release();
}

void pmb887x_dsp_core_destroy(pmb887x_dsp_core_t *core) {
	delete core;
}

void pmb887x_dsp_core_reset(pmb887x_dsp_core_t *core) {
	core->core->Reset();
}

void pmb887x_dsp_core_run(pmb887x_dsp_core_t *core, size_t cycles) {
	core->core->Run(cycles);
}

uint16_t pmb887x_dsp_core_shared_read(pmb887x_dsp_core_t *core, uint16_t offset) {
	return core->core->SharedRead(offset);
}

void pmb887x_dsp_core_shared_write(pmb887x_dsp_core_t *core, uint16_t offset, uint16_t value) {
	core->core->SharedWrite(offset, value);
}

void pmb887x_dsp_core_set_request(pmb887x_dsp_core_t *core, size_t index, bool level) {
	core->core->SetRequest(index, level);
}

void pmb887x_dsp_core_set_boot_mode(pmb887x_dsp_core_t *core, bool boot_mode) {
	core->core->SetBootMode(boot_mode);
}

uint16_t pmb887x_dsp_core_take_com_clear(pmb887x_dsp_core_t *core) {
	return core->core->TakeCommunicationClear();
}

uint16_t pmb887x_dsp_core_get_mcu_interrupts(pmb887x_dsp_core_t *core) {
	return core->core->GetMcuInterrupts();
}

uint32_t pmb887x_dsp_core_get_pc(pmb887x_dsp_core_t *core) {
	return core->core->GetPC();
}

}
