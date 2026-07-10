#pragma once

#include <cstddef>
#include <array>
#include <initializer_list>
#include <string>
#include <vector>
#include <mutex>

#include "esphome/core/component.h"

#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"

namespace esphome {
namespace dolphin_ble {

class DolphinBle : public Component {
 public:
  static constexpr size_t NUM_NUMERIC_SENSORS = 13;
  static constexpr size_t NUM_TEXT_SENSORS = 9;

  void setup() override;
  void loop() override;
  float get_setup_priority() const override;

  void set_mac_address(const std::string &mac) { this->mac_address_ = mac; }
  void set_name_filter(const std::string &name) { this->name_filter_ = name; }

  void set_numeric_sensor(uint8_t kind, sensor::Sensor *sensor);
  void set_text_sensor(uint8_t kind, text_sensor::TextSensor *sensor);
  void set_cleaning_mode_select(select::Select *select);
  void set_manual_drive_direction_select(select::Select *select);
  void set_manual_drive_speed(float speed);
  void press_start_cleaning();
  void press_stop_cleaning();
  void press_pickup_mode();
  void press_quit_manual_drive();
  void set_cleaning_mode_option(const std::string &option);
  void set_manual_drive_direction_option(const std::string &option);

 protected:
  enum NumericSensorKind : uint8_t {
    NUMERIC_FILTER_STATE = 0,
    NUMERIC_IS_SMART = 1,
    NUMERIC_CYCLE_DURATION = 2,
    NUMERIC_CYCLE_START_TIME = 3,
    NUMERIC_TEMPERATURE = 4,
    NUMERIC_ROBOT_TYPE = 5,
    NUMERIC_TURN_ON_COUNT = 6,
    NUMERIC_MU_FLASH_WRITE_COUNTER = 7,
    NUMERIC_MU_PCB_RUNTIME = 8,
    NUMERIC_MU_IMPELLER_RUNTIME = 9,
    NUMERIC_MU_NOT_COMPLETED_CYCLES = 10,
    NUMERIC_MU_CLIMB_PERIOD = 11,
    NUMERIC_SM_TIMEZONE = 12,
  };

  enum TextSensorKind : uint8_t {
    TEXT_ROBOT_STATE = 0,
    TEXT_PWS_STATE = 1,
    TEXT_CLEANING_MODE = 2,
    TEXT_IN_WATER_STATUS = 3,
    TEXT_PWS_FEATURES = 4,
    TEXT_FILTER_STATUS = 5,
    TEXT_WIFI_SSID = 6,
    TEXT_QUICK_FEATURES = 7,
    TEXT_MU_SW_VERSION = 8,
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
  void handle_polling_();
  void send_local_notification_text_(const std::string &text);
  void send_command_frame_(uint8_t opcode, uint16_t destination, const uint8_t *payload,
                           size_t payload_len, const char *name);
  void send_command_frame_(uint8_t opcode, uint16_t destination, std::initializer_list<uint8_t> payload,
                           const char *name);
  void handle_robot_notification_(const uint8_t *data, size_t len);
  void process_robot_notification_(const uint8_t *data, size_t len);
  void maybe_log_complete_text_frame_();
  void parse_robot_text_frame_(const std::vector<uint8_t> &frame);
  void log_uuid_(const char *prefix, const esp_bt_uuid_t &uuid);

  static bool uuid_from_string_(const char *uuid, esp_bt_uuid_t *out);
  static bool uuid_equal_(const esp_bt_uuid_t &a, const esp_bt_uuid_t &b);
  static bool parse_hex_(const std::string &hex, std::vector<uint8_t> *out);
  static std::string format_hex_(const uint8_t *data, size_t len);
  static std::string format_ascii_(const uint8_t *data, size_t len);
  static bool is_text_frame_chunk_(const uint8_t *data, size_t len);
  static uint16_t read_u16_be_(const uint8_t *data);
  static uint32_t read_u32_be_(const uint8_t *data);
  static uint16_t read_u16_le_(const uint8_t *data);
  static uint32_t read_u32_le_(const uint8_t *data);
  static uint64_t read_u64_be_(const uint8_t *data, size_t len);
  static std::string mode_to_string_(uint8_t mode);
  static uint8_t mode_from_string_(const std::string &mode);
  static std::string direction_to_string_(uint8_t direction);
  static uint8_t direction_from_string_(const std::string &direction);
  static std::string robot_state_to_string_(uint8_t state);
  static std::string pws_state_to_string_(uint8_t state);
  static std::string water_status_to_string_(uint8_t state);
  static std::string filter_status_to_string_(uint8_t state);
  static std::string hex_string_(const uint8_t *data, size_t len);
  static std::string summarize_printable_runs_(const uint8_t *data, size_t len, size_t max_runs,
                                               size_t max_run_len);

  void publish_numeric_(uint8_t kind, float value);
  void publish_text_(uint8_t kind, const std::string &value);
  void publish_current_cleaning_mode_(uint8_t mode);
  void publish_status_from_frame_(const std::vector<uint8_t> &frame);
  void publish_temperature_from_frame_(const std::vector<uint8_t> &frame);
  void publish_mu_data_from_frame_(const std::vector<uint8_t> &frame);
  void publish_sm_data_from_frame_(const std::vector<uint8_t> &frame);
  void publish_pws_features_from_frame_(const std::vector<uint8_t> &frame);

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
  bool in_water_capability_known_{false};
  bool in_water_capable_{false};

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

  uint8_t metadata_step_{0};
  uint32_t last_metadata_poll_{0};
  uint32_t last_status_poll_{0};
  uint32_t last_temp_poll_{0};

  std::string rx_text_buffer_;
  std::string tx_text_buffer_;
  std::mutex rx_mutex_;
  std::vector<std::vector<uint8_t>> rx_queue_;

  std::array<sensor::Sensor *, NUM_NUMERIC_SENSORS> numeric_sensors_{};
  std::array<text_sensor::TextSensor *, NUM_TEXT_SENSORS> text_sensors_{};
  select::Select *cleaning_mode_select_{nullptr};
  select::Select *manual_drive_direction_select_{nullptr};
  uint8_t selected_cleaning_mode_{1};
  uint8_t selected_manual_drive_direction_{1};
  float selected_manual_drive_speed_{50.0f};
};

class DolphinBleButton : public button::Button {
 public:
  DolphinBleButton(DolphinBle *parent, uint8_t kind) : parent_(parent), kind_(kind) {}

 protected:
  void press_action() override;

  DolphinBle *parent_;
  uint8_t kind_;
};

class DolphinBleSelect : public select::Select {
 public:
  DolphinBleSelect(DolphinBle *parent, uint8_t kind) : parent_(parent), kind_(kind) {}

 protected:
  void control(const std::string &value) override;

  DolphinBle *parent_;
  uint8_t kind_;
};

class DolphinBleNumber : public number::Number {
 public:
  explicit DolphinBleNumber(DolphinBle *parent) : parent_(parent) {}

 protected:
  void control(float value) override;

  DolphinBle *parent_;
};

}  // namespace dolphin_ble
}  // namespace esphome
