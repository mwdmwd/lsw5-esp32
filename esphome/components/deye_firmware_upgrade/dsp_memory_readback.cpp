// SPDX-License-Identifier: GPL-3.0-or-later
#include "deye_firmware_upgrade.h"

#include <algorithm>

namespace esphome::deye_firmware_upgrade
{
namespace
{
constexpr uint32_t DSP_POINTER0 = 0xDF9C;
constexpr uint32_t DSP_SENTINEL = 0x84000;
constexpr uint16_t DSP_SENTINEL_WORD = 0x0049;
constexpr uint32_t DSP_STAGE_MS = 30000;
constexpr uint32_t DSP_POLL_MS = 300;
constexpr uint32_t DSP_QUIET_MS = 20;

bool dsp_due(uint32_t now, uint32_t deadline)
{
	return static_cast<int32_t>(now - deadline) >= 0;
}
uint16_t dsp_word(std::vector<uint8_t> const &reply, size_t index)
{
	return uint16_t(reply[3 + 2 * index]) << 8 | reply[4 + 2 * index];
}
bool dsp_tagged_reply(std::vector<uint8_t> const &reply, uint32_t address)
{
	return dsp_word(reply, 0) == dsp_word(reply, 1) &&
	       dsp_word(reply, 2) == uint16_t(address) &&
	       dsp_word(reply, 3) == uint16_t(address) &&
	       dsp_word(reply, 4) == address >> 16 &&
	       dsp_word(reply, 5) == address >> 16;
}
} // namespace

void DeyeFirmwareUpgrade::dsp_read_stage_(DspReadStage stage, uint32_t now)
{
	this->dspReadStage_ = stage;
	this->dspStageDeadline_ = now + DSP_STAGE_MS;
	this->dspNextRequest_ = now;
}

void DeyeFirmwareUpgrade::start_dsp_readback_(uint32_t now)
{
	this->dspTouched_ = false;
	this->dspStopping_ = false;
	this->dspSucceeded_ = false;
	this->dspProbeIndex_ = 0;
	this->dspConfigureIndex_ = 0;
	this->dspRestoreAttempts_ = 0;
	this->dspPointers_ = {};
	this->dspResult_.clear();
	this->dspRequest_.clear();
	this->dspReply_.clear();
	this->memoryRead_.message = "Checking DSP 1175 and initializing address tags.";
	this->dsp_read_stage_(DspReadStage::IDENTITY, now);
}

void DeyeFirmwareUpgrade::stop_dsp_readback_(bool success, char const *message, uint32_t now)
{
	this->restore_bus_(false);
	this->readbackPhase_ = ReadbackPhase::YIELDING;
	if(!this->dspTouched_)
	{
		this->finish_memory_readback_(success, message);
		return;
	}
	if(!this->dspStopping_)
	{
		this->dspResult_ = message;
		this->dspSucceeded_ = success;
		this->dspStopping_ = true;
		this->dspRestoreAttempts_ = 0;
	}
	// Only mode-zero restoration is retried. Additive pointer writes never are.
	if(++this->dspRestoreAttempts_ > 3)
	{
		this->dspResult_ += " DSP diagnostics restoration was not confirmed; start "
		                    "another DSP read to retry it.";
		this->finish_memory_readback_(false, this->dspResult_.c_str());
		return;
	}
	this->memoryRead_.message = "Restoring normal DSP diagnostics; partial capture retained.";
	this->dsp_read_stage_(DspReadStage::RESTORE, now);
}

void DeyeFirmwareUpgrade::dsp_read_request_()
{
	uint16_t reg = 560;
	uint16_t count = 2;
	bool write = false;
	uint32_t value = 0;
	switch(this->dspReadStage_)
	{
		case DspReadStage::IDENTITY:
			reg = 15;
			count = 1;
			break;
		case DspReadStage::DISABLE:
		case DspReadStage::RESTORE:
			write = true;
			reg = 584;
			break;
		case DspReadStage::ENABLE:
			write = true;
			reg = 584;
			value = 0x00010000;
			break;
		case DspReadStage::WAIT_DISABLED:
		case DspReadStage::WAIT_ENABLED:
		case DspReadStage::WAIT_RESTORED:
			reg = 584;
			count = 1;
			break;
		case DspReadStage::SENTINEL:
		case DspReadStage::CONFIG_SENTINEL:
			write = true;
			value = DSP_SENTINEL;
			break;
		case DspReadStage::PROBE:
			write = true;
			value = DSP_POINTER0 + 2 + this->dspProbeIndex_;
			break;
		case DspReadStage::CONFIGURE:
			write = true;
			reg = 562 + 2 * this->dspConfigureIndex_;
			value = DSP_POINTER0 + this->dspConfigureIndex_;
			break;
		case DspReadStage::ADDRESS:
			write = true;
			value = this->memoryRead_.address + this->memoryRead_.received / 2;
			break;
		case DspReadStage::WAIT_SENTINEL:
		case DspReadStage::DATA:
			count = 6;
			break;
		default:
			break;
	}
	this->dspRequest_ = { uint8_t(write ? 0xAA : this->readbackUnit_),
	                      uint8_t(write ? 0x10 : 3),
	                      uint8_t(reg >> 8),
	                      uint8_t(reg),
	                      uint8_t(count >> 8),
	                      uint8_t(count) };
	if(write)
	{
		this->dspTouched_ = true;
		this->memoryRead_.diagnostics_restored = false;
		std::array<uint8_t, 5> const payload = { 4, uint8_t(value >> 24), uint8_t(value >> 16),
		                                         uint8_t(value >> 8), uint8_t(value) };
		this->dspRequest_.insert(this->dspRequest_.end(), payload.begin(), payload.end());
	}
	auto const crc = crc16_modbus(this->dspRequest_.data(), this->dspRequest_.size());
	this->dspRequest_.push_back(uint8_t(crc));
	this->dspRequest_.push_back(uint8_t(crc >> 8));
	this->dspReply_.clear();
}

void DeyeFirmwareUpgrade::dsp_read_reply_(uint32_t now)
{
	auto next = [this, now](DspReadStage stage) { this->dsp_read_stage_(stage, now); };
	// Polling the same stage never renews its deadline.
	this->dspNextRequest_ = now + DSP_POLL_MS;
	auto const &r = this->dspReply_;
	auto const pair = [&r](uint16_t value)
	{ return dsp_word(r, 0) == value && dsp_word(r, 1) == value; };
	switch(this->dspReadStage_)
	{
		case DspReadStage::IDENTITY:
			if(dsp_word(r, 0) != 0x1175)
				this->stop_dsp_readback_(false, "This reader requires DSP firmware 1175.", now);
			else
				next(DspReadStage::DISABLE);
			break;
		case DspReadStage::DISABLE:
			this->dspRestoreAfter_ = now + 1500;
			next(DspReadStage::WAIT_DISABLED);
			break;
		case DspReadStage::WAIT_DISABLED:
			if(dsp_due(now, this->dspRestoreAfter_) && dsp_word(r, 0) == 0)
				next(DspReadStage::ENABLE);
			break;
		case DspReadStage::ENABLE:
			next(DspReadStage::WAIT_ENABLED);
			break;
		case DspReadStage::WAIT_ENABLED:
			if(dsp_word(r, 0) == 1)
				next(DspReadStage::SENTINEL);
			break;
		case DspReadStage::SENTINEL:
			next(DspReadStage::WAIT_SENTINEL);
			break;
		case DspReadStage::WAIT_SENTINEL:
			if(!pair(DSP_SENTINEL_WORD))
				break;
			// Reuse matching address tags from a previous capture. Do not add to
			// their pointers again; every subsequent word still needs both tags.
			if(this->dspProbeIndex_ != 0 || !dsp_tagged_reply(r, DSP_SENTINEL))
			{
				next(DspReadStage::PROBE);
				break;
			}
			if(this->memoryRead_.address != DSP_SENTINEL)
			{
				next(DspReadStage::ADDRESS);
				break;
			}
			// The verified sentinel is also the first application word. Retain
			// this sample instead of assigning and waiting for the same word again.
			[[fallthrough]];
		case DspReadStage::DATA:
		{
			uint32_t const address = this->memoryRead_.address + this->memoryRead_.received / 2;
			if(!dsp_tagged_reply(r, address))
				break;
			uint16_t const word = dsp_word(r, 0);
			this->memoryRead_.data.get()[this->memoryRead_.received++] = uint8_t(word);
			this->memoryRead_.data.get()[this->memoryRead_.received++] = uint8_t(word >> 8);
			if(this->memoryRead_.received == this->memoryRead_.length)
				this->stop_dsp_readback_(true, "Capture complete.", now);
			else
				next(DspReadStage::ADDRESS);
			break;
		}
		case DspReadStage::PROBE:
			next(DspReadStage::WAIT_PROBE);
			break;
		case DspReadStage::WAIT_PROBE:
		{
			// Bootstrap alternates a known nonzero flash word with the pointer
			// table. Neither accepted pointer representation equals the sentinel,
			// so stale cached data cannot pass the pointer-initialization check.
			unsigned const slot = this->dspProbeIndex_ / 2;
			bool const high = (this->dspProbeIndex_ & 1) != 0;
			uint16_t const expected = high ? 0 : DSP_POINTER0 + slot;
			if(!pair(0) && !pair(expected))
				break;
			this->dspPointers_[slot] |= uint32_t(dsp_word(r, 0)) << (high ? 16 : 0);
			if(++this->dspProbeIndex_ < 4)
				next(DspReadStage::SENTINEL);
			else
			{
				this->dspConfigureIndex_ = this->dspPointers_[0] == DSP_POINTER0 ? 1 : 0;
				if(this->dspConfigureIndex_ == 1 && this->dspPointers_[1] == DSP_POINTER0 + 1)
					next(DspReadStage::ADDRESS);
				else
					next(DspReadStage::CONFIG_SENTINEL);
			}
			break;
		}
		case DspReadStage::CONFIG_SENTINEL:
			next(DspReadStage::WAIT_CONFIG_SENTINEL);
			break;
		case DspReadStage::WAIT_CONFIG_SENTINEL:
			if(pair(DSP_SENTINEL_WORD))
				next(DspReadStage::CONFIGURE);
			break;
		case DspReadStage::CONFIGURE:
			next(DspReadStage::WAIT_CLEARED);
			break;
		case DspReadStage::WAIT_CLEARED:
			// The source-0 clear proves this particular write was processed.
			// The following DATA stage also checks both configured address tags.
			if(pair(0))
			{
				if(this->dspConfigureIndex_ == 0 && this->dspPointers_[1] == 0)
				{
					this->dspConfigureIndex_ = 1;
					next(DspReadStage::CONFIG_SENTINEL);
				}
				else
					next(DspReadStage::ADDRESS);
			}
			break;
		case DspReadStage::ADDRESS:
			this->memoryRead_.message = "Reading DSP flash; waiting for matching address tags.";
			next(DspReadStage::DATA);
			break;
		case DspReadStage::RESTORE:
			this->dspRestoreAfter_ = now + 1500;
			next(DspReadStage::WAIT_RESTORED);
			break;
		case DspReadStage::WAIT_RESTORED:
			if(dsp_due(now, this->dspRestoreAfter_) && dsp_word(r, 0) == 0)
			{
				this->memoryRead_.diagnostics_restored = true;
				this->dspResult_ += " Normal DSP diagnostics restored.";
				this->finish_memory_readback_(this->dspSucceeded_, this->dspResult_.c_str());
			}
			break;
	}
}

void DeyeFirmwareUpgrade::service_dsp_readback_(uint32_t now)
{
	if(this->readbackPhase_ == ReadbackPhase::YIELDING)
	{
		if(this->readbackCancel_ && !this->dspStopping_)
		{
			this->stop_dsp_readback_(false, "Cancelled; partial capture retained.", now);
			return;
		}
		if(dsp_due(now, this->dspStageDeadline_))
		{
			this->stop_dsp_readback_(false, "DSP readback timed out waiting for verified state.",
			                         now);
			return;
		}
		if(!dsp_due(now, this->dspNextRequest_) ||
		   !this->modbus_controller_->is_in_loop_state() ||
		   !this->modbus_->is_in_loop_state() ||
		   this->modbus_controller_->get_command_queue_length() != 0 ||
		   !this->modbus_->tx_buffer_empty() ||
		   this->modbus_->tx_blocked() ||
		   this->available())
			return;
		this->dsp_read_request_();
		this->take_bus_();
		this->write_array(this->dspRequest_.data(), this->dspRequest_.size());
		this->readbackDeadline_ = now + 1500 + (this->dspRequest_.size() * 10000 + 9599) / 9600;
		this->readbackPhase_ = ReadbackPhase::WAITING;
		return;
	}
	if(this->readbackPhase_ == ReadbackPhase::SETTLING)
	{
		if(this->available())
		{
			this->drain_uart_();
			this->readbackQuietUntil_ = now + DSP_QUIET_MS;
			this->readbackPhase_ = ReadbackPhase::STOPPING;
			return;
		}
		if(!dsp_due(now, this->readbackQuietUntil_))
			return;
		this->restore_bus_(false);
		this->readbackPhase_ = ReadbackPhase::YIELDING;
		if(this->readbackCancel_ && !this->dspStopping_)
			this->stop_dsp_readback_(false, "Cancelled; partial capture retained.", now);
		else
			this->dsp_read_reply_(now);
		return;
	}
	if(this->readbackPhase_ == ReadbackPhase::STOPPING)
	{
		if(this->available())
		{
			this->drain_uart_();
			this->readbackQuietUntil_ = now + DSP_QUIET_MS;
		}
		if(dsp_due(now, this->readbackQuietUntil_) || dsp_due(now, this->readbackDeadline_ + 1000))
		{
			if(this->dspStopping_ || this->readbackCancel_)
				this->stop_dsp_readback_(false, "DSP transport failed; partial capture retained.",
				                         now);
			else
			{
				this->restore_bus_(false);
				this->readbackPhase_ = ReadbackPhase::YIELDING;
				if(this->dspReadStage_ == DspReadStage::CONFIGURE)
				{
					// An absent echo does not mean the additive write was lost.
					// Observe its source-0 clear and the eventual address tags.
					this->dsp_read_stage_(DspReadStage::WAIT_CLEARED, now);
				}
				// Every other request is a read or an absolute assignment. Retry
				// only within the original stage deadline; DATA remains tagged.
				this->dspNextRequest_ = now + DSP_POLL_MS;
			}
		}
		return;
	}
	bool const write = this->dspRequest_[1] == 0x10;
	size_t const expected = write ? this->dspRequest_.size() : 5 + 2 * this->dspRequest_[5];
	for(unsigned i = 0; i < 256 && this->available(); ++i)
	{
		uint8_t byte;
		if(!this->read_byte(&byte))
			break;
		this->dspReply_.push_back(byte);
		if(this->dspReply_.size() < expected)
			continue;
		auto const &reply = this->dspReply_;
		bool const valid = write ? reply == this->dspRequest_
		                         : reply[0] == this->readbackUnit_ &&
		                               reply[1] == 3 &&
		                               reply[2] == 2 * this->dspRequest_[5] &&
		                               crc16_modbus(reply.data(), reply.size()) == 0;
		if(valid)
		{
			this->readbackPhase_ = ReadbackPhase::SETTLING;
			this->readbackQuietUntil_ = now + DSP_QUIET_MS;
			return;
		}
		this->dspReply_.erase(this->dspReply_.begin());
	}
	if(dsp_due(now, this->readbackDeadline_))
	{
		this->readbackPhase_ = ReadbackPhase::STOPPING;
		this->readbackQuietUntil_ = now + DSP_QUIET_MS;
	}
}
} // namespace esphome::deye_firmware_upgrade
