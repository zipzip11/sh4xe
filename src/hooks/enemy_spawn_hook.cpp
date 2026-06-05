#include "hooks/enemy_spawn_hook.h"

#include "hooks/file_loader_hook.h"
#include "sh4/addresses.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace sh4xe::hooks::enemy_spawn
{
namespace
{

struct Vec3
{
    float x;
    float y;
    float z;
};

struct EnemyExtra
{
    float roomFloat;
    int roomValueA;
    int variant;
    int roomValueB;
};

using FinalizeResourceLoadFn = int(__cdecl*)(int);
using EnemyResourceIdFn = int(__cdecl*)(int, int);
using TaskCallback = void(__cdecl*)(int);
using CreateCharacterTaskFn =
    void*(__cdecl*)(int, int, const Vec3*, float, int, TaskCallback, TaskCallback, TaskCallback, TaskCallback);
using SetupEnemyBaseFn = int(__cdecl*)(void*, int);
using AttachEnemyModelFn = void(__cdecl*)(void*, int, int);
using AllocateEnemyExtraFn = void*(__cdecl*)(void*, int);
using SetEnemyFloatBitsFn = int(__cdecl*)(void*, int);
using SetupCharacterCollisionFn = int(__cdecl*)(void*, int, int, int, int, float);
using SetEnemyIntFn = int(__cdecl*)(void*, int);
using LinkCharacterTaskFn = int(__cdecl*)(void*);

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kDefaultSpawnDistance = 600.0f;
constexpr int kDefaultEnemyScaleBits = 0x43480000;
constexpr float kDefaultCollisionRadius = 100.0f;
constexpr int kDefaultEnemyMode = 15;
constexpr int kInertEnemyMode = 16;

unsigned int g_spawnSerial = 0;

constexpr EnemyChoice kEnemyChoices[] = {
    {"mush", 2, 0},         {"mushroom", 2, 0},   {"buzz", 3, 0},    {"bat", 3, 0},     {"mm", 4, 0},
    {"wall", 5, 0},         {"kabe", 5, 0},       {"wheel", 6, 0},   {"jinmen", 7, 0},  {"face", 7, 0},
    {"twins", 8, 0},        {"hil", 9, 0},        {"hyena", 10, 0},  {"dog", 10, 0},    {"multi", 11, 0},
    {"mlt", 11, 0},         {"mln", 11, 1},       {"mlu", 11, 2},    {"mlb", 11, 3},    {"cyo", 12, 0},
    {"baby", 12, 1},        {"bab", 12, 1},       {"oji", 13, 0},    {"fla", 13, 1},    {"fat", 14, 0},
    {"scratch", 15, 0},     {"scr", 15, 0},       {"killer", 16, 0}, {"walter", 16, 0}, {"killerlast", 16, 99},
    {"lastkiller", 16, 99}, {"detective", 17, 0}, {"det", 17, 0},
};

constexpr EnemyChoice kSurpriseChoices[] = {
    {"buzz", 3, 0},
    {"mush", 2, 0},
    {"twins", 8, 0},
    {"hyena", 10, 0},
    {"wheel", 6, 0},
    {"baby", 12, 1},
    {"fat", 14, 0},
    {"killer", 16, 0},
};

template<typename Fn> Fn GameFn(uintptr_t address)
{
    return reinterpret_cast<Fn>(address);
}

void SetMessage(char* message, size_t messageSize, const char* fmt, ...)
{
    if (!message || messageSize == 0)
        return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(message, messageSize, fmt, args);
    va_end(args);
    message[messageSize - 1] = '\0';
}

bool EqualsIgnoreCase(const char* lhs, const char* rhs)
{
    if (!lhs || !rhs)
        return false;

    while (*lhs && *rhs)
    {
        char a = *lhs++;
        char b = *rhs++;
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b + ('a' - 'A'));
        if (a != b)
            return false;
    }

    return *lhs == '\0' && *rhs == '\0';
}

const char* EnemyName(int type, int variant)
{
    for (const EnemyChoice& choice : kEnemyChoices)
    {
        if (choice.type == type && choice.variant == variant)
            return choice.name;
    }

    return "enemy";
}

bool IsReadableMemory(const void* ptr, size_t size)
{
    if (!ptr || size == 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;

    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
    return begin >= regionBegin && begin < regionEnd && size <= regionEnd - begin;
}

template<typename T> bool ReadGameValue(uintptr_t address, T& value)
{
    const void* ptr = reinterpret_cast<const void*>(address);
    if (!IsReadableMemory(ptr, sizeof(T)))
        return false;

    value = *reinterpret_cast<const T*>(ptr);
    return true;
}

bool TryGetAnchorPosition(Vec3& position)
{
    uintptr_t task = 0;
    if (!ReadGameValue(sh4::addr::kCharacterTaskListHead, task) || task < 0x10000)
        return false;

    int16_t characterType = 0;
    if (!ReadGameValue(task + 0x414, characterType))
        return false;

    uintptr_t model = 0;
    if (characterType > 0 && characterType < 19)
    {
        uintptr_t state = 0;
        if (!ReadGameValue(task + 0x408, state) || state < 0x10000 || !ReadGameValue(state + 0x30, model))
            return false;
    }
    else if (!ReadGameValue(task + 0x40C, model))
    {
        return false;
    }

    if (model < 0x10000)
        return false;

    const auto* gamePosition = reinterpret_cast<const float*>(model + 0x10);
    if (!IsReadableMemory(gamePosition, sizeof(Vec3)))
        return false;

    position = {gamePosition[0], gamePosition[1], gamePosition[2]};
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

unsigned int NextSeed()
{
    ++g_spawnSerial;
    return static_cast<unsigned int>(GetTickCount()) ^ (g_spawnSerial * 0x9E3779B9u);
}

bool PreloadEnemyResources(int type, int variant, char* message, size_t messageSize)
{
    const auto modelFileId = GameFn<EnemyResourceIdFn>(sh4::addr::kEnemyModelFileIdForType);
    const auto textureFileId = GameFn<EnemyResourceIdFn>(sh4::addr::kEnemyTextureFileIdForType);
    const auto extraFileId = GameFn<EnemyResourceIdFn>(sh4::addr::kEnemyExtraFileIdForType);
    const auto finalize = GameFn<FinalizeResourceLoadFn>(sh4::addr::kFinalizeResourceLoad);

    const int ids[] = {modelFileId(type, variant), textureFileId(type, variant), extraFileId(type, variant)};
    if (ids[0] == 0 || ids[1] == 0)
    {
        SetMessage(message, messageSize, "err spawn: unsupported type=%d variant=%d", type, variant);
        return false;
    }

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
    {
        if (ids[i] == 0)
            continue;

        bool alreadyRequested = false;
        for (size_t j = 0; j < i; ++j)
            alreadyRequested = alreadyRequested || ids[j] == ids[i];
        if (!alreadyRequested)
            file_loader::LoadFileByIdDirect(ids[i]);
    }

    finalize(0);
    return true;
}

bool SpawnOneEnemy(int type, int variant, const Vec3& position, float yaw)
{
    const auto createTask = GameFn<CreateCharacterTaskFn>(sh4::addr::kCreateCharacterTask);
    const auto setupBase = GameFn<SetupEnemyBaseFn>(sh4::addr::kSetupEnemyBase);
    const auto attachModel = GameFn<AttachEnemyModelFn>(sh4::addr::kAttachEnemyModel);
    const auto allocateExtra = GameFn<AllocateEnemyExtraFn>(sh4::addr::kAllocateEnemyExtra);
    const auto setScale = GameFn<SetEnemyFloatBitsFn>(sh4::addr::kSetEnemyScale);
    const auto setupCollision = GameFn<SetupCharacterCollisionFn>(sh4::addr::kSetupCharacterCollision);
    const auto setStateSlot = GameFn<SetEnemyIntFn>(sh4::addr::kSetEnemyStateSlot);
    const auto linkTask = GameFn<LinkCharacterTaskFn>(sh4::addr::kLinkCharacterTask);
    const auto setMode = GameFn<SetEnemyIntFn>(sh4::addr::kSetEnemyMode);
    const auto update = GameFn<TaskCallback>(sh4::addr::kEnemyTaskUpdate);
    const auto step = GameFn<TaskCallback>(sh4::addr::kEnemyTaskStep);

    void* task = createTask(0, 0, &position, yaw, variant << 8, update, nullptr, step, nullptr);
    if (!task)
        return false;

    setupBase(task, type);
    attachModel(task, variant, variant);

    auto* extra = static_cast<EnemyExtra*>(allocateExtra(task, sizeof(EnemyExtra)));
    if (!extra)
    {
        setMode(task, kInertEnemyMode);
        return false;
    }

    extra->roomFloat = 0.0f;
    extra->roomValueA = 0;
    extra->variant = variant;
    extra->roomValueB = 0;

    setScale(task, kDefaultEnemyScaleBits);
    setupCollision(task, 0, 0, 3, 7, kDefaultCollisionRadius);
    setStateSlot(task, 0);
    linkTask(task);
    setMode(task, kDefaultEnemyMode);
    return true;
}

} // namespace

const char* EnemyListSummary()
{
    return "mush buzz mm wall wheel jinmen twins hil hyena multi cyo baby oji fat scratch killer detective";
}

const EnemyChoice* FindEnemyChoice(const char* name)
{
    for (const EnemyChoice& choice : kEnemyChoices)
    {
        if (EqualsIgnoreCase(name, choice.name))
            return &choice;
    }

    return nullptr;
}

bool SpawnEnemy(int type, int variant, int count, float distance, char* message, size_t messageSize)
{
    if (type < 2 || type > 17)
    {
        SetMessage(message, messageSize, "err spawn: type must be 2..17");
        return false;
    }

    if (variant < 0 || variant > 99)
    {
        SetMessage(message, messageSize, "err spawn: variant must be 0..99");
        return false;
    }

    if (count < 1 || count > 8)
    {
        SetMessage(message, messageSize, "err spawn: count must be 1..8");
        return false;
    }

    if (!(distance >= 150.0f && distance <= 2500.0f))
    {
        SetMessage(message, messageSize, "err spawn: distance must be 150..2500");
        return false;
    }

    Vec3 anchor = {};
    if (!TryGetAnchorPosition(anchor))
    {
        SetMessage(message, messageSize, "err spawn: enter a live room first");
        return false;
    }

    if (!PreloadEnemyResources(type, variant, message, messageSize))
        return false;

    const unsigned int seed = NextSeed();
    const float baseAngle = (static_cast<float>(seed % 4096) / 4096.0f) * kTwoPi;
    int spawned = 0;

    for (int i = 0; i < count; ++i)
    {
        const float angle = baseAngle + (kTwoPi * static_cast<float>(i) / static_cast<float>(count));
        Vec3 position = anchor;
        position.x += std::sin(angle) * distance;
        position.z += std::cos(angle) * distance;

        if (SpawnOneEnemy(type, variant, position, angle + kPi))
            ++spawned;
    }

    if (spawned <= 0)
    {
        SetMessage(message, messageSize, "err spawn: task allocation failed");
        return false;
    }

    if (spawned != count)
    {
        SetMessage(message, messageSize, "err spawn: created %d/%d %s", spawned, count, EnemyName(type, variant));
        return false;
    }

    SetMessage(message,
               messageSize,
               "ok spawned %d %s type=%d variant=%d dist=%.0f",
               spawned,
               EnemyName(type, variant),
               type,
               variant,
               static_cast<double>(distance));
    return true;
}

bool SpawnSurprise(char* message, size_t messageSize)
{
    const unsigned int seed = NextSeed();
    const EnemyChoice& choice = kSurpriseChoices[seed % (sizeof(kSurpriseChoices) / sizeof(kSurpriseChoices[0]))];
    int count = 1 + static_cast<int>((seed >> 8) % 3);

    if (choice.type == 6 || choice.type == 8 || choice.type == 14 || choice.type == 16)
        count = (std::min)(count, 2);

    const float distance = kDefaultSpawnDistance + static_cast<float>((seed >> 12) % 220);
    return SpawnEnemy(choice.type, choice.variant, count, distance, message, messageSize);
}

} // namespace sh4xe::hooks::enemy_spawn
