#pragma once

#include "../common/Expected.hpp"

struct sqlite3;

namespace localscribe {

inline constexpr int kJournalSchemaVersion = 1;

[[nodiscard]] Expected<void> applyMigrations(sqlite3 *database);

} // namespace localscribe
