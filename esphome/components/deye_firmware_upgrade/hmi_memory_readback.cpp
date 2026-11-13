// SPDX-License-Identifier: GPL-3.0-or-later
#include "deye_firmware_upgrade.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/log.h"
#include "memory_console.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace esphome::deye_firmware_upgrade
{
// Raw readback bypasses Modbus's receive timestamps. Keep a post-reply guard
// before returning the UART to any sender, including normal Modbus polling.
static constexpr uint32_t READ_QUIET_MS = 20;
static constexpr uint32_t READ_REPLY_MS = 1000;

static bool readback_due(uint32_t now, uint32_t deadline)
{
	return static_cast<int32_t>(now - deadline) >= 0;
}

static std::string readback_url(AsyncWebServerRequest *request)
{
	char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
	return request->url_to(buffer);
}

// Strict unsigned decimal / 0x-prefixed hex.
static bool readback_number(std::string const &text, uint32_t &result)
{
	size_t index = 0;
	unsigned base = 10;
	if(text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
	{
		base = 16;
		index = 2;
	}
	if(index == text.size())
		return false;
	result = 0;
	for(; index < text.size(); ++index)
	{
		char const c = text[index];
		unsigned const digit = c >= '0' && c <= '9'   ? c - '0'
		                       : c >= 'a' && c <= 'f' ? c - 'a' + 10
		                       : c >= 'A' && c <= 'F' ? c - 'A' + 10
		                                              : 255;
		if(digit >= base || result > (UINT32_MAX - digit) / base)
			return false;
		result = result * base + digit;
	}
	return true;
}

static void readback_headers(AsyncWebServerResponse *response)
{
	response->addHeader("Cache-Control", "no-store");
	response->addHeader("X-Content-Type-Options", "nosniff");
	response->addHeader(
	    "Content-Security-Policy",
	    "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
	    "connect-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
}

class MemoryReadRequestHandler final : public AsyncWebHandler
{
public:
	explicit MemoryReadRequestHandler(DeyeFirmwareUpgrade *parent) : parent_(parent)
	{
	}
	bool canHandle(AsyncWebServerRequest *request) const override
	{
		auto const url = readback_url(request);
		return (request->method() == HTTP_GET && (url == "/memory" ||
		                                          url == "/memory/status" ||
		                                          url == "/memory/data" ||
		                                          url == "/memory/download")) ||
		       (request->method() == HTTP_POST &&
		        (url == "/memory/read" || url == "/memory/cancel" || url == "/memory/clear"));
	}
	bool isRequestHandlerTrivial() const override
	{
		return false;
	}
	void handleRequest(AsyncWebServerRequest *request) override
	{
		auto const url = readback_url(request);
		if(url == "/memory")
		{
			auto *response =
			    request->beginResponse(200, "text/html; charset=utf-8",
				                       reinterpret_cast<uint8_t const *>(MEMORY_CONSOLE_HTML),
				                       sizeof(MEMORY_CONSOLE_HTML) - 1);
			readback_headers(response);
			request->send(response);
			return;
		}
		char const *error = nullptr;
		if(request->method() == HTTP_POST)
		{
			uint32_t address, length, jobId;
			if(url == "/memory/read")
			{
				if(!readback_number(request->arg("address"), address) ||
				   !readback_number(request->arg("length"), length))
					error = "Use decimal or 0x-prefixed hexadecimal address and length.";
				else
					error = this->parent_->request_memory_read(address, length);
			}
			else if(!readback_number(request->arg("job"), jobId))
				error = "Missing capture ID.";
			else if(url == "/memory/cancel")
			{
				if(!this->parent_->cancel_memory_read(jobId))
					error = "That capture is no longer active.";
			}
			else if(!this->parent_->clear_memory_read(jobId))
				error = "Cancel the active capture before clearing it.";
		}
		// Keep the buffer alive even if another request replaces the capture.
		auto const s = this->parent_->memory_snapshot();
		if(url == "/memory/download" || url == "/memory/data")
		{
			uint32_t jobId, offset = 0, length = s.received;
			if(!readback_number(request->arg("job"), jobId) || jobId != s.jobId || !s.data)
				error = "Capture has been replaced or cleared.";
			else if(url == "/memory/data" && (!readback_number(request->arg("offset"), offset) ||
			                                  !readback_number(request->arg("length"), length) ||
			                                  length > 4096))
				error = "Invalid page; maximum 4096 bytes.";
			else if(!length || offset > s.received || length > s.received - offset)
				error = "Those bytes have not been captured yet.";
			if(!error)
			{
				auto *response = request->beginResponse(200, "application/octet-stream",
				                                        s.data.get() + offset, length);
				std::string disposition;
				if(url == "/memory/download")
				{
					std::ostringstream name;
					name << "attachment; filename=\"hmi-" << std::hex << std::setfill('0')
					     << std::setw(8) << s.address << '-' << std::dec << length
					     << (s.complete ? ".bin\"" : ".partial.bin\"");
					disposition = name.str();
					response->addHeader("Content-Disposition", disposition.c_str());
				}
				readback_headers(response);
				// ESP-IDF sends synchronously; the snapshot owns the buffer until send returns.
				request->send(response);
				return;
			}
		}
		std::ostringstream body;
		// Strings here are fixed local messages, never reflected user input.
		body << "{\"job_id\":" << s.jobId << ",\"active\":" << (s.active ? "true" : "false")
		     << ",\"complete\":" << (s.complete ? "true" : "false") << ",\"address\":" << s.address
		     << ",\"length\":" << s.length << ",\"received\":" << s.received
		     << ",\"unit\":" << unsigned(s.unit) << ",\"elapsed_ms\":" << s.elapsedMs
		     << ",\"flash_size\":" << s.flashSize << ",\"message\":\"" << s.message
		     << "\",\"error\":\"" << (error ? error : "") << "\"}";
		// ESPHome 2026.6's IDF adapter maps 202 (and 400) to 500. Use supported codes.
		auto *response = request->beginResponse(error ? 409 : 200, "application/json", body.str());
		readback_headers(response);
		request->send(response);
	}

private:
	DeyeFirmwareUpgrade *parent_;
};

void DeyeFirmwareUpgrade::setup_memory_readback_()
{
	web_server_base::global_web_server_base->add_handler(new MemoryReadRequestHandler(this));
}

MemoryReadSnapshot DeyeFirmwareUpgrade::memory_snapshot()
{
	LockGuard lock(this->mutex_);
	auto result = this->memoryRead_;
	result.flashSize = this->readbackFlashSize_;
	if(result.active)
		result.elapsedMs = millis() - this->readbackStarted_;
	return result;
}

char const *DeyeFirmwareUpgrade::request_memory_read(uint32_t address, size_t length)
{
	LockGuard lock(this->mutex_);
	if(this->memoryRead_.active ||
	   this->raw_modbus_.active ||
	   state_is_active_(this->state_) ||
	   this->upload_owner_ != nullptr)
		return "Another read, Modbus request, upload or upgrade is active.";
	if(this->readbackUnit_ == 0 || this->readbackUnit_ > 247)
		return "Invalid HMI unit address.";
	if(!length || uint64_t(address) + length > (uint64_t(1) << 32))
		return "Length must be nonzero and the end address must fit in 32 bits.";
	RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
	auto *buffer = allocator.allocate(length);
	if(buffer == nullptr)
		return "Not enough PSRAM; download and clear the previous capture first.";
	std::shared_ptr<uint8_t> data(buffer,
	                              [length](uint8_t *p)
	                              {
		                              RAMAllocator<uint8_t> allocator(
		                                  RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
		                              allocator.deallocate(p, length);
	                              });
	uint32_t const jobId = this->memoryRead_.jobId + 1;
	this->memoryRead_ = MemoryReadSnapshot{};
	this->memoryRead_.jobId = jobId;
	this->memoryRead_.address = address;
	this->memoryRead_.length = length;
	this->memoryRead_.unit = this->readbackUnit_;
	this->memoryRead_.data = std::move(data);
	this->memoryRead_.active = true;
	this->memoryRead_.message = "Waiting for an idle Modbus slot.";
	this->readbackStarted_ = millis();
	this->readbackCancel_ = false;
	this->readbackPhase_ = ReadbackPhase::YIELDING;
	return nullptr;
}

bool DeyeFirmwareUpgrade::cancel_memory_read(uint32_t jobId)
{
	LockGuard lock(this->mutex_);
	if(jobId != this->memoryRead_.jobId || !this->memoryRead_.active)
		return false;
	this->readbackCancel_ = true;
	return true;
}

bool DeyeFirmwareUpgrade::clear_memory_read(uint32_t jobId)
{
	LockGuard lock(this->mutex_);
	if(jobId != this->memoryRead_.jobId || this->memoryRead_.active)
		return false;
	this->memoryRead_ = MemoryReadSnapshot{};
	this->memoryRead_.jobId = jobId + 1;
	this->memoryRead_.unit = this->readbackUnit_;
	return true;
}

// Main loop only, with the admission mutex held.
void DeyeFirmwareUpgrade::finish_memory_readback_(bool success, char const *message)
{
	this->restore_bus_(false);
	this->readbackPhase_ = ReadbackPhase::IDLE;
	this->memoryRead_.elapsedMs = millis() - this->readbackStarted_;
	this->memoryRead_.complete = success;
	this->memoryRead_.message = message;
	this->memoryRead_.active = false;
	ESP_LOGI("hmi_memory_readback", "%s (%zu/%zu bytes)", message, this->memoryRead_.received,
	         this->memoryRead_.length);
}

void DeyeFirmwareUpgrade::service_memory_readback_(uint32_t now)
{
	LockGuard lock(this->mutex_);
	if(this->readbackCancel_ && this->readbackPhase_ != ReadbackPhase::STOPPING)
	{
		if(!this->bus_owned_)
		{
			this->finish_memory_readback_(false, "Cancelled; partial capture retained.");
			return;
		}
		this->readbackPhase_ = ReadbackPhase::STOPPING;
		this->readbackDeadline_ = now + READ_REPLY_MS;
		this->memoryRead_.message = "Cancelling; draining the pending reply.";
	}
	if(this->readbackPhase_ == ReadbackPhase::STOPPING)
	{
		this->drain_uart_();
		if(readback_due(now, this->readbackDeadline_))
			this->finish_memory_readback_(false, "Cancelled; partial capture retained.");
		return;
	}
	if(this->readbackPhase_ == ReadbackPhase::SETTLING)
	{
		if(this->available())
		{
			this->finish_memory_readback_(false,
			                              "Unexpected UART data after a reply; capture stopped.");
			return;
		}
		if(!readback_due(now, this->readbackQuietUntil_))
			return;
		// Release after one word. Do not stop/restart pollers: their existing cadence survives.
		this->restore_bus_(false);
		if(this->memoryRead_.received == this->memoryRead_.length)
			this->finish_memory_readback_(true, "Capture complete.");
		else
		{
			this->readbackPhase_ = ReadbackPhase::YIELDING;
			this->memoryRead_.message = "Reading in the background; normal polling continues.";
		}
		// Return to the scheduler before borrowing again. Pollers keep running and
		// queued work wins below; an additional fixed idle delay only wastes bandwidth.
		return;
	}
	if(this->readbackPhase_ == ReadbackPhase::YIELDING)
	{
		// Normal transactions always win. Borrow only an idle UART for one request/reply.
		if(!this->modbus_controller_->is_in_loop_state() ||
		   !this->modbus_->is_in_loop_state() ||
		   this->modbus_controller_->get_command_queue_length() != 0 ||
		   !this->modbus_->tx_buffer_empty() ||
		   this->modbus_->tx_blocked() ||
		   this->available())
			return;
		this->take_bus_();
		uint32_t const address =
		    (this->memoryRead_.address + this->memoryRead_.received) & ~uint32_t(3);
		std::array<uint8_t, 8> request = { uint8_t(this->readbackUnit_ == 1 ? 2 : 1),
		                                   5,
		                                   uint8_t(address >> 24),
		                                   uint8_t(address >> 16),
		                                   uint8_t(address >> 8),
		                                   uint8_t(address),
		                                   0,
		                                   0 };
		uint16_t const crc = crc16_modbus(request.data(), 6);
		request[6] = uint8_t(crc);
		request[7] = uint8_t(crc >> 8);
		this->readbackFrameLength_ = 0;
		this->write_array(request);
		this->readbackDeadline_ = now + READ_REPLY_MS;
		this->readbackPhase_ = ReadbackPhase::WAITING;
		this->memoryRead_.message = "Reading in the background; normal polling continues.";
		return;
	}
	if(this->readbackPhase_ != ReadbackPhase::WAITING)
		return;
	if(readback_due(now, this->readbackDeadline_))
	{
		// No address/sequence in the reply: do not assign a late reply to a retried/next word.
		this->finish_memory_readback_(false, "Reply timed out; partial capture retained.");
		return;
	}
	for(unsigned i = 0; i < 256 && this->available(); ++i)
	{
		if(this->readbackFrameLength_ == this->readbackFrame_.size())
		{
			std::move(this->readbackFrame_.begin() + 1, this->readbackFrame_.end(),
			          this->readbackFrame_.begin());
			--this->readbackFrameLength_;
		}
		this->read_byte(&this->readbackFrame_[this->readbackFrameLength_++]);
		auto const &reply = this->readbackFrame_;
		if(this->readbackFrameLength_ != reply.size() ||
		   reply[0] != this->readbackUnit_ ||
		   reply[1] != 5 ||
		   crc16_modbus(reply.data(), reply.size()) != 0)
			continue;
		std::array<uint8_t, 4> const word = { reply[5], reply[4], reply[3], reply[2] };
		size_t const skip = (this->memoryRead_.address + this->memoryRead_.received) & 3;
		size_t const count =
		    std::min(size_t(4) - skip, this->memoryRead_.length - this->memoryRead_.received);
		std::copy_n(word.begin() + skip, count,
		            this->memoryRead_.data.get() + this->memoryRead_.received);
		this->memoryRead_.received += count;
		this->readbackPhase_ = ReadbackPhase::SETTLING;
		this->readbackQuietUntil_ = now + READ_QUIET_MS;
		return;
	}
}
} // namespace esphome::deye_firmware_upgrade
