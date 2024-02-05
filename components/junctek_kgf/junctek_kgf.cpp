#include "junctek_kgf.h"

#include "esphome/core/log.h"
#include "esphome/core/optional.h"


#include <string>
#include <string.h>

#include <setjmp.h>

static jmp_buf parsing_failed;
static const char *const TAG = "JunkTek KG-F";

esphome::optional<int> try_getval(const char*& cursor)
{
  long val;
  const char* pos = cursor;
  char* end = nullptr;
  val = strtoll(pos, &end, 10);
  if (end == pos || end == nullptr)
  {
    return nullopt;
  }
  if (*end != ',' && *end != '.')
  {
    ESP_LOGE("JunkTekKGF", "Error no coma %s", cursor);
    return nullopt;
  }
  cursor = end + 1; // Skip coma
  return val;
}

// Get a value where it's expected to be "<number>[,.], incrementing the cursor past the end"
int getval(const char*& cursor)
{
  auto val = try_getval(cursor);
  if (!val)
  {
    longjmp(parsing_failed, 1);
  }
  return *val;
}
  

JuncTekKGF::JuncTekKGF(unsigned address, bool invert_current)
  : address_(address)
  , invert_current_(invert_current)
{
}

void JuncTekKGF::dump_config()
{
  ESP_LOGCONFIG(TAG, "junctek_kgf:");
  ESP_LOGCONFIG(TAG, "  Address: %d", this->address_);
  ESP_LOGCONFIG(TAG, "  Invert Current: %s", this->invert_current_ ? "True" : "False");
  ESP_LOGCONFIG(TAG, "  Update Settings Interval: %d", this->update_settings_interval_);
  ESP_LOGCONFIG(TAG, "  Update Stats Interval: %d", this->update_stats_interval_);
}

void JuncTekKGF::handle_settings(const char* buffer)
{
  ESP_LOGD("JunkTekKGF", "Settings %s", buffer);
  const char* cursor = buffer;
  const int address = getval(cursor);
  if (address != this->address_)
    return;
  const int checksum = getval(cursor);
  if (! verify_checksum(checksum, cursor))
    return;

  const float overVoltage = getval(cursor) / 100.0;
  const float underVoltage = getval(cursor) / 100.0;
  const float positiveOverCurrent = getval(cursor) / 1000.0;
  const float negativeOverCurrent = getval(cursor) / 100.00;
  const float overPowerProtection = getval(cursor) / 100.00;
  const float overTemperature = getval(cursor) - 100.0;
  const int protectionRecoverySeconds = getval(cursor);
  const int delayTime = getval(cursor);
  const float batteryAmpHourCapacity = getval(cursor) / 10.0;
  const int voltageCalibration = getval(cursor);
  const int currentCalibration = getval(cursor);
  const float temperatureCalibration = getval(cursor) - 100.0;
  const int reserved = getval(cursor);
  const int relayNormallyOpen = getval(cursor);
  const int currentratio = getval(cursor);
  // NOTE these are in the docs, but I don't seem to get them
  //const int voltageCurveScale = getval(cursor);
  //const int currentCurveScale = getval(cursor);

  // Save the capacity for calculating the %
  this->battery_capacity_ = batteryAmpHourCapacity;

  this->last_settings_ = esphome::millis();
}

void JuncTekKGF::handle_status(const char* buffer)
{
  ESP_LOGD("JunkTekKGF", "Status %s", buffer);
  const char* cursor = buffer;
  const int address = getval(cursor);
  if (address != this->address_)
    return;
 
  const int checksum = getval(cursor);
  if (! verify_checksum(checksum, cursor))
    return;

  const float voltage = getval(cursor) / 100.0;
  const float amps = getval(cursor) / 100.0;
  const float ampHourRemaining = getval(cursor) / 1000.0;
  const float ampHourTotalUsed = getval(cursor) / 1000.00;
  const float wattHour = getval(cursor) / 100.0;
  const float runtimeSeconds = getval(cursor);
  const float temperature = getval(cursor) - 100.0;
//todo - my unit always returns 0; from manual: 0 means the function is pending
  const float powerInWatts = getval(cursor) / 100.0;
  const int relayStatus = getval(cursor);
  const int direction = getval(cursor);
  const int batteryLifeMinutes = getval(cursor);
  const float batteryInternalOhms = getval(cursor) / 100.0;
  ESP_LOGV("JunkTekKGF", "Recv %f %f %d %f %f %f", voltage, ampHourRemaining, direction, powerInWatts, amps, temperature);

  if (voltage_sensor_)
    this->voltage_sensor_->publish_state(voltage);

  if (battery_level_sensor_ && this->battery_capacity_)
    this->battery_level_sensor_->publish_state(ampHourRemaining * 100.0 / *this->battery_capacity_);

  if (current_sensor_) {
    float adjustedCurrent = direction == 0 ? amps : -amps;
    if (invert_current_)
      adjustedCurrent *= -1;
    current_sensor_->publish_state(adjustedCurrent);
  }

  if (current_direction_sensor_)
    this->current_direction_sensor_->publish_state(direction == 0);

  if (battery_ohm_sensor_)
    this->battery_ohm_sensor_->publish_state(batteryInternalOhms);

  if (amp_hour_remain_sensor_)
    this->amp_hour_remain_sensor_->publish_state(ampHourRemaining);

  if (relay_status_sensor_)
    this->relay_status_sensor_->publish_state(relayStatus == 0);

  if (temperature_)
    this->temperature_->publish_state(temperature);

  if (power_sensor_) {
    float adjustedCurrent = direction == 0 ? amps : -amps;
    if (invert_current_)
      adjustedCurrent *= -1;
    float watts = voltage * adjustedCurrent;
    this->power_sensor_->publish_state(watts);
  }

  if (battery_charged_energy_sensor_) {
    this->battery_charged_energy_sensor_->publish_state(wattHour / 1000);
  }

  if (battery_discharged_energy_sensor_) {
    float dischargedEnergy = ampHourTotalUsed * (3.2 * 18) / 1000;  // Nominal voltange on cell * count off cell;
    this->battery_discharged_energy_sensor_->publish_state(dischargedEnergy);
  }
  //  if (battery_life_sensor_)
    //    this->battery_life_sensor_->publish_state(batteryLifeMinutes);

  //  if (runtime_sensor_)
  //    this->runtime_sensor_->publish_state(runtimeSeconds);

  this->last_stats_ = esphome::millis();
}

void JuncTekKGF::handle_line()
{
  //A failure in parsing will return back to here with a non-zero value
  if (setjmp(parsing_failed)){
    ESP_LOGE("JunkTekKGF", "parsing_failed");
    return;
  }

  const char* buffer = &this->line_buffer_[0];

  if (buffer[0] != ':' || buffer[1] != 'r') {
    return;
  }

  if (strncmp(&buffer[2], "50=", 3) == 0)
    handle_status(&buffer[5]);
  else if (strncmp(&buffer[2], "51=", 3) == 0)
    handle_settings(&buffer[5]);
  else
    ESP_LOGD("JunkTekKGF", "buffer:", buffer);

  return;
}

bool JuncTekKGF::readline()
{
  while (available()) {
    const char readch = read();
    if (readch > 0) {
      switch (readch) {
        case '\r': // Ignore carriage return
          break;
        case '\n': // Return on line feed
          this->line_pos_ = 0;  // Reset position index ready for next time
          return true;
        default:
          if (this->line_pos_ < MAX_LINE_LEN - 1)
          {
            this->line_buffer_[this->line_pos_++] = readch;
            this->line_buffer_[this->line_pos_] = 0;
          }
      }
    }
  }
  return false;
}

bool JuncTekKGF::verify_checksum(int checksum, const char* buffer)
{
  long total = 0;
  while (auto val = try_getval(buffer))
  {
    total += *val;
  }
  const bool checksum_valid = (total % 255) + 1 == checksum;
  ESP_LOGD("JunkTekKGF", "Recv checksum %d total %ld valid %d", checksum, total, checksum_valid);
  return checksum_valid;
}

void JuncTekKGF::loop()
{
//todo not needed. uncomment if you do not receive any data

  const unsigned long start_time = esphome::millis();

  if (!this->last_settings_ || (*this->last_settings_ + this->update_settings_interval_) < start_time)
  {
    this->last_settings_ = start_time;
    char buffer[20];
    sprintf(buffer, ":R51=%d,2,1,\r\n", this->address_);
    write_str(buffer);
  }

  if (!this->last_stats_ || (*this->last_stats_ + this->update_stats_interval_) < start_time)
  {
    this->last_stats_ = start_time;
    char buffer[20];
    sprintf(buffer, ":R50=%d,2,1,\r\n", this->address_);
    write_str(buffer);
  }

  if (readline())
  {
    handle_line();
  }
}

float JuncTekKGF::get_setup_priority() const
{
  return setup_priority::DATA;
}
