#include "dolphin_ble.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"
#include "esp_bt_main.h"

namespace esphome {
namespace dolphin_ble {

namespace {
constexpr const char *TAG = "dolphin_ble";
constexpr uint16_t GATTS_APP_ID = 0x44;
constexpr uint16_t GATTC_APP_ID = 0x45;
constexpr const char *IOT_SERVICE_UUID = "fd5abba0-3935-11e5-85a6-0002a5d5c51b";
constexpr const char *IOT_CHAR_UUID = "fd5abba1-3935-11e5-85a6-0002a5d5c51b";
constexpr const char *CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb";
constexpr uint16_t INVALID_HANDLE = 0;
}  // namespace

DolphinBle *DolphinBle::instance_ = nullptr;

void DolphinBle::setup() {
  instance_ = this;
  ESP_LOGI(TAG, "Setting up Dolphin BLE bridge for %s (%s)", this->mac_address_.c_str(),
           this->name_filter_.empty() ? "no name filter" : this->name_filter_.c_str());
  ESP_LOGI(TAG, "Local protocol profile: MU leds.start=%u, temperature=%s",
           this->mu_led_data_offset_, this->temperature_supported_ ? "enabled" : "disabled");

  this->parsed_mac_ = this->parse_mac_();
  if (!this->parsed_mac_) {
    ESP_LOGE(TAG, "Invalid MAC address: %s", this->mac_address_.c_str());
    this->mark_failed();
    return;
  }

  if (!uuid_from_string_(IOT_SERVICE_UUID, &this->service_uuid_) ||
      !uuid_from_string_(IOT_CHAR_UUID, &this->char_uuid_) ||
      !uuid_from_string_(CCCD_UUID, &this->cccd_uuid_)) {
    ESP_LOGE(TAG, "Failed to parse static UUIDs");
    this->mark_failed();
    return;
  }
}

void DolphinBle::set_numeric_sensor(uint8_t kind, sensor::Sensor *sensor) {
  if (kind < this->numeric_sensors_.size())
    this->numeric_sensors_[kind] = sensor;
}

void DolphinBle::set_text_sensor(uint8_t kind, text_sensor::TextSensor *sensor) {
  if (kind < this->text_sensors_.size())
    this->text_sensors_[kind] = sensor;
}

void DolphinBle::set_cleaning_mode_select(select::Select *select) {
  this->cleaning_mode_select_ = select;
}

void DolphinBle::set_manual_drive_direction_select(select::Select *select) {
  this->manual_drive_direction_select_ = select;
}

void DolphinBle::loop() {
  this->maybe_start_gatt_();
  this->maybe_connect_();

  std::vector<std::vector<uint8_t>> local_queue;
  {
    std::lock_guard<std::mutex> lock(this->rx_mutex_);
    if (!this->rx_queue_.empty()) {
      local_queue = std::move(this->rx_queue_);
      this->rx_queue_.clear();
    }
  }
  for (const auto &chunk : local_queue) {
    this->process_robot_notification_(chunk.data(), chunk.size());
  }

  this->handle_polling_();
  this->handle_command_queue_();
}

float DolphinBle::get_setup_priority() const { return setup_priority::DATA; }

void DolphinBle::maybe_start_gatt_() {
  if (this->gatt_setup_started_)
    return;
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
    return;
  this->gatt_setup_started_ = true;

  esp_err_t err = esp_ble_gatts_register_callback(DolphinBle::gatts_event_handler_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gatts_register_callback failed: %s", esp_err_to_name(err));
    this->gatt_setup_started_ = false;
    return;
  }

  err = esp_ble_gattc_register_callback(DolphinBle::gattc_event_handler_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gattc_register_callback failed: %s", esp_err_to_name(err));
    this->gatt_setup_started_ = false;
    return;
  }

  esp_ble_gatt_set_local_mtu(517);
  esp_ble_gatts_app_register(GATTS_APP_ID);
  esp_ble_gattc_app_register(GATTC_APP_ID);
}

void DolphinBle::gatts_event_handler_(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                      esp_ble_gatts_cb_param_t *param) {
  if (instance_ != nullptr)
    instance_->on_gatts_event_(event, gatts_if, param);
}

void DolphinBle::gattc_event_handler_(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                      esp_ble_gattc_cb_param_t *param) {
  if (instance_ != nullptr)
    instance_->on_gattc_event_(event, gattc_if, param);
}

void DolphinBle::on_gatts_event_(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param) {
  switch (event) {
    case ESP_GATTS_REG_EVT:
      ESP_LOGI(TAG, "GATTS registered, app_id=%u status=%u", param->reg.app_id, param->reg.status);
      if (param->reg.status == ESP_GATT_OK) {
        this->gatts_if_ = gatts_if;
        this->create_local_server_(gatts_if);
      }
      break;

    case ESP_GATTS_CREATE_EVT:
      ESP_LOGI(TAG, "Local GATT service created, status=%u handle=%u", param->create.status,
               param->create.service_handle);
      this->gatts_service_handle_ = param->create.service_handle;
      esp_ble_gatts_start_service(this->gatts_service_handle_);
      {
        esp_attr_value_t char_value{};
        char_value.attr_max_len = 512;
        char_value.attr_len = 0;
        char_value.attr_value = nullptr;
        esp_ble_gatts_add_char(this->gatts_service_handle_, &this->char_uuid_, ESP_GATT_PERM_READ,
                               ESP_GATT_CHAR_PROP_BIT_NOTIFY, &char_value, nullptr);
      }
      break;

    case ESP_GATTS_ADD_CHAR_EVT:
      ESP_LOGI(TAG, "Local notify characteristic added, status=%u attr_handle=%u",
               param->add_char.status, param->add_char.attr_handle);
      this->gatts_char_handle_ = param->add_char.attr_handle;
      esp_ble_gatts_add_char_descr(this->gatts_service_handle_, &this->cccd_uuid_,
                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, nullptr, nullptr);
      break;

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
      ESP_LOGI(TAG, "Local CCCD added, status=%u attr_handle=%u", param->add_char_descr.status,
               param->add_char_descr.attr_handle);
      this->gatts_cccd_handle_ = param->add_char_descr.attr_handle;
      this->server_ready_ = param->add_char_descr.status == ESP_GATT_OK;
      break;

    case ESP_GATTS_CONNECT_EVT:
      this->gatts_conn_id_ = param->connect.conn_id;
      this->gatts_peer_connected_ = true;
      ESP_LOGI(TAG, "Remote peer connected to local GATT server, conn_id=%u", this->gatts_conn_id_);
      break;

    case ESP_GATTS_DISCONNECT_EVT:
      this->gatts_peer_connected_ = false;
      this->local_notify_enabled_ = false;
      ESP_LOGW(TAG, "Remote peer disconnected from local GATT server, reason=0x%02x",
               param->disconnect.reason);
      break;

    case ESP_GATTS_CONF_EVT:
      if (this->protocol_debug_logging_)
        ESP_LOGD(TAG, "Local notify confirmation, status=%u", param->conf.status);
      break;

    case ESP_GATTS_WRITE_EVT:
      ESP_LOGI(TAG, "Local GATT write handle=%u len=%u data=%s", param->write.handle,
               param->write.len, format_hex_(param->write.value, param->write.len).c_str());
      if (param->write.need_rsp) {
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                    ESP_GATT_OK, nullptr);
      }
      if (param->write.handle == this->gatts_cccd_handle_ && param->write.len >= 2) {
        uint16_t cccd = param->write.value[0] | (static_cast<uint16_t>(param->write.value[1]) << 8);
        this->local_notify_enabled_ = (cccd & 0x0001) != 0;
        ESP_LOGI(TAG, "Robot local notification subscription %s",
                 this->local_notify_enabled_ ? "enabled" : "disabled");
      }
      break;

    default:
      ESP_LOGV(TAG, "GATTS event %d", event);
      break;
  }
}

void DolphinBle::on_gattc_event_(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_REG_EVT:
      ESP_LOGI(TAG, "GATTC registered, app_id=%u status=%u", param->reg.app_id, param->reg.status);
      if (param->reg.status == ESP_GATT_OK)
        this->gattc_if_ = gattc_if;
      break;

    case ESP_GATTC_OPEN_EVT:
      ESP_LOGI(TAG, "GATTC open, status=%u conn_id=%u mtu=%u", param->open.status,
               param->open.conn_id, param->open.mtu);
      if (param->open.status == ESP_GATT_OK) {
        this->client_connected_ = true;
        this->gattc_conn_id_ = param->open.conn_id;
        esp_ble_gattc_search_service(gattc_if, this->gattc_conn_id_, nullptr);
      } else {
        this->connect_started_ = false;
      }
      break;

    case ESP_GATTC_SEARCH_RES_EVT:
      this->log_uuid_("Remote service", param->search_res.srvc_id.uuid);
      if (uuid_equal_(param->search_res.srvc_id.uuid, this->service_uuid_)) {
        this->remote_service_start_ = param->search_res.start_handle;
        this->remote_service_end_ = param->search_res.end_handle;
        ESP_LOGI(TAG, "Found robot IOT service handles start=%u end=%u", this->remote_service_start_,
                 this->remote_service_end_);
      }
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
      ESP_LOGI(TAG, "Service search complete, status=%u", param->search_cmpl.status);
      this->discover_remote_characteristic_();
      break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      ESP_LOGI(TAG, "Register-for-notify complete, status=%u handle=%u", param->reg_for_notify.status,
               param->reg_for_notify.handle);
      if (param->reg_for_notify.status == ESP_GATT_OK) {
        this->notify_registered_ = true;
        this->enable_remote_notifications_();
      }
      break;

    case ESP_GATTC_WRITE_DESCR_EVT:
      ESP_LOGI(TAG, "Remote CCCD write complete, status=%u handle=%u", param->write.status,
               param->write.handle);
      if (param->write.status == ESP_GATT_OK) {
        this->cccd_written_ = true;
        esp_ble_gattc_send_mtu_req(gattc_if, this->gattc_conn_id_);
      }
      break;

    case ESP_GATTC_CFG_MTU_EVT:
      ESP_LOGI(TAG, "MTU configured, status=%u mtu=%u", param->cfg_mtu.status, param->cfg_mtu.mtu);
      this->mtu_done_ = param->cfg_mtu.status == ESP_GATT_OK;
      break;

    case ESP_GATTC_NOTIFY_EVT:
      this->handle_robot_notification_(param->notify.value, param->notify.value_len);
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGW(TAG, "GATTC disconnected, reason=0x%02x", param->disconnect.reason);
      this->client_connected_ = false;
      this->connect_started_ = false;
      this->notify_registered_ = false;
      this->cccd_written_ = false;
      this->mtu_done_ = false;
      this->initialization_queued_ = false;
      this->command_queue_.clear();
      this->last_status_poll_ = 0;
      this->last_mu_poll_ = 0;
      this->last_temp_poll_ = 0;
      this->last_remaining_publish_ = 0;
      this->cycle_active_ = false;
      this->current_cycle_start_time_ = 0;
      this->publish_cycle_time_remaining_();
      this->rx_text_buffer_.clear();
      this->remote_service_start_ = 0;
      this->remote_service_end_ = 0;
      this->remote_char_handle_ = 0;
      this->remote_cccd_handle_ = 0;
      break;

    default:
      ESP_LOGV(TAG, "GATTC event %d", event);
      break;
  }
}

void DolphinBle::handle_polling_() {
  if (!this->gatts_peer_connected_ || !this->local_notify_enabled_ || !this->mtu_done_) {
    this->initialization_queued_ = false;
    this->command_queue_.clear();
    this->last_status_poll_ = 0;
    this->last_mu_poll_ = 0;
    this->last_temp_poll_ = 0;
    return;
  }

  uint32_t now = millis();

  if (!this->initialization_queued_) {
    this->queue_initialization_();
    this->initialization_queued_ = true;
    this->last_status_poll_ = now;
    this->last_mu_poll_ = now;
    this->last_temp_poll_ = now;
  }

  // Polls are deduplicated against queued/in-flight commands. This prevents
  // simultaneous status, MU and temperature requests at the 30-second boundary.
  if (now - this->last_status_poll_ >= 2000) {
    this->queue_command_frame_(0x07, 0xFFF8, nullptr, 0, "system_status", true);
    this->last_status_poll_ = now;
  }

  if (now - this->last_mu_poll_ >= 30000) {
    const uint8_t payload[] = {0x01, 0x00, 0xff};
    this->queue_command_frame_(0x01, 0xFFFD, payload, sizeof(payload), "get_mu_data", true);
    this->last_mu_poll_ = now;
  }

  // Product temperature support is a separate profile feature. The
  // compact inWat bit alone is not sufficient evidence that opcode 09 exists.
  if (this->temperature_supported_ && this->numeric_sensors_[NUMERIC_TEMPERATURE] != nullptr) {
    if (now - this->last_temp_poll_ >= 30000) {
      this->queue_command_frame_(0x09, 0xFFF8, nullptr, 0, "temperature", true);
      this->last_temp_poll_ = now;
    }
  }

  if (this->time_id_ != nullptr) {
    if (this->last_rtc_sync_ == 0 || now - this->last_rtc_sync_ >= 3600000) {
      auto rtc_now = this->time_id_->now();
      if (rtc_now.is_valid()) {
        this->send_timezone_();
        this->send_rtc_time_();
        this->last_rtc_sync_ = now;
      }
    }
  }

  if (this->numeric_sensors_[NUMERIC_CYCLE_TIME_REMAINING] != nullptr &&
      now - this->last_remaining_publish_ >= 1000) {
    this->publish_cycle_time_remaining_();
    this->last_remaining_publish_ = now;
  }
}

void DolphinBle::queue_initialization_() {
  ESP_LOGI(TAG, "Queueing robot initialization commands in protocol order");
  if (this->time_id_ != nullptr && this->time_id_->now().is_valid()) {
    this->send_timezone_();
    this->send_rtc_time_();
    this->last_rtc_sync_ = millis();
  }

  const uint8_t sm_payload[] = {0x02, 0x00, 0xff};
  this->queue_command_frame_(0x02, 0xFFFD, sm_payload, sizeof(sm_payload), "get_sm_data", true);
  const uint8_t mu_payload[] = {0x01, 0x00, 0xff};
  this->queue_command_frame_(0x01, 0xFFFD, mu_payload, sizeof(mu_payload), "get_mu_data", true);
  this->queue_command_frame_(0x07, 0xFFF8, nullptr, 0, "system_status", true);
  this->queue_command_frame_(0x1a, 0xFFFA, nullptr, 0, "pws_features", true);
  this->queue_command_frame_(0xdf, 0xFFFE, nullptr, 0, "cloud_connection_status", true);
  if (this->temperature_supported_ && this->numeric_sensors_[NUMERIC_TEMPERATURE] != nullptr)
    this->queue_command_frame_(0x09, 0xFFF8, nullptr, 0, "temperature", true);
}

void DolphinBle::send_timezone_() {
  int16_t offset_minutes = static_cast<int16_t>(ESPTime::timezone_offset() / 60);
  uint16_t encoded = static_cast<uint16_t>(offset_minutes);
  uint8_t payload[] = {static_cast<uint8_t>(encoded >> 8), static_cast<uint8_t>(encoded & 0xff)};
  this->send_command_frame_(0xc2, 0xFFFE, payload, sizeof(payload), "timezone");
}

void DolphinBle::send_rtc_time_() {
  if (this->time_id_ == nullptr)
    return;
  auto now = this->time_id_->now();
  if (!now.is_valid())
    return;

  uint32_t timestamp = now.timestamp;
  uint8_t payload[4];
  payload[0] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
  payload[1] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
  payload[2] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
  payload[3] = static_cast<uint8_t>(timestamp & 0xFF);

  this->send_command_frame_(0x09, 0xFFF9, payload, 4, "RealTimeClock");
}

bool DolphinBle::send_local_notification_text_(const std::string &text) {
  if (this->gatts_if_ == ESP_GATT_IF_NONE || this->gatts_char_handle_ == 0 ||
      !this->gatts_peer_connected_ || !this->local_notify_enabled_) {
    return false;
  }
  esp_err_t err = esp_ble_gatts_send_indicate(
      this->gatts_if_, this->gatts_conn_id_, this->gatts_char_handle_, text.size(),
      reinterpret_cast<uint8_t *>(const_cast<char *>(text.data())), false);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Local notify for raw text failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

void DolphinBle::send_command_frame_(uint8_t opcode, uint16_t destination, const uint8_t *payload,
                                     size_t payload_len, const char *name, bool expects_response) {
  this->queue_command_frame_(opcode, destination, payload, payload_len, name, false, expects_response);
}

void DolphinBle::queue_command_frame_(uint8_t opcode, uint16_t destination, const uint8_t *payload,
                                      size_t payload_len, const char *name, bool deduplicate, bool expects_response) {
  std::vector<uint8_t> frame;
  frame.reserve(7 + payload_len + 2);
  frame.push_back(0xab);
  frame.push_back(0x03);
  frame.push_back(static_cast<uint8_t>((destination >> 8) & 0xff));
  frame.push_back(static_cast<uint8_t>(destination & 0xff));
  frame.push_back(opcode);
  frame.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xff));
  frame.push_back(static_cast<uint8_t>(payload_len & 0xff));
  if (payload != nullptr && payload_len > 0)
    frame.insert(frame.end(), payload, payload + payload_len);

  uint16_t checksum = 0;
  for (uint8_t byte : frame)
    checksum = static_cast<uint16_t>(checksum + byte);
  frame.push_back(static_cast<uint8_t>((checksum >> 8) & 0xff));
  frame.push_back(static_cast<uint8_t>(checksum & 0xff));

  std::string text = "03:";
  text += format_hex_(frame.data(), frame.size());
  if (deduplicate) {
    for (const auto &pending : this->command_queue_) {
      if (pending.opcode == opcode && pending.destination == destination)
        return;
    }
  }
  this->command_queue_.push_back({opcode, destination, name, text, 0, 0, expects_response});
  if (this->protocol_debug_logging_) {
    ESP_LOGD(TAG, "Queued command %s opcode=0x%02x dest=0x%04x payload=%s expects_response=%d", name, opcode,
             destination, payload_len ? format_hex_(payload, payload_len).c_str() : "", expects_response);
  }
}

void DolphinBle::send_command_frame_(uint8_t opcode, uint16_t destination,
                                     std::initializer_list<uint8_t> payload, const char *name, bool expects_response) {
  std::vector<uint8_t> data(payload.begin(), payload.end());
  this->send_command_frame_(opcode, destination, data.data(), data.size(), name, expects_response);
}

void DolphinBle::request_status_refresh_() {
  this->queue_command_frame_(0x07, 0xFFF8, nullptr, 0, "system_status", true);
  this->last_status_poll_ = millis();
}

void DolphinBle::handle_command_queue_() {
  if (this->command_queue_.empty() || !this->gatts_peer_connected_ || !this->local_notify_enabled_ ||
      !this->mtu_done_)
    return;

  auto &pending = this->command_queue_.front();
  uint32_t now = millis();
  if (pending.sent_at != 0 && now - pending.sent_at < 2000)
    return;
  if (pending.attempts >= 2) {
    ESP_LOGW(TAG, "Command %s timed out after two attempts", pending.name.c_str());
    this->command_queue_.pop_front();
    return;
  }

  pending.attempts++;
  pending.sent_at = now == 0 ? 1 : now;
  this->tx_text_buffer_ = pending.text;
  if (this->protocol_debug_logging_) {
    ESP_LOGI(TAG, "Sending command %s opcode=0x%02x dest=0x%04x attempt=%u text=\"%s\"",
             pending.name.c_str(), pending.opcode, pending.destination, pending.attempts,
             pending.text.c_str());
  } else if (pending.name != "system_status") {
    ESP_LOGD(TAG, "Sending command %s opcode=0x%02x dest=0x%04x attempt=%u",
             pending.name.c_str(), pending.opcode, pending.destination, pending.attempts);
  }
  this->send_local_notification_text_(pending.text);
  if (!pending.expects_response) {
    this->command_queue_.pop_front();
  }
}

void DolphinBle::handle_command_response_(uint16_t destination, uint8_t opcode) {
  if (this->command_queue_.empty())
    return;
  const auto &pending = this->command_queue_.front();
  if (pending.destination != destination || pending.opcode != opcode) {
    if (this->protocol_debug_logging_) {
      ESP_LOGD(TAG, "Unsolicited/non-matching response dest=0x%04x opcode=0x%02x while waiting for %s",
               destination, opcode, pending.name.c_str());
    }
    return;
  }
  if (this->protocol_debug_logging_)
    ESP_LOGD(TAG, "Command %s received matching response", pending.name.c_str());
  this->command_queue_.pop_front();
}

void DolphinBle::handle_robot_notification_(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0)
    return;
  std::lock_guard<std::mutex> lock(this->rx_mutex_);
  this->rx_queue_.push_back(std::vector<uint8_t>(data, data + len));
}

void DolphinBle::process_robot_notification_(const uint8_t *data, size_t len) {
  std::string text = format_ascii_(data, len);
  ESP_LOGV(TAG, "Robot notification chunk len=%u text=\"%s\"", static_cast<unsigned>(len), text.c_str());

  if (!is_text_frame_chunk_(data, len))
    return;

  std::string chunk(reinterpret_cast<const char *>(data), len);
  if (this->rx_text_buffer_.empty()) {
    size_t colon = chunk.find(':');
    if (colon == std::string::npos)
      return;
    this->rx_text_buffer_ = chunk.substr(colon);
  } else {
    this->rx_text_buffer_ += chunk;
  }

  this->maybe_log_complete_text_frame_();
}

void DolphinBle::maybe_log_complete_text_frame_() {
  while (!this->rx_text_buffer_.empty()) {
    size_t colon = this->rx_text_buffer_.find(':');
    if (colon == std::string::npos) {
      this->rx_text_buffer_.clear();
      return;
    }
    if (colon > 0)
      this->rx_text_buffer_.erase(0, colon);

    const std::string hex = this->rx_text_buffer_.substr(1);
    if (hex.size() < 14)
      return;

    std::vector<uint8_t> header;
    if (!parse_hex_(hex.substr(0, 14), &header) || header.size() < 7) {
      ESP_LOGW(TAG, "Dropping malformed robot text frame prefix: %s", this->rx_text_buffer_.c_str());
      this->rx_text_buffer_.clear();
      return;
    }

    uint16_t payload_len = (static_cast<uint16_t>(header[5]) << 8) | header[6];
    size_t expected_hex_len = (7 + payload_len + 2) * 2;
    size_t expected_text_len = 1 + expected_hex_len;
    if (this->rx_text_buffer_.size() < expected_text_len)
      return;

    std::string frame_hex = this->rx_text_buffer_.substr(1, expected_hex_len);
    std::vector<uint8_t> frame;
    if (!parse_hex_(frame_hex, &frame) || frame.size() != 7 + payload_len + 2) {
      ESP_LOGW(TAG, "Dropping malformed complete robot text frame");
      this->rx_text_buffer_.erase(0, expected_text_len);
      continue;
    }

    uint16_t calculated = 0;
    for (size_t i = 0; i + 2 < frame.size(); i++)
      calculated = static_cast<uint16_t>(calculated + frame[i]);
    uint16_t received =
        (static_cast<uint16_t>(frame[frame.size() - 2]) << 8) | frame[frame.size() - 1];
    uint16_t dest = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];

    if (this->protocol_debug_logging_) {
      if (payload_len < 100) {
        ESP_LOGI(TAG,
                 "Robot text frame src=0x%02x dest=0x%04x opcode=0x%02x payload_len=%u checksum=%s hex=%s",
                 frame[1], dest, frame[4], payload_len, calculated == received ? "ok" : "bad", frame_hex.c_str());
      } else {
        ESP_LOGI(TAG,
                 "Robot text frame src=0x%02x dest=0x%04x opcode=0x%02x payload_len=%u checksum=%s",
                 frame[1], dest, frame[4], payload_len, calculated == received ? "ok" : "bad");
      }
    }

    if (calculated == received)
      this->parse_robot_text_frame_(frame);

    this->rx_text_buffer_.erase(0, expected_text_len);
  }
}

void DolphinBle::parse_robot_text_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 9)
    return;
  uint16_t dest = read_u16_be_(&frame[2]);
  uint8_t opcode = frame[4];
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;

  this->handle_command_response_(dest, opcode);
  if (payload_len == 0) {
    ESP_LOGW(TAG, "Response dest=0x%04x opcode=0x%02x has no ACK byte", dest, opcode);
    return;
  }
  uint8_t ack = payload[0];
  if (ack != 0) {
    ESP_LOGW(TAG, "Command response dest=0x%04x opcode=0x%02x returned ACK/status 0x%02x",
             dest, opcode, ack);
    return;
  }

  if (dest == 0xFFFA && opcode == 0x1a) {
    this->publish_pws_features_from_frame_(frame);
    return;
  }
  if (dest == 0xFFF8 && opcode == 0x07) {
    this->publish_status_from_frame_(frame);
    return;
  }
  if (dest == 0xFFF8 && opcode == 0x09) {
    this->publish_temperature_from_frame_(frame);
    return;
  }
  if (dest == 0xFFFD && opcode == 0x01) {
    if (this->protocol_debug_logging_ && payload_len >= 160) {
      ESP_LOGI(TAG, "MU payload bytes 130-160: %s", this->format_hex_(payload + 130, 30).c_str());
      ESP_LOGI(TAG, "MU LED window: raw[156]=0x%02X raw[157]=0x%02X raw[158]=0x%02X "
                    "(data[155], data[156], data[157])",
               payload[156], payload[157], payload[158]);
    }
    this->publish_mu_data_from_frame_(frame);
    return;
  }
  if (dest == 0xFFFD && opcode == 0x02) {
    if (this->protocol_debug_logging_ && payload_len >= 80) {
      ESP_LOGI(TAG, "SM payload bytes 55-75: %s", this->format_hex_(payload + 55, 20).c_str());
    }
    this->publish_sm_data_from_frame_(frame);
    return;
  }
  if (dest == 0xFFF9 && opcode == 0x45) {
    this->parse_weekly_timer_response_(frame);
    return;
  }
  if (dest == 0xFFF9 && opcode == 0x47) {
    ESP_LOGI(TAG, "Single day schedule updated and confirmed by PWS.");
    return;
  }

  ESP_LOGD(TAG, "Unrecognized robot response dest=0x%04x opcode=0x%02x payload_len=%u", dest, opcode,
           static_cast<unsigned>(payload_len));
  (void) payload;
}

void DolphinBle::log_uuid_(const char *prefix, const esp_bt_uuid_t &uuid) {
  if (uuid.len == ESP_UUID_LEN_16) {
    ESP_LOGD(TAG, "%s UUID16 0x%04x", prefix, uuid.uuid.uuid16);
  } else if (uuid.len == ESP_UUID_LEN_32) {
    ESP_LOGD(TAG, "%s UUID32 0x%08" PRIx32, prefix, uuid.uuid.uuid32);
  } else if (uuid.len == ESP_UUID_LEN_128) {
    ESP_LOGD(TAG, "%s UUID128 %s", prefix, format_hex_(uuid.uuid.uuid128, ESP_UUID_LEN_128).c_str());
  }
}

bool DolphinBle::uuid_from_string_(const char *uuid, esp_bt_uuid_t *out) {
  unsigned int b[16];
  int matched = std::sscanf(uuid,
                            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                            "%02x%02x%02x%02x%02x%02x",
                            &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7], &b[8], &b[9],
                            &b[10], &b[11], &b[12], &b[13], &b[14], &b[15]);
  if (matched != 16)
    return false;
  out->len = ESP_UUID_LEN_128;
  for (int i = 0; i < 16; i++)
    out->uuid.uuid128[i] = static_cast<uint8_t>(b[15 - i]);
  return true;
}

bool DolphinBle::uuid_equal_(const esp_bt_uuid_t &a, const esp_bt_uuid_t &b) {
  if (a.len != b.len)
    return false;
  if (a.len == ESP_UUID_LEN_16)
    return a.uuid.uuid16 == b.uuid.uuid16;
  if (a.len == ESP_UUID_LEN_32)
    return a.uuid.uuid32 == b.uuid.uuid32;
  return std::memcmp(a.uuid.uuid128, b.uuid.uuid128, ESP_UUID_LEN_128) == 0;
}

bool DolphinBle::parse_mac_() {
  unsigned int b[6];
  if (std::sscanf(this->mac_address_.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2],
                  &b[3], &b[4], &b[5]) != 6)
    return false;
  for (int i = 0; i < 6; i++)
    this->remote_bda_[i] = static_cast<uint8_t>(b[i]);
  return true;
}

void DolphinBle::maybe_connect_() {
  if (!this->parsed_mac_ || !this->server_ready_ || this->gattc_if_ == ESP_GATT_IF_NONE ||
      this->connect_started_ || this->client_connected_) {
    return;
  }

  ESP_LOGI(TAG, "Opening direct GATT client connection to %s", this->mac_address_.c_str());
  this->connect_started_ = true;
  esp_err_t err = esp_ble_gattc_open(this->gattc_if_, this->remote_bda_, BLE_ADDR_TYPE_PUBLIC, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gattc_open failed: %s", esp_err_to_name(err));
    this->connect_started_ = false;
  }
}

void DolphinBle::create_local_server_(esp_gatt_if_t gatts_if) {
  esp_gatt_srvc_id_t service_id{};
  service_id.is_primary = true;
  service_id.id.inst_id = 0;
  service_id.id.uuid = this->service_uuid_;
  ESP_LOGI(TAG, "Creating local Maytronics GATT server service");
  esp_ble_gatts_create_service(gatts_if, &service_id, 6);
}

void DolphinBle::discover_remote_characteristic_() {
  if (this->remote_service_start_ == 0 || this->remote_service_end_ == 0) {
    ESP_LOGE(TAG, "Robot IOT service was not found");
    return;
  }

  uint16_t count = 0;
  esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
      this->gattc_if_, this->gattc_conn_id_, ESP_GATT_DB_CHARACTERISTIC, this->remote_service_start_,
      this->remote_service_end_, INVALID_HANDLE, &count);
  if (status != ESP_GATT_OK || count == 0) {
    ESP_LOGE(TAG, "No remote characteristics found, status=%u count=%u", status, count);
    return;
  }

  std::vector<esp_gattc_char_elem_t> chars(count);
  status = esp_ble_gattc_get_char_by_uuid(this->gattc_if_, this->gattc_conn_id_,
                                          this->remote_service_start_, this->remote_service_end_,
                                          this->char_uuid_, chars.data(), &count);
  if (status != ESP_GATT_OK || count == 0) {
    ESP_LOGE(TAG, "Robot IOT characteristic not found, status=%u count=%u", status, count);
    return;
  }

  this->remote_char_handle_ = chars[0].char_handle;
  ESP_LOGI(TAG, "Found robot IOT characteristic handle=%u properties=0x%02x",
           this->remote_char_handle_, chars[0].properties);

  uint16_t descr_count = 1;
  esp_gattc_descr_elem_t descr{};
  status = esp_ble_gattc_get_descr_by_char_handle(this->gattc_if_, this->gattc_conn_id_,
                                                  this->remote_char_handle_, this->cccd_uuid_, &descr,
                                                  &descr_count);
  if (status != ESP_GATT_OK || descr_count == 0) {
    ESP_LOGE(TAG, "Robot CCCD not found, status=%u count=%u", status, descr_count);
    return;
  }
  this->remote_cccd_handle_ = descr.handle;
  ESP_LOGI(TAG, "Found robot CCCD handle=%u", this->remote_cccd_handle_);

  esp_ble_gattc_register_for_notify(this->gattc_if_, this->remote_bda_, this->remote_char_handle_);
}

void DolphinBle::enable_remote_notifications_() {
  if (this->remote_cccd_handle_ == 0)
    return;
  uint16_t notify_en = 1;
  ESP_LOGI(TAG, "Writing robot CCCD to enable notifications");
  esp_ble_gattc_write_char_descr(this->gattc_if_, this->gattc_conn_id_, this->remote_cccd_handle_,
                                 sizeof(notify_en), reinterpret_cast<uint8_t *>(&notify_en),
                                 ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

bool DolphinBle::parse_hex_(const std::string &hex, std::vector<uint8_t> *out) {
  std::string compact;
  compact.reserve(hex.size());
  for (char c : hex) {
    if (c == ' ' || c == ':' || c == '-' || c == '_')
      continue;
    compact.push_back(c);
  }
  if (compact.size() % 2 != 0)
    return false;

  out->clear();
  out->reserve(compact.size() / 2);
  for (size_t i = 0; i < compact.size(); i += 2) {
    unsigned int byte = 0;
    if (std::sscanf(compact.substr(i, 2).c_str(), "%02x", &byte) != 1)
      return false;
    out->push_back(static_cast<uint8_t>(byte));
  }
  return true;
}

std::string DolphinBle::format_hex_(const uint8_t *data, size_t len) {
  static constexpr char HEX[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(HEX[(data[i] >> 4) & 0x0F]);
    out.push_back(HEX[data[i] & 0x0F]);
  }
  return out;
}

std::string DolphinBle::format_ascii_(const uint8_t *data, size_t len) {
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (c >= 0x20 && c <= 0x7e) {
      if (c == '\\' || c == '"')
        out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('.');
    }
  }
  return out;
}

bool DolphinBle::is_text_frame_chunk_(const uint8_t *data, size_t len) {
  if (len == 0)
    return false;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex && c != ':')
      return false;
  }
  return true;
}

uint16_t DolphinBle::read_u16_be_(const uint8_t *data) {
  return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint32_t DolphinBle::read_u32_be_(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint16_t DolphinBle::read_u16_le_(const uint8_t *data) {
  return (static_cast<uint16_t>(data[1]) << 8) | data[0];
}

uint32_t DolphinBle::read_u32_le_(const uint8_t *data) {
  return (static_cast<uint32_t>(data[3]) << 24) | (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[1]) << 8) | data[0];
}

uint64_t DolphinBle::read_u64_be_(const uint8_t *data, size_t len) {
  uint64_t out = 0;
  for (size_t i = 0; i < len; i++)
    out = (out << 8) | data[i];
  return out;
}

std::string DolphinBle::hex_string_(const uint8_t *data, size_t len) { return format_hex_(data, len); }

std::string DolphinBle::summarize_printable_runs_(const uint8_t *data, size_t len, size_t max_runs,
                                                  size_t max_run_len) {
  std::string out;
  std::string current;
  size_t runs = 0;

  auto flush = [&]() {
    if (current.size() < 4 || runs >= max_runs) {
      current.clear();
      return;
    }
    if (!out.empty())
      out += " | ";
    if (current.size() > max_run_len)
      current.resize(max_run_len);
    out += current;
    current.clear();
    runs++;
  };

  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (c >= 0x20 && c <= 0x7e) {
      current.push_back(static_cast<char>(c));
    } else {
      flush();
      if (runs >= max_runs)
        break;
    }
  }
  flush();

  if (out.empty())
    return "NA";
  return out;
}

std::string DolphinBle::mode_to_string_(uint8_t mode) {
  switch (mode) {
    case 0x00:
      return "None";
    case 0xfe: // -2 unsigned
      return "Empty";
    case 0x01:
      return "All Surfaces";
    case 0x02:
      return "Quick Clean";
    case 0x03:
      return "Cove";
    case 0x04:
      return "Floor Only";
    case 0x05:
      return "Water Line";
    case 0x06:
      return "Ultra Clean";
    case 0x07:
      return "Spot";
    case 0x08:
      return "Wall Only";
    case 0x09:
      return "Tic Tac";
    case 0x0a:
      return "Custom";
    case 0x0b:
      return "Pickup";
    case 0x0f:
      return "Stairs";
    default:
      return "Unknown";
  }
}

uint8_t DolphinBle::mode_from_string_(const std::string &mode) {
  if (mode == "All Surfaces")
    return 0x01;
  if (mode == "Quick Clean")
    return 0x02;
  if (mode == "Cove")
    return 0x03;
  if (mode == "Floor Only")
    return 0x04;
  if (mode == "Water Line")
    return 0x05;
  if (mode == "Ultra Clean")
    return 0x06;
  if (mode == "Spot")
    return 0x07;
  if (mode == "Wall Only")
    return 0x08;
  if (mode == "Tic Tac")
    return 0x09;
  if (mode == "Custom")
    return 0x0a;
  if (mode == "Pickup")
    return 0x0b;
  if (mode == "Stairs")
    return 0x0f;
  if (mode == "Empty")
    return 0xfe;
  return 0x01;
}

std::string DolphinBle::direction_to_string_(uint8_t direction) {
  switch (direction) {
    case 0x01:
      return "Stop";
    case 0x02:
      return "Forward";
    case 0x03:
      return "Backward";
    case 0x04:
      return "Right";
    case 0x05:
      return "Left";
    default:
      return "Stop";
  }
}

uint8_t DolphinBle::direction_from_string_(const std::string &direction) {
  if (direction == "Forward")
    return 0x02;
  if (direction == "Backward")
    return 0x03;
  if (direction == "Right")
    return 0x04;
  if (direction == "Left")
    return 0x05;
  return 0x01;
}

std::string DolphinBle::robot_state_to_string_(uint8_t state) {
  switch (state) {
    case 0x00:
      return "Idle";
    case 0x01:
      return "Mapping";
    case 0x02:
      return "Cleaning";
    case 0x03:
      return "Recovery";
    case 0x04:
      return "Finished";
    case 0x05:
      return "Fault";
    case 0x06:
      return "Programming";
    case 0x07:
      return "Not Connected";
    default:
      return "Unknown";
  }
}

std::string DolphinBle::pws_state_to_string_(uint8_t state) {
  switch (state) {
    case 0x00:
      return "Off";
    case 0x01:
      return "Active";
    case 0x02:
      return "Hold (Weekly)";
    case 0x03:
      return "Hold (Delay)";
    case 0x04:
      return "Programming";
    case 0x05:
      return "Cleaning";
    case 0x06:
      return "Sleep";
    default:
      return "Unknown";
  }
}

std::string DolphinBle::water_status_to_string_(uint8_t state) {
  switch (state) {
    case 0x00:
      return "Not In Water";
    case 0x01:
      return "In Water";
    case 0x02:
      return "Unknown";
    case 0x03:
      return "Error";
    case 0x04:
      return "No Barometer";
    case 0x0f:
      return "Loading";
    default:
      return "N/A";
  }
}

std::string DolphinBle::filter_status_to_string_(uint8_t state) {
  if (state == 0) return "empty";
  if (state >= 1 && state <= 25) return "partially_full";
  if (state >= 26 && state <= 74) return "getting_full";
  if (state >= 75 && state <= 99) return "almost_full";
  if (state == 100) return "full";
  if (state == 101) return "fault";
  if (state == 102) return "not_available";
  if (state == 255) return "unknown";
  return "unknown";
}

void DolphinBle::publish_numeric_(uint8_t kind, float value) {
  if (kind < this->numeric_sensors_.size() && this->numeric_sensors_[kind] != nullptr)
    this->numeric_sensors_[kind]->publish_state(value);
}

bool DolphinBle::get_configured_cycle_duration_seconds_(uint32_t *seconds) const {
  if (!this->configured_cycle_times_known_) {
    return false;
  }
  uint16_t minutes;
  if (this->selected_cleaning_mode_ >= 1 && this->selected_cleaning_mode_ <= 11) {
    minutes = this->configured_cycle_times_mins_[this->selected_cleaning_mode_ - 1];
  } else if (this->selected_cleaning_mode_ == 15) {
    minutes = this->stairs_cycle_time_mins_;
  } else {
    return false;
  }
  if (minutes == 0 || minutes == 0xffff || minutes > 24 * 60) {
    return false;
  }
  *seconds = static_cast<uint32_t>(minutes) * 60UL;
  return true;
}

void DolphinBle::publish_configured_cycle_duration_() {
  uint32_t seconds = 0;
  if (!this->get_configured_cycle_duration_seconds_(&seconds)) {
    this->publish_numeric_(NUMERIC_CYCLE_DURATION, NAN);
    return;
  }
  this->publish_numeric_(NUMERIC_CYCLE_DURATION, static_cast<float>(seconds));
}

void DolphinBle::publish_cycle_time_remaining_() {
  uint32_t duration_seconds = 0;
  if (!this->cycle_active_ || this->current_cycle_start_time_ == 0 ||
      !this->get_configured_cycle_duration_seconds_(&duration_seconds) || this->time_id_ == nullptr) {
    this->publish_numeric_(NUMERIC_CYCLE_TIME_REMAINING, NAN);
    return;
  }

  auto now = this->time_id_->now();
  if (!now.is_valid() || now.timestamp < this->current_cycle_start_time_) {
    this->publish_numeric_(NUMERIC_CYCLE_TIME_REMAINING, NAN);
    return;
  }

  uint32_t elapsed = now.timestamp - this->current_cycle_start_time_;
  uint32_t remaining = elapsed >= duration_seconds ? 0 : duration_seconds - elapsed;
  this->publish_numeric_(NUMERIC_CYCLE_TIME_REMAINING, static_cast<float>(remaining));
}

void DolphinBle::publish_text_(uint8_t kind, const std::string &value) {
  if (kind < this->text_sensors_.size() && this->text_sensors_[kind] != nullptr)
    this->text_sensors_[kind]->publish_state(value);
}

void DolphinBle::publish_current_cleaning_mode_(uint8_t mode) {
  if ((mode >= 1 && mode <= 11) || mode == 15) {
    this->selected_cleaning_mode_ = mode;
  }
  std::string value = mode_to_string_(mode);
  this->publish_text_(TEXT_CLEANING_MODE, value);
  // Stairs is product-feature gated and is not offered by the generic select.
  if (this->cleaning_mode_select_ != nullptr && mode >= 1 && mode <= 11)
    this->cleaning_mode_select_->publish_state(value);
  this->publish_configured_cycle_duration_();
  this->publish_cycle_time_remaining_();
}

void DolphinBle::publish_pws_features_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 10)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;
  if (payload_len < 3)
    return;

  // Unlike the other structured responses, PWS features is a compact
  // three-byte reply. Its feature bitfield is payload byte 2.
  // LSB-first bit mapping:
  // bit 0: Network Sensing (WiFi support)
  // bit 1: In-Water capability
  // bit 2: Cellular support
  // bit 3: OTA support
  // bit 4: PCS support
  uint8_t bits = payload[2];
  bool network_sensing = (bits & 0x01) != 0;
  this->in_water_capable_ = (bits & 0x02) != 0;
  bool cellular = (bits & 0x04) != 0;
  bool ota = (bits & 0x08) != 0;
  bool pcs = (bits & 0x10) != 0;

  this->in_water_capability_known_ = true;

  std::string summary = "network_sensing=" + std::string(network_sensing ? "true" : "false");
  summary += " in_water=" + std::string(this->in_water_capable_ ? "true" : "false");
  summary += " cellular=" + std::string(cellular ? "true" : "false");
  summary += " ota=" + std::string(ota ? "true" : "false");
  summary += " pcs=" + std::string(pcs ? "true" : "false");

  this->publish_text_(TEXT_PWS_FEATURES, summary);
  ESP_LOGI(TAG, "PWS features: %s", summary.c_str());
}

void DolphinBle::publish_status_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 16)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;

  // The first status payload byte is the response ACK. All system_status
  // fields therefore start one byte later in the raw ESPHome frame.
  this->publish_text_(TEXT_ROBOT_STATE, robot_state_to_string_(payload[1]));
  this->publish_text_(TEXT_PWS_STATE, pws_state_to_string_(payload[2]));
  uint8_t filter_state = payload[3];
  this->publish_numeric_(NUMERIC_FILTER_STATE,
                         filter_state == 0x66 || filter_state == 0xff ? NAN : filter_state);
  this->publish_text_(TEXT_FILTER_STATUS, filter_status_to_string_(filter_state));
  this->publish_current_cleaning_mode_(payload[4]);

  constexpr size_t D = 1;
  if (payload_len >= 30) {
    uint8_t faults_code = payload[18 + D];
    if (faults_code != 0 && faults_code != 0xFF) {
      uint16_t faults_pcb_hrs = read_u16_be_(payload + 19 + D);
      uint8_t faults_pcb_mins = payload[21 + D];
      uint16_t faults_cycle_counter = read_u16_be_(payload + 22 + D);
      uint16_t faults_val_1 = read_u16_be_(payload + 24 + D);
      uint16_t faults_val_2 = read_u16_be_(payload + 26 + D);
      uint16_t faults_val_3 = read_u16_be_(payload + 28 + D);

      char fault_buf[128];
      std::snprintf(fault_buf, sizeof(fault_buf),
                    "Code: 0x%02X, PCB Hours: %u:%02u, Cycle: %u, V1: 0x%04X, V2: 0x%04X, V3: 0x%04X",
                    faults_code, faults_pcb_hrs, faults_pcb_mins, faults_cycle_counter,
                    faults_val_1, faults_val_2, faults_val_3);
      this->publish_text_(TEXT_ACTIVE_FAULT, fault_buf);
    } else {
      this->publish_text_(TEXT_ACTIVE_FAULT, "OK");
    }
  } else {
    this->publish_text_(TEXT_ACTIVE_FAULT, "OK");
  }

  if (payload_len >= 14) {
    // Cycle information contains cycle type/time, device uptime, and UTC
    // cycle start time. With the ACK at raw payload[0], the UTC field is raw
    // payload bytes 11-14.
    uint32_t start_time = read_u32_be_(payload + 11);
    bool cycle_active = payload[2] == 0x05 || payload[1] == 0x01 || payload[1] == 0x02 || payload[1] == 0x03;
    bool timestamp_sane = start_time >= 1577836800UL && start_time <= 4102444800UL;
    if (!cycle_active || !timestamp_sane) {
      this->cycle_active_ = false;
      this->current_cycle_start_time_ = 0;
      this->publish_numeric_(NUMERIC_CYCLE_START_TIME, NAN);
      this->publish_cycle_time_remaining_();
    } else {
      bool too_far_in_future = false;
      if (this->time_id_ != nullptr) {
        auto now = this->time_id_->now();
        too_far_in_future = now.is_valid() && start_time > now.timestamp + 86400UL;
      }
      if (too_far_in_future) {
        this->cycle_active_ = false;
        this->current_cycle_start_time_ = 0;
        this->publish_numeric_(NUMERIC_CYCLE_START_TIME, NAN);
        this->publish_cycle_time_remaining_();
      } else {
        this->cycle_active_ = true;
        this->current_cycle_start_time_ = start_time;
        this->publish_numeric_(NUMERIC_CYCLE_START_TIME, static_cast<float>(start_time));
        this->publish_cycle_time_remaining_();
      }
    }
  }
}

void DolphinBle::publish_temperature_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 13)
    return;
  const uint8_t *payload = &frame[7];
  // The first payload byte is the response ACK.
  this->publish_text_(TEXT_IN_WATER_STATUS, water_status_to_string_(payload[1]));

  int16_t temperature = static_cast<int16_t>(read_u16_be_(payload + 2));
  if (temperature == 0xFFFF || temperature == 0x3E9 || temperature == 0x3EA || temperature < 0) {
    this->publish_numeric_(NUMERIC_TEMPERATURE, NAN);
  } else {
    this->publish_numeric_(NUMERIC_TEMPERATURE, static_cast<float>(temperature) / 10.0f);
  }
}

void DolphinBle::publish_mu_data_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 16)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;

  if (payload_len >= 172) {
    // These offsets apply after stripping the response ACK byte.
    constexpr size_t D = 1;
    this->publish_numeric_(NUMERIC_ROBOT_TYPE, read_u16_le_(payload + 132 + D));
    this->publish_numeric_(NUMERIC_MU_FLASH_WRITE_COUNTER, read_u32_le_(payload + 134 + D));
    this->publish_numeric_(NUMERIC_TURN_ON_COUNT, read_u16_le_(payload + 146 + D));
    this->publish_numeric_(NUMERIC_MU_NOT_COMPLETED_CYCLES, read_u16_le_(payload + 148 + D));
    this->publish_numeric_(NUMERIC_MU_CLIMB_PERIOD, payload[170 + D]);

    // Combined float runtime hours (hours + minutes/60)
    uint16_t pcb_hrs = read_u16_le_(payload + 140 + D);
    if (pcb_hrs == 0xFFFF) {
      this->publish_numeric_(NUMERIC_MU_PCB_RUNTIME, NAN);
    } else {
      uint8_t pcb_mins = payload[142 + D];
      this->publish_numeric_(NUMERIC_MU_PCB_RUNTIME, static_cast<float>(pcb_hrs) + static_cast<float>(pcb_mins) / 60.0f);
    }

    uint16_t imp_hrs = read_u16_le_(payload + 143 + D);
    uint8_t imp_mins = payload[145 + D];
    this->publish_numeric_(NUMERIC_MU_IMPELLER_RUNTIME, static_cast<float>(imp_hrs) + static_cast<float>(imp_mins) / 60.0f);

    // MU/S/1 has no separate major-version field. Its software-version value
    // is the little-endian two-byte field at data offsets 168-169.
    uint16_t sw_version = read_u16_le_(payload + 168 + D);
    if (sw_version == 0xffff) {
      this->publish_text_(TEXT_MU_SW_VERSION, "Unknown");
    } else {
      char ver_buf[8];
      std::snprintf(ver_buf, sizeof(ver_buf), "%04x", sw_version);
      this->publish_text_(TEXT_MU_SW_VERSION, ver_buf);
    }

    // This field comes from a robot-specific MU protocol profile.
    // mu_led_data_offset_ is post-ACK; add one for the raw response payload.
    // bits 3-7 are intensity in 5% increments, bit 0 is Disco, and bit 1
    // is Constant. No mode bit means Blinking.
    size_t led_raw_offset = static_cast<size_t>(this->mu_led_data_offset_) + 1;
    if (led_raw_offset >= payload_len) {
      ESP_LOGW(TAG, "Configured MU LED data offset %u is outside payload length %u",
               this->mu_led_data_offset_, static_cast<unsigned>(payload_len));
      return;
    }
    uint8_t led_packed = payload[led_raw_offset];
    uint8_t led_intensity = static_cast<uint8_t>(((led_packed >> 3) & 0x1f) * 5);
    bool led_on = led_intensity != 0;
    std::string led_effect = "Blinking";
    if (led_packed & 0x01) {
      led_effect = "Disco";
    } else if (led_packed & 0x02) {
      led_effect = "Constant";
    }

    ESP_LOGD(TAG, "Telemetry LED packed byte data[%u]/raw[%u]=0x%02X enabled=%d intensity=%u mode=%s",
             this->mu_led_data_offset_, static_cast<unsigned>(led_raw_offset), led_packed, led_on,
             led_intensity, led_effect.c_str());
    if (this->led_light_ != nullptr) {
      this->is_telemetry_sync_ = true;
      auto call = this->led_light_->make_call();
      call.set_state(led_on);
      call.set_brightness(static_cast<float>(led_intensity) / 100.0f);
      if (led_on) {
        call.set_effect(led_effect);
      }
      call.perform();
      this->is_telemetry_sync_ = false;
    }
  }
}

void DolphinBle::publish_sm_data_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 16)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;

  if (payload_len >= 151) {
    constexpr size_t D = 1;
    int16_t timezone = static_cast<int16_t>(read_u16_be_(payload + 63 + D));
    this->publish_numeric_(NUMERIC_SM_TIMEZONE, timezone);

    uint8_t qf = payload[65 + D];
    std::string qf_str;
    if (qf & 0x01) qf_str += "WeeklyTimer1D ";
    if (qf & 0x02) qf_str += "WeeklyTimer2D ";
    if (qf & 0x04) qf_str += "WeeklyTimer3D ";
    if (qf & 0x08) qf_str += "DelayTimer ";
    if (qf & 0x10) qf_str += "FilterLED ";
    if (qf & 0x20) qf_str += "FloorOnly ";
    if (qf & 0x40) qf_str += "FastMode ";
    if (qf & 0x80) qf_str += "PickupMode ";
    if (!qf_str.empty())
      qf_str.pop_back();
    this->publish_text_(TEXT_QUICK_FEATURES, qf_str);

    // Parse weekly schedule (offset 72 to 107)
    uint8_t should_repeat = payload[72 + D];
    this->schedule_repeat_ = (should_repeat == 0);
    if (this->weekly_repeat_switch_ != nullptr) {
      this->weekly_repeat_switch_->publish_state(this->schedule_repeat_);
    }

    for (int block = 0; block < 7; block++) {
      size_t block_offset = 73 + D + block * 5;
      if (block_offset + 5 <= payload_len) {
        uint8_t day_id = payload[block_offset];
        uint8_t enabled = payload[block_offset + 1];
        uint8_t hour = payload[block_offset + 2];
        uint8_t minute = payload[block_offset + 3];
        uint8_t mode = payload[block_offset + 4];

        if (day_id < 1 || day_id > 7) {
          ESP_LOGW(TAG, "Ignoring invalid schedule day ID %u", day_id);
          continue;
        }
        size_t day = day_id - 1;  // Home Assistant order: Sunday=0 ... Saturday=6.
        this->day_id_val_[day] = day_id;
        this->day_enabled_[day] = (enabled == 0x01);
        this->day_hour_[day] = hour;
        this->day_minute_[day] = minute;
        this->day_mode_[day] = mode;

        // Publish time string to text entity
        if (this->day_time_texts_[day] != nullptr) {
          char time_buf[16];
          std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d", hour, minute);
          this->day_time_texts_[day]->publish_state(time_buf);
        }

        // Publish mode to select entity
        if (this->day_mode_selects_[day] != nullptr) {
          std::string mode_str = (enabled == 0x01) ? mode_to_string_(mode) : "Disabled";
          this->day_mode_selects_[day]->publish_state(mode_str);
        }
      }
    }

    // Parse start delay timer (offset 108 to 113)
    uint8_t delay_enabled = payload[110 + D];
    if (delay_enabled == 0x01) {
      uint8_t trigger = payload[108 + D];
      uint8_t hour = payload[111 + D];
      uint8_t minute = payload[112 + D];
      uint8_t mode = payload[113 + D];

      char delay_buf[64];
      std::snprintf(delay_buf, sizeof(delay_buf), "Enabled (Type %u): Delay %02d:%02d (%s)",
                    trigger, hour, minute, mode_to_string_(mode).c_str());
      this->publish_text_(TEXT_DELAY_TIMER, delay_buf);
    } else {
      this->publish_text_(TEXT_DELAY_TIMER, "Disabled");
    }

    if (payload_len >= 215) {
      this->schedule_trigger_by_ = payload[213 + D];
    }

    if (this->protocol_debug_logging_ && payload_len >= 237) {
      ESP_LOGI(TAG, "SM payload bytes 210-240: %s", this->format_hex_(payload + 210, 30).c_str());
    }

    std::string ssid;
    for (size_t i = 118; i <= 150 && i + D < payload_len; i++) {
      if (payload[i + D] == '\0')
        break;
      ssid.push_back(static_cast<char>(payload[i + D]));
    }
    this->publish_text_(TEXT_WIFI_SSID, ssid);
  }
}

void DolphinBleButton::press_action() {
  if (this->parent_ == nullptr)
    return;
  switch (this->kind_) {
    case 0:
      this->parent_->press_start_cleaning();
      break;
    case 1:
      this->parent_->press_stop_cleaning();
      break;
    case 2:
      this->parent_->press_pickup_mode();
      break;
    case 3:
      this->parent_->press_refresh_status();
      break;
    case 6:
      this->parent_->press_reset_filter();
      break;
    default:
      break;
  }
}

void DolphinBle::press_start_cleaning() {
  this->send_command_frame_(0x06, 0xFFF8, {}, "start_up_dolphin", false);
  this->request_status_refresh_();
}

void DolphinBle::press_stop_cleaning() {
  this->send_command_frame_(0x05, 0xFFF8, {}, "shutdown_dolphin", false);
  this->request_status_refresh_();
}

void DolphinBle::press_reset_filter() {
  this->send_command_frame_(0x0a, 0xFFF7, {}, "reset_filter_indicator", false);
  this->request_status_refresh_();
}

void DolphinBle::press_pickup_mode() {
  this->publish_current_cleaning_mode_(0x0b);
  this->send_command_frame_(0x03, 0xFFE9, {0x0b}, "start_pickup_mode");
  this->request_status_refresh_();
}

void DolphinBle::press_quit_manual_drive() {
  this->send_command_frame_(0x04, 0xFFF7, {}, "quit_manual_drive", false);
}

void DolphinBle::press_refresh_status() {
  const uint8_t sm_payload[] = {0x02, 0x00, 0xff};
  this->queue_command_frame_(0x02, 0xFFFD, sm_payload, sizeof(sm_payload), "get_sm_data", true);
  const uint8_t mu_payload[] = {0x01, 0x00, 0xff};
  this->queue_command_frame_(0x01, 0xFFFD, mu_payload, sizeof(mu_payload), "get_mu_data", true);
  this->queue_command_frame_(0x07, 0xFFF8, nullptr, 0, "system_status", true);
  this->last_status_poll_ = millis();
}

void DolphinBle::set_cleaning_mode_option(const std::string &option) {
  uint8_t mode = mode_from_string_(option);
  this->selected_cleaning_mode_ = mode;
  if (this->cleaning_mode_select_ != nullptr)
    this->cleaning_mode_select_->publish_state(mode_to_string_(mode));
  this->publish_configured_cycle_duration_();
  this->publish_cycle_time_remaining_();
  this->send_command_frame_(0x03, 0xFFE9, {mode}, "set_cleaning_mode");
  this->request_status_refresh_();
}

void DolphinBle::set_manual_drive_direction_option(const std::string &option) {
  uint8_t direction = direction_from_string_(option);
  this->selected_manual_drive_direction_ = direction;
  if (this->manual_drive_direction_select_ != nullptr)
    this->manual_drive_direction_select_->publish_state(direction_to_string_(direction));

  if (direction == 0x01) { // stop
    ESP_LOGI(TAG, "Exiting manual control steering mode");
    this->press_quit_manual_drive();
  } else {
    uint8_t speed = static_cast<uint8_t>(std::clamp(this->selected_manual_drive_speed_, 0.0f, 100.0f));
    ESP_LOGI(TAG, "Steering robot direction=%s speed=%d", option.c_str(), speed);
    this->send_command_frame_(0x03, 0xFFF7, {direction, speed}, "manual_drive", false);
    this->request_status_refresh_();
  }
}

void DolphinBle::set_manual_drive_speed(float speed) {
  this->selected_manual_drive_speed_ = speed;
  uint8_t direction = this->selected_manual_drive_direction_;
  if (direction != 0x01) { // not stop
    uint8_t speed_byte = static_cast<uint8_t>(std::clamp(speed, 0.0f, 100.0f));
    ESP_LOGI(TAG, "Updating steering speed direction=%s speed=%d",
             direction_to_string_(direction).c_str(), speed_byte);
    this->send_command_frame_(0x03, 0xFFF7, {direction, speed_byte}, "manual_drive", false);
    this->request_status_refresh_();
  }
}

void DolphinBleSelect::control(const std::string &value) {
  if (this->parent_ == nullptr)
    return;
  if (this->kind_ == 0) {
    this->parent_->set_cleaning_mode_option(value);
  } else if (this->kind_ == 1) {
    this->parent_->set_manual_drive_direction_option(value);
  } else {
    this->parent_->set_day_mode_option(this->kind_ - 2, value);
  }
}

void DolphinBleNumber::control(float value) {
  if (this->parent_ == nullptr)
    return;
  this->parent_->set_manual_drive_speed(value);
  this->publish_state(value);
}

void DolphinBle::write_led_state(light::LightState *state) {
  if (this->is_telemetry_sync_) {
    return;
  }

  light::LightColorValues values = state->current_values;
  bool enabled = values.is_on();
  float brightness = values.get_brightness(); // 0.0 to 1.0
  uint8_t intensity = static_cast<uint8_t>(std::round(brightness * 100.0f));

  // Determine pattern mode from effect
  std::string effect = state->get_effect_name();
  uint8_t mode = 0x02; // Default: Constant
  if (effect == "Blinking") {
    mode = 0x01;
  } else if (effect == "Disco") {
    mode = 0x03;
  }

  if (this->last_led_initialized_ &&
      this->last_led_enabled_ == enabled &&
      this->last_led_intensity_ == intensity &&
      this->last_led_mode_ == mode) {
    return;
  }

  this->last_led_enabled_ = enabled;
  this->last_led_intensity_ = intensity;
  this->last_led_mode_ = mode;
  this->last_led_initialized_ = true;

  ESP_LOGI(TAG, "Setting LED: enabled=%d, intensity=%d, mode=%d (effect=%s)",
           enabled, intensity, mode, effect.c_str());

  // Send the command
  uint8_t enabled_byte = enabled ? 0x01 : 0x00;
  this->send_command_frame_(0x10, 0xFFF7, {enabled_byte, intensity, mode}, "led_control", false);
  this->request_status_refresh_();
}

void DolphinBle::set_weekly_repeat_state(bool state) {
  if (this->schedule_repeat_ == state)
    return;
  this->schedule_repeat_ = state;
  this->send_weekly_schedule_();
}

void DolphinBle::set_protocol_debug_logging_state(bool state) {
  this->protocol_debug_logging_ = state;
  if (this->protocol_debug_switch_ != nullptr)
    this->protocol_debug_switch_->publish_state(state);
  ESP_LOGI(TAG, "Verbose protocol logging %s", state ? "enabled" : "disabled");
}

void DolphinBle::set_day_time_option(uint8_t day, const std::string &value) {
  if (day >= 7)
    return;
  int hour = 9;
  int minute = 0;
  if (std::sscanf(value.c_str(), "%d:%d", &hour, &minute) == 2) {
    hour = std::clamp(hour, 0, 23);
    minute = std::clamp(minute, 0, 59);
    if (this->day_hour_[day] == hour && this->day_minute_[day] == minute)
      return;
    this->day_hour_[day] = hour;
    this->day_minute_[day] = minute;
    this->send_weekly_schedule_();
  } else {
    ESP_LOGE(TAG, "Invalid time format: %s. Use HH:MM", value.c_str());
  }
}

void DolphinBle::set_day_mode_option(uint8_t day, const std::string &value) {
  if (day >= 7)
    return;
  bool enabled = (value != "Disabled");
  uint8_t mode = 1;
  if (enabled) {
    mode = mode_from_string_(value);
  }

  if (this->day_enabled_[day] == enabled && this->day_mode_[day] == mode)
    return;

  this->day_enabled_[day] = enabled;
  this->day_mode_[day] = mode;
  this->send_weekly_schedule_();
}

void DolphinBle::send_weekly_schedule_() {
  std::vector<uint8_t> payload;
  payload.reserve(37);

  // Byte 0: Repeat (00 = repeat, 01 = once)
  payload.push_back(this->schedule_repeat_ ? 0x00 : 0x01);

  // Byte 1: Trigger source
  payload.push_back(this->schedule_trigger_by_);

  // Protocol wire order is Monday through Saturday, then Sunday: IDs 2..7,1.
  static constexpr uint8_t WIRE_DAY_IDS[] = {2, 3, 4, 5, 6, 7, 1};
  for (uint8_t day_id : WIRE_DAY_IDS) {
    size_t day = day_id - 1;  // Entity storage order is Sunday=0 ... Saturday=6.
    payload.push_back(day_id);
    payload.push_back(this->day_enabled_[day] ? 0x01 : 0x00);
    payload.push_back(this->day_hour_[day]);
    payload.push_back(this->day_minute_[day]);
    payload.push_back(this->day_mode_[day]);
  }

  ESP_LOGI(TAG, "Sending weekly schedule: Repeat=%d, Trigger=0x%02X",
           this->schedule_repeat_, this->schedule_trigger_by_);
  this->send_command_frame_(0x45, 0xFFF9, payload.data(), payload.size(), "set_weekly_timer");
}

void DolphinBle::parse_weekly_timer_response_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 9)
    return;
  size_t payload_len = frame.size() - 9;
  const uint8_t *payload = &frame[7];
  constexpr size_t D = 1; // ACK byte at index 0

  if (payload_len >= 38) {
    uint8_t should_repeat = payload[0 + D];
    this->schedule_repeat_ = (should_repeat == 0);
    if (this->weekly_repeat_switch_ != nullptr) {
      this->weekly_repeat_switch_->publish_state(this->schedule_repeat_);
    }
    this->schedule_trigger_by_ = payload[1 + D];

    for (int block = 0; block < 7; block++) {
      size_t block_offset = 2 + D + block * 5;
      if (block_offset + 5 <= payload_len) {
        uint8_t day_id = payload[block_offset];
        uint8_t enabled = payload[block_offset + 1];
        uint8_t hour = payload[block_offset + 2];
        uint8_t minute = payload[block_offset + 3];
        uint8_t mode = payload[block_offset + 4];

        if (day_id < 1 || day_id > 7) {
          ESP_LOGW(TAG, "Ignoring invalid weekly-timer day ID %u", day_id);
          continue;
        }
        size_t day = day_id - 1;
        this->day_id_val_[day] = day_id;
        this->day_enabled_[day] = (enabled == 0x01);
        this->day_hour_[day] = hour;
        this->day_minute_[day] = minute;
        this->day_mode_[day] = mode;

        // Publish time string to text entity
        if (this->day_time_texts_[day] != nullptr) {
          char time_buf[16];
          std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d", hour, minute);
          this->day_time_texts_[day]->publish_state(time_buf);
        }

        // Publish mode to select entity
        if (this->day_mode_selects_[day] != nullptr) {
          std::string mode_str = (enabled == 0x01) ? mode_to_string_(mode) : "Disabled";
          this->day_mode_selects_[day]->publish_state(mode_str);
        }
      }
    }
    ESP_LOGI(TAG, "Weekly schedule updated and confirmed by PWS.");
  }
}

void DolphinBleSwitch::write_state(bool state) {
  if (this->parent_ != nullptr) {
    if (this->kind_ == 0) {
      this->parent_->set_weekly_repeat_state(state);
    } else if (this->kind_ == 1) {
      this->parent_->set_protocol_debug_logging_state(state);
    }
  }
  this->publish_state(state);
}

void DolphinBleText::control(const std::string &value) {
  if (this->parent_ != nullptr) {
    this->parent_->set_day_time_option(this->kind_, value);
  }
  this->publish_state(value);
}
}  // namespace dolphin_ble
}  // namespace esphome
