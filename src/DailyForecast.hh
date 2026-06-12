#pragma once

#include <stdint.h>
#include <time.h>
#include <random>
#include <string>
#include <vector>
#include <format>

#include "StaticGameData.hh"

struct DailyForecast {
  enum class Profession : uint8_t {
    NONE = 0,
    HUNTER = 1,
    RANGER = 2,
    FORCE = 3,
  };
  enum class Race : uint8_t {
    NONE = 0,
    HUMAN = 1,
    NEWMAN = 2,
    CAST = 3, // Droid
  };
  enum class Gender : uint8_t {
    NONE = 0,
    MALE = 1,
    FEMALE = 2,
  };

  Profession profession = Profession::NONE;
  Race race = Race::NONE;
  Gender gender = Gender::NONE;

  static DailyForecast calculate(time_t t) {
    struct tm utc_tm;
#ifdef _WIN32
    gmtime_s(&utc_tm, &t);
#else
    gmtime_r(&t, &utc_tm);
#endif
    uint32_t seed = (utc_tm.tm_year + 1900) * 10000 + (utc_tm.tm_mon + 1) * 100 + utc_tm.tm_mday;
    std::mt19937 rng(seed);

    DailyForecast df;
    df.profession = static_cast<Profession>(rng() % 4);
    df.race = static_cast<Race>(rng() % 4);
    df.gender = static_cast<Gender>(rng() % 3);
    return df;
  }

  std::string to_string() const {
    std::vector<std::string> parts;
    parts.push_back(this->profession_name());
    parts.push_back(this->race_name());
    parts.push_back(this->gender_name());
    return std::format("{}, {}, {}", parts[0], parts[1], parts[2]);
  }

  std::string profession_name() const {
    switch (this->profession) {
      case Profession::HUNTER: return "Hunter";
      case Profession::RANGER: return "Ranger";
      case Profession::FORCE: return "Force";
      default: return "None";
    }
  }

  std::string race_name() const {
    switch (this->race) {
      case Race::HUMAN: return "Human";
      case Race::NEWMAN: return "Newman";
      case Race::CAST: return "Droid";
      default: return "None";
    }
  }

  std::string gender_name() const {
    switch (this->gender) {
      case Gender::MALE: return "Male";
      case Gender::FEMALE: return "Female";
      default: return "None";
    }
  }

  size_t count_matches(uint8_t char_class) const {
    size_t matches = 0;
    if (this->profession != Profession::NONE) {
      bool prof_match = false;
      if (this->profession == Profession::HUNTER && char_class_is_hunter(char_class)) prof_match = true;
      else if (this->profession == Profession::RANGER && char_class_is_ranger(char_class)) prof_match = true;
      else if (this->profession == Profession::FORCE && char_class_is_force(char_class)) prof_match = true;
      if (prof_match) {
        matches++;
      }
    }
    if (this->race != Race::NONE) {
      bool race_match = false;
      if (this->race == Race::HUMAN && char_class_is_human(char_class)) race_match = true;
      else if (this->race == Race::NEWMAN && char_class_is_newman(char_class)) race_match = true;
      else if (this->race == Race::CAST && char_class_is_android(char_class)) race_match = true;
      if (race_match) {
        matches++;
      }
    }
    if (this->gender != Gender::NONE) {
      bool gender_match = false;
      if (this->gender == Gender::MALE && char_class_is_male(char_class)) gender_match = true;
      else if (this->gender == Gender::FEMALE && !char_class_is_male(char_class)) gender_match = true;
      if (gender_match) {
        matches++;
      }
    }
    return matches;
  }
};
