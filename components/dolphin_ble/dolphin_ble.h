#pragma once

#include <array>
#include <string>
#include <vector>

#include "esphome/core/component.h"

#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"

namespace esphome {
namespace dolphin_ble {

class DolphinBle : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override;

  void set_mac_address(const std::string &mac) { this->mac_address_ = mac; }
  void set_name_filter(const std::string &name) { this->name_filter_ = name; }
  void set_auto_probe(bool auto_probe) { this->auto_probe_ = auto_probe; }
  void add_probe(const std::string &name, const std::string &packet_hex, uint32_t delay_ms);

 protected:
  struct Probe {
    std::string name;
    std::vector<uint8_t> packet;
    uint32_t delay_ms{1500};
  };

  static DolphinBle *instance_;

  static void gatts_event_handler_(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                   esp_ble_gatts_cb_param_t *param);
  static void gattc_event_handler_(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param);

  void on_gatts_event_(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                       esp_ble_gatts_cb_param_t *param);
  void on_gattc_event_(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                       esp_ble_gattc_cb_param_t *param);

  bool parse_mac_();
  void maybe_connect_();
  void maybe_start_gatt_();
  void create_local_server_(esp_gatt_if_t gatts_if);
  void discover_remote_characteristic_();
  void enable_remote_notifications_();
  void maybe_send_probe_();
  void send_local_notification_(const Probe &probe);
  void log_uuid_(const char *prefix, const esp_bt_uuid_t &uuid);

  static bool uuid_from_string_(const char *uuid, esp_bt_uuid_t *out);
  static bool uuid_equal_(const esp_bt_uuid_t &a, const esp_bt_uuid_t &b);
  static bool parse_hex_(const std::string &hex, std::vector<uint8_t> *out);
  static std::string format_hex_(const uint8_t *data, size_t len);

  std::string mac_address_;
  std::string name_filter_;
  esp_bd_addr_t remote_bda_{};

  esp_bt_uuid_t service_uuid_{};
  esp_bt_uuid_t char_uuid_{};
  esp_bt_uuid_t cccd_uuid_{};

  bool parsed_mac_{false};
  bool gatt_setup_started_{false};
  bool server_ready_{false};
  bool connect_started_{false};
  bool client_connected_{false};
  bool notify_registered_{false};
  bool cccd_written_{false};
  bool mtu_done_{false};
  bool auto_probe_{false};

  esp_gatt_if_t gatts_if_{ESP_GATT_IF_NONE};
  esp_gatt_if_t gattc_if_{ESP_GATT_IF_NONE};
  uint16_t gatts_service_handle_{0};
  uint16_t gatts_char_handle_{0};
  uint16_t gatts_cccd_handle_{0};
  uint16_t gatts_conn_id_{0};
  bool gatts_peer_connected_{false};
  bool local_notify_enabled_{false};

  uint16_t gattc_conn_id_{0};
  uint16_t remote_service_start_{0};
  uint16_t remote_service_end_{0};
  uint16_t remote_char_handle_{0};
  uint16_t remote_cccd_handle_{0};

  std::vector<Probe> probes_;
  size_t next_probe_index_{0};
  uint32_t last_probe_ms_{0};
  bool probes_started_{false};
};

}  // namespace dolphin_ble
}  // namespace esphome
