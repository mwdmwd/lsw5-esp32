// SPDX-License-Identifier: GPL-3.0-or-later
#include "deye_firmware_upgrade.h"

#include "esphome/components/sha256/sha256.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace esphome::deye_firmware_upgrade
{

static char const *const TAG = "deye_firmware_upgrade";
static constexpr char UPGRADE_PATH[] = "/firmware";
static constexpr char UPLOAD_PATH[] = "/firmware/upload";
static constexpr char START_PATH[] = "/firmware/start";
static constexpr char CLEAR_PATH[] = "/firmware/clear";

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
	return static_cast<int32_t>(now - deadline) >= 0;
}

static std::string basename_for_upload(std::string const &filename)
{
	size_t const separator = filename.find_last_of("/\\");
	std::string basename =
	    separator == std::string::npos ? filename : filename.substr(separator + 1);
	if(basename.size() > 128)
		basename.resize(128);
	return basename.empty() ? "firmware.bin" : basename;
}

static std::string html_escape(std::string const &input)
{
	std::string output;
	output.reserve(input.size());
	for(char value : input)
	{
		switch(value)
		{
			case '&':
				output += "&amp;";
				break;
			case '<':
				output += "&lt;";
				break;
			case '>':
				output += "&gt;";
				break;
			case '\"':
				output += "&quot;";
				break;
			case '\'':
				output += "&#39;";
				break;
			default:
				output += value;
				break;
		}
	}
	return output;
}

static std::string request_url(AsyncWebServerRequest *request)
{
	char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
	return std::string(request->url_to(buffer));
}

static uint8_t parse_target(AsyncWebServerRequest *request)
{
	std::string const value = request->arg("target");
	return value == "1" ? 1 : value == "2" ? 2 : 0;
}

static char const *target_name(uint8_t target)
{
	switch(target)
	{
		case 1:
			return "MCU1 (HMI)";
		case 2:
			return "MCU2 (DSP)";
		default:
			return "unknown";
	}
}

class FirmwareUpgradeRequestHandler final : public AsyncWebHandler
{
public:
	explicit FirmwareUpgradeRequestHandler(DeyeFirmwareUpgrade *parent) : parent_(parent)
	{
	}

	bool canHandle(AsyncWebServerRequest *request) const override
	{
		std::string const url = request_url(request);
		if(request->method() == HTTP_GET)
			return url == UPGRADE_PATH;
		if(request->method() == HTTP_POST)
			return url == UPLOAD_PATH || url == START_PATH || url == CLEAR_PATH;
		return false;
	}

	bool isRequestHandlerTrivial() const override
	{
		return false;
	}

	void handleUpload(AsyncWebServerRequest *request, std::string const &filename, size_t index,
	                  uint8_t *data, size_t length, bool final) override
	{
		if(request->method() != HTTP_POST || request_url(request) != UPLOAD_PATH)
			return;

		// ESP-IDF emits a start callback before the first data chunk of each file.
		if(index == 0 && data == nullptr && !final)
		{
			if(!this->parent_->begin_upload(request, filename, parse_target(request),
			                                request->contentLength()))
			{
				// A second file invalidates this request, but another request must
				// neither replace nor release the currently admitted owner's buffer.
				if(this->upload_request_ == request)
					this->upload_ok_ = false;
				return;
			}
			this->upload_request_ = request;
			this->upload_ok_ = true;
			this->upload_complete_ = false;
		}
		if(this->upload_request_ != request || !this->upload_ok_)
			return;
		if(this->upload_complete_)
		{
			this->upload_ok_ = false;
			return;
		}
		if(length != 0)
			this->upload_ok_ = this->parent_->append_upload(request, index, data, length);
		if(final)
			this->upload_complete_ = this->upload_ok_;
		// Validate only when the HTTP request ends, after checking that exactly
		// one complete file was received. A file-final callback is not request-final.
	}

	void handleRequest(AsyncWebServerRequest *request) override
	{
		std::string const url = request_url(request);
		bool accepted = true;

		if(request->method() == HTTP_POST && url == UPLOAD_PATH)
		{
			accepted =
			    this->upload_request_ == request && this->upload_ok_ && this->upload_complete_;
			if(accepted)
				accepted = this->parent_->finish_upload(request);
			else
				this->parent_->abort_upload(request);
			if(this->upload_request_ == request)
			{
				this->upload_request_ = nullptr;
				this->upload_ok_ = false;
				this->upload_complete_ = false;
			}
		}
		else if(request->method() == HTTP_POST && url == START_PATH)
		{
			accepted = this->parent_->request_start(request->arg("sha256"),
			                                        request->arg("confirmation_token"));
		}
		else if(request->method() == HTTP_POST && url == CLEAR_PATH)
		{
			accepted = this->parent_->clear_staged(request->arg("sha256"),
			                                       request->arg("confirmation_token"));
		}

		this->send_page_(request, accepted ? 200 : 409);
	}

protected:
	void send_page_(AsyncWebServerRequest *request, int status_code)
	{
		UpgradeSnapshot const status = this->parent_->snapshot();
		std::string const sha256 = html_escape(status.sha256);
		std::ostringstream page;
		page << R"(<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">)";
		if(status.active)
			page << "<meta http-equiv=refresh content=\"2;url=" << UPGRADE_PATH << "\">";
		page << R"(<title>Inverter firmware upgrade</title>
<style>
body{font:16px system-ui,sans-serif;max-width:52rem;margin:2rem auto;padding:0 1rem;line-height:1.45}
section{border:1px solid #aaa;border-radius:.5rem;padding:1rem;margin:1rem 0}
.danger{border-color:#b00;background:#fee}code{overflow-wrap:anywhere}
button,input{font:inherit;padding:.45rem;margin:.25rem 0}progress{width:100%}
</style></head><body><h1>Inverter firmware upgrade</h1>
<section class=danger>Use firmware for the exact inverter model and processor.
Keep power stable and do not reboot or update the ESP32 during transfer.
A wrong image or interrupted update can leave the inverter unbootable.</section>)";

		page << "<section><h2>Status</h2><p><strong>" << html_escape(status.state_name)
		     << "</strong>: " << html_escape(status.message) << "</p>";
		if(status.total_blocks != 0)
			page << "<progress max=" << status.total_blocks
			     << " value=" << status.acknowledged_blocks << "></progress><p>"
			     << status.acknowledged_blocks << " / " << status.total_blocks << " blocks</p>";
		page << "</section>";

		if(status.has_verified_image)
		{
			page << "<section><h2>Verified image</h2><dl><dt>Target</dt><dd>"
			     << target_name(status.target) << "</dd><dt>File</dt><dd>"
			     << html_escape(status.filename) << "</dd><dt>Image / payload size</dt><dd>"
			     << status.image_size << " / " << status.payload_size
			     << " bytes</dd><dt>Selectors</dt><dd><code>" << std::hex << std::setfill('0');
			for(uint8_t selector : status.selectors)
				page << std::setw(2) << static_cast<unsigned>(selector);
			page << "</code></dd><dt>Payload CRC32</dt><dd><code>" << std::setw(8) << status.crc32
			     << "</code></dd><dt>SHA-256</dt><dd><code>" << sha256
			     << "</code></dd></dl></section>" << std::dec;
		}

		if(!status.active)
		{
			page << "<section><h2>Stage a Deye image</h2><p>Uploading verifies the file in PSRAM. "
			        "It does not start an inverter update. Maximum file size: "
			     << status.max_firmware_size << " bytes.</p>";
			for(uint8_t target : { 1, 2 })
				page << "<form method=post enctype=multipart/form-data action=\"" << UPLOAD_PATH
				     << "?target=" << static_cast<unsigned>(target) << "\"><label>"
				     << target_name(target)
				     << ": <input type=file name=firmware accept=.bin required>"
				        "</label> <button type=submit>Stage image</button></form>";
			page << "</section>";
		}

		// Both forms carry the staging/attempt token, so an old page cannot act on
		// identical bytes staged for another processor or replay a previous start.
		std::string const identity = "<input type=hidden name=sha256 value=\"" +
		                             sha256 +
		                             "\"><input type=hidden name=confirmation_token value=\"" +
		                             html_escape(status.confirmation_token) +
		                             "\">";
		if(status.has_verified_image && !status.active && status.state != UpgradeState::COMPLETE)
		{
			page << "<section class=danger><h2>Confirm transfer</h2>";
			if(status.state == UpgradeState::ERROR)
				page << "<p>The previous transfer failed. Restore the target to a known state "
				        "using its service procedure before retrying.</p>";
			page << "<p>Compare the SHA-256 with your source file, then click Start.</p>"
			     << "<form method=post action=\"" << START_PATH << "\">" << identity
			     << R"(<button type=submit>Start inverter transfer</button></form></section>)";
		}
		if(status.has_verified_image && !status.active)
			page << "<form method=post action=\"" << CLEAR_PATH << "\">" << identity
			     << "<button type=submit>Discard staged image</button></form>";
		page << "<p><a href=/>Return to ESPHome</a></p></body></html>";

		auto *response =
		    request->beginResponse(status_code, "text/html; charset=utf-8", page.str());
		response->addHeader("Cache-Control", "no-store");
		response->addHeader("X-Content-Type-Options", "nosniff");
		response->addHeader("Content-Security-Policy",
		                    "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; "
		                    "base-uri 'none'; frame-ancestors 'none'");
		response->addHeader("Connection", "close");
		request->send(response);
	}

	DeyeFirmwareUpgrade *parent_;
	void const *upload_request_{ nullptr };
	bool upload_ok_{ false };
	bool upload_complete_{ false };
};

DeyeFirmwareUpgrade::~DeyeFirmwareUpgrade()
{
	LockGuard lock(this->mutex_);
	this->free_firmware_locked_();
}

void DeyeFirmwareUpgrade::setup()
{
	auto *base = web_server_base::global_web_server_base;
	base->add_handler(new FirmwareUpgradeRequestHandler(this));
}

void DeyeFirmwareUpgrade::dump_config()
{
	ESP_LOGCONFIG(TAG,
	              "Inverter firmware upgrade:\n"
	              "  Web path: %s\n"
	              "  Staging memory: PSRAM only\n"
	              "  Maximum image size: %zu bytes\n"
	              "  Supported targets: MCU1/HMI, MCU2/DSP",
	              UPGRADE_PATH, this->max_firmware_size_);
}

bool DeyeFirmwareUpgrade::begin_upload(void const *owner, std::string const &filename,
                                       uint8_t target, size_t content_length)
{
	LockGuard lock(this->mutex_);
	if(content_length == 0 || state_is_active_(this->state_) || this->upload_owner_ != nullptr)
	{
		ESP_LOGW(TAG, "Rejecting empty upload or upload while another operation is active");
		return false;
	}
	if(!valid_target_(target))
	{
		this->message_ = "Only target 1 (MCU1/HMI) and target 2 (MCU2/DSP) are supported.";
		this->status_set_warning();
		return false;
	}

	this->free_firmware_locked_();

	this->filename_ = basename_for_upload(filename);
	this->target_ = target;
	// HTTP Content-Length includes the multipart envelope, so it bounds the file.
	// Allocate once; chunks may never grow beyond that bound or the memory limit.
	this->firmware_capacity_ = std::min(this->max_firmware_size_, content_length);
	RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
	this->firmware_ = allocator.allocate(this->firmware_capacity_);
	if(this->firmware_ == nullptr)
	{
		ESP_LOGE(TAG, "Could not allocate %zu bytes in PSRAM", this->firmware_capacity_);
		return this->reject_image_locked_(
		    "Could not allocate the firmware staging buffer in PSRAM.");
	}

	this->firmware_size_ = 0;
	this->upload_owner_ = owner;
	this->upload_last_activity_ = millis();
	this->state_ = UpgradeState::UPLOADING;
	this->message_ = "Receiving the complete image into PSRAM; the inverter UART is untouched.";
	this->status_clear_warning();
	ESP_LOGI(TAG, "Staging %s for %s in PSRAM", this->filename_.c_str(),
	         target_name(this->target_));
	return true;
}

bool DeyeFirmwareUpgrade::append_upload(void const *owner, size_t index, uint8_t const *data,
                                        size_t length)
{
	LockGuard lock(this->mutex_);
	if(this->state_ != UpgradeState::UPLOADING || this->upload_owner_ != owner)
		return false;
	if(index != this->firmware_size_)
		return this->reject_image_locked_(
		    "Upload chunks arrived out of order; the image was discarded.");
	if(length > this->firmware_capacity_ - this->firmware_size_)
		return this->reject_image_locked_(
		    "Upload exceeds Content-Length or max_firmware_size; discarded.");
	if(length != 0)
		std::memcpy(this->firmware_ + this->firmware_size_, data, length);
	this->firmware_size_ += length;
	this->upload_last_activity_ = millis();
	return true;
}

bool DeyeFirmwareUpgrade::finish_upload(void const *owner)
{
	LockGuard lock(this->mutex_);
	if(this->upload_owner_ != owner)
		return false;
	this->upload_owner_ = nullptr;
	if(this->state_ != UpgradeState::UPLOADING)
		return false;
	if(!this->validate_uploaded_image_locked_())
		return this->reject_image_locked_(this->message_);

	this->renew_confirmation_token_locked_();
	this->state_ = UpgradeState::STAGED;
	this->message_ = "Image checked in PSRAM. Compare SHA-256, then click Start inverter transfer.";
	this->status_clear_warning();
	ESP_LOGI(TAG,
	         "Verified staged image: target=%s file=%s image=%zu payload=%" PRIu32
	         " selectors=%02x%02x%02x crc32=%08" PRIx32 " sha256=%s",
	         target_name(this->target_), this->filename_.c_str(), this->firmware_size_,
	         this->image_info_.payload_size, this->image_info_.selectors[0],
	         this->image_info_.selectors[1], this->image_info_.selectors[2],
	         this->image_info_.crc32, this->sha256_.data());
	return true;
}

void DeyeFirmwareUpgrade::abort_upload(void const *owner)
{
	LockGuard lock(this->mutex_);
	if(this->upload_owner_ != owner)
		return;
	this->upload_owner_ = nullptr;
	this->reject_image_locked_(
	    this->state_ == UpgradeState::UPLOADING
	        ? "Upload did not contain exactly one complete firmware file; discarded."
			: this->message_);
}

void DeyeFirmwareUpgrade::renew_confirmation_token_locked_()
{
	// Bind the form to this staging/attempt, not just bytes shared by both targets.
	// Rotate on staging and acceptance; a failed attempt needs a fresh status page.
	for(size_t index = 0; index < 4; index++)
		snprintf(this->confirmation_token_.data() + index * 8, 9, "%08" PRIx32, random_uint32());
}

bool DeyeFirmwareUpgrade::validate_uploaded_image_locked_()
{
	if(this->firmware_size_ % XMODEM_BLOCK_SIZE != 0)
	{
		this->message_ = "The firmware file size must be a multiple of 1024 bytes.";
		ESP_LOGE(TAG, "%s", this->message_.c_str());
		return false;
	}

	DeyeImageInfo info{};
	if(!inspect_deye_image(this->firmware_, this->firmware_size_, info))
	{
		this->message_ =
		    "No valid Deye length/CRC32 trailer was found at the exact end of the aligned file.";
		ESP_LOGE(TAG, "%s", this->message_.c_str());
		return false;
	}

	// Include the selectors and trailer in the digest shown for confirmation.
	esphome::sha256::SHA256 hasher;
	hasher.init();
	hasher.add(this->firmware_, this->firmware_size_);
	hasher.calculate();
	hasher.get_hex(this->sha256_.data());

	this->image_info_ = info;
	this->total_blocks_ = this->firmware_size_ / XMODEM_BLOCK_SIZE;
	return true;
}

bool DeyeFirmwareUpgrade::request_start(std::string const &sha256,
                                        std::string const &confirmation_token)
{
	LockGuard lock(this->mutex_);
	if((this->state_ != UpgradeState::STAGED && this->state_ != UpgradeState::ERROR) ||
	   this->sha256_[0] == '\0')
	{
		// A replay must not restart a completed transfer or overwrite its status.
		ESP_LOGW(TAG, "No verified, startable image is available");
		return false;
	}
	if(sha256 != this->sha256_.data() ||
	   confirmation_token.empty() ||
	   confirmation_token != this->confirmation_token_.data())
	{
		this->message_ = "The confirmation refers to a stale or different staged image.";
		return false;
	}

	this->acknowledged_blocks_ = 0;
	this->renew_confirmation_token_locked_();
	this->state_ = UpgradeState::QUEUED;
	this->message_ = "Confirmation accepted; waiting for normal Modbus traffic to become idle.";
	this->status_clear_warning();
	ESP_LOGW(TAG, "Firmware transfer confirmed: target=%s sha256=%s", target_name(this->target_),
	         this->sha256_.data());
	return true;
}

bool DeyeFirmwareUpgrade::clear_staged(std::string const &sha256,
                                       std::string const &confirmation_token)
{
	LockGuard lock(this->mutex_);
	if(state_is_active_(this->state_) || this->upload_owner_ != nullptr)
		return false;
	if(this->firmware_ == nullptr ||
	   sha256 != this->sha256_.data() ||
	   confirmation_token.empty() ||
	   confirmation_token != this->confirmation_token_.data())
	{
		this->message_ = "The discard request refers to a stale or missing image.";
		return false;
	}
	this->free_firmware_locked_();
	this->state_ = UpgradeState::EMPTY;
	this->message_ = "The staged image was discarded from PSRAM.";
	this->status_clear_warning();
	return true;
}

UpgradeSnapshot DeyeFirmwareUpgrade::snapshot()
{
	LockGuard lock(this->mutex_);
	UpgradeSnapshot result;
	result.state = this->state_;
	result.state_name = state_name_(this->state_);
	result.message = this->message_;
	result.filename = this->filename_;
	result.sha256 = this->sha256_.data();
	result.confirmation_token = this->confirmation_token_.data();
	result.target = this->target_;
	result.image_size = this->firmware_size_;
	result.payload_size = this->image_info_.payload_size;
	result.selectors = this->image_info_.selectors;
	result.crc32 = this->image_info_.crc32;
	result.acknowledged_blocks = this->acknowledged_blocks_;
	result.total_blocks = this->total_blocks_;
	result.max_firmware_size = this->max_firmware_size_;
	result.has_verified_image = this->sha256_[0] != '\0';
	result.active = state_is_active_(this->state_) || this->upload_owner_ != nullptr;
	return result;
}

bool DeyeFirmwareUpgrade::reject_image_locked_(std::string const &message)
{
	this->message_ = message;
	this->free_firmware_locked_();
	this->state_ = UpgradeState::ERROR;
	this->status_set_warning();
	return false;
}

void DeyeFirmwareUpgrade::free_firmware_locked_()
{
	if(this->firmware_ != nullptr)
	{
		RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
		allocator.deallocate(this->firmware_, this->firmware_capacity_);
	}
	this->firmware_ = nullptr;
	this->confirmation_token_[0] = '\0';
	this->firmware_capacity_ = 0;
	this->firmware_size_ = 0;
	this->filename_.clear();
	this->sha256_[0] = '\0';
	this->image_info_ = {};
	this->target_ = 0;
	this->acknowledged_blocks_ = 0;
	this->total_blocks_ = 0;
	// Keep the upload owner until HTTP completion/timeout, even after a bad chunk.
}

void DeyeFirmwareUpgrade::expire_interrupted_upload_(uint32_t now)
{
	LockGuard lock(this->mutex_);
	if(this->upload_owner_ == nullptr || now - this->upload_last_activity_ <= UPLOAD_TIMEOUT_MS)
		return;
	this->upload_owner_ = nullptr;
	this->reject_image_locked_(
	    "The firmware upload timed out; its incomplete image was discarded.");
	ESP_LOGW(TAG, "Discarded interrupted firmware upload");
}

void DeyeFirmwareUpgrade::loop()
{
	uint32_t const now = millis();
	this->expire_interrupted_upload_(now);
	UpgradeState state;
	{
		LockGuard lock(this->mutex_);
		state = this->state_;
	}
	if(state == UpgradeState::QUEUED)
		this->begin_transport_(now);

	switch(this->transport_phase_)
	{
		case TransportPhase::IDLE:
			break;
		case TransportPhase::QUIESCING:
			this->service_quiescing_(now);
			break;
		case TransportPhase::WAIT_ACTIVATION_ACK:
			this->service_activation_ack_(now);
			break;
		case TransportPhase::WAIT_RECEIVER:
			this->service_receiver_wait_(now);
			break;
		case TransportPhase::SEND_BLOCK:
			this->service_block_send_(now);
			break;
		case TransportPhase::WAIT_BLOCK_REPLY:
			this->service_block_reply_(now);
			break;
		case TransportPhase::RETRY_DELAY:
			if(deadline_reached(now, this->phase_deadline_))
			{
				this->packet_offset_ = 0;
				this->transport_phase_ = TransportPhase::SEND_BLOCK;
			}
			break;
		case TransportPhase::EOT_DELAY:
			if(deadline_reached(now, this->phase_deadline_))
				this->send_eot_(now);
			break;
		case TransportPhase::POST_TRANSFER:
			this->service_post_transfer_(now);
			break;
	}
}

void DeyeFirmwareUpgrade::begin_transport_(uint32_t now)
{
	if(this->event_monitor_ != nullptr)
		this->event_monitor_->stop_poller();
	this->modbus_controller_->stop_poller();
	this->transport_phase_ = TransportPhase::QUIESCING;
	this->phase_deadline_ = now + QUIESCE_TIMEOUT_MS;
	this->set_state_(
	    UpgradeState::QUIESCING,
	    "Periodic polling is paused; draining already queued Modbus commands before UART handoff.");
}

void DeyeFirmwareUpgrade::service_quiescing_(uint32_t now)
{
	if(this->modbus_controller_->get_command_queue_length() == 0 &&
	   this->modbus_->tx_buffer_empty() &&
	   !this->modbus_->tx_blocked())
	{
		this->take_bus_();
		this->activation_attempt_ = 0;
		this->send_activation_(now);
		return;
	}
	if(deadline_reached(now, this->phase_deadline_))
		this->fail_transport_(
		    "Timed out while waiting for queued Modbus traffic to drain; activation was not sent.");
}

void DeyeFirmwareUpgrade::take_bus_()
{
	this->controller_loop_was_enabled_ = this->modbus_controller_->is_in_loop_state();
	this->modbus_loop_was_enabled_ = this->modbus_->is_in_loop_state();
	this->modbus_controller_->disable_loop();
	this->modbus_->disable_loop();
	this->bus_owned_ = true;
	this->drain_uart_();
}

void DeyeFirmwareUpgrade::restore_bus_()
{
	if(this->bus_owned_)
	{
		this->drain_uart_();
		if(this->modbus_loop_was_enabled_)
			this->modbus_->enable_loop();
		if(this->controller_loop_was_enabled_)
			this->modbus_controller_->enable_loop();
		this->bus_owned_ = false;
	}
	this->modbus_controller_->start_poller();
	if(this->event_monitor_ != nullptr)
		this->event_monitor_->start_poller();
}

void DeyeFirmwareUpgrade::drain_uart_()
{
	uint8_t value;
	while(this->available() != 0)
		this->read_byte(&value);
}

void DeyeFirmwareUpgrade::send_activation_(uint32_t now)
{
	std::array<uint8_t, 13> request = { 0x01, 0x10,          0x04, 0x38, 0x00, 0x02, 0x04,
	                                    0x00, this->target_, 0x00, 0x60, 0x00, 0x00 };
	uint16_t const crc = crc16_modbus(request.data(), request.size() - 2);
	request[11] = static_cast<uint8_t>(crc);
	request[12] = static_cast<uint8_t>(crc >> 8U);

	this->activation_attempt_++;
	this->activation_ack_match_ = 0;
	this->activation_ack_received_ = false;
	this->early_crc_request_ = false;
	this->drain_uart_();
	this->write_array(request);
	this->transport_phase_ = TransportPhase::WAIT_ACTIVATION_ACK;
	this->phase_deadline_ = now + ACTIVATION_ACK_TIMEOUT_MS;

	char message[128];
	snprintf(message, sizeof(message), "Sent FC16 update activation for %s (attempt %u/%u).",
	         target_name(this->target_), this->activation_attempt_, MAX_ACTIVATION_ATTEMPTS);
	this->set_state_(UpgradeState::ACTIVATING, message);
}

void DeyeFirmwareUpgrade::service_activation_ack_(uint32_t now)
{
	static constexpr std::array<uint8_t, 8> EXPECTED_ACK = { 0x01, 0x10, 0x04, 0x38,
	                                                         0x00, 0x02, 0xC1, 0x35 };
	uint8_t value;
	while(this->available() != 0 && this->read_byte(&value))
	{
		if(value == 0x43)
			this->early_crc_request_ = true;
		if(value == EXPECTED_ACK[this->activation_ack_match_])
		{
			this->activation_ack_match_++;
			if(this->activation_ack_match_ == EXPECTED_ACK.size())
			{
				this->activation_ack_received_ = true;
				this->activation_ack_match_ = 0;
			}
		}
		else
		{
			this->activation_ack_match_ = value == EXPECTED_ACK[0] ? 1 : 0;
		}
	}

	if(this->activation_ack_received_)
	{
		if(this->early_crc_request_)
		{
			this->begin_block_(0);
		}
		else
		{
			this->transport_phase_ = TransportPhase::WAIT_RECEIVER;
			this->phase_deadline_ = now + RECEIVER_READY_TIMEOUT_MS;
			this->set_state_(UpgradeState::WAITING_FOR_RECEIVER,
			                 "Activation acknowledged; waiting for the HMI receiver's XMODEM CRC "
			                 "request ('C').");
		}
		return;
	}

	if(!deadline_reached(now, this->phase_deadline_))
		return;
	if(this->early_crc_request_)
	{
		ESP_LOGW(TAG, "Receiver sent 'C' before a complete Modbus acknowledgement; continuing "
		              "because activation succeeded");
		this->begin_block_(0);
	}
	else if(this->activation_attempt_ < MAX_ACTIVATION_ATTEMPTS)
	{
		this->send_activation_(now);
	}
	else
	{
		this->fail_transport_("The inverter did not acknowledge the firmware activation request.");
	}
}

void DeyeFirmwareUpgrade::service_receiver_wait_(uint32_t now)
{
	uint8_t value;
	while(this->available() != 0 && this->read_byte(&value))
	{
		if(value == 0x43)
		{
			this->begin_block_(0);
			return;
		}
		if(value == 0x18)
		{
			this->fail_transport_(
			    "The inverter update receiver cancelled the transfer before the first block.");
			return;
		}
	}
	if(deadline_reached(now, this->phase_deadline_))
		this->fail_transport_("Timed out waiting for the HMI receiver's XMODEM CRC request ('C').");
}

void DeyeFirmwareUpgrade::begin_block_(size_t block_index)
{
	// Nothing sent by the receiver before this block can acknowledge it. In
	// particular, discard a duplicated activation ACK or CRC request so it
	// cannot be mistaken for the reply to the new packet.
	this->drain_uart_();
	build_xmodem_packet(this->firmware_ + block_index * XMODEM_BLOCK_SIZE, block_index,
	                    this->packet_);
	this->block_index_ = block_index;
	this->packet_offset_ = 0;
	this->block_retries_ = 0;
	this->transport_phase_ = TransportPhase::SEND_BLOCK;

	char message[112];
	snprintf(message, sizeof(message), "Sending XMODEM-1K block %zu of %zu.", block_index + 1,
	         this->total_blocks_);
	this->set_state_(UpgradeState::SENDING, message);
}

void DeyeFirmwareUpgrade::service_block_send_(uint32_t now)
{
	// A delayed reply to the previous attempt may have arrived during the retry
	// delay. Do not let it acknowledge a packet which has not been resent yet.
	if(this->packet_offset_ == 0 && this->block_retries_ != 0)
		this->drain_uart_();

	size_t const bytes =
	    std::min(UART_WRITE_CHUNK_SIZE, this->packet_.size() - this->packet_offset_);
	this->write_array(this->packet_.data() + this->packet_offset_, bytes);
	this->packet_offset_ += bytes;
	if(this->packet_offset_ == this->packet_.size())
	{
		this->transport_phase_ = TransportPhase::WAIT_BLOCK_REPLY;
		this->phase_deadline_ =
		    now + (this->block_index_ == 0 ? FIRST_BLOCK_ACK_TIMEOUT_MS : BLOCK_ACK_TIMEOUT_MS);
	}
}

void DeyeFirmwareUpgrade::service_block_reply_(uint32_t now)
{
	uint8_t value;
	while(this->available() != 0 && this->read_byte(&value))
	{
		switch(value)
		{
			case 0x06: // ACK
				this->accept_block_(now);
				return;
			case 0x15: // NAK
				this->retry_block_(now, "NAK");
				return;
			case 0x18: // CAN
				this->fail_transport_(
				    "The inverter update receiver cancelled the XMODEM transfer.");
				return;
			case 0x43: // A repeated CRC request means the receiver still wants this block.
				this->retry_block_(now, "repeated CRC request");
				return;
			default:
				ESP_LOGW(TAG, "Ignoring unexpected receiver reply byte 0x%02x", value);
				break;
		}
	}
	if(deadline_reached(now, this->phase_deadline_))
		this->retry_block_(now, "ACK timeout");
}

void DeyeFirmwareUpgrade::retry_block_(uint32_t now, char const *reason)
{
	this->block_retries_++;
	if(this->block_retries_ > MAX_BLOCK_RETRIES)
	{
		char message[144];
		snprintf(message, sizeof(message), "XMODEM block %zu failed after %u retries (%s).",
		         this->block_index_ + 1, MAX_BLOCK_RETRIES, reason);
		this->fail_transport_(message);
		return;
	}
	ESP_LOGW(TAG, "Retrying XMODEM block %zu/%zu after %s (%u/%u)", this->block_index_ + 1,
	         this->total_blocks_, reason, this->block_retries_, MAX_BLOCK_RETRIES);
	this->transport_phase_ = TransportPhase::RETRY_DELAY;
	this->phase_deadline_ = now + BLOCK_RETRY_DELAY_MS;
}

void DeyeFirmwareUpgrade::accept_block_(uint32_t now)
{
	{
		LockGuard lock(this->mutex_);
		this->acknowledged_blocks_ = this->block_index_ + 1;
	}
	if(this->block_index_ == 0 ||
	   (this->block_index_ + 1) % 16 == 0 ||
	   this->block_index_ + 1 == this->total_blocks_)
	{
		ESP_LOGI(TAG, "Firmware transfer: %zu/%zu blocks acknowledged", this->block_index_ + 1,
		         this->total_blocks_);
	}

	if(this->block_index_ + 1 == this->total_blocks_)
	{
		this->transport_phase_ = TransportPhase::EOT_DELAY;
		this->phase_deadline_ = now + EOT_DELAY_MS;
		this->set_state_(
		    UpgradeState::FINISHING,
		    "All data blocks were acknowledged; waiting one second before XMODEM EOT.");
	}
	else
	{
		this->begin_block_(this->block_index_ + 1);
	}
}

void DeyeFirmwareUpgrade::send_eot_(uint32_t now)
{
	// A reply to the last data block cannot be the result of EOT.
	this->drain_uart_();
	this->eot_ack_received_ = false;
	this->write_byte(0x04);
	this->transport_phase_ = TransportPhase::POST_TRANSFER;
	this->phase_deadline_ = now + POST_TRANSFER_DELAY_MS;
	this->set_state_(
	    UpgradeState::FINISHING,
	    "XMODEM EOT sent; checking for an optional staging result before resuming Modbus.");
}

void DeyeFirmwareUpgrade::service_post_transfer_(uint32_t now)
{
	uint8_t value;
	while(this->available() != 0 && this->read_byte(&value))
	{
		if(value == 0x15) // NAK: the HMI rejected the staged image.
		{
			this->fail_transport_("The HMI rejected the staged image after XMODEM EOT.");
			return;
		}
		if(value == 0x18) // CAN
		{
			this->fail_transport_("The inverter update receiver cancelled after XMODEM EOT.");
			return;
		}
		if(value == 0x06) // ACK: staging/handoff only, not target boot success.
			this->eot_ack_received_ = true;
		else
			ESP_LOGW(TAG, "Ignoring unexpected post-EOT byte 0x%02x", value);
	}
	if(deadline_reached(now, this->phase_deadline_))
		this->complete_transport_();
}

void DeyeFirmwareUpgrade::complete_transport_()
{
	this->restore_bus_();
	this->transport_phase_ = TransportPhase::IDLE;
	char const *message;
	if(this->target_ == 2 && this->eot_ack_received_)
		message = "External transfer complete: HMI acknowledged staging and DSP handoff. "
		          "DSP programming and boot are not verified.";
	else if(this->eot_ack_received_)
		message = "External transfer complete: final receiver ACK received. "
		          "Target programming and boot are not verified.";
	else
		message = "External transfer complete: all data blocks were acknowledged and EOT was "
		          "sent, with no final reply. Target programming and boot are not verified.";

	// Publish COMPLETE last: it allows HTTP requests to replace the image,
	// so finish reading its metadata and updating the component status first.
	this->status_clear_warning();
	ESP_LOGI(TAG, "External firmware transfer complete: target=%s blocks=%zu eot_ack=%s sha256=%s",
	         target_name(this->target_), this->total_blocks_,
	         this->eot_ack_received_ ? "yes" : "no", this->sha256_.data());
	this->set_state_(UpgradeState::COMPLETE, message);
}

void DeyeFirmwareUpgrade::fail_transport_(std::string const &message)
{
	this->restore_bus_();
	this->transport_phase_ = TransportPhase::IDLE;
	this->status_set_warning();
	ESP_LOGE(TAG, "%s", message.c_str());
	// Do not let a new upload race this attempt's final warning update.
	this->set_state_(UpgradeState::ERROR, message);
}

void DeyeFirmwareUpgrade::set_state_(UpgradeState state, std::string const &message)
{
	LockGuard lock(this->mutex_);
	this->state_ = state;
	this->message_ = message;
}

char const *DeyeFirmwareUpgrade::state_name_(UpgradeState state)
{
	switch(state)
	{
		case UpgradeState::EMPTY:
			return "Empty";
		case UpgradeState::UPLOADING:
			return "Uploading to PSRAM";
		case UpgradeState::STAGED:
			return "Verified and staged";
		case UpgradeState::QUEUED:
			return "Confirmed and queued";
		case UpgradeState::QUIESCING:
			return "Pausing Modbus";
		case UpgradeState::ACTIVATING:
			return "Activating update receiver";
		case UpgradeState::WAITING_FOR_RECEIVER:
			return "Waiting for HMI receiver";
		case UpgradeState::SENDING:
			return "Transferring XMODEM-1K";
		case UpgradeState::FINISHING:
			return "Finishing transfer";
		case UpgradeState::COMPLETE:
			return "External transfer complete";
		case UpgradeState::ERROR:
			return "Error";
	}
	return "Unknown";
}

bool DeyeFirmwareUpgrade::state_is_active_(UpgradeState state)
{
	return state == UpgradeState::QUEUED ||
	       state == UpgradeState::QUIESCING ||
	       state == UpgradeState::ACTIVATING ||
	       state == UpgradeState::WAITING_FOR_RECEIVER ||
	       state == UpgradeState::SENDING ||
	       state == UpgradeState::FINISHING;
}

} // namespace esphome::deye_firmware_upgrade
