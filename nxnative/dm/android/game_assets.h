#ifndef DM_GAME_ASSETS_H
#define DM_GAME_ASSETS_H
#include <android/asset_manager.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*dm_assets_progress)(uint64_t done, uint64_t total, void *ctx);
// 0: an external-data APK; 1: embedded game data ready; -1: error (do not start the host).
int dm_game_assets_prepare(AAssetManager *manager, const char *root, char *data_root, size_t capacity,
                           dm_assets_progress progress, void *ctx, char *error, size_t error_capacity);
#endif
