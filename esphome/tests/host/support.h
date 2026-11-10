// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Small deterministic test doubles, NOT an ESPHome ABI/compile compatibility test.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace esphome
{
inline uint32_t test_now = 0;
inline uint32_t millis()
{
	return test_now;
}
inline uint32_t random_uint32()
{
	static uint32_t n = 0;
	return ++n;
}
using std::make_unique;
using Mutex = std::mutex;
using LockGuard = std::lock_guard<Mutex>;
namespace setup_priority
{
constexpr float AFTER_WIFI = 100;
}
class Component
{
public:
	virtual ~Component() = default;
	virtual void setup()
	{
	}
	virtual void loop()
	{
	}
	virtual void dump_config()
	{
	}
	virtual float get_setup_priority() const
	{
		return 0;
	}
	void disable_loop()
	{
		enabled = false;
	}
	void enable_loop()
	{
		enabled = true;
	}
	bool is_in_loop_state() const
	{
		return enabled;
	}
	void mark_failed()
	{
		failed = true;
	}
	void status_set_warning()
	{
		warning = true;
	}
	void status_clear_warning()
	{
		warning = false;
	}
	bool enabled = true, failed = false, warning = false;
};
class PollingComponent : public Component
{
public:
	virtual void update()
	{
	}
	void stop_poller()
	{
		polling = false;
	}
	void start_poller()
	{
		polling = true;
	}
	uint32_t get_update_interval() const
	{
		return 1000;
	}
	bool polling = true;
};
template<class T>
class RAMAllocator
{
public:
	enum
	{
		ALLOC_EXTERNAL = 1
	};
	explicit RAMAllocator(int)
	{
	}
	T *allocate(size_t n)
	{
		return fail ? nullptr : static_cast<T *>(std::malloc(n * sizeof(T)));
	}
	void deallocate(T *p, size_t)
	{
		std::free(p);
	}
	inline static bool fail = false;
};
template<class T>
class StateSensor
{
public:
	bool has_state() const
	{
		return !history.empty();
	}
	void publish_state(T const &v)
	{
		state = v;
		history.push_back(v);
	}
	T state{};
	std::vector<T> history;
};
namespace sensor
{
using Sensor = StateSensor<float>;
}
namespace text_sensor
{
using TextSensor = StateSensor<std::string>;
}
namespace binary_sensor
{
using BinarySensor = StateSensor<bool>;
}
#define SUB_SENSOR(name) \
 protected: sensor::Sensor *name##_sensor_{nullptr}; \
 public: void set_##name##_sensor(sensor::Sensor *s) { name##_sensor_ = s; }
#define SUB_TEXT_SENSOR(name) \
 protected: text_sensor::TextSensor *name##_text_sensor_{nullptr}; \
 public: void set_##name##_text_sensor(text_sensor::TextSensor *s) { name##_text_sensor_ = s; }
#define SUB_BINARY_SENSOR(name) \
 protected: binary_sensor::BinarySensor *name##_binary_sensor_{nullptr}; \
 public: void set_##name##_binary_sensor(binary_sensor::BinarySensor *s) { name##_binary_sensor_ = s; }
namespace uart
{
class UARTDevice
{
public:
	size_t available() const
	{
		return rx.size();
	}
	bool read_byte(uint8_t *b)
	{
		if(rx.empty())
			return false;
		*b = rx.front();
		rx.pop_front();
		return true;
	}
	void write_array(uint8_t const *b, size_t n)
	{
		tx.insert(tx.end(), b, b + n);
	}
	template<size_t N>
	void write_array(std::array<uint8_t, N> const &b)
	{
		write_array(b.data(), N);
	}
	void write_byte(uint8_t b)
	{
		tx.push_back(b);
	}
	void check_uart_settings(unsigned)
	{
	}
	std::deque<uint8_t> rx;
	std::vector<uint8_t> tx;
};
}
namespace modbus
{
enum class ModbusRegisterType
{
	HOLDING
};
class Modbus : public Component
{
public:
	bool tx_buffer_empty() const
	{
		return empty;
	}
	bool tx_blocked() const
	{
		return blocked;
	}
	bool empty = true, blocked = false;
};
}
namespace modbus_controller
{
class ModbusController;
struct ModbusCommandItem
{
	using Callback =
	    std::function<void(modbus::ModbusRegisterType, uint16_t, std::vector<uint8_t> const &)>;
	uint16_t register_address = 0, register_count = 0;
	std::vector<uint8_t> payload;
	Callback on_data_func;
	bool send()
	{
		++send_count_;
		return true;
	}
	bool should_retry(uint8_t max_retries)
	{
		return send_count_ <= max_retries;
	}
	bool is_equal(ModbusCommandItem const &c) const
	{
		return register_address == c.register_address && register_count == c.register_count;
	}
	static ModbusCommandItem create_read_command(ModbusController *, modbus::ModbusRegisterType,
	                                             uint16_t address, uint16_t count, Callback cb)
	{
		ModbusCommandItem c;
		c.register_address = address;
		c.register_count = count;
		c.on_data_func = std::move(cb);
		return c;
	}

private:
	uint8_t send_count_ = 0;
};
class ModbusController : public PollingComponent
{
public:
	bool ready_for_immediate_send() const
	{
		return ready;
	}
	size_t get_command_queue_length() const
	{
		return command_queue_.size();
	}
	void queue_command(ModbusCommandItem const &c)
	{
		command_queue_.push_back(make_unique<ModbusCommandItem>(c));
	}
	bool ready = true;
	std::list<std::unique_ptr<ModbusCommandItem>> command_queue_;
};
}
namespace sha256
{
class SHA256
{
public:
	void init()
	{
		bytes.clear();
	}
	void add(uint8_t const *p, size_t n)
	{
		bytes.insert(bytes.end(), p, p + n);
	}
	void calculate()
	{
		unsigned n = 0;
		if(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &n, EVP_sha256(), nullptr) != 1 ||
		   n != 32)
			throw std::runtime_error("OpenSSL SHA-256 failed");
	}
	void get_hex(char *out) const
	{
		for(size_t i = 0; i < digest.size(); ++i)
			std::snprintf(out + i * 2, 3, "%02x", digest[i]);
	}
	std::vector<uint8_t> bytes;
	std::array<uint8_t, 32> digest{};
};
}
template<class... Args>
void test_log(Args &&...)
{
}
}
#define ESP_LOGCONFIG(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGE(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGW(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGI(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGD(...) ::esphome::test_log(__VA_ARGS__)
#define ESP_LOGV(...) ::esphome::test_log(__VA_ARGS__)

constexpr int HTTP_GET = 1, HTTP_POST = 2;
struct AsyncWebServerResponse
{
	int status;
	std::string body;
	std::map<std::string, std::string> headers;
	void addHeader(std::string const &k, std::string const &v)
	{
		headers[k] = v;
	}
};
class AsyncWebServerRequest
{
public:
	static constexpr size_t URL_BUF_SIZE = 256;
	int method() const
	{
		return verb;
	}
	char const *url_to(char *p) const
	{
		std::snprintf(p, URL_BUF_SIZE, "%s", url.c_str());
		return p;
	}
	std::string arg(std::string const &k) const
	{
		auto i = args.find(k);
		return i == args.end() ? "" : i->second;
	}
	size_t contentLength() const
	{
		return content_length;
	}
	AsyncWebServerResponse *beginResponse(int status, char const *, std::string const &body)
	{
		return new AsyncWebServerResponse{ status, body, {} };
	}
	AsyncWebServerResponse *beginResponse(int status, char const *type, uint8_t const *data,
	                                      size_t length)
	{
		return beginResponse(status, type,
		                     std::string(reinterpret_cast<char const *>(data), length));
	}
	void send(AsyncWebServerResponse *r)
	{
		response.reset(r);
	}
	int verb = HTTP_POST;
	std::string url = "/firmware/upload";
	size_t content_length = 4096;
	std::map<std::string, std::string> args{ { "target", "1" } };
	std::unique_ptr<AsyncWebServerResponse> response;
};
class AsyncWebHandler
{
public:
	virtual ~AsyncWebHandler() = default;
	virtual bool canHandle(AsyncWebServerRequest *) const
	{
		return false;
	}
	virtual bool isRequestHandlerTrivial() const
	{
		return false;
	}
	virtual void handleUpload(AsyncWebServerRequest *, std::string const &, size_t, uint8_t *,
	                          size_t, bool)
	{
	}
	virtual void handleRequest(AsyncWebServerRequest *)
	{
	}
};
namespace esphome::web_server_base
{
class WebServerBase
{
public:
	void add_handler(AsyncWebHandler *h)
	{
		handlers.emplace_back(h);
	}
	std::vector<std::unique_ptr<AsyncWebHandler>> handlers;
};
inline WebServerBase test_base;
inline WebServerBase *global_web_server_base = &test_base;
}
