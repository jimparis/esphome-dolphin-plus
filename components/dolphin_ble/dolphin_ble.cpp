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

  this->send_local_notification_(this->probes_[this->next_probe_index_]);
  this->last_probe_ms_ = now;
  this->next_probe_index_++;
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

    this->rx_text_buffer_.erase(0, expected_text_len);
  }
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

}  // namespace dolphin_ble
}  // namespace esphome
