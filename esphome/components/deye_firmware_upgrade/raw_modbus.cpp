// SPDX-License-Identifier: GPL-3.0-or-later
#include "deye_firmware_upgrade.h"
#include "modbus_console.h"

#include "esphome/components/web_server_base/web_server_base.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace esphome::deye_firmware_upgrade
{
static constexpr size_t RAW_MODBUS_MAX_FRAME = 256;
// Match the readback handoff guard: raw UART reads bypass Modbus's RX timestamps.
static constexpr uint32_t RAW_MODBUS_QUIET_MS = 20;

static bool raw_modbus_due(uint32_t now, uint32_t deadline)
{
	return static_cast<int32_t>(now - deadline) >= 0;
}

static bool raw_modbus_hex(std::string const &text, std::vector<uint8_t> &bytes)
{
	// Bound parsing work as well as the frame. Accept compact hex or whole-byte whitespace.
	if(text.size() > 1024)
		return false;
	int high = -1;
	for(unsigned char c : text)
	{
		if(std::isspace(c))
		{
			if(high >= 0)
				return false;
			continue;
		}
		int digit = c >= '0' && c <= '9'   ? c - '0'
		            : c >= 'a' && c <= 'f' ? c - 'a' + 10
		            : c >= 'A' && c <= 'F' ? c - 'A' + 10
		                                   : -1;
		if(digit < 0)
			return false;
		if(high < 0)
			high = digit;
		else
		{
			if(bytes.size() == RAW_MODBUS_MAX_FRAME)
				return false;
			bytes.push_back(uint8_t(high * 16 + digit));
			high = -1;
		}
	}
	return high < 0;
}

static bool raw_modbus_number(std::string const &text, uint32_t &value)
{
	if(text.empty())
		return false;
	value = 0;
	for(char c : text)
	{
		if(c < '0' || c > '9' || value > (UINT32_MAX - unsigned(c - '0')) / 10)
			return false;
		value = value * 10 + unsigned(c - '0');
	}
	return true;
}

static std::string raw_modbus_format(std::vector<uint8_t> const &bytes)
{
	std::ostringstream text;
	text << std::hex << std::setfill('0');
	for(size_t i = 0; i < bytes.size(); ++i)
		text << (i ? " " : "") << std::setw(2) << unsigned(bytes[i]);
	return text.str();
}

static void raw_modbus_headers(AsyncWebServerResponse *response)
{
	response->addHeader("Cache-Control", "no-store");
	response->addHeader("X-Content-Type-Options", "nosniff");
	response->addHeader(
	    "Content-Security-Policy",
	    "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
	    "connect-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
}

class RawModbusRequestHandler final : public AsyncWebHandler
{
public:
	explicit RawModbusRequestHandler(DeyeFirmwareUpgrade *parent) : parent_(parent)
	{
	}
	bool canHandle(AsyncWebServerRequest *request) const override
	{
		char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
		std::string const url = request->url_to(buffer);
		return (request->method() == HTTP_GET && (url == "/modbus" || url == "/modbus/status")) ||
		       (request->method() == HTTP_POST &&
		        (url == "/modbus/send" || url == "/modbus/cancel"));
	}
	bool isRequestHandlerTrivial() const override
	{
		return false;
	}
	void handleRequest(AsyncWebServerRequest *request) override
	{
		char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
		std::string const url = request->url_to(buffer);
		if(url == "/modbus")
		{
			auto *response =
			    request->beginResponse(200, "text/html; charset=utf-8",
				                       reinterpret_cast<uint8_t const *>(MODBUS_CONSOLE_HTML),
				                       sizeof(MODBUS_CONSOLE_HTML) - 1);
			raw_modbus_headers(response);
			request->send(response);
			return;
		}
		char const *error = nullptr;
		if(request->method() == HTTP_POST)
		{
			uint32_t number;
			if(url == "/modbus/send")
			{
				auto const crc = request->arg("append_crc");
				if(crc != "1" && crc != "0")
					error = "append_crc must be 1 or 0.";
				else if(!raw_modbus_number(request->arg("timeout_ms"), number))
					error = "Use a decimal response timeout in milliseconds.";
				else
					error =
					    this->parent_->request_raw_modbus(request->arg("hex"), crc == "1", number);
			}
			else if(!raw_modbus_number(request->arg("request_id"), number) ||
			        !this->parent_->cancel_raw_modbus(number))
				error = "That request is no longer active.";
		}
		auto const s = this->parent_->raw_modbus_snapshot();
		bool const have_crc = !s.active && s.response.size() >= 4;
		bool const crc_valid = have_crc && crc16_modbus(s.response.data(), s.response.size()) == 0;
		std::ostringstream body;
		// Only fixed messages and formatted bytes enter JSON; no user text is reflected.
		body << std::boolalpha << "{\"request_id\":" << s.request_id << ",\"active\":" << s.active
		     << ",\"sent\":" << s.sent << ",\"timed_out\":" << s.timed_out
		     << ",\"cancelled\":" << s.cancelled << ",\"overflow\":" << s.overflow
		     << ",\"elapsed_ms\":" << s.elapsed_ms << ",\"timeout_ms\":" << s.timeout_ms
		     << ",\"request\":\"" << raw_modbus_format(s.request) << "\",\"response\":\""
		     << raw_modbus_format(s.response)
		     << "\",\"crc_valid\":" << (have_crc ? (crc_valid ? "true" : "false") : "null")
		     << ",\"message\":\"" << s.message << "\",\"error\":\"" << (error ? error : "")
		     << "\"}";
		// Use status codes supported by ESPHome's ESP-IDF web adapter.
		auto *response = request->beginResponse(error ? 409 : 200, "application/json", body.str());
		raw_modbus_headers(response);
		request->send(response);
	}

private:
	DeyeFirmwareUpgrade *parent_;
};

void DeyeFirmwareUpgrade::setup_raw_modbus_()
{
	web_server_base::global_web_server_base->add_handler(new RawModbusRequestHandler(this));
}

char const *DeyeFirmwareUpgrade::request_raw_modbus(std::string const &hex, bool append_crc,
                                                    uint32_t timeout_ms)
{
	std::vector<uint8_t> frame;
	if(!raw_modbus_hex(hex, frame))
		return "Use pairs of hex digits, optionally separated by whitespace; maximum 256 wire "
		       "bytes.";
	if(frame.size() < (append_crc ? 2U : 4U) ||
	   frame.size() > RAW_MODBUS_MAX_FRAME - (append_crc ? 2U : 0U))
		return "Include unit and function; use 2-254 bytes without CRC or 4-256 bytes including "
		       "CRC.";
	if(timeout_ms < 100 || timeout_ms > 10000)
		return "Response timeout must be between 100 and 10000 milliseconds.";
	if(append_crc)
	{
		auto const crc = crc16_modbus(frame.data(), frame.size());
		frame.push_back(uint8_t(crc));
		frame.push_back(uint8_t(crc >> 8));
	}
	LockGuard lock(this->mutex_);
	if(this->raw_modbus_.active ||
	   this->memoryRead_.active ||
	   state_is_active_(this->state_) ||
	   this->upload_owner_ != nullptr)
		return "Another Modbus request, memory capture, upload or upgrade is active.";
	uint32_t const id = this->raw_modbus_.request_id + 1;
	this->raw_modbus_ = RawModbusSnapshot{};
	this->raw_modbus_.request_id = id;
	this->raw_modbus_.request = std::move(frame);
	this->raw_modbus_.timeout_ms = timeout_ms;
	this->raw_modbus_.active = true;
	this->raw_modbus_.message = "Waiting for an idle Modbus slot.";
	this->raw_modbus_started_ = millis();
	this->raw_modbus_deadline_ = this->raw_modbus_started_ + QUIESCE_TIMEOUT_MS;
	this->raw_modbus_cancel_ = false;
	return nullptr;
}

bool DeyeFirmwareUpgrade::cancel_raw_modbus(uint32_t request_id)
{
	LockGuard lock(this->mutex_);
	if(!this->raw_modbus_.active || this->raw_modbus_.request_id != request_id)
		return false;
	this->raw_modbus_cancel_ = true;
	return true;
}

RawModbusSnapshot DeyeFirmwareUpgrade::raw_modbus_snapshot()
{
	LockGuard lock(this->mutex_);
	auto result = this->raw_modbus_;
	if(result.active)
		result.elapsed_ms = millis() - this->raw_modbus_started_;
	return result;
}

// Main loop only, with the admission mutex held. Polling timers keep their cadence.
void DeyeFirmwareUpgrade::finish_raw_modbus_(char const *message)
{
	this->restore_bus_(false);
	this->raw_modbus_.active = false;
	this->raw_modbus_.cancelled = this->raw_modbus_cancel_;
	this->raw_modbus_.elapsed_ms = millis() - this->raw_modbus_started_;
	this->raw_modbus_.message = message;
}

void DeyeFirmwareUpgrade::service_raw_modbus_(uint32_t now)
{
	LockGuard lock(this->mutex_);
	auto &s = this->raw_modbus_;
	if(!s.sent)
	{
		if(this->raw_modbus_cancel_)
		{
			this->finish_raw_modbus_("Cancelled before sending.");
			return;
		}
		if(raw_modbus_due(now, this->raw_modbus_deadline_))
		{
			s.timed_out = true;
			this->finish_raw_modbus_("Timed out waiting for an idle bus; request was not sent.");
			return;
		}
		if(!this->modbus_controller_->is_in_loop_state() ||
		   !this->modbus_->is_in_loop_state() ||
		   this->modbus_controller_->get_command_queue_length() != 0 ||
		   !this->modbus_->tx_buffer_empty() ||
		   this->modbus_->tx_blocked() ||
		   this->available())
			return;
		this->take_bus_();
		this->write_array(s.request.data(), s.request.size());
		s.sent = true;
		s.message = "Request sent; waiting for a reply.";
		// The component's UART is validated as 9600 8N1. Include transmission time,
		// since write_array may return before the last byte has left the UART.
		this->raw_modbus_deadline_ = now + (s.request.size() * 10000 + 9599) / 9600 + s.timeout_ms;
		return;
	}
	if(this->raw_modbus_cancel_)
		s.message = "Cancelling; draining the pending reply.";
	for(size_t i = 0; i < 512 && this->available(); ++i)
	{
		uint8_t byte;
		if(!this->read_byte(&byte))
			break;
		if(s.response.size() < RAW_MODBUS_MAX_FRAME)
			s.response.push_back(byte);
		else
			s.overflow = true;
		this->raw_modbus_quiet_until_ = now + RAW_MODBUS_QUIET_MS;
	}
	if((!s.response.empty() || s.timed_out) &&
	   !this->available() &&
	   raw_modbus_due(now, this->raw_modbus_quiet_until_))
	{
		bool const crc_valid =
		    s.response.size() >= 4 && crc16_modbus(s.response.data(), s.response.size()) == 0;
		this->finish_raw_modbus_(this->raw_modbus_cancel_ ? "Cancelled; captured bytes retained."
		                         : s.overflow
		                             ? "Reply exceeded 256 bytes; truncated capture retained."
								 : s.timed_out ? "Reply timed out; captured bytes retained."
								 : crc_valid   ? "Reply received."
									           : "Reply received with incomplete or invalid CRC.");
		return;
	}
	if(raw_modbus_due(now, this->raw_modbus_deadline_))
	{
		if(!s.timed_out)
		{
			s.timed_out = true;
			this->raw_modbus_quiet_until_ = now + RAW_MODBUS_QUIET_MS;
		}
		else if(raw_modbus_due(now, this->raw_modbus_deadline_ + 1000))
			this->finish_raw_modbus_("UART did not become quiet after the response timeout.");
	}
}
} // namespace esphome::deye_firmware_upgrade
