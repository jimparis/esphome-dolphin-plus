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

void DolphinBle::loop() {
  this->maybe_start_gatt_();
  this->maybe_connect_();
  this->maybe_send_probe_();
}

float DolphinBle::get_setup_priority() const { return setup_priority::DATA; }

void DolphinBle::maybe_start_gatt_() {
  if (this->gatt_setup_started_)
    return;
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
    return;

  ESP_LOGI(TAG, "Bluedroid is enabled; registering Dolphin GATT client/server");
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
      this->probes_started_ = false;
      this->next_probe_index_ = 0;
      this->last_probe_ms_ = 0;
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

void DolphinBle::add_probe(const std::string &name, const std::string &packet_hex, uint32_t delay_ms) {
  Probe probe;
  probe.name = name;
  probe.delay_ms = delay_ms;
  if (!parse_hex_(packet_hex, &probe.packet)) {
    ESP_LOGE(TAG, "Invalid probe packet hex for %s: %s", name.c_str(), packet_hex.c_str());
    return;
  }
  this->probes_.push_back(probe);
}

void DolphinBle::add_text_probe(const std::string &name, const std::string &packet_text, uint32_t delay_ms) {
  Probe probe;
  probe.name = name;
  probe.text = true;
  probe.delay_ms = delay_ms;
  probe.packet.assign(packet_text.begin(), packet_text.end());
  this->probes_.push_back(probe);
}

void DolphinBle::set_numeric_sensor(uint8_t kind, sensor::Sensor *sensor) {
  if (kind < this->numeric_sensors_.size())
    this->numeric_sensors_[kind] = sensor;
}

void DolphinBle::set_text_sensor(uint8_t kind, text_sensor::TextSensor *sensor) {
  if (kind < this->text_sensors_.size())
    this->text_sensors_[kind] = sensor;
}

void DolphinBle::set_cleaning_mode_select(select::Select *select) { this->cleaning_mode_select_ = select; }

void DolphinBle::set_manual_drive_direction_select(select::Select *select) {
  this->manual_drive_direction_select_ = select;
}

void DolphinBle::set_manual_drive_speed(float speed) { this->selected_manual_drive_speed_ = speed; }

void DolphinBle::press_start_cleaning() { this->send_command_frame_(0x06, 0xFFF8, {}, "start_up_dolphin"); }

void DolphinBle::press_stop_cleaning() { this->send_command_frame_(0x05, 0xFFF8, {}, "shutdown_dolphin"); }

void DolphinBle::press_pickup_mode() {
  this->publish_current_cleaning_mode_(0x0b);
  this->send_command_frame_(0x03, 0xFFE9, {0x0b}, "start_pickup_mode");
}

void DolphinBle::press_manual_drive() {
  uint8_t direction = this->selected_manual_drive_direction_;
  uint8_t speed = static_cast<uint8_t>(std::clamp(this->selected_manual_drive_speed_, 0.0f, 100.0f));
  this->send_command_frame_(0x03, 0xFFF7, {direction, speed}, "manual_drive");
}

void DolphinBle::press_quit_manual_drive() {
  this->send_command_frame_(0x04, 0xFFF7, {}, "quit_manual_drive");
}

void DolphinBle::set_cleaning_mode_option(const std::string &option) {
  uint8_t mode = mode_from_string_(option);
  this->selected_cleaning_mode_ = mode;
  if (this->cleaning_mode_select_ != nullptr)
    this->cleaning_mode_select_->publish_state(mode_to_string_(mode));
  this->send_command_frame_(0x03, 0xFFE9, {mode}, "set_cleaning_mode");
}

void DolphinBle::set_manual_drive_direction_option(const std::string &option) {
  uint8_t direction = direction_from_string_(option);
  this->selected_manual_drive_direction_ = direction;
  if (this->manual_drive_direction_select_ != nullptr)
    this->manual_drive_direction_select_->publish_state(direction_to_string_(direction));
}

void DolphinBle::restart_probes() {
  this->next_probe_index_ = 0;
  this->last_probe_ms_ = 0;
  this->probes_started_ = false;
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

void DolphinBle::maybe_send_probe_() {
  if (!this->auto_probe_ || this->probes_.empty() || this->next_probe_index_ >= this->probes_.size())
    return;
  if (!this->gatts_peer_connected_ || !this->local_notify_enabled_ || !this->mtu_done_)
    return;

  uint32_t now = millis();
  if (!this->probes_started_) {
    this->probes_started_ = true;
    this->last_probe_ms_ = now;
    return;
  }
  if (now - this->last_probe_ms_ < this->probes_[this->next_probe_index_].delay_ms)
    return;

  const Probe &probe = this->probes_[this->next_probe_index_];
  if (probe.name == "temperature" && this->in_water_capability_known_ && !this->in_water_capable_) {
    ESP_LOGW(TAG, "Skipping temperature probe: power supply does not advertise in-water support");
    this->last_probe_ms_ = now;
    this->next_probe_index_++;
    if (this->next_probe_index_ >= this->probes_.size() && this->repeat_probes_)
      this->next_probe_index_ = 0;
    return;
  }

  this->send_local_notification_(probe);
  this->last_probe_ms_ = now;
  this->next_probe_index_++;
  if (this->next_probe_index_ >= this->probes_.size() && this->repeat_probes_) {
    this->next_probe_index_ = 0;
    ESP_LOGI(TAG, "Probe cycle complete; scheduling another status poll");
  }
}

void DolphinBle::send_local_notification_(const Probe &probe) {
  if (probe.packet.empty()) {
    ESP_LOGW(TAG, "Skipping empty probe %s", probe.name.c_str());
    return;
  }
  ESP_LOGI(TAG, "Sending probe %s len=%u data=%s text=\"%s\"", probe.name.c_str(),
           static_cast<unsigned>(probe.packet.size()),
           format_hex_(probe.packet.data(), probe.packet.size()).c_str(),
           probe.text ? format_ascii_(probe.packet.data(), probe.packet.size()).c_str() : "");
  esp_err_t err = esp_ble_gatts_send_indicate(this->gatts_if_, this->gatts_conn_id_,
                                              this->gatts_char_handle_, probe.packet.size(),
                                              const_cast<uint8_t *>(probe.packet.data()), false);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Probe %s notification failed: %s", probe.name.c_str(), esp_err_to_name(err));
}

void DolphinBle::send_command_frame_(uint8_t opcode, uint16_t destination, const uint8_t *payload,
                                     size_t payload_len, const char *name) {
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
  this->tx_text_buffer_ = text;
  ESP_LOGI(TAG, "Sending command %s opcode=0x%02x dest=0x%04x payload=%s text=\"%s\"", name, opcode,
           destination, payload_len ? format_hex_(payload, payload_len).c_str() : "",
           text.c_str());
  esp_err_t err = esp_ble_gatts_send_indicate(this->gatts_if_, this->gatts_conn_id_,
                                              this->gatts_char_handle_, text.size(),
                                              reinterpret_cast<uint8_t *>(this->tx_text_buffer_.data()),
                                              false);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Command %s notification failed: %s", name, esp_err_to_name(err));
}

void DolphinBle::send_command_frame_(uint8_t opcode, uint16_t destination,
                                     std::initializer_list<uint8_t> payload, const char *name) {
  std::vector<uint8_t> data(payload.begin(), payload.end());
  this->send_command_frame_(opcode, destination, data.data(), data.size(), name);
}

void DolphinBle::handle_robot_notification_(const uint8_t *data, size_t len) {
  std::string text = format_ascii_(data, len);
  ESP_LOGI(TAG, "Robot notification len=%u data=%s text=\"%s\"", static_cast<unsigned>(len),
           format_hex_(data, len).c_str(), text.c_str());

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

    ESP_LOGI(TAG,
             "Robot text frame src=0x%02x dest=0x%04x opcode=0x%02x payload_len=%u checksum=%s "
             "frame=%s",
             frame[1], dest, frame[4], payload_len, calculated == received ? "ok" : "bad",
             frame_hex.c_str());

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
    this->publish_mu_data_from_frame_(frame);
    return;
  }
  if (dest == 0xFFFD && opcode == 0x02) {
    this->publish_sm_data_from_frame_(frame);
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

std::string DolphinBle::extract_printable_runs_(const uint8_t *data, size_t len) {
  std::string out;
  std::string current;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (c >= 0x20 && c <= 0x7e) {
      current.push_back(static_cast<char>(c));
      continue;
    }
    if (current.size() >= 4) {
      if (!out.empty())
        out += " | ";
      out += current;
    }
    current.clear();
  }
  if (current.size() >= 4) {
    if (!out.empty())
      out += " | ";
    out += current;
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

uint64_t DolphinBle::read_u64_be_(const uint8_t *data, size_t len) {
  uint64_t out = 0;
  for (size_t i = 0; i < len; i++)
    out = (out << 8) | data[i];
  return out;
}

uint32_t DolphinBle::bytes_to_u32_(const uint8_t *data, size_t len) {
  uint32_t out = 0;
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
    case 0xfe: // -2 unsigned
      return "empty";
    case 0x01:
      return "regular";
    case 0x02:
      return "fast";
    case 0x03:
      return "cove";
    case 0x04:
      return "floor_only";
    case 0x05:
      return "water_line";
    case 0x06:
      return "ultra_clean";
    case 0x07:
      return "spot";
    case 0x08:
      return "wall_only";
    case 0x09:
      return "tic_tac";
    case 0x0a:
      return "custom";
    case 0x0b:
      return "pickup";
    default:
      return "unknown";
  }
}

uint8_t DolphinBle::mode_from_string_(const std::string &mode) {
  if (mode == "regular" || mode == "all_surfaces" || mode == "all")
    return 0x01;
  if (mode == "fast" || mode == "short")
    return 0x02;
  if (mode == "cove")
    return 0x03;
  if (mode == "floor_only" || mode == "floor")
    return 0x04;
  if (mode == "water_line" || mode == "water")
    return 0x05;
  if (mode == "ultra_clean" || mode == "ultra")
    return 0x06;
  if (mode == "spot")
    return 0x07;
  if (mode == "wall_only" || mode == "wall" || mode == "walls")
    return 0x08;
  if (mode == "tic_tac" || mode == "tictac")
    return 0x09;
  if (mode == "custom")
    return 0x0a;
  if (mode == "pick_up" || mode == "pickup")
    return 0x0b;
  if (mode == "empty")
    return 0xfe; // -2
  return 0x01;
}

std::string DolphinBle::direction_to_string_(uint8_t direction) {
  switch (direction) {
    case 0x01:
      return "stop";
    case 0x02:
      return "forward";
    case 0x03:
      return "backward";
    case 0x04:
      return "right";
    case 0x05:
      return "left";
    default:
      return "stop";
  }
}

uint8_t DolphinBle::direction_from_string_(const std::string &direction) {
  if (direction == "forward")
    return 0x02;
  if (direction == "backward")
    return 0x03;
  if (direction == "right")
    return 0x04;
  if (direction == "left")
    return 0x05;
  return 0x01;
}

std::string DolphinBle::robot_state_to_string_(uint8_t state) {
  switch (state) {
    case 0x00:
      return "init";
    case 0x01:
      return "mapping";
    case 0x02:
      return "cleaning";
    case 0x03:
      return "recovery";
    case 0x04:
      return "finished";
    case 0x05:
      return "programing";
    case 0x06:
      return "fault";
    case 0x07:
      return "not_connected";
    default:
      return "unknown";
  }
}

std::string DolphinBle::pws_state_to_string_(uint8_t state) {
  switch (state) {
    case 0x00:
      return "off";
    case 0x01:
      return "on";
    case 0x02:
      return "hold_weekly";
    case 0x03:
      return "hold_delay";
    case 0x04:
      return "programing";
    case 0x05:
      return "on_clean_mode";
    case 0x06:
      return "sleep";
    default:
      return "unknown";
  }
}

std::string DolphinBle::water_status_to_string_(uint8_t state) {
  switch (state) {
    case 0x00:
      return "false";
    case 0x01:
      return "true";
    case 0x02:
      return "unknown";
    case 0x03:
      return "error";
    case 0x04:
      return "no_p_baro";
    case 0x0f:
      return "loading";
    default:
      return "NA";
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

void DolphinBle::publish_text_(uint8_t kind, const std::string &value) {
  if (kind < this->text_sensors_.size() && this->text_sensors_[kind] != nullptr)
    this->text_sensors_[kind]->publish_state(value);
}

void DolphinBle::publish_current_cleaning_mode_(uint8_t mode) {
  this->selected_cleaning_mode_ = mode;
  std::string value = mode_to_string_(mode);
  this->publish_text_(TEXT_CLEANING_MODE, value);
  if (this->cleaning_mode_select_ != nullptr && value != "unknown")
    this->cleaning_mode_select_->publish_state(value);
}

void DolphinBle::publish_pws_features_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 12)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;
  if (payload_len < 3)
    return;

  this->in_water_capability_known_ = true;
  this->in_water_capable_ = (payload[1] & 0x01) != 0;
  std::string summary = "network_sensing=" + std::string((payload[0] & 0x01) ? "true" : "false");
  summary += " in_water=" + std::string(this->in_water_capable_ ? "true" : "false");
  summary += " cellular=" + std::string((payload[2] & 0x01) ? "true" : "false");
  this->publish_text_(TEXT_PWS_FEATURES, summary);
  ESP_LOGI(TAG, "PWS features: %s", summary.c_str());
}

void DolphinBle::publish_status_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 16)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;

  this->publish_text_(TEXT_ROBOT_STATE, robot_state_to_string_(payload[0]));
  this->publish_text_(TEXT_PWS_STATE, pws_state_to_string_(payload[1]));
  this->publish_numeric_(NUMERIC_FILTER_STATE, payload[2]);
  this->publish_text_(TEXT_FILTER_STATUS, filter_status_to_string_(payload[2]));
  this->publish_current_cleaning_mode_(payload[3]);

  std::string status_summary = "robot=" + robot_state_to_string_(payload[0]);
  status_summary += " pws=" + pws_state_to_string_(payload[1]);
  status_summary += " filter=" + filter_status_to_string_(payload[2]);
  status_summary += " mode=" + mode_to_string_(payload[3]);

  if (payload_len >= 14) {
    this->publish_numeric_(NUMERIC_CYCLE_TIME, read_u16_be_(payload + 4));
    this->publish_numeric_(NUMERIC_CYCLE_DURATION, read_u32_be_(payload + 6));
    this->publish_numeric_(NUMERIC_CYCLE_TIME_REMAINING, read_u32_be_(payload + 10));
    std::string cycle_summary = "cycle=0x" + hex_string_(payload + 4, 2);
    cycle_summary += " duration=" + std::to_string(read_u32_be_(payload + 6)) + "s";
    cycle_summary += " remaining=" + std::to_string(read_u32_be_(payload + 10)) + "s";
    this->publish_text_(TEXT_CYCLE_INFO_SUMMARY, cycle_summary);
  }
  if (payload_len >= 15) {
    this->publish_numeric_(NUMERIC_IS_SMART, payload[14] ? 1.0f : 0.0f);
    status_summary += payload[14] ? " smart=true" : " smart=false";
  }
  if (payload_len >= 18) {
    std::string next_cycle = "mode=" + mode_to_string_(payload[15]);
    next_cycle += " duration=" + std::to_string(read_u16_be_(payload + 16)) + "m";
    next_cycle += " raw=" + hex_string_(payload + 15, 3);
    this->publish_text_(TEXT_NEXT_CYCLE_INFO_SUMMARY, next_cycle);
  }
  if (payload_len >= 30)
    this->publish_text_(TEXT_FAULTS_SUMMARY, hex_string_(payload + 18, 12));
  if (payload_len >= 53)
    this->publish_text_(TEXT_CLEANING_MODES_SUMMARY, hex_string_(payload + 30, 23));
  this->publish_text_(TEXT_SYSTEM_STATUS_SUMMARY, status_summary);
}

void DolphinBle::publish_temperature_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 17)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;
  this->publish_text_(TEXT_TEMPERATURE_RAW, hex_string_(payload, payload_len));
  this->publish_text_(TEXT_IN_WATER_STATUS, water_status_to_string_(payload[0]));
  int16_t temperature = static_cast<int16_t>(read_u16_be_(payload + 1));
  this->publish_numeric_(NUMERIC_TEMPERATURE, static_cast<float>(temperature));
  this->publish_numeric_(NUMERIC_READING_DURING_CYCLE, payload[3] ? 1.0f : 0.0f);
  this->publish_numeric_(NUMERIC_MEASURING, payload[4] ? 1.0f : 0.0f);
  this->publish_numeric_(NUMERIC_TEMPERATURE_TIMESTAMP, static_cast<float>(read_u64_be_(payload + 5, 5)));
}

void DolphinBle::publish_mu_data_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 16)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;
  this->publish_text_(TEXT_MU_DATA_RAW, hex_string_(payload, payload_len));

  if (payload_len >= 172) {
    this->publish_numeric_(NUMERIC_ROBOT_TYPE, read_u16_be_(payload + 132));
    this->publish_numeric_(NUMERIC_MU_FLASH_WRITE_COUNTER, read_u32_be_(payload + 134));
    this->publish_numeric_(NUMERIC_MU_CYCLE_TIME, read_u16_be_(payload + 138));
    this->publish_numeric_(NUMERIC_MU_PCB_HOURS, read_u16_be_(payload + 140));
    this->publish_numeric_(NUMERIC_MU_PCB_MINUTES, payload[142]);
    this->publish_numeric_(NUMERIC_MU_IMPELLER_HOURS, read_u16_be_(payload + 143));
    this->publish_numeric_(NUMERIC_MU_IMPELLER_MINUTES, payload[145]);
    this->publish_numeric_(NUMERIC_TURN_ON_COUNT, read_u16_be_(payload + 146));
    this->publish_numeric_(NUMERIC_MU_NOT_COMPLETED_CYCLES, read_u16_be_(payload + 148));
    this->publish_numeric_(NUMERIC_MU_SW_VERSION_MAJOR, payload[152]);
    this->publish_numeric_(NUMERIC_MU_SW_VERSION_MINOR, read_u16_be_(payload + 153));
    this->publish_numeric_(NUMERIC_MU_CLIMB_PERIOD, payload[170]);
  }
}

void DolphinBle::publish_sm_data_from_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 16)
    return;
  const uint8_t *payload = &frame[7];
  size_t payload_len = frame.size() - 9;
  this->publish_text_(TEXT_SM_DATA_RAW, hex_string_(payload, payload_len));

  std::string printable_runs = summarize_printable_runs_(payload, payload_len, 4, 32);
  this->publish_text_(TEXT_SM_SUMMARY, printable_runs);
  ESP_LOGI(TAG, "SM printable summary: %s", printable_runs.c_str());

  if (payload_len >= 151) {
    int16_t timezone = static_cast<int16_t>(read_u16_be_(payload + 63));
    this->publish_numeric_(NUMERIC_SM_TIMEZONE, timezone);

    uint8_t qf = payload[65];
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

    std::string ssid;
    for (size_t i = 118; i <= 150 && i < payload_len; i++) {
      if (payload[i] == '\0')
        break;
      ssid.push_back(static_cast<char>(payload[i]));
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
      this->parent_->restart_probes();
      break;
    case 4:
      this->parent_->press_manual_drive();
      break;
    case 5:
      this->parent_->press_quit_manual_drive();
      break;
    default:
      break;
  }
}

void DolphinBleSelect::control(const std::string &value) {
  if (this->parent_ == nullptr)
    return;
  if (this->kind_ == 0) {
    this->parent_->set_cleaning_mode_option(value);
  } else if (this->kind_ == 1) {
    this->parent_->set_manual_drive_direction_option(value);
  }
}

void DolphinBleNumber::control(float value) {
  if (this->parent_ == nullptr)
    return;
  this->parent_->set_manual_drive_speed(value);
  this->publish_state(value);
}

}  // namespace dolphin_ble
}  // namespace esphome
