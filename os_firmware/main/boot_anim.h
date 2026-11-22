/**
 * @file boot_anim.h
 * @brief First-boot Flash-to-TF animation migration and TF-card boot animation player
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Migrate staged boot animation from internal Flash ("storage") to TF card
 *        on first boot (and erase Flash copy), then play the boot animation from TF card.
 */
void boot_anim_run(void);

#ifdef __cplusplus
}
#endif
