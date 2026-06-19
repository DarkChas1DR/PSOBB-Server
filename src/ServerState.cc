#include "ServerState.hh"

#include <string.h>

#include <filesystem>
#include <memory>
#include <random>
#include <phosg/Filesystem.hh>
#include <phosg/Image.hh>
#include <phosg/Network.hh>
#include <phosg/Platform.hh>

#include "Compression.hh"
#include "GameServer.hh"
#include "IPStackSimulator.hh"
#include "ImageEncoder.hh"
#include "Loggers.hh"
#include "NetworkAddresses.hh"
#include "SendCommands.hh"
#include "Text.hh"
#include "TextIndex.hh"

std::shared_ptr<ServerState> ServerState::create_shared(std::shared_ptr<DataIndex> data, bool is_replay) {
  std::shared_ptr<ServerState> s(new ServerState());
  s->data = data;
  s->io_context = std::make_shared<asio::io_context>(1);
  if (s->data->num_worker_threads > 0) {
    config_log.info_f("Starting thread pool with {} threads", s->data->num_worker_threads);
    s->thread_pool = std::make_shared<asio::thread_pool>(s->data->num_worker_threads);
  } else {
    config_log.warning_f("WorkerThreads is zero or not set; using default thread count");
    s->thread_pool = std::make_shared<asio::thread_pool>();
  }
  s->is_replay = is_replay;
  s->reload_config();
  s->load_accounts();
  s->load_ep3_tournament_state();
  s->create_default_lobbies();
  s->load_teams();

  if (!s->is_replay) {
    s->daily_forecast_timer = std::make_unique<asio::steady_timer>(*s->io_context);
    s->update_daily_forecast(false);
    s->schedule_daily_forecast_timer();
  }

  return s;
}

std::shared_ptr<ServerState> ServerState::clone_shared() {
  std::shared_ptr<ServerState> ret(new ServerState());
  ret->data = this->data;
  ret->io_context = this->io_context;
  ret->thread_pool = this->thread_pool;
  ret->is_replay = this->is_replay;
  ret->reload_config();
  ret->load_accounts();
  ret->load_ep3_tournament_state();
  ret->create_default_lobbies();
  ret->load_teams();
  return ret;
}

void ServerState::reload_config() {
  if (!this->data || !this->data->config_json) {
    return;
  }
  try {
    this->bb_shop_item_limits_enabled = this->data->config_json->get_bool("BBShopItemLimits", false);
  } catch (const std::out_of_range&) {}
  try {
    this->exp_boost_multiplier = this->data->config_json->get_float("EventExpBoostMultiplier", 1.5f);
  } catch (const std::out_of_range&) {}
  try {
    this->dar_boost_multiplier = this->data->config_json->get_float("EventDarBoostMultiplier", 1.25f);
  } catch (const std::out_of_range&) {}
  try {
    this->rdr_boost_multiplier = this->data->config_json->get_float("EventRdrBoostMultiplier", 1.25f);
  } catch (const std::out_of_range&) {}
  try {
    this->rare_monster_boost_multiplier = this->data->config_json->get_float("EventRareMonsterBoostMultiplier", 1.5f);
  } catch (const std::out_of_range&) {}
  try {
    this->event_changer_mode = this->data->config_json->get_string("EventChangerMode", "none");
  } catch (const std::out_of_range&) {}

  try {
    const auto& hh = this->data->config_json->get_dict("HappyHour");
    this->happy_hour_enabled = hh.at("Enabled")->as_bool();
    this->happy_hour_min_multiplier = hh.at("MinDropMultiplier")->as_float();
    this->happy_hour_max_multiplier = hh.at("MaxDropMultiplier")->as_float();
  } catch (const std::out_of_range&) {}

  try {
    const auto& ph = this->data->config_json->get_dict("PartyHour");
    this->party_hour_enabled = ph.at("Enabled")->as_bool();
    this->party_hour_min_multiplier = ph.at("MinEXPMultiplier")->as_float();
    this->party_hour_max_multiplier = ph.at("MaxEXPMultiplier")->as_float();
  } catch (const std::out_of_range&) {}

  try {
    this->hour_event_interval_seconds = this->data->config_json->get_int("HourEventIntervalSeconds", 18000);
  } catch (const std::out_of_range&) {}
  try {
    this->hour_event_duration_seconds = this->data->config_json->get_int("HourEventDurationSeconds", 5400);
  } catch (const std::out_of_range&) {}
}

float ServerState::happy_hour_multiplier() const {
  if (!this->happy_hour_enabled) {
    return 1.0f;
  }
  uint64_t t = time(nullptr);
  uint64_t interval = this->hour_event_interval_seconds;
  uint64_t duration = this->hour_event_duration_seconds;
  if (interval == 0 || duration == 0) {
    return 1.0f;
  }
  uint64_t cycle_start = t - (t % interval);
  uint64_t cycle_end = cycle_start + duration;
  if (t >= cycle_start && t < cycle_end) {
    std::mt19937 prng(cycle_start);
    std::uniform_real_distribution<float> dist(this->happy_hour_min_multiplier, this->happy_hour_max_multiplier);
    return dist(prng);
  }
  return 1.0f;
}

float ServerState::party_hour_multiplier() const {
  if (!this->party_hour_enabled) {
    return 1.0f;
  }
  uint64_t t = time(nullptr);
  uint64_t interval = this->hour_event_interval_seconds;
  uint64_t duration = this->hour_event_duration_seconds;
  if (interval == 0 || duration == 0) {
    return 1.0f;
  }
  uint64_t cycle_start = t - (t % interval);
  uint64_t cycle_end = cycle_start + duration;
  if (t >= cycle_start && t < cycle_end) {
    std::mt19937 prng(cycle_start + 1);
    std::uniform_real_distribution<float> dist(this->party_hour_min_multiplier, this->party_hour_max_multiplier);
    return dist(prng);
  }
  return 1.0f;
}


void ServerState::add_client_to_available_lobby(std::shared_ptr<Client> c, bool allow_games) {
  std::shared_ptr<Lobby> added_to_lobby;

  auto try_join_lobby = [&](uint32_t lobby_id) -> std::shared_ptr<Lobby> {
    auto l = this->find_lobby(lobby_id);
    if (!l) {
      c->log.info_f("Cannot join lobby {:08X}: lobby does not exist", lobby_id);
      return nullptr;
    }
    if (!allow_games && l->is_game()) {
      c->log.info_f("Cannot join lobby {:08X}: lobby is a game", lobby_id);
      return nullptr;
    }
    static const std::string password = "";
    auto join_error = l->join_error_for_client(c, &password);
    if (join_error == Lobby::JoinError::ALLOWED) {
      try {
        l->add_client(c);
        c->log.info_f("Joined lobby {:08X}", lobby_id);
        return l;
      } catch (const std::out_of_range& e) {
        c->log.info_f("Cannot join lobby {:08X}: {}", lobby_id, e.what());
        return nullptr;
      }
    }
    c->log.info_f("Cannot join lobby {:08X}: {}", lobby_id, phosg::name_for_enum(join_error));
    return nullptr;
  };

  if (c->preferred_lobby_id >= 0) {
    added_to_lobby = try_join_lobby(c->preferred_lobby_id);
    c->preferred_lobby_id = -1;
  }

  if (!added_to_lobby) {
    for (const auto& lobby_id : this->data->public_lobby_search_order(c)) {
      added_to_lobby = try_join_lobby(lobby_id);
      if (added_to_lobby) {
        break;
      }
    }
  }

  if (!added_to_lobby) {
    added_to_lobby = this->create_lobby(false);
    added_to_lobby->event = this->data->pre_lobby_event;
    added_to_lobby->set_flag(Lobby::Flag::PUBLIC);
    added_to_lobby->set_flag(Lobby::Flag::PERSISTENT);
    added_to_lobby->allow_version(c->version());
    added_to_lobby->block = 1;
    added_to_lobby->name = "LOBBY1";
    added_to_lobby->max_clients = 12;
    added_to_lobby->add_client(c);
    c->log.info_f("Created and joined lobby {:08X}", added_to_lobby->lobby_id);
  }

  if (added_to_lobby) {
    this->send_lobby_join_notifications(added_to_lobby, c);
  }
}

void ServerState::remove_client_from_lobby(std::shared_ptr<Client> c) {
  auto l = c->lobby.lock();
  if (l) {
    l->remove_client(c);
  }
}

bool ServerState::change_client_lobby(
    std::shared_ptr<Client> c,
    std::shared_ptr<Lobby> new_lobby,
    bool send_join_notification,
    ssize_t required_client_id) {
  auto old_lobby = c->lobby.lock();
  if (old_lobby == new_lobby) {
    return true;
  }

  if (old_lobby) {
    old_lobby->remove_client(c);
  }
  try {
    new_lobby->add_client(c, required_client_id);
    if (send_join_notification) {
      this->send_lobby_join_notifications(new_lobby, c);
    }
    return true;
  } catch (const std::exception& e) {
    c->log.error_f("Failed to add client to new lobby: {}", e.what());
    if (old_lobby) {
      try {
        old_lobby->add_client(c);
      } catch (const std::exception& e) {
        c->log.error_f("Failed to return client to old lobby: {}", e.what());
        c->channel->disconnect();
      }
    } else {
      c->channel->disconnect();
    }
    return false;
  }
}

void ServerState::send_lobby_join_notifications(std::shared_ptr<Lobby> l, std::shared_ptr<Client> joining_client) {
  for (auto& other_client : l->clients) {
    if (!other_client) {
      continue;
    } else if (other_client == joining_client) {
      send_join_lobby(joining_client, l);
    } else {
      send_player_join_notification(other_client, l, joining_client);
    }
  }

  for (auto& watcher_l : l->watcher_lobbies) {
    for (auto& watcher_c : watcher_l->clients) {
      if (!watcher_c) {
        continue;
      }
      send_player_join_notification(watcher_c, watcher_l, joining_client);
    }
  }

  if (l->is_game()) {
    for (auto lc : l->clients) {
      if (!lc || (lc == joining_client)) {
        continue;
      }
      if (lc->lobby_client_id == l->leader_id) {
        l->log.info_f("Expecting {} to send game state to {}", lc->channel->name, joining_client->channel->name);
        lc->expected_game_state_sync_commands.emplace(0x6B00 | (joining_client->lobby_client_id));
        lc->expected_game_state_sync_commands.emplace(0x6C00 | (joining_client->lobby_client_id));
        lc->expected_game_state_sync_commands.emplace(0x6D00 | (joining_client->lobby_client_id));
        lc->expected_game_state_sync_commands.emplace(0x6E00 | (joining_client->lobby_client_id));
        lc->expected_game_state_sync_commands.emplace(0x6F00 | (joining_client->lobby_client_id));
        lc->expected_game_state_sync_commands.emplace(0x7100 | (joining_client->lobby_client_id));
        lc->expected_game_state_sync_commands.emplace(0x7200);
      }
      l->log.info_f("Expecting {} to send 6x70 to {}", lc->channel->name, joining_client->channel->name);
      lc->expected_game_state_sync_commands.emplace(0x7000 | (joining_client->lobby_client_id));
    }
  }
}

std::shared_ptr<Lobby> ServerState::find_lobby(uint32_t lobby_id) {
  try {
    return this->id_to_lobby.at(lobby_id);
  } catch (const std::out_of_range&) {
    return nullptr;
  }
}

std::vector<std::shared_ptr<Lobby>> ServerState::all_lobbies() {
  std::vector<std::shared_ptr<Lobby>> ret;
  for (auto& it : this->id_to_lobby) {
    ret.emplace_back(it.second);
  }
  return ret;
}

std::shared_ptr<Lobby> ServerState::create_lobby(bool is_game) {
  while (this->id_to_lobby.count(this->next_lobby_id)) {
    this->next_lobby_id++;
  }
  auto l = std::make_shared<Lobby>(this->shared_from_this(), this->next_lobby_id++, is_game);
  this->id_to_lobby.emplace(l->lobby_id, l);
  l->idle_timeout_usecs = this->data->persistent_game_idle_timeout_usecs;
  return l;
}

void ServerState::remove_lobby(std::shared_ptr<Lobby> l) {
  auto lobby_it = this->id_to_lobby.find(l->lobby_id);
  if (lobby_it == this->id_to_lobby.end()) {
    throw std::logic_error("lobby not registered");
  }
  if (lobby_it->second != l) {
    throw std::logic_error("incorrect lobby ID in registry");
  }

  if (l->check_flag(Lobby::Flag::IS_SPECTATOR_TEAM)) {
    auto primary_l = l->watched_lobby.lock();
    if (primary_l) {
      primary_l->log.info_f("Unlinking watcher lobby {:X}", l->lobby_id);
      primary_l->watcher_lobbies.erase(l);
    } else {
      l->log.info_f("No watched lobby to unlink");
    }
    l->watched_lobby.reset();
  } else {
    send_ep3_disband_watcher_lobbies(l);
  }

  l->log.info_f("Unlinking lobby from index");
  this->id_to_lobby.erase(lobby_it);
}

void ServerState::on_player_left_lobby(std::shared_ptr<Lobby> l, uint8_t leaving_client_id) {
  if (l->count_clients() > 0) {
    send_player_leave_notification(l, leaving_client_id);
  } else if (!l->check_flag(Lobby::Flag::PERSISTENT)) {
    this->remove_lobby(l);
  }
}

std::shared_ptr<Client> ServerState::find_client(const std::string* identifier, uint64_t account_id, std::shared_ptr<Lobby> l) {
  if ((account_id == 0) && identifier) {
    try {
      account_id = stoull(*identifier, nullptr, 0);
    } catch (const std::exception&) {
    }
  }

  if (l) {
    try {
      return l->find_client(identifier, account_id);
    } catch (const std::out_of_range&) {
    }
  }

  for (auto& other_l : this->all_lobbies()) {
    if (l == other_l) {
      continue; // Don't bother looking again
    }
    try {
      return other_l->find_client(identifier, account_id);
    } catch (const std::out_of_range&) {
    }
  }

  throw std::out_of_range("client not found");
}

void ServerState::load_accounts() {
  config_log.info_f("Indexing accounts");
  this->account_index = std::make_shared<AccountIndex>(this->is_replay);
}

void ServerState::load_teams() {
  config_log.info_f("Indexing teams");
  this->team_index = std::make_shared<TeamIndex>("system/teams", this->data->team_reward_defs_json);
}

void ServerState::load_ep3_tournament_state() {
  config_log.info_f("Loading Episode 3 tournament state");
  const std::string& tournament_state_filename = "system/ep3/tournament-state.json";
  this->ep3_tournament_index = std::make_shared<Episode3::TournamentIndex>(
      this->data->ep3_map_index, this->data->ep3_com_deck_index, tournament_state_filename);
  this->ep3_tournament_index->link_all_clients(this->shared_from_this());
}

void ServerState::create_default_lobbies() {
  if (this->default_lobbies_created) {
    return;
  }
  this->default_lobbies_created = true;

  std::vector<std::shared_ptr<Lobby>> non_v1_only_lobbies;
  std::vector<std::shared_ptr<Lobby>> ep3_only_lobbies;

  for (size_t x = 0; x < 20; x++) {
    auto lobby_name = std::format("LOBBY{}", x + 1);
    bool allow_v1 = (x <= 9);
    bool allow_non_ep3 = (x <= 14);

    std::shared_ptr<Lobby> l = this->create_lobby(false);
    l->event = ((l->lobby_id - 1) < this->data->per_lobby_events.size())
        ? this->data->per_lobby_events[l->lobby_id - 1]
        : this->data->pre_lobby_event;
    l->set_flag(Lobby::Flag::PUBLIC);
    l->set_flag(Lobby::Flag::DEFAULT);
    l->set_flag(Lobby::Flag::PERSISTENT);
    if (allow_non_ep3) {
      if (allow_v1) {
        l->allow_version(Version::DC_NTE);
        l->allow_version(Version::DC_11_2000);
        l->allow_version(Version::DC_V1);
      }
      l->allow_version(Version::DC_V2);
      l->allow_version(Version::PC_NTE);
      l->allow_version(Version::PC_V2);
      l->allow_version(Version::GC_NTE);
      l->allow_version(Version::GC_V3);
      l->allow_version(Version::XB_V3);
      l->allow_version(Version::BB_V4);
    }
    l->allow_version(Version::GC_EP3_NTE);
    l->allow_version(Version::GC_EP3);

    l->block = x + 1;
    l->name = lobby_name;
    l->max_clients = 12;
    if (!allow_non_ep3) {
      l->episode = Episode::EP3;
    }
  }
}

void ServerState::update_default_lobby_events_from_config() {
  this->reload_config();
  for (size_t z = 1; z <= 20; z++) {
    auto l = this->find_lobby(z);
    if (l) {
      l->event = 0;
    }
  }
  for (size_t z = 0; z < this->data->per_lobby_events.size(); z++) {
    const auto& l = this->find_lobby(z + 1);
    if (l && l->check_flag(Lobby::Flag::DEFAULT)) {
      l->event = this->data->per_lobby_events[z];
      send_change_event(l, l->event);
    }
  }
}

void ServerState::reset_between_replays() {
  this->account_index = std::make_shared<AccountIndex>(true);

  this->next_lobby_id = 0;
  std::vector<std::shared_ptr<Lobby>> lobbies_to_delete;
  for (const auto& l : this->all_lobbies()) {
    if (l->is_game()) {
      lobbies_to_delete.emplace_back(l);
    } else {
      this->next_lobby_id = std::max<uint32_t>(this->next_lobby_id, l->lobby_id + 1);
    }
  }
  for (const auto& l : lobbies_to_delete) {
    phosg::log_warning_f("Previous replay left a game open ({:08X}); destroying it", l->lobby_id);
    this->remove_lobby(l);
  }
}

void ServerState::disconnect_all_banned_clients() {
  uint64_t now_usecs = phosg::now();

  if (this->game_server) {
    for (const auto& c : this->game_server->all_clients()) {
      uint32_t addr = 0;
      auto ipss_channel = dynamic_pointer_cast<IPSSChannel>(c->channel);
      if (ipss_channel) {
        auto ipss_c = ipss_channel->ipss_client.lock();
        if (ipss_c) {
          addr = ipss_c->ipv4_addr;
        }
      } else {
        auto socket_channel = dynamic_pointer_cast<SocketChannel>(c->channel);
        if (socket_channel) {
          addr = ipv4_addr_for_asio_addr(socket_channel->local_addr.address());
        }
      }
      if ((c->login && (c->login->account->ban_end_time > now_usecs)) || this->data->banned_ipv4_ranges->check(addr)) {
        c->channel->disconnect();
      }
    }
  }
}

ServerState::RotatingEvent ServerState::current_event() const {
  if (this->event_changer_mode == "week1") {
    return RotatingEvent::EXP_BOOST;
  }
  if (this->event_changer_mode == "week2") {
    return RotatingEvent::DAR_BOOST;
  }
  if (this->event_changer_mode == "week3") {
    return RotatingEvent::RDR_BOOST;
  }
  if (this->event_changer_mode == "week4") {
    return RotatingEvent::RARE_MONSTER_BOOST;
  }
  if (this->event_changer_mode == "rotate") {
    uint64_t week = (time(nullptr) / 604800) % 4;
    return static_cast<RotatingEvent>(week + 1);
  }
  return RotatingEvent::NONE;
}

float ServerState::event_exp_multiplier() const {
  return (this->current_event() == RotatingEvent::EXP_BOOST) ? this->exp_boost_multiplier : 1.0f;
}

void ServerState::update_daily_forecast(bool broadcast_changes) {
  time_t t = time(nullptr);
  struct tm utc_tm;
#ifdef _WIN32
  gmtime_s(&utc_tm, &t);
#else
  gmtime_r(&t, &utc_tm);
#endif
  uint32_t today_date = (utc_tm.tm_year + 1900) * 10000 + (utc_tm.tm_mon + 1) * 100 + utc_tm.tm_mday;

  if (this->current_forecast_date != today_date) {
    this->current_daily_forecast = DailyForecast::calculate(t);
    this->current_forecast_date = today_date;
    phosg::log_info_f("Daily forecast updated: {}", this->current_daily_forecast.to_string());

    if (broadcast_changes) {
      for (const auto& c : this->game_server->all_clients()) {
        if (c->login && is_v4(c->version())) {
          send_text_or_scrolling_message(c, "You have had a change of fortune.", "You have had a change of fortune.");
        }
      }
    }
  }
}

void ServerState::schedule_daily_forecast_timer() {
  if (!this->daily_forecast_timer) {
    return;
  }
  this->daily_forecast_timer->expires_after(std::chrono::seconds(30));
  this->daily_forecast_timer->async_wait([this](std::error_code ec) {
    if (!ec) {
      try {
        this->update_daily_forecast(true);
      } catch (const std::exception& e) {
        phosg::log_error_f("Failed to update daily forecast: {}", e.what());
      }
      this->schedule_daily_forecast_timer();
    }
  });
}
