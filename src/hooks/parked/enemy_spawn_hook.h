#pragma once

#include <cstddef>

// PARKED: experimental enemy spawn console support. Not part of the active
// build unless it is intentionally moved back out of src/hooks/parked.

namespace sh4xe::hooks::enemy_spawn
{

struct EnemyChoice
{
    const char* name;
    int type;
    int variant;
};

const char* EnemyListSummary();
const EnemyChoice* FindEnemyChoice(const char* name);

bool SpawnEnemy(int type, int variant, int count, float distance, char* message, size_t messageSize);
bool SpawnSurprise(char* message, size_t messageSize);

} // namespace sh4xe::hooks::enemy_spawn
